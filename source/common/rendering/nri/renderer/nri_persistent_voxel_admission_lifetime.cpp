#include "nri_persistent_voxels.h"
#include "perf_audit.h"

NRIPersistentVoxelSchedulerRetirement NRIPersistentVoxelResidency::TerminalizeAdmissionSchedulerToken(
    PersistentVoxelAdmissionEntry& entry, bool failed)
{
    const uint64_t tokenId = entry.schedulerTokenId;
    entry.schedulerTokenId = 0;
    NRIVoxelAdmissionTokenSnapshot token;
    if (!tokenId || !admissionScheduler.GetTokenSnapshot(tokenId, token)) return {};
    // Several material bindings can attach to one geometry job. A failed
    // follower must not cancel the job owned by its first binding.
    if (!token.bindings.empty() && token.bindings.front().pairKey != entry.pairKey) return {};
    const bool terminal = token.stage == NRIVoxelAdmissionStage::Ready ||
        token.stage == NRIVoxelAdmissionStage::Cancelled || token.stage == NRIVoxelAdmissionStage::Failed ||
        token.stage == NRIVoxelAdmissionStage::Stale;
    if (!terminal)
    {
        if (failed) admissionScheduler.Fail(tokenId); else admissionScheduler.Cancel(tokenId);
    }
    const NRIPersistentVoxelSchedulerRetirement ticket{ schedulerRetirementEpoch, tokenId, token.mesh.mapGeneration };
    return IsSchedulerRetirementPending(ticket) ? ticket : NRIPersistentVoxelSchedulerRetirement{};
}

bool NRIPersistentVoxelResidency::IsSchedulerRetirementPending(
    NRIPersistentVoxelSchedulerRetirement ticket, uint64_t* bytes) const
{
    if (bytes) *bytes = 0;
    if (!ticket || ticket.epoch != schedulerRetirementEpoch ||
        ticket.mapGeneration != admissionScheduler.GetSnapshot().activeMapGeneration) return false;
    NRIVoxelAdmissionTokenSnapshot token;
    if (!admissionScheduler.GetTokenSnapshot(ticket.tokenId, token) || token.mesh.mapGeneration != ticket.mapGeneration ||
        (token.reservationState != NRIVoxelAdmissionReservationState::RetirePending &&
         token.reservationState != NRIVoxelAdmissionReservationState::Abandoned)) return false;
    if (bytes) *bytes = token.reservation.TotalBytes();
    return true;
}

bool NRIPersistentVoxelResidency::ReleaseCompletedSchedulerReservation(NRIPersistentVoxelSchedulerRetirement ticket)
{
    uint64_t bytes = 0;
    if (!IsSchedulerRetirementPending(ticket, &bytes)) return true; // Duplicate or an old scheduler epoch.
    if (!admissionScheduler.RetireTerminalReservation(ticket.tokenId)) return false;
    PerfAuditAdd(PerfAuditMetric::VoxelSchedulerReservationRetirements);
    PerfAuditAdd(PerfAuditMetric::VoxelSchedulerReservationRetiredBytes, bytes);
    MarkMaintenanceMutation();
    return true;
}
