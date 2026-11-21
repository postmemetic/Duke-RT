#pragma once

#include "nri_persistent_voxel_geometry_ranges.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace nri { struct Buffer; }

// Physical descriptor capacities reserved above the ordinary startup layout.
// The two slots have disjoint absolute offsets in the original GPU buffers.
// Neither a live slot nor its fence retirement changes the ordinary allocator.
struct NRIVoxelRuntimeExtent
{
    std::array<nri::Buffer*, 3> buffers{};
    std::array<uint64_t, 3> physicalBytes{};
    std::array<uint64_t, 3> baseRows{};
    uint64_t extraPrimitives = 0;
    uint64_t migrationMesh = 0;
    uint32_t migrationFrame = 0;
    bool migrationServiced = false;

    bool Funded() const { return extraPrimitives != 0; }
    bool Live() const { return bool(slots[0].range) || bool(slots[1].range); }
    bool SponsoredBy(uint64_t pageGeneration) const
    {
        return pageGeneration && std::any_of(slots.begin(), slots.end(),
            [&](const auto& slot) { return slot.range && slot.pageGeneration == pageGeneration; });
    }
    bool AwaitingMigration(uint32_t frame) const
    { return migrationMesh && frame >= migrationFrame && frame - migrationFrame <= 30; }
    uint32_t PrimitiveEnd() const
    {
        uint32_t end = 0;
        for (const auto& slot : slots)
            if (slot.range) end = std::max(end, slot.range.primitives.offset + slot.range.primitives.capacity);
        return end;
    }
    void Reset()
    {
        buffers = {}; physicalBytes = {}; baseRows = {}; slots = {}; extraPrimitives = 0;
        migrationMesh = 0; migrationFrame = 0; migrationServiced = false;
        // Fence tickets from a previous backing can arrive after re-funding.
    }
    NRIPersistentVoxelGeometryRangeHandle Acquire(uint64_t mesh, uint64_t pageGeneration, const std::array<nri::Buffer*, 3>& current,
        const std::array<uint32_t, 3>& rows)
    {
        if (!mesh || !pageGeneration || !nextGeneration || !Funded() || buffers != current) return {};
        const uint64_t half = extraPrimitives / slots.size();
        const std::array<uint64_t, 3> lengths{2 * half, 3 * half, half};
        for (size_t i = 0; i < rows.size(); ++i)
            if (!rows[i] || rows[i] > lengths[i]) return {};
        for (const auto& slot : slots) if (slot.mesh == mesh) return {};
        for (size_t i = 0; i < slots.size(); ++i)
        {
            auto& slot = slots[i];
            if (slot.range) continue;
            slot.mesh = mesh;
            slot.pageGeneration = pageGeneration;
            auto& h = slot.range;
            h.runtimeExtent = true;
            h.transaction = nextGeneration++;
            h.vertices = {uint32_t(baseRows[0] + i * lengths[0]), rows[0], h.transaction};
            h.indices = {uint32_t(baseRows[1] + i * lengths[1]), rows[1], h.transaction};
            h.primitives = {uint32_t(baseRows[2] + i * lengths[2]), rows[2], h.transaction};
            return h;
        }
        return {};
    }
    bool Owns(const NRIPersistentVoxelGeometryRangeHandle& h) const
    {
        if (!h || !h.runtimeExtent || h.overflowPage) return false;
        const auto same = [](const auto& a, const auto& b)
        { return a.offset == b.offset && a.capacity == b.capacity && a.generation == b.generation; };
        for (const auto& slot : slots)
            if (slot.range.transaction == h.transaction && same(slot.range.vertices,h.vertices) &&
                same(slot.range.indices,h.indices) && same(slot.range.primitives,h.primitives)) return true;
        return false;
    }
    bool Release(const NRIPersistentVoxelGeometryRangeHandle& h)
    {
        if (!Owns(h)) return false;
        for (auto& slot : slots) if (slot.range.transaction == h.transaction) { slot = {}; return true; }
        return false;
    }
    struct Slot { uint64_t mesh = 0, pageGeneration = 0; NRIPersistentVoxelGeometryRangeHandle range; };
    std::array<Slot, 2> slots{};
private:
    // Distinct diagnostic IDs keep ordinary and extent retirement receipts unambiguous.
    uint64_t nextGeneration = uint64_t(1) << 63;
};
