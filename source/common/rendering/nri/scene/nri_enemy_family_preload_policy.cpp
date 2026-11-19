#include "nri_enemy_family_preload_policy.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace nri_scene
{
std::vector<EnemyFamilyPreloadSet> SelectEnemyFamilyPreloadSets(
    const std::vector<EnemyFamilyPreloadSet>& candidates,
    const EnemyFamilyPreloadCost& measure, uint32_t poseLimit)
{
    std::vector<EnemyFamilyPreloadSet> families;
    std::unordered_map<uint64_t, size_t> bySignature;
    for (const auto& candidate : candidates)
    {
        if (!candidate.family || !candidate.signature || candidate.textures.empty() || candidate.textures.size() > 64) continue;
        auto existing = bySignature.find(candidate.signature);
        if (existing != bySignature.end())
        {
            // A signature collision must never silently combine coverage.
            auto& family = families[existing->second];
            if (family.family == candidate.family && family.textures == candidate.textures)
            {
                family.liveActors += candidate.liveActors;
                family.spawnSources += candidate.spawnSources;
                if (!family.actor) family.actor = candidate.actor;
            }
            continue;
        }
        bySignature.emplace(candidate.signature, families.size());
        families.push_back(candidate);
    }
    families.erase(std::remove_if(families.begin(), families.end(), [&](EnemyFamilyPreloadSet& family)
    {
        family.estimatedGeometryBytes = 0;
        family.costKnown = true;
        for (int32_t texture : family.textures)
        {
            uint64_t bytes = 0;
            bool known = false;
            if (!measure(texture, bytes, known)) return true;
            family.costKnown = family.costKnown && known;
            if (bytes > std::numeric_limits<uint64_t>::max() - family.estimatedGeometryBytes) return true;
            family.estimatedGeometryBytes += bytes;
        }
        return false;
    }), families.end());
    std::sort(families.begin(), families.end(), [](const EnemyFamilyPreloadSet& a, const EnemyFamilyPreloadSet& b)
    {
        if ((a.liveActors > 0) != (b.liveActors > 0)) return a.liveActors > 0;
        if (a.costKnown != b.costKnown) return a.costKnown;
        if (a.costKnown && a.estimatedGeometryBytes != b.estimatedGeometryBytes) return a.estimatedGeometryBytes < b.estimatedGeometryBytes;
        if (a.textures.size() != b.textures.size()) return a.textures.size() < b.textures.size();
        if (a.liveActors != b.liveActors) return a.liveActors > b.liveActors;
        return a.family < b.family;
    });
    uint32_t selectedPoses = 0;
    families.erase(std::remove_if(families.begin(), families.end(), [&](const EnemyFamilyPreloadSet& family)
    {
        if (family.textures.size() > poseLimit - selectedPoses) return true;
        selectedPoses += uint32_t(family.textures.size());
        return false;
    }), families.end());
    return families;
}
}
