#include "nri_smoke_transient_diagnostics.h"
#include "printf.h"

void NRIPrintSmokeTransientTelemetry(const NRISmokeTransientTelemetry& t)
{
	const auto& c = t.cpu;
	const auto& g = t.gpu;
	Printf("PERF pt smoke transient NRI: renderer_frame=%llu epoch=%u profile=%u valid=%u groups=%u lobes=%u group_high_water=%u lobe_high_water=%u group_requested=%llu group_admitted=%llu group_expired=%llu group_drop=%llu lobe_drop=%llu reduced=%llu resident_bytes=%llu bins=%u candidates=%u overflow=%u light_groups=%u full_builds=%u fallback_builds=%u anchors=%u full_published=%u fallback_published=%u point_candidates=%u point_selected=%u directional_samples=%u emissive_samples=%u build_rays=%u self_tests=%u observed=%u observed_full=%u observed_fallback=%u missing=%u identity_rejects=%u froxels_tested=%u froxels_applied=%u lobe_tests=%u lobe_contributions=%u apply_visibility_rays=%u "
		"cpu_visible_groups=%u cpu_visible_lobes=%u group_limit=%u lobe_limit=%u "
		"drop_invalid=%llu drop_stale=%llu drop_epoch=%llu drop_expired=%llu "
		"cpu_full_scheduled=%u cpu_refresh_scheduled=%u cpu_fallback=%u oldest_ms=%u compact=1\n",
		(unsigned long long)t.rendererFrame, t.epoch, t.profile, t.valid ? 1u : 0u,
		c.activeGroups, c.activeLobes, c.groupHighWater, c.lobeHighWater,
		(unsigned long long)c.groupsRequested, (unsigned long long)c.groupsAdmitted,
		(unsigned long long)c.groupsExpired, (unsigned long long)c.droppedGroupCapacity,
		(unsigned long long)c.droppedLobeCapacity, (unsigned long long)c.deterministicallyReducedGroups,
		(unsigned long long)t.residentBytes,
		g.transientBinsTouched, g.transientBinCandidates, g.transientBinOverflow,
		g.transientLightBuildGroups, g.transientLightFullBuilds, g.transientLightFallbackBuilds,
		g.transientLightAnchorsWritten, g.transientLightPublishedFull, g.transientLightPublishedFallback,
		g.transientLightPointCandidatesTested, g.transientLightPointSelected,
		g.transientLightDirectionalSamples, g.transientLightEmissiveSamples,
		g.transientLightVisibilityRays, g.transientLightSelfTransmittanceTests,
		g.transientLightObservedGroups, g.transientLightObservedFull, g.transientLightObservedFallback,
		g.transientLightMissing, g.transientLightIdentityRejects,
		g.transientMaterializeFroxelsTested, g.transientMaterializeFroxelsApplied,
		g.transientMaterializeLobeTests, g.transientMaterializeLobeContributions,
		g.transientLightApplyVisibilityRays,
		c.visibleGroups, c.visibleLobes, c.maximumActiveGroups, c.maximumActiveLobes,
		(unsigned long long)c.droppedInvalidRequest, (unsigned long long)c.droppedStaleOnArrival,
		(unsigned long long)c.droppedStaleEpoch, (unsigned long long)c.droppedExpiredOnArrival,
		c.fullLightFreshScheduledThisFrame, c.fullLightRefreshScheduledThisFrame, c.fallbackLightGroups,
		c.oldestActiveAgeMilliseconds);
}
