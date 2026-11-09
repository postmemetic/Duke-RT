#include "nri_persistent_voxels.h"
#include "../scene/nri_voxel_publication_diagnostic_status.h"
#include "nri_cvars.h"
#include "perf_audit.h"
#include "actor_presentation_snapshot.h"
#include "coreactor.h"
#include "printf.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
constexpr size_t ActorLimit = 32768, MeshLimit = 8192;
constexpr uint32_t EventLimit = 1024;
const char* Bool(bool value) { return value ? "true" : "false"; }
using U64 = unsigned long long;
std::string JsonName(const char* text)
{
    std::string result;
    for (size_t i = 0; text && text[i] && i < 192; ++i)
    {
        const unsigned char c = text[i];
        if (c == '"' || c == '\\') result += '\\';
        result += c >= 32 && c < 127 ? char(c) : '?';
    }
    return result;
}
}

struct NRIPersistentVoxelCacheDiagnosticState
{
    struct Actor
    {
        uint64_t world = 0, lifetime = 0, desired = 0, material = 0, published = 0;
        uint64_t pendingPresentation = 0, lastObservedPresentation = 0;
        uint32_t pendingFrame = 0;
        int32_t actorIndex = -1, picnum = -1;
        bool everPublished = false, sourcePresent = false, captured = false;
        bool pending = false, pendingPreviouslyActive = false;
        std::string outcome, reason, className;
    };
    struct Previous { uint64_t world, lifetime, mesh; };
    struct Eviction
    {
        PerfAuditToken token;
        uint32_t frame = 0, map = 0;
        bool readyObserved = false;
    };
    std::unordered_map<uint64_t, Actor> actors;
    std::unordered_map<uint64_t, Previous> previous;
    std::unordered_map<uint64_t, const char*> reasons;
    std::unordered_map<uint64_t, Eviction> evictions;
    uint64_t world = 0, outputEpoch = 0, outputPresentation = UINT64_MAX;
    uint32_t emitted = 0, suppressed = 0, actorOverflow = 0, meshOverflow = 0;
    uint32_t previousOverflow = 0, reasonOverflow = 0;
    bool begun = false;

    void Boundary()
    {
        const auto token = PerfAuditCurrentToken();
        if (token.epoch == outputEpoch && token.presentation == outputPresentation) return;
        outputEpoch = token.epoch;
        outputPresentation = token.presentation;
        emitted = suppressed = 0;
    }
    void Emit(const char* kind, const char* fields)
    {
        Boundary();
        if (emitted < EventLimit) { ++emitted; PerfAuditWriteEvent(kind, PerfAuditCurrentToken(), fields); }
        else if (++suppressed == 1)
            PerfAuditWriteEvent("voxel_cache_diagnostic_overflow", PerfAuditCurrentToken(),
                "\"domain\":\"detail-events\",\"event_limit\":1024,\"complete\":false");
    }
};

NRIVoxelCacheDiagnosticScope::NRIVoxelCacheDiagnosticScope(NRIPersistentVoxelResidency& value,
    uint32_t valueFrame, const NRIPersistentVoxelSettings& valueSettings)
    : owner(value), settings(valueSettings), frame(valueFrame) { owner.BeginCacheDiagnostics(frame); }
NRIVoxelCacheDiagnosticScope::~NRIVoxelCacheDiagnosticScope() { owner.EndCacheDiagnostics(frame, settings); }

void NRIPersistentVoxelResidency::BeginCacheDiagnostics(uint32_t)
{
    if (!(bool)nri_ptvoxelcachevalidate || !PerfAuditActive()) { cacheDiagnosticState.reset(); return; }
    if (!cacheDiagnosticState) cacheDiagnosticState = std::make_shared<NRIPersistentVoxelCacheDiagnosticState>();
    auto& state = *cacheDiagnosticState;
    state.Boundary();
    const auto world = GetActorPresentationSnapshot().worldEpoch;
    if (state.world != world)
    {
        state.world = world;
        state.actors.clear();
        state.actorOverflow = 0;
        state.previousOverflow = state.reasonOverflow = 0;
    }
    state.previous.clear();
    state.reasons.clear();
    state.begun = true;
    for (const auto& actor : batch.actors)
        if (actor.active)
        {
            if (state.previous.count(actor.identityKey) || state.previous.size() < ActorLimit)
                state.previous.emplace(actor.identityKey, NRIPersistentVoxelCacheDiagnosticState::Previous{
                    actor.ownerWorldEpoch, actor.ownerLifetimeGeneration, actor.meshResourceKey });
            else ++state.previousOverflow;
        }
}

void NRIPersistentVoxelResidency::NoteCacheActorDeferred(uint64_t identity, const char* reason)
{
    if (!(bool)nri_ptvoxelcachevalidate || !PerfAuditActive() || !cacheDiagnosticState) return;
    auto& reasons = cacheDiagnosticState->reasons;
    if (reasons.count(identity) || reasons.size() < ActorLimit) reasons[identity] = reason;
    else ++cacheDiagnosticState->reasonOverflow;
}

void NRIPersistentVoxelResidency::NoteCacheMeshEviction(uint64_t key, uint32_t frameIndex,
    const char* cause, bool sourceOrPendingPinned)
{
    if (!(bool)nri_ptvoxelcachevalidate || !PerfAuditActive()) return;
    if (!cacheDiagnosticState) cacheDiagnosticState = std::make_shared<NRIPersistentVoxelCacheDiagnosticState>();
    auto& state = *cacheDiagnosticState;
    uint32_t activeActors = 0;
    for (const auto& actor : batch.actors) activeActors += actor.active && actor.meshResourceKey == key;
    const auto refs = activeMeshReferences.find(key);
    const uint32_t activeRefs = refs == activeMeshReferences.end() ? 0 : refs->second;
    const bool violation = activeActors || activeRefs || sourceOrPendingPinned;
    if (violation)
    {
        PerfAuditAdd(PerfAuditMetric::VoxelCacheInvariantMismatches);
        Printf("NRI PT voxel cache invariant mismatch: diagnostic pin key=%llu frame=%u\n", U64(key), frameIndex);
    }
    const auto found = meshVariantResources.find(key);
    const uint64_t transaction = found == meshVariantResources.end() ? 0 : found->second.geometryRange.transaction;
    bool tracked = state.evictions.count(key) || state.evictions.size() < MeshLimit;
    if (tracked) state.evictions[key] = { PerfAuditCurrentToken(), frameIndex, residencyMapGeneration, false };
    else ++state.meshOverflow;
    char fields[1024];
    std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"map_generation\":%u,\"mesh_key\":\"%llu\","
        "\"cause\":\"%s\",\"geometry_transaction\":%llu,\"active_actor_refs\":%u,\"active_resource_refs\":%u,"
        "\"source_or_pending_pinned\":%s,\"pin_violation\":%s,\"history_tracked\":%s",
        frameIndex, residencyMapGeneration, U64(key), cause, U64(transaction), activeActors, activeRefs,
        Bool(sourceOrPendingPinned), Bool(violation), Bool(tracked));
    state.Emit("voxel_cache_mesh_evict", fields);
}

void NRIPersistentVoxelResidency::EndCacheDiagnostics(uint32_t frameIndex, const NRIPersistentVoxelSettings& settings)
{
    if (!(bool)nri_ptvoxelcachevalidate || !PerfAuditActive() || !cacheDiagnosticState || !cacheDiagnosticState->begun) return;
    PerfAuditScope overhead(PerfAuditMetric::AuditCensusNs);
    auto& state = *cacheDiagnosticState;
    state.begun = false;
    const auto token = PerfAuditCurrentToken();
    std::vector<nri_scene::PersistentVoxelCacheEntryView> source;
    std::string emptySourceEvidence;
    if (!nri_scene::BuildPersistentVoxelCacheEntries(source))
    {
        const auto status = nri_scene::InspectPersistentVoxelEmptyPublication();
        using Filter = nri_scene::VoxelPublicationFilter;
        const auto& captured = status.completedCapture;
        char evidence[3072];
        std::snprintf(evidence, sizeof(evidence),
            "\"source_status\":{\"schema\":1,\"scope\":\"eligible-discovered-publication-set\","
            "\"classification\":\"%s\",\"provider_current\":%s,\"capture_current\":%s,\"counts_match\":%s,"
            "\"startup_suppressed\":%s,\"snapshot_complete\":%s,\"snapshot_generation\":%llu,"
            "\"snapshot_world\":%llu,\"journal_world\":%llu,\"snapshot_simulation\":%llu,\"current_simulation\":%llu,"
            "\"snapshot_lifecycle\":%llu,\"journal_serial\":%llu,\"snapshot_actors\":%llu,"
            "\"audit_epoch\":%llu,\"cache_frame\":%llu,\"cache_serial\":%llu,\"capture_depth\":%u,"
            "\"capture_observed\":%s,\"capture_audit_epoch\":%llu,\"capture_frame\":%llu,\"capture_serial\":%llu,"
            "\"capture_snapshot_generation\":%llu,\"capture_world\":%llu,\"capture_simulation\":%llu,"
            "\"capture_lifecycle\":%llu,\"capture_journal_serial\":%llu,"
            "\"cache_entries\":%llu,\"eligible_entries\":%llu,\"filter_accepted\":%llu,\"filter_startup\":%llu,"
            "\"filter_invalid_placement\":%llu,\"filter_authority_not_current\":%llu,\"filter_publication_ineligible\":%llu,"
            "\"filter_pending_removal\":%llu,\"filter_representation_unready\":%llu,\"filter_shared_surface_unavailable\":%llu}",
            status.Classification(), Bool(status.ProviderCurrent()), Bool(status.CaptureCurrent()), Bool(status.CountsMatch()),
            Bool(status.startupSuppressed), Bool(status.snapshotComplete), U64(status.snapshotGeneration),
            U64(status.snapshotWorld), U64(status.journalWorld), U64(status.snapshotSimulation), U64(status.currentSimulation),
            U64(status.snapshotLifecycle), U64(status.journalSerial), U64(status.snapshotActors),
            U64(status.auditEpoch), U64(status.cacheFrame), U64(status.cacheSerial), status.captureDepth,
            Bool(captured.observed), U64(captured.auditEpoch), U64(captured.cacheFrame), U64(captured.cacheSerial),
            U64(captured.snapshotGeneration), U64(captured.worldEpoch), U64(captured.simulationGeneration),
            U64(captured.lifecycleSerial), U64(captured.journalSerial), U64(status.cacheEntries), U64(status.eligibleEntries),
            U64(status.Filter(Filter::Accepted)), U64(status.Filter(Filter::StartupSuppressed)),
            U64(status.Filter(Filter::InvalidPlacement)), U64(status.Filter(Filter::AuthorityNotCurrent)),
            U64(status.Filter(Filter::PublicationIneligible)), U64(status.Filter(Filter::PendingRemoval)),
            U64(status.Filter(Filter::RepresentationUnready)), U64(status.Filter(Filter::SharedSurfaceUnavailable)));
        emptySourceEvidence = evidence;
        if (!status.ObservationAvailable())
        {
            char unavailable[3584];
            std::snprintf(unavailable, sizeof(unavailable),
                "\"authority_available\":false,\"stage\":\"batch-publication\",\"complete\":false,"
                "\"renderer_frame\":%u,\"map_generation\":%u,%s", frameIndex, residencyMapGeneration, evidence);
            PerfAuditWriteEvent("voxel_cache_actor_summary", token, unavailable);
            return; // Unavailable/suppressed/unready authority is never empty/dead actor evidence.
        }
        // Only a proven current empty eligible set proceeds through the same
        // history reconciliation; no fabricated successful publication.
    }
    std::unordered_map<uint64_t, const PersistentVoxelBatch::ActorEntry*> published;
    for (const auto& actor : batch.actors) if (actor.active) published[actor.identityKey] = &actor;
    // Diagnostic-only transient lookup; never retain an actor pointer.
    std::unordered_map<int32_t, std::string> classNames;
    if (!source.empty())
    {
        TSpriteIterator<DCoreActor> iterator;
        while (auto actor = iterator.Next())
            classNames.emplace(actor->GetIndex(), JsonName(actor->GetClass()->TypeName.GetChars()));
    }
    std::unordered_set<uint64_t> observed;
    uint32_t readyCount = 0, heldCount = 0, omittedCaptured = 0, omittedRetained = 0;
    uint32_t omittedPreviouslyActive = 0, transitions = 0, pendingMaxFrames = 0;
    char fields[4096];
    for (const auto& entry : source)
    {
        observed.insert(entry.identityKey);
        auto found = state.actors.find(entry.identityKey);
        if (found == state.actors.end() && state.actors.size() >= ActorLimit) { ++state.actorOverflow; continue; }
        auto& history = state.actors[entry.identityKey];
        if (history.world != entry.ownerWorldEpoch || history.lifetime != entry.ownerLifetimeGeneration)
            history = {};
        const uint64_t desired = BuildPersistentVoxelMeshResourceKey(entry, settings);
        const auto current = published.find(entry.identityKey);
        const auto* actor = current == published.end() ? nullptr : current->second;
        const bool active = actor && actor->ownerWorldEpoch == entry.ownerWorldEpoch &&
            actor->ownerLifetimeGeneration == entry.ownerLifetimeGeneration;
        const auto old = state.previous.find(entry.identityKey);
        const bool previouslyActive = old != state.previous.end() && old->second.world == entry.ownerWorldEpoch &&
            old->second.lifetime == entry.ownerLifetimeGeneration;
        const auto instance = instances.find(entry.identityKey);
        const bool instancePending = instance != instances.end() && instance->second.pending;
        const bool held = active && (actor->meshResourceKey != desired || actor->materialKeyHash != entry.materialKeyHash || instancePending);
        const char* outcome = !active ? "omitted" : held ? "held" : "published";
        const bool pending = !active || held;
        const auto readiness = GetSharedVariantReadiness(desired, entry.materialKeyHash);
        const auto reason = state.reasons.find(entry.identityKey);
        const char* why = reason != state.reasons.end() ? reason->second : pending ? readiness.reason : "ready";
        const bool changed = history.outcome != outcome || history.desired != desired || history.material != entry.materialKeyHash ||
            history.reason != why || (pending && history.captured != entry.capturedThisFrame);
        const bool pendingStarted = pending && (!history.pending || history.desired != desired || history.material != entry.materialKeyHash);
        if (pendingStarted)
        {
            history.pendingFrame = frameIndex;
            history.pendingPresentation = token.presentation;
            history.pendingPreviouslyActive = previouslyActive;
        }
        const uint32_t age = (pending || history.pending) && frameIndex >= history.pendingFrame ? frameIndex - history.pendingFrame : 0;
        if (pending) pendingMaxFrames = std::max(pendingMaxFrames, age);
        if (held) ++heldCount;
        else if (active) ++readyCount;
        else { entry.capturedThisFrame ? ++omittedCaptured : ++omittedRetained; omittedPreviouslyActive += previouslyActive; }
        const auto evicted = state.evictions.find(desired);
        const bool knownEviction = evicted != state.evictions.end();
        if (knownEviction && readiness.blasReady && !evicted->second.readyObserved)
        {
            auto& eviction = evicted->second;
            std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"mesh_key\":\"%llu\",\"map_generation\":%u,"
                "\"eviction_frame\":%u,\"eviction_map_generation\":%u,\"eviction_epoch\":%llu,\"eviction_presentation\":%llu,"
                "\"observation\":\"first-desired-ready-observation\"", frameIndex, U64(desired), residencyMapGeneration,
                eviction.frame, eviction.map, U64(eviction.token.epoch), U64(eviction.token.presentation));
            state.Emit("voxel_cache_mesh_ready", fields);
            eviction.readyObserved = true;
        }
        // Ordinary first-use success is intentionally silent. Pending starts,
        // reason/key changes, resolves and source disappearance are transitions.
        if (changed && (pending || history.pending || (!history.sourcePresent && history.everPublished)))
        {
            ++transitions;
            const auto name = classNames.find(entry.actorIndex);
            const char* className = name == classNames.end() ? "unknown" : name->second.c_str();
            std::snprintf(fields, sizeof(fields), "\"stage\":\"batch-publication\",\"renderer_frame\":%u,\"map_generation\":%u,"
                "\"identity\":\"%llu\",\"world_epoch\":%llu,\"lifetime\":%llu,\"actor_index\":%d,\"class\":\"%s\",\"picnum\":%d,"
                "\"desired_mesh\":\"%llu\",\"desired_material\":\"%llu\",\"published_mesh\":\"%llu\",\"outcome\":\"%s\",\"reason\":\"%s\","
                "\"captured_this_frame\":%s,\"authority_current\":%s,\"publication_eligible\":%s,\"desired_pending\":%s,\"direct_only_admission\":%s,"
                "\"previously_active_at_batch_begin\":%s,\"ever_published_in_diagnostic_history\":%s,\"pending_started_previously_active\":%s,"
                "\"pending_start_frame\":%u,\"pending_start_presentation\":%llu,\"pending_age_frames\":%u,\"prior_pending\":%s,"
                "\"mesh_present\":%s,\"blas_ready\":%s,\"material_present\":%s,\"shared_ready\":%s,\"known_mesh_eviction\":%s,"
                "\"eviction_frame\":%u,\"eviction_map_generation\":%u,\"eviction_epoch\":%llu,\"eviction_presentation\":%llu",
                frameIndex, residencyMapGeneration, U64(entry.identityKey), U64(entry.ownerWorldEpoch), U64(entry.ownerLifetimeGeneration), entry.actorIndex, className,
                entry.sourcePicnum, U64(desired), U64(entry.materialKeyHash), U64(active ? actor->meshResourceKey : 0), outcome, why,
                Bool(entry.capturedThisFrame), Bool(entry.authorityCurrent), Bool(entry.publicationEligible), Bool(entry.desiredPending), Bool(entry.directOnlyAdmission),
                Bool(previouslyActive), Bool(history.everPublished), Bool(history.pendingPreviouslyActive), history.pendingFrame, U64(history.pendingPresentation), age,
                Bool(history.pending), Bool(readiness.meshPresent), Bool(readiness.blasReady), Bool(readiness.materialPresent), Bool(readiness.ready), Bool(knownEviction),
                knownEviction ? evicted->second.frame : 0, knownEviction ? evicted->second.map : 0,
                U64(knownEviction ? evicted->second.token.epoch : 0), U64(knownEviction ? evicted->second.token.presentation : 0));
            state.Emit("voxel_cache_actor_state", fields);
        }
        history.world = entry.ownerWorldEpoch; history.lifetime = entry.ownerLifetimeGeneration;
        history.desired = desired; history.material = entry.materialKeyHash; history.published = active ? actor->meshResourceKey : 0;
        history.outcome = outcome; history.reason = why; history.pending = pending; history.sourcePresent = true;
        history.captured = entry.capturedThisFrame; history.everPublished |= active; history.actorIndex = entry.actorIndex;
        history.picnum = entry.sourcePicnum; history.lastObservedPresentation = token.presentation;
        const auto name = classNames.find(entry.actorIndex);
        if (name != classNames.end()) history.className = name->second;
    }
    for (auto& pair : state.actors)
    {
        auto& history = pair.second;
        if (!history.sourcePresent || observed.count(pair.first)) continue;
        const auto* live = LookupActorPresentationByOwnerKey({ history.world, int64_t(history.lifetime) });
        if (history.pending || history.published)
        {
            ++transitions;
            std::snprintf(fields, sizeof(fields), "\"stage\":\"batch-publication\",\"renderer_frame\":%u,\"identity\":\"%llu\","
                "\"world_epoch\":%llu,\"lifetime\":%llu,\"actor_index\":%d,\"class\":\"%s\",\"picnum\":%d,"
                "\"desired_mesh\":\"%llu\",\"outcome\":\"source-absent\",\"prior_pending\":%s,\"previous_published_mesh\":\"%llu\",\"pending_start_frame\":%u,"
                "\"pending_age_frames\":%u,\"pending_start_presentation\":%llu,\"live_presentation_owner\":%s,\"live_presentation_eligible\":%s,"
                "\"resolution\":\"not-proven-published\"", frameIndex, U64(pair.first), U64(history.world), U64(history.lifetime), history.actorIndex,
                history.className.c_str(), history.picnum, U64(history.desired), Bool(history.pending), U64(history.published), history.pendingFrame,
                history.pending && frameIndex >= history.pendingFrame ? frameIndex - history.pendingFrame : 0, U64(history.pendingPresentation), Bool(live != nullptr),
                Bool(live && live->publicationEligible));
            state.Emit("voxel_cache_actor_state", fields);
        }
        history.sourcePresent = false;
        history.pending = false;
        history.outcome = "source-absent";
    }
    if (source.empty() || transitions || omittedCaptured || omittedRetained || heldCount || PerfAuditCensusRequested() || state.suppressed)
    {
        std::snprintf(fields, sizeof(fields), "\"stage\":\"batch-publication\",\"renderer_frame\":%u,\"map_generation\":%u,"
            "\"authority_available\":true,\"source_eligible\":%zu,\"published\":%u,\"held\":%u,\"omitted_captured\":%u,\"omitted_retained\":%u,"
            "\"omitted_previously_active_at_batch_begin\":%u,\"pending_max_age_frames\":%u,\"transitions\":%u,"
            "\"actor_history_count\":%zu,\"actor_history_capacity\":%zu,\"actor_history_overflow_observations\":%u,"
            "\"previous_active_overflow_observations\":%u,\"defer_reason_overflow_observations\":%u,"
            "\"mesh_history_count\":%zu,\"mesh_history_capacity\":%zu,\"mesh_history_overflow_events\":%u,"
            "\"event_limit_per_presentation\":%u,\"suppressed_events_this_presentation\":%u,\"complete\":%s",
            frameIndex, residencyMapGeneration, source.size(), readyCount, heldCount, omittedCaptured, omittedRetained,
            omittedPreviouslyActive, pendingMaxFrames, transitions, state.actors.size(), ActorLimit, state.actorOverflow,
            state.previousOverflow, state.reasonOverflow,
            state.evictions.size(), MeshLimit, state.meshOverflow, EventLimit, state.suppressed,
            Bool(!state.actorOverflow && !state.meshOverflow && !state.previousOverflow && !state.reasonOverflow && !state.suppressed));
        // One summary per EnsureBatch even if the detailed event cap was hit.
        if (!emptySourceEvidence.empty())
        {
            const std::string combined = std::string(fields) + "," + emptySourceEvidence;
            PerfAuditWriteEvent("voxel_cache_actor_summary", token, combined.c_str());
        }
        else PerfAuditWriteEvent("voxel_cache_actor_summary", token, fields);
    }
}
