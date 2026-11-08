#include "nri_persistent_voxels.h"
#include "nri_persistent_voxel_admission_diagnostics.h"
#include "nri_cvars.h"
#include "perf_audit.h"

#include <algorithm>
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
        "\"map_generation\":%u,\"renderer_frame\":%u,\"pump\":%llu,\"pair_key\":\"%llu\",\"mesh_key\":\"%llu\",\"material_key\":\"%llu\","
        "\"diagnostic_episode\":%llu,\"episode_start_reason\":\"%s\",\"event\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\",\"result_code\":%d,\"rank_position\":%u,\"admission_rank\":%d,\"priority\":%d,"
        "\"first_observed_presentation\":%llu,\"first_attempt_presentation\":%llu,\"last_attempt_presentation\":%llu,\"attempts\":%llu,"
        "\"first_queued_frame\":%u,\"runtime_first_queued_frame\":%u,\"retry_count\":%u,\"runtime_requested\":%s,\"direct_only\":%s,"
        "\"source_picnum\":%d,\"voxel_index\":%d,\"primitive_count\":%u,\"estimated_bytes\":%llu,\"uploaded_bytes\":%llu,"
        "\"mesh_present\":%s,\"blas_ready\":%s,\"material_present\":%s,\"shared_ready\":%s,\"readiness_reason\":\"%s\", "
        "\"scheduler_epoch\":%llu,\"mesh_owner_token\":%llu,\"scheduler_token\":%llu,\"token_found\":%s,\"scheduler_stage\":%u,\"reservation_state\":%u,"
        "\"scheduler_active_jobs\":%u,\"scheduler_compute_inflight\":%u,\"scheduler_blas_inflight\":%u,\"scheduler_held_bytes\":%llu,\"scheduler_retire_pending_bytes\":%llu,"
        "\"direct_generation\":\"%llu\",\"direct_job\":%u,\"direct_request_frame\":%u,\"blas_recorded_frame\":%u,\"blas_fence\":%llu,\"complete\":%s",
        state.map, frame, U64(pair->pump), U64(key), U64(mesh), U64(entry.variant.materialKeyHash),
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
    state = State(owner);
    if (state)
    {
        stateOwner = owner.admissionDiagnosticState;
        pump = ++state->nextPump;
    }
}
void NRIVoxelAdmissionDiagnosticScope::BeginBudget()
{
    if (!state) return;
    budgetObserverStart = diagnosticWorkNs;
    budgetWallStart = PerfAuditClockNs();
}
double NRIVoxelAdmissionDiagnosticScope::ElapsedMs(double rawElapsedMs) const
{
    if (!state || !budgetWallStart) return rawElapsedMs;
    return std::max(0.0, rawElapsedMs - double(diagnosticWorkNs - budgetObserverStart) / 1e6);
}
void NRIVoxelAdmissionDiagnosticScope::Budgets(uint32_t variants, uint64_t bytes, uint32_t blas, double ms)
{
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
    if (!state) return;
    DiagnosticWork overhead;
    currentPair = key; disposition = "visited"; ++visited;
    auto found = state->pairs.find(key);
    if (found != state->pairs.end()) found->second.visitedPump = pump;
}
void NRIVoxelAdmissionDiagnosticScope::Transition(uint64_t pair, const char* event)
{
    if (state) Observe(owner, *state, pair, frame, event);
}
void NRIVoxelAdmissionDiagnosticScope::Attempt()
{
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
        "\"map_generation\":%u,\"renderer_frame\":%u,\"pump\":%llu,\"phase\":\"%s\",\"stop\":\"%s\","
        "\"registry_entries\":%llu,\"active_entries\":%u,\"required_ready_history\":%u,\"optional_ready_history\":%u,\"failed_history\":%u,"
        "\"last_visited_pair\":\"%llu\",\"last_disposition\":\"%s\",\"tracked_candidates\":%llu,\"visited\":%u,\"attempted\":%u,\"admitted\":%u,\"variants_budget\":%u,\"bytes_budget\":%llu,\"bytes_used\":%llu,\"blas_budget\":%u,\"blas_used\":%u,\"ms_budget\":%.3f,"
        "\"scheduler_active_jobs\":%u,\"scheduler_compute_inflight\":%u,\"scheduler_blas_inflight\":%u,\"scheduler_held_bytes\":%llu,\"scheduler_retire_pending_bytes\":%llu,"
        "\"budget_raw_ns\":%llu,\"budget_observer_excluded_ns\":%llu,\"budget_scheduling_ns\":%llu,\"pair_history\":%llu,\"pair_overflow\":%llu,\"detail_suppressed_total\":%llu,\"complete\":%s",
        state->map, frame, U64(pump), phase, stop, U64(owner.admissionQueue.size()), owner.admissionIndex.ActiveCount(),
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
