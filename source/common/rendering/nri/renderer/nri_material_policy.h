#pragma once

#include "nri_scene_lights.h"
#include "nri_actor_material_override_cache.h"
#include "../scene/nri_material_bridge.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

struct ResolvedLightOverlaySet;

namespace nri_material_policy
{
	// The actor-dependent inputs which can change the material bridge result for
	// one sprite surface. This intentionally contains no actor identity so callers
	// can compare equivalent presentations owned by different actors.
	struct ActorMaterialPresentationPolicy
	{
		ActorMaterialOverrideState overrideState = {};
		uint32_t resolvedGeneration = 0;
		uint32_t actorOverlayRuleCount = 0;
		uint32_t actorOverlayRuleIds[nri_scene::MaxActorOverlayRuleIdsPerSurface] = {};
	};

	bool HasActorMaterialOverrideRules(const ResolvedLightOverlaySet& resolved);
	bool HasActorFullbrightOverrides(const ResolvedLightOverlaySet& resolved);
	void BuildActorMaterialOverrideMap(
		const ResolvedLightOverlaySet& resolved,
		ActorMaterialOverrideMap& outOverrides);
	const ActorMaterialOverrideMap& GetActorMaterialOverrideMapForFrame(
		const ResolvedLightOverlaySet& resolved,
		uint32_t frameIndex,
		ActorMaterialOverrideCache& cache,
		bool& outBuilt);

	void ApplyActorMaterialOverridesToBuiltMaterials(
		const ActorMaterialOverrideMap& actorOverrides,
		float fullbrightBoost,
		nri_scene::MaterialBridgeData& materials);

	// Begin with the unmodified base row when rebuilding a final material product;
	// these operations preserve unrelated fields and do not undo prior overrides.
	void ApplyEmissiveMaterialOverride(
		const SceneLightSystem& sceneLights,
		const ResolvedLightOverlaySet& resolved,
		float glowmapVisibleBlendScale,
		const nri_scene::MaterialLightingMetadata& metadata,
		nri_scene::MaterialData& inOutGpuMaterial);

	void ApplyActorShadowMaterialOverride(
		const ActorMaterialOverrideMap& actorOverrides,
		float fullbrightBoost,
		const nri_scene::MaterialLightingMetadata& metadata,
		nri_scene::MaterialData& inOutGpuMaterial);

	void ApplyEmissiveMaterialOverrides(
		const SceneLightSystem& sceneLights,
		const ResolvedLightOverlaySet& resolved,
		float glowmapVisibleBlendScale,
		const nri_scene::MaterialBridgeData& materials,
		std::vector<nri_scene::MaterialData>& inOutGpuMaterials);

	void ApplyActorShadowMaterialOverrides(
		const ActorMaterialOverrideMap& actorOverrides,
		float fullbrightBoost,
		const nri_scene::MaterialBridgeData& materials,
		std::vector<nri_scene::MaterialData>& inOutGpuMaterials);

	bool MaterialDataEqual(
		const nri_scene::MaterialData& a,
		const nri_scene::MaterialData& b);

	bool MaterialDataVectorEqual(
		const std::vector<nri_scene::MaterialData>& a,
		const std::vector<nri_scene::MaterialData>& b);

	uint64_t ComputeChunkActorOverrideHash(
		const ActorMaterialOverrideMap& actorOverrides,
		const nri_scene::MaterialBridgeData& materials);

	uint64_t ComputeChunkEmissiveOverrideHash(
		const SceneLightSystem& sceneLights,
		const nri_scene::MaterialBridgeData& materials);
}
