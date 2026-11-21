#include "nri_persistent_voxels.h"
#include "nri_persistent_voxel_publication_services.h"
#include "nri_cvars.h"
#include "perf_audit.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

namespace
{
struct ReclaimPassTiming
{
    std::function<void(double)> report;
    uint64_t start = PerfAuditClockNs();
    ~ReclaimPassTiming() { report(double(PerfAuditClockNs() - start) / 1e6); }
};
uint64_t GeometryBytes(const NRIPersistentVoxelGeometryRangeHandle& range)
{
    return uint64_t(range.vertices.capacity) * sizeof(nri_scene::SceneVertex) +
        uint64_t(range.indices.capacity) * sizeof(uint32_t) +
        uint64_t(range.primitives.capacity) * sizeof(nri_scene::PrimitiveData);
}
}

void NRIPersistentVoxelResidency::ReportAdmissionReclaim(const char* action, const char* reason,
    const NRIVoxelReclaimRequest& request, uint32_t frameIndex, uint64_t victim, uint32_t rank,
    uint64_t geometryBytes, uint64_t physicalBytes, uint32_t candidates, uint32_t pins,
    const NRIPersistentVoxelGeometryRangeHandle* victimRange, double workCpuMs,
    uint32_t recordsExamined, uint32_t plansEvaluated, const NRIVoxelMeshUpgradeDemand* previousDemand) const
{
    if (!PerfAuditActive()) return;
    const auto demand = actorDemand.Upgrade(request.mesh, frameIndex);
    const auto& witness = previousDemand ? *previousDemand : demand;
    const auto lease = victimRange ? *victimRange : NRIPersistentVoxelGeometryRangeHandle{};
    char fields[3072];
    snprintf(fields, sizeof(fields), "\"action\":\"%s\",\"reason\":\"%s\",\"map_epoch\":%u,"
        "\"renderer_frame\":%u,\"retry_tick\":%llu,\"retry_clock_unit\":\"presentation\",\"mesh_key\":%llu,"
        "\"actor_identity\":%llu,\"actor_world\":%llu,\"actor_lifetime\":%llu,\"actor_from_prior_observation\":%s,\"useful_consumers\":%u,"
        "\"oldest_exact_wait_frames\":%u,\"vertex_rows\":%u,\"index_rows\":%u,\"primitive_rows\":%u,"
        "\"usage_bytes\":%llu,\"budget_bytes\":%llu,\"reserve_bytes\":%llu,\"nonarena_bytes\":%llu,"
        "\"avoided_arena_allocation_bytes\":%llu,\"victim_mesh_key\":%llu,\"warm_rank\":%u,"
        "\"geometry_bytes_retiring\":%llu,\"physical_bytes_retiring\":%llu,\"candidate_rows\":%u,"
        "\"hard_pinned_meshes\":%u,\"geometry_revision\":%llu,\"suppressed_preflights\":%llu,"
        "\"vertex_hole_rows\":%llu,\"index_hole_rows\":%llu,\"primitive_hole_rows\":%llu,"
        "\"episode_id\":%llu,\"before_growth\":%s,\"work_cpu_ms\":%.6f,\"owner_records\":%llu,\"records_examined\":%u,\"plans_evaluated\":%u,"
        "\"vertex_capacity_rows\":%llu,\"index_capacity_rows\":%llu,\"primitive_capacity_rows\":%llu,"
        "\"victim_transaction\":%llu,\"victim_vertex_offset\":%u,\"victim_vertex_capacity\":%u,\"victim_vertex_generation\":%llu,"
        "\"victim_index_offset\":%u,\"victim_index_capacity\":%u,\"victim_index_generation\":%llu,"
        "\"victim_primitive_offset\":%u,\"victim_primitive_capacity\":%u,\"victim_primitive_generation\":%llu",
        action, reason, residencyMapGeneration, frameIndex, (unsigned long long)GetNRIVoxelAdmissionRetryClock(),
        (unsigned long long)request.mesh, (unsigned long long)witness.identity, (unsigned long long)witness.world,
        (unsigned long long)witness.lifetime, previousDemand ? "true" : "false", demand.consumers, witness.oldestWait,
        request.rows[0], request.rows[1], request.rows[2], (unsigned long long)request.usage,
        (unsigned long long)request.budget, (unsigned long long)request.reserve, (unsigned long long)request.nonArenaBytes,
        (unsigned long long)request.avoidedArenaBytes, (unsigned long long)victim, rank,
        (unsigned long long)geometryBytes, (unsigned long long)physicalBytes, candidates, pins,
        (unsigned long long)reclaimGeometryRevision, (unsigned long long)admissionReclaim.suppressed,
        (unsigned long long)geometryRanges.VertexStats().holeRows, (unsigned long long)geometryRanges.IndexStats().holeRows,
        (unsigned long long)geometryRanges.PrimitiveStats().holeRows, (unsigned long long)request.episode,
        request.beforeGrowth ? "true" : "false", workCpuMs,
        (unsigned long long)admissionReclaim.RecordCount(), recordsExamined, plansEvaluated,
        (unsigned long long)(vertexBuffer.size / sizeof(nri_scene::SceneVertex)),
        (unsigned long long)(indexBuffer.size / sizeof(uint32_t)),
        (unsigned long long)(primitiveBuffer.size / sizeof(nri_scene::PrimitiveData)),
        (unsigned long long)lease.transaction, lease.vertices.offset, lease.vertices.capacity,
        (unsigned long long)lease.vertices.generation, lease.indices.offset, lease.indices.capacity,
        (unsigned long long)lease.indices.generation, lease.primitives.offset, lease.primitives.capacity,
        (unsigned long long)lease.primitives.generation);
    PerfAuditWriteEvent("voxel_admission_reclaim", PerfAuditCurrentToken(), fields);
}

void NRIPersistentVoxelResidency::ProcessAdmissionReclaim(uint32_t frameIndex,
    const NRIPersistentVoxelSettings& settings, const NRIPersistentVoxelResetServices& services)
{
    if (!(bool)nri_ptvoxeladmissionreclaim || !(bool)nri_ptvoxelallocationbudget) return;
    admissionReclaim.BeginTick(residencyMapGeneration, GetNRIVoxelAdmissionRetryClock());
    if (!admissionReclaim.BeginProcess() || !actorDemand.AuthorityAvailable()) return;
    auto& records = admissionReclaim.Records();
    if (records.empty()) return;
    NRIVoxelReclaimRequest workRequest;
    uint32_t workRecords = 0, workPlans = 0, workCandidates = 0, workPins = 0;
    ReclaimPassTiming timing{[&](double ms) { ReportAdmissionReclaim("work", "bounded-owner-pass", workRequest,
        frameIndex, 0, 0, 0, 0, workCandidates, workPins, nullptr, ms, workRecords, workPlans); }};
    NRIVoxelAdmissionReclaim::Record* selected = nullptr;
    uint32_t oldest = 0;
    bool activeEpisode = false;
    bool selectedConfirmed = false;
    const std::array<uint64_t, 3> capacity = { vertexBuffer.size / sizeof(nri_scene::SceneVertex),
        indexBuffer.size / sizeof(uint32_t), primitiveBuffer.size / sizeof(nri_scene::PrimitiveData) };
    for (auto it = records.begin(); it != records.end(); )
    {
        auto& record = it->second;
        ++workRecords;
        if (!workRequest.mesh) workRequest = record.request;
        if (record.request.beforeGrowth && !(bool)nri_ptvoxelarenarecovery)
        {
            it = records.erase(it);
            continue;
        }
        const auto demand = actorDemand.Upgrade(it->first, frameIndex);
        const bool confirming = record.stage == NRIVoxelAdmissionReclaim::Stage::Confirming;
        const auto transition = admissionReclaim.Poll(record, demand.consumers != 0,
            reclaimGeometryRevision, actorDemand.UpgradeRevision(), geometryRanges, capacity);
        if (!demand.consumers || record.request.epoch != residencyMapGeneration)
        {
            ReportAdmissionReclaim("outcome", transition, record.request, frameIndex, confirming ? record.confirmationVictim : 0,
                0, 0, 0, 0, 0, nullptr, 0, 0, 0, confirming ? &record.confirmationDemand : nullptr);
            it = records.erase(it); continue; // Retirement stays owned by its original fence.
        }
        ++it;
        if (transition)
            ReportAdmissionReclaim(confirming || record.stage == NRIVoxelAdmissionReclaim::Stage::Retry ? "retry" : "outcome",
                transition, record.request, frameIndex, confirming ? record.confirmationVictim : 0);
        activeEpisode |= record.stage == NRIVoxelAdmissionReclaim::Stage::Waiting ||
            record.stage == NRIVoxelAdmissionReclaim::Stage::Retry;
        if (record.stage == NRIVoxelAdmissionReclaim::Stage::Observed &&
            (!selected || (confirming != selectedConfirmed ? confirming :
                demand.oldestWait > oldest || (demand.oldestWait == oldest && record.request.mesh < selected->request.mesh))))
        { selected = &record; oldest = demand.oldestWait; selectedConfirmed = confirming; }
    }
    if (activeEpisode || !selected || retentionQuarantined || preloadPending || loadingWarmupActive ||
        !services.cacheRetirementAllowed || !services.cacheRetirementAllowed(services.user) ||
        !services.retireGeometryRange || !services.allocationBudget) return;
    auto& record = *selected;
    workRequest = record.request;
    record.stage = NRIVoxelAdmissionReclaim::Stage::Blocked;
    record.noRangeFit = false;
    record.processedRevision = reclaimGeometryRevision;
    record.processedDemandRevision = actorDemand.UpgradeRevision();
    record.lastPlanTick = admissionReclaim.Tick();
    record.retiring.clear();
    for (size_t i = 0; i < capacity.size(); ++i)
        if (!record.request.rows[i] || record.request.rows[i] > capacity[i])
        { record.noRangeFit = true; ReportAdmissionReclaim("outcome", "request-exceeds-existing-capacity", record.request, frameIndex); return; }
    if (meshVariantResources.size() > NRIVoxelAdmissionReclaim::MaxOwnerRows ||
        geometryRanges.TransactionCount() > NRIVoxelAdmissionReclaim::MaxOwnerRows)
    { ReportAdmissionReclaim("outcome", "owner-snapshot-bound", record.request, frameIndex); return; }
    NRIVoxelCachePins pins;
    if (!CollectCachePins(settings, pins, NRIVoxelAdmissionReclaim::MaxOwnerRows))
    { ReportAdmissionReclaim("outcome", "pin-authority-unavailable-or-bound", record.request, frameIndex); return; }
    std::vector<NRIVoxelReclaimCandidate> candidates;
    candidates.reserve(meshVariantResources.size());
    for (const auto& pair : meshVariantResources)
    {
        if (pins.meshes.count(pair.first) || pair.second.geometryRange.runtimeExtent) continue;
        candidates.push_back({pair.first, pair.second.geometryRange, actorDemand.WarmMeshRank(pair.first, frameIndex),
            inactiveMeshSince.emplace(pair.first, frameIndex).first->second, false});
    }
    services.allocationBudget(services.user, record.request.usage, record.request.budget);
    workRequest = record.request;
    NRIVoxelReclaimTailProbe tailProbe;
    const bool traceTail = (bool)nri_ptvoxelmeshlifetrace && PerfAuditActive();
    const auto plan = PlanNRIVoxelAdmissionReclaim(geometryRanges, record.request, capacity, std::move(candidates),
        traceTail ? &tailProbe : nullptr);
    workPlans = 1; workCandidates = plan.examined; workPins = uint32_t(pins.meshes.size());
    ReportAdmissionReclaim("plan", plan.reason, record.request, frameIndex, 0, 0, 0, 0,
        plan.examined, uint32_t(pins.meshes.size()));
    if (tailProbe.limited)
    {
        const auto& donor = tailProbe.donor;
        ReportAdmissionReclaim("probe", donor.mesh ? "verified-whole-tail-lease" : "no-verified-whole-tail-lease",
            record.request, frameIndex, donor.mesh, donor.warmRank, 0, 0,
            tailProbe.examined, uint32_t(pins.meshes.size()), &donor.range);
    }
    if (!plan.fits)
    {
        record.noRangeFit = strcmp(plan.reason, "bounded-preview-limit") == 0 ||
            strcmp(plan.reason, "no-fitting-victims") == 0;
        return;
    }
    if (plan.victims.empty())
    { record.stage = NRIVoxelAdmissionReclaim::Stage::Retry; record.retryReadyTick = admissionReclaim.Tick(); return; }
    if (!record.confirmationVictim)
    {
        const auto victim = FindHeldUpgradeFamilyVictim(record.request, plan, frameIndex);
        if (admissionReclaim.ConfirmHeldUpgrade(record, victim, actorDemand.Upgrade(record.request.mesh, frameIndex)))
        {
            ReportAdmissionReclaim("defer", "verify-held-upgrade", record.request, frameIndex, victim);
            return;
        }
    }
    // No submission occurs between the authoritative pin snapshot and these
    // retire calls. The live allocator remains occupied until completion.
    for (const auto& victim : plan.victims)
    {
        const auto found = meshVariantResources.find(victim.mesh);
        if (found == meshVariantResources.end()) continue;
        const auto& mesh = found->second;
        const auto* shared = sharedBlasCache.Find(victim.mesh);
        const uint64_t physical = mesh.vertexBuffer.memorySize + mesh.indexBuffer.memorySize +
            mesh.accelerationStructure.memorySize + mesh.shadowProxy.vertexBuffer.memorySize +
            mesh.shadowProxy.indexBuffer.memorySize + mesh.shadowProxy.accelerationStructure.memorySize +
            (shared ? shared->accelerationStructure.memorySize : 0);
        ReportAdmissionReclaim("victim", "fence-retirement-queued", record.request, frameIndex, victim.mesh,
            victim.warmRank, GeometryBytes(victim.range), physical, 0, 0, &victim.range);
        record.retiring.push_back(victim.range);
        NoteCacheMeshEviction(victim.mesh, frameIndex, "admission-reclaim", false);
        EvictInactiveMesh(victim.mesh, services);
        for (auto it = instances.begin(); it != instances.end(); )
            if (it->second.meshResourceKey == victim.mesh) it = instances.erase(it); else ++it;
    }
    record.stage = NRIVoxelAdmissionReclaim::Stage::Waiting;
    ReportAdmissionReclaim("wait", "owned-ranges-await-fence", record.request, frameIndex);
    PerfAuditAdd(PerfAuditMetric::VoxelCacheMeshEvictions, plan.victims.size());
    // Ready/Failed history must not turn back into speculative Pending work
    // when the index sees backing deliberately removed for a useful target.
    const auto pruned = PruneOrphanedTerminalAdmissions(pins, services);
    PerfAuditAdd(PerfAuditMetric::VoxelCacheTerminalPruned, pruned);
    ResetActorPublicationConsumer();
    MarkMaintenanceMutation(); RebuildAdmissionIndex(true); services.InvalidateSceneDataDescriptors();
}
