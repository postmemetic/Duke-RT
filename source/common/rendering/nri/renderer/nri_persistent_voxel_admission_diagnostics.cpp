#include "nri_persistent_voxels.h"
#include "nri_persistent_voxel_admission_diagnostics.h"
#include "nri_cvars.h"
#include "perf_audit.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace
{
constexpr size_t PairLimit = 65536;
constexpr uint32_t DetailLimit = 2048, HeartbeatFrames = 30;
using U64 = unsigned long long;
bool Enabled() { return (bool)nri_ptvoxelcachevalidate && PerfAuditActive(); }
const char* Bool(bool b) { return b ? "true" : "false"; }
const char* Safe(const char* s) { return s ? s : "none"; } // Fixed internal labels only.
const char* StateName(PersistentVoxelAdmissionState state)
{
    switch (state)
    {
    case PersistentVoxelAdmissionState::Pending: return "pending";
    case PersistentVoxelAdmissionState::DirectComputePending: return "direct-compute-pending";
    case PersistentVoxelAdmissionState::DirectBlasPending: return "direct-blas-pending";
    case PersistentVoxelAdmissionState::UploadingVertices: return "uploading-vertices";
    case PersistentVoxelAdmissionState::UploadingIndices: return "uploading-indices";
    case PersistentVoxelAdmissionState::UploadingPrimitives: return "uploading-primitives";
    case PersistentVoxelAdmissionState::BuildingBlas: return "building-blas";
    case PersistentVoxelAdmissionState::Ready: return "ready";
    case PersistentVoxelAdmissionState::Deferred: return "deferred";
    case PersistentVoxelAdmissionState::Failed: return "failed";
    }
    return "unknown";
}
bool Terminal(PersistentVoxelAdmissionState state)
{
    return state == PersistentVoxelAdmissionState::Ready || state == PersistentVoxelAdmissionState::Failed;
}
struct EventBudget
{
    uint64_t epoch = 0, presentation = UINT64_MAX, suppressedTotal = 0;
    uint32_t emitted = 0, suppressed = 0;
    void Boundary()
    {
        const auto token = PerfAuditCurrentToken();
        if (epoch != token.epoch) { epoch = token.epoch; suppressedTotal = 0; presentation = UINT64_MAX; }
        if (presentation != token.presentation) { presentation = token.presentation; emitted = suppressed = 0; }
    }
    void Emit(const char* kind, const char* fields)
    {
        Boundary();
        if (emitted < DetailLimit) { ++emitted; PerfAuditWriteEvent(kind, PerfAuditCurrentToken(), fields); }
        else
        {
            ++suppressedTotal;
            if (++suppressed == 1)
                PerfAuditWriteEvent("voxel_admission_diagnostic_overflow", PerfAuditCurrentToken(),
                    "\"domain\":\"detail-events\",\"event_limit\":2048,\"complete\":false");
        }
    }
};
// Shared output bound includes compute callbacks outside a pump. No heap storage.
EventBudget output;
uint64_t diagnosticWorkNs = 0;
uint32_t diagnosticWorkDepth = 0;
// Main-thread observer calls can nest (an enqueue note takes a snapshot). Charge
// each outer body once; never let JSON I/O manufacture the scheduler's ms stop.
class DiagnosticWork
{
    uint64_t start = 0;
public:
    DiagnosticWork() { if (diagnosticWorkDepth++ == 0) start = PerfAuditClockNs(); }
    ~DiagnosticWork()
    {
        if (--diagnosticWorkDepth == 0)
        {
            const uint64_t elapsed = PerfAuditClockNs() - start;
            diagnosticWorkNs += elapsed;
            PerfAuditAdd(PerfAuditMetric::AuditCensusNs, elapsed);
        }
    }
};
}

struct NRIPersistentVoxelAdmissionDiagnosticState
{
    struct Pair
    {
        uint64_t firstPresentation = 0, firstAttempt = UINT64_MAX, lastAttempt = UINT64_MAX;
        uint64_t attempts = 0, pump = 0, visitedPump = 0, episode = 0;
        int32_t schedulerResult = -1;
        const char* episodeStart = "first-observed-existing";
        uint32_t position = UINT32_MAX, lastFrame = UINT32_MAX;
        bool terminal = false;
        std::string signature;
    };
    uint64_t epoch = 0, nextPump = 0, nextEpisode = 0, pairOverflow = 0;
    uint32_t map = 0;
    std::unordered_map<uint64_t, Pair> pairs;
};

// Separate optional observer: bounded storage and output without enabling the
// expensive per-pair correctness history. These are enclosing wall times,
// which may include driver/GPU waits, not claims about pure CPU execution.
struct NRIVoxelAdmissionTimingState
{
    struct Visit
    {
        uint64_t pair = 0, mesh = 0, material = 0, elapsed = 0, beforeAttempt = 0;
        uint32_t beforeState = 0, afterState = 0;
        int32_t picnum = -1, voxel = -1;
        bool attempted = false;
        const char* reason = "none";
    };
    uint64_t start = PerfAuditClockNs(), budgetStart = 0, visitStart = 0, attemptStart = 0;
    uint64_t maintenance = 0, setup = 0, candidatesNs = 0, visitsNs = 0, beforeAttemptNs = 0;
    uint64_t stageStart = start;
    NRIVoxelAdmissionTimingStage stage = NRIVoxelAdmissionTimingStage::None;
    std::array<uint64_t, size_t(NRIVoxelAdmissionTimingStage::Count)> stages = {};
    uint32_t candidates = 0, visited = 0, attempted = 0;
    bool firstVisit = true, activeVisit = false, maintenanceComplete = false;
    Visit current;
    std::array<Visit, 4> top = {};

    void Stage(NRIVoxelAdmissionTimingStage next, uint64_t now)
    {
        if (stage != NRIVoxelAdmissionTimingStage::None) stages[size_t(stage)] += now - stageStart;
        stage = next; stageStart = now;
    }
    void EndMaintenance()
    {
        const uint64_t now = PerfAuditClockNs();
        Stage(NRIVoxelAdmissionTimingStage::None, now);
        maintenance = now - start;
        maintenanceComplete = true;
    }

    void VisitBegin(NRIPersistentVoxelResidency& owner, uint64_t pair)
    {
        visitStart = PerfAuditClockNs();
        if (firstVisit && budgetStart) candidatesNs = visitStart - budgetStart;
        firstVisit = false; activeVisit = true; attemptStart = 0; ++visited;
        current = {}; current.pair = pair;
        const auto found = owner.admissionQueue.find(pair);
        if (found == owner.admissionQueue.end()) return;
        const auto& entry = found->second;
        current.mesh = BuildPersistentVoxelVariantMeshResourceKey(entry.variant);
        current.material = entry.variant.materialKeyHash;
        current.picnum = entry.variant.sourcePicnum;
        current.voxel = entry.variant.resolvedVoxelIndex;
        current.beforeState = uint32_t(entry.state);
    }
    void VisitEnd(NRIPersistentVoxelResidency& owner)
    {
        if (!activeVisit) return;
        const uint64_t now = PerfAuditClockNs();
        current.elapsed = now - visitStart;
        current.beforeAttempt = attemptStart ? attemptStart - visitStart : current.elapsed;
        current.attempted = attemptStart != 0;
        visitsNs += current.elapsed; beforeAttemptNs += current.beforeAttempt;
        const auto found = owner.admissionQueue.find(current.pair);
        if (found != owner.admissionQueue.end())
        {
            current.afterState = uint32_t(found->second.state);
            current.reason = Safe(found->second.lastReason);
        }
        for (auto& entry : top)
            if (current.elapsed > entry.elapsed) std::swap(entry, current);
        activeVisit = false;
    }
    void Emit(NRIPersistentVoxelResidency& owner, uint32_t frame, const char* phase, const char* stop)
    {
        VisitEnd(owner);
        const uint64_t finish = PerfAuditClockNs();
        Stage(NRIVoxelAdmissionTimingStage::None, finish);
        const uint64_t total = finish - start;
        if (!maintenanceComplete) maintenance = total;
        if (firstVisit && budgetStart) candidatesNs = finish - budgetStart;
        const uint64_t accounted = maintenance + setup + candidatesNs + visitsNs;
        const uint64_t remainder = total > accounted ? total - accounted : 0;
        uint64_t staged = 0;
        for (auto duration : stages) staged += duration;
        const auto stageMs = [&](NRIVoxelAdmissionTimingStage value) { return double(stages[size_t(value)]) / 1e6; };
        char fields[2304];
        std::snprintf(fields, sizeof(fields),
            "\"action\":\"summary\",\"renderer_frame\":%u,\"map_generation\":%u,\"phase\":\"%s\",\"stop\":\"%s\","
            "\"scope_begin\":\"pump-entry\",\"wall_time_includes_waits\":true,\"detail_validator_enabled\":%s,"
            "\"total_ms\":%.6f,\"maintenance_ms\":%.6f,\"budget_setup_ms\":%.6f,\"candidate_preparation_ms\":%.6f,"
            "\"visits_ms\":%.6f,\"visit_before_attempt_ms\":%.6f,\"visit_after_attempt_ms\":%.6f,\"unclassified_ms\":%.6f,"
            "\"candidates\":%u,\"visited\":%u,\"attempted\":%u,\"registry_entries\":%llu,\"active_entries\":%u,\"top_visit_limit\":4,"
            "\"material_retirement_begin_ms\":%.6f,\"material_sharing_ms\":%.6f,\"map_sync_ms\":%.6f,\"enemy_policy_ms\":%.6f,"
            "\"actor_demand_ms\":%.6f,\"reclaim_ms\":%.6f,\"pressure_policy_ms\":%.6f,\"cache_maintenance_ms\":%.6f,\"pre_budget_unclassified_ms\":%.6f",
            frame, owner.residencyMapGeneration, Safe(phase), Safe(stop), Bool((bool)nri_ptvoxelcachevalidate),
            double(total)/1e6, double(maintenance)/1e6, double(setup)/1e6, double(candidatesNs)/1e6,
            double(visitsNs)/1e6, double(beforeAttemptNs)/1e6, double(visitsNs - beforeAttemptNs)/1e6, double(remainder)/1e6,
            candidates, visited, attempted, U64(owner.admissionQueue.size()), owner.admissionIndex.ActiveCount(),
            stageMs(NRIVoxelAdmissionTimingStage::MaterialRetirement), stageMs(NRIVoxelAdmissionTimingStage::MaterialSharing),
            stageMs(NRIVoxelAdmissionTimingStage::MapSync), stageMs(NRIVoxelAdmissionTimingStage::EnemyPolicy),
            stageMs(NRIVoxelAdmissionTimingStage::ActorDemand), stageMs(NRIVoxelAdmissionTimingStage::Reclaim),
            stageMs(NRIVoxelAdmissionTimingStage::PressurePolicy), stageMs(NRIVoxelAdmissionTimingStage::CacheMaintenance),
            double(maintenance > staged ? maintenance - staged : 0) / 1e6);
        PerfAuditWriteEvent("voxel_admission_work_timing", PerfAuditCurrentToken(), fields);
        // A complete aggregate is emitted for every pump. Detailed rows are
        // intentionally a top-four sample only for slow diagnostic pumps.
        if (total < 5000000) return;
        for (const auto& entry : top)
        {
            if (!entry.elapsed) continue;
            std::snprintf(fields, sizeof(fields),
                "\"action\":\"visit\",\"renderer_frame\":%u,\"map_generation\":%u,\"phase\":\"%s\","
                "\"pair_key\":%llu,\"mesh_key\":%llu,\"material_key\":%llu,\"source_picnum\":%d,\"voxel_index\":%d,"
                "\"before_state\":%u,\"after_state\":%u,\"attempted\":%s,\"reason\":\"%s\","
                "\"visit_ms\":%.6f,\"before_attempt_ms\":%.6f,\"after_attempt_ms\":%.6f,\"sample\":\"top-four-slow-pump\"",
                frame, owner.residencyMapGeneration, Safe(phase), U64(entry.pair), U64(entry.mesh), U64(entry.material),
                entry.picnum, entry.voxel, entry.beforeState, entry.afterState, Bool(entry.attempted), Safe(entry.reason),
                double(entry.elapsed)/1e6, double(entry.beforeAttempt)/1e6, double(entry.elapsed-entry.beforeAttempt)/1e6);
            PerfAuditWriteEvent("voxel_admission_work_timing", PerfAuditCurrentToken(), fields);
        }
    }
};

namespace
{
NRIPersistentVoxelAdmissionDiagnosticState* State(NRIPersistentVoxelResidency& owner)
{
    if (!Enabled()) { owner.admissionDiagnosticState.reset(); return nullptr; }
    DiagnosticWork overhead;
    if (!owner.admissionDiagnosticState)
        owner.admissionDiagnosticState = std::make_shared<NRIPersistentVoxelAdmissionDiagnosticState>();
    auto& state = *owner.admissionDiagnosticState;
    const auto token = PerfAuditCurrentToken();
    if (state.epoch != token.epoch || state.map != owner.residencyMapGeneration)
    {
        state = {};
        state.epoch = token.epoch;
        state.map = owner.residencyMapGeneration;
        char fields[256];
        std::snprintf(fields, sizeof(fields), "\"map_generation\":%u,\"reason\":\"audit-or-map-boundary\",\"pair_limit\":65536,\"complete\":true", state.map);
        PerfAuditWriteEvent("voxel_admission_diagnostic_reset", token, fields);
    }
    output.Boundary();
    return &state;
}

NRIPersistentVoxelAdmissionDiagnosticState::Pair* Pair(NRIPersistentVoxelAdmissionDiagnosticState& state,
    const PersistentVoxelAdmissionEntry& entry)
{
    auto found = state.pairs.find(entry.pairKey);
    if (found != state.pairs.end())
    {
        if (found->second.terminal && !Terminal(entry.state))
        {
            found->second = {};
            found->second.firstPresentation = PerfAuditCurrentToken().presentation;
            found->second.episode = ++state.nextEpisode;
            found->second.episodeStart = "reopened-terminal";
        }
        return &found->second;
    }
    // Do not turn all historical resident bindings into diagnostic history.
    if (Terminal(entry.state)) return nullptr;
    if (state.pairs.size() >= PairLimit)
    {
        if (++state.pairOverflow == 1)
            PerfAuditWriteEvent("voxel_admission_diagnostic_overflow", PerfAuditCurrentToken(),
                "\"domain\":\"pair-history\",\"pair_limit\":65536,\"complete\":false");
        return nullptr;
    }
    auto& value = state.pairs[entry.pairKey];
    value.firstPresentation = PerfAuditCurrentToken().presentation;
    value.episode = ++state.nextEpisode;
    return &value;
}

void Observe(NRIPersistentVoxelResidency& owner, NRIPersistentVoxelAdmissionDiagnosticState& state,
    uint64_t key, uint32_t frame, const char* event, int32_t result = -1)
{
    auto found = owner.admissionQueue.find(key);
    if (found == owner.admissionQueue.end()) return;
    const auto& entry = found->second;
    auto* pair = Pair(state, entry);
    if (!pair) return;
    DiagnosticWork overhead;
    pair->terminal = Terminal(entry.state);
    const uint64_t mesh = BuildPersistentVoxelVariantMeshResourceKey(entry.variant);
    const auto ready = owner.GetSharedVariantReadiness(mesh, entry.variant.materialKeyHash);
    const bool materialPresent = owner.materialVariantResources.find(entry.variant.materialKeyHash) != owner.materialVariantResources.end();
    NRIVoxelAdmissionTokenSnapshot scheduler;
    const bool hasToken = entry.schedulerTokenId != 0 && owner.admissionScheduler.GetTokenSnapshot(entry.schedulerTokenId, scheduler);
    const auto snapshot = owner.admissionScheduler.GetSnapshot();
    const uint64_t meshOwner = owner.admissionScheduler.FindMeshOwner({owner.residencyMapGeneration, mesh});
    if (result >= 0) pair->schedulerResult = result;
    result = pair->schedulerResult;
    char signature[768];
    // Position/age/attempt counters are emitted on changes or a 30-frame heartbeat,
    // not on every rank movement. Actual attempt totals remain exact while tracked.
    std::snprintf(signature, sizeof(signature), "%u|%s|%s|%d|%u|%llu|%u|%u|%llu|%llu|%u|%u|%u|%u|%u|%u",
        unsigned(entry.state), Safe(entry.lastReason), Safe(event), result, entry.retryCount,
        U64(entry.schedulerTokenId), hasToken ? unsigned(scheduler.stage) : UINT32_MAX,
        hasToken ? unsigned(scheduler.reservationState) : UINT32_MAX, U64(entry.directComputeGeneration),
        U64(entry.directBlasFenceValue), entry.runtimeRequested, ready.meshPresent, ready.blasReady,
        materialPresent, ready.ready, entry.directComputeRequested);
    if (pair->signature == signature && (frame == UINT32_MAX ||
        (pair->lastFrame != UINT32_MAX && frame >= pair->lastFrame && frame - pair->lastFrame < HeartbeatFrames))) return;
    pair->signature = signature;
    pair->lastFrame = frame;
    char fields[4096];
    std::snprintf(fields, sizeof(fields),
        "\"map_generation\":%u,\"renderer_frame\":%u,\"retry_tick\":%llu,\"allocation_retry_tick\":%llu,\"retry_clock_unit\":\"presentation\","
        "\"pump\":%llu,\"pair_key\":\"%llu\",\"mesh_key\":\"%llu\",\"material_key\":\"%llu\","
        "\"diagnostic_episode\":%llu,\"episode_start_reason\":\"%s\",\"event\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\",\"result_code\":%d,\"rank_position\":%u,\"admission_rank\":%d,\"priority\":%d,"
        "\"first_observed_presentation\":%llu,\"first_attempt_presentation\":%llu,\"last_attempt_presentation\":%llu,\"attempts\":%llu,"
        "\"first_queued_frame\":%u,\"runtime_first_queued_frame\":%u,\"retry_count\":%u,\"runtime_requested\":%s,\"direct_only\":%s,"
        "\"source_picnum\":%d,\"voxel_index\":%d,\"primitive_count\":%u,\"estimated_bytes\":%llu,\"uploaded_bytes\":%llu,"
        "\"mesh_present\":%s,\"blas_ready\":%s,\"material_present\":%s,\"shared_ready\":%s,\"readiness_reason\":\"%s\", "
        "\"scheduler_epoch\":%llu,\"mesh_owner_token\":%llu,\"scheduler_token\":%llu,\"token_found\":%s,\"scheduler_stage\":%u,\"reservation_state\":%u,"
        "\"scheduler_active_jobs\":%u,\"scheduler_compute_inflight\":%u,\"scheduler_blas_inflight\":%u,\"scheduler_held_bytes\":%llu,\"scheduler_retire_pending_bytes\":%llu,"
        "\"direct_generation\":\"%llu\",\"direct_job\":%u,\"direct_request_frame\":%u,\"blas_recorded_frame\":%u,\"blas_fence\":%llu,\"complete\":%s",
        state.map, frame, U64(GetNRIVoxelAdmissionRetryClock()), U64(entry.allocationRetryTick),
        U64(pair->pump), U64(key), U64(mesh), U64(entry.variant.materialKeyHash),
        U64(pair->episode), pair->episodeStart, Safe(event), StateName(entry.state), Safe(entry.lastReason), result, pair->position, entry.admissionRank, entry.priority,
        U64(pair->firstPresentation), U64(pair->firstAttempt), U64(pair->lastAttempt), U64(pair->attempts),
        entry.firstQueuedFrame, entry.runtimeFirstQueuedFrame, entry.retryCount, Bool(entry.runtimeRequested), Bool(entry.variant.directOnlyAdmission),
        entry.variant.sourcePicnum, entry.variant.resolvedVoxelIndex, entry.variant.primitiveCount, U64(entry.estimatedBytes), U64(entry.bytesUploaded),
        Bool(ready.meshPresent), Bool(ready.blasReady), Bool(materialPresent), Bool(ready.ready), Safe(ready.reason),
        U64(owner.schedulerRetirementEpoch), U64(meshOwner), U64(entry.schedulerTokenId), Bool(hasToken), hasToken ? unsigned(scheduler.stage) : UINT32_MAX,
        hasToken ? unsigned(scheduler.reservationState) : UINT32_MAX, snapshot.activeJobs, snapshot.computeInFlight, snapshot.blasInFlight,
        U64(snapshot.heldReservationBytes), U64(snapshot.retirePendingBytes), U64(entry.directComputeGeneration), entry.directComputeJobId,
        entry.directComputeRequestFrame, entry.directBlasRecordedFrame, U64(entry.directBlasFenceValue), Bool(state.pairOverflow == 0 && output.suppressedTotal == 0));
    output.Emit("voxel_admission_entry", fields);
}
}

NRIVoxelAdmissionDiagnosticScope::NRIVoxelAdmissionDiagnosticScope(NRIPersistentVoxelResidency& value,
    uint32_t valueFrame, const char* valuePhase) : owner(value), frame(valueFrame), phase(Safe(valuePhase))
{
    if ((bool)nri_ptvoxeladmissiontiming && PerfAuditActive()) timing = std::make_unique<NRIVoxelAdmissionTimingState>();
}
void NRIVoxelAdmissionDiagnosticScope::AfterMapSync()
{
    TimingStage(NRIVoxelAdmissionTimingStage::None);
    // Keep correctness history attached to the newly synchronized map, as
    // before the timing observer moved to the entry of the enclosing pump.
    state = State(owner);
    if (state)
    {
        stateOwner = owner.admissionDiagnosticState;
        pump = ++state->nextPump;
    }
}
void NRIVoxelAdmissionDiagnosticScope::BeginBudget()
{
    if (timing)
    {
        timing->budgetStart = PerfAuditClockNs();
        timing->setup = timing->budgetStart - timing->start - timing->maintenance;
    }
    if (!state) return;
    budgetObserverStart = diagnosticWorkNs;
    budgetWallStart = PerfAuditClockNs();
}
void NRIVoxelAdmissionDiagnosticScope::TimingStage(NRIVoxelAdmissionTimingStage stage)
{
    if (timing) timing->Stage(stage, PerfAuditClockNs());
}
void NRIVoxelAdmissionDiagnosticScope::CandidateCount(uint32_t count)
{
    if (timing) timing->candidates = count;
}
double NRIVoxelAdmissionDiagnosticScope::ElapsedMs(double rawElapsedMs) const
{
    if (!state || !budgetWallStart) return rawElapsedMs;
    return std::max(0.0, rawElapsedMs - double(diagnosticWorkNs - budgetObserverStart) / 1e6);
}
void NRIVoxelAdmissionDiagnosticScope::Budgets(uint32_t variants, uint64_t bytes, uint32_t blas, double ms)
{
    if (timing) timing->EndMaintenance();
    variantBudget = variants; byteBudget = bytes; blasBudget = blas; msBudget = ms;
}
void NRIVoxelAdmissionDiagnosticScope::Candidate(uint64_t key, uint32_t position)
{
    if (!state) return;
    DiagnosticWork overhead;
    const auto found = owner.admissionQueue.find(key);
    if (found == owner.admissionQueue.end()) return;
    if (auto* pair = Pair(*state, found->second))
    {
        pair->pump = pump; pair->position = position;
        candidates.push_back(key);
    }
}
void NRIVoxelAdmissionDiagnosticScope::Visit(uint64_t key)
{
    if (timing) timing->VisitBegin(owner, key);
    if (!state) return;
    DiagnosticWork overhead;
    currentPair = key; disposition = "visited"; ++visited;
    auto found = state->pairs.find(key);
    if (found != state->pairs.end()) found->second.visitedPump = pump;
}
void NRIVoxelAdmissionDiagnosticScope::Transition(uint64_t key, const char* event)
{
    if (!state) return;
    DiagnosticWork overhead;
    const auto found = owner.admissionQueue.find(key);
    if (found == owner.admissionQueue.end()) return;
    auto* pair = Pair(*state, found->second);
    if (!pair) return;
    // Pre-sort readiness transitions belong to this pump, but have no current
    // candidate rank. Never carry the previous pump/rank into their evidence.
    pair->pump = pump;
    pair->position = UINT32_MAX;
    Observe(owner, *state, key, frame, event);
}
void NRIVoxelAdmissionDiagnosticScope::Attempt()
{
    if (timing) { timing->attemptStart = PerfAuditClockNs(); ++timing->attempted; }
    if (!state) return;
    DiagnosticWork overhead;
    ++attempted;
    auto found = state->pairs.find(currentPair);
    if (found == state->pairs.end()) return;
    auto& pair = found->second;
    pair.lastAttempt = PerfAuditCurrentToken().presentation;
    if (pair.firstAttempt == UINT64_MAX) pair.firstAttempt = pair.lastAttempt;
    ++pair.attempts;
}
void NRIVoxelAdmissionDiagnosticScope::EndVisit()
{
    if (timing) timing->VisitEnd(owner);
    if (!state || !currentPair) return;
    Observe(owner, *state, currentPair, frame, disposition);
    lastVisitedPair = currentPair; lastDisposition = disposition;
    currentPair = 0;
}
void NRIVoxelAdmissionDiagnosticScope::Finish(const char* valueStop, uint32_t valueAdmitted, uint64_t bytes, uint32_t blas)
{
    stop = Safe(valueStop); admitted = valueAdmitted; bytesUsed = bytes; blasUsed = blas;
    if (state && budgetWallStart)
    {
        budgetRawNs = PerfAuditClockNs() - budgetWallStart;
        budgetExcludedNs = diagnosticWorkNs - budgetObserverStart;
    }
}
NRIVoxelAdmissionDiagnosticScope::~NRIVoxelAdmissionDiagnosticScope()
{
    if (timing) timing->Emit(owner, frame, phase, stop);
    if (!state) return;
    EndVisit();
    for (uint64_t key : candidates)
    {
        const auto found = state->pairs.find(key);
        if (found != state->pairs.end() && found->second.visitedPump != pump)
            Observe(owner, *state, key, frame, "not-visited-after-stop");
    }
    DiagnosticWork overhead;
    const auto scheduler = owner.admissionScheduler.GetSnapshot();
    char fields[2048];
    std::snprintf(fields, sizeof(fields),
        "\"map_generation\":%u,\"renderer_frame\":%u,\"retry_tick\":%llu,\"retry_clock_unit\":\"presentation\",\"pump\":%llu,\"phase\":\"%s\",\"stop\":\"%s\","
        "\"registry_entries\":%llu,\"active_entries\":%u,\"required_ready_history\":%u,\"optional_ready_history\":%u,\"failed_history\":%u,"
        "\"last_visited_pair\":\"%llu\",\"last_disposition\":\"%s\",\"tracked_candidates\":%llu,\"visited\":%u,\"attempted\":%u,\"admitted\":%u,\"variants_budget\":%u,\"bytes_budget\":%llu,\"bytes_used\":%llu,\"blas_budget\":%u,\"blas_used\":%u,\"ms_budget\":%.3f,"
        "\"scheduler_active_jobs\":%u,\"scheduler_compute_inflight\":%u,\"scheduler_blas_inflight\":%u,\"scheduler_held_bytes\":%llu,\"scheduler_retire_pending_bytes\":%llu,"
        "\"budget_raw_ns\":%llu,\"budget_observer_excluded_ns\":%llu,\"budget_scheduling_ns\":%llu,\"pair_history\":%llu,\"pair_overflow\":%llu,\"detail_suppressed_total\":%llu,\"complete\":%s",
        state->map, frame, U64(GetNRIVoxelAdmissionRetryClock()), U64(pump), phase, stop, U64(owner.admissionQueue.size()), owner.admissionIndex.ActiveCount(),
        owner.admissionIndex.RequiredReadyCount(), owner.admissionIndex.OptionalReadyCount(), owner.admissionIndex.FailedCount(),
        U64(lastVisitedPair), lastDisposition, U64(candidates.size()), visited, attempted, admitted, variantBudget, U64(byteBudget), U64(bytesUsed), blasBudget, blasUsed, msBudget,
        scheduler.activeJobs, scheduler.computeInFlight, scheduler.blasInFlight, U64(scheduler.heldReservationBytes), U64(scheduler.retirePendingBytes),
        U64(budgetRawNs), U64(budgetExcludedNs), U64(budgetRawNs > budgetExcludedNs ? budgetRawNs - budgetExcludedNs : 0), U64(state->pairs.size()), U64(state->pairOverflow), U64(output.suppressedTotal), Bool(state->pairOverflow == 0 && output.suppressedTotal == 0));
    // One uncapped summary per pump keeps output-loss evidence observable.
    PerfAuditWriteEvent("voxel_admission_pump", PerfAuditCurrentToken(), fields);
}
void NoteNRIVoxelAdmissionDiagnostic(NRIPersistentVoxelResidency& owner, uint64_t pair, const char* event, int32_t result)
{
    if (!Enabled()) { owner.admissionDiagnosticState.reset(); return; }
    DiagnosticWork overhead;
    auto* state = State(owner);
    if (!state) return;
    const auto entry = owner.admissionQueue.find(pair);
    if (entry == owner.admissionQueue.end()) return;
    auto* history = Pair(*state, entry->second);
    if (!history) return;
    if (std::strcmp(event, "enqueue-new") == 0)
    {
        *history = {};
        history->episode = ++state->nextEpisode;
        history->firstPresentation = PerfAuditCurrentToken().presentation;
        history->episodeStart = "enqueue-new";
    }
    if (result >= 0 && history->schedulerResult == result) return;
    Observe(owner, *state, pair, UINT32_MAX, event, result);
}
void NoteNRIVoxelAdmissionComputeDiagnostic(uint64_t mesh, uint64_t generation,
    const char* event, uint32_t job, uint64_t frame, uint64_t fence)
{
    if (!Enabled()) return;
    DiagnosticWork overhead;
    char fields[512];
    std::snprintf(fields, sizeof(fields), "\"mesh_key\":\"%llu\",\"direct_generation\":\"%llu\",\"event\":\"%s\",\"job\":%u,\"renderer_frame\":%llu,\"frame_semantics\":\"%s\",\"fence\":%llu",
        U64(mesh), U64(generation), Safe(event), job, U64(frame),
        frame == UINT64_MAX ? "unavailable" : (std::strcmp(event, "attached-inflight") == 0 ? "original-dispatch" :
            (std::strcmp(event, "dispatch-recorded") == 0 ? "dispatch-recorded" :
                (std::strcmp(event, "ring-submit-failed") == 0 ? "dispatch-attempt" : "completion-observed"))), U64(fence));
    output.Emit("voxel_admission_compute", fields);
}

void NoteNRIVoxelAdmissionRejectedDiagnostic(uint64_t mesh, uint64_t material, uint32_t map, const char* reason)
{
    if (!Enabled()) return;
    DiagnosticWork overhead;
    char fields[512];
    std::snprintf(fields, sizeof(fields), "\"map_generation\":%u,\"mesh_key\":\"%llu\",\"material_key\":\"%llu\",\"reason\":\"%s\"",
        map, U64(mesh), U64(material), Safe(reason));
    output.Emit("voxel_admission_request_rejected", fields);
}

void NoteNRIVoxelDirectBlasFence(const PersistentVoxelAdmissionEntry& entry,
    const PersistentVoxelMeshVariantResource& mesh, uint32_t frame, const char* event)
{
    if (!entry.directBlasExclusive || !entry.directBlasFenceValue ||
        !(bool)nri_ptvoxeladmissiontiming || !PerfAuditActive()) return;
    DiagnosticWork overhead;
    char fields[640];
    std::snprintf(fields, sizeof(fields),
        "\"event\":\"%s\",\"renderer_frame\":%u,\"submit_frame\":%u,\"map_generation\":%u,"
        "\"mesh_key\":%llu,\"pair_key\":%llu,\"direct_generation\":%llu,\"scheduler_token\":%llu,"
        "\"range_transaction\":%llu,\"fence\":%llu,\"runtime_requested\":%s,\"primitives\":%u,"
        "\"as_bytes\":%llu,\"private_scratch_bytes\":%llu",
        event, frame, entry.directBlasRecordedFrame, entry.mapGeneration, U64(mesh.resourceKey),
        U64(entry.pairKey), U64(mesh.directComputeGeneration), U64(entry.schedulerTokenId),
        U64(mesh.geometryRange.transaction), U64(entry.directBlasFenceValue), Bool(entry.runtimeRequested),
        mesh.primitiveCount, U64(mesh.accelerationStructure.memorySize), U64(entry.directBlasScratchBuffer.memorySize));
    output.Emit("voxel_direct_blas_fence", fields);
}
