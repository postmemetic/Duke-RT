#pragma once

#include "perf_audit.h"

// Diagnostic observations only: never authorize skipping capture, upload or
// descriptor writes. Each call site retains one observation, not a history map.
struct NRIWallDecalAuditCall
{
    const void* owner = nullptr;
    uint64_t epoch = 0;
    uint64_t presentation = 0;
};

inline bool NoteNRIWallDecalAuditCall(const void* owner, NRIWallDecalAuditCall& previous,
    PerfAuditMetric calls, PerfAuditMetric repeats)
{
    if (!PerfAuditActive()) return false;
    const auto token = PerfAuditCurrentToken();
    PerfAuditAdd(calls);
    if (token && previous.owner == owner && previous.epoch == token.epoch &&
        previous.presentation == token.presentation)
        PerfAuditAdd(repeats);
    previous = { owner, token.epoch, token.presentation };
    return true;
}

struct NRIWallDecalAuditDescriptorTuple
{
    const void* owner = nullptr;
    const void* set = nullptr;
    const void* headerBuffer = nullptr;
    const void* recordBuffer = nullptr;
    const void* headerView = nullptr;
    const void* recordView = nullptr;
    uint64_t epoch = 0;
    uint64_t presentation = 0;
    uint64_t mapEpoch = 0;
    uint64_t buildEpoch = 0;
    uint64_t generation = 0;

    bool SameKnownPublication(const NRIWallDecalAuditDescriptorTuple& rhs) const
    {
        return epoch != 0 && set != nullptr && headerView != nullptr && recordView != nullptr &&
            owner == rhs.owner && set == rhs.set && headerBuffer == rhs.headerBuffer && recordBuffer == rhs.recordBuffer &&
            headerView == rhs.headerView && recordView == rhs.recordView &&
            epoch == rhs.epoch && presentation == rhs.presentation && mapEpoch == rhs.mapEpoch &&
            buildEpoch == rhs.buildEpoch && generation == rhs.generation;
    }
};

inline NRIWallDecalAuditDescriptorTuple& LastNRIWallDecalAuditPublication()
{
    static thread_local NRIWallDecalAuditDescriptorTuple publication;
    return publication;
}
