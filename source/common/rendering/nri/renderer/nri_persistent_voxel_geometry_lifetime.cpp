#include "nri_persistent_voxels.h"
#include "perf_audit.h"
#include "printf.h"
#include "nri_cvars.h"
#include "nri_diagnostic_names.h"
#include "../scene/nri_hash.h"
#include <cstdio>
#include <algorithm>

CVAR(Bool, nri_ptvoxelpageactorpriority, false, 0)

namespace
{
    uint64_t SelectedOverflowMesh()
    {
        switch (int(nri_ptvoxeloverflowpagetarget))
        {
        case 0: return NRIVoxelOverflowPage::TelescopeMesh;
        case 1: return 14261219071072162745ull; // 1905_EXPLODE.kvx
        default: return 0;
        }
    }
}

void NRIPersistentVoxelResidency::SyncGeometryCursors()
{
    arenaVertexCursor = uint32_t(geometryRanges.VertexStats().cursorRows);
    arenaIndexCursor = uint32_t(geometryRanges.IndexStats().cursorRows);
    arenaPrimitiveCursor = uint32_t(geometryRanges.PrimitiveStats().cursorRows);
}

void NRIPersistentVoxelResidency::ResetGeometryRanges()
{
    geometryRanges.Reset();
    TraceGeometryLease("reset", 0, {});
}

void NRIPersistentVoxelResidency::RollbackUnsubmittedGeometryRange(PersistentVoxelMeshVariantResource& resource,
    const NRIPersistentVoxelGeometryRangeHandle& previous)
{
    if (resource.geometryRange.transaction == previous.transaction &&
        resource.geometryRange.overflowPage == previous.overflowPage) return;
    // Only used before the allocation service records any copy/compute command.
    // Published and submitted ranges must continue through fence retirement.
    ReleaseCompletedGeometryRange(resource.geometryRange, false);
    resource.geometryRange = previous;
    resource.vertexOffset = previous.vertices.offset;
    resource.vertexCapacity = previous.vertices.capacity;
    resource.indexOffset = previous.indices.offset;
    resource.indexCapacity = previous.indices.capacity;
    resource.primitiveOffset = previous.ShaderPrimitiveBase();
    resource.primitiveCapacity = previous.primitives.capacity;
}

bool NRIPersistentVoxelResidency::AllocateGeometryRange(PersistentVoxelMeshVariantResource& resource, uint64_t meshKey,
    uint32_t vertices, uint32_t indices, uint32_t primitives, NRIPersistentVoxelGeometryRangeHandle& previous)
{
    previous = {};
    if (geometryRanges.Owns(resource.geometryRange) && resource.vertexCapacity >= vertices &&
        resource.indexCapacity >= indices && resource.primitiveCapacity >= primitives)
        return true;
    const uint64_t oldHigh[3] = { geometryRanges.VertexStats().highWaterRows,
        geometryRanges.IndexStats().highWaterRows, geometryRanges.PrimitiveStats().highWaterRows };
    const auto replacement = geometryRanges.Allocate(vertices, indices, primitives);
    if (!replacement) return false;
    TraceGeometryLease("allocate", meshKey, replacement);
    PerfAuditAdd(PerfAuditMetric::VoxelGeometryRangeAllocations);
    const auto reused = [](const NRIPersistentVoxelMaterialRangeHandle& range, uint64_t high)
    { return high > range.offset ? std::min<uint64_t>(range.capacity, high - range.offset) : 0; };
    PerfAuditAdd(PerfAuditMetric::VoxelGeometryRangeReusedBytes,
        reused(replacement.vertices, oldHigh[0]) * sizeof(nri_scene::SceneVertex) +
        reused(replacement.indices, oldHigh[1]) * sizeof(uint32_t) +
        reused(replacement.primitives, oldHigh[2]) * sizeof(nri_scene::PrimitiveData));
    previous = resource.geometryRange;
    resource.geometryRange = replacement;
    resource.vertexOffset = replacement.vertices.offset;
    resource.vertexCapacity = replacement.vertices.capacity;
    resource.indexOffset = replacement.indices.offset;
    resource.indexCapacity = replacement.indices.capacity;
    resource.primitiveOffset = replacement.primitives.offset;
    resource.primitiveCapacity = replacement.primitives.capacity;
    SyncGeometryCursors();
    return true;
}

void NRIPersistentVoxelResidency::ReleaseCompletedGeometryRange(NRIPersistentVoxelGeometryRangeHandle range, bool committedRetirement)
{
    if (range.overflowPage)
    {
        // Unsubmitted rollback has no physical buffers. The renderer destroys
        // a committed triplet at proven fence completion before releasing it.
        if (!overflowVertex.buffer && !overflowIndex.buffer && !overflowPrimitive.buffer && overflowPage.Release(range))
        {
            if (committedRetirement) ++reclaimGeometryRevision;
            MarkMaintenanceMutation();
        }
        return;
    }
    if (geometryRanges.Release(range))
    {
        TraceGeometryLease(committedRetirement ? "retired" : "rollback", 0, range);
        PerfAuditAdd(PerfAuditMetric::VoxelGeometryRangeReleases);
        PerfAuditAdd(PerfAuditMetric::VoxelGeometryRangeReleasedBytes,
            uint64_t(range.vertices.capacity) * sizeof(nri_scene::SceneVertex) +
            uint64_t(range.indices.capacity) * sizeof(uint32_t) + uint64_t(range.primitives.capacity) * sizeof(nri_scene::PrimitiveData));
        if (committedRetirement) ++reclaimGeometryRevision;
        SyncGeometryCursors();
        MarkMaintenanceMutation();
    }
}

bool NRIPersistentVoxelResidency::HasOverflowNoFit(uint64_t mesh) const
{
    if (!mesh || mesh != SelectedOverflowMesh() || overflowPage.Lease()) return false;
    const auto& records = admissionReclaim.Records();
    const auto found = records.find(mesh);
    if (found == records.end()) return false;
    const auto& record = found->second;
    return record.stage == NRIVoxelAdmissionReclaim::Stage::Blocked && record.noRangeFit &&
        record.request.epoch == residencyMapGeneration && record.processedRevision == reclaimGeometryRevision;
}

void NRIPersistentVoxelResidency::BindOverflowPageForScene(const std::vector<SceneInstanceData>& instances)
{
    const uint64_t previous = boundOverflowGeneration;
    boundOverflowGeneration = overflowPage.PublishedGeneration();
    if (boundOverflowGeneration)
    {
        // Primary and secondary-ray occurrences use this same bound payload.
        const bool reachable = std::any_of(instances.begin(), instances.end(), [](const SceneInstanceData& instance)
        {
            return instance.dataSource == nri_diag::SceneDataSourcePersistentVoxel &&
                (instance.primitiveBase & NRIPersistentVoxelGeometryRangeHandle::PageTag) != 0;
        });
        if (!reachable) boundOverflowGeneration = 0;
    }
    if (previous != boundOverflowGeneration && PerfAuditActive())
    {
        char fields[156];
        snprintf(fields, sizeof(fields), "\"selected\":\"%s\",\"page_generation\":%llu,\"previous_generation\":%llu",
            boundOverflowGeneration ? "page" : "ordinary", (unsigned long long)boundOverflowGeneration,
            (unsigned long long)previous);
        PerfAuditWriteEvent("voxel_trace_pipeline", PerfAuditCurrentToken(), fields);
    }
}

int NRIPersistentVoxelResidency::EffectiveOverflowPageMode()
{
    const int mode = nri_ptvoxeloverflowpage;
    if (mode != 1 && mode != 2) return 0;
    if (pageTraceState == PageTraceState::Cold)
    {
        pageTraceState = PageTraceState::WaitingForLoad;
        Printf("NRI PT voxel page pipelines: state=waiting-for-level-load admission=ordinary\n");
    }
    return pageTraceState == PageTraceState::Ready ? mode : 0;
}

uint64_t NRIPersistentVoxelResidency::CurrentExplicitPagePair(uint32_t frame) const
{
    if (!(bool)nri_ptvoxelpageactorpriority || (int)nri_ptvoxeloverflowpage != 2 ||
        pageTraceState != PageTraceState::Ready || overflowPage.Lease() || retentionQuarantined ||
        !(bool)nri_ptvoxelallocationbudget) return 0;
    const uint64_t mesh = SelectedOverflowMesh();
    if (!mesh) return 0;
    const auto demand = actorDemand.Upgrade(mesh, frame);
    if (!demand.consumers) return 0;
    const auto actor = actorDemand.GetActor(demand.identity, demand.world, demand.lifetime, frame);
    return actor.found && actor.desiredMesh == mesh && actor.desiredMaterial ?
        nri_scene::HashCombine64(mesh, actor.desiredMaterial) : 0;
}

bool NRIPersistentVoxelResidency::TryReserveOverflowPage(PersistentVoxelMeshVariantResource& resource,
    uint64_t mesh, uint32_t vertices, uint32_t indices, uint32_t primitives, uint32_t frame, bool runtime)
{
    const int mode = EffectiveOverflowPageMode();
    if ((mode != 1 && mode != 2) || !(bool)nri_ptvoxelallocationbudget ||
        !mesh || mesh != SelectedOverflowMesh() || retentionQuarantined || overflowPage.Lease()) return false;
    if (!runtime || !actorDemand.Upgrade(mesh, frame).requestedConsumers) return false;
    if (mode == 1)
    {
        const bool fits = uint64_t(arenaVertexCursor) * sizeof(nri_scene::SceneVertex) <= vertexBuffer.size &&
            uint64_t(arenaIndexCursor) * sizeof(uint32_t) <= indexBuffer.size &&
            uint64_t(arenaPrimitiveCursor) * sizeof(nri_scene::PrimitiveData) <= primitiveBuffer.size;
        if (fits || !HasOverflowNoFit(mesh) || !actorDemand.Upgrade(mesh, frame).consumers)
            return false;
    }
    if (!geometryRanges.Owns(resource.geometryRange)) return false;
    const auto page = overflowPage.Acquire(mesh, vertices, indices, primitives);
    if (!page) return false;
    // The provisional ordinary lease is still unpublished. Restore its holes
    // and tail immediately so this allocation never grows the ordinary arena.
    ReleaseCompletedGeometryRange(resource.geometryRange, false);
    resource.geometryRange = page;
    resource.vertexOffset = 0;
    resource.indexOffset = 0;
    resource.primitiveOffset = page.ShaderPrimitiveBase();
    return true;
}

void NRIPersistentVoxelResidency::ReportOverflowPage(const char* action, uint64_t fence) const
{
    if (!PerfAuditActive()) return;
    char fields[512];
    const auto& lease = overflowPage.Lease();
    snprintf(fields, sizeof(fields), "\"action\":\"%s\",\"mesh_key\":%llu,\"page_generation\":%llu,"
        "\"vertex_rows\":%u,\"index_rows\":%u,\"primitive_rows\":%u,\"geometry_committed_bytes\":%llu,\"fence\":%llu",
        action, (unsigned long long)overflowPage.Owner(), (unsigned long long)lease.transaction,
        lease.vertices.capacity, lease.indices.capacity, lease.primitives.capacity,
        (unsigned long long)(overflowVertex.memorySize + overflowIndex.memorySize + overflowPrimitive.memorySize),
        (unsigned long long)fence);
    PerfAuditWriteEvent("voxel_overflow_page", PerfAuditCurrentToken(), fields);
}

bool NRIPersistentVoxelResidency::HasPublishedGeometryBuffers(const PersistentVoxelMeshVariantResource& mesh) const
{
    if (mesh.geometryRange.overflowPage)
        return overflowPage.Published() && overflowPage.Owns(mesh.geometryRange) &&
            overflowVertex.shaderView && overflowIndex.shaderView && overflowPrimitive.shaderView;
    return vertexBuffer.shaderView && indexBuffer.shaderView && primitiveBuffer.shaderView;
}
