#include "nri_enemy_family_staging.h"
#include <algorithm>
#include <limits>

namespace
{
uint64_t Add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
uint64_t Cost(const NRIEnemyStageFamily& family)
{
    uint64_t result = 0;
    std::unordered_set<uint64_t> seen;
    for (const auto& mesh : family.meshes) if (seen.insert(mesh.key).second)
        result = Add(result, Add(mesh.geometryBytes, mesh.physicalBytes));
    return result;
}
}

void NRIEnemyFamilyStaging::Reset()
{
    spawnPosesByFamily.clear(); liveSpawnPoses.clear();
    owners.clear(); selected.clear(); pins.clear(); previouslyStaged.clear(); pressureRejected.clear(); decisions.clear(); stats = {};
}

bool NRIEnemyFamilyStaging::NeedsFamily(uint64_t signature) const
{
    return std::any_of(owners.begin(), owners.end(), [=](const auto& item) { return item.family == signature; });
}

bool NRIEnemyFamilyStaging::RequestSpawnPose(uint64_t signature, uint64_t pairKey)
{
    if (!pairKey || !NeedsFamily(signature)) return false;
    if (!spawnPosesByFamily[signature].insert(pairKey).second) return false;
    liveSpawnPoses.insert(pairKey);
    return true;
}

bool NRIEnemyFamilyStaging::RequestedSpawnPose(uint64_t signature, uint64_t pairKey) const
{
    const auto family = spawnPosesByFamily.find(signature);
    return family != spawnPosesByFamily.end() && family->second.count(pairKey) != 0;
}

void NRIEnemyFamilyStaging::Rebuild()
{
    bool lostFamily = false;
    for (auto it = spawnPosesByFamily.begin(); it != spawnPosesByFamily.end();)
    {
        if (NeedsFamily(it->first)) { ++it; continue; }
        it = spawnPosesByFamily.erase(it);
        lostFamily = true;
    }
    if (lostFamily)
    {
        // Shared pairs remain wanted until the last family that requested
        // them has no uncommitted owner.
        liveSpawnPoses.clear();
        for (const auto& family : spawnPosesByFamily)
            liveSpawnPoses.insert(family.second.begin(), family.second.end());
    }

    selected.erase(std::remove_if(selected.begin(), selected.end(),
        [&](const Set& item) { return !NeedsFamily(item.family.signature); }), selected.end());
    pins.clear(); stats.geometryBytes = stats.physicalBytes = 0;
    for (const auto& item : selected) for (const auto& mesh : item.family.meshes)
        if (pins.insert(mesh.key).second)
        { stats.geometryBytes = Add(stats.geometryBytes, mesh.geometryBytes); stats.physicalBytes = Add(stats.physicalBytes, mesh.physicalBytes); }
    stats.chargedBytes = Add(stats.geometryBytes, stats.physicalBytes);
    stats.families = uint32_t(selected.size()); stats.meshes = uint32_t(pins.size()); stats.owners = 0;
    for (const auto& owner : owners) if (std::any_of(selected.begin(), selected.end(),
        [&](const Set& item) { return item.family.signature == owner.family; })) ++stats.owners;
}

void NRIEnemyFamilyStaging::Reconcile(uint64_t world, const std::vector<NRIEnemyStageOwner>& live)
{
    if (stats.world != world) { Reset(); stats.world = world; }
    owners.clear(); owners.reserve(std::min<size_t>(live.size(), MaximumOwners));
    std::unordered_set<uint64_t> lifetimes;
    for (const auto& item : live)
    {
        if (!item.owner.world || item.owner.world != world || !item.family) continue;
        if (!lifetimes.insert(item.owner.lifetime).second) continue;
        if (owners.size() == MaximumOwners) { ++stats.capRefusalsTotal; break; }
        owners.push_back(item);
    }
    for (auto it = pressureRejected.begin(); it != pressureRejected.end();)
        if (!NeedsFamily(*it)) it = pressureRejected.erase(it); else ++it;
    for (auto it = previouslyStaged.begin(); it != previouslyStaged.end();)
        if (!NeedsFamily(*it)) it = previouslyStaged.erase(it); else ++it;
    Rebuild();
}

void NRIEnemyFamilyStaging::ReleaseOwner(NRIEnemyRepresentationOwner owner)
{
    owners.erase(std::remove_if(owners.begin(), owners.end(),
        [&](const auto& item) { return item.owner == owner; }), owners.end());
    Rebuild();
}

void NRIEnemyFamilyStaging::Select(const std::vector<NRIEnemyStageFamily>& families, NRIEnemyStageBudget budget)
{
    if (!budget.budget) return; // Unknown budget cannot admit or justify new pressure eviction.
    const uint64_t previousLimit = stats.limitBytes;
    stats.limitBytes = std::min(MaximumChargedBytes, budget.budget / 4);
    const uint64_t available = budget.budget > budget.reserve ? budget.budget - budget.reserve : 0;
    stats.requiredReclaimBytes = budget.usage > available ? budget.usage - available : 0;
    const bool prepareAll = budget.completePreloadFits && budget.usage < available;
    if (prepareAll)
        stats.limitBytes = std::max(stats.limitBytes, Add(stats.chargedBytes, available - budget.usage));
    const uint32_t familyLimit = prepareAll ? MaximumPreparedFamilies : MaximumFamilies;
    const uint32_t meshLimit = prepareAll ? MaximumPreparedFamilies * MaximumFamilyPoses : MaximumMeshes;
    decisions.clear();
    struct Candidate { const NRIEnemyStageFamily* family; uint64_t cost; uint32_t owners; };
    std::vector<Candidate> candidates;
    std::unordered_set<uint64_t> seen;
    for (const auto& family : families)
    {
        if (!family.signature || !NeedsFamily(family.signature) || !seen.insert(family.signature).second) continue;
        const auto count = uint32_t(std::count_if(owners.begin(), owners.end(),
            [&](const auto& owner) { return owner.family == family.signature; }));
        if (family.complete && family.poses && !family.meshes.empty()) candidates.push_back({&family, Cost(family), count});
        else decisions.push_back({family.signature, 0, family.id, count, "incomplete"});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
    {
        if (a.cost != b.cost) return a.cost < b.cost;
        return a.owners != b.owners ? a.owners > b.owners : a.family->signature < b.family->signature;
    });
    std::unordered_set<uint64_t> old;
    for (const auto& item : selected) old.insert(item.family.signature);
    selected.clear(); pins.clear();
    uint64_t charged = 0, protectedCold = 0;
    for (const auto& candidate : candidates)
    {
        const auto* family = candidate.family;
        decisions.push_back({family->signature, candidate.cost, family->id, candidate.owners, "selected"});
        auto& decision = decisions.back();
        const auto rejected = pressureRejected.find(family->signature);
        if (rejected != pressureRejected.end())
        {
            // A noisy budget increase is not recovery. Re-admit only after
            // actual usage has regained the complete reserve and margin.
            if (stats.requiredReclaimBytes) { decision.reason = "pressure-cooldown"; continue; }
            pressureRejected.erase(rejected);
        }
        uint64_t extra = 0, cold = 0; std::unordered_set<uint64_t> keys;
        bool valid = true;
        for (const auto& mesh : family->meshes)
        {
            if (!mesh.key || !mesh.geometryBytes || mesh.coldPhysicalBytes > mesh.physicalBytes) valid = false;
            if (!pins.count(mesh.key) && keys.insert(mesh.key).second)
            { extra = Add(extra, Add(mesh.geometryBytes, mesh.physicalBytes)); cold = Add(cold, mesh.coldPhysicalBytes); }
        }
        if (!valid || family->poses > MaximumFamilyPoses || selected.size() >= familyLimit ||
            keys.size() > meshLimit - std::min<size_t>(meshLimit, pins.size()) || extra > stats.limitBytes - std::min(charged, stats.limitBytes))
        {
            ++stats.capRefusalsTotal; decision.reason = "cap";
            if (old.count(family->signature) && stats.limitBytes < previousLimit)
            { ++stats.pressureDropsTotal; pressureRejected.insert(family->signature); }
            continue;
        }
        const uint64_t retainedCold = Add(protectedCold, cold);
        const uint64_t remainingCold = budget.coldPhysical - std::min(retainedCold, budget.coldPhysical);
        if (stats.requiredReclaimBytes > remainingCold)
        {
            decision.reason = "pressure-reserve";
            if (old.count(family->signature)) { ++stats.pressureDropsTotal; pressureRejected.insert(family->signature); }
            continue;
        }
        previouslyStaged.insert(family->signature);
        selected.push_back({*family}); pins.insert(keys.begin(), keys.end());
        charged = Add(charged, extra); protectedCold = retainedCold;
    }
    stats.otherColdPhysicalBytes = budget.coldPhysical - std::min(protectedCold, budget.coldPhysical);
    Rebuild();
}
