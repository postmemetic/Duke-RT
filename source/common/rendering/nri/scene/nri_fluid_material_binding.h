#pragma once

#include "nri_material_bridge.h"

#include <cstdint>
#include <vector>

struct ResolvedFluidOverlaySet;

namespace nri_scene
{
struct FluidMaterialBindingStats
{
	uint32_t materialCount = 0;
	uint32_t eligibleCount = 0;
	uint32_t matchedCount = 0;
	uint32_t disabledCount = 0;
	uint32_t unmatchedCount = 0;
};

// Resolves FLUIDOVR strictly from authored material identity. The sampled
// texture can continue to animate without changing this result.
uint32_t ResolveFluidMaterialBindingIndex(
	const ResolvedFluidOverlaySet& overlays,
	const MaterialLightingMetadata& metadata,
	bool* outEligible = nullptr,
	bool* outDisabled = nullptr);

void ApplyFluidMaterialBindings(
	const ResolvedFluidOverlaySet& overlays,
	const MaterialBridgeData& materials,
	std::vector<MaterialData>& inOutGpuMaterials,
	FluidMaterialBindingStats* outStats = nullptr);

void ApplyFluidMaterialBindings(
	const ResolvedFluidOverlaySet& overlays,
	MaterialBridgeData& inOutMaterials,
	FluidMaterialBindingStats* outStats = nullptr);
}
