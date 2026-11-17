#include "nri_voxel_preload_budget.h"
#include "nri_persistent_voxel_geometry_arena_policy.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace
{
uint64_t Add(uint64_t a, uint64_t b)
{
    return b > std::numeric_limits<uint64_t>::max() - a ? std::numeric_limits<uint64_t>::max() : a + b;
}
}

uint64_t EstimateNRIVoxelPreloadPeak(uint64_t geometryBytes, uint64_t tailBytes,
    uint64_t sourceBytes, uint64_t largestBytes, uint32_t peakPercent, bool strict)
{
    const auto budget = BuildNRIPersistentVoxelGeometryArenaBudget(0, UINT64_MAX, 0,
        geometryBytes, geometryBytes, sourceBytes, peakPercent);
    if (budget.overflow) return UINT64_MAX;
    uint64_t additional = budget.requiredBytes;
    if (strict)
    {
        const auto arena = BuildNRIPersistentVoxelGeometryArenaPlan(geometryBytes, tailBytes, largestBytes);
        if (arena.overflow) return UINT64_MAX;
        additional = (std::max)(additional, Add(arena.targetGeometryBytes, sourceBytes));
    }
    return additional;
}

NRIVoxelPreloadBudgetSelection SelectNRIVoxelPreloadBudgetGroups(
    const std::vector<NRIVoxelPreloadBudgetGroup>& groups, const NRIVoxelPreloadBudgetLimits& limits)
{
    NRIVoxelPreloadBudgetSelection result;
    if (!limits.budget) return result;
    result.applied = true;
    std::vector<uint32_t> order(groups.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b)
    {
        if (groups[a].family != groups[b].family) return groups[a].family;
        return groups[a].family && groups[a].order != groups[b].order ? groups[a].order < groups[b].order : a < b;
    });
    struct Mesh { uint64_t bytes; bool preload; };
    std::unordered_map<uint64_t, Mesh> meshes;
    std::unordered_map<uint64_t, uint64_t> sources;
    for (uint32_t index : order)
    {
        const auto& group = groups[index];
        if (!group.eligible || group.resources.empty()) continue;
        std::unordered_map<uint64_t, Mesh> newMeshes;
        std::unordered_map<uint64_t, uint64_t> newSources;
        uint64_t geometry = result.geometryBytes, tail = result.tailBytes, source = result.sourceBytes, largest = result.largestBytes;
        bool valid = true;
        for (const auto& item : group.resources)
        {
            if (!item.mesh || !item.source) { valid = false; break; }
            auto prior = meshes.find(item.mesh);
            auto pending = newMeshes.find(item.mesh);
            const Mesh* existing = pending != newMeshes.end() ? &pending->second : prior != meshes.end() ? &prior->second : nullptr;
            if (existing && existing->bytes != item.geometryBytes) { valid = false; break; }
            if (!existing)
            {
                newMeshes.emplace(item.mesh, Mesh{item.geometryBytes, item.preload});
                if (item.preload) geometry = Add(geometry, item.geometryBytes); else tail = Add(tail, item.geometryBytes);
                largest = (std::max)(largest, item.geometryBytes);
            }
            else if (item.preload && !existing->preload)
            {
                tail -= item.geometryBytes;
                geometry = Add(geometry, item.geometryBytes);
                newMeshes[item.mesh] = {item.geometryBytes, true};
            }
            auto oldSource = sources.find(item.source);
            auto newSource = newSources.find(item.source);
            if ((oldSource != sources.end() && oldSource->second != item.sourceBytes) ||
                (newSource != newSources.end() && newSource->second != item.sourceBytes)) { valid = false; break; }
            if (oldSource == sources.end() && newSource == newSources.end())
            { newSources.emplace(item.source, item.sourceBytes); source = Add(source, item.sourceBytes); }
        }
        const uint64_t peak = EstimateNRIVoxelPreloadPeak(geometry, tail, source, largest, limits.peakPercent, limits.strict);
        // Subtraction keeps the actual guard inclusive at the exact boundary.
        if (!valid || peak == UINT64_MAX || limits.usage > limits.budget || limits.reserve > limits.budget - limits.usage ||
            peak > limits.budget - limits.usage - limits.reserve) continue;
        for (const auto& item : newMeshes) meshes[item.first] = item.second;
        sources.insert(newSources.begin(), newSources.end());
        result.geometryBytes = geometry; result.tailBytes = tail; result.sourceBytes = source;
        result.largestBytes = largest; result.peakAdditional = peak; result.selected.push_back(index);
    }
    return result;
}
