#include "nri_voxel_page_pose_demand.h"
#include "nri_persistent_voxel_actor_demand.h"

#include <algorithm>

namespace
{
// A short direct animation can move through several poses while an earlier
// build is queued. Limit the observed sequence and never extend a request
// using retained, indirect-only, or departed actors.
constexpr uint32_t HistoryFrames = 64;
constexpr size_t MaxPosesPerOwner = 32, MaxOwners = 4096, MaxSiblings = 1024;
bool Recent(uint32_t seen, uint32_t now) { return seen != UINT32_MAX && seen <= now && now - seen <= HistoryFrames; }
}

void NRIVoxelPagePoseDemand::Reset()
{
    worldEpoch = anchor = 0;
    refreshedFrame = UINT32_MAX;
    owners.clear();
    siblings.clear();
    eligible.clear();
}

void NRIVoxelPagePoseDemand::Refresh(uint64_t world, uint32_t frame, uint64_t pageMesh, bool pagePublished,
    const std::vector<NRIVoxelActorDemandInput>& inputs)
{
    if (!world || !pageMesh) { Reset(); return; }
    if (worldEpoch != world || anchor != pageMesh || frame < refreshedFrame) Reset();
    worldEpoch = world;
    anchor = pageMesh;
    refreshedFrame = frame;
    eligible.clear();
    for (auto it = owners.begin(); it != owners.end();)
    {
        if (!Recent(it->second.lastSeen, frame)) it = owners.erase(it);
        else { it->second.visibleNow = false; ++it; }
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
            if (pose == poses.end() && poses.size() == MaxPosesPerOwner)
                pose = std::min_element(poses.begin(), poses.end(), [](const Pose& a, const Pose& b) { return a.frame < b.frame; });
            if (pose == poses.end()) poses.push_back({input.mesh, frame});
            else *pose = {input.mesh, frame};
        }
        if (Recent(owner.lastPage, frame))
            for (const auto& pose : owner.poses)
                if (Recent(pose.frame, frame) && siblings.size() < MaxSiblings) siblings.insert(pose.mesh);
    }
    // Resolve after observing the whole frame so the order of two actors
    // requesting related poses cannot change eligibility.
    if (!pagePublished || siblings.empty()) return;
    for (const auto& entry : owners)
    {
        const auto& owner = entry.second;
        if (!owner.visibleNow) continue;
        for (const auto& pose : owner.poses)
            if (Recent(pose.frame, frame) && siblings.count(pose.mesh)) eligible.insert(pose.mesh);
    }
}

bool NRIVoxelPagePoseDemand::MayUse(uint64_t mesh, uint32_t frame) const
{
    return refreshedFrame == frame && eligible.count(mesh) != 0;
}
