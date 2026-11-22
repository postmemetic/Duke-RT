#include "nri_enemy_representation_policy.h"
#include <algorithm>
#include <utility>

bool NRIEnemyFamilyContainsCurrentMesh(const std::vector<uint64_t>& meshes, uint64_t currentMesh)
{
    return currentMesh != 0 && std::find(meshes.begin(), meshes.end(), currentMesh) != meshes.end();
}

namespace
{
void Unique(std::vector<uint64_t>& keys)
{
    std::sort(keys.begin(), keys.end());
    keys.erase(std::remove(keys.begin(), keys.end(), 0), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
}
}

void NRIEnemyRepresentationPolicy::Pin(NRIEnemyRepresentationCommitment& commitment,
    std::vector<uint64_t> meshes, std::vector<uint64_t> materials)
{
    Unique(meshes); Unique(materials);
    commitment.meshes = std::move(meshes); commitment.materials = std::move(materials);
    for (uint64_t key : commitment.meshes) ++meshPins[key];
    for (uint64_t key : commitment.materials) ++materialPins[key];
}

void NRIEnemyRepresentationPolicy::ReleasePins(NRIEnemyRepresentationCommitment& commitment)
{
    for (uint64_t key : commitment.meshes)
    {
        auto found = meshPins.find(key);
        if (found != meshPins.end() && --found->second == 0) meshPins.erase(found);
    }
    for (uint64_t key : commitment.materials)
    {
        auto found = materialPins.find(key);
        if (found != materialPins.end() && --found->second == 0)
        { materialPins.erase(found); releasedMaterials.push_back(key); }
    }
    commitment.meshes.clear(); commitment.materials.clear();
}

void NRIEnemyRepresentationPolicy::Release(NRIEnemyRepresentationCommitment& commitment)
{
    ReleasePins(commitment);
    releasedMaterials.insert(releasedMaterials.end(), commitment.prospectiveMaterials.begin(), commitment.prospectiveMaterials.end());
    commitment.prospectiveMaterials.clear();
}

void NRIEnemyRepresentationPolicy::Reconcile(uint64_t world, const std::vector<NRIEnemyRepresentationOwner>& live)
{
    if (counters.world != world) { Reset(); counters.world = world; }
    std::unordered_set<NRIEnemyRepresentationOwner, Hash> authority(live.begin(), live.end());
    for (auto it = owners.begin(); it != owners.end();)
    {
        if (it->first.world != world || !authority.count(it->first))
        {
            if (it->second.mode == NRIEnemyRepresentationMode::Voxel) --counters.liveVoxel;
            else --counters.liveSprite;
            Release(it->second); it = owners.erase(it);
        }
        else ++it;
    }
}

void NRIEnemyRepresentationPolicy::Reset()
{
    for (auto& entry : owners) Release(entry.second);
    owners.clear();
    // Repeated disabled-frame resets do not manufacture new census epochs.
    const uint64_t epoch = counters.epoch + (counters.world != 0 || counters.firstSprite != 0 || counters.firstVoxel != 0);
    counters = {}; counters.epoch = epoch;
}

const NRIEnemyRepresentationCommitment* NRIEnemyRepresentationPolicy::Find(NRIEnemyRepresentationOwner owner) const
{
    const auto found = owners.find(owner);
    return found == owners.end() ? nullptr : &found->second;
}

const NRIEnemyRepresentationCommitment& NRIEnemyRepresentationPolicy::Commit(NRIEnemyRepresentationOwner owner,
    uint64_t identity, uint32_t family, uint64_t signature, uint32_t frame, bool voxel,
    const char* reason, std::vector<uint64_t> meshes, std::vector<uint64_t> materials, bool awaitingSprite, bool supported,
    uint64_t successorSignature, std::vector<uint64_t> prospectiveMaterials)
{
    if (counters.world != owner.world) { Reset(); counters.world = owner.world; }
    auto result = owners.try_emplace(owner);
    if (!result.second) return result.first->second;
    auto& commitment = result.first->second;
    commitment.mode = voxel ? NRIEnemyRepresentationMode::Voxel : NRIEnemyRepresentationMode::Sprite;
    commitment.identity = identity; commitment.family = family; commitment.familySignature = signature;
    commitment.successorSignature = successorSignature;
    commitment.sinceFrame = frame; commitment.reason = reason;
    commitment.awaitingSprite = !voxel && awaitingSprite;
    commitment.emergency = commitment.awaitingSprite;
    commitment.supported = supported;
    if (voxel) { ++counters.firstVoxel; ++counters.liveVoxel; counters.supportedFirstVoxel += supported; }
    else { ++counters.firstSprite; ++counters.liveSprite; counters.supportedFirstSprite += supported; }
    counters.emergencies += commitment.emergency;
    if (voxel || commitment.awaitingSprite)
        Pin(commitment, std::move(meshes), std::move(materials));
    if (commitment.MayPromote())
        commitment.prospectiveMaterials = std::move(prospectiveMaterials); // Retirement keys only. No residency pins.
    return commitment;
}

bool NRIEnemyRepresentationPolicy::Promote(NRIEnemyRepresentationOwner owner, uint32_t frame,
    uint32_t family, uint64_t signature, uint64_t successor, std::vector<uint64_t> meshes, std::vector<uint64_t> materials)
{
    auto found = owners.find(owner);
    if (found == owners.end() || !found->second.MayPromote() || !signature ||
        (signature != found->second.familySignature && signature != found->second.successorSignature) ||
        meshes.empty() || materials.empty()) return false;
    auto& commitment = found->second;
    if (signature == commitment.familySignature) commitment.prospectiveMaterials.clear();
    Release(commitment);
    Pin(commitment, std::move(meshes), std::move(materials));
    commitment.family = family;
    commitment.familySignature = signature;
    commitment.successorSignature = successor;
    commitment.awaitingSprite = false;
    commitment.mode = NRIEnemyRepresentationMode::Voxel;
    commitment.sinceFrame = frame;
    commitment.reason = "prepared-after-first-sight";
    --counters.liveSprite; ++counters.liveVoxel; ++counters.promotions;
    return true;
}

bool NRIEnemyRepresentationPolicy::ResolveSpriteContinuity(NRIEnemyRepresentationOwner owner)
{
    auto found = owners.find(owner);
    if (found == owners.end() || !found->second.awaitingSprite) return false;
    // Resolving the temporary body releases only its pins. Preparation can
    // still acquire rows for this owner until it promotes or leaves the world.
    ReleasePins(found->second);
    found->second.awaitingSprite = false;
    found->second.reason = "initial-fallback-failure-resolved";
    return true;
}

bool NRIEnemyRepresentationPolicy::Downgrade(NRIEnemyRepresentationOwner owner, const char* reason)
{
    auto found = owners.find(owner);
    if (found == owners.end() || found->second.mode != NRIEnemyRepresentationMode::Voxel) return false;
    Release(found->second);
    found->second.mode = NRIEnemyRepresentationMode::Sprite;
    --counters.liveVoxel; ++counters.liveSprite; ++counters.emergencies;
    found->second.emergency = true;
    found->second.downgraded = true;
    found->second.reason = reason;
    return true;
}

std::vector<uint64_t> NRIEnemyRepresentationPolicy::TakeReleasedMaterialKeys()
{
    std::vector<uint64_t> result;
    result.swap(releasedMaterials);
    return result;
}
