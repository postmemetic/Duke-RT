#pragma once

#include <cstdint>

namespace nri_scene
{
// Diagnostic facts only. Scope is the discovered/captured cache publication
// set, never all live actors or pixels. No pointer ownership or renderer policy.
struct VoxelPublicationDiagnosticStamp
{
    uint64_t auditEpoch = 0, cacheFrame = 0, cacheSerial = 0;
    uint64_t snapshotGeneration = 0, worldEpoch = 0;
    uint64_t simulationGeneration = 0, lifecycleSerial = 0, journalSerial = 0;
    bool observed = false;
};

enum class VoxelPublicationFilter : uint8_t
{
    Accepted, StartupSuppressed, InvalidPlacement, AuthorityNotCurrent,
    PublicationIneligible, PendingRemoval, RepresentationUnready, SharedSurfaceUnavailable,
    Count
};

struct VoxelPublicationEntryDiagnosticFacts
{
    bool startupSuppressed = false, validPlacement = false;
    bool authorityCurrent = false, publicationEligible = false, pendingRemoval = false;
    bool currentReady = false, desiredDirectPending = false, surfaceAvailable = false;
};

// Predicate order intentionally matches BuildVoxelActorPublicationEntry.
// Exclusive buckets sum to cacheEntries; raw reasons may coexist in a row.
inline VoxelPublicationFilter ClassifyVoxelPublicationEntry(const VoxelPublicationEntryDiagnosticFacts& f)
{
    if (f.startupSuppressed) return VoxelPublicationFilter::StartupSuppressed;
    if (!f.validPlacement) return VoxelPublicationFilter::InvalidPlacement;
    if (!f.authorityCurrent) return VoxelPublicationFilter::AuthorityNotCurrent;
    if (!f.publicationEligible) return VoxelPublicationFilter::PublicationIneligible;
    if (f.pendingRemoval) return VoxelPublicationFilter::PendingRemoval;
    if (!f.currentReady && !f.desiredDirectPending) return VoxelPublicationFilter::RepresentationUnready;
    if (!f.desiredDirectPending && !f.surfaceAvailable) return VoxelPublicationFilter::SharedSurfaceUnavailable;
    return VoxelPublicationFilter::Accepted;
}

struct VoxelPublicationDiagnosticStatus
{
    VoxelPublicationDiagnosticStamp completedCapture;
    uint64_t auditEpoch = 0, cacheFrame = 0, cacheSerial = 0;
    uint64_t snapshotGeneration = 0, snapshotWorld = 0, journalWorld = 0;
    uint64_t snapshotSimulation = 0, currentSimulation = 0;
    uint64_t snapshotLifecycle = 0, journalSerial = 0;
    uint64_t snapshotActors = 0, cacheEntries = 0, eligibleEntries = 0;
    uint64_t filters[uint32_t(VoxelPublicationFilter::Count)] = {};
    uint32_t captureDepth = 0;
    bool snapshotComplete = false, startupSuppressed = false;

    uint64_t Filter(VoxelPublicationFilter filter) const { return filters[uint32_t(filter)]; }
    bool CountsMatch() const
    {
        uint64_t count = 0;
        for (uint64_t value : filters) count += value;
        return count == cacheEntries && Filter(VoxelPublicationFilter::Accepted) == eligibleEntries;
    }
    bool ProviderCurrent() const
    {
        return snapshotComplete && snapshotGeneration != 0 && snapshotWorld != 0 &&
            snapshotWorld == journalWorld && snapshotSimulation == currentSimulation &&
            snapshotLifecycle == journalSerial;
    }
    bool CaptureCurrent() const
    {
        const auto& c = completedCapture;
        return c.observed && c.auditEpoch == auditEpoch && captureDepth == 0 &&
            c.cacheFrame == cacheFrame && c.cacheSerial == cacheSerial &&
            c.snapshotGeneration == snapshotGeneration && c.worldEpoch == snapshotWorld &&
            c.simulationGeneration == snapshotSimulation && c.lifecycleSerial == snapshotLifecycle &&
            c.journalSerial == journalSerial;
    }
    bool HasUnreadyRows() const
    {
        return Filter(VoxelPublicationFilter::RepresentationUnready) != 0 ||
            Filter(VoxelPublicationFilter::SharedSurfaceUnavailable) != 0;
    }
    const char* Classification() const
    {
        if (startupSuppressed) return "startup-suppressed";
        if (!snapshotComplete || snapshotGeneration == 0) return "snapshot-unpublished-or-incomplete";
        if (!ProviderCurrent()) return "snapshot-stale";
        if (captureDepth != 0) return "capture-in-progress";
        if (!completedCapture.observed || completedCapture.auditEpoch != auditEpoch) return "capture-not-observed";
        if (!CaptureCurrent()) return "capture-stale";
        if (!CountsMatch()) return "filter-count-mismatch";
        if (eligibleEntries != 0) return HasUnreadyRows() ? "eligible-with-unready-filters" : "eligible";
        if (HasUnreadyRows()) return "empty-unready";
        return cacheEntries == 0 ? "valid-empty-cache" : "valid-empty-filtered";
    }
    bool ObservationAvailable() const
    {
        return !startupSuppressed && ProviderCurrent() && CaptureCurrent() && CountsMatch() &&
            (eligibleEntries != 0 || !HasUnreadyRows());
    }
};
}
