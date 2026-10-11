#pragma once
#include <cstdint>
#include <limits>

// Allocation/binding lifetime only. This is deliberately not a content cache.
struct NRIStaticTangentLease
{
    uint64_t epoch = std::numeric_limits<uint64_t>::max();
    uint64_t publication = 0;
    uint64_t commandFence = 0;
    bool published = false;
    bool readable = false;
    bool readableBeforeFence = false;

    bool CanBeginEpoch(uint64_t nextEpoch, bool priorFrameComplete,
        bool priorCommandComplete, bool priorCommandAbandoned) const
    {
        if (nextEpoch == epoch) return true;
        if (epoch != std::numeric_limits<uint64_t>::max() && nextEpoch < epoch) return false;
        return priorFrameComplete && (!commandFence || priorCommandComplete || priorCommandAbandoned);
    }
    void BeginEpoch(uint64_t nextEpoch)
    {
        if (epoch == nextEpoch) return;
        epoch = nextEpoch;
        published = false;
        publication = 0;
        commandFence = 0;
        // Contents remain irrelevant; preserve the actual/predicted access state.
    }
    bool CanGrow() const { return !published; }
    void Publish(uint64_t value) { published = true; publication = value; }
    bool Matches(uint64_t expectedEpoch, uint64_t expectedPublication) const
    { return published && publication != 0 && epoch == expectedEpoch && publication == expectedPublication; }
    bool CanRecord(uint64_t fence, bool priorComplete, bool priorAbandoned) const
    { return fence && (!commandFence || commandFence == fence || priorComplete || priorAbandoned); }
    void Record(uint64_t fence)
    {
        if (commandFence != fence) readableBeforeFence = readable;
        commandFence = fence;
        readable = true;
    }
    void ReconcileAbandonment(bool abandoned)
    {
        if (!commandFence || !abandoned) return;
        readable = readableBeforeFence;
        commandFence = 0;
        publication = 0;
        // Do not unlock growth after same-epoch abandonment: already published
        // scene descriptors can still exist in other command recordings.
    }
};
