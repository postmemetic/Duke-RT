#include "nri_persistent_voxel_geometry_ranges.h"

NRIPersistentVoxelGeometryRangeHandle NRIPersistentVoxelGeometryRanges::Allocate(
    uint32_t vertices, uint32_t indices, uint32_t primitives)
{
    if (!vertices || !indices || !primitives || !nextTransaction) return {};
    NRIPersistentVoxelGeometryRangeHandle handle;
    handle.vertices = streams[0].Allocate(vertices);
    if (!handle.vertices) return {};
    handle.indices = streams[1].Allocate(indices);
    if (!handle.indices)
    {
        streams[0].Release(handle.vertices);
        return {};
    }
    handle.primitives = streams[2].Allocate(primitives);
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

bool NRIPersistentVoxelGeometryRanges::Owns(const NRIPersistentVoxelGeometryRangeHandle& handle) const
{
    if (!handle) return false;
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
