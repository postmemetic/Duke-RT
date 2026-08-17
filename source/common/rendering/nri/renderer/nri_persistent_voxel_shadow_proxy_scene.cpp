#include "nri_persistent_voxel_shadow_proxy.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_voxel_material_slots.h"

#include <cmath>

namespace
{
	bool IsZero3(const float values[3])
	{
		return values[0] == 0.0f && values[1] == 0.0f && values[2] == 0.0f;
	}
}

bool CertifyNRIVoxelShadowProxyPrimitiveSemantics(
	const std::vector<nri_scene::PrimitiveData>& primitives,
	uint32_t materialCount)
{
	if (primitives.empty() || materialCount == 0)
	{
		return false;
	}
	for (const nri_scene::PrimitiveData& primitive : primitives)
	{
		NRIVoxelShadowProxyPrimitiveFacts facts = {};
		facts.flagsSupported = primitive.flags == nri_scene::PrimitiveFlag_None;
		facts.portalFree = primitive.portalIndex == UINT32_MAX;
		facts.materialInRange = nri_scene::IsVoxelPrimitiveMaterialIndexCompatible(
			primitive.materialIndex,
			materialCount);
		if (!CertifyNRIVoxelShadowProxyPrimitiveFacts(facts))
		{
			return false;
		}
	}
	return true;
}

bool CertifyNRIVoxelShadowProxyMaterialClosure(
	const nri_scene::MaterialBridgeData& materials,
	bool hasActorOverlayLights,
	NRIVoxelShadowProxyRejectReason& outReason)
{
	outReason = NRIVoxelShadowProxyRejectReason::None;
	if (hasActorOverlayLights)
	{
		outReason = NRIVoxelShadowProxyRejectReason::ActorOverlay;
		return false;
	}
	if (materials.materials.empty() || materials.materials.size() != materials.lightMetadata.size())
	{
		outReason = NRIVoxelShadowProxyRejectReason::MaterialClosure;
		return false;
	}

	constexpr uint32_t RequiredFlags = nri_scene::MaterialFlag_Indexed | nri_scene::MaterialFlag_PointSampled;
	constexpr uint32_t AllowedFlags = RequiredFlags | nri_scene::MaterialFlag_Sprite;
	for (size_t index = 0; index < materials.materials.size(); ++index)
	{
		const nri_scene::MaterialData& material = materials.materials[index];
		const nri_scene::MaterialLightingMetadata& metadata = materials.lightMetadata[index];
		NRIVoxelShadowProxyMaterialFacts facts = {};
		facts.flagsSupported =
			(material.flags & RequiredFlags) == RequiredFlags &&
			(material.flags & ~AllowedFlags) == 0 &&
			(metadata.materialFlags & RequiredFlags) == RequiredFlags &&
			(metadata.materialFlags & ~AllowedFlags) == 0;
		facts.alphaOpaque =
			std::isfinite(material.alpha) && material.alpha == 1.0f &&
			std::isfinite(metadata.alpha) && metadata.alpha == 1.0f;
		facts.lightingNeutral =
			material.lightingFlags == nri_scene::MaterialLightingFlag_None &&
			metadata.lightingFlags == nri_scene::MaterialLightingFlag_None;
		facts.emissiveFree =
			material.emissiveMode == nri_scene::MaterialEmissiveMode_None &&
			metadata.emissiveMode == nri_scene::MaterialEmissiveMode_None &&
			material.emissiveTextureIndex == UINT32_MAX &&
			metadata.emissiveTextureIndex == UINT32_MAX &&
			metadata.glowmapTextureIndex == UINT32_MAX &&
			material.emissiveIntensity == 0.0f && material.emissiveMaskScale == 0.0f &&
			metadata.emissiveIntensity == 0.0f && metadata.emissiveMaskScale == 0.0f &&
			IsZero3(material.emissiveColor) && IsZero3(metadata.emissiveColor) &&
			IsZero3(metadata.glowColor);
		facts.actorOverlayFree = metadata.actorOverlayRuleCount == 0;
		if (!CertifyNRIVoxelShadowProxyMaterialFacts(facts, outReason))
		{
			return false;
		}
	}
	return true;
}
