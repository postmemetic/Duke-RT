#pragma once

#include <cstdint>
#include <functional>
#include <vector>

class DCoreActor;

namespace nri_scene
{
struct EnemyFamilyPreloadSet
{
    uint32_t family = 0;
    uint64_t signature = 0;
    DCoreActor* actor = nullptr;
    std::vector<int32_t> textures;
    uint32_t liveActors = 1;
    uint64_t estimatedGeometryBytes = 0;
    bool costKnown = false;
};

// A read-only lookup: false means a required mounted pose is absent. Unknown
// cost is allowed before the raw archive exists; no geometry is built here.
using EnemyFamilyPreloadCost = std::function<bool(int32_t, uint64_t&, bool&)>;
std::vector<EnemyFamilyPreloadSet> SelectEnemyFamilyPreloadSets(
    const std::vector<EnemyFamilyPreloadSet>& candidates,
    const EnemyFamilyPreloadCost& measure, uint32_t poseLimit = 256);
}
