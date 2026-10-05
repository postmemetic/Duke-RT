#include "nri_scene_lights.h"
#include "buildtiles.h"
#include "nri_cvars.h"
#include "nri_scene_light_rule_helpers.h"
#include "../scene/nri_hash.h"
#include "coreactor.h"
#include "gamefuncs.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
	uint64_t QuantizeLightOverlayPositionKey(const float position[3])
	{
		const int64_t x = (int64_t)std::llround(position[0] * 16.0f);
		const int64_t y = (int64_t)std::llround(position[1] * 16.0f);
		const int64_t z = (int64_t)std::llround(position[2] * 16.0f);
		uint64_t key = 1469598103934665603ull;
		key = nri_scene::HashCombine64(key, (uint64_t)x);
		key = nri_scene::HashCombine64(key, (uint64_t)y);
		key = nri_scene::HashCombine64(key, (uint64_t)z);
		return key;
	}

	void ComputeCapturedSurfaceCenter(const nri_scene::SurfaceRef& surface, float outCenter[3])
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		if (surface.vertices.empty())
		{
			return;
		}

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			outCenter[0] += vertex.position[0];
			outCenter[1] += vertex.position[1];
			outCenter[2] += vertex.position[2];
		}

		const float invCount = 1.0f / (float)surface.vertices.size();
		outCenter[0] *= invCount;
		outCenter[1] *= invCount;
		outCenter[2] *= invCount;
	}

	uint32_t BuildActorOverlayRuleId(const ResolvedLightOverlayActorRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.actorClassName.GetChars(), rule.source);
	}

	bool IsSupportedActorOverlayRule(const ResolvedLightOverlayActorRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0;
	}

	bool TryBuildActorAnalyticOverlayRule(
		const ResolvedLightOverlayActorRule& resolvedRule,
		SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule& actorRule)
	{
		if (!resolvedRule.actorClassResolved ||
			resolvedRule.actorClass == nullptr ||
			!IsSupportedActorOverlayRule(resolvedRule) ||
			resolvedRule.intensity <= 0.0f ||
			resolvedRule.radius <= 0.0f)
		{
			return false;
		}

		actorRule = {};
		actorRule.ruleId = BuildActorOverlayRuleId(resolvedRule);
		actorRule.ruleName = resolvedRule.id.GetChars();
		actorRule.hasTileFilter = resolvedRule.hasTileFilter;
		actorRule.tileFilter = resolvedRule.hasTileFilter && resolvedRule.tileFilter >= 0 ? (uint32_t)resolvedRule.tileFilter : 0u;
		const bool lightCastsShadow = resolvedRule.hasLightShadowCast
			? resolvedRule.lightShadowCast
			: (!resolvedRule.hasShadowCast || resolvedRule.shadowCast);
		actorRule.flags = lightCastsShadow ? SceneAnalyticLightFlag_CastsShadow : SceneAnalyticLightFlag_None;
		actorRule.materialNoShadowReceive = resolvedRule.hasShadowReceive && !resolvedRule.shadowReceive;
		actorRule.materialNoShadowCast = resolvedRule.hasShadowCast && !resolvedRule.shadowCast;
		actorRule.materialFullbright = resolvedRule.hasFullbright && resolvedRule.fullbright;
		actorRule.activateImmediately = resolvedRule.activationPolicy == LightOverlayActorActivationPolicy::Immediate;
		actorRule.color[0] = resolvedRule.color[0];
		actorRule.color[1] = resolvedRule.color[1];
		actorRule.color[2] = resolvedRule.color[2];
		actorRule.intensity = resolvedRule.intensity;
		actorRule.radius = resolvedRule.radius;
		actorRule.offset[0] = resolvedRule.offset[0];
		actorRule.offset[1] = resolvedRule.offset[1];
		actorRule.offset[2] = resolvedRule.offset[2];
		actorRule.hasNudgeFromSurface = resolvedRule.hasNudgeFromSurface && resolvedRule.nudgeFromSurfaceDistance > 0.0f;
		actorRule.nudgeFromSurfaceDistance = resolvedRule.nudgeFromSurfaceDistance;
		actorRule.flickerFrames = resolvedRule.flickerFrames;
		actorRule.hasRandomIntensity = resolvedRule.hasRandom;
		actorRule.randomIntensityRange[0] = resolvedRule.randomIntensityRange[0];
		actorRule.randomIntensityRange[1] = resolvedRule.randomIntensityRange[1];
		return true;
	}

	void BuildActorAnalyticOverlayRules(
		const ResolvedLightOverlaySet& resolved,
		std::unordered_map<int32_t, std::vector<SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule>>& outRules)
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
					(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
				{
					continue;
				}

				SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule actorRule = {};
				if (TryBuildActorAnalyticOverlayRule(resolvedRule, actorRule))
				{
					actorRule.actorClassName = actorClass->TypeName.GetChars();
					actorRule.actorIndex = (int32_t)actor->GetIndex();
					actorRule.actorPalette = actor->spr.pal;
					actorRule.actorPosition[0] = (float)actor->spr.pos.X;
					actorRule.actorPosition[1] = (float)-actor->spr.pos.Z;
					actorRule.actorPosition[2] = (float)-actor->spr.pos.Y;
					const FTextureID liveTextureId = actor->dispictex.isValid() ? actor->dispictex : actor->spr.spritetexture();
					actorRule.actorTextureId = liveTextureId.isValid() ? (uint32_t)liveTextureId.GetIndex() : 0u;
					actorRules.push_back(actorRule);
				}
			}

			if (actorRules.empty())
			{
				outRules.erase((int32_t)actor->GetIndex());
			}
		}
	}

	void BuildActorAnalyticOverlayRuleLookup(
		const ResolvedLightOverlaySet& resolved,
		std::unordered_map<uint32_t, SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule>& outRulesById)
	{
		for (const auto& resolvedRule : resolved.actorRules)
		{
			SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule actorRule = {};
			if (TryBuildActorAnalyticOverlayRule(resolvedRule, actorRule))
			{
				outRulesById[actorRule.ruleId] = actorRule;
			}
		}
	}

	bool IsSupportedMapOverlayRule(const ResolvedLightOverlayMapLightRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0;
	}

	bool IsSupportedSurfaceLightRule(const ResolvedLightOverlaySurfaceLightRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0 || rule.lightType.CompareNoCase("rect") == 0;
	}

	uint32_t BuildMapOverlayRuleId(const ResolvedLightOverlayMapLightRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	uint32_t BuildSurfaceLightRuleId(const ResolvedLightOverlaySurfaceLightRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	uint32_t BuildEmissiveOverrideRuleId(const ResolvedLightOverlayEmissiveOverrideRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
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

	uint64_t BuildMapOverlayStableKey(uint32_t ruleId, const float position[3])
	{
		uint64_t key = 1469598103934665603ull;
		key = nri_scene::HashCombine64(key, (uint64_t)ruleId);
		key = nri_scene::HashCombine64(key, QuantizeLightOverlayPositionKey(position));
		return key;
	}

	void ConvertMapOverlayWorldVectorToPathTracing(const float source[3], float destination[3])
	{
		destination[0] = source[0];
		destination[1] = -source[2];
		destination[2] = -source[1];
	}

	bool TryResolveSectorMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex, float outPosition[3])
	{
		const nri_scene::PTMapChunk* matchedChunk = nullptr;
		for (const auto& chunk : mapWorld.chunks)
		{
			if (chunk.sectorIndex == sectorIndex)
			{
				matchedChunk = &chunk;
				break;
			}
		}
		if (matchedChunk == nullptr)
		{
			return false;
		}

		float flatCenterSum[3] = {};
		int flatCenterCount = 0;
		float anyCenterSum[3] = {};
		int anyCenterCount = 0;
		const uint32_t endSurface = matchedChunk->firstSurface + matchedChunk->surfaceCount;
		for (uint32_t surfaceIndex = matchedChunk->firstSurface; surfaceIndex < endSurface && surfaceIndex < mapWorld.surfaces.size(); ++surfaceIndex)
		{
			const auto& surface = mapWorld.surfaces[surfaceIndex].surface;
			if (surface.provenance.sectorIndex != sectorIndex)
			{
				continue;
			}

			float center[3] = {};
			ComputeCapturedSurfaceCenter(surface, center);
			anyCenterSum[0] += center[0];
			anyCenterSum[1] += center[1];
			anyCenterSum[2] += center[2];
			anyCenterCount++;

			if (surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapFloorSection ||
				surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapCeilingSection)
			{
				flatCenterSum[0] += center[0];
				flatCenterSum[1] += center[1];
				flatCenterSum[2] += center[2];
				flatCenterCount++;
			}
		}

		const float* sum = flatCenterCount > 0 ? flatCenterSum : anyCenterSum;
		const int count = flatCenterCount > 0 ? flatCenterCount : anyCenterCount;
		if (count <= 0)
		{
			return false;
		}

		const float invCount = 1.0f / (float)count;
		outPosition[0] = sum[0] * invCount;
		outPosition[1] = sum[1] * invCount;
		outPosition[2] = sum[2] * invCount;
		return true;
	}

	bool TryResolveWallMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, int32_t wallIndex, float outPosition[3])
	{
		float centerSum[3] = {};
		int centerCount = 0;
		for (const auto& mapSurface : mapWorld.surfaces)
		{
			if (mapSurface.surface.provenance.wallIndex != wallIndex)
			{
				continue;
			}

			float center[3] = {};
			ComputeCapturedSurfaceCenter(mapSurface.surface, center);
			centerSum[0] += center[0];
			centerSum[1] += center[1];
			centerSum[2] += center[2];
			centerCount++;
		}

		if (centerCount <= 0)
		{
			return false;
		}

		const float invCount = 1.0f / (float)centerCount;
		outPosition[0] = centerSum[0] * invCount;
		outPosition[1] = centerSum[1] * invCount;
		outPosition[2] = centerSum[2] * invCount;
		return true;
	}

	bool TryResolveMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, const ResolvedLightOverlayMapLightRule& rule, float outPosition[3])
	{
		switch (rule.anchorType)
		{
		case LightOverlayAnchorType::Position:
			if (!rule.hasAnchorPosition)
			{
				return false;
			}
			ConvertMapOverlayWorldVectorToPathTracing(rule.anchorPosition, outPosition);
			return true;

		case LightOverlayAnchorType::Sector:
			return rule.anchorIndex >= 0 && TryResolveSectorMapOverlayAnchorPosition(mapWorld, rule.anchorIndex, outPosition);

		case LightOverlayAnchorType::Wall:
			return rule.anchorIndex >= 0 && TryResolveWallMapOverlayAnchorPosition(mapWorld, rule.anchorIndex, outPosition);

		default:
			return false;
		}
	}

	void BuildStaticMapAnalyticOverlayRules(
		const ResolvedLightOverlaySet& resolved,
		const nri_scene::PTMapWorld& mapWorld,
		std::vector<SceneLightSystem::AnalyticLightRegistry::MapOverlayRule>& outRules)
	{
		for (const auto& resolvedRule : resolved.mapLightRules)
		{
			if (!IsSupportedMapOverlayRule(resolvedRule) ||
				resolvedRule.intensity <= 0.0f ||
				resolvedRule.radius <= 0.0f)
			{
				continue;
			}

			float anchorPosition[3] = {};
			if (!TryResolveMapOverlayAnchorPosition(mapWorld, resolvedRule, anchorPosition))
			{
				continue;
			}

			SceneLightSystem::AnalyticLightRegistry::MapOverlayRule overlayRule = {};
			float offset[3] = {};
			ConvertMapOverlayWorldVectorToPathTracing(resolvedRule.offset, offset);
			overlayRule.ruleId = BuildMapOverlayRuleId(resolvedRule);
			overlayRule.source = SceneLightRecordSource::StaticMapScene;
			overlayRule.position[0] = anchorPosition[0] + offset[0];
			overlayRule.position[1] = anchorPosition[1] + offset[1];
			overlayRule.position[2] = anchorPosition[2] + offset[2];
			overlayRule.stableKey = BuildMapOverlayStableKey(overlayRule.ruleId, overlayRule.position);
			overlayRule.color[0] = resolvedRule.color[0];
			overlayRule.color[1] = resolvedRule.color[1];
			overlayRule.color[2] = resolvedRule.color[2];
			overlayRule.intensity = resolvedRule.intensity;
			overlayRule.radius = resolvedRule.radius;
			overlayRule.flickerFrames = resolvedRule.flickerFrames;
			outRules.push_back(overlayRule);
		}

		for (const auto& resolvedRule : resolved.surfaceLightRules)
		{
			if (!IsSupportedSurfaceLightRule(resolvedRule) ||
				!resolvedRule.hasPosition ||
				!resolvedRule.hasNormal ||
				resolvedRule.intensity <= 0.0f ||
				resolvedRule.radius <= 0.0f)
			{
				continue;
			}

			SceneLightSystem::AnalyticLightRegistry::MapOverlayRule overlayRule = {};
			const float offset = resolvedRule.hasOffset ? resolvedRule.offset : 0.0f;
			overlayRule.ruleId = BuildSurfaceLightRuleId(resolvedRule);
			overlayRule.source = SceneLightRecordSource::DynamicScene;
			overlayRule.position[0] = resolvedRule.position[0] + resolvedRule.normal[0] * offset;
			overlayRule.position[1] = resolvedRule.position[1] + resolvedRule.normal[1] * offset;
			overlayRule.position[2] = resolvedRule.position[2] + resolvedRule.normal[2] * offset;
			overlayRule.stableKey = BuildMapOverlayStableKey(overlayRule.ruleId, overlayRule.position);
			overlayRule.color[0] = resolvedRule.color[0];
			overlayRule.color[1] = resolvedRule.color[1];
			overlayRule.color[2] = resolvedRule.color[2];
			overlayRule.intensity = resolvedRule.intensity;
			overlayRule.radius = resolvedRule.radius;
			overlayRule.hasSectorResponse = resolvedRule.hasSectorResponse;
			overlayRule.sectorResponse = resolvedRule.sectorResponse;
			overlayRule.hasSignalSector = resolvedRule.hasSignalSector;
			overlayRule.signalSector = resolvedRule.signalSector;
			overlayRule.hasResponseIntensity = resolvedRule.hasResponseIntensity;
			overlayRule.responseIntensity = resolvedRule.responseIntensity;
			overlayRule.hasResponseMin = resolvedRule.hasResponseMin;
			overlayRule.responseMin = resolvedRule.responseMin;
			overlayRule.hasResponseMax = resolvedRule.hasResponseMax;
			overlayRule.responseMax = resolvedRule.responseMax;
			overlayRule.hasResponseInputMin = resolvedRule.hasResponseInputMin;
			overlayRule.responseInputMin = resolvedRule.responseInputMin;
			overlayRule.hasResponseInputMax = resolvedRule.hasResponseInputMax;
			overlayRule.responseInputMax = resolvedRule.responseInputMax;
			outRules.push_back(overlayRule);
		}
	}

	void BuildEmissiveOverrideRules(
		const ResolvedLightOverlaySet& resolved,
		std::vector<SceneLightSystem::EmissiveOverrideRule>& outRules)
	{
		outRules.clear();
		outRules.reserve((size_t)resolved.emissiveOverrideRules.Size());
		for (const auto& resolvedRule : resolved.emissiveOverrideRules)
		{
			if (!resolvedRule.hasSectorFilter &&
				!resolvedRule.hasWallFilter &&
				!resolvedRule.hasTileFilter)
			{
				continue;
			}

			SceneLightSystem::EmissiveOverrideRule rule = {};
			rule.ruleId = BuildEmissiveOverrideRuleId(resolvedRule);
			rule.hasSectorFilter = resolvedRule.hasSectorFilter;
			rule.sectorFilter = resolvedRule.sectorFilter;
			rule.hasWallFilter = resolvedRule.hasWallFilter;
			rule.wallFilter = resolvedRule.wallFilter;
			rule.hasTileFilter = resolvedRule.hasTileFilter && resolvedRule.tileFilter >= 0;
			rule.tileFilter = rule.hasTileFilter ? (uint32_t)resolvedRule.tileFilter : 0u;
			rule.hasIntensityScale = resolvedRule.hasIntensityScale;
			rule.intensityScale = resolvedRule.intensityScale;
			rule.hasReachScale = resolvedRule.hasReachScale;
			rule.reachScale = resolvedRule.reachScale;
			rule.hasSectorResponse = resolvedRule.hasSectorResponse;
			rule.sectorResponse = resolvedRule.sectorResponse;
			rule.hasSignalSector = resolvedRule.hasSignalSector && resolvedRule.signalSector >= 0;
			rule.signalSector = rule.hasSignalSector ? resolvedRule.signalSector : -1;
			rule.hasResponseIntensity = resolvedRule.hasResponseIntensity;
			rule.responseIntensity = resolvedRule.responseIntensity;
			rule.hasResponseMin = resolvedRule.hasResponseMin;
			rule.responseMin = resolvedRule.responseMin;
			rule.hasResponseMax = resolvedRule.hasResponseMax;
			rule.responseMax = resolvedRule.responseMax;
			rule.hasResponseInputMin = resolvedRule.hasResponseInputMin;
			rule.responseInputMin = resolvedRule.responseInputMin;
			rule.hasResponseInputMax = resolvedRule.hasResponseInputMax;
			rule.responseInputMax = resolvedRule.responseInputMax;
			rule.hasResponseIntensityMin = resolvedRule.hasResponseIntensityMin;
			rule.responseIntensityMin = resolvedRule.responseIntensityMin;
			rule.hasResponseIntensityMax = resolvedRule.hasResponseIntensityMax;
			rule.responseIntensityMax = resolvedRule.responseIntensityMax;
			rule.hasResponseReachMin = resolvedRule.hasResponseReachMin;
			rule.responseReachMin = resolvedRule.responseReachMin;
			rule.hasResponseReachMax = resolvedRule.hasResponseReachMax;
			rule.responseReachMax = resolvedRule.responseReachMax;
			rule.hasMaterialResponse = resolvedRule.hasMaterialResponse;
			rule.materialResponse = resolvedRule.materialResponse;
			rule.hasMaterialResponseMin = resolvedRule.hasMaterialResponseMin;
			rule.materialResponseMin = resolvedRule.materialResponseMin;
			rule.hasMaterialResponseMax = resolvedRule.hasMaterialResponseMax;
			rule.materialResponseMax = resolvedRule.materialResponseMax;
			outRules.push_back(rule);
		}
	}

	void BuildSurfaceLightFixtureResponseRules(
		const ResolvedLightOverlaySet& resolved,
		std::vector<SceneLightSystem::EmissiveOverrideRule>& outRules)
	{
		outRules.clear();
		outRules.reserve((size_t)resolved.surfaceLightRules.Size());
		for (const auto& resolvedRule : resolved.surfaceLightRules)
		{
			if (!resolvedRule.hasPosition || !resolvedRule.hasNormal)
			{
				continue;
			}

			const bool sectorResponseEnabled = resolvedRule.hasSectorResponse && resolvedRule.sectorResponse;
			SceneLightSystem::EmissiveOverrideRule rule = {};
			rule.ruleId = BuildSurfaceLightRuleId(resolvedRule);
			rule.hasSectorResponse = true;
			rule.sectorResponse = sectorResponseEnabled;
			rule.hasSignalSector = resolvedRule.hasSignalSector && resolvedRule.signalSector >= 0;
			rule.signalSector = rule.hasSignalSector ? resolvedRule.signalSector : -1;
			rule.hasResponseIntensity = resolvedRule.hasResponseIntensity;
			rule.responseIntensity = resolvedRule.responseIntensity;
			rule.hasResponseMin = resolvedRule.hasResponseMin;
			rule.responseMin = resolvedRule.responseMin;
			rule.hasResponseMax = resolvedRule.hasResponseMax;
			rule.responseMax = resolvedRule.responseMax;
			rule.hasResponseInputMin = resolvedRule.hasResponseInputMin;
			rule.responseInputMin = resolvedRule.responseInputMin;
			rule.hasResponseInputMax = resolvedRule.hasResponseInputMax;
			rule.responseInputMax = resolvedRule.responseInputMax;
			if (resolvedRule.fixtureMaterialResponse && sectorResponseEnabled)
			{
				rule.hasMaterialResponse = true;
				rule.materialResponse = true;
				rule.hasMaterialResponseMin = resolvedRule.hasMaterialResponseMin;
				rule.materialResponseMin = resolvedRule.materialResponseMin;
				rule.hasMaterialResponseMax = resolvedRule.hasMaterialResponseMax;
				rule.materialResponseMax = resolvedRule.materialResponseMax;
			}
			outRules.push_back(rule);
		}
	}

	void BuildEmissiveMaterialResponseRules(
		const ResolvedLightOverlaySet& resolved,
		std::vector<SceneLightSystem::EmissiveMaterialResponseRule>& outRules)
	{
		outRules.clear();
		outRules.reserve((size_t)resolved.emissiveMaterialResponseRules.Size());
		for (const auto& resolvedRule : resolved.emissiveMaterialResponseRules)
		{
			SceneLightSystem::EmissiveMaterialResponseRule rule = {};
			rule.ruleId = BuildResolvedLightOverlayRuleId(resolvedRule.id.GetChars(), "", resolvedRule.source);
			rule.textureIds.reserve((size_t)resolvedRule.tileFilters.Size() + (size_t)resolvedRule.textureNames.Size());
			for (int tile : resolvedRule.tileFilters)
			{
				if (tile >= 0)
				{
					rule.textureIds.push_back((uint32_t)tile);
				}
			}
			rule.textureRanges.reserve((size_t)resolvedRule.tileRanges.Size());
			for (const auto& range : resolvedRule.tileRanges)
			{
				if (range.first >= 0 && range.last >= 0)
				{
					rule.textureRanges.emplace_back((uint32_t)range.first, (uint32_t)range.last);
				}
			}
			for (const auto& textureName : resolvedRule.textureNames)
			{
				rule.textureNames.push_back(NormalizeLightOverlayTextureSelector(textureName.GetChars()));
			}
			if (rule.textureIds.empty() && rule.textureRanges.empty() && rule.textureNames.empty())
			{
				continue;
			}
			rule.hasMaterialResponse = resolvedRule.hasMaterialResponse;
			rule.materialResponse = resolvedRule.materialResponse;
			rule.hasMaterialResponseMin = resolvedRule.hasMaterialResponseMin;
			rule.materialResponseMin = resolvedRule.materialResponseMin;
			rule.hasMaterialResponseMax = resolvedRule.hasMaterialResponseMax;
			rule.materialResponseMax = resolvedRule.materialResponseMax;
			rule.hasVisibleGlowBlend = resolvedRule.hasVisibleGlowBlend;
			rule.visibleGlowBlend = resolvedRule.visibleGlowBlend;
			outRules.push_back(rule);
		}
	}

}

const SceneLightSystem::CompiledOverlayRules& SceneLightSystem::RefreshCompiledOverlayRules(
	const ResolvedLightOverlaySet& resolved,
	const nri_scene::PTMapWorld& mapWorld)
{
	auto& compiled = mCompiledOverlayRules;
	const bool generationChanged = compiled.resolvedGeneration != resolved.resolvedGeneration ||
		compiled.mapBuildSerial != mapWorld.buildSerial || compiled.mapValid != mapWorld.valid;
	if (generationChanged)
	{
		compiled.quarantined = false;
	}
	if (!nri_ptlightregistry || compiled.quarantined)
	{
		// Keep the original builders authoritative after a mismatch, including
		// when validation is switched off later in the same content generation.
		CompiledOverlayRules full;
		BuildFullOverlayRules(resolved, mapWorld, full);
		compiled.liveActorRules = std::move(full.liveActorRules);
		compiled.actorRulesById = std::move(full.actorRulesById);
		compiled.mapRules = std::move(full.mapRules);
		compiled.emissiveRules = std::move(full.emissiveRules);
		compiled.fixtureRules = std::move(full.fixtureRules);
		compiled.materialResponseRules = std::move(full.materialResponseRules);
		compiled.valid = false;
		compiled.resolvedGeneration = resolved.resolvedGeneration;
		compiled.mapBuildSerial = mapWorld.buildSerial;
		compiled.mapValid = mapWorld.valid;
		++mLightRegistryStats.ruleCacheRebuilds;
		return compiled;
	}
	const bool rebuild = !compiled.valid || generationChanged;
	if (rebuild)
	{
		compiled.actorTemplates.clear();
		compiled.actorRulesById.clear();
		compiled.mapRules.clear();
		compiled.liveActorRules.clear();
		for (const auto& resolvedRule : resolved.actorRules)
		{
			AnalyticLightRegistry::ActorOverlayRule actorRule = {};
			if (TryBuildActorAnalyticOverlayRule(resolvedRule, actorRule))
			{
				compiled.actorRulesById[actorRule.ruleId] = actorRule;
				compiled.actorTemplates.push_back({ resolvedRule.actorClass, std::move(actorRule) });
			}
		}
		BuildEmissiveOverrideRules(resolved, compiled.emissiveRules);
		BuildSurfaceLightFixtureResponseRules(resolved, compiled.fixtureRules);
		BuildEmissiveMaterialResponseRules(resolved, compiled.materialResponseRules);
		if (mapWorld.valid)
		{
			BuildStaticMapAnalyticOverlayRules(resolved, mapWorld, compiled.mapRules);
		}
		compiled.valid = true;
		compiled.resolvedGeneration = resolved.resolvedGeneration;
		compiled.mapBuildSerial = mapWorld.buildSerial;
		compiled.mapValid = mapWorld.valid;
		++mLightRegistryStats.ruleCacheRebuilds;
	}
	else
	{
		++mLightRegistryStats.ruleCacheHits;
	}

	// Cap selection uses a stable distance sort, so equal-distance lights retain
	// the original fresh unordered_map enumeration order. Preserve its exact
	// insertion/rehash sequence, reusing only the actor vectors via swap.
	auto previousActorRules = std::move(compiled.liveActorRules);
	compiled.liveActorRules = decltype(compiled.liveActorRules){};
	if (resolved.actorRules.Size() != 0)
	{
		TSpriteIterator<DCoreActor> it;
		while (auto actor = it.Next())
		{
			if (!actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0)
			{
				continue;
			}
			PClass* actorClass = actor->GetClass();
			if (actorClass == nullptr)
			{
				continue;
			}
			const int32_t actorIndex = (int32_t)actor->GetIndex();
			auto& actorRules = compiled.liveActorRules[actorIndex];
			const auto previous = previousActorRules.find(actorIndex);
			if (previous != previousActorRules.end())
			{
				actorRules.swap(previous->second);
				actorRules.clear();
			}
			for (const auto& actorTemplate : compiled.actorTemplates)
			{
				if (actorClass != actorTemplate.actorClass && !actorClass->IsDescendantOf(actorTemplate.actorClass))
				{
					continue;
				}
				auto rule = actorTemplate.rule;
				rule.actorClassName = actorClass->TypeName.GetChars();
				rule.actorIndex = (int32_t)actor->GetIndex();
				rule.actorPalette = actor->spr.pal;
				rule.actorPosition[0] = (float)actor->spr.pos.X;
				rule.actorPosition[1] = (float)-actor->spr.pos.Z;
				rule.actorPosition[2] = (float)-actor->spr.pos.Y;
				const FTextureID liveTextureId = actor->dispictex.isValid() ? actor->dispictex : actor->spr.spritetexture();
				rule.actorTextureId = liveTextureId.isValid() ? (uint32_t)liveTextureId.GetIndex() : 0u;
				actorRules.push_back(std::move(rule));
			}
			if (actorRules.empty())
			{
				compiled.liveActorRules.erase(actorIndex);
			}
		}
	}
	if (nri_ptlightregistryvalidate)
	{
		ValidateCompiledOverlayRules(resolved, mapWorld);
	}
	return compiled;
}

void SceneLightSystem::BuildFullOverlayRules(
	const ResolvedLightOverlaySet& resolved,
	const nri_scene::PTMapWorld& mapWorld,
	CompiledOverlayRules& out)
{
	BuildActorAnalyticOverlayRules(resolved, out.liveActorRules);
	BuildActorAnalyticOverlayRuleLookup(resolved, out.actorRulesById);
	BuildEmissiveOverrideRules(resolved, out.emissiveRules);
	BuildSurfaceLightFixtureResponseRules(resolved, out.fixtureRules);
	BuildEmissiveMaterialResponseRules(resolved, out.materialResponseRules);
	if (mapWorld.valid)
	{
		BuildStaticMapAnalyticOverlayRules(resolved, mapWorld, out.mapRules);
	}
}
