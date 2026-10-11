#include "nri_occurrence_workload_mask_policy.h"

#include "nri_tlas_masks.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"

#include <limits>

namespace
{
	bool IsValidRange(uint32_t offset, uint32_t count, size_t capacity)
	{
		const uint64_t end = (uint64_t)offset + count;
		return end <= capacity && end <= std::numeric_limits<uint32_t>::max();
	}

	NRIOccurrenceWorkloadCertificate CertifyReflectionOnly(
		const nri_scene::GeometryData& geometry,
		const NRISceneBufferUploadDomainSpan& span)
	{
		for (uint32_t local = 0; local < span.primitiveCount; ++local)
		{
			if ((geometry.primitives[span.primitiveOffset + local].flags & nri_scene::PrimitiveFlag_ReflectionOnly) == 0u)
				return NRIOccurrenceWorkloadCertificate::MixedReflection;
		}
		return NRIOccurrenceWorkloadCertificate::Certified;
	}

	NRIOccurrenceWorkloadCertificate CertifySurfaceLightNoShadow(
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& finalMaterials,
		const NRISceneBufferUploadDomainSpan& span)
	{
		if (span.materialCount == 0u || !IsValidRange(span.materialOffset, span.materialCount, finalMaterials.size()))
			return NRIOccurrenceWorkloadCertificate::MaterialRange;
		const uint32_t materialEnd = span.materialOffset + span.materialCount;
		for (uint32_t materialIndex = span.materialOffset; materialIndex < materialEnd; ++materialIndex)
		{
			if ((finalMaterials[materialIndex].lightingFlags & nri_scene::MaterialLightingFlag_NoShadowCast) == 0u)
				return NRIOccurrenceWorkloadCertificate::MissingNoShadow;
		}
		for (uint32_t local = 0; local < span.primitiveCount; ++local)
		{
			const uint32_t materialIndex = geometry.primitives[span.primitiveOffset + local].materialIndex;
			if (materialIndex < span.materialOffset || materialIndex >= materialEnd)
				return NRIOccurrenceWorkloadCertificate::MaterialReference;
		}
		return NRIOccurrenceWorkloadCertificate::Certified;
	}

	void AccumulateCounts(NRIOccurrenceWorkloadMaskCounts& counts, const NRIOccurrenceWorkloadMaskDecision& decision)
	{
		counts.occurrences++;
		counts.reflectionOutcomes[(size_t)decision.reflection]++;
		counts.surfaceLightOutcomes[(size_t)decision.surfaceLight]++;
		counts.reflectionCertified += decision.reflection == NRIOccurrenceWorkloadCertificate::Certified ? 1u : 0u;
		counts.surfaceLightCertified += decision.surfaceLight == NRIOccurrenceWorkloadCertificate::Certified ? 1u : 0u;
		if (decision.certifiedRemovalMask != 0u)
			counts.certifiedPrimitives += decision.primitiveCount;
		counts.appliedOccurrences += decision.removedMask != 0u ? 1u : 0u;
		if ((decision.removedMask & NRI_TLAS_MASK_MAIN) != 0u)
			counts.removedMainPrimitives += decision.primitiveCount;
		if ((decision.removedMask & NRI_TLAS_MASK_SHADOW) != 0u)
			counts.removedShadowPrimitives += decision.primitiveCount;
		if ((decision.removedMask & NRI_TLAS_MASK_GI) != 0u)
			counts.removedGiPrimitives += decision.primitiveCount;
		counts.certifiedMask |= decision.certifiedRemovalMask;
		counts.removedMask |= decision.removedMask;
		counts.withheldMask |= decision.withheldMask;
	}
}

const NRISceneBufferUploadDomainSpan* FindNRIUniqueCoveringWorkloadMaskSpan(
	const std::vector<NRISceneBufferUploadDomainSpan>& spans,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t indexOffset,
	uint32_t indexCount)
{
	if (primitiveCount == 0u || (uint64_t)primitiveCount * 3u != indexCount ||
		!IsValidRange(primitiveOffset, primitiveCount, std::numeric_limits<uint32_t>::max()) ||
		!IsValidRange(indexOffset, indexCount, std::numeric_limits<uint32_t>::max()))
		return nullptr;
	const NRISceneBufferUploadDomainSpan* covering = nullptr;
	for (const NRISceneBufferUploadDomainSpan& span : spans)
	{
		if (span.primitiveCount == 0u && span.indexCount == 0u)
			continue;
		if (covering != nullptr || span.primitiveOffset != primitiveOffset || span.primitiveCount != primitiveCount ||
			span.indexOffset != indexOffset || span.indexCount != indexCount || span.domain >= NRISceneBufferUploadDomain::Count)
			return nullptr;
		covering = &span;
	}
	return covering;
}

NRIOccurrenceWorkloadMaskDecision EvaluateNRIOccurrenceWorkloadMaskPolicy(
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& finalMaterials,
	const NRISceneBufferUploadDomainSpan& span,
	NRIOccurrenceWorkloadMaskScope scope,
	uint32_t requestedMask,
	const NRIOccurrenceWorkloadMaskFacts& facts)
{
	NRIOccurrenceWorkloadMaskDecision decision = {};
	decision.requestedMask = requestedMask;
	decision.publishedMask = requestedMask;
	decision.primitiveCount = span.primitiveCount;
	const bool surfaceLightCandidate = scope == NRIOccurrenceWorkloadMaskScope::UploadSpan &&
		span.domain == NRISceneBufferUploadDomain::SurfaceLightOverlay;
	auto reject = [&](NRIOccurrenceWorkloadCertificate outcome)
	{
		decision.reflection = outcome;
		if (surfaceLightCandidate)
			decision.surfaceLight = outcome;
		return decision;
	};
	if ((scope != NRIOccurrenceWorkloadMaskScope::UploadSpan && scope != NRIOccurrenceWorkloadMaskScope::CapturedGeometry) ||
		(scope == NRIOccurrenceWorkloadMaskScope::UploadSpan && span.domain >= NRISceneBufferUploadDomain::Count))
		return reject(NRIOccurrenceWorkloadCertificate::AmbiguousOccurrence);
	if (span.primitiveCount == 0u)
		return reject(NRIOccurrenceWorkloadCertificate::Empty);
	if (!IsValidRange(span.primitiveOffset, span.primitiveCount, geometry.primitives.size()))
		return reject(NRIOccurrenceWorkloadCertificate::PrimitiveRange);

	decision.reflection = CertifyReflectionOnly(geometry, span);
	if (decision.reflection == NRIOccurrenceWorkloadCertificate::Certified)
		decision.certifiedRemovalMask |= NRI_TLAS_MASK_MAIN | NRI_TLAS_MASK_SHADOW | NRI_TLAS_MASK_GI;
	if (surfaceLightCandidate)
	{
		decision.surfaceLight = CertifySurfaceLightNoShadow(geometry, finalMaterials, span);
		if (decision.surfaceLight == NRIOccurrenceWorkloadCertificate::Certified)
			decision.certifiedRemovalMask |= NRI_TLAS_MASK_SHADOW;
	}

	uint32_t permittedMask = NRI_TLAS_MASK_MAIN;
	if (facts.shadowRemovalPermitted)
		permittedMask |= NRI_TLAS_MASK_SHADOW;
	if (facts.giRemovalPermitted)
		permittedMask |= NRI_TLAS_MASK_GI;
	decision.withheldMask = requestedMask & decision.certifiedRemovalMask & ~permittedMask;
	if (facts.enabled)
	{
		decision.removedMask = requestedMask & decision.certifiedRemovalMask & permittedMask;
		decision.publishedMask = requestedMask & ~decision.removedMask;
	}
	return decision;
}

uint32_t ApplyNRIOccurrenceWorkloadMaskPolicy(
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& finalMaterials,
	const NRISceneBufferUploadDomainSpan& span,
	NRIOccurrenceWorkloadMaskScope scope,
	uint32_t requestedMask,
	const NRIOccurrenceWorkloadMaskFacts& facts,
	NRIOccurrenceWorkloadMaskStats& stats)
{
	const NRIOccurrenceWorkloadMaskDecision decision = EvaluateNRIOccurrenceWorkloadMaskPolicy(
		geometry, finalMaterials, span, scope, requestedMask, facts);
	stats.facts = facts;
	AccumulateCounts(stats.total, decision);
	const size_t domain = scope == NRIOccurrenceWorkloadMaskScope::CapturedGeometry ? NRI_OCCURRENCE_MASK_CAPTURED_DOMAIN :
		(scope == NRIOccurrenceWorkloadMaskScope::UploadSpan && span.domain < NRISceneBufferUploadDomain::Count ?
			(size_t)span.domain : NRI_OCCURRENCE_MASK_AGGREGATE_DOMAIN);
	AccumulateCounts(stats.domains[domain], decision);
	return decision.publishedMask;
}
