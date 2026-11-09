#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

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
		uint64_t revision = 0;
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
		bool mergedOverridesValid = false;
		uint64_t mergedBaseRevision = 0;
		ActorMaterialOverrideMap mergedOverrides;
	};


	// Overlay bits augment the complete base state, including stable-frame policy.
	inline void MergeActorMaterialOverrides(const ActorMaterialOverrideMap* base,
		const ActorOverlayMaterialRuleMap& rules, ActorMaterialOverrideMap& output)
	{
		output.clear();
		if (base != nullptr) output = *base;
		for (const auto& entry : rules)
		{
			uint32_t bits = ActorMaterialOverride_None;
			for (const auto& rule : entry.second) bits |= rule.overrideBits;
			if (bits != ActorMaterialOverride_None) output[entry.first].bits |= bits;
		}
	}

	inline const ActorMaterialOverrideMap& GetMergedActorMaterialOverrides(
		const ActorMaterialOverrideMap* base, uint64_t baseRevision,
		ActorOverlayMaterialRuleCache& cache, bool& built)
	{
		built = !cache.mergedOverridesValid || cache.mergedBaseRevision != baseRevision;
		if (built)
		{
			MergeActorMaterialOverrides(base, cache.rules, cache.mergedOverrides);
			cache.mergedBaseRevision = baseRevision;
			cache.mergedOverridesValid = true;
		}
		return cache.mergedOverrides;
	}
}
