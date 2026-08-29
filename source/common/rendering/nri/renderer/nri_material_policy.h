#pragma once

#include "nri_scene_lights.h"
#include "../scene/nri_material_bridge.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

struct ResolvedLightOverlaySet;

namespace nri_material_policy
{
	enum ActorMaterialOverrideBits : uint32_t
	{
		ActorMaterialOverride_None = 0,
		ActorMaterialOverride_NoShadowReceive = 1u << 0,
		ActorMaterialOverride_NoShadowCast = 1u << 1,
		ActorMaterialOverride_Fullbright = 1u << 2,
		ActorMaterialOverride_SmokeForeground = 1u << 3,
	};

	struct ActorMaterialOverrideState
	{
		uint32_t bits = ActorMaterialOverride_None;
		uint32_t emissiveStableFrames = 0;

		bool Empty() const
		{
			return bits == ActorMaterialOverride_None && emissiveStableFrames == 0;
		}
	};

	using ActorMaterialOverrideMap = std::unordered_map<int32_t, ActorMaterialOverrideState>;

	struct ActorOverlayMaterialRule
	{
		uint32_t ruleId = 0;
		bool hasTileFilter = false;
		uint32_t tileFilter = 0;
		uint32_t overrideBits = ActorMaterialOverride_None;
	};

	using ActorOverlayMaterialRuleMap = std::unordered_map<int32_t, std::vector<ActorOverlayMaterialRule>>;

	struct ActorMaterialOverrideCache
	{
		bool valid = false;
		uint32_t frameIndex = UINT32_MAX;
		uint32_t resolvedGeneration = 0;
		bool hasFullbrightOverrides = false;
		ActorMaterialOverrideMap overrides;
	};

	struct ActorOverlayMaterialRuleCache
	{
		bool valid = false;
		uint32_t frameIndex = UINT32_MAX;
		uint32_t resolvedGeneration = 0;
		uint32_t actorRuleCount = 0;
		uint32_t totalRuleCount = 0;
		ActorOverlayMaterialRuleMap rules;
	};

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
