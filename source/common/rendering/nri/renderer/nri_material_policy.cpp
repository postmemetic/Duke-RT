#include "nri_material_policy.h"
#include "nri_cvars.h"

#include "nri_renderer.h"
#include "nri_runtime_mutation_trace.h"
#include "../system/nri_renderdevice.h"
#include "../scene/nri_hash.h"

#include "c_cvars.h"
#include "coreactor.h"
#include "lightoverlay.h"
#include "texinfo.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>


namespace
{
	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	float GetFullbrightRoughnessHint(uint32_t materialFlags)
	{
		if ((materialFlags & nri_scene::MaterialFlag_Sprite) != 0)
		{
			return 0.45f;
		}
		if ((materialFlags & nri_scene::MaterialFlag_Flat) != 0)
		{
			return 0.60f;
		}
		return 0.55f;
	}

	void ApplyFullbrightMaterialOverride(nri_scene::MaterialData& material, float fullbrightBoost)
	{
		material.flags |= nri_scene::MaterialFlag_Fullbright;
		material.lightLevel = 1.0f;
		material.roughnessHint = GetFullbrightRoughnessHint(material.flags);
		material.lightingFlags |= nri_scene::MaterialLightingFlag_MaterialFullbright;
		material.emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
		material.emissiveTextureIndex = material.textureIndex;
		material.emissiveIntensity = 1.0f;
		material.emissiveMaskScale = 1.0f;
		material.emissiveReserved = fullbrightBoost;
		material.emissiveColor[0] = 1.0f;
		material.emissiveColor[1] = 1.0f;
		material.emissiveColor[2] = 1.0f;
	}

	float GetFullbrightBoostScale()
	{
		return std::clamp((float)nri_ptfullbrightboost, 0.50f, 8.00f);
	}

	float GetGlowmapVisibleBlendScale()
	{
		return std::clamp((float)nri_ptglowblend, 0.0f, 3.0f);
	}

	uint32_t ResolveLegacyTileTextureId(int tile)
	{
		if (tile < 0)
		{
			return 0u;
		}

		const FTextureID textureId = tileGetTextureID(tile);
		return textureId.isValid() ? (uint32_t)textureId.GetIndex() : 0u;
	}

	std::string NormalizeLightOverlayTextureSelector(const char* value)
	{
		std::string normalized = value != nullptr ? value : "";
		for (char& c : normalized)
		{
			c = (char)std::tolower((unsigned char)c);
		}

		const size_t slash = normalized.find_last_of("/\\");
		const size_t dot = normalized.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		{
			normalized.erase(dot);
		}
		return normalized;
	}

	std::string NormalizeMaterialTextureName(const FGameTexture* texture)
	{
		return NormalizeLightOverlayTextureSelector(texture != nullptr ? texture->GetName().GetChars() : "");
	}

	bool MaterialTextureIdMatches(const nri_scene::MaterialLightingMetadata& metadata, uint32_t textureId)
	{
		if (textureId == 0u)
		{
			return false;
		}
		return metadata.textureId == textureId || metadata.baseTextureId == textureId;
	}

	bool MaterialTextureIdInRange(const nri_scene::MaterialLightingMetadata& metadata, uint32_t firstTextureId, uint32_t lastTextureId)
	{
		if (firstTextureId == 0u || lastTextureId == 0u)
		{
			return false;
		}
		if (firstTextureId > lastTextureId)
		{
			std::swap(firstTextureId, lastTextureId);
		}
		return
			(metadata.textureId >= firstTextureId && metadata.textureId <= lastTextureId) ||
			(metadata.baseTextureId >= firstTextureId && metadata.baseTextureId <= lastTextureId);
	}

	bool EmissiveMaterialResponseRuleMatchesMaterial(
		const ResolvedLightOverlayEmissiveMaterialResponseRule& rule,
		const nri_scene::MaterialLightingMetadata& metadata)
	{
		for (int tile : rule.tileFilters)
		{
			if (MaterialTextureIdMatches(metadata, ResolveLegacyTileTextureId(tile)))
			{
				return true;
			}
		}

		for (const auto& range : rule.tileRanges)
		{
			if (MaterialTextureIdInRange(metadata, ResolveLegacyTileTextureId(range.first), ResolveLegacyTileTextureId(range.last)))
			{
				return true;
			}
		}

		if (rule.textureNames.Size() != 0)
		{
			const std::string materialName = NormalizeMaterialTextureName(metadata.texture);
			for (const auto& textureName : rule.textureNames)
			{
				if (materialName == NormalizeLightOverlayTextureSelector(textureName.GetChars()))
				{
					return true;
				}
			}
		}

		return false;
	}

	float ResolveVisibleGlowBlendScale(
		const ResolvedLightOverlaySet& resolved,
		const nri_scene::MaterialLightingMetadata& metadata,
		float fallbackScale)
	{
		float scale = fallbackScale;
		for (const auto& rule : resolved.emissiveMaterialResponseRules)
		{
			if (rule.hasVisibleGlowBlend && EmissiveMaterialResponseRuleMatchesMaterial(rule, metadata))
			{
				scale = std::clamp(rule.visibleGlowBlend, 0.0f, 3.0f);
			}
		}
		return scale;
	}

	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	size_t GetMaterialBuildTraceSlotIndex(NRIRenderer::MaterialBuildTraceSlot slot)
	{
		return (size_t)slot;
	}

	const char* GetMaterialBuildTraceSlotNameInternal(NRIRenderer::MaterialBuildTraceSlot slot)
	{
		switch (slot)
		{
		case NRIRenderer::MaterialBuildTraceSlot::DynamicLive: return "dynamic_live";
		case NRIRenderer::MaterialBuildTraceSlot::SceneLightMergedDynamic: return "scene_light_merged_dynamic";
		case NRIRenderer::MaterialBuildTraceSlot::LocalPlayerReflection: return "local_player_reflection";
		case NRIRenderer::MaterialBuildTraceSlot::DynamicWithPersistentEmissive: return "dynamic_with_persistent_emissive";
		case NRIRenderer::MaterialBuildTraceSlot::SceneLightMergedPersistent: return "scene_light_merged_persistent";
		case NRIRenderer::MaterialBuildTraceSlot::CapturedScene: return "captured_scene";
		case NRIRenderer::MaterialBuildTraceSlot::PersistentEmissiveCachePrune: return "persistent_emissive_cache_prune";
		case NRIRenderer::MaterialBuildTraceSlot::PersistentEmissiveCacheRebuild: return "persistent_emissive_cache_rebuild";
		case NRIRenderer::MaterialBuildTraceSlot::StaticMapAnimChunk: return "static_map_anim_chunk";
		case NRIRenderer::MaterialBuildTraceSlot::StaticMapChunk: return "static_map_chunk";
		case NRIRenderer::MaterialBuildTraceSlot::RuntimeMutationChunk: return "runtime_mutation_chunk";
		case NRIRenderer::MaterialBuildTraceSlot::ResidentRuntimeMutationChunk: return "resident_runtime_mutation_chunk";
		case NRIRenderer::MaterialBuildTraceSlot::ResidentRuntimeMutationChunkRecover: return "resident_runtime_mutation_chunk_recover";
		case NRIRenderer::MaterialBuildTraceSlot::RuntimeSpaceLinkChunk: return "runtime_space_link_chunk";
		case NRIRenderer::MaterialBuildTraceSlot::Unknown: return "unknown";
		case NRIRenderer::MaterialBuildTraceSlot::Count: break;
		}

		return "unknown";
	}

	struct MaterialTextureAttributionCounts
	{
		uint32_t materialCount = 0;
		uint32_t actorMaterialCount = 0;
		uint32_t textureCount = 0;
		uint32_t baseTextureCount = 0;
		uint32_t glowTextureCount = 0;
		uint32_t normalTextureCount = 0;
		uint32_t metallicTextureCount = 0;
		uint32_t roughnessTextureCount = 0;
		uint32_t emissiveTextureCount = 0;
	};

	MaterialTextureAttributionCounts GatherMaterialTextureAttribution(
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<nri_scene::MaterialLightingMetadata>& lightMetadata,
		size_t textureCount)
	{
		MaterialTextureAttributionCounts counts = {};
		counts.materialCount = (uint32_t)materials.size();
		counts.textureCount = (uint32_t)textureCount;

		std::unordered_set<uint32_t> baseTextures;
		std::unordered_set<uint32_t> glowTextures;
		std::unordered_set<uint32_t> normalTextures;
		std::unordered_set<uint32_t> metallicTextures;
		std::unordered_set<uint32_t> roughnessTextures;
		std::unordered_set<uint32_t> emissiveTextures;
		baseTextures.reserve(materials.size());
		glowTextures.reserve(lightMetadata.size());
		normalTextures.reserve(materials.size());
		metallicTextures.reserve(materials.size());
		roughnessTextures.reserve(materials.size());
		emissiveTextures.reserve(materials.size());

		const auto addTextureIndex = [textureCount](std::unordered_set<uint32_t>& destination, uint32_t textureIndex)
		{
			if (textureIndex != UINT32_MAX && (size_t)textureIndex < textureCount)
			{
				destination.insert(textureIndex);
			}
		};

		for (uint32_t materialIndex = 0; materialIndex < (uint32_t)materials.size(); ++materialIndex)
		{
			const auto& material = materials[materialIndex];
			addTextureIndex(baseTextures, material.textureIndex);
			addTextureIndex(normalTextures, material.normalTextureIndex);
			addTextureIndex(metallicTextures, material.metallicTextureIndex);
			addTextureIndex(roughnessTextures, material.roughnessTextureIndex);
			addTextureIndex(emissiveTextures, material.emissiveTextureIndex);
			if (materialIndex < lightMetadata.size())
			{
				const auto& metadata = lightMetadata[materialIndex];
				addTextureIndex(glowTextures, metadata.glowmapTextureIndex);
				if (metadata.actorIndex >= 0)
				{
					counts.actorMaterialCount++;
				}
			}
		}

		counts.baseTextureCount = (uint32_t)baseTextures.size();
		counts.glowTextureCount = (uint32_t)glowTextures.size();
		counts.normalTextureCount = (uint32_t)normalTextures.size();
		counts.metallicTextureCount = (uint32_t)metallicTextures.size();
		counts.roughnessTextureCount = (uint32_t)roughnessTextures.size();
		counts.emissiveTextureCount = (uint32_t)emissiveTextures.size();
		return counts;
	}

	void AccumulateMaterialTextureAttribution(NRIRenderer::MaterialBuildTraceEntry& entry, const MaterialTextureAttributionCounts& counts)
	{
		entry.materialCount += counts.materialCount;
		entry.actorMaterialCount += counts.actorMaterialCount;
		entry.textureCount += counts.textureCount;
		entry.baseTextureCount += counts.baseTextureCount;
		entry.glowTextureCount += counts.glowTextureCount;
		entry.normalTextureCount += counts.normalTextureCount;
		entry.metallicTextureCount += counts.metallicTextureCount;
		entry.roughnessTextureCount += counts.roughnessTextureCount;
		entry.emissiveTextureCount += counts.emissiveTextureCount;
	}

	uint64_t HashLightOverlayText(uint64_t hash, const char* text)
	{
		if (text == nullptr)
		{
			return hash;
		}

		for (const unsigned char* cursor = (const unsigned char*)text; *cursor != '\0'; ++cursor)
		{
			hash ^= (uint64_t)(*cursor);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	uint32_t BuildResolvedLightOverlayRuleId(const char* id, const char* classOrMapName, const LightOverlaySourceLocation& source)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashLightOverlayText(hash, id);
		hash = HashLightOverlayText(hash, classOrMapName);
		hash = HashLightOverlayText(hash, source.sourceName.GetChars());
		hash ^= (uint64_t)source.orderIndex + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		const uint32_t ruleId = (uint32_t)(hash ^ (hash >> 32));
		return ruleId != 0 ? ruleId : 1u;
	}

	uint32_t BuildActorOverlayRuleId(const ResolvedLightOverlayActorRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.actorClassName.GetChars(), rule.source);
	}

	bool IsSupportedActorOverlayRule(const ResolvedLightOverlayActorRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0;
	}

	struct ActorOverlayStampStats
	{
		uint32_t stampedSpriteSurfaces = 0;
		uint32_t skippedNonSpriteSurfaces = 0;
		uint32_t skippedNoActorSurfaces = 0;
	};

	void BuildActorOverlayMaterialRules(
		const ResolvedLightOverlaySet& resolved,
		nri_material_policy::ActorOverlayMaterialRuleMap& outRules)
	{
		if (resolved.actorRules.Size() == 0)
		{
			return;
		}

		TSpriteIterator<DCoreActor> it;
		while (auto actor = it.Next())
		{
			if (actor == nullptr ||
				!actor->exists() ||
				(actor->ObjectFlags & OF_EuthanizeMe) != 0)
			{
				continue;
			}

			PClass* actorClass = actor->GetClass();
			if (actorClass == nullptr)
			{
				continue;
			}

			auto& actorRules = outRules[(int32_t)actor->GetIndex()];
			for (const auto& resolvedRule : resolved.actorRules)
			{
				if (!resolvedRule.actorClassResolved ||
					resolvedRule.actorClass == nullptr ||
					(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)) ||
					!IsSupportedActorOverlayRule(resolvedRule) ||
					resolvedRule.intensity <= 0.0f ||
					resolvedRule.radius <= 0.0f)
				{
					continue;
				}

				nri_material_policy::ActorOverlayMaterialRule rule = {};
				rule.ruleId = BuildActorOverlayRuleId(resolvedRule);
				rule.hasTileFilter = resolvedRule.hasTileFilter;
				rule.tileFilter = resolvedRule.hasTileFilter && resolvedRule.tileFilter >= 0 ? (uint32_t)resolvedRule.tileFilter : 0u;
				if (resolvedRule.hasShadowReceive && !resolvedRule.shadowReceive)
				{
					rule.overrideBits |= nri_material_policy::ActorMaterialOverride_NoShadowReceive;
				}
				if (resolvedRule.hasShadowCast && !resolvedRule.shadowCast)
				{
					rule.overrideBits |= nri_material_policy::ActorMaterialOverride_NoShadowCast;
				}
				if (resolvedRule.hasFullbright && resolvedRule.fullbright)
				{
					rule.overrideBits |= nri_material_policy::ActorMaterialOverride_Fullbright;
				}
				actorRules.push_back(rule);
			}

			if (actorRules.empty())
			{
				outRules.erase((int32_t)actor->GetIndex());
			}
		}
	}

	const nri_material_policy::ActorOverlayMaterialRuleMap& GetActorOverlayMaterialRulesForFrame(
		const ResolvedLightOverlaySet& resolved,
		uint32_t frameIndex,
		nri_material_policy::ActorOverlayMaterialRuleCache& cache,
		bool& outBuilt,
		bool& outCacheHit)
	{
		outBuilt = false;
		outCacheHit = false;
		const uint32_t actorRuleCount = (uint32_t)resolved.actorRules.Size();
		if (cache.valid &&
			cache.frameIndex == frameIndex &&
			cache.resolvedGeneration == resolved.resolvedGeneration &&
			cache.actorRuleCount == actorRuleCount)
		{
			outCacheHit = true;
			return cache.rules;
		}

		cache.valid = true;
		cache.frameIndex = frameIndex;
		cache.resolvedGeneration = resolved.resolvedGeneration;
		cache.actorRuleCount = actorRuleCount;
		cache.totalRuleCount = 0;
		cache.rules.clear();
		if (actorRuleCount > 0)
		{
			BuildActorOverlayMaterialRules(resolved, cache.rules);
			for (const auto& entry : cache.rules)
			{
				cache.totalRuleCount += (uint32_t)entry.second.size();
			}
			outBuilt = true;
		}
		return cache.rules;
	}

	void StampActorOverlayRuleIdsOnSurface(
		const std::vector<nri_material_policy::ActorOverlayMaterialRule>& actorRules,
		nri_scene::SurfaceRef& surface,
		ActorOverlayStampStats& stats)
	{
		uint32_t textureId = 0;
		if (surface.material.texture != nullptr)
		{
			const FTextureID id = surface.material.texture->GetID();
			textureId = id.isValid() ? (uint32_t)id.GetIndex() : 0u;
		}

		surface.provenance.actorOverlayRuleCount = 0;
		for (const auto& rule : actorRules)
		{
			if (rule.hasTileFilter && rule.tileFilter != textureId)
			{
				continue;
			}
			if (surface.provenance.actorOverlayRuleCount >= nri_scene::MaxActorOverlayRuleIdsPerSurface)
			{
				break;
			}

			surface.provenance.actorOverlayRuleIds[surface.provenance.actorOverlayRuleCount++] = rule.ruleId;
		}
		if (surface.provenance.actorOverlayRuleCount > 0)
		{
			stats.stampedSpriteSurfaces++;
		}
	}

	void StampActorOverlayRuleIdsOnSceneView(
		const nri_material_policy::ActorOverlayMaterialRuleMap& actorOverlayRules,
		nri_scene::SceneView& sceneView,
		ActorOverlayStampStats& stats)
	{
		auto stampSurfaces = [&actorOverlayRules, &stats](auto& surfaces)
		{
			for (auto& surface : surfaces)
			{
				if (surface.provenance.actorIndex < 0)
				{
					stats.skippedNoActorSurfaces++;
					continue;
				}
				if ((surface.material.flags & nri_scene::MaterialFlag_Sprite) == 0)
				{
					stats.skippedNonSpriteSurfaces++;
					continue;
				}

				const auto it = actorOverlayRules.find(surface.provenance.actorIndex);
				if (it != actorOverlayRules.end())
				{
					StampActorOverlayRuleIdsOnSurface(it->second, surface, stats);
				}
			}
		};

		stampSurfaces(sceneView.opaqueWalls);
		stampSurfaces(sceneView.opaqueFlats);
		stampSurfaces(sceneView.opaqueSprites);
	}
}

const char* NRIRenderer::GetMaterialBuildTraceSlotName(MaterialBuildTraceSlot slot)
{
	return GetMaterialBuildTraceSlotNameInternal(slot);
}

NRIRenderer::MaterialBuildTraceSlot NRIRenderer::ResolveMaterialBuildTraceSlot(const char* traceLabel)
{
	if (traceLabel == nullptr || traceLabel[0] == '\0')
	{
		return MaterialBuildTraceSlot::Unknown;
	}

	for (size_t index = 0; index < GetMaterialBuildTraceSlotIndex(MaterialBuildTraceSlot::Count); ++index)
	{
		const MaterialBuildTraceSlot slot = (MaterialBuildTraceSlot)index;
		if (std::strcmp(traceLabel, GetMaterialBuildTraceSlotNameInternal(slot)) == 0)
		{
			return slot;
		}
	}

	return MaterialBuildTraceSlot::Unknown;
}

const nri_material_policy::ActorMaterialOverrideMap& NRIRenderer::GetActorMaterialOverrideMapForFrame(MaterialBuildTraceSlot traceSlot)
{
	const ResolvedLightOverlaySet& resolvedLightOverlays = GetResolvedLightOverlaySet();
	auto& materialTraceEntry = mLastPerfShellTraceStats.materialBuildByLabel[GetMaterialBuildTraceSlotIndex(traceSlot)];
	bool built = false;
	if (nri_runtime_mutation::ShouldTracePtPerf())
	{
		const auto start = std::chrono::steady_clock::now();
		const auto& overrides = nri_material_policy::GetActorMaterialOverrideMapForFrame(resolvedLightOverlays, mFrameIndex, mActorMaterialOverrideCache, built);
		const double elapsedMs = DurationMs(start, std::chrono::steady_clock::now());
		if (built)
		{
			mLastPerfShellTraceStats.actorOverrideMapBuildCalls++;
			mLastPerfShellTraceStats.actorOverrideMapBuildMs += elapsedMs;
			materialTraceEntry.overrideBuildCalls++;
			materialTraceEntry.overrideBuildMs += elapsedMs;
		}
		return overrides;
	}

	const auto& overrides = nri_material_policy::GetActorMaterialOverrideMapForFrame(resolvedLightOverlays, mFrameIndex, mActorMaterialOverrideCache, built);
	if (built)
	{
		mLastPerfShellTraceStats.actorOverrideMapBuildCalls++;
		materialTraceEntry.overrideBuildCalls++;
	}
	return overrides;
}

void NRIRenderer::ApplyEmissiveMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials) const
{
	nri_material_policy::ApplyEmissiveMaterialOverrides(mSceneLights, GetResolvedLightOverlaySet(), GetGlowmapVisibleBlendScale(), materials, inOutGpuMaterials);
}

void NRIRenderer::BuildMaterialsWithActorOverrides(nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& outMaterials, const char* traceLabel)
{
	mLastPerfShellTraceStats.materialBuildCalls++;
	const bool tracePerf = nri_runtime_mutation::ShouldTracePtPerf();
	const MaterialBuildTraceSlot materialTraceSlot = ResolveMaterialBuildTraceSlot(traceLabel);
	auto& materialTraceEntry = mLastPerfShellTraceStats.materialBuildByLabel[GetMaterialBuildTraceSlotIndex(materialTraceSlot)];
	materialTraceEntry.calls++;
	const ResolvedLightOverlaySet& resolvedLightOverlays = GetResolvedLightOverlaySet();
	const bool useActorRuleFrameCaches = mFrameBuffer == nullptr || !mFrameBuffer->IsPathTracingLevelPreloadPending();
	const bool hasActorMaterialRules = nri_material_policy::HasActorMaterialOverrideRules(resolvedLightOverlays);
	const nri_material_policy::ActorMaterialOverrideMap* actorOverridesForBuild = nullptr;
	nri_material_policy::ActorMaterialOverrideMap transientActorOverrides;
	nri_material_policy::ActorMaterialOverrideMap mergedActorOverridesForBuild;
	if (hasActorMaterialRules)
	{
		const auto actorOverrideStart = std::chrono::steady_clock::now();
		const nri_material_policy::ActorMaterialOverrideMap* actorOverrides = nullptr;
		bool transientActorOverridesBuilt = false;
		if (useActorRuleFrameCaches)
		{
			actorOverrides = &GetActorMaterialOverrideMapForFrame(materialTraceSlot);
		}
		else
		{
			nri_material_policy::BuildActorMaterialOverrideMap(resolvedLightOverlays, transientActorOverrides);
			actorOverrides = &transientActorOverrides;
			transientActorOverridesBuilt = true;
		}
		if (tracePerf && transientActorOverridesBuilt)
		{
			const double elapsedMs = DurationMs(actorOverrideStart, std::chrono::steady_clock::now());
			mLastPerfShellTraceStats.actorOverrideMapBuildCalls++;
			mLastPerfShellTraceStats.actorOverrideMapBuildMs += elapsedMs;
			materialTraceEntry.overrideBuildCalls++;
			materialTraceEntry.overrideBuildMs += elapsedMs;
		}
		if (actorOverrides != nullptr && !actorOverrides->empty())
		{
			actorOverridesForBuild = actorOverrides;
		}
	}

	const nri_material_policy::ActorOverlayMaterialRuleMap* actorOverlayRules = nullptr;
	nri_material_policy::ActorOverlayMaterialRuleMap transientActorOverlayRules;
	if (resolvedLightOverlays.actorRules.Size() > 0)
	{
		const auto actorOverlayRuleBuildStart = std::chrono::steady_clock::now();
		bool actorOverlayRulesBuilt = false;
		bool actorOverlayRuleCacheHit = false;
		uint32_t actorOverlayRuleCount = 0;
		if (useActorRuleFrameCaches)
		{
			const auto& cachedActorOverlayRules = GetActorOverlayMaterialRulesForFrame(
				resolvedLightOverlays,
				mFrameIndex,
				mActorOverlayMaterialRuleCache,
				actorOverlayRulesBuilt,
				actorOverlayRuleCacheHit);
			actorOverlayRuleCount = mActorOverlayMaterialRuleCache.totalRuleCount;
			actorOverlayRules = &cachedActorOverlayRules;
		}
		else
		{
			BuildActorOverlayMaterialRules(resolvedLightOverlays, transientActorOverlayRules);
			for (const auto& entry : transientActorOverlayRules)
			{
				actorOverlayRuleCount += (uint32_t)entry.second.size();
			}
			actorOverlayRulesBuilt = true;
			actorOverlayRules = &transientActorOverlayRules;
		}
		if (tracePerf)
		{
			if (actorOverlayRuleCacheHit)
			{
				materialTraceEntry.actorOverlayRuleMapCacheHits++;
			}
			else if (useActorRuleFrameCaches)
			{
				materialTraceEntry.actorOverlayRuleMapCacheMisses++;
			}
			if (actorOverlayRulesBuilt)
			{
				materialTraceEntry.actorOverlayRuleMapBuilds++;
				materialTraceEntry.actorOverlayRuleBuildMs += DurationMs(actorOverlayRuleBuildStart, std::chrono::steady_clock::now());
			}
			materialTraceEntry.actorOverlayRuleCount += actorOverlayRuleCount;
		}
		if (!actorOverlayRules->empty())
		{
			const auto stampStart = std::chrono::steady_clock::now();
			ActorOverlayStampStats stampStats = {};
			StampActorOverlayRuleIdsOnSceneView(*actorOverlayRules, sceneView, stampStats);
			if (tracePerf)
			{
				materialTraceEntry.actorOverlayStampMs += DurationMs(stampStart, std::chrono::steady_clock::now());
				materialTraceEntry.actorOverlayStampedSpriteSurfaces += stampStats.stampedSpriteSurfaces;
				materialTraceEntry.actorOverlaySkippedNonSpriteSurfaces += stampStats.skippedNonSpriteSurfaces;
				materialTraceEntry.actorOverlaySkippedNoActorSurfaces += stampStats.skippedNoActorSurfaces;
			}
			for (const auto& entry : *actorOverlayRules)
			{
				uint32_t overrideBits = nri_material_policy::ActorMaterialOverride_None;
				for (const auto& rule : entry.second)
				{
					overrideBits |= rule.overrideBits;
				}

				if (overrideBits != nri_material_policy::ActorMaterialOverride_None)
				{
					if (actorOverridesForBuild != nullptr && mergedActorOverridesForBuild.empty())
					{
						mergedActorOverridesForBuild = *actorOverridesForBuild;
					}
					mergedActorOverridesForBuild[entry.first].bits |= overrideBits;
				}
			}
			if (!mergedActorOverridesForBuild.empty())
			{
				actorOverridesForBuild = &mergedActorOverridesForBuild;
			}
		}
	}

	struct SavedMaterialFlags
	{
		uint32_t* flags = nullptr;
		uint32_t value = 0;
	};

	std::vector<SavedMaterialFlags> savedFlags;
	if (actorOverridesForBuild != nullptr)
	{
		const auto fullbrightFlagStart = std::chrono::steady_clock::now();
		savedFlags.reserve(sceneView.opaqueWalls.size() + sceneView.opaqueFlats.size() + sceneView.opaqueSprites.size());
		auto applyFullbrightSurfaceFlags = [actorOverridesForBuild, &savedFlags](auto& surfaces)
		{
			for (auto& surface : surfaces)
			{
				if ((surface.material.flags & nri_scene::MaterialFlag_Sprite) == 0)
				{
					continue;
				}

				auto it = actorOverridesForBuild->find(surface.provenance.actorIndex);
				if (it == actorOverridesForBuild->end() ||
					(it->second.bits & nri_material_policy::ActorMaterialOverride_Fullbright) == 0)
				{
					continue;
				}

				savedFlags.push_back({ &surface.material.flags, surface.material.flags });
				surface.material.flags |= nri_scene::MaterialFlag_Fullbright;
			}
		};

		applyFullbrightSurfaceFlags(sceneView.opaqueWalls);
		applyFullbrightSurfaceFlags(sceneView.opaqueFlats);
		applyFullbrightSurfaceFlags(sceneView.opaqueSprites);
		if (tracePerf)
		{
			materialTraceEntry.fullbrightFlagMs += DurationMs(fullbrightFlagStart, std::chrono::steady_clock::now());
			materialTraceEntry.fullbrightFlaggedSurfaces += (uint32_t)savedFlags.size();
		}
	}

	if (tracePerf)
	{
		const auto start = std::chrono::steady_clock::now();
		nri_scene::BuildMaterials(sceneView, outMaterials);
		const double elapsedMs = DurationMs(start, std::chrono::steady_clock::now());
		mLastPerfShellTraceStats.materialBuildMs += elapsedMs;
		materialTraceEntry.materialBuildMs += elapsedMs;
	}
	else
	{
		nri_scene::BuildMaterials(sceneView, outMaterials);
	}
	if (actorOverridesForBuild != nullptr)
	{
		const auto overrideApplyStart = std::chrono::steady_clock::now();
		nri_material_policy::ApplyActorMaterialOverridesToBuiltMaterials(*actorOverridesForBuild, GetFullbrightBoostScale(), outMaterials);
		if (tracePerf)
		{
			materialTraceEntry.actorOverrideApplyMs += DurationMs(overrideApplyStart, std::chrono::steady_clock::now());
		}
		for (const SavedMaterialFlags& saved : savedFlags)
		{
			*saved.flags = saved.value;
		}
	}
	AccumulateMaterialTextureAttribution(
		materialTraceEntry,
		GatherMaterialTextureAttribution(outMaterials.materials, outMaterials.lightMetadata, outMaterials.textures.size()));
	TraceActorSpriteMaterialAssignments(sceneView, outMaterials, traceLabel);
}

bool NRIRenderer::ResolveActorMaterialPresentationPolicy(
	const nri_scene::SurfaceRef& surface,
	nri_material_policy::ActorMaterialPresentationPolicy& outPolicy)
{
	outPolicy = {};
	if (surface.provenance.actorIndex < 0 ||
		(surface.material.flags & nri_scene::MaterialFlag_Sprite) == 0 ||
		(mFrameBuffer != nullptr && mFrameBuffer->IsPathTracingLevelPreloadPending()))
	{
		return false;
	}

	const ResolvedLightOverlaySet& resolved = GetResolvedLightOverlaySet();
	outPolicy.resolvedGeneration = resolved.resolvedGeneration;
	if (nri_material_policy::HasActorMaterialOverrideRules(resolved))
	{
		const auto& overrides = GetActorMaterialOverrideMapForFrame(
			ResolveMaterialBuildTraceSlot("persistent_voxel_material_variant"));
		const auto overrideIt = overrides.find(surface.provenance.actorIndex);
		if (overrideIt != overrides.end())
		{
			outPolicy.overrideState = overrideIt->second;
		}
	}

	outPolicy.actorOverlayRuleCount = std::min<uint32_t>(
		surface.provenance.actorOverlayRuleCount,
		nri_scene::MaxActorOverlayRuleIdsPerSurface);
	std::copy_n(
		surface.provenance.actorOverlayRuleIds,
		outPolicy.actorOverlayRuleCount,
		outPolicy.actorOverlayRuleIds);

	if (resolved.actorRules.Size() == 0)
	{
		return true;
	}

	bool built = false;
	bool cacheHit = false;
	const auto& actorRules = GetActorOverlayMaterialRulesForFrame(
		resolved,
		mFrameIndex,
		mActorOverlayMaterialRuleCache,
		built,
		cacheHit);
	const auto actorRuleIt = actorRules.find(surface.provenance.actorIndex);
	if (actorRuleIt == actorRules.end())
	{
		return true;
	}

	uint32_t textureId = 0;
	if (surface.material.texture != nullptr)
	{
		const FTextureID id = surface.material.texture->GetID();
		textureId = id.isValid() ? (uint32_t)id.GetIndex() : 0u;
	}
	outPolicy.actorOverlayRuleCount = 0;
	std::fill(
		std::begin(outPolicy.actorOverlayRuleIds),
		std::end(outPolicy.actorOverlayRuleIds),
		0u);
	for (const auto& rule : actorRuleIt->second)
	{
		// BuildMaterialsWithActorOverrides applies rule material bits actor-wide,
		// while provenance IDs retain the rule's optional tile filter.
		outPolicy.overrideState.bits |= rule.overrideBits;
		if (rule.hasTileFilter && rule.tileFilter != textureId)
		{
			continue;
		}
		if (outPolicy.actorOverlayRuleCount < nri_scene::MaxActorOverlayRuleIdsPerSurface)
		{
			outPolicy.actorOverlayRuleIds[outPolicy.actorOverlayRuleCount++] = rule.ruleId;
		}
	}
	return true;
}

void NRIRenderer::ApplyActorShadowMaterialOverrides(const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& inOutGpuMaterials)
{
	const ResolvedLightOverlaySet& resolvedLightOverlays = GetResolvedLightOverlaySet();
	if (!nri_material_policy::HasActorMaterialOverrideRules(resolvedLightOverlays))
	{
		return;
	}

	const auto& actorOverrides = GetActorMaterialOverrideMapForFrame();
	if (actorOverrides.empty())
	{
		return;
	}

	nri_material_policy::ApplyActorShadowMaterialOverrides(actorOverrides, GetFullbrightBoostScale(), materials, inOutGpuMaterials);
}

uint64_t NRIRenderer::ComputeChunkActorOverrideHash(const nri_scene::MaterialBridgeData& materials)
{
	const auto& actorOverrides = GetActorMaterialOverrideMapForFrame();
	return nri_material_policy::ComputeChunkActorOverrideHash(actorOverrides, materials);
}

uint64_t NRIRenderer::ComputeChunkEmissiveOverrideHash(const nri_scene::MaterialBridgeData& materials) const
{
	return nri_material_policy::ComputeChunkEmissiveOverrideHash(mSceneLights, materials);
}

bool nri_material_policy::HasActorMaterialOverrideRules(const ResolvedLightOverlaySet& resolved)
{
	if (resolved.actorRules.Size() > 0 || resolved.actorOverrideRules.Size() > 0)
	{
		return true;
	}
	for (const auto& rule : resolved.smokeActorRules)
	{
		if (rule.emitterForeground)
		{
			return true;
		}
	}
	return false;
}

bool nri_material_policy::HasActorFullbrightOverrides(const ResolvedLightOverlaySet& resolved)
{
	for (const auto& rule : resolved.actorRules)
	{
		if (rule.hasFullbright)
		{
			return true;
		}
	}
	return false;
}

static uint64_t HashActorMaterialOverrideState(uint64_t hash, const nri_material_policy::ActorMaterialOverrideState& state)
{
	hash = nri_scene::HashCombine64(hash, (uint64_t)state.bits);
	hash = nri_scene::HashCombine64(hash, (uint64_t)state.emissiveStableFrames);
	return hash;
}

void nri_material_policy::BuildActorMaterialOverrideMap(
	const ResolvedLightOverlaySet& resolved,
	ActorMaterialOverrideMap& outOverrides)
{
	if (!HasActorMaterialOverrideRules(resolved))
	{
		return;
	}

	TSpriteIterator<DCoreActor> it;
	while (auto actor = it.Next())
	{
		if (actor == nullptr || !actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			continue;
		}

		PClass* actorClass = actor->GetClass();
		if (actorClass == nullptr)
		{
			continue;
		}

		ActorMaterialOverrideState overrideState = {};
		bool touched = false;
		const uint32_t actorTextureId = (unsigned)actor->spr.picnum < MAXTILES ? (uint32_t)tileGetTextureID(actor->spr.picnum).GetIndex() : 0u;
		for (const auto& resolvedRule : resolved.actorRules)
		{
			if (!resolvedRule.actorClassResolved ||
				resolvedRule.actorClass == nullptr ||
				(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
			{
				continue;
			}

			if (resolvedRule.hasTileFilter && actorTextureId != (uint32_t)resolvedRule.tileFilter)
			{
				continue;
			}

			if (resolvedRule.hasShadowReceive)
			{
				touched = true;
				if (resolvedRule.shadowReceive)
				{
					overrideState.bits &= ~ActorMaterialOverride_NoShadowReceive;
				}
				else
				{
					overrideState.bits |= ActorMaterialOverride_NoShadowReceive;
				}
			}

			if (resolvedRule.hasShadowCast)
			{
				touched = true;
				if (resolvedRule.shadowCast)
				{
					overrideState.bits &= ~ActorMaterialOverride_NoShadowCast;
				}
				else
				{
					overrideState.bits |= ActorMaterialOverride_NoShadowCast;
				}
			}

			if (resolvedRule.hasFullbright)
			{
				touched = true;
				if (resolvedRule.fullbright)
				{
					overrideState.bits |= ActorMaterialOverride_Fullbright;
				}
				else
				{
					overrideState.bits &= ~ActorMaterialOverride_Fullbright;
				}
			}

			if (resolvedRule.hasEmissiveStableFrames)
			{
				touched = true;
				overrideState.emissiveStableFrames = std::max(overrideState.emissiveStableFrames, resolvedRule.emissiveStableFrames);
			}
		}

		for (const auto& resolvedRule : resolved.actorOverrideRules)
		{
			if (!resolvedRule.actorClassResolved ||
				resolvedRule.actorClass == nullptr ||
				(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
			{
				continue;
			}

			if (resolvedRule.hasShadowReceive)
			{
				touched = true;
				if (resolvedRule.shadowReceive)
				{
					overrideState.bits &= ~ActorMaterialOverride_NoShadowReceive;
				}
				else
				{
					overrideState.bits |= ActorMaterialOverride_NoShadowReceive;
				}
			}

			if (resolvedRule.hasShadowCast)
			{
				touched = true;
				if (resolvedRule.shadowCast)
				{
					overrideState.bits &= ~ActorMaterialOverride_NoShadowCast;
				}
				else
				{
					overrideState.bits |= ActorMaterialOverride_NoShadowCast;
				}
			}
		}

		for (const auto& resolvedRule : resolved.smokeActorRules)
		{
			if (!resolvedRule.emitterForeground ||
				!resolvedRule.actorClassResolved ||
				!resolvedRule.styleResolved ||
				resolvedRule.actorClass == nullptr ||
				(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
			{
				continue;
			}

			DCoreActor* owner = actor->GetOwnerActor();
			if (!resolvedRule.ownerClassName.IsEmpty() &&
				(!resolvedRule.ownerClassResolved || owner == nullptr || owner->GetClass() == nullptr ||
					(owner->GetClass() != resolvedRule.ownerClass && !owner->GetClass()->IsDescendantOf(resolvedRule.ownerClass))))
			{
				continue;
			}
			if (!resolvedRule.excludeOwnerClassName.IsEmpty() &&
				(!resolvedRule.excludeOwnerClassResolved || (owner != nullptr && owner->GetClass() != nullptr &&
					(owner->GetClass() == resolvedRule.excludeOwnerClass || owner->GetClass()->IsDescendantOf(resolvedRule.excludeOwnerClass)))))
			{
				continue;
			}

			touched = true;
			overrideState.bits |= ActorMaterialOverride_SmokeForeground;
		}

		if (touched && !overrideState.Empty())
		{
			outOverrides[(int32_t)actor->GetIndex()] = overrideState;
		}
	}
}

const nri_material_policy::ActorMaterialOverrideMap& nri_material_policy::GetActorMaterialOverrideMapForFrame(
	const ResolvedLightOverlaySet& resolved,
	uint32_t frameIndex,
	ActorMaterialOverrideCache& cache,
	bool& outBuilt)
{
	outBuilt = false;
	const bool hasActorRules = HasActorMaterialOverrideRules(resolved);
	const bool hasFullbrightOverrides = HasActorFullbrightOverrides(resolved);
	if (cache.valid &&
		cache.frameIndex == frameIndex &&
		cache.resolvedGeneration == resolved.resolvedGeneration &&
		cache.hasFullbrightOverrides == hasFullbrightOverrides)
	{
		return cache.overrides;
	}

	cache.valid = true;
	cache.frameIndex = frameIndex;
	cache.resolvedGeneration = resolved.resolvedGeneration;
	cache.hasFullbrightOverrides = hasFullbrightOverrides;
	cache.overrides.clear();
	if (hasActorRules)
	{
		BuildActorMaterialOverrideMap(resolved, cache.overrides);
		outBuilt = true;
	}
	return cache.overrides;
}

void nri_material_policy::ApplyActorMaterialOverridesToBuiltMaterials(
	const ActorMaterialOverrideMap& actorOverrides,
	float fullbrightBoost,
	nri_scene::MaterialBridgeData& materials)
{
	const uint32_t count = std::min<uint32_t>((uint32_t)materials.materials.size(), (uint32_t)materials.lightMetadata.size());
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		nri_scene::MaterialLightingMetadata& metadata = materials.lightMetadata[materialIndex];
		if (metadata.actorIndex < 0)
		{
			continue;
		}

		auto it = actorOverrides.find(metadata.actorIndex);
		if (it == actorOverrides.end())
		{
			continue;
		}

		nri_scene::MaterialData& material = materials.materials[materialIndex];
		const ActorMaterialOverrideState& overrideState = it->second;
		const uint32_t overrideBits = overrideState.bits;
		const bool explicitVoxelPolicy = metadata.voxelPalettePolicyApplied;
		bool appliedOverride = false;
		if (overrideState.emissiveStableFrames > 0)
		{
			metadata.emissiveStableFrames = std::max(metadata.emissiveStableFrames, overrideState.emissiveStableFrames);
			appliedOverride = true;
		}
		if (!explicitVoxelPolicy && (overrideBits & ActorMaterialOverride_NoShadowReceive) != 0)
		{
			material.lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowReceive;
			metadata.lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowReceive;
			appliedOverride = true;
		}
		if (!explicitVoxelPolicy && (overrideBits & ActorMaterialOverride_NoShadowCast) != 0)
		{
			material.lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowCast;
			metadata.lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowCast;
			appliedOverride = true;
		}
		if ((overrideBits & ActorMaterialOverride_SmokeForeground) != 0)
		{
			material.lightingFlags |= nri_scene::MaterialLightingFlag_SmokeForeground;
			metadata.lightingFlags |= nri_scene::MaterialLightingFlag_SmokeForeground;
			appliedOverride = true;
		}
		if (explicitVoxelPolicy || (overrideBits & ActorMaterialOverride_Fullbright) == 0)
		{
			if (appliedOverride)
			{
				metadata.materialKey = HashActorMaterialOverrideState(
					nri_scene::HashCombine64(metadata.materialKey, 0xAC70A11C00000001ull),
					overrideState);
			}
			continue;
		}

		ApplyFullbrightMaterialOverride(material, fullbrightBoost);
		appliedOverride = true;
		metadata.materialFlags |= nri_scene::MaterialFlag_Fullbright;
		metadata.lightingFlags |= nri_scene::MaterialLightingFlag_MaterialFullbright;
		metadata.lightLevel = 1.0f;
		metadata.emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
		metadata.emissiveTextureIndex = material.textureIndex;
		metadata.emissiveIntensity = 1.0f;
		metadata.emissiveMaskScale = 1.0f;
		metadata.visibleFullbrightBoost = fullbrightBoost;
		metadata.emissiveColor[0] = 1.0f;
		metadata.emissiveColor[1] = 1.0f;
		metadata.emissiveColor[2] = 1.0f;
		if (appliedOverride)
		{
			metadata.materialKey = HashActorMaterialOverrideState(
				nri_scene::HashCombine64(metadata.materialKey, 0xAC70A11C00000001ull),
				overrideState);
		}
	}
}

void nri_material_policy::ApplyEmissiveMaterialOverrides(
	const SceneLightSystem& sceneLights,
	const ResolvedLightOverlaySet& resolved,
	float glowmapVisibleBlendScale,
	const nri_scene::MaterialBridgeData& materials,
	std::vector<nri_scene::MaterialData>& inOutGpuMaterials)
{
	const uint32_t count = std::min<uint32_t>((uint32_t)inOutGpuMaterials.size(), (uint32_t)materials.lightMetadata.size());
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		nri_scene::MaterialData& material = inOutGpuMaterials[materialIndex];
		sceneLights.ApplyEmissiveMaterialSettings(materials.lightMetadata[materialIndex], material);
		if (material.emissiveMode == nri_scene::MaterialEmissiveMode_UseGlowmapTexture)
		{
			material.emissiveReserved = ResolveVisibleGlowBlendScale(resolved, materials.lightMetadata[materialIndex], glowmapVisibleBlendScale);
		}
	}
}

void nri_material_policy::ApplyActorShadowMaterialOverrides(
	const ActorMaterialOverrideMap& actorOverrides,
	float fullbrightBoost,
	const nri_scene::MaterialBridgeData& materials,
	std::vector<nri_scene::MaterialData>& inOutGpuMaterials)
{
	if (actorOverrides.empty())
	{
		return;
	}

	const uint32_t count = std::min<uint32_t>((uint32_t)inOutGpuMaterials.size(), (uint32_t)materials.lightMetadata.size());
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		const nri_scene::MaterialLightingMetadata& metadata = materials.lightMetadata[materialIndex];
		if (metadata.actorIndex < 0)
		{
			continue;
		}

		auto it = actorOverrides.find(metadata.actorIndex);
		if (it == actorOverrides.end())
		{
			continue;
		}

		const uint32_t overrideBits = it->second.bits;
		const bool explicitVoxelPolicy = metadata.voxelPalettePolicyApplied;
		if (!explicitVoxelPolicy && (overrideBits & ActorMaterialOverride_NoShadowReceive) != 0)
		{
			inOutGpuMaterials[materialIndex].lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowReceive;
		}
		if (!explicitVoxelPolicy && (overrideBits & ActorMaterialOverride_NoShadowCast) != 0)
		{
			inOutGpuMaterials[materialIndex].lightingFlags |= nri_scene::MaterialLightingFlag_NoShadowCast;
		}
		if ((overrideBits & ActorMaterialOverride_SmokeForeground) != 0)
		{
			inOutGpuMaterials[materialIndex].lightingFlags |= nri_scene::MaterialLightingFlag_SmokeForeground;
		}
		if (!explicitVoxelPolicy && (overrideBits & ActorMaterialOverride_Fullbright) != 0)
		{
			ApplyFullbrightMaterialOverride(inOutGpuMaterials[materialIndex], fullbrightBoost);
		}
	}
}

bool nri_material_policy::MaterialDataEqual(
	const nri_scene::MaterialData& a,
	const nri_scene::MaterialData& b)
{
	return
		a.textureIndex == b.textureIndex &&
		a.paletteIndex == b.paletteIndex &&
		a.flags == b.flags &&
		a.materialClass == b.materialClass &&
		a.lightingFlags == b.lightingFlags &&
		a.normalTextureIndex == b.normalTextureIndex &&
		a.metallicTextureIndex == b.metallicTextureIndex &&
		a.roughnessTextureIndex == b.roughnessTextureIndex &&
		a.sectorIndex == b.sectorIndex &&
		a.emissiveTextureIndex == b.emissiveTextureIndex &&
		a.lightLevel == b.lightLevel &&
		a.alpha == b.alpha &&
		a.roughnessHint == b.roughnessHint &&
		a.metalnessHint == b.metalnessHint &&
		a.emissiveColor[0] == b.emissiveColor[0] &&
		a.emissiveColor[1] == b.emissiveColor[1] &&
		a.emissiveColor[2] == b.emissiveColor[2] &&
		a.emissiveIntensity == b.emissiveIntensity &&
		a.emissiveMaskScale == b.emissiveMaskScale &&
		a.emissiveMode == b.emissiveMode &&
		a.emissiveReserved == b.emissiveReserved;
}

bool nri_material_policy::MaterialDataVectorEqual(
	const std::vector<nri_scene::MaterialData>& a,
	const std::vector<nri_scene::MaterialData>& b)
{
	if (a.size() != b.size())
	{
		return false;
	}

	for (size_t i = 0; i < a.size(); ++i)
	{
		if (!MaterialDataEqual(a[i], b[i]))
		{
			return false;
		}
	}

	return true;
}

uint64_t nri_material_policy::ComputeChunkActorOverrideHash(
	const ActorMaterialOverrideMap& actorOverrides,
	const nri_scene::MaterialBridgeData& materials)
{
	if (actorOverrides.empty() || materials.lightMetadata.empty())
	{
		return 0;
	}

	uint64_t hash = 1469598103934665603ull;
	bool touched = false;
	for (const auto& metadata : materials.lightMetadata)
	{
		if (metadata.actorIndex < 0)
		{
			continue;
		}

		auto it = actorOverrides.find(metadata.actorIndex);
		if (it == actorOverrides.end() || it->second.Empty())
		{
			continue;
		}

		touched = true;
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)metadata.actorIndex);
		hash = HashActorMaterialOverrideState(hash, it->second);
	}

	return touched ? hash : 0;
}

uint64_t nri_material_policy::ComputeChunkEmissiveOverrideHash(
	const SceneLightSystem& sceneLights,
	const nri_scene::MaterialBridgeData& materials)
{
	const uint32_t count = std::min<uint32_t>((uint32_t)materials.materials.size(), (uint32_t)materials.lightMetadata.size());
	if (count == 0)
	{
		return 0;
	}

	uint64_t hash = 1469598103934665603ull;
	bool touched = false;
	for (uint32_t materialIndex = 0; materialIndex < count; ++materialIndex)
	{
		nri_scene::MaterialData effectiveMaterial = materials.materials[materialIndex];
		const bool emissiveApplied = sceneLights.ApplyEmissiveMaterialSettings(materials.lightMetadata[materialIndex], effectiveMaterial);
		if (!emissiveApplied)
		{
			continue;
		}

		touched = true;
		hash = nri_scene::HashCombine64(hash, (uint64_t)materialIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)effectiveMaterial.materialClass);
		hash = nri_scene::HashCombine64(hash, (uint64_t)effectiveMaterial.emissiveMode);
		hash = nri_scene::HashCombine64(hash, (uint64_t)effectiveMaterial.emissiveTextureIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveColor[0]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveColor[1]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveColor[2]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveIntensity));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveMaskScale));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(effectiveMaterial.emissiveReserved));
	}

	return touched ? hash : 0;
}
