#pragma once
#include <cstdint>
#include "nri_smoke_descriptor_budget.h"
#include "nri_shader_contracts.h"

// Fixed owners only; UI/texture-set allocations have an independent lifetime cap.
constexpr uint32_t NRIStaticTangentStructuredPoolCapacity(uint32_t queuedFrames)
{
    const uint32_t snapshots = queuedFrames * 4u > 8u ? queuedFrames * 4u : 8u;
    // Scene SRVs follow the shader contract; four voxel input sets each contain four SRVs;
    // smoke publishes the complete current SRV layouts per queued frame;
    // optional grid owns six. Root-descriptor tangent producer owns NO sets.
    const uint32_t known = NRI_SCENE_DATA_DESCRIPTOR_NUM * (snapshots + queuedFrames) + 16u +
        nri_smoke_descriptors::StructuredPerQueuedFrame * queuedFrames + 6u;
    // Preserve smoke's established non-smoke reserve while also covering
    // the expanded scene snapshots if their count exceeds that reserve.
    const uint32_t smokeReserve = nri_smoke_descriptors::SharedStructuredPoolCapacity(queuedFrames);
    return known + 32u > smokeReserve ? known + 32u : smokeReserve;
}
static_assert(NRIStaticTangentStructuredPoolCapacity(2) == 570);
static_assert(NRIStaticTangentStructuredPoolCapacity(3) == 606);
static_assert(NRIStaticTangentStructuredPoolCapacity(4) == 790);
