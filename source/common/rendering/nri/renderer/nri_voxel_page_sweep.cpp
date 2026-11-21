#include "nri_voxel_page_sweep.h"
#include "perf_audit.h"
#include <cstdio>

void NRIAuditColdPageSweep(const std::vector<NRIVoxelSweepCandidate>& ordered, uint64_t pageKey,
    uint64_t generation, uint64_t target, uint32_t frame, uint32_t grace)
{
    const auto choice = NRIPriceColdPageSweep(ordered, pageKey, target, frame, grace);
    if (!choice.remaining) return;
    const auto& page = ordered[choice.page];
    const auto& donor = ordered[choice.donor];
    const bool cheaper = choice.page != choice.donor;
    char fields[864];
    std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"generation\":%llu,\"mesh_key\":%llu,"
        "\"action\":\"%s\",\"physical_target\":%llu,\"remaining\":%llu,\"page_bytes\":%llu,"
        "\"page_physical\":%llu,\"page_since\":%u,\"donor_mesh\":%llu,\"donor_bytes\":%llu,"
        "\"donor_physical\":%llu,\"donor_since\":%u,\"grace\":%u,\"same_tier_physical\":%llu,"
        "\"cheaper_count\":%zu,\"cheaper_physical\":%llu,\"largest_cheaper_physical\":%llu",
        frame, (unsigned long long)generation, (unsigned long long)pageKey,
        cheaper ? "observe-cheaper-cold" : "retain-ordinary-order",
        (unsigned long long)target, (unsigned long long)choice.remaining, (unsigned long long)page.bytes,
        (unsigned long long)page.physicalBytes, page.since, (unsigned long long)(cheaper ? donor.key : 0),
        (unsigned long long)(cheaper ? donor.bytes : 0), (unsigned long long)(cheaper ? donor.physicalBytes : 0),
        cheaper ? donor.since : 0, grace, (unsigned long long)choice.sameTierPhysical,
        choice.cheaperCount, (unsigned long long)choice.cheaperPhysical, (unsigned long long)choice.largestCheaper);
    PerfAuditWriteEvent("voxel_page_sweep_price", PerfAuditCurrentToken(), fields);
}
