#include "nri_persistent_voxel_admission_reclaim.h"
#include "nri_persistent_voxel_retention_policy.h"
#include <algorithm>

bool NRIVoxelReclaimFits(NRIPersistentVoxelGeometryRanges& preview,
    const std::array<uint32_t, 3>& rows, const std::array<uint64_t, 3>& capacity)
{
    const auto lease = preview.Allocate(rows[0], rows[1], rows[2]);
    if (!lease) return false;
    const bool fits = preview.VertexStats().cursorRows <= capacity[0] &&
        preview.IndexStats().cursorRows <= capacity[1] && preview.PrimitiveStats().cursorRows <= capacity[2];
    preview.Release(lease);
    return fits;
}

NRIVoxelReclaimPlan PlanNRIVoxelAdmissionReclaim(const NRIPersistentVoxelGeometryRanges& ranges,
    const NRIVoxelReclaimRequest& request, const std::array<uint64_t, 3>& capacity,
    std::vector<NRIVoxelReclaimCandidate> candidates)
{
    NRIVoxelReclaimPlan plan;
    for (size_t i = 0; i < request.rows.size(); ++i)
        if (!request.rows[i] || request.rows[i] > capacity[i])
        { plan.reason = "request-exceeds-existing-capacity"; return plan; }
    // No speculative physical credit: even the plan must retain current usage,
    // AS/scratch/provider requirements and the complete general reserve.
    if (!request.budget || request.usage > request.budget ||
        request.reserve > request.budget - request.usage ||
        request.nonArenaBytes > request.budget - request.usage - request.reserve)
    { plan.reason = "nonarena-budget-floor"; return plan; }
    if (ranges.TransactionCount() > NRIVoxelAdmissionReclaim::MaxOwnerRows ||
        candidates.size() > NRIVoxelAdmissionReclaim::MaxOwnerRows)
    { plan.reason = "owner-snapshot-bound"; return plan; }
    auto preview = ranges;
    if (NRIVoxelReclaimFits(preview, request.rows, capacity))
    { plan.reason = "already-fits"; plan.fits = true; return plan; }
    const auto coverage = [&](const NRIVoxelReclaimCandidate& candidate)
    {
        return std::min({double(candidate.range.vertices.capacity) / request.rows[0],
            double(candidate.range.indices.capacity) / request.rows[1],
            double(candidate.range.primitives.capacity) / request.rows[2]});
    };
    std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b)
    {
        if (a.warmRank != b.warmRank) return a.warmRank < b.warmRank;
        // Within the same retention rank, useful shape comes before age. This
        // avoids a tiny/wrong-stream prefix hiding a single complete victim.
        const double ac = coverage(a), bc = coverage(b);
        if (ac != bc) return ac > bc;
        if (a.inactiveSince != b.inactiveSince) return a.inactiveSince < b.inactiveSince;
        return a.mesh < b.mesh;
    });
    uint32_t previews = 0;
    std::vector<NRIVoxelReclaimCandidate> combined;
    for (const auto& candidate : candidates)
    {
        if (plan.examined == NRIVoxelAdmissionReclaim::MaxCandidates) break;
        ++plan.examined;
        if (request.beforeGrowth && candidate.warmRank != 0) continue;
        if (candidate.pinned || candidate.mesh == request.mesh || !preview.Owns(candidate.range)) continue;
        if (previews == NRIVoxelAdmissionReclaim::MaxPreviews)
        { plan.reason = "bounded-preview-limit"; break; }
        ++previews;
        auto single = ranges;
        single.Release(candidate.range);
        if (NRIVoxelReclaimFits(single, request.rows, capacity))
        { plan.victims = {candidate}; plan.reason = "single-contiguous-range-fit"; plan.fits = true; return plan; }
        if (combined.size() == NRIVoxelAdmissionReclaim::MaxVictims) continue;
        preview.Release(candidate.range);
        combined.push_back(candidate);
        if (NRIVoxelReclaimFits(preview, request.rows, capacity))
        { plan.victims = combined; plan.reason = "contiguous-ranges-fit"; plan.fits = true; return plan; }
    }
    // A partial plan does not authorize any eviction.
    plan.victims.clear();
    return plan;
}

void NRIVoxelAdmissionReclaim::Reset()
{ records.clear(); epoch = tick = suppressed = 0; processedTick = growthPreviewTick = UINT64_MAX; nextEpisode = 1; probes = 0; }

void NRIVoxelAdmissionReclaim::BeginTick(uint64_t nextEpoch, uint64_t nextTick)
{
    if (epoch != nextEpoch || nextTick < tick) Reset();
    epoch = nextEpoch;
    if (tick != nextTick) { tick = nextTick; probes = 0; }
}

bool NRIVoxelAdmissionReclaim::Suppress(uint64_t mesh, const std::array<uint32_t, 3>& rows,
    uint64_t revision, bool growsArena, uint64_t signature)
{
    if (!growsArena) return false; // Existing fitting ranges and ready reuse stay eligible.
    auto found = records.find(mesh);
    if (found != records.end())
    {
        const auto& record = found->second;
        if (!record.request.beforeGrowth && record.request.rows == rows && record.request.revision == revision &&
            record.stage != Stage::Retry && record.request.signature == signature && tick >= record.request.tick &&
            tick - record.request.tick < ProbeTicks)
        { ++suppressed; return true; }
    }
    return false;
}

bool NRIVoxelAdmissionReclaim::PermitPreflight(bool growsArena)
{
    if (!growsArena) return true;
    if (probes >= MaxPreflightsPerTick) { ++suppressed; return false; }
    ++probes;
    return true;
}

bool NRIVoxelAdmissionReclaim::Observe(NRIVoxelReclaimRequest& request)
{
    auto found = records.find(request.mesh);
    if (found == records.end())
    {
        if (records.size() >= MaxRecords) return false;
        request.episode = nextEpisode++;
        records.emplace(request.mesh, Record{request});
        return true;
    }
    auto& record = found->second;
    request.episode = record.request.episode;
    if (record.stage == Stage::Waiting) { record.request = request; return false; }
    const bool changed = record.request.rows != request.rows || record.request.revision != request.revision ||
        record.request.nonArenaBytes != request.nonArenaBytes || record.request.signature != request.signature ||
        record.request.beforeGrowth != request.beforeGrowth;
    const auto oldStage = record.stage;
    if (changed) request.episode = nextEpisode++;
    record.request = request;
    // A change during the one-presentation wait gets a fresh plan on the
    // original deadline. Changes after that episode may start a new observation.
    if (oldStage == Stage::Confirming) return changed;
    if (changed) { record.confirmationVictim = 0; record.confirmationDemand = {}; }
    record.stage = changed ? Stage::Observed :
        (oldStage == Stage::Retry ? Stage::Blocked : oldStage);
    if (record.stage != Stage::Waiting) record.retiring.clear();
    return changed;
}

NRIVoxelAdmissionReclaim::GrowthPreview NRIVoxelAdmissionReclaim::BeforeGrowth(
    NRIVoxelReclaimRequest& request, const std::array<uint64_t, 3>& capacity,
    uint64_t oldBacking, uint64_t projectedPeak)
{
    // This is an opportunity to reuse cold geometry. The normal preflight has
    // already admitted this replacement. Roomy and small allocations stay on it.
    if (!oldBacking || oldBacking < request.reserve || !request.budget ||
        projectedPeak <= NRIVoxelCacheLowTarget(request.budget))
        return GrowthPreview::Proceed;
    for (size_t i = 0; i < capacity.size(); ++i)
        if (!request.rows[i] || request.rows[i] > capacity[i]) return GrowthPreview::Proceed;

    const auto found = records.find(request.mesh);
    if (found != records.end())
    {
        const auto& record = found->second;
        if (record.request.beforeGrowth && record.request.rows == request.rows &&
            request.nonArenaBytes >= record.request.nonArenaBytes &&
            request.avoidedArenaBytes >= record.request.avoidedArenaBytes &&
            (request.tick == record.request.tick ||
                (record.stage == Stage::Waiting && request.tick - record.request.tick < ProbeTicks)))
            return GrowthPreview::Waiting;
        return GrowthPreview::Proceed;
    }
    // Offer one new target per presentation, matching the planner's work
    // budget. An unserviced notice delays that owner for just this presentation.
    if (growthPreviewTick == tick) return GrowthPreview::Proceed;
    request.beforeGrowth = true;
    if (!Observe(request)) return GrowthPreview::Proceed;
    growthPreviewTick = tick;
    return GrowthPreview::Queued;
}

void NRIVoxelAdmissionReclaim::Success(uint64_t mesh) { records.erase(mesh); }
bool NRIVoxelAdmissionReclaim::RetryAllowed(uint64_t mesh) const
{
    const auto found = records.find(mesh);
    return found != records.end() && found->second.stage == Stage::Retry;
}

bool NRIVoxelAdmissionReclaim::ConfirmHeldUpgrade(Record& record, uint64_t victim, const NRIVoxelMeshUpgradeDemand& demand)
{
    if (!victim || record.confirmationVictim || record.request.beforeGrowth || !record.request.freshActorPair ||
        demand.consumers != 1 || demand.requestedConsumers != 1 || demand.oldestWait > 1) return false;
    record.confirmationVictim = victim;
    record.confirmationDemand = demand;
    record.lastPlanTick = tick;
    record.stage = Stage::Confirming;
    return true;
}

const char* NRIVoxelAdmissionReclaim::Poll(Record& record, bool usefulDemand,
    uint64_t geometryRevision, uint64_t demandRevision, const NRIPersistentVoxelGeometryRanges& ranges,
    const std::array<uint64_t, 3>& capacity)
{
    if (!usefulDemand || record.request.epoch != epoch)
        return record.stage == Stage::Confirming ? "held-upgrade-superseded" : "demand-superseded";
    if (record.stage == Stage::Confirming && tick != record.lastPlanTick)
    {
        record.stage = Stage::Observed;
        return "held-upgrade-confirmed";
    }
    if (record.request.beforeGrowth &&
        ((record.stage == Stage::Observed && tick - record.request.tick > 1) ||
         (record.stage == Stage::Waiting && tick - record.request.tick >= ProbeTicks)))
    {
        // The actual retirement service continues to own any queued leases.
        record.stage = Stage::Blocked;
        record.retiring.clear();
        return "growth-preview-expired";
    }
    if (record.stage == Stage::Waiting)
    {
        for (const auto& lease : record.retiring) if (ranges.Owns(lease)) return nullptr;
        if (ranges.TransactionCount() > MaxOwnerRows) return nullptr;
        auto preview = ranges;
        record.retiring.clear();
        record.stage = NRIVoxelReclaimFits(preview, record.request.rows, capacity) ? Stage::Retry : Stage::Blocked;
        record.processedRevision = geometryRevision; record.processedDemandRevision = demandRevision;
        record.retryReadyTick = record.lastPlanTick = tick;
        return record.stage == Stage::Retry ? "retired-ranges-reusable" : "ranges-consumed-by-other-work";
    }
    if (record.stage == Stage::Retry && tick - record.retryReadyTick >= ProbeTicks)
    {
        record.stage = Stage::Blocked; record.processedRevision = geometryRevision;
        record.processedDemandRevision = demandRevision; record.lastPlanTick = tick;
        return "retry-window-expired";
    }
    if (!record.request.beforeGrowth && record.stage == Stage::Blocked && (record.processedRevision != geometryRevision ||
        record.processedDemandRevision != demandRevision || tick - record.lastPlanTick >= ProbeTicks))
        record.stage = Stage::Observed;
    return nullptr;
}
