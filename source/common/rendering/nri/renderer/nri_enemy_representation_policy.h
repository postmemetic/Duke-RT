#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class NRIEnemyRepresentationMode : uint8_t { Uncommitted, Sprite, Voxel };

bool NRIEnemyFamilyContainsCurrentMesh(const std::vector<uint64_t>& meshes, uint64_t currentMesh);

struct NRIEnemyRepresentationOwner
{
    uint64_t world = 0, lifetime = 0;
    bool operator==(const NRIEnemyRepresentationOwner& other) const
    { return world == other.world && lifetime == other.lifetime; }
};

struct NRIEnemyRepresentationCommitment
{
    NRIEnemyRepresentationMode mode = NRIEnemyRepresentationMode::Uncommitted;
    uint64_t familySignature = 0, successorSignature = 0, identity = 0;
    uint32_t family = 0, sinceFrame = 0;
    bool emergency = false;
    bool awaitingSprite = false;
    bool supported = false;
    bool downgraded = false;
    bool MayPromote() const { return mode == NRIEnemyRepresentationMode::Sprite && supported && !downgraded; }
    const char* reason = "uncommitted";
    std::vector<uint64_t> meshes, materials;
    std::vector<uint64_t> prospectiveMaterials;
};

struct NRIEnemyRepresentationCounters
{
    uint64_t world = 0, epoch = 0;
    uint64_t firstSprite = 0, firstVoxel = 0, supportedFirstSprite = 0, supportedFirstVoxel = 0, emergencies = 0, promotions = 0;
    uint32_t liveSprite = 0, liveVoxel = 0;
};

// Pure ownership/choice policy. Residency supplies complete actor authority and
// real GPU readiness; no source pointers or future memory credits are retained.
class NRIEnemyRepresentationPolicy
{
public:
    void Reconcile(uint64_t world, const std::vector<NRIEnemyRepresentationOwner>& live);
    void Reset();
    const NRIEnemyRepresentationCommitment* Find(NRIEnemyRepresentationOwner owner) const;
    const NRIEnemyRepresentationCommitment& Commit(NRIEnemyRepresentationOwner owner,
        uint64_t identity, uint32_t family, uint64_t signature, uint32_t frame,
        bool voxel, const char* reason, std::vector<uint64_t> meshes = {}, std::vector<uint64_t> materials = {},
        bool awaitingSprite = false, bool supported = false, uint64_t successorSignature = 0,
        std::vector<uint64_t> prospectiveMaterials = {});
    bool Promote(NRIEnemyRepresentationOwner owner, uint32_t frame, uint32_t family, uint64_t signature, uint64_t successor,
        std::vector<uint64_t> meshes, std::vector<uint64_t> materials);
    bool Downgrade(NRIEnemyRepresentationOwner owner, const char* reason);
    bool ResolveSpriteContinuity(NRIEnemyRepresentationOwner owner);
    const std::unordered_map<uint64_t, uint32_t>& MeshPins() const { return meshPins; }
    const std::unordered_map<uint64_t, uint32_t>& MaterialPins() const { return materialPins; }
    std::vector<uint64_t> TakeReleasedMaterialKeys();
    size_t OwnerCount() const { return owners.size(); }
    const NRIEnemyRepresentationCounters& Counters() const { return counters; }
private:
    struct Hash
    {
        size_t operator()(const NRIEnemyRepresentationOwner& owner) const
        { return size_t(owner.world ^ (owner.lifetime + UINT64_C(0x9e3779b97f4a7c15) + (owner.world << 6) + (owner.world >> 2))); }
    };
    void Pin(NRIEnemyRepresentationCommitment& commitment, std::vector<uint64_t> meshes, std::vector<uint64_t> materials);
    void ReleasePins(NRIEnemyRepresentationCommitment& commitment);
    void Release(NRIEnemyRepresentationCommitment& commitment);
    std::unordered_map<NRIEnemyRepresentationOwner, NRIEnemyRepresentationCommitment, Hash> owners;
    std::unordered_map<uint64_t, uint32_t> meshPins, materialPins;
    std::vector<uint64_t> releasedMaterials;
    NRIEnemyRepresentationCounters counters;
};
