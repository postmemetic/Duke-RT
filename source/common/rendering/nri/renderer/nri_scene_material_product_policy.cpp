#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_cpu_detail_audit.h"
#include "nri_scene_material_product.h"
#include "../scene/nri_fluid_material_binding.h"
#include "perf_audit.h"
#include "nri_material_policy_audit.h"

#include "lightoverlay.h"
#include "fluidoverlay.h"
#include "printf.h"

#include <algorithm>

namespace
{
	struct MaterialPolicyContext
	{
		const SceneLightSystem& lights;
		const ResolvedLightOverlaySet& overlays;
		const nri_material_policy::ActorMaterialOverrideMap* actors;
		const nri_scene::MaterialBridgeData& source;
		float glowBlend;
		float fullbrightBoost;
		const ResolvedFluidOverlaySet* fluids = nullptr;
		NRIMaterialPolicySnapshot* snapshot = nullptr;
	};

	void TransformRow(void* opaque, size_t index, nri_scene::MaterialData& row)
	{
		const auto& context = *static_cast<MaterialPolicyContext*>(opaque);
		if (context.snapshot != nullptr)
		{
			context.snapshot->Transform(index < context.source.lightMetadata.size() ? &context.source.lightMetadata[index] : nullptr, row);
			return;
		}
		if (context.fluids != nullptr) row.fluidBindingIndex = UINT32_MAX;
		if (index >= context.source.lightMetadata.size()) return;
		const auto& metadata = context.source.lightMetadata[index];
		nri_material_policy::ApplyEmissiveMaterialOverride(context.lights, context.overlays, context.glowBlend, metadata, row);
		if (context.actors != nullptr)
			nri_material_policy::ApplyActorShadowMaterialOverride(*context.actors, context.fullbrightBoost, metadata, row);
		if (context.fluids != nullptr)
			row.fluidBindingIndex = nri_scene::ResolveFluidMaterialBindingIndex(*context.fluids, metadata);
	}

	void TransformReference(void* opaque, std::vector<nri_scene::MaterialData>& rows)
	{
		const auto& context = *static_cast<MaterialPolicyContext*>(opaque);
		nri_material_policy::ApplyEmissiveMaterialOverrides(context.lights, context.overlays, context.glowBlend, context.source, rows);
		if (context.actors != nullptr)
			nri_material_policy::ApplyActorShadowMaterialOverrides(*context.actors, context.fullbrightBoost, context.source, rows);
		if (context.fluids != nullptr)
			nri_scene::ApplyFluidMaterialBindings(*context.fluids, context.source, rows);
	}

	void BeginPolicy(MaterialPolicyContext& context, NRIMaterialPolicySnapshot& owner)
	{
		NRICpuDetailAuditScope inputScope(PerfAuditMetric::MaterialPolicyInputResolveNs);
		if (!(bool)nri_ptmaterialpolicycache || owner.Quarantined()) return;
		owner.Begin(context.lights, context.overlays, context.actors, context.fluids,
			{ (float)nri_ptglowscale, (float)nri_ptglowfalloff, context.glowBlend, context.fullbrightBoost });
		context.snapshot = &owner;
	}

	void FinishPolicy(MaterialPolicyContext& context, const NRIMaterialProductPatchResult& result, const char* stage)
	{
		if (context.snapshot == nullptr) return;
		// These hits refer to reused current-operation selector matches, not
		// retained transformed rows. The oracle still checks the complete product.
		const bool hadHits = context.snapshot->Finish();
		if ((bool)nri_ptmaterialpolicyvalidate && result.validationChecked)
		{
			PerfAuditAdd(PerfAuditMetric::MaterialPolicyValidationChecks);
			if (hadHits) PerfAuditAdd(PerfAuditMetric::MaterialPolicyHitValidationChecks);
		}
		if (result.validationMismatch)
		{
			PerfAuditAdd(PerfAuditMetric::MaterialPolicyValidationMismatches);
			context.snapshot->Quarantine();
			Printf("NRI PT material policy: stage=%s mismatch=1 action=reference-fallback-quarantine\n", stage);
		}
	}

	void AuditMaterialProduct(const NRIMaterialProductPatchResult& result)
	{
		if (result.segments.attempts != 0)
		{
			PerfAuditAdd(PerfAuditMetric::MaterialSegmentAttempts, result.segments.attempts);
			PerfAuditAdd(PerfAuditMetric::MaterialSegmentHits, result.segments.hits);
			PerfAuditAdd(PerfAuditMetric::MaterialSegmentRowsReused, result.segments.reusedRows);
			PerfAuditAdd(PerfAuditMetric::MaterialSegmentCompareCalls, result.segments.compareCalls);
			PerfAuditAdd(PerfAuditMetric::MaterialSegmentCompareExtentBytes, result.segments.compareExtentRows * sizeof(nri_scene::MaterialData));
			PerfAuditAdd(PerfAuditMetric::MaterialSegmentFallbackRows, result.segments.fallbackRows);
			if (result.validationChecked && result.segments.hits != 0)
				PerfAuditAdd(PerfAuditMetric::MaterialSegmentHitValidationChecks);
			PerfAuditAdd(PerfAuditMetric::MaterialSegmentValidationMismatches, result.validationMismatch);
			PerfAuditAdd(PerfAuditMetric::MaterialSegmentQuarantines, result.validationMismatch);
		}
		if (PerfAuditCpuDetailActive())
		{
			PerfAuditAdd(PerfAuditMetric::MaterialPolicyProductTransformRows, result.evaluatedRows);
			PerfAuditAdd(PerfAuditMetric::MaterialPolicyProductComparedRows, result.outputComparedRows);
			PerfAuditAdd(PerfAuditMetric::MaterialPolicyProductCompareExtentBytes, result.outputComparedRows * sizeof(nri_scene::MaterialData));
			PerfAuditAdd(PerfAuditMetric::MaterialPolicyProductWrittenRows, result.copiedRows);
			PerfAuditAdd(PerfAuditMetric::MaterialPolicyProductWrittenBytes, result.copiedRows * sizeof(nri_scene::MaterialData));
			PerfAuditAdd(PerfAuditMetric::MaterialPolicyProductDeferredRows, result.deferredRowsSkipped);
		}
		PerfAuditAdd(PerfAuditMetric::MaterialRangePolicyRowsEvaluated, result.evaluatedRows);
		PerfAuditAdd(PerfAuditMetric::MaterialRangePolicyUniformRows, result.uniformRows);
		if (result.validationChecked) PerfAuditAdd(PerfAuditMetric::MaterialRangePolicyValidations);
		if (result.validationMismatch) PerfAuditAdd(PerfAuditMetric::MaterialRangePolicyMismatches);
	}
}

void NRIRenderer::BuildSceneMaterialPolicyProduct(
	const nri_scene::MaterialBridgeData& source,
	const std::vector<nri_scene::MaterialRowRange>& uniformRanges,
	std::vector<nri_scene::MaterialData>& output)
{
	PerfAuditScope policyAudit(PerfAuditMetric::MaterialPolicyPrelightNs);
	NRIMaterialPolicyAuditScope policyCounters;
	PerfAuditAdd(PerfAuditMetric::MaterialPolicyPrelightCalls);
	PerfAuditAdd(PerfAuditMetric::MaterialPolicyPrelightInputRows, source.materials.size());
	const auto& overlays = GetResolvedLightOverlaySet();
	const auto* actors = nri_material_policy::HasActorMaterialOverrideRules(overlays) ?
		&GetActorMaterialOverrideMapForFrame() : nullptr;
	MaterialPolicyContext context = {
		mSceneLights, overlays, actors, source,
		std::clamp((float)nri_ptglowblend, 0.0f, 3.0f),
		std::clamp((float)nri_ptfullbrightboost, 0.50f, 8.00f),
		&GetResolvedFluidOverlaySet()
	};
	BeginPolicy(context, mPrelightMaterialPolicy);
	const auto result = [&]()
	{
		NRICpuDetailAuditScope productScope(PerfAuditMetric::MaterialPolicyPrelightProductNs);
		return mCombinedMaterialProduct.BuildTransformed(source.materials,
		uniformRanges, output, &context, TransformRow, TransformReference,
		(bool)nri_ptmaterialrangesvalidate || (bool)nri_ptmaterialpolicyvalidate, context.snapshot != nullptr);
	}();
	if (context.snapshot != nullptr && !result.fullRebuild && result.copiedRows < source.materials.size())
	{
		PerfAuditAdd(PerfAuditMetric::MaterialPolicyOutputRowsReused, source.materials.size() - result.copiedRows);
		if ((bool)nri_ptmaterialpolicyvalidate && result.validationChecked)
			PerfAuditAdd(PerfAuditMetric::MaterialPolicyOutputHitValidationChecks);
	}
	FinishPolicy(context, result, "prelight");
	AuditMaterialProduct(result);
	PerfAuditAdd(PerfAuditMetric::SceneTexturesGpuMaterialRowsCopied, result.copiedRows);
	PerfAuditAdd(PerfAuditMetric::SceneTexturesGpuMaterialBytesCopied, result.copiedRows * sizeof(nri_scene::MaterialData));
	if (result.validationMismatch)
		Printf("NRI PT material ranges: stage=prelight mismatch=1 action=reference-fallback-quarantine\n");
}

NRIMaterialProductPatchResult NRIRenderer::RefreshCombinedMaterialProduct(
	const nri_scene::MaterialBridgeData& source,
	size_t staticCount,
	size_t persistentCount,
	const std::vector<uint32_t>& deferredIndices,
	std::vector<nri_scene::MaterialData>& combined,
	std::vector<nri_scene::MaterialData>& persistent,
	std::vector<nri_scene::MaterialData>& dynamic,
	const std::vector<nri_scene::MaterialRowRange>* uniformRanges)
{
	PerfAuditScope policyAudit(PerfAuditMetric::MaterialPolicyPostlightNs);
	NRIMaterialPolicyAuditScope policyCounters;
	PerfAuditAdd(PerfAuditMetric::MaterialPolicyPostlightCalls);
	PerfAuditAdd(PerfAuditMetric::MaterialPolicyPostlightInputRows, source.materials.size());
	const auto& overlays = GetResolvedLightOverlaySet();
	const auto* actors = nri_material_policy::HasActorMaterialOverrideRules(overlays) ?
		&GetActorMaterialOverrideMapForFrame() : nullptr;
	MaterialPolicyContext context = {
		mSceneLights, overlays, actors, source,
		std::clamp((float)nri_ptglowblend, 0.0f, 3.0f),
		std::clamp((float)nri_ptfullbrightboost, 0.50f, 8.00f)
	};
	BeginPolicy(context, mPostlightMaterialPolicy);
	const auto result = [&]()
	{
		NRICpuDetailAuditScope productScope(PerfAuditMetric::MaterialPolicyPostlightProductNs);
		return mCombinedMaterialProduct.Refresh(
		source.materials, staticCount, persistentCount, deferredIndices,
		combined, persistent, dynamic, &context, TransformRow, TransformReference,
		(bool)nri_ptmaterialpatch, (bool)nri_ptmaterialpatchvalidate || (bool)nri_ptmaterialpolicyvalidate,
		uniformRanges, (bool)nri_ptmaterialrangesvalidate || (bool)nri_ptmaterialpolicyvalidate);
	}();
	FinishPolicy(context, result, "postlight");
	if (result.valid && (bool)nri_ptmaterialrangesvalidate)
		ValidateVoxelMaterialSharingProduct(source, combined, staticCount, true, &deferredIndices);
	AuditMaterialProduct(result);
	if (nri_ptscenestats || result.validationChecked || result.validationMismatch)
	{
		Printf("NRI PT material patch: rows=%zu evaluated=%zu changed=%zu copied=%zu ranges=%zu preserved=%zu full=%u validation=%u mismatch=%u quarantined=%u\n",
			source.materials.size(), result.evaluatedRows, result.changedRows,
			result.copiedRows, result.dirtyRanges, result.preservedRows,
			(uint32_t)result.fullRebuild, (uint32_t)result.validationChecked,
			(uint32_t)result.validationMismatch, (uint32_t)mCombinedMaterialProduct.Quarantined());
	}
	return result;
}
