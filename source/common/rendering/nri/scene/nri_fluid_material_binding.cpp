#include "nri_fluid_material_binding.h"

#include "fluidoverlay.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <algorithm>

namespace nri_scene
{
namespace
{
	bool IsEligible(const MaterialLightingMetadata& metadata)
	{
		const bool eligibleSource =
			metadata.sourceType == SurfaceSourceType::MapFloorSection ||
			metadata.sourceType == SurfaceSourceType::MapCeilingSection ||
			metadata.sourceType == SurfaceSourceType::MapWallBand;
		const uint32_t rejectedFlags =
			MaterialFlag_Sprite |
			MaterialFlag_Mirror |
			MaterialFlag_Sky |
			MaterialFlag_Portal |
			MaterialFlag_PlainMirror;
		return eligibleSource && (metadata.materialFlags & rejectedFlags) == 0u;
	}

	FluidOverlaySurfaceClass ResolveSurfaceClass(SurfaceSourceType sourceType)
	{
		return sourceType == SurfaceSourceType::MapWallBand ?
			FluidOverlaySurfaceClass::Wall : FluidOverlaySurfaceClass::Flat;
	}

	int ResolveLegacyTile(uint32_t textureId)
	{
		if (textureId == UINT32_MAX ||
			textureId < (uint32_t)firstarttile ||
			textureId > (uint32_t)(firstarttile + maxarttile))
		{
			return -1;
		}
		return legacyTileNum(FSetTextureID((int)textureId));
	}

	int ResolveAnimationFamily(uint32_t textureId, int legacyTile)
	{
		if (legacyTile < 0 || textureId == UINT32_MAX)
		{
			return legacyTile;
		}

		const auto& picanm = GetExtInfo(FSetTextureID((int)textureId)).picanm;
		return picanm.type() == PICANM_ANIMTYPE_BACK ?
			legacyTile - (int)picanm.num : legacyTile;
	}

	FluidOverlayMaterialIdentity BuildIdentity(const MaterialLightingMetadata& metadata)
	{
		FluidOverlayMaterialIdentity identity;
		identity.tile = ResolveLegacyTile(metadata.authoredTextureId);
		identity.tileFamily = ResolveAnimationFamily(metadata.authoredTextureId, identity.tile);
		identity.palette = (int)metadata.paletteIndex;
		identity.surfaceClass = ResolveSurfaceClass(metadata.sourceType);
		if (metadata.authoredTextureId != UINT32_MAX)
		{
			if (FGameTexture* texture = TexMan.GameByIndex((int)metadata.authoredTextureId))
			{
				identity.textureName = texture->GetName();
			}
		}
		return identity;
	}
}

uint32_t ResolveFluidMaterialBindingIndex(
	const ResolvedFluidOverlaySet& overlays,
	const MaterialLightingMetadata& metadata,
	bool* outEligible,
	bool* outDisabled)
{
	const bool eligible = IsEligible(metadata);
	if (outEligible != nullptr)
	{
		*outEligible = eligible;
	}
	if (outDisabled != nullptr)
	{
		*outDisabled = false;
	}
	if (!eligible || metadata.authoredTextureId == UINT32_MAX)
	{
		return UINT32_MAX;
	}

	FluidOverlayMaterialMatch match;
	ResolveFluidOverlayMaterial(overlays, BuildIdentity(metadata), match);
	if (!match.matched)
	{
		return UINT32_MAX;
	}
	if (outDisabled != nullptr)
	{
		*outDisabled = match.disabled;
	}
	return match.disabled ? UINT32_MAX : match.bindingIndex;
}

void ApplyFluidMaterialBindings(
	const ResolvedFluidOverlaySet& overlays,
	const MaterialBridgeData& materials,
	std::vector<MaterialData>& inOutGpuMaterials,
	FluidMaterialBindingStats* outStats)
{
	FluidMaterialBindingStats stats;
	const size_t materialCount = std::min(inOutGpuMaterials.size(), materials.lightMetadata.size());
	stats.materialCount = (uint32_t)materialCount;
	for (size_t materialIndex = 0; materialIndex < inOutGpuMaterials.size(); ++materialIndex)
	{
		inOutGpuMaterials[materialIndex].fluidBindingIndex = UINT32_MAX;
		if (materialIndex >= materialCount)
		{
			continue;
		}

		bool eligible = false;
		bool disabled = false;
		const uint32_t bindingIndex = ResolveFluidMaterialBindingIndex(
			overlays,
			materials.lightMetadata[materialIndex],
			&eligible,
			&disabled);
		stats.eligibleCount += eligible ? 1u : 0u;
		stats.disabledCount += disabled ? 1u : 0u;
		if (bindingIndex != UINT32_MAX)
		{
			inOutGpuMaterials[materialIndex].fluidBindingIndex = bindingIndex;
			stats.matchedCount++;
		}
		else if (eligible && !disabled)
		{
			stats.unmatchedCount++;
		}
	}
	if (outStats != nullptr)
	{
		*outStats = stats;
	}
}

void ApplyFluidMaterialBindings(
	const ResolvedFluidOverlaySet& overlays,
	MaterialBridgeData& inOutMaterials,
	FluidMaterialBindingStats* outStats)
{
	ApplyFluidMaterialBindings(overlays, inOutMaterials, inOutMaterials.materials, outStats);
}
}
