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

void NRIEnemyRepresentationPolicy::Release(NRIEnemyRepresentationCommitment& commitment)
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

void NRIEnemyRepresentationPolicy::Reconcile(uint64_t world, const std::vector<NRIEnemyRepresentationOwner>& live)
{
    std::unordered_set<NRIEnemyRepresentationOwner, Hash> authority(live.begin(), live.end());
    for (auto it = owners.begin(); it != owners.end();)
    {
        if (it->first.world != world || !authority.count(it->first))
        { Release(it->second); it = owners.erase(it); }
        else ++it;
    }
}

void NRIEnemyRepresentationPolicy::Reset()
{
    for (auto& entry : owners) Release(entry.second);
    owners.clear();
}

const NRIEnemyRepresentationCommitment* NRIEnemyRepresentationPolicy::Find(NRIEnemyRepresentationOwner owner) const
{
    const auto found = owners.find(owner);
    return found == owners.end() ? nullptr : &found->second;
}

const NRIEnemyRepresentationCommitment& NRIEnemyRepresentationPolicy::Commit(NRIEnemyRepresentationOwner owner,
    uint64_t identity, uint32_t family, uint64_t signature, uint32_t frame, bool voxel,
    const char* reason, std::vector<uint64_t> meshes, std::vector<uint64_t> materials, bool awaitingSprite)
{
    auto result = owners.try_emplace(owner);
    if (!result.second) return result.first->second; // No opportunistic promotion.
    auto& commitment = result.first->second;
    commitment.mode = voxel ? NRIEnemyRepresentationMode::Voxel : NRIEnemyRepresentationMode::Sprite;
    commitment.identity = identity; commitment.family = family; commitment.familySignature = signature;
    commitment.sinceFrame = frame; commitment.reason = reason;
    commitment.awaitingSprite = !voxel && awaitingSprite;
    commitment.emergency = commitment.awaitingSprite;
    if (voxel || commitment.awaitingSprite)
    {
        Unique(meshes); Unique(materials);
        commitment.meshes = std::move(meshes); commitment.materials = std::move(materials);
        for (uint64_t key : commitment.meshes) ++meshPins[key];
        for (uint64_t key : commitment.materials) ++materialPins[key];
    }
    return commitment;
}

bool NRIEnemyRepresentationPolicy::ResolveSpriteContinuity(NRIEnemyRepresentationOwner owner)
{
    auto found = owners.find(owner);
    if (found == owners.end() || !found->second.awaitingSprite) return false;
    Release(found->second);
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
    found->second.emergency = true;
    found->second.reason = reason;
    return true;
}

std::vector<uint64_t> NRIEnemyRepresentationPolicy::TakeReleasedMaterialKeys()
{
    std::vector<uint64_t> result;
    result.swap(releasedMaterials);
    return result;
}
