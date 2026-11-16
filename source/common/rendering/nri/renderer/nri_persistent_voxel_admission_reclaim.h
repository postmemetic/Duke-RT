#pragma once

#include "nri_persistent_voxel_geometry_ranges.h"
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <set>
#include <unordered_set>

struct NRIVoxelCachePins
{
    std::unordered_set<uint64_t> meshes, materials, pendingMeshes;
    std::set<std::pair<uint64_t, uint64_t>> pairs;
};

inline bool NRIVoxelPruneOrphanedAdmission(bool terminal, bool backingMissing, bool pairPinned,
    bool directRequested, bool pendingMesh)
{ return terminal && backingMissing && !pairPinned && (!directRequested || !pendingMesh); }

struct NRIVoxelReclaimRequest
{
    uint64_t mesh = 0, epoch = 0, tick = 0, revision = 0;
    std::array<uint32_t, 3> rows = {};
    uint64_t usage = 0, budget = 0, reserve = 0, nonArenaBytes = 0;
    uint64_t signature = 0, avoidedArenaBytes = 0, episode = 0;
};

struct NRIVoxelReclaimCandidate
{
    uint64_t mesh = 0;
    NRIPersistentVoxelGeometryRangeHandle range;
    uint32_t warmRank = 0, inactiveSince = 0;
    bool pinned = true;
};

struct NRIVoxelReclaimPlan
{
    const char* reason = "no-fitting-victims";
    std::vector<NRIVoxelReclaimCandidate> victims;
    uint32_t examined = 0;
    bool fits = false;
};

// Preview uses the actual range allocator, including contiguous holes and tail
// trim. Buffer row capacity must come from buffer.size, never padded memorySize.
bool NRIVoxelReclaimFits(NRIPersistentVoxelGeometryRanges& preview,
    const std::array<uint32_t, 3>& rows, const std::array<uint64_t, 3>& capacity);
NRIVoxelReclaimPlan PlanNRIVoxelAdmissionReclaim(const NRIPersistentVoxelGeometryRanges& ranges,
    const NRIVoxelReclaimRequest& request, const std::array<uint64_t, 3>& capacity,
    std::vector<NRIVoxelReclaimCandidate> candidates);

class NRIVoxelAdmissionReclaim
{
public:
    static constexpr uint32_t MaxRecords = 256, MaxCandidates = 128, MaxVictims = 4, MaxPreviews = 16;
    static constexpr uint32_t MaxOwnerRows = 4096, MaxPreflightsPerTick = 16;
    static constexpr uint64_t ProbeTicks = 120;
    enum class Stage { Observed, Waiting, Retry, Blocked };
    struct Record
    {
        NRIVoxelReclaimRequest request;
        Stage stage = Stage::Observed;
        std::vector<NRIPersistentVoxelGeometryRangeHandle> retiring;
        uint64_t processedRevision = UINT64_MAX;
        uint64_t retryReadyTick = 0;
        uint64_t processedDemandRevision = 0, lastPlanTick = 0;
    };
    void Reset();
    void BeginTick(uint64_t epoch, uint64_t tick);
    bool PermitPreflight(bool growsArena);
    bool Suppress(uint64_t mesh, const std::array<uint32_t, 3>& rows, uint64_t revision,
        bool growsArena, uint64_t signature);
    bool Observe(NRIVoxelReclaimRequest& request);
    void Success(uint64_t mesh);
    bool RetryAllowed(uint64_t mesh) const;
    const char* Poll(Record& record, bool usefulDemand, uint64_t geometryRevision, uint64_t demandRevision,
        const NRIPersistentVoxelGeometryRanges& ranges, const std::array<uint64_t, 3>& capacity);
    std::unordered_map<uint64_t, Record>& Records() { return records; }
    std::size_t RecordCount() const { return records.size(); }
    uint64_t Tick() const { return tick; }
    bool BeginProcess() { if (processedTick == tick) return false; processedTick = tick; return true; }
    uint64_t suppressed = 0;
private:
    std::unordered_map<uint64_t, Record> records;
    uint64_t epoch = 0, tick = 0, processedTick = UINT64_MAX, nextEpisode = 1;
    uint32_t probes = 0;
};
