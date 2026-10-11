#include "nri_static_tangent_scratch.h"
#include <algorithm>
#include <limits>

namespace
{
bool CreateBuffer(const NRIResourceServices& services, NRIBufferResource& result, uint64_t bytes, bool writable)
{
    const auto& c = services.context;
    nri::BufferDesc desc = {};
    desc.size = bytes;
    desc.structureStride = NRIStaticTangentScratch::RecordStride;
    desc.usage = writable ? NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE,
        nri::BufferUsageBits::SHADER_RESOURCE_STORAGE) : nri::BufferUsageBits::SHADER_RESOURCE;
    if (c.core->CreateCommittedBuffer(*c.device, nri::MemoryLocation::DEVICE, 0.0f, desc, result.buffer) != nri::Result::SUCCESS)
    {
        services.DestroyBufferResource(result);
        result = {};
        return false;
    }
    result.size = result.usedSize = bytes;
    result.stride = desc.structureStride;
    result.usage = desc.usage;
    nri::MemoryDesc memory = {};
    c.core->GetBufferMemoryDesc(*result.buffer, nri::MemoryLocation::DEVICE, memory);
    result.memorySize = memory.size;
    nri::BufferViewDesc view = {};
    view.buffer = result.buffer;
    view.type = nri::BufferView::STRUCTURED_BUFFER;
    view.size = nri::WHOLE_SIZE;
    view.structureStride = desc.structureStride;
    if (c.core->CreateBufferView(view, result.shaderView) != nri::Result::SUCCESS)
    {
        services.DestroyBufferResource(result);
        result = {};
        return false;
    }
    if (writable)
    {
        view.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
        if (c.core->CreateBufferView(view, result.storageView) != nri::Result::SUCCESS)
        {
            services.DestroyBufferResource(result);
            result = {};
            return false;
        }
    }
    return true;
}
bool Valid(const NRIStaticTangentServices& s)
{
    return s.resources.context.device && s.resources.context.core &&
        s.resources.destroyBufferResource && s.commandComplete && s.commandAbandoned && s.loadProducer;
}
bool Complete(const NRIStaticTangentServices& s, uint64_t fence)
{ return !fence || s.commandComplete(s.user, fence); }
bool Abandoned(const NRIStaticTangentServices& s, uint64_t fence)
{ return fence && s.commandAbandoned(s.user, fence); }
}

bool NRIStaticTangentScratch::EnsureFallback(const NRIStaticTangentServices& s)
{
    if (!Valid(s)) return false;
    const bool result = mFallback.shaderView || CreateBuffer(s.resources, mFallback, RecordStride, false);
    mTelemetry.fallbackAllocatedBytes = mFallback.memorySize;
    return result;
}

bool NRIStaticTangentScratch::EnsurePipeline(const NRIStaticTangentServices& s)
{
    if (mPipeline) return true;
    if (!Valid(s)) return false;
    const auto& c = s.resources.context;
    const auto& limits = c.core->GetDeviceDesc(*c.device).pipelineLayout;
    if (limits.rootDescriptorMaxNum < 3 || limits.rootConstantMaxSize < 16) return false;
    if (!mLayout)
    {
        nri::RootDescriptorDesc roots[3] = {};
        for (uint32_t i = 0; i < 3; ++i)
        {
            roots[i].registerIndex = i; // t0/t1/u2: distinct Vulkan bindings.
            roots[i].descriptorType = i == 2 ? nri::DescriptorType::STORAGE_STRUCTURED_BUFFER : nri::DescriptorType::STRUCTURED_BUFFER;
            roots[i].shaderStages = nri::StageBits::COMPUTE_SHADER;
        }
        nri::RootConstantDesc constants = {};
        constants.size = 16;
        constants.shaderStages = nri::StageBits::COMPUTE_SHADER;
        nri::PipelineLayoutDesc layout = {};
        layout.rootRegisterSpace = 0;
        layout.rootConstants = &constants;
        layout.rootConstantNum = 1;
        layout.rootDescriptors = roots;
        layout.rootDescriptorNum = 3;
        layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
        if (c.core->CreatePipelineLayout(*c.device, layout, mLayout) != nri::Result::SUCCESS) return false;
    }
    std::vector<uint8_t> blob;
    if (!s.loadProducer(s.user, blob)) return false;
    nri::ComputePipelineDesc pipeline = {};
    pipeline.pipelineLayout = mLayout;
    pipeline.shader.stage = nri::StageBits::COMPUTE_SHADER;
    pipeline.shader.bytecode = blob.data();
    pipeline.shader.size = blob.size();
    pipeline.shader.entryPointName = "main";
    return c.core->CreateComputePipeline(*c.device, pipeline, mPipeline) == nri::Result::SUCCESS;
}

NRIStaticTangentScratch::Binding NRIStaticTangentScratch::Publish(
    const NRIStaticTangentServices& s, const NRIStaticTangentFrame& frame,
    const NRIBufferResource& vertices, const NRIBufferResource& primitives,
    uint32_t primitiveCount, bool enabled)
{
    Binding result;
    mTelemetry.reason = NRIStaticTangentReason::InvalidServices;
    if (!EnsureFallback(s)) return result;
    result.output = mFallback.shaderView;
    mTelemetry.reason = NRIStaticTangentReason::InvalidFrame;
    if (frame.queuedSlot >= SlotCount || frame.epoch == UINT64_MAX || !frame.commandFence) return result;
    Slot& slot = mSlots[frame.queuedSlot];
    slot.lease.ReconcileAbandonment(Abandoned(s, slot.lease.commandFence));
    if (!slot.lease.CanBeginEpoch(frame.epoch, frame.priorSlotFrameComplete,
        Complete(s, slot.lease.commandFence), Abandoned(s, slot.lease.commandFence)))
    { mTelemetry.reason = NRIStaticTangentReason::PendingLease; return result; }
    const bool newEpoch = slot.lease.epoch != frame.epoch;
    slot.lease.BeginEpoch(frame.epoch);
    if (newEpoch) slot.epochView = nullptr;
    const bool validSource = vertices.buffer && vertices.shaderView && primitives.buffer && primitives.shaderView &&
        vertices.stride == 32 && primitives.stride == 88 && vertices.usedSize && vertices.usedSize % 32 == 0 &&
        primitives.usedSize % 88 == 0 && vertices.usedSize <= vertices.size && primitives.usedSize <= primitives.size &&
        primitiveCount && primitives.usedSize / 88 >= primitiveCount && vertices.usedSize / 32 <= UINT32_MAX;
    const uint64_t bytes = uint64_t(primitiveCount) * RecordStride;
    NRIStaticTangentReason reason = enabled ? NRIStaticTangentReason::None : NRIStaticTangentReason::Disabled;
    if (enabled && !validSource) reason = NRIStaticTangentReason::InvalidSource;
    if (enabled && validSource && slot.output.size < bytes)
    {
        if (!slot.lease.CanGrow() || bytes > ByteCap ||
            mTelemetry.allocatedBytes + bytes > ByteCap)
            reason = NRIStaticTangentReason::Capacity;
        else
        {
            const auto& c = s.resources.context;
            nri::BufferDesc desc = {};
            desc.size = bytes;
            desc.structureStride = RecordStride;
            desc.usage = NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
            nri::MemoryDesc predicted = {};
            if (c.core->GetDeviceDesc(*c.device).features.getMemoryDesc2)
                c.core->GetBufferMemoryDesc2(*c.device, desc, nri::MemoryLocation::DEVICE, predicted);
            NRIBufferResource replacement;
            // Bound transient old+new memory as well as final retained capacity.
            if (!predicted.size || mTelemetry.allocatedBytes + predicted.size > ByteCap)
                reason = NRIStaticTangentReason::Capacity;
            else if (!CreateBuffer(s.resources, replacement, bytes, true)) reason = NRIStaticTangentReason::Allocation;
            else if (replacement.memorySize != predicted.size || mTelemetry.allocatedBytes + replacement.memorySize > ByteCap)
            {
                s.resources.DestroyBufferResource(replacement);
                reason = NRIStaticTangentReason::Capacity;
            }
            else
            {
                mTelemetry.allocatedBytes -= slot.output.memorySize;
                s.resources.DestroyBufferResource(slot.output);
                slot.output = replacement;
                mTelemetry.allocatedBytes += replacement.memorySize;
                slot.lease.readable = false;
            }
        }
    }
    // Freeze the descriptor on the FIRST publication even if disabled/failed.
    // Later views may not replace fallback with an output or resize that output.
    if (!slot.epochView) slot.epochView = slot.output.shaderView ? slot.output.shaderView : mFallback.shaderView;
    slot.lease.Publish(mNextPublication++);
    if (!mNextPublication) ++mNextPublication;
    result.epoch = frame.epoch;
    result.publication = slot.lease.publication;
    result.slot = frame.queuedSlot;
    result.output = slot.epochView;
    result.vertices = vertices.shaderView;
    result.primitives = primitives.shaderView;
    result.primitiveCount = primitiveCount;
    result.vertexCount = validSource ? uint32_t(vertices.usedSize / 32) : 0;
    result.usable = enabled && validSource && reason == NRIStaticTangentReason::None &&
        result.output == slot.output.shaderView && slot.output.storageView && slot.output.size >= bytes;
    mTelemetry.reason = reason;
    ++mTelemetry.publications;
    return result;
}

bool NRIStaticTangentScratch::Record(const NRIStaticTangentServices& s,
    const NRIStaticTangentFrame& frame, const Binding& binding)
{
    mTelemetry.reason = NRIStaticTangentReason::MissingPublication;
    if (!Valid(s) || !s.resources.context.commandBuffer || frame.queuedSlot >= SlotCount ||
        binding.slot != frame.queuedSlot || !binding.usable || !frame.commandFence) return false;
    Slot& slot = mSlots[frame.queuedSlot];
    slot.lease.ReconcileAbandonment(Abandoned(s, slot.lease.commandFence));
    if (!slot.lease.Matches(frame.epoch, binding.publication)) return false;
    if (binding.output != slot.epochView || binding.output != slot.output.shaderView ||
        !binding.vertices || !binding.primitives || !binding.primitiveCount || !binding.vertexCount ||
        uint64_t(binding.primitiveCount) * RecordStride > ByteCap ||
        slot.output.size / RecordStride < binding.primitiveCount)
    { mTelemetry.reason = NRIStaticTangentReason::BindingMismatch; return false; }
    if (!slot.lease.CanRecord(frame.commandFence, Complete(s, slot.lease.commandFence), Abandoned(s, slot.lease.commandFence)))
    { mTelemetry.reason = NRIStaticTangentReason::PendingLease; return false; }
    if (!EnsurePipeline(s))
    { mTelemetry.reason = NRIStaticTangentReason::Pipeline; return false; }
    const auto& c = s.resources.context;
    nri::BufferBarrierDesc transition = {};
    transition.buffer = slot.output.buffer;
    transition.before = slot.lease.readable ? NRIResourceComputeShaderResourceAccess() : nri::AccessStage{};
    transition.after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
    nri::BarrierDesc barrier = {};
    barrier.buffers = &transition;
    barrier.bufferNum = 1;
    c.core->CmdBeginAnnotation(*c.commandBuffer, "Raze.StaticTangent.Build", nri::BGRA_UNUSED);
    c.core->CmdBarrier(*c.commandBuffer, barrier);
    c.core->CmdSetPipelineLayout(*c.commandBuffer, nri::BindPoint::COMPUTE, *mLayout);
    const uint32_t constants[] = { binding.primitiveCount, binding.vertexCount, 0, 0 };
    c.core->CmdSetRootConstants(*c.commandBuffer, { 0, constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
    c.core->CmdSetRootDescriptor(*c.commandBuffer, { 0, binding.vertices, 0, nri::BindPoint::COMPUTE });
    c.core->CmdSetRootDescriptor(*c.commandBuffer, { 1, binding.primitives, 0, nri::BindPoint::COMPUTE });
    c.core->CmdSetRootDescriptor(*c.commandBuffer, { 2, slot.output.storageView, 0, nri::BindPoint::COMPUTE });
    c.core->CmdSetPipeline(*c.commandBuffer, *mPipeline);
    const uint32_t groups = (binding.primitiveCount + 63u) / 64u;
    static_assert(ByteCap / RecordStride <= 65535ull * 64ull);
    c.core->CmdDispatch(*c.commandBuffer, { groups, 1, 1 });
    transition.before = transition.after;
    transition.after = NRIResourceComputeShaderResourceAccess();
    c.core->CmdBarrier(*c.commandBuffer, barrier);
    c.core->CmdEndAnnotation(*c.commandBuffer);
    slot.lease.Record(frame.commandFence);
    ++mTelemetry.builds;
    mTelemetry.primitiveCount = binding.primitiveCount;
    mTelemetry.reason = NRIStaticTangentReason::None;
    return true;
}

void NRIStaticTangentScratch::Destroy(const NRIResourceServices& services)
{
    for (auto& slot : mSlots) services.DestroyBufferResource(slot.output);
    services.DestroyBufferResource(mFallback);
    if (services.context.core)
    {
        if (mPipeline) services.context.core->DestroyPipeline(mPipeline);
        if (mLayout) services.context.core->DestroyPipelineLayout(mLayout);
    }
    mSlots = {};
    mPipeline = nullptr;
    mLayout = nullptr;
    mTelemetry = {};
    mNextPublication = 1;
}
