#pragma once

#include "nri_voxel_family_sweep.h"
#include <algorithm>
#include <cstddef>

struct NRIVoxelPageSweepChoice
{
    size_t page = 0, donor = 0;
    uint64_t remaining = 0;
    uint64_t sameTierPhysical = 0, cheaperPhysical = 0, largestCheaper = 0;
    size_t cheaperCount = 0;
};

// Input is the actual age/warm-ordered, unpinned mesh list, after all family protections.
// The caller limits this to automatic cache bounds and an active physical pressure sweep.
inline NRIVoxelPageSweepChoice NRIPriceColdPageSweep(const std::vector<NRIVoxelSweepCandidate>& ordered,
    uint64_t pageKey, uint64_t physicalTarget, uint32_t frame, uint32_t grace)
{
    NRIVoxelPageSweepChoice choice;
    uint64_t remaining = physicalTarget;
    for (size_t i = 0; i < ordered.size() && remaining; ++i)
    {
        const auto& mesh = ordered[i];
        if (mesh.warmRank) break;
        if (mesh.key != pageKey)
        {
            if (mesh.physicalBytes >= remaining) break;
            remaining -= mesh.physicalBytes;
            continue;
        }
        if (!mesh.physicalBytes) break;
        choice = {i, i, remaining};
        const bool aged = NRIVoxelInactiveAgeEligible(frame, mesh.since, grace);
        for (size_t j = i + 1; j < ordered.size() && !ordered[j].warmRank; ++j)
        {
            const auto& donor = ordered[j];
            if (!donor.physicalBytes || NRIVoxelInactiveAgeEligible(frame, donor.since, grace) != aged)
                continue;
            choice.sameTierPhysical += std::min(donor.physicalBytes, UINT64_MAX - choice.sameTierPhysical);
            if (donor.physicalBytes >= mesh.physicalBytes || donor.bytes > mesh.bytes) continue;
            choice.cheaperPhysical += std::min(donor.physicalBytes, UINT64_MAX - choice.cheaperPhysical);
            choice.largestCheaper = std::max(choice.largestCheaper, donor.physicalBytes);
            ++choice.cheaperCount;
            if (donor.physicalBytes >= remaining && donor.physicalBytes < ordered[choice.donor].physicalBytes)
                choice.donor = j;
        }
        break;
    }
    return choice;
}

void NRIAuditColdPageSweep(const std::vector<NRIVoxelSweepCandidate>& ordered, uint64_t pageKey,
    uint64_t generation, uint64_t target, uint32_t frame, uint32_t grace);
