#include "nri_voxel_page_pose_demand.h"
#include "nri_persistent_voxel_actor_demand.h"

#include <algorithm>

namespace
{
// A short direct animation can move through several poses while an earlier
// build is queued. Only primary-camera requests can establish a relationship.
constexpr uint32_t HistoryFrames = 64;
constexpr uint32_t FinishFrames = 32;
constexpr size_t MaxPosesPerOwner = 32, MaxOwners = 4096, MaxSiblings = 1024;
bool Recent(uint32_t seen, uint32_t now, uint32_t window = HistoryFrames)
{
    return seen != UINT32_MAX && seen <= now && now - seen <= window;
}
}

void NRIVoxelPagePoseDemand::Reset()
{
    worldEpoch = anchor = pageGeneration = 0;
    refreshedFrame = UINT32_MAX;
    owners.clear();
    siblings.clear();
    eligible.clear();
    finishing.clear();
}

void NRIVoxelPagePoseDemand::Refresh(uint64_t world, uint32_t frame, uint64_t pageMesh, uint64_t publishedGeneration,
    const std::vector<NRIVoxelActorDemandInput>& inputs)
{
    if (!world || !pageMesh) { Reset(); return; }
    if (worldEpoch != world || anchor != pageMesh || frame < refreshedFrame ||
        (pageGeneration && publishedGeneration != pageGeneration)) Reset();
    worldEpoch = world;
    anchor = pageMesh;
    pageGeneration = publishedGeneration;
    refreshedFrame = frame;
    eligible.clear();
    finishing.clear();
    for (auto it = owners.begin(); it != owners.end();)
    {
        if (!Recent(it->second.lastSeen, frame)) it = owners.erase(it);
        else
        {
            it->second.visibleNow = false;
            ++it;
        }
    }
    for (const auto& input : inputs)
    {
        if (!input.visible || !input.rayEligible || input.world != world || !input.lifetime || !input.mesh || !input.material) continue;
        auto it = owners.find(input.lifetime);
        if (it == owners.end())
        {
            if (owners.size() >= MaxOwners) continue;
            it = owners.try_emplace(input.lifetime).first;
        }
        auto& owner = it->second;
        owner.lastSeen = frame;
        owner.visibleNow = true;
        if (input.mesh == pageMesh)
            owner.lastPage = frame;
        else
        {
            auto& poses = owner.poses;
            auto pose = std::find_if(poses.begin(), poses.end(), [&](const Pose& p) { return p.mesh == input.mesh; });
            if (pose != poses.end())
            {
                if (!Recent(pose->frame, frame)) pose->spilled = false;
                pose->frame = frame;
            }
            else if (poses.size() < MaxPosesPerOwner) poses.push_back({input.mesh, frame});
            else *std::min_element(poses.begin(), poses.end(), [](const Pose& a, const Pose& b) { return a.frame < b.frame; }) =
                {input.mesh, frame};
        }
        if (Recent(owner.lastPage, frame))
            for (const auto& pose : owner.poses)
                if (Recent(pose.frame, frame) && siblings.size() < MaxSiblings) siblings.insert(pose.mesh);
    }
    // Resolve after observing the whole frame so the order of two actors
    // requesting related poses cannot change eligibility.
    if (!pageGeneration || siblings.empty()) return;
    for (const auto& entry : owners)
    {
        const auto& owner = entry.second;
        // A remembered pose must yield the build lane to the live animation.
        // When the observed page owner leaves, its already witnessed sequence
        // has one short window to finish using the two prepaid geometry slots.
        const bool finishSequence = !owner.visibleNow && Recent(owner.lastSeen, frame, FinishFrames) &&
            Recent(owner.lastPage, owner.lastSeen);
        for (const auto& pose : owner.poses)
        {
            const bool current = owner.visibleNow && pose.frame == frame;
            if (!siblings.count(pose.mesh)) continue;
            if (current) eligible.insert(pose.mesh);
            if (finishSequence && pose.spilled && Recent(pose.frame, owner.lastSeen)) finishing.insert(pose.mesh);
        }
    }
}

bool NRIVoxelPagePoseDemand::MayUse(uint64_t mesh, uint32_t frame) const
{
    return refreshedFrame == frame && (eligible.count(mesh) || finishing.count(mesh));
}

bool NRIVoxelPagePoseDemand::Finishing(uint64_t mesh, uint32_t frame) const
{
    return refreshedFrame == frame && finishing.count(mesh) != 0;
}

void NRIVoxelPagePoseDemand::ObserveOrdinarySpill(uint64_t mesh, uint32_t frame)
{
    if (!mesh || refreshedFrame != frame) return;
    for (auto& entry : owners)
    {
        auto& owner = entry.second;
        if (!owner.visibleNow) continue;
        for (auto& pose : owner.poses)
            if (pose.mesh == mesh && Recent(pose.frame, frame)) pose.spilled = true;
    }
}
