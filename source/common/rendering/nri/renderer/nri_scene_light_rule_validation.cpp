#include "nri_scene_lights.h"
#include "printf.h"

#include <cstring>

namespace
{
	template<class T> bool EqualRuleField(const T& left, const T& right)
	{
		return left == right;
	}
	bool EqualRuleField(float left, float right)
	{
		return std::memcmp(&left, &right, sizeof(float)) == 0;
	}
	template<class T, size_t N> bool EqualRuleField(const T (&left)[N], const T (&right)[N])
	{
		return std::memcmp(left, right, sizeof(left)) == 0;
	}
	bool EqualRuleField(const char* left, const char* right)
	{
		return left == right || (left != nullptr && right != nullptr && std::strcmp(left, right) == 0);
	}

	bool EqualCompiledRule(const SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule& left, const SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule& right)
	{
		return
			EqualRuleField(left.ruleId, right.ruleId) &&
			EqualRuleField(left.ruleName, right.ruleName) &&
			EqualRuleField(left.actorClassName, right.actorClassName) &&
			EqualRuleField(left.actorIndex, right.actorIndex) &&
			EqualRuleField(left.actorTextureId, right.actorTextureId) &&
			EqualRuleField(left.actorPalette, right.actorPalette) &&
			EqualRuleField(left.actorPosition, right.actorPosition) &&
			EqualRuleField(left.materialNoShadowReceive, right.materialNoShadowReceive) &&
			EqualRuleField(left.materialNoShadowCast, right.materialNoShadowCast) &&
			EqualRuleField(left.materialFullbright, right.materialFullbright) &&
			EqualRuleField(left.activateImmediately, right.activateImmediately) &&
			EqualRuleField(left.hasTileFilter, right.hasTileFilter) &&
			EqualRuleField(left.tileFilter, right.tileFilter) &&
			EqualRuleField(left.flags, right.flags) &&
			EqualRuleField(left.color, right.color) &&
			EqualRuleField(left.intensity, right.intensity) &&
			EqualRuleField(left.radius, right.radius) &&
			EqualRuleField(left.offset, right.offset) &&
			EqualRuleField(left.hasNudgeFromSurface, right.hasNudgeFromSurface) &&
			EqualRuleField(left.nudgeFromSurfaceDistance, right.nudgeFromSurfaceDistance) &&
			EqualRuleField(left.flickerFrames, right.flickerFrames) &&
			EqualRuleField(left.hasRandomIntensity, right.hasRandomIntensity) &&
			EqualRuleField(left.randomIntensityRange, right.randomIntensityRange);
	}

	bool EqualCompiledRule(const SceneLightSystem::AnalyticLightRegistry::MapOverlayRule& left, const SceneLightSystem::AnalyticLightRegistry::MapOverlayRule& right)
	{
		return
			EqualRuleField(left.ruleId, right.ruleId) &&
			EqualRuleField(left.stableKey, right.stableKey) &&
			EqualRuleField(left.source, right.source) &&
			EqualRuleField(left.position, right.position) &&
			EqualRuleField(left.color, right.color) &&
			EqualRuleField(left.intensity, right.intensity) &&
			EqualRuleField(left.radius, right.radius) &&
			EqualRuleField(left.flickerFrames, right.flickerFrames) &&
			EqualRuleField(left.hasSectorResponse, right.hasSectorResponse) &&
			EqualRuleField(left.sectorResponse, right.sectorResponse) &&
			EqualRuleField(left.hasSignalSector, right.hasSignalSector) &&
			EqualRuleField(left.signalSector, right.signalSector) &&
			EqualRuleField(left.hasResponseIntensity, right.hasResponseIntensity) &&
			EqualRuleField(left.responseIntensity, right.responseIntensity) &&
			EqualRuleField(left.hasResponseMin, right.hasResponseMin) &&
			EqualRuleField(left.responseMin, right.responseMin) &&
			EqualRuleField(left.hasResponseMax, right.hasResponseMax) &&
			EqualRuleField(left.responseMax, right.responseMax) &&
			EqualRuleField(left.hasResponseInputMin, right.hasResponseInputMin) &&
			EqualRuleField(left.responseInputMin, right.responseInputMin) &&
			EqualRuleField(left.hasResponseInputMax, right.hasResponseInputMax) &&
			EqualRuleField(left.responseInputMax, right.responseInputMax);
	}

	bool EqualCompiledRule(const SceneLightSystem::EmissiveOverrideRule& left, const SceneLightSystem::EmissiveOverrideRule& right)
	{
		return
			EqualRuleField(left.ruleId, right.ruleId) &&
			EqualRuleField(left.hasSectorFilter, right.hasSectorFilter) &&
			EqualRuleField(left.sectorFilter, right.sectorFilter) &&
			EqualRuleField(left.hasWallFilter, right.hasWallFilter) &&
			EqualRuleField(left.wallFilter, right.wallFilter) &&
			EqualRuleField(left.hasTileFilter, right.hasTileFilter) &&
			EqualRuleField(left.tileFilter, right.tileFilter) &&
			EqualRuleField(left.hasIntensityScale, right.hasIntensityScale) &&
			EqualRuleField(left.intensityScale, right.intensityScale) &&
			EqualRuleField(left.hasReachScale, right.hasReachScale) &&
			EqualRuleField(left.reachScale, right.reachScale) &&
			EqualRuleField(left.hasSectorResponse, right.hasSectorResponse) &&
			EqualRuleField(left.sectorResponse, right.sectorResponse) &&
			EqualRuleField(left.hasSignalSector, right.hasSignalSector) &&
			EqualRuleField(left.signalSector, right.signalSector) &&
			EqualRuleField(left.hasResponseIntensity, right.hasResponseIntensity) &&
			EqualRuleField(left.responseIntensity, right.responseIntensity) &&
			EqualRuleField(left.hasResponseMin, right.hasResponseMin) &&
			EqualRuleField(left.responseMin, right.responseMin) &&
			EqualRuleField(left.hasResponseMax, right.hasResponseMax) &&
			EqualRuleField(left.responseMax, right.responseMax) &&
			EqualRuleField(left.hasResponseInputMin, right.hasResponseInputMin) &&
			EqualRuleField(left.responseInputMin, right.responseInputMin) &&
			EqualRuleField(left.hasResponseInputMax, right.hasResponseInputMax) &&
			EqualRuleField(left.responseInputMax, right.responseInputMax) &&
			EqualRuleField(left.hasResponseIntensityMin, right.hasResponseIntensityMin) &&
			EqualRuleField(left.responseIntensityMin, right.responseIntensityMin) &&
			EqualRuleField(left.hasResponseIntensityMax, right.hasResponseIntensityMax) &&
			EqualRuleField(left.responseIntensityMax, right.responseIntensityMax) &&
			EqualRuleField(left.hasResponseReachMin, right.hasResponseReachMin) &&
			EqualRuleField(left.responseReachMin, right.responseReachMin) &&
			EqualRuleField(left.hasResponseReachMax, right.hasResponseReachMax) &&
			EqualRuleField(left.responseReachMax, right.responseReachMax) &&
			EqualRuleField(left.hasMaterialResponse, right.hasMaterialResponse) &&
			EqualRuleField(left.materialResponse, right.materialResponse) &&
			EqualRuleField(left.hasMaterialResponseMin, right.hasMaterialResponseMin) &&
			EqualRuleField(left.materialResponseMin, right.materialResponseMin) &&
			EqualRuleField(left.hasMaterialResponseMax, right.hasMaterialResponseMax) &&
			EqualRuleField(left.materialResponseMax, right.materialResponseMax);
	}

	bool EqualCompiledRule(const SceneLightSystem::EmissiveMaterialResponseRule& left, const SceneLightSystem::EmissiveMaterialResponseRule& right)
	{
		return
			EqualRuleField(left.ruleId, right.ruleId) &&
			EqualRuleField(left.textureIds, right.textureIds) &&
			EqualRuleField(left.textureRanges, right.textureRanges) &&
			EqualRuleField(left.textureNames, right.textureNames) &&
			EqualRuleField(left.hasMaterialResponse, right.hasMaterialResponse) &&
			EqualRuleField(left.materialResponse, right.materialResponse) &&
			EqualRuleField(left.hasMaterialResponseMin, right.hasMaterialResponseMin) &&
			EqualRuleField(left.materialResponseMin, right.materialResponseMin) &&
			EqualRuleField(left.hasMaterialResponseMax, right.hasMaterialResponseMax) &&
			EqualRuleField(left.materialResponseMax, right.materialResponseMax) &&
			EqualRuleField(left.hasVisibleGlowBlend, right.hasVisibleGlowBlend) &&
			EqualRuleField(left.visibleGlowBlend, right.visibleGlowBlend);
	}

	template<class T> bool EqualRuleVector(const std::vector<T>& left, const std::vector<T>& right)
	{
		if (left.size() != right.size()) return false;
		for (size_t index = 0; index < left.size(); ++index)
		{
			if (!EqualCompiledRule(left[index], right[index])) return false;
		}
		return true;
	}
}

void SceneLightSystem::ValidateCompiledOverlayRules(const ResolvedLightOverlaySet& resolved, const nri_scene::PTMapWorld& mapWorld)
{
	CompiledOverlayRules full;
	BuildFullOverlayRules(resolved, mapWorld, full);
	auto& cached = mCompiledOverlayRules;
	++mLightRegistryStats.ruleValidationChecks;
	bool equal =
		EqualRuleVector(cached.mapRules, full.mapRules) &&
		EqualRuleVector(cached.emissiveRules, full.emissiveRules) &&
		EqualRuleVector(cached.fixtureRules, full.fixtureRules) &&
		EqualRuleVector(cached.materialResponseRules, full.materialResponseRules) &&
		cached.actorRulesById.size() == full.actorRulesById.size() &&
		cached.liveActorRules.size() == full.liveActorRules.size();
	if (equal)
	{
		for (const auto& entry : full.actorRulesById)
		{
			const auto found = cached.actorRulesById.find(entry.first);
			if (found == cached.actorRulesById.end() || !EqualCompiledRule(found->second, entry.second))
			{
				equal = false;
				break;
			}
		}
	}
	// Actor enumeration order is observable in equal-distance cap selection.
	// Compare it explicitly, together with every rule property and selector.
	if (equal)
	{
		auto cachedActor = cached.liveActorRules.begin();
		for (const auto& fullActor : full.liveActorRules)
		{
			if (cachedActor->first != fullActor.first || !EqualRuleVector(cachedActor->second, fullActor.second))
			{
				equal = false;
				break;
			}
			++cachedActor;
		}
	}
	if (equal) return;
	++mLightRegistryStats.ruleValidationMismatches;
	Printf("NRI PT light rule cache mismatch: frame=%llu resolved=%u map=%llu action=full-rules-quarantine\n",
		(unsigned long long)mFrameSerial, resolved.resolvedGeneration, (unsigned long long)mapWorld.buildSerial);
	cached.liveActorRules = std::move(full.liveActorRules);
	cached.actorRulesById = std::move(full.actorRulesById);
	cached.mapRules = std::move(full.mapRules);
	cached.emissiveRules = std::move(full.emissiveRules);
	cached.fixtureRules = std::move(full.fixtureRules);
	cached.materialResponseRules = std::move(full.materialResponseRules);
	cached.quarantined = true;
}
