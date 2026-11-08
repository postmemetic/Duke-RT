#include "nri_renderer.h"
#include "nri_persistent_voxel_services.h"
#include "perf_audit.h"
#include <cstdio>

// Called only from OnLevelUnloadBegin, after its explicit command wait, source
// authority reset, and admission cancellation. This proof also covers reload
// of the same map; ordinary camera transitions never enter this path.
void NRIRenderer::TrimPersistentVoxelCacheAfterLevelWait()
{
    auto& cache = mPersistentVoxels;
    if (!cache.batch.actors.empty() || !cache.instances.empty() || !cache.admissionQueue.empty()) return;
    // The explicit wait also covers compaction queries/copies. Drop its raw
    // source pointers and unadopted destinations before erasing cache records.
    ResetPersistentVoxelBlasCompaction();
    auto services = BuildNRIPersistentVoxelResetServices(*this);
    // Completion is established above even when no command buffer is open.
    services.cacheRetirementAllowed = [](void*) { return true; };
    mVoxelBudgetSnapshotFrame = UINT32_MAX;
    const auto budget = GetPersistentVoxelBudgetSnapshot();
    cache.RetirePendingMaterialRanges(services);
    cache.MaintainInactiveCache("transition", mFrameIndex, BuildNRIPersistentVoxelSettingsFromCVars(),
        budget.usageBytes, budget.budgetBytes, services, true);
    // Every ticket predates the explicit wait or belongs to the now-cancelled
    // empty scene. Reclaim without waiting for another three presented frames.
    const auto releaseMaterial = [&](NRIPersistentVoxelMaterialRangeHandle range)
    {
        if (cache.materialPayloads.ReleaseRetiredRange(range, cache.materialRangeAllocator))
        {
            cache.materialResourceGeneration++;
            cache.MarkMaintenanceMutation();
        }
        // A stale ticket is harmless; an owned but unreleasable payload must
        // remain visible as quarantined instead of being reported as freed.
        return !cache.materialRangeAllocator.Owns(range);
    };
    for (auto& scratch : mResidentUploadScratchFrames)
    {
        for (auto ticket : scratch.retiredVoxelSchedulerReservations)
            if (!cache.ReleaseCompletedSchedulerReservation(ticket)) cache.quarantinedSchedulerReservations.push_back(ticket);
        scratch.retiredVoxelSchedulerReservations.clear();
        for (auto range : scratch.retiredVoxelGeometryRanges) cache.ReleaseCompletedGeometryRange(range);
        scratch.retiredVoxelGeometryRanges.clear();
        for (auto range : scratch.retiredVoxelMaterialRanges)
            if (!releaseMaterial(range)) cache.quarantinedMaterialRanges.push_back(range);
        scratch.retiredVoxelMaterialRanges.clear();
    }
    for (auto range : cache.quarantinedGeometryRanges) cache.ReleaseCompletedGeometryRange(range);
    cache.quarantinedGeometryRanges.clear();
    for (auto it = cache.quarantinedMaterialRanges.begin(); it != cache.quarantinedMaterialRanges.end(); )
        if (releaseMaterial(*it)) it = cache.quarantinedMaterialRanges.erase(it); else ++it;
    for (auto it = cache.quarantinedSchedulerReservations.begin(); it != cache.quarantinedSchedulerReservations.end(); )
        if (cache.ReleaseCompletedSchedulerReservation(*it)) it = cache.quarantinedSchedulerReservations.erase(it); else ++it;
    uint64_t releasedArenaBytes = 0;
    if (cache.meshVariantResources.empty() && cache.geometryRanges.VertexStats().liveRows == 0 &&
        cache.geometryRanges.IndexStats().liveRows == 0 && cache.geometryRanges.PrimitiveStats().liveRows == 0)
    {
        releasedArenaBytes = cache.vertexBuffer.memorySize + cache.indexBuffer.memorySize + cache.primitiveBuffer.memorySize;
        DestroyBufferResource(cache.vertexBuffer);
        DestroyBufferResource(cache.indexBuffer);
        DestroyBufferResource(cache.primitiveBuffer);
        cache.geometryRanges.Reset();
        cache.SyncGeometryCursors();
        cache.arenaPresizeBuildSerial = 0;
        cache.MarkMaintenanceMutation();
    }
    if (PerfAuditActive())
    {
        char fields[640];
        snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"map_epoch\":%u,\"mesh_resources\":%llu,"
            "\"material_resources\":%llu,\"geometry_committed_released_bytes\":%llu,\"quiescent\":true,"
            "\"geometry_quarantined_ranges_remaining\":%llu,\"material_quarantined_ranges_remaining\":%llu,"
            "\"scheduler_quarantined_reservations_remaining\":%llu",
            mFrameIndex, cache.residencyMapGeneration, (unsigned long long)cache.meshVariantResources.size(),
            (unsigned long long)cache.materialVariantResources.size(), (unsigned long long)releasedArenaBytes,
            (unsigned long long)cache.quarantinedGeometryRanges.size(), (unsigned long long)cache.quarantinedMaterialRanges.size(),
            (unsigned long long)cache.quarantinedSchedulerReservations.size());
        PerfAuditWriteEvent("voxel_cache_transition", PerfAuditCurrentToken(), fields);
    }
}
