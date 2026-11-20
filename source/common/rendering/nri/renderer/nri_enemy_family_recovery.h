#pragma once
#include <cstdint>
#include <unordered_set>
#include <vector>

// A prospective family can keep existing geometry and request one missing pose.
// Actual storage, native budget checks and submitted work belong to admission.
struct NRIEnemyFamilyRecovery
{
    struct Pose
    {
        uint64_t mesh, pair, sourceBytes, workspaceBytes;
        uint32_t primitives;
    };
    uint64_t signature = 0, world = 0, build = 0, permittedPair = 0;
    uint32_t family = 0, poseCount = 0, lastProgressFrame = 0, ready = 0;
    std::vector<Pose> poses;
    std::unordered_set<uint64_t> pins;
};
