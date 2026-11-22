#pragma once
#include "nri_enemy_representation_policy.h"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct NRIEnemyStageOwner
{
    NRIEnemyRepresentationOwner owner;
    uint64_t family = 0;
    bool promotionRequested = false;
};
struct NRIEnemyStageMesh
{
    uint64_t key = 0, geometryBytes = 0, physicalBytes = 0, coldPhysicalBytes = 0;
};
struct NRIEnemyStageFamily
{
    uint64_t signature = 0;
    uint32_t id = 0, poses = 0;
    bool complete = false;
    std::vector<NRIEnemyStageMesh> meshes;
    bool startupPriced = false;
    bool promotionRequested = false;
};
struct NRIEnemyStageBudget
{
    uint64_t usage = 0, budget = 0, reserve = 0, coldPhysical = 0;
    bool completePreloadFits = false;
    uint64_t startupChargedBytes = 0, startupScratchBytes = 0, materialBytesPerPose = 0;
    uint64_t promotionHeadroom = 0;
};
struct NRIEnemyStageStats
{
    uint64_t world = 0, geometryBytes = 0, physicalBytes = 0, chargedBytes = 0;
    uint64_t limitBytes = 0, requiredReclaimBytes = 0, otherColdPhysicalBytes = 0;
    uint64_t capRefusalsTotal = 0, pressureDropsTotal = 0;
    uint32_t owners = 0, families = 0, meshes = 0;
};
struct NRIEnemyStageDecision
{
    uint64_t signature = 0, chargedBytes = 0;
    uint32_t family = 0, owners = 0;
    const char* reason = "incomplete";
    bool reserveLimited = false;
};

// Geometry-only prospective retention. This owner never allocates or frees a
// GPU resource and cannot remove any current/pending/committed ownership pin.
class NRIEnemyFamilyStaging
{
public:
    static constexpr uint32_t MaximumOwners = 512, MaximumFamilies = 8, MaximumPreparedFamilies = 32;
    static constexpr uint32_t MaximumFamilyPoses = 64, MaximumMeshes = 256;
    static constexpr uint64_t MaximumChargedBytes = 512ull * 1024 * 1024;
    void Reset();
    void Reconcile(uint64_t world, const std::vector<NRIEnemyStageOwner>& live);
    void ReleaseOwner(NRIEnemyRepresentationOwner owner);
    void Select(const std::vector<NRIEnemyStageFamily>& families, NRIEnemyStageBudget budget);
    const std::unordered_set<uint64_t>& MeshPins() const { return pins; }
    const NRIEnemyStageStats& Stats() const { return stats; }
    const std::vector<NRIEnemyStageDecision>& Decisions() const { return decisions; }
    bool NeedsFamily(uint64_t signature) const;
    bool IsSelected(uint64_t signature) const;
    bool NeedsPromotion(uint64_t signature) const;
    bool AllowsPromotionPair(uint64_t pair) const;
    uint64_t PromotionPair(uint64_t signature) const;
    void PermitPromotion(uint64_t signature, uint64_t pair);
    void StopPromotions() { promotionPairs.clear(); }
    bool RequestSpawnPose(uint64_t signature, uint64_t pairKey);
    bool RequestedSpawnPose(uint64_t signature, uint64_t pairKey) const;
    bool NeedsSpawnPose(uint64_t pairKey) const { return liveSpawnPoses.count(pairKey) != 0; }
    bool WasStaged(uint64_t signature) const { return previouslyStaged.count(signature) != 0; }
    bool WasPriced(uint64_t signature) const { return previouslyPriced.count(signature) != 0; }
    uint32_t OwnerCount(uint64_t signature) const;
private:
    struct Set { NRIEnemyStageFamily family; };
    void Rebuild();
    std::vector<NRIEnemyStageOwner> owners;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> spawnPosesByFamily;
    std::unordered_set<uint64_t> liveSpawnPoses;
    std::unordered_map<uint64_t, uint64_t> promotionPairs;
    std::vector<Set> selected;
    std::unordered_set<uint64_t> pins;
    std::unordered_set<uint64_t> pressureRejected;
    std::unordered_set<uint64_t> previouslyStaged;
    std::unordered_set<uint64_t> previouslyPriced;
    std::vector<NRIEnemyStageDecision> decisions;
    NRIEnemyStageStats stats;
};
