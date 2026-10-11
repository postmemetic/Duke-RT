#include "nri_occurrence_workload_mask_diagnostics.h"

#include "nri_occurrence_workload_mask_policy.h"
#include "printf.h"

namespace
{
	const char* GetDomainName(size_t domain)
	{
		if (domain == NRI_OCCURRENCE_MASK_CAPTURED_DOMAIN) return "captured";
		if (domain == NRI_OCCURRENCE_MASK_AGGREGATE_DOMAIN) return "aggregate";
		switch ((NRISceneBufferUploadDomain)domain)
		{
		case NRISceneBufferUploadDomain::StaticOverlay: return "static_overlay";
		case NRISceneBufferUploadDomain::RuntimeSpaceLink: return "runtime_space_link";
		case NRISceneBufferUploadDomain::RuntimeMutation: return "runtime_mutation";
		case NRISceneBufferUploadDomain::Dynamic: return "dynamic";
		case NRISceneBufferUploadDomain::LocalPlayerReflection: return "local_player_reflection";
		case NRISceneBufferUploadDomain::RuntimeDebugSphere: return "debug_sphere";
		case NRISceneBufferUploadDomain::SurfaceLightOverlay: return "surface_light_overlay";
		case NRISceneBufferUploadDomain::PersistentVoxelMaterial: return "voxel_material";
		default: return "unknown";
		}
	}

	void LogCounts(uint64_t frameNumber, const char* domain, const NRIOccurrenceWorkloadMaskCounts& counts)
	{
		const auto outcomeCount = [&](NRIOccurrenceWorkloadCertificate outcome)
		{
			// Empty/range/ambiguous outcomes are shared by both certificates.
			return counts.reflectionOutcomes[(size_t)outcome];
		};
		Printf("PERF pt occurrence workload mask NRI: frame=%llu domain=%s occurrences=%u reflection_certified=%u surface_light_certified=%u certified_prims=%u applied=%u main_prims=%u shadow_prims=%u gi_prims=%u certified_mask=0x%x removed_mask=0x%x withheld_mask=0x%x reject_empty=%u reject_primitive_range=%u reject_mixed_reflection=%u reject_material_range=%u reject_material_reference=%u reject_no_shadow=%u reject_ambiguous=%u\n",
			(unsigned long long)frameNumber, domain, counts.occurrences,
			counts.reflectionCertified, counts.surfaceLightCertified, counts.certifiedPrimitives, counts.appliedOccurrences,
			counts.removedMainPrimitives, counts.removedShadowPrimitives, counts.removedGiPrimitives,
			counts.certifiedMask, counts.removedMask, counts.withheldMask,
			outcomeCount(NRIOccurrenceWorkloadCertificate::Empty),
			outcomeCount(NRIOccurrenceWorkloadCertificate::PrimitiveRange),
			outcomeCount(NRIOccurrenceWorkloadCertificate::MixedReflection),
			counts.surfaceLightOutcomes[(size_t)NRIOccurrenceWorkloadCertificate::MaterialRange],
			counts.surfaceLightOutcomes[(size_t)NRIOccurrenceWorkloadCertificate::MaterialReference],
			counts.surfaceLightOutcomes[(size_t)NRIOccurrenceWorkloadCertificate::MissingNoShadow],
			outcomeCount(NRIOccurrenceWorkloadCertificate::AmbiguousOccurrence));
	}
}

void LogNRIOccurrenceWorkloadMaskStats(uint64_t frameNumber, const NRIOccurrenceWorkloadMaskStats& stats)
{
	Printf("PERF pt occurrence workload mask policy NRI: frame=%llu enabled=%u shadow_removal_permitted=%u gi_removal_permitted=%u\n",
		(unsigned long long)frameNumber, stats.facts.enabled ? 1u : 0u,
		stats.facts.shadowRemovalPermitted ? 1u : 0u, stats.facts.giRemovalPermitted ? 1u : 0u);
	LogCounts(frameNumber, "total", stats.total);
	for (size_t domain = 0; domain < stats.domains.size(); ++domain)
	{
		if (stats.domains[domain].occurrences != 0u)
			LogCounts(frameNumber, GetDomainName(domain), stats.domains[domain]);
	}
}
