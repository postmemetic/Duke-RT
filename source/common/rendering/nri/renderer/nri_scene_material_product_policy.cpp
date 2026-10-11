#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_scene_material_product.h"

#include "lightoverlay.h"
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
	};

	void TransformRow(void* opaque, size_t index, nri_scene::MaterialData& row)
	{
		const auto& context = *static_cast<MaterialPolicyContext*>(opaque);
		if (index >= context.source.lightMetadata.size()) return;
		const auto& metadata = context.source.lightMetadata[index];
		nri_material_policy::ApplyEmissiveMaterialOverride(context.lights, context.overlays, context.glowBlend, metadata, row);
		if (context.actors != nullptr)
			nri_material_policy::ApplyActorShadowMaterialOverride(*context.actors, context.fullbrightBoost, metadata, row);
	}

	void TransformReference(void* opaque, std::vector<nri_scene::MaterialData>& rows)
	{
		const auto& context = *static_cast<MaterialPolicyContext*>(opaque);
		nri_material_policy::ApplyEmissiveMaterialOverrides(context.lights, context.overlays, context.glowBlend, context.source, rows);
		if (context.actors != nullptr)
			nri_material_policy::ApplyActorShadowMaterialOverrides(*context.actors, context.fullbrightBoost, context.source, rows);
	}
}

NRIMaterialProductPatchResult NRIRenderer::RefreshCombinedMaterialProduct(
	const nri_scene::MaterialBridgeData& source,
	size_t staticCount,
	size_t persistentCount,
	const std::vector<uint32_t>& deferredIndices,
	std::vector<nri_scene::MaterialData>& combined,
	std::vector<nri_scene::MaterialData>& persistent,
	std::vector<nri_scene::MaterialData>& dynamic)
{
	const auto& overlays = GetResolvedLightOverlaySet();
	const auto* actors = nri_material_policy::HasActorMaterialOverrideRules(overlays) ?
		&GetActorMaterialOverrideMapForFrame() : nullptr;
	MaterialPolicyContext context = {
		mSceneLights, overlays, actors, source,
		std::clamp((float)nri_ptglowblend, 0.0f, 3.0f),
		std::clamp((float)nri_ptfullbrightboost, 0.50f, 8.00f)
	};
	const auto result = mCombinedMaterialProduct.Refresh(
		source.materials, staticCount, persistentCount, deferredIndices,
		combined, persistent, dynamic, &context, TransformRow, TransformReference,
		(bool)nri_ptmaterialpatch, (bool)nri_ptmaterialpatchvalidate);
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
