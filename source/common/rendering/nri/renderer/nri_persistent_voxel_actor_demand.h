#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

enum class NRIVoxelActorRepresentation : uint8_t { Missing, SpriteFallback, HeldVoxel, Exact };

struct NRIVoxelActorDemandInput
{
    uint64_t identity = 0, world = 0, lifetime = 0;
    uint64_t pair = 0, mesh = 0, material = 0;
    uint64_t selectedMesh = 0, selectedMaterial = 0;
    bool visible = false;
    NRIVoxelActorRepresentation representation = NRIVoxelActorRepresentation::Missing;
    bool rayEligible = false;
};

struct NRIVoxelPairDemand
{
    uint32_t consumers = 0, visibleConsumers = 0, oldestWait = 0;
    uint32_t priorityTier = 3; // missing/sprite, held, retained, speculative
};

struct NRIVoxelActorDemandSnapshot
{
    uint32_t actors = 0, pairs = 0, missing = 0, sprite = 0, held = 0, exact = 0;
    uint32_t oldestExactWait = 0, warmMeshes = 0, warmMaterials = 0;
    bool authorityAvailable = false;
};

struct NRIVoxelActorDemandState
{
    bool found = false;
    uint64_t desiredMesh = 0, desiredMaterial = 0, selectedMesh = 0, selectedMaterial = 0;
    NRIVoxelActorRepresentation representation = NRIVoxelActorRepresentation::Missing;
    uint32_t exactMissingAge = 0;
};

struct NRIVoxelMeshUpgradeDemand
{
    uint64_t identity = 0, world = 0, lifetime = 0;
    uint32_t consumers = 0, oldestWait = 0, requestedConsumers = 0;
};

// CPU policy only. No source pointers, GPU resources or ownership pins.
class NRIVoxelActorDemand
{
public:
    void Reset();
    void Refresh(uint64_t generation, uint32_t frame, const std::vector<NRIVoxelActorDemandInput>& inputs,
        bool captureUpgrades = false, bool capturePoseRequests = false);
    void InvalidateAuthority() { valid = false; snapshot.authorityAvailable = false; }
    bool HasCurrentHoldHeadroom(uint32_t frame) const { return valid && holdHeadroomAvailable && refreshedFrame == frame; }
    void SetHoldHeadroom(bool available) { holdHeadroomAvailable = available; }
    void ObserveRepresentation(uint64_t identity, uint64_t world, uint64_t lifetime,
        NRIVoxelActorRepresentation representation, uint64_t selectedMesh, uint64_t selectedMaterial, uint32_t frame);
    bool CanHold(uint64_t identity, uint64_t world, uint64_t lifetime, uint32_t frame, uint32_t maximumFrames) const;
    NRIVoxelPairDemand Pair(uint64_t key) const;
    NRIVoxelMeshUpgradeDemand Upgrade(uint64_t mesh, uint32_t frame) const;
    bool HasCurrentMeshAuthority(uint32_t frame) const { return valid && captureUpgrades && refreshedFrame == frame; }
    uint64_t UpgradeRevision() const { return upgradeRevision; }
    NRIVoxelActorDemandState GetActor(uint64_t identity, uint64_t world, uint64_t lifetime, uint32_t frame) const;
    uint32_t WarmMeshRank(uint64_t key, uint32_t frame) const;
    uint32_t RepeatedMeshSelectionFrame(uint64_t key, uint32_t frame) const;
    std::unordered_map<uint64_t, uint32_t> RecentRecurringPoseRequests(uint32_t frame, uint32_t interval) const;
    uint32_t WarmMaterialRank(uint64_t key, uint32_t frame) const;
    const NRIVoxelActorDemandSnapshot& Snapshot() const { return snapshot; }
    bool AuthorityAvailable() const { return valid; }
    static bool HigherPriority(const NRIVoxelPairDemand& a, const NRIVoxelPairDemand& b);

private:
    struct Pose { uint64_t mesh = 0, material = 0; };
    struct Actor
    {
        NRIVoxelActorDemandInput input;
        uint32_t exactMissingSince = UINT32_MAX, seen = 0;
        std::array<Pose, 4> recent = {};
    };
    struct Warm { uint32_t frame = UINT32_MAX, previousFrame = UINT32_MAX, currentActorFrame = UINT32_MAX; };
    struct MeshRequest { uint64_t mesh = 0; uint32_t frame = 0; };
    std::unordered_map<uint64_t, std::array<MeshRequest, 8>> poseRequests;
    void NotePoseRequest(const NRIVoxelActorDemandInput& input, uint32_t frame);
    void NoteSelected(Actor& actor, uint64_t mesh, uint64_t material, uint32_t frame);
    void RebuildPairs(uint32_t frame);
    void TrimHistory(std::unordered_map<uint64_t, Warm>& history, uint32_t frame);
    uint32_t WarmRank(const std::unordered_map<uint64_t, Warm>& history, uint64_t key, uint32_t frame) const;
    uint64_t generation = 0;
    bool valid = false;
    std::unordered_map<uint64_t, Actor> actors;
    std::unordered_map<uint64_t, NRIVoxelPairDemand> pairs;
    std::unordered_map<uint64_t, NRIVoxelMeshUpgradeDemand> upgrades;
    uint32_t refreshedFrame = UINT32_MAX;
    bool holdHeadroomAvailable = false;
    bool captureUpgrades = false, capturePoseRequests = false;
    uint64_t upgradeRevision = 0, upgradeSignature = 0;
    std::unordered_map<uint64_t, Warm> meshes, materials;
    NRIVoxelActorDemandSnapshot snapshot;
};

struct NRIVoxelAdmissionPolicySettings
{
    bool actorPriority = false, coalesce = false, warmRetention = false;
    uint32_t heldMaximumFrames = 0;
};
NRIVoxelAdmissionPolicySettings GetNRIVoxelAdmissionPolicySettings();
