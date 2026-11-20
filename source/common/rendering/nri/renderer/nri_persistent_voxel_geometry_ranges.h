#pragma once

#include "nri_persistent_voxel_material_range_allocator.h"
#include <array>
#include <cstddef>
#include <unordered_map>
#include <vector>

// Generation-qualified ownership of all three shared geometry streams. Counts
// need not have the direct compute path's 2:3:1 relationship.
struct NRIPersistentVoxelGeometryRangeHandle
{
    NRIPersistentVoxelMaterialRangeHandle vertices, indices, primitives;
    uint64_t transaction = 0;
    // Ordinary and dedicated leases have separate generation spaces.
    bool overflowPage = false;
    static constexpr uint32_t PageTag = 0x80000000u;
    uint32_t ShaderPrimitiveBase() const { return primitives.offset | (overflowPage ? PageTag : 0u); }
    explicit operator bool() const { return transaction != 0 && bool(vertices) && bool(indices) && bool(primitives); }
};

struct NRIVoxelFamilyRangeRequest
{
    uint64_t mesh = 0;
    uint32_t primitives = 0;
    NRIPersistentVoxelGeometryRangeHandle reserved;
};

struct NRIVoxelFamilyRangeFit
{
    bool fits = false;
    uint64_t blockedMesh = 0;
    std::array<uint64_t, 3> requiredRows = {};
};

class NRIPersistentVoxelGeometryRanges
{
public:
    NRIPersistentVoxelGeometryRangeHandle Allocate(uint32_t vertices, uint32_t indices, uint32_t primitives);
    // Preview all distinct missing direct-build meshes in admission order.
    // Capacities are rows in the current buffers. Retiring ranges stay owned.
    NRIVoxelFamilyRangeFit PreviewFamily(const std::vector<NRIVoxelFamilyRangeRequest>& requests,
        const std::array<uint64_t, 3>& capacity) const;
    bool Owns(const NRIPersistentVoxelGeometryRangeHandle& handle) const;
    // Caller must establish GPU completion before releasing recorded/published
    // reservations. Until then the handle remains live in this allocator.
    bool Release(const NRIPersistentVoxelGeometryRangeHandle& handle);
    void Reset();
    bool ValidateInvariants() const;
    std::size_t TransactionCount() const { return transactions.size(); }
    const NRIPersistentVoxelMaterialRangeStats& VertexStats() const { return streams[0].Stats(); }
    const NRIPersistentVoxelMaterialRangeStats& IndexStats() const { return streams[1].Stats(); }
    const NRIPersistentVoxelMaterialRangeStats& PrimitiveStats() const { return streams[2].Stats(); }
private:
    std::array<NRIPersistentVoxelMaterialRangeAllocator, 3> streams;
    std::unordered_map<uint64_t, NRIPersistentVoxelGeometryRangeHandle> transactions;
    uint64_t nextTransaction = 1;
};
