#include "nri_renderer.h"
#include "nri_cvars.h"
#include "../system/nri_renderdevice.h"
#include "../system/nri_gpu_timing.h"

static_assert(NRIStaticTangentScratch::SlotCount == NRIFrameShell::QueuedFrameCount);

NRIStaticTangentServices NRIRenderer::BuildStaticTangentServices()
{
    NRIStaticTangentServices s;
    s.resources = BuildResourceServices();
    s.user = mFrameBuffer;
    s.commandComplete = [](void* p, uint64_t f) { return p && static_cast<NRIRenderDevice*>(p)->IsCommandFenceValueComplete(f); };
    s.commandAbandoned = [](void* p, uint64_t f) { return p && static_cast<NRIRenderDevice*>(p)->IsCommandFenceValueAbandoned(f); };
    s.loadProducer = [](void* p, std::vector<uint8_t>& blob)
    {
        if (!p) return false;
        auto* backend = static_cast<NRIRenderDevice*>(p);
        return backend->LoadShaderBlob(backend->GetSelectedAPI() == nri::GraphicsAPI::D3D12 ?
            "StaticShadingBuild.cs.dxil" : "StaticShadingBuild.cs.spirv", blob);
    };
    return s;
}
NRIStaticTangentFrame NRIRenderer::BuildStaticTangentFrame() const
{
    NRIStaticTangentFrame f;
    if (!mFrameBuffer || mFrameBuffer->IsPreloadCommandContextActive() ||
        !mFrameBuffer->GetRecordingCommandFenceValue()) return f;
    const uint32_t index = mFrameBuffer->mCurrentQueuedFrameIndex;
    if (index >= mFrameBuffer->mQueuedFrames.size()) return f;
    const auto& slot = mFrameBuffer->mQueuedFrames[index];
    f.epoch = mFrameBuffer->mFrameIndex; // Backend epoch, shared by all its views.
    f.queuedSlot = index;
    f.commandFence = mFrameBuffer->GetRecordingCommandFenceValue();
    f.priorSlotFrameComplete = !slot.hasSubmittedWork || !slot.lastSubmittedFenceValue ||
        mFrameBuffer->IsFrameFenceValueComplete(slot.lastSubmittedFenceValue);
    return f;
}
nri::Descriptor* NRIRenderer::PublishStaticTangents(const NRIBufferResource& vertices,
    const NRIBufferResource& primitives, uint32_t primitiveCount)
{
    auto& state = mStaticTangents;
    state.sceneGeneration = 0;
    state.sceneSet = nullptr;
    const int mode = (int)nri_ptstatictangents;
    state.binding = state.scratch.Publish(BuildStaticTangentServices(), BuildStaticTangentFrame(),
        vertices, primitives, primitiveCount, mode == 1 || mode == 2);
    return state.binding.output;
}
void NRIRenderer::CommitStaticTangentPublication()
{
    auto& state = mStaticTangents;
    if (!IsCurrentSceneDataDescriptorsInitialized() || !state.binding.publication ||
        mSceneDataDescriptors[0] != state.binding.vertices ||
        mSceneDataDescriptors[2] != state.binding.primitives ||
        mSceneDataDescriptors[NRI_SCENE_DATA_STATIC_TANGENT_SLOT] != state.binding.output)
    { state.sceneGeneration = 0; state.sceneSet = nullptr; return; }
    state.sceneGeneration = mSceneDataSnapshotGeneration;
    state.sceneSet = GetCurrentSceneDataSet();
}
uint32_t NRIRenderer::RecordStaticTangents(uint32_t primitiveCount, bool supported, bool diagnosticWindow, nri::CommandBuffer* consumingCommand)
{
    auto& state = mStaticTangents;
    const int requested = (int)nri_ptstatictangents;
    const uint32_t mode = requested == 1 || requested == 2 ? uint32_t(requested) : 0u;
    if (mode == 1 && state.lastRequestedMode != 1) state.probeDispatchesRemaining = 4;
    state.lastRequestedMode = mode;
    const auto finish = [&](uint32_t active, NRIStaticTangentRouteReason reason)
    {
        const auto& owner = state.scratch.Telemetry();
        state.dispatch.requestedMode = uint32_t(requested);
        state.dispatch.activeMode = active;
        state.dispatch.reason = reason;
        state.dispatch.ownerReason = owner.reason;
        state.dispatch.primitiveCount = primitiveCount;
        state.dispatch.probeDispatchesRemaining = state.probeDispatchesRemaining;
        state.dispatch.outputAllocatedBytes = owner.allocatedBytes;
        state.dispatch.fallbackAllocatedBytes = owner.fallbackAllocatedBytes;
        state.dispatch.totalOwnerAllocatedBytes = owner.allocatedBytes + owner.fallbackAllocatedBytes;
        state.dispatch.builds = owner.builds;
        return active;
    };
    if (!mode) return finish(0, NRIStaticTangentRouteReason::Disabled);
    const auto services = BuildStaticTangentServices();
    if (!consumingCommand || consumingCommand != services.resources.context.commandBuffer)
        return finish(0, NRIStaticTangentRouteReason::BindingMismatch);
    if (!supported || !mFrameBuffer || !mActiveSceneDataSnapshot ||
        mActiveSceneDataSetFrameIndex != mFrameIndex ||
        (mode == 1 && !mFrameBuffer->UsesDiagnosticShaderVariant()))
        return finish(0, NRIStaticTangentRouteReason::Unsupported);
    // Never enable global shader statistics here. Existing after-drain trace
    // collection must already be active; warmup/compact keep legacy mode0.
    if (mode == 1 && !diagnosticWindow) return finish(0, NRIStaticTangentRouteReason::DiagnosticWindow);
    if (mode == 1 && !state.probeDispatchesRemaining) return finish(0, NRIStaticTangentRouteReason::ProbeQuota);
    if (!state.sceneGeneration || state.sceneGeneration != mSceneDataSnapshotGeneration ||
        state.sceneSet != GetCurrentSceneDataSet() || !IsCurrentSceneDataDescriptorsInitialized())
        return finish(0, NRIStaticTangentRouteReason::MissingPublication);
    const auto frame = BuildStaticTangentFrame();
    const auto& binding = state.binding;
    if (binding.epoch != frame.epoch || binding.slot != frame.queuedSlot ||
        binding.primitiveCount != primitiveCount || mBoundStaticPrimitiveCount != primitiveCount ||
        mSceneDataDescriptors[0] != binding.vertices || mSceneDataDescriptors[2] != binding.primitives ||
        mSceneDataDescriptors[NRI_SCENE_DATA_STATIC_TANGENT_SLOT] != binding.output)
        return finish(0, NRIStaticTangentRouteReason::BindingMismatch);
    bool recorded = false;
    {
        NRIScopedGpuTiming producerTiming(mFrameBuffer, NRIGpuTimingScope::StaticTangentBuild);
        recorded = state.scratch.Record(services, frame, binding);
    }
    if (!recorded)
        return finish(0, NRIStaticTangentRouteReason::OwnerFallback);
    if (mode == 1) --state.probeDispatchesRemaining;
    return finish(mode, NRIStaticTangentRouteReason::Active);
}
