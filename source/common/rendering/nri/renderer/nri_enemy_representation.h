#pragma once

#include "nri_enemy_representation_policy.h"
#include <cstdint>
#include <vector>

namespace nri_scene { struct ActorSpriteFallbackCandidate; }

struct NRIEnemyFamilyResidency
{
    uint32_t family = 0, poses = 0, ready = 0;
    uint64_t signature = 0, logicalGeometryBytes = 0, physicalBytes = 0;
    uint64_t firstUnavailableMesh = 0;
    int32_t firstUnavailableTexture = -1;
    bool complete = false;
    const char* reason = "unknown-family";
    std::vector<uint64_t> meshes;
};

struct NRIEnemyRepresentationChoice
{
    bool managed = false;
    bool sprite = false;
};
