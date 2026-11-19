#pragma once

#include "nri_persistent_voxel_geometry_ranges.h"

// One dedicated mesh, three physically contiguous streams. A detached frame
// descriptor or completion ticket can never give a new occupant its old lease.
class NRIVoxelOverflowPage
{
public:
    static constexpr uint64_t TelescopeMesh = 3296701532778108590ull; // 479_TELESCOPE.kvx in the prototype fixture.
    NRIPersistentVoxelGeometryRangeHandle Acquire(uint64_t mesh, uint32_t vertices, uint32_t indices, uint32_t primitives)
    {
        constexpr auto limit = NRIPersistentVoxelGeometryRangeHandle::PageTag;
        if (lease || !nextGeneration || !mesh || !vertices || !indices || !primitives ||
            vertices >= limit || primitives >= limit) return {};
        owner = mesh;
        lease.overflowPage = true;
        lease.transaction = nextGeneration++;
        lease.vertices = {0, vertices, lease.transaction};
        lease.indices = {0, indices, lease.transaction};
        lease.primitives = {0, primitives, lease.transaction};
        return lease;
    }
    bool Owns(const NRIPersistentVoxelGeometryRangeHandle& other) const
    {
        return lease && other.overflowPage && lease.transaction == other.transaction &&
            Matches(lease.vertices, other.vertices) && Matches(lease.indices, other.indices) &&
            Matches(lease.primitives, other.primitives);
    }
    bool Publish(const NRIPersistentVoxelGeometryRangeHandle& other)
    {
        if (retiring || !Owns(other)) return false;
        published = true;
        return true;
    }
    bool BeginRetirement(const NRIPersistentVoxelGeometryRangeHandle& other, uint64_t fence)
    {
        if (retiring || !Owns(other)) return false;
        retiring = true;
        retirementFence = fence;
        published = false;
        return true;
    }
    bool Release(const NRIPersistentVoxelGeometryRangeHandle& other)
    {
        if (!Owns(other)) return false;
        lease = {};
        owner = retirementFence = 0;
        published = retiring = false;
        return true;
    }
    const NRIPersistentVoxelGeometryRangeHandle& Lease() const { return lease; }
    uint64_t Owner() const { return owner; }
    bool Published() const { return published; }
    uint64_t RetirementFence() const { return retirementFence; }
    bool Retiring() const { return retiring; }
private:
    static bool Matches(const NRIPersistentVoxelMaterialRangeHandle& a, const NRIPersistentVoxelMaterialRangeHandle& b)
    { return a.generation == b.generation && a.offset == b.offset && a.capacity == b.capacity; }
    NRIPersistentVoxelGeometryRangeHandle lease;
    uint64_t nextGeneration = 1, owner = 0, retirementFence = 0;
    bool published = false, retiring = false;
};
