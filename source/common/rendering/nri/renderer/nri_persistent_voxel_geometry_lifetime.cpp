#include "nri_persistent_voxels.h"
#include "perf_audit.h"
#include <algorithm>

void NRIPersistentVoxelResidency::SyncGeometryCursors()
{
    arenaVertexCursor = uint32_t(geometryRanges.VertexStats().cursorRows);
    arenaIndexCursor = uint32_t(geometryRanges.IndexStats().cursorRows);
    arenaPrimitiveCursor = uint32_t(geometryRanges.PrimitiveStats().cursorRows);
}

bool NRIPersistentVoxelResidency::AllocateGeometryRange(PersistentVoxelMeshVariantResource& resource,
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

void NRIPersistentVoxelResidency::ReleaseCompletedGeometryRange(NRIPersistentVoxelGeometryRangeHandle range)
{
    if (geometryRanges.Release(range))
    {
        PerfAuditAdd(PerfAuditMetric::VoxelGeometryRangeReleases);
        PerfAuditAdd(PerfAuditMetric::VoxelGeometryRangeReleasedBytes,
            uint64_t(range.vertices.capacity) * sizeof(nri_scene::SceneVertex) +
            uint64_t(range.indices.capacity) * sizeof(uint32_t) + uint64_t(range.primitives.capacity) * sizeof(nri_scene::PrimitiveData));
        SyncGeometryCursors();
        MarkMaintenanceMutation();
    }
}
