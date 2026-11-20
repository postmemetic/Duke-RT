#include "nri_persistent_voxel_geometry_ranges.h"
#include <algorithm>
#include <unordered_set>

NRIPersistentVoxelGeometryRangeHandle NRIPersistentVoxelGeometryRanges::Allocate(
    uint32_t vertices, uint32_t indices, uint32_t primitives)
{
    if (!vertices || !indices || !primitives || !nextTransaction) return {};
    NRIPersistentVoxelGeometryRangeHandle handle;
    handle.vertices = streams[0].AllocateWithin(vertices, NRIPersistentVoxelGeometryRangeHandle::PageTag);
    if (!handle.vertices) return {};
    handle.indices = streams[1].Allocate(indices);
    if (!handle.indices)
    {
        streams[0].Release(handle.vertices);
        return {};
    }
    handle.primitives = streams[2].AllocateWithin(primitives, NRIPersistentVoxelGeometryRangeHandle::PageTag);
    if (!handle.primitives)
    {
        streams[1].Release(handle.indices);
        streams[0].Release(handle.vertices);
        return {};
    }
    handle.transaction = nextTransaction++;
    transactions.emplace(handle.transaction, handle);
    return handle;
}

NRIVoxelFamilyRangeFit NRIPersistentVoxelGeometryRanges::PreviewFamily(
    const std::vector<NRIVoxelFamilyRangeRequest>& requests, const std::array<uint64_t, 3>& capacity) const
{
    NRIVoxelFamilyRangeFit result;
    result.requiredRows = {VertexStats().cursorRows, IndexStats().cursorRows, PrimitiveStats().cursorRows};
    if (requests.empty())
    {
        result.fits = true;
        return result;
    }
    // Two material variants may already share a mesh or its pending lease.
    std::vector<NRIVoxelFamilyRangeRequest> meshes;
    meshes.reserve(requests.size());
    for (const auto& request : requests)
    {
        result.blockedMesh = request.mesh;
        if (!request.mesh || !request.primitives || request.primitives > UINT32_MAX / 3u ||
            (request.reserved.transaction && !Owns(request.reserved))) return result;
        auto shared = std::find_if(meshes.begin(), meshes.end(),
            [&](const auto& pose) { return pose.mesh == request.mesh; });
        if (shared == meshes.end()) meshes.push_back(request);
        else
        {
            if (shared->primitives != request.primitives || (shared->reserved && request.reserved &&
                shared->reserved.transaction != request.reserved.transaction)) return result;
            if (request.reserved) shared->reserved = request.reserved;
        }
    }

    auto preview = *this;
    std::unordered_set<uint64_t> usedReservations;
    for (const auto& pose : meshes)
    {
        result.blockedMesh = pose.mesh;
        const uint32_t vertices = 2u * pose.primitives, indices = 3u * pose.primitives;
        if (pose.reserved && !usedReservations.insert(pose.reserved.transaction).second) return result;
        // A too-small old lease stays live until the replacement's fence.
        if (!pose.reserved || pose.reserved.vertices.capacity < vertices ||
            pose.reserved.indices.capacity < indices || pose.reserved.primitives.capacity < pose.primitives)
        {
            if (!preview.Allocate(vertices, indices, pose.primitives)) return result;
            result.requiredRows = {preview.VertexStats().cursorRows, preview.IndexStats().cursorRows,
                preview.PrimitiveStats().cursorRows};
        }
        if (result.requiredRows[0] > capacity[0] || result.requiredRows[1] > capacity[1] ||
            result.requiredRows[2] > capacity[2]) return result;
    }
    result.fits = true;
    result.blockedMesh = 0;
    return result;
}

bool NRIPersistentVoxelGeometryRanges::Owns(const NRIPersistentVoxelGeometryRangeHandle& handle) const
{
    if (!handle || handle.overflowPage) return false;
    const auto found = transactions.find(handle.transaction);
    return found != transactions.end() &&
        found->second.vertices.generation == handle.vertices.generation &&
        found->second.indices.generation == handle.indices.generation &&
        found->second.primitives.generation == handle.primitives.generation &&
        streams[0].Owns(handle.vertices) && streams[1].Owns(handle.indices) && streams[2].Owns(handle.primitives);
}

bool NRIPersistentVoxelGeometryRanges::Release(const NRIPersistentVoxelGeometryRangeHandle& handle)
{
    // Validate all streams before mutating any of them, including duplicate or
    // stale callbacks arriving after a complete resource reset.
    if (!Owns(handle)) return false;
    streams[0].Release(handle.vertices);
    streams[1].Release(handle.indices);
    streams[2].Release(handle.primitives);
    transactions.erase(handle.transaction);
    return true;
}

void NRIPersistentVoxelGeometryRanges::Reset()
{
    // The underlying allocators preserve generation monotonicity across reset.
    for (auto& stream : streams) stream.Reset();
    transactions.clear();
}

bool NRIPersistentVoxelGeometryRanges::ValidateInvariants() const
{
    for (const auto& stream : streams) if (!stream.ValidateInvariants()) return false;
    for (const auto& transaction : transactions)
        if (transaction.first != transaction.second.transaction || !Owns(transaction.second)) return false;
    for (const auto& stream : streams)
        if (stream.Stats().allocations - stream.Stats().releases != transactions.size()) return false;
    return true;
}
