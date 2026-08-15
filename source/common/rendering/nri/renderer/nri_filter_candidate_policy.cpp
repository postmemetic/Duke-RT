#include "nri_filter_candidate_policy.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_scene_surface_types.h"

#include <limits>

NRIFilterCandidateCertificate ClassifyReflectionOnlyFilterCandidateSpan(
	const nri_scene::GeometryData& geometry,
	uint32_t primitiveOffset,
	uint32_t primitiveCount)
{
	NRIFilterCandidateCertificate result = {};
	result.primitiveCount = primitiveCount;
	if (primitiveCount == 0)
	{
		result.outcome = NRIFilterCandidateCertificateOutcome::Empty;
		return result;
	}

	const uint64_t primitiveEnd = (uint64_t)primitiveOffset + primitiveCount;
	if (primitiveEnd > geometry.primitives.size())
	{
		result.outcome = NRIFilterCandidateCertificateOutcome::OutOfBounds;
		return result;
	}

	for (uint64_t primitiveIndex = primitiveOffset; primitiveIndex < primitiveEnd; ++primitiveIndex)
	{
		if ((geometry.primitives[primitiveIndex].flags & nri_scene::PrimitiveFlag_ReflectionOnly) == 0u)
		{
			result.outcome = NRIFilterCandidateCertificateOutcome::MixedPolicy;
			return result;
		}
	}

	result.outcome = NRIFilterCandidateCertificateOutcome::Certified;
	return result;
}
