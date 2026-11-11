#pragma once

#include "nri_scene_lights.h"
#include "nri_cvars.h"
#include "perf_audit.h"
#include "c_cvars.h"

#include <cmath>
#include <cstdio>

// CPU-only aggregate telemetry. The renderer owns the update and writer thread.
class NRIEmissiveSamplingAudit
{
public:
	NRIEmissiveSamplingAudit(const SceneLightSystem& lights,
		const SceneLightSystem::EmissiveSamplingUploadStats& stats,
		const NRIEmissiveSamplingUploadScratch& scratch, uint32_t frame)
		: mLights(lights), mStats(stats), mScratch(scratch), mFrame(frame) {}
	bool built = false;
	bool reused = false;
	bool completed = false;
	~NRIEmissiveSamplingAudit()
	{
		if (!PerfAuditActive()) return;
		const auto& cache = mLights.GetEmissiveGeometryCacheStats();
		const auto time = [](PerfAuditMetric metric, double ms)
		{
			PerfAuditAdd(metric, std::isfinite(ms) && ms > 0.0 ? (uint64_t)(ms * 1000000.0) : 0);
		};
		PerfAuditAdd(PerfAuditMetric::EmissiveSamplingUpdates);
		PerfAuditAdd(PerfAuditMetric::EmissiveSamplingBuilds, built);
		PerfAuditAdd(PerfAuditMetric::EmissiveSamplingDestinationHits, reused);
		PerfAuditAdd(PerfAuditMetric::EmissiveSamplingCompleted, completed);
		time(PerfAuditMetric::EmissiveSamplingIdentityNs, cache.identityMs);
		time(PerfAuditMetric::EmissiveSamplingFullHashNs, cache.validationHashMs);
		time(PerfAuditMetric::EmissiveSamplingTopologyNs, cache.topologyMs);
		if (PerfAuditCpuDetailActive()) time(PerfAuditMetric::EmissiveSamplingPrimitiveFactNs, cache.primitiveFactMs);
		time(PerfAuditMetric::EmissiveSamplingWeightsNs, mStats.weightsBuildMs);
		time(PerfAuditMetric::EmissiveSamplingValidationNs, mStats.payloadValidationMs);
#define EMISSIVE_AUDIT(name, value) PerfAuditAdd(PerfAuditMetric::EmissiveSampling##name, value)
		EMISSIVE_AUDIT(HashedVertices, cache.hashedVertices);
		EMISSIVE_AUDIT(HashedPrimitives, cache.hashedPrimitives);
		EMISSIVE_AUDIT(SnapshotChecks, cache.snapshotChecks);
		EMISSIVE_AUDIT(SnapshotHits, cache.snapshotHits);
		EMISSIVE_AUDIT(SnapshotComparedBytes, cache.snapshotComparedBytes);
		EMISSIVE_AUDIT(SnapshotWrittenBytes, cache.snapshotWrittenBytes);
		EMISSIVE_AUDIT(SnapshotPositionOnlyChanges, cache.snapshotPositionOnlyChanges);
		EMISSIVE_AUDIT(TopologyBuilds, cache.topologyBuilds);
		EMISSIVE_AUDIT(TopologyReuses, cache.topologyReuses);
		EMISSIVE_AUDIT(PrimitiveBuilds, cache.primitiveBuilds);
		EMISSIVE_AUDIT(PrimitiveReuses, cache.primitiveReuses);
		EMISSIVE_AUDIT(PrimitiveInvalidations, cache.primitiveInvalidations);
		EMISSIVE_AUDIT(IdentityChecks, cache.identityChecks);
		EMISSIVE_AUDIT(IdentityMismatches, cache.identityMismatches);
		EMISSIVE_AUDIT(PayloadChecks, mStats.payloadValidationChecks);
		EMISSIVE_AUDIT(PayloadMismatches, mStats.payloadValidationMismatches);
		EMISSIVE_AUDIT(DistributionTopologyBuilds, mStats.distributionTopologyBuilds);
		EMISSIVE_AUDIT(DistributionTopologyReuses, mStats.distributionTopologyReuses);
		EMISSIVE_AUDIT(DistributionComparedCandidates, mStats.distributionComparedCandidates);
#undef EMISSIVE_AUDIT
		PerfAuditSet(PerfAuditMetric::EmissiveSamplingSnapshotCapacityBytes, mLights.GetEmissiveGeometrySnapshotCapacityBytes());
		PerfAuditSet(PerfAuditMetric::EmissiveSamplingGeometryCapacityBytes, mLights.GetEmissiveGeometryCacheCapacityBytes());
		PerfAuditSet(PerfAuditMetric::EmissiveSamplingUploadCapacityBytes, mScratch.CapacityBytes());
		PerfAuditSet(PerfAuditMetric::EmissiveSamplingDistributionTopologyCapacityBytes, mLights.GetEmissiveDistributionTopologyCapacityBytes());
		PerfAuditSet(PerfAuditMetric::EmissiveSamplingQuarantinedDomains, cache.quarantinedDomains);
		if (PerfAuditCensusRequested() || mStats.payloadValidationMismatches != 0 || cache.identityMismatches != 0)
		{
			char fields[1024];
			std::snprintf(fields, sizeof(fields),
				"\"renderer_frame\":%u,\"cache_enabled\":%s,\"reference_enabled\":%s,\"built\":%s,\"destination_reused\":%s,\"completed\":%s,\"snapshot_checks\":%u,\"snapshot_hits\":%u,\"snapshot_bytes\":%llu,\"identity_checks\":%u,\"identity_mismatches\":%u,\"payload_checks\":%u,\"payload_mismatches\":%u,\"quarantined_domains\":%u,\"distribution_reuses\":%u",
				mFrame, (bool)nri_ptemissivecache ? "true" : "false", (bool)nri_ptemissivecachevalidate ? "true" : "false",
				built ? "true" : "false", reused ? "true" : "false", completed ? "true" : "false",
				cache.snapshotChecks, cache.snapshotHits, (unsigned long long)mLights.GetEmissiveGeometrySnapshotCapacityBytes(),
				cache.identityChecks, cache.identityMismatches, mStats.payloadValidationChecks,
				mStats.payloadValidationMismatches, cache.quarantinedDomains, mStats.distributionTopologyReuses);
			PerfAuditWriteEvent("emissive_sampling_cpu", PerfAuditCurrentToken(), fields);
		}
	}
private:
	const SceneLightSystem& mLights;
	const SceneLightSystem::EmissiveSamplingUploadStats& mStats;
	const NRIEmissiveSamplingUploadScratch& mScratch;
	uint32_t mFrame;
};
