#include "nri_filter_candidate_policy.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_surface_types.h"

namespace
{
	uint32_t ClassifyPrimitivePolicy(
		const nri_scene::PrimitiveData& primitive,
		const nri_scene::MaterialData& material)
	{
		uint32_t policy = NRIFilterCandidatePolicy_None;
		if ((primitive.flags & nri_scene::PrimitiveFlag_ReflectionOnly) != 0u)
		{
			policy |= NRIFilterCandidatePolicy_ReflectionOnly;
		}
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

bool GetFilterCandidatePolicyForSpan(
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t enabledPolicyMask,
	uint32_t& outPolicyMask)
{
	outPolicyMask = NRIFilterCandidatePolicy_None;
	const uint64_t primitiveEnd = (uint64_t)primitiveOffset + primitiveCount;
	if (primitiveEnd > geometry.primitives.size())
	{
		return false;
	}

	uint32_t policyMask = NRIFilterCandidatePolicy_None;
	for (uint64_t primitiveIndex = primitiveOffset; primitiveIndex < primitiveEnd; ++primitiveIndex)
	{
		const nri_scene::PrimitiveData& primitive = geometry.primitives[primitiveIndex];
		if (primitive.materialIndex >= materials.size())
		{
			return false;
		}
		policyMask |= ClassifyPrimitivePolicy(primitive, materials[primitive.materialIndex]) & enabledPolicyMask;
	}
	outPolicyMask = policyMask;
	return true;
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
	if (primitiveCount == 0u || (uint64_t)indexCount != (uint64_t)primitiveCount * 3u || maxRuns == 0u)
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
		const nri_scene::PrimitiveData& primitive = geometry.primitives[absolutePrimitive];
		if (primitive.materialIndex >= materials.size())
		{
			outRuns.clear();
			return false;
		}
		const uint32_t policy =
			ClassifyPrimitivePolicy(primitive, materials[primitive.materialIndex]) & enabledPolicyMask;
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
