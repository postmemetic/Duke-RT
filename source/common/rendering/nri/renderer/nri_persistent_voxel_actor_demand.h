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

// CPU policy only. No source pointers, GPU resources or ownership pins.
class NRIVoxelActorDemand
{
public:
    void Reset();
    void Refresh(uint64_t generation, uint32_t frame, const std::vector<NRIVoxelActorDemandInput>& inputs);
    void InvalidateAuthority() { valid = false; snapshot.authorityAvailable = false; }
    void ObserveRepresentation(uint64_t identity, uint64_t world, uint64_t lifetime,
        NRIVoxelActorRepresentation representation, uint64_t selectedMesh, uint64_t selectedMaterial, uint32_t frame);
    bool CanHold(uint64_t identity, uint64_t world, uint64_t lifetime, uint32_t frame, uint32_t maximumFrames) const;
    NRIVoxelPairDemand Pair(uint64_t key) const;
    NRIVoxelActorDemandState GetActor(uint64_t identity, uint64_t world, uint64_t lifetime, uint32_t frame) const;
    uint32_t WarmMeshRank(uint64_t key, uint32_t frame) const;
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
    struct Warm { uint32_t frame = 0, uses = 0, currentActorFrame = UINT32_MAX; };
    void NoteSelected(Actor& actor, uint64_t mesh, uint64_t material, uint32_t frame);
    void RebuildPairs(uint32_t frame);
    void TrimHistory(std::unordered_map<uint64_t, Warm>& history, uint32_t frame);
    uint32_t WarmRank(const std::unordered_map<uint64_t, Warm>& history, uint64_t key, uint32_t frame) const;
    uint64_t generation = 0;
    bool valid = false;
    std::unordered_map<uint64_t, Actor> actors;
    std::unordered_map<uint64_t, NRIVoxelPairDemand> pairs;
    std::unordered_map<uint64_t, Warm> meshes, materials;
    NRIVoxelActorDemandSnapshot snapshot;
};

struct NRIVoxelAdmissionPolicySettings
{
    bool actorPriority = false, coalesce = false, warmRetention = false;
    uint32_t heldMaximumFrames = 0;
};
NRIVoxelAdmissionPolicySettings GetNRIVoxelAdmissionPolicySettings();
