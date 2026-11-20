#include "nri_voxel_page_sweep.h"
#include "nri_cvars.h"
#include "perf_audit.h"
#include <algorithm>
#include <cstdio>

CVAR(Bool, nri_ptvoxelpagecheapsweep, false, 0)

void NRIConsiderColdPageSweep(std::vector<NRIVoxelSweepCandidate>& ordered, uint64_t pageKey,
    uint64_t generation, uint64_t target, uint32_t frame, uint32_t grace)
{
    const bool enabled = nri_ptvoxelpagecheapsweep;
    const bool audit = (bool)nri_ptvoxelmeshlifetrace && PerfAuditActive();
    if (!enabled && !audit) return;
    const auto choice = NRIPriceColdPageSweep(ordered, pageKey, target, frame, grace);
    if (!choice.remaining) return;
    const auto& page = ordered[choice.page];
    const auto& donor = ordered[choice.donor];
    const bool cheaper = choice.page != choice.donor;
    if (audit)
    {
        char fields[640];
        std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"generation\":%llu,\"mesh_key\":%llu,"
            "\"action\":\"%s\",\"physical_target\":%llu,\"remaining\":%llu,\"page_bytes\":%llu,"
            "\"page_physical\":%llu,\"page_since\":%u,\"donor_mesh\":%llu,\"donor_bytes\":%llu,"
            "\"donor_physical\":%llu,\"donor_since\":%u,\"grace\":%u",
            frame, (unsigned long long)generation, (unsigned long long)pageKey,
            cheaper ? (enabled ? "use-cheaper-cold" : "observe-cheaper-cold") : "retain-ordinary-order",
            (unsigned long long)target, (unsigned long long)choice.remaining, (unsigned long long)page.bytes,
            (unsigned long long)page.physicalBytes, page.since, (unsigned long long)(cheaper ? donor.key : 0),
            (unsigned long long)(cheaper ? donor.bytes : 0), (unsigned long long)(cheaper ? donor.physicalBytes : 0),
            cheaper ? donor.since : 0, grace);
        PerfAuditWriteEvent("voxel_page_sweep_price", PerfAuditCurrentToken(), fields);
    }
    if (enabled && cheaper)
    {
        // The original prefix evicts in the same order. This one smaller, eligible physical
        // retirement pays the entire remainder before the page and preserves the hard reserve.
        std::rotate(ordered.begin() + choice.page, ordered.begin() + choice.donor, ordered.begin() + choice.donor + 1);
    }
}
