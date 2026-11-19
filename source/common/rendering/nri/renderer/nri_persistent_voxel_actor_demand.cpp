#include "nri_persistent_voxel_actor_demand.h"
#include <algorithm>
#include <utility>

namespace
{
constexpr uint32_t HistoryFrames = 600;
constexpr size_t HistoryRecords = 8192;
uint32_t Age(uint32_t frame, uint32_t since) { return since != UINT32_MAX && frame >= since ? frame - since : 0; }
}

void NRIVoxelActorDemand::Reset()
{
    generation = 0;
    valid = false;
    actors.clear(); pairs.clear(); upgrades.clear(); meshes.clear(); materials.clear(); snapshot = {};
    refreshedFrame = UINT32_MAX;
    captureUpgrades = false; upgradeRevision = upgradeSignature = 0;
}

void NRIVoxelActorDemand::NoteSelected(Actor& actor, uint64_t mesh, uint64_t material, uint32_t frame)
{
    if (mesh && material && (actor.recent[0].mesh != mesh || actor.recent[0].material != material))
    {
        for (size_t i = actor.recent.size() - 1; i; --i) actor.recent[i] = actor.recent[i - 1];
        actor.recent[0] = { mesh, material };
    }
    const auto touch = [frame](Warm& item)
    {
        if (item.uses == 0 || item.frame != frame) item.uses = std::min(65535u, item.uses + 1u);
        item.frame = frame;
    };
    if (mesh && material) { touch(meshes[mesh]); touch(materials[material]); }
    for (const auto& pose : actor.recent)
    {
        auto m = meshes.find(pose.mesh);
        if (m != meshes.end()) m->second.currentActorFrame = frame;
        auto p = materials.find(pose.material);
        if (p != materials.end()) p->second.currentActorFrame = frame;
    }
}

void NRIVoxelActorDemand::TrimHistory(std::unordered_map<uint64_t, Warm>& history, uint32_t frame)
{
    for (auto it = history.begin(); it != history.end();)
        if (Age(frame, it->second.frame) > HistoryFrames) it = history.erase(it); else ++it;
    if (history.size() <= HistoryRecords) return;
    std::vector<std::pair<uint32_t, uint64_t>> oldest;
    oldest.reserve(history.size());
    for (const auto& entry : history) oldest.emplace_back(entry.second.frame, entry.first);
    std::sort(oldest.begin(), oldest.end());
    const size_t remove = history.size() - HistoryRecords;
    for (size_t i = 0; i < remove; ++i) history.erase(oldest[i].second);
}

void NRIVoxelActorDemand::Refresh(uint64_t nextGeneration, uint32_t frame, const std::vector<NRIVoxelActorDemandInput>& inputs, bool nextCaptureUpgrades)
{
    if (generation != nextGeneration) { Reset(); generation = nextGeneration; }
    valid = true;
    captureUpgrades = nextCaptureUpgrades;
    if (captureUpgrades)
    {
        uint64_t signature = inputs.size();
        const auto mix = [](uint64_t a, uint64_t b) { return (a ^ b) * UINT64_C(1099511628211); };
        for (const auto& input : inputs)
        {
            uint64_t row = mix(input.identity, input.world);
            row = mix(row, input.lifetime); row = mix(row, input.mesh); row = mix(row, input.material);
            row = mix(row, input.selectedMesh); row = mix(row, input.selectedMaterial);
            signature ^= mix(row, uint64_t(input.visible) | (uint64_t(input.representation) << 1));
        }
        if (signature != upgradeSignature) { upgradeSignature = signature; ++upgradeRevision; }
    }
    for (const auto& input : inputs)
    {
        if (!input.identity || !input.world || !input.pair) continue;
        Actor& actor = actors[input.identity];
        if (actor.input.world != input.world || actor.input.lifetime != input.lifetime) actor = {};
        actor.input = input;
        actor.seen = frame;
        if (input.representation == NRIVoxelActorRepresentation::Exact) actor.exactMissingSince = UINT32_MAX;
        else if (actor.exactMissingSince == UINT32_MAX) actor.exactMissingSince = frame;
        NoteSelected(actor, input.selectedMesh, input.selectedMaterial, frame);
    }
    for (auto it = actors.begin(); it != actors.end();)
        if (it->second.seen != frame) it = actors.erase(it); else ++it;
    TrimHistory(meshes, frame); TrimHistory(materials, frame);
    RebuildPairs(frame);
}

void NRIVoxelActorDemand::ObserveRepresentation(uint64_t identity, uint64_t world, uint64_t lifetime,
    NRIVoxelActorRepresentation representation, uint64_t selectedMesh, uint64_t selectedMaterial, uint32_t frame)
{
    auto found = actors.find(identity);
    if (!valid || found == actors.end() || found->second.input.world != world || found->second.input.lifetime != lifetime) return;
    auto& actor = found->second;
    actor.input.representation = representation;
    actor.input.selectedMesh = selectedMesh;
    actor.input.selectedMaterial = selectedMaterial;
    if (representation == NRIVoxelActorRepresentation::Exact) actor.exactMissingSince = UINT32_MAX;
    else if (actor.exactMissingSince == UINT32_MAX) actor.exactMissingSince = frame;
    NoteSelected(actor, selectedMesh, selectedMaterial, frame);
    // Pair aggregation is rebuilt once by the next pump, not once per actor.
}

void NRIVoxelActorDemand::RebuildPairs(uint32_t frame)
{
    pairs.clear(); upgrades.clear(); refreshedFrame = frame;
    snapshot = {}; snapshot.authorityAvailable = valid;
    for (const auto& entry : actors)
    {
        const Actor& actor = entry.second;
        const auto representation = actor.input.representation;
        const uint32_t age = Age(frame, actor.exactMissingSince);
        if (captureUpgrades) ++upgrades[actor.input.mesh].requestedConsumers;
        if (captureUpgrades && actor.input.visible && representation != NRIVoxelActorRepresentation::Exact)
        {
            auto& upgrade = upgrades[actor.input.mesh];
            if (!upgrade.consumers || age > upgrade.oldestWait ||
                (age == upgrade.oldestWait && actor.input.identity < upgrade.identity))
            {
                upgrade.identity = actor.input.identity; upgrade.world = actor.input.world;
                upgrade.lifetime = actor.input.lifetime; upgrade.oldestWait = age;
            }
            ++upgrade.consumers;
        }
        auto& demand = pairs[actor.input.pair];
        demand.consumers++;
        demand.visibleConsumers += actor.input.visible;
        demand.oldestWait = std::max(demand.oldestWait, age);
        uint32_t tier = !actor.input.visible ? 2u :
            (representation == NRIVoxelActorRepresentation::HeldVoxel || representation == NRIVoxelActorRepresentation::Exact ? 1u : 0u);
        // A long-held/offscreen required upgrade eventually competes with new
        // foreground work; pair/pose changes cannot erase this actor's age.
        tier = age >= 240 ? 0u : (age >= 120 ? std::min(tier, 1u) : tier);
        demand.priorityTier = std::min(demand.priorityTier, tier);
        snapshot.oldestExactWait = std::max(snapshot.oldestExactWait, age);
        switch (representation)
        {
        case NRIVoxelActorRepresentation::Missing: snapshot.missing++; break;
        case NRIVoxelActorRepresentation::SpriteFallback: snapshot.sprite++; break;
        case NRIVoxelActorRepresentation::HeldVoxel: snapshot.held++; break;
        case NRIVoxelActorRepresentation::Exact: snapshot.exact++; break;
        }
    }
    snapshot.actors = uint32_t(actors.size()); snapshot.pairs = uint32_t(pairs.size());
    snapshot.warmMeshes = uint32_t(meshes.size()); snapshot.warmMaterials = uint32_t(materials.size());
}

bool NRIVoxelActorDemand::CanHold(uint64_t identity, uint64_t world, uint64_t lifetime, uint32_t frame, uint32_t maximumFrames) const
{
    if (!maximumFrames) return true;
    const auto found = actors.find(identity);
    if (!valid || found == actors.end() || found->second.input.world != world || found->second.input.lifetime != lifetime) return false;
    return Age(frame, found->second.exactMissingSince) < maximumFrames;
}

NRIVoxelPairDemand NRIVoxelActorDemand::Pair(uint64_t key) const
{
    if (!valid) return {};
    const auto found = pairs.find(key);
    return found != pairs.end() ? found->second : NRIVoxelPairDemand{};
}

NRIVoxelMeshUpgradeDemand NRIVoxelActorDemand::Upgrade(uint64_t mesh, uint32_t frame) const
{
    if (!valid || frame != refreshedFrame) return {};
    const auto found = upgrades.find(mesh);
    return found == upgrades.end() ? NRIVoxelMeshUpgradeDemand{} : found->second;
}

NRIVoxelActorDemandState NRIVoxelActorDemand::GetActor(uint64_t identity, uint64_t world, uint64_t lifetime, uint32_t frame) const
{
    const auto found = actors.find(identity);
    if (!valid || found == actors.end() || found->second.input.world != world || found->second.input.lifetime != lifetime) return {};
    const auto& actor = found->second;
    return { true, actor.input.mesh, actor.input.material, actor.input.selectedMesh, actor.input.selectedMaterial,
        actor.input.representation, Age(frame, actor.exactMissingSince) };
}

bool NRIVoxelActorDemand::HigherPriority(const NRIVoxelPairDemand& a, const NRIVoxelPairDemand& b)
{
    if (a.priorityTier != b.priorityTier) return a.priorityTier < b.priorityTier;
    if (a.oldestWait != b.oldestWait) return a.oldestWait > b.oldestWait;
    return a.visibleConsumers > b.visibleConsumers;
}

uint32_t NRIVoxelActorDemand::WarmRank(const std::unordered_map<uint64_t, Warm>& history, uint64_t key, uint32_t frame) const
{
    const auto found = history.find(key);
    if (!valid || found == history.end() || Age(frame, found->second.frame) > HistoryFrames) return 0;
    if (found->second.currentActorFrame == frame) return 3;
    return found->second.uses > 1 ? 2u : 1u;
}
uint32_t NRIVoxelActorDemand::WarmMeshRank(uint64_t key, uint32_t frame) const { return WarmRank(meshes, key, frame); }
uint32_t NRIVoxelActorDemand::WarmMaterialRank(uint64_t key, uint32_t frame) const { return WarmRank(materials, key, frame); }
