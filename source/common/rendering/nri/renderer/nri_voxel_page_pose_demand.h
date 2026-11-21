#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct NRIVoxelActorDemandInput;

// Learn pose relationships from persistent mesh requests made by one live
// actor. This owner tracks demand only; native range and fence ownership are
// unchanged when an actor disappears or the selected page changes.
class NRIVoxelPagePoseDemand
{
public:
    void Reset();
    void Refresh(uint64_t world, uint32_t frame, uint64_t pageMesh, uint64_t publishedGeneration,
        const std::vector<NRIVoxelActorDemandInput>& inputs);
    bool MayUse(uint64_t mesh, uint32_t frame) const;
    bool Finishing(uint64_t mesh, uint32_t frame) const;
    void ObserveOrdinarySpill(uint64_t mesh, uint32_t frame);

private:
    struct Pose { uint64_t mesh; uint32_t frame; bool spilled = false; };
    struct Owner
    {
        std::vector<Pose> poses;
        uint32_t lastSeen = 0;
        uint32_t lastPage = UINT32_MAX;
        bool visibleNow = false;
    };
    uint64_t worldEpoch = 0, anchor = 0, pageGeneration = 0;
    uint32_t refreshedFrame = UINT32_MAX;
    std::unordered_map<uint64_t, Owner> owners;
    std::unordered_set<uint64_t> siblings, eligible, finishing;
};
