#pragma once
#include "nri_resources.h"
#include "nri_static_tangent_lease.h"
#include <array>
#include <vector>

struct NRIStaticTangentServices
{
    NRIResourceServices resources;
    void* user = nullptr;
    bool (*commandComplete)(void*, uint64_t) = nullptr;
    bool (*commandAbandoned)(void*, uint64_t) = nullptr;
    bool (*loadProducer)(void*, std::vector<uint8_t>&) = nullptr;
};
// Integration must capture this from the backend shell, not renderer frameIndex.
struct NRIStaticTangentFrame
{
    uint64_t epoch = UINT64_MAX;
    uint64_t commandFence = 0;
    uint32_t queuedSlot = UINT32_MAX;
    bool priorSlotFrameComplete = false;
};
enum class NRIStaticTangentReason : uint32_t
{
    None, Disabled, InvalidServices, InvalidFrame, PendingLease,
    InvalidSource, Capacity, Allocation, Pipeline, MissingPublication,
    BindingMismatch
};
struct NRIStaticTangentTelemetry
{
    NRIStaticTangentReason reason = NRIStaticTangentReason::Disabled;
    uint32_t primitiveCount = 0;
    // allocatedBytes is output allocation only, the domain of ByteCap.
    uint64_t allocatedBytes = 0, fallbackAllocatedBytes = 0, publications = 0, builds = 0;
};
class NRIStaticTangentScratch
{
public:
    static constexpr uint32_t SlotCount = 3, RecordStride = 32;
    static constexpr uint64_t ByteCap = 64ull * 1024ull * 1024ull;
    struct Binding
    {
        uint64_t epoch = UINT64_MAX, publication = 0;
        uint32_t slot = UINT32_MAX, primitiveCount = 0, vertexCount = 0;
        nri::Descriptor* output = nullptr;
        nri::Descriptor* vertices = nullptr;
        nri::Descriptor* primitives = nullptr;
        bool usable = false;
    };
    // Always returns a stable valid typed output when resources are available.
    // The caller publishes it into t28 even when disabled/overflowed.
    Binding Publish(const NRIStaticTangentServices&, const NRIStaticTangentFrame&,
        const NRIBufferResource& vertices, const NRIBufferResource& primitives,
        uint32_t primitiveCount, bool enabled);
    // Rebuilds every time; caller must restore all consuming pipeline bindings.
    bool Record(const NRIStaticTangentServices&, const NRIStaticTangentFrame&, const Binding&);
    void Destroy(const NRIResourceServices&); // Only after shell-owned quiescence.
    const NRIStaticTangentTelemetry& Telemetry() const { return mTelemetry; }
private:
    struct Slot { NRIStaticTangentLease lease; NRIBufferResource output; nri::Descriptor* epochView = nullptr; };
    std::array<Slot, SlotCount> mSlots{};
    NRIBufferResource mFallback;
    nri::PipelineLayout* mLayout = nullptr;
    nri::Pipeline* mPipeline = nullptr;
    uint64_t mNextPublication = 1;
    NRIStaticTangentTelemetry mTelemetry;
    bool EnsurePipeline(const NRIStaticTangentServices&);
    bool EnsureFallback(const NRIStaticTangentServices&);
};
