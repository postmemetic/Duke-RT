#include "nri_filter_candidate_policy.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
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

namespace
{
	uint32_t ClassifyPrimitivePolicy(
		const nri_scene::PrimitiveData& primitive,
		const std::vector<nri_scene::MaterialData>& materials)
	{
		uint32_t policy = NRIFilterCandidatePolicy_None;
		if ((primitive.flags & nri_scene::PrimitiveFlag_ReflectionOnly) != 0u)
		{
			policy |= NRIFilterCandidatePolicy_ReflectionOnly;
		}
		if (primitive.materialIndex >= materials.size())
		{
			return policy;
		}

		const nri_scene::MaterialData& material = materials[primitive.materialIndex];
		if ((material.flags & nri_scene::MaterialFlag_OneWay) != 0u)
		{
			policy |= NRIFilterCandidatePolicy_OneWay;
		}
		if ((material.lightingFlags & nri_scene::MaterialLightingFlag_NoShadowCast) != 0u)
		{
			policy |= NRIFilterCandidatePolicy_NoShadow;
		}
		// AlphaClip is the producer's exact declaration that raw texels can
		// reject an intersection. A fractional constant alpha is independently
		// sufficient for the non-indexed alpha test.
		if ((material.flags & nri_scene::MaterialFlag_AlphaClip) != 0u || material.alpha < 0.999f)
		{
			policy |= NRIFilterCandidatePolicy_Alpha;
		}
		return policy;
	}
}

bool BuildFilterCandidateRuns(
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t enabledPolicyMask,
	uint32_t maxRuns,
	std::vector<NRIFilterCandidateRun>& outRuns)
{
	outRuns.clear();
	if (primitiveCount == 0u || indexCount != primitiveCount * 3u || maxRuns == 0u)
	{
		return false;
	}
	if ((uint64_t)primitiveOffset + primitiveCount > geometry.primitives.size() ||
		(uint64_t)indexOffset + indexCount > geometry.indices.size())
	{
		return false;
	}

	for (uint32_t localPrimitive = 0u; localPrimitive < primitiveCount; ++localPrimitive)
	{
		const uint32_t absolutePrimitive = primitiveOffset + localPrimitive;
		const uint32_t policy =
			ClassifyPrimitivePolicy(geometry.primitives[absolutePrimitive], materials) & enabledPolicyMask;
		const bool startsNewTraversalClass = outRuns.empty() ||
			((outRuns.back().policyMask == NRIFilterCandidatePolicy_None) !=
				(policy == NRIFilterCandidatePolicy_None));
		if (startsNewTraversalClass)
		{
			if (outRuns.size() >= maxRuns)
			{
				outRuns.clear();
				return false;
			}
			NRIFilterCandidateRun run = {};
			run.primitiveOffset = absolutePrimitive;
			run.indexOffset = indexOffset + localPrimitive * 3u;
			run.policyMask = policy;
			outRuns.push_back(run);
		}
		else
		{
			// All candidate policies share the same non-opaque traversal class;
			// HLSL derives the precise predicate from immutable primitive/material
			// facts. OR the reasons instead of fragmenting adjacent candidates.
			outRuns.back().policyMask |= policy;
		}
		outRuns.back().primitiveCount++;
		outRuns.back().indexCount += 3u;
	}

	uint32_t coveredPrimitives = 0u;
	uint32_t coveredIndices = 0u;
	for (const NRIFilterCandidateRun& run : outRuns)
	{
		if (run.primitiveOffset != primitiveOffset + coveredPrimitives ||
			run.indexOffset != indexOffset + coveredIndices ||
			run.indexCount != run.primitiveCount * 3u)
		{
			outRuns.clear();
			return false;
		}
		coveredPrimitives += run.primitiveCount;
		coveredIndices += run.indexCount;
	}
	return coveredPrimitives == primitiveCount && coveredIndices == indexCount;
}
