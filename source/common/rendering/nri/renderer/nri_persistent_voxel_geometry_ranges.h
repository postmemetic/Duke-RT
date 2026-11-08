#pragma once

#include "nri_persistent_voxel_material_range_allocator.h"
#include <array>
#include <unordered_map>

// Generation-qualified ownership of all three shared geometry streams. Counts
// need not have the direct compute path's 2:3:1 relationship.
struct NRIPersistentVoxelGeometryRangeHandle
{
    NRIPersistentVoxelMaterialRangeHandle vertices, indices, primitives;
    uint64_t transaction = 0;
    explicit operator bool() const { return transaction != 0 && bool(vertices) && bool(indices) && bool(primitives); }
};

class NRIPersistentVoxelGeometryRanges
{
public:
    NRIPersistentVoxelGeometryRangeHandle Allocate(uint32_t vertices, uint32_t indices, uint32_t primitives);
    bool Owns(const NRIPersistentVoxelGeometryRangeHandle& handle) const;
    // Caller must establish GPU completion before releasing recorded/published
    // reservations. Until then the handle remains live in this allocator.
    bool Release(const NRIPersistentVoxelGeometryRangeHandle& handle);
    void Reset();
    bool ValidateInvariants() const;
    const NRIPersistentVoxelMaterialRangeStats& VertexStats() const { return streams[0].Stats(); }
    const NRIPersistentVoxelMaterialRangeStats& IndexStats() const { return streams[1].Stats(); }
    const NRIPersistentVoxelMaterialRangeStats& PrimitiveStats() const { return streams[2].Stats(); }
private:
    std::array<NRIPersistentVoxelMaterialRangeAllocator, 3> streams;
    std::unordered_map<uint64_t, NRIPersistentVoxelGeometryRangeHandle> transactions;
    uint64_t nextTransaction = 1;
};
