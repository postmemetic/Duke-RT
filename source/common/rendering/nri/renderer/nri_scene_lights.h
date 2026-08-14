#pragma once

#include "../scene/nri_material_bridge.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_map_world.h"
#include "../scene/nri_scene_bridge.h"
#include "nri_emissive_sampling_distribution.h"
#include "lightoverlay.h"
#include "v_video.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

enum class SceneLightRecordSource : uint32_t
{
	None = 0,
	CapturedScene,
	StaticMapScene,
	RuntimeMutationScene,
	DynamicScene,
	SurfaceLightOverlayScene,
	PersistentVoxelScene,
};

enum SceneAnalyticLightSourceFlags : uint32_t
{
	SceneAnalyticLightSourceFlag_None = 0,
	SceneAnalyticLightSourceFlag_Manual = 1u << 0,
	SceneAnalyticLightSourceFlag_SpriteTileHeuristic = 1u << 1,
	SceneAnalyticLightSourceFlag_ActorOverlay = 1u << 2,
	SceneAnalyticLightSourceFlag_MapOverlay = 1u << 3,
	SceneAnalyticLightSourceFlag_MuzzleFlash = 1u << 4,
};

enum SceneAnalyticLightFlags : uint32_t
{
	SceneAnalyticLightFlag_None = 0,
	SceneAnalyticLightFlag_CastsShadow = 1u << 0,
};

enum SceneEmissiveSurfaceSourceFlags : uint32_t
{
	SceneEmissiveSurfaceSourceFlag_None = 0,
	SceneEmissiveSurfaceSourceFlag_AutoFullbright = 1u << 0,
	SceneEmissiveSurfaceSourceFlag_AutoTextureGlow = 1u << 1,
	SceneEmissiveSurfaceSourceFlag_AutoGlowmap = 1u << 2,
	SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule = 1u << 3,
	SceneEmissiveSurfaceSourceFlag_LightOverlayOverride = 1u << 4,
};

enum SceneSectorLightSourceFlags : uint32_t
{
	SceneSectorLightSourceFlag_None = 0,
	SceneSectorLightSourceFlag_Heuristic = 1u << 0,
	SceneSectorLightSourceFlag_PaletteFilter = 1u << 1,
	SceneSectorLightSourceFlag_LotagFilter = 1u << 2,
	SceneSectorLightSourceFlag_FogPresent = 1u << 3,
	SceneSectorLightSourceFlag_Pulsing = 1u << 4,
};

enum SceneLightDiagnosticFlags : uint32_t
{
	SceneLightDiagnosticFlag_None = 0,
	SceneLightDiagnosticFlag_PreviousMatch = 1u << 0,
	SceneLightDiagnosticFlag_PropertyChanged = 1u << 1,
	SceneLightDiagnosticFlag_Rebound = 1u << 2,
	SceneLightDiagnosticFlag_Added = 1u << 3,
};

struct NRIRuntimePointLightGpuData
{
	float position[3] = {};
	float radius = 0.0f;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	float intensity = 1.0f;
	uint32_t flags = 0;
	float emitterRadius = 0.0f;
	uint32_t stableKeyLo = 0;
	uint32_t stableKeyHi = 0;
};
static_assert(sizeof(NRIRuntimePointLightGpuData) == 48, "Runtime point-light CPU/GPU layout must remain 48 bytes");

struct NRISectorLightHeaderGpuData
{
	uint32_t sectorCount = 0;
	uint32_t activeCount = 0;
	uint32_t pulsingCount = 0;
	uint32_t flags = 0;
};

struct NRISectorLightGpuData
{
	float ambientColor[3] = {};
	float ambientIntensity = 0.0f;
	float hemisphereColor[3] = {};
	float hemisphereAmount = 0.0f;
	float fogAmount = 0.0f;
	float pulseScale = 1.0f;
	uint32_t sourceFlags = 0;
	int32_t paletteIndex = -1;
	int32_t lotag = 0;
	int32_t hitag = 0;
};

struct NRISectorLightingBoundState
{
	uint32_t sectorCount = 0;
	uint32_t activeCount = 0;
	uint32_t pulsingCount = 0;
	uint32_t dominantSector = UINT32_MAX;
	float dominantContribution = 0.0f;
};

struct StaticMapSceneCache;

inline constexpr uint32_t NRI_MAX_RUNTIME_POINT_LIGHTS = 64u;

float NRIGetSectorLightMultiplier();

struct NRIRuntimeLightTileHeaderGpuData
{
	uint32_t indexOffset = 0;
	uint32_t indexCount = 0;
};

struct NRIEmissivePrimitiveHeaderGpuData
{
	uint32_t activeCount = 0;
	uint32_t dominantIndex = UINT32_MAX;
	uint32_t flags = 0;
	float totalPower = 0.0f;
};

struct NRIEmissivePrimitiveGpuData
{
	uint32_t dataSource = 0;
	uint32_t primitiveIndex = UINT32_MAX;
	uint32_t sourceFlags = 0;
	uint32_t textureId = 0;
	float primitiveArea = 0.0f;
	float powerEstimate = 0.0f;
	float selectionWeight = 0.0f;
	float selectionPdf = 0.0f;
	float emissionScale = 1.0f;
	uint32_t stableKeyLo = 0;
	uint32_t stableKeyHi = 0;
	uint32_t sceneInstanceIndex = UINT32_MAX;
	uint32_t primitiveCount = 1;
	uint32_t occurrenceKeyLo = 0;
	uint32_t occurrenceKeyHi = 0;
	uint32_t occurrenceGeneration = 0;
	float boundsCenter[3] = {};
	float boundsRadius = 0.0f;
	float materialResponseScale = 1.0f;
	uint32_t reserved[3] = {};
};

static_assert(sizeof(NRIEmissivePrimitiveGpuData) == 96);

// Hot shader record. Sampling-distribution and diagnostic-only fields remain in
// NRIEmissivePrimitiveGpuData on the CPU and are not uploaded per candidate.
struct NRIEmissivePrimitiveShaderData
{
	uint32_t dataSource = 0;
	uint32_t primitiveIndex = UINT32_MAX;
	float primitiveArea = 0.0f;
	float selectionPdf = 0.0f;
	float emissionScale = 1.0f;
	uint32_t stableKeyLo = 0;
	uint32_t stableKeyHi = 0;
	uint32_t sceneInstanceIndex = UINT32_MAX;
	uint32_t primitiveCount = 1;
	uint32_t occurrenceKeyLo = 0;
	uint32_t occurrenceKeyHi = 0;
	uint32_t occurrenceGeneration = 0;
	float boundsCenter[3] = {};
	float boundsRadius = 0.0f;
	float materialResponseScale = 1.0f;
};

static_assert(sizeof(NRIEmissivePrimitiveShaderData) == 68);
static_assert(offsetof(NRIEmissivePrimitiveShaderData, primitiveArea) == 8);
static_assert(offsetof(NRIEmissivePrimitiveShaderData, emissionScale) == 16);
static_assert(offsetof(NRIEmissivePrimitiveShaderData, sceneInstanceIndex) == 28);
static_assert(offsetof(NRIEmissivePrimitiveShaderData, boundsCenter) == 48);
static_assert(offsetof(NRIEmissivePrimitiveShaderData, materialResponseScale) == 64);

inline NRIEmissivePrimitiveShaderData PackNRIEmissivePrimitiveShaderData(
	const NRIEmissivePrimitiveGpuData& source)
{
	NRIEmissivePrimitiveShaderData target = {};
	target.dataSource = source.dataSource;
	target.primitiveIndex = source.primitiveIndex;
	target.primitiveArea = source.primitiveArea;
	target.selectionPdf = source.selectionPdf;
	target.emissionScale = source.emissionScale;
	target.stableKeyLo = source.stableKeyLo;
	target.stableKeyHi = source.stableKeyHi;
	target.sceneInstanceIndex = source.sceneInstanceIndex;
	target.primitiveCount = source.primitiveCount;
	target.occurrenceKeyLo = source.occurrenceKeyLo;
	target.occurrenceKeyHi = source.occurrenceKeyHi;
	target.occurrenceGeneration = source.occurrenceGeneration;
	target.boundsCenter[0] = source.boundsCenter[0];
	target.boundsCenter[1] = source.boundsCenter[1];
	target.boundsCenter[2] = source.boundsCenter[2];
	target.boundsRadius = source.boundsRadius;
	target.materialResponseScale = source.materialResponseScale;
	return target;
}

struct NRIEmissiveMaterialResponseGpuData
{
	uint32_t dataSource = 0;
	uint32_t primitiveIndex = UINT32_MAX;
	float materialScale = 1.0f;
	uint32_t flags = 0;
};

struct NRIEmissivePrimitiveDebugRecord
{
	uint64_t stableKey = 0;
	uint64_t surfaceStableKey = 0;
	uint32_t dataSource = 0;
	uint32_t primitiveIndex = UINT32_MAX;
	uint32_t primitiveCount = 1;
	uint32_t sceneInstanceIndex = UINT32_MAX;
	uint32_t occurrenceKeyLo = 0;
	uint32_t occurrenceKeyHi = 0;
	uint32_t occurrenceGeneration = 0;
	uint32_t materialIndex = UINT32_MAX;
	uint32_t sourceFlags = 0;
	uint32_t sourceRuleId = 0;
	uint32_t overrideRuleId = 0;
	uint32_t textureId = 0;
	uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	int32_t actorIndex = -1;
	int32_t sectorIndex = -1;
	float center[3] = {};
	float boundsRadius = 0.0f;
	float primitiveArea = 0.0f;
	float powerEstimate = 0.0f;
	float selectionWeight = 0.0f;
	float selectionPdf = 0.0f;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	float sectorResponseScale = 1.0f;
	float sectorReachScale = 1.0f;
	float materialResponseScale = 1.0f;
	bool materialResponseEnabled = false;
	bool sectorResponseApplied = false;
};

struct NRILightingSettings
{
	float emissiveMinPower = 0.0f;
	float emissiveMinSurface = 0.0f;
	float glowScale = 1.0f;
	float glowReach = 1.0f;
	float glowFalloff = 1.0f;
	bool sectorLighting = false;
	float sectorAmbientScale = 1.0f;
	float sectorHemisphereScale = 1.0f;
	float sectorFogScale = 1.0f;
	float sectorClamp = 1.0f;
	int sectorFilterPalette = -1;
	int sectorFilterMinShade = -128;
	int sectorFilterMaxShade = 127;
	int sectorFilterLotag = -1;
	int sectorPulseFrames = 0;
	float sectorPulseAmount = 0.0f;
	float sectorEmissionSignalStrength = 1.0f;
	float sectorEmissionResponseMin = 0.25f;
	float sectorEmissionResponseMax = 3.0f;
};

class SceneLightSystem
{
public:
	struct SurfaceIdentityOverrides
	{
		std::vector<uint64_t> opaqueWalls;
		std::vector<uint64_t> opaqueFlats;
		std::vector<uint64_t> opaqueSprites;

		void Clear()
		{
			opaqueWalls.clear();
			opaqueFlats.clear();
			opaqueSprites.clear();
		}
	};

	struct SceneAnalyticLight
	{
		uint32_t id = 0;
		uint64_t stableKey = 0;
		uint32_t sourceFlags = SceneAnalyticLightSourceFlag_None;
		uint32_t flags = SceneAnalyticLightFlag_CastsShadow;
		uint32_t sourceRuleId = 0;
		SceneLightRecordSource source = SceneLightRecordSource::None;
		int32_t actorIndex = -1;
		uint32_t textureId = 0;
		float position[3] = {};
		float color[3] = { 1.0f, 1.0f, 1.0f };
		float intensity = 1.0f;
		float radius = 0.0f;
		float emitterRadius = 0.0f;
	};

	struct RuntimeLightClusterBuildInput
	{
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		uint32_t tileSize = 1;
		uint32_t maxRuntimeLights = 0;
		float currentCameraPos[3] = {};
		float currentCameraForward[3] = { 0.0f, 0.0f, 1.0f };
		float currentCameraRight[3] = { 1.0f, 0.0f, 0.0f };
		float currentCameraUp[3] = { 0.0f, 1.0f, 0.0f };
		float tanHalfFovX = 0.0f;
		float tanHalfFovY = 0.0f;
	};

	struct EmissiveSamplingBuildContext
	{
		const nri_scene::GeometryData* staticGeometry = nullptr;
		const nri_scene::GeometryData* capturedGeometry = nullptr;
		const nri_scene::GeometryData* runtimeMutationGeometry = nullptr;
		uint32_t runtimeMutationPrimitiveBaseOffset = 0;
		const nri_scene::GeometryData* dynamicGeometry = nullptr;
		uint32_t dynamicPrimitiveBaseOffset = 0;
		const nri_scene::GeometryData* surfaceLightOverlayGeometry = nullptr;
		uint32_t surfaceLightOverlayPrimitiveBaseOffset = 0;
	};

	struct AnalyticLightHeuristicRule
	{
		uint32_t ruleId = 0;
		uint32_t textureId = 0;
		float color[3] = { 1.0f, 1.0f, 1.0f };
		float intensity = 1.0f;
		float radius = 0.0f;
		uint32_t flickerFrames = 0;
	};

	struct AnalyticLightRegistry
	{
		struct ActorOverlayRule
		{
			uint32_t ruleId = 0;
			std::string ruleName;
			const char* actorClassName = "";
			int32_t actorIndex = -1;
			uint32_t actorTextureId = 0;
			int32_t actorPalette = 0;
			float actorPosition[3] = {};
			bool materialNoShadowReceive = false;
			bool materialNoShadowCast = false;
			bool materialFullbright = false;
			bool activateImmediately = false;
			bool hasTileFilter = false;
			uint32_t tileFilter = 0;
			uint32_t flags = SceneAnalyticLightFlag_CastsShadow;
			float color[3] = { 1.0f, 1.0f, 1.0f };
			float intensity = 0.0f;
			float radius = 0.0f;
			float offset[3] = {};
			bool hasNudgeFromSurface = false;
			float nudgeFromSurfaceDistance = 0.0f;
			uint32_t flickerFrames = 0;
			bool hasRandomIntensity = false;
			float randomIntensityRange[2] = { 0.0f, 0.0f };
		};

		struct MapOverlayRule
		{
			uint32_t ruleId = 0;
			uint64_t stableKey = 0;
			SceneLightRecordSource source = SceneLightRecordSource::None;
			float position[3] = {};
			float color[3] = { 1.0f, 1.0f, 1.0f };
			float intensity = 0.0f;
			float radius = 0.0f;
			uint32_t flickerFrames = 0;
			bool hasSectorResponse = false;
			bool sectorResponse = true;
			bool hasSignalSector = false;
			int32_t signalSector = -1;
			bool hasResponseIntensity = false;
			float responseIntensity = 1.0f;
			bool hasResponseMin = false;
			float responseMin = 0.25f;
			bool hasResponseMax = false;
			float responseMax = 3.0f;
			bool hasResponseInputMin = false;
			float responseInputMin = 0.0f;
			bool hasResponseInputMax = false;
			float responseInputMax = 1.0f;
		};

		std::vector<SceneAnalyticLight> manualLights;
		std::vector<SceneAnalyticLight> transientLights;
		std::vector<AnalyticLightHeuristicRule> spriteTileRules;
		std::vector<SceneAnalyticLight> activeLights;
		std::vector<uint64_t> activeTopologyKeys;
		std::unordered_map<uint64_t, uint64_t> activePropertyHashes;
		std::unordered_map<uint64_t, uint64_t> activeBindingHashes;
		std::unordered_map<uint64_t, uint32_t> activeDiagnosticFlags;
		std::vector<uint64_t> addedTopologyKeys;
		std::vector<uint64_t> removedTopologyKeys;
		std::vector<uint64_t> reboundTopologyKeys;
		uint32_t matchedSurfaceCount = 0;
		uint32_t actorOverlayRuleCount = 0;
		uint32_t actorOverlayMatchedSurfaceCount = 0;
		uint32_t actorOverlayPublishedActorCount = 0;
		uint32_t actorOverlayPublishedFallbackActivationCount = 0;
		uint32_t mapOverlayRuleCount = 0;
		uint32_t spriteTileRuleCount = 0;
		uint32_t spriteRecordCandidateScans = 0;
		uint32_t actorOverlaySurfaceLookups = 0;
		uint32_t actorOverlayFullRecordScans = 0;
		uint32_t actorOverlaySurfaceCandidateScans = 0;
		uint32_t actorOverlayIndexedCandidateCount = 0;
		uint32_t topologyKeyCount = 0;
		uint32_t topologyRebuildCount = 0;
		uint32_t propertyOnlyUpdateCount = 0;
		uint32_t topologySortSkippedCount = 0;
		uint32_t topologyAddedKeyCount = 0;
		uint32_t topologyRemovedKeyCount = 0;
		uint32_t topologyReboundKeyCount = 0;
		uint32_t softLightCount = 0;
		uint32_t survivingKeyIndexChangeCount = 0;
		uint32_t survivingSoftLightIndexChangeCount = 0;
		uint64_t orderedStableKeyHash = 0;
		uint32_t transientMuzzleSlotCount = 0;
		uint32_t transientMuzzleActiveCount = 0;
		uint32_t dedupedMatchCount = 0;
		uint32_t truncatedLightCount = 0;
		uint32_t nextRuleId = 1;
		double topologySortMs = 0.0;
		bool topologyChanged = false;
		bool propertiesChanged = false;
		bool lastBuildTopologyChanged = false;
		bool lastBuildPropertiesChanged = false;
	};

	struct EmissiveSurfaceRegistry
	{
		struct EmissiveHeuristicRule
		{
			uint32_t ruleId = 0;
			uint32_t textureId = 0;
			uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
			float intensityScale = 1.0f;
			float emissiveColor[3] = { 1.0f, 1.0f, 1.0f };
			bool hasExplicitColor = false;
		};

		struct EmissiveSurfaceRecord
		{
			uint64_t stableKey = 0;
			uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
			uint32_t sourceRuleId = 0;
			uint32_t overrideRuleId = 0;
			SceneLightRecordSource source = SceneLightRecordSource::None;
			int32_t actorIndex = -1;
			int32_t sectorIndex = -1;
			int32_t authoredSectorIndex = -1;
			int32_t wallIndex = -1;
			uint32_t textureId = 0;
			uint32_t emissiveTextureIndex = UINT32_MAX;
			uint32_t materialIndex = UINT32_MAX;
			uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
			float center[3] = {};
			float boundsRadius = 0.0f;
			float surfaceArea = 0.0f;
			float emissiveColor[3] = {};
			float emissiveIntensity = 0.0f;
			float reachScale = 1.0f;
			bool hasSectorResponseParams = false;
			float sectorResponseIntensity = 1.0f;
			float sectorResponseMin = 0.25f;
			float sectorResponseMax = 3.0f;
			bool hasSectorResponseInputRange = false;
			float sectorResponseInputMin = 0.0f;
			float sectorResponseInputMax = 1.0f;
			bool hasSectorResponseIntensityMin = false;
			float sectorResponseIntensityMin = 0.0f;
			bool hasSectorResponseIntensityMax = false;
			float sectorResponseIntensityMax = 4.0f;
			bool hasSectorResponseReachMin = false;
			float sectorResponseReachMin = 0.0f;
			bool hasSectorResponseReachMax = false;
			float sectorResponseReachMax = 24.0f;
			bool materialResponseEnabled = false;
			bool materialResponseExplicit = false;
			bool hasMaterialResponseParams = false;
			bool hasMaterialResponseMin = false;
			bool hasMaterialResponseMax = false;
			float materialResponseMin = 0.0f;
			float materialResponseMax = 1.0f;
			float powerEstimate = 0.0f;
			bool sectorResponseEnabled = true;
			uint32_t placedPrimitiveBase = 0;
			uint32_t placedPrimitiveCount = 0;
			uint32_t sceneInstanceIndex = UINT32_MAX;
			uint32_t occurrenceKeyLo = 0;
			uint32_t occurrenceKeyHi = 0;
			uint32_t occurrenceGeneration = 0;
		};

		std::vector<EmissiveHeuristicRule> textureRules;
		std::vector<EmissiveSurfaceRecord> activeSurfaces;
		std::vector<uint64_t> activeTopologyKeys;
		std::unordered_map<uint64_t, uint64_t> activePropertyHashes;
		std::unordered_map<uint64_t, uint64_t> activeBindingHashes;
		std::unordered_map<uint64_t, uint32_t> activeDiagnosticFlags;
		std::vector<uint64_t> addedTopologyKeys;
		std::vector<uint64_t> removedTopologyKeys;
		std::vector<uint64_t> reboundTopologyKeys;
		float totalPowerEstimate = 0.0f;
		uint32_t autoTaggedCount = 0;
		uint32_t explicitRuleMatchCount = 0;
		uint32_t overrideRuleCount = 0;
		uint32_t overrideMatchedSurfaceCount = 0;
		uint32_t materialResponseRuleCount = 0;
		uint32_t materialResponseMatchedSurfaceCount = 0;
		uint32_t truncatedSurfaceCount = 0;
		uint32_t nextRuleId = 1;
		bool topologyChanged = false;
		bool propertiesChanged = false;
		bool materialBindingChanged = false;
		bool materialPropertiesChanged = false;
		bool lastBuildTopologyChanged = false;
		bool lastBuildPropertiesChanged = false;
	};

	struct EmissiveOverrideRule
	{
		uint32_t ruleId = 0;
		bool hasSectorFilter = false;
		int32_t sectorFilter = -1;
		bool hasWallFilter = false;
		int32_t wallFilter = -1;
		bool hasTileFilter = false;
		uint32_t tileFilter = 0;
		bool hasIntensityScale = false;
		float intensityScale = 1.0f;
		bool hasReachScale = false;
		float reachScale = 1.0f;
		bool hasSectorResponse = false;
		bool sectorResponse = true;
		bool hasSignalSector = false;
		int32_t signalSector = -1;
		bool hasResponseIntensity = false;
		float responseIntensity = 1.0f;
		bool hasResponseMin = false;
		float responseMin = 0.25f;
		bool hasResponseMax = false;
		float responseMax = 3.0f;
		bool hasResponseInputMin = false;
		float responseInputMin = 0.0f;
		bool hasResponseInputMax = false;
		float responseInputMax = 1.0f;
		bool hasResponseIntensityMin = false;
		float responseIntensityMin = 0.0f;
		bool hasResponseIntensityMax = false;
		float responseIntensityMax = 4.0f;
		bool hasResponseReachMin = false;
		float responseReachMin = 0.0f;
		bool hasResponseReachMax = false;
		float responseReachMax = 24.0f;
		bool hasMaterialResponse = false;
		bool materialResponse = false;
		bool hasMaterialResponseMin = false;
		float materialResponseMin = 0.0f;
		bool hasMaterialResponseMax = false;
		float materialResponseMax = 1.0f;
	};

	struct EmissiveMaterialResponseRule
	{
		uint32_t ruleId = 0;
		std::vector<uint32_t> textureIds;
		std::vector<std::pair<uint32_t, uint32_t>> textureRanges;
		std::vector<std::string> textureNames;
		bool hasMaterialResponse = false;
		bool materialResponse = true;
		bool hasMaterialResponseMin = false;
		float materialResponseMin = 0.0f;
		bool hasMaterialResponseMax = false;
		float materialResponseMax = 1.0f;
		bool hasVisibleGlowBlend = false;
		float visibleGlowBlend = 1.0f;
	};

	struct SectorLightingRegistry
	{
		struct SectorLightRecord
		{
			uint32_t sectorIndex = UINT32_MAX;
			uint32_t sourceFlags = SceneSectorLightSourceFlag_None;
			int32_t paletteIndex = -1;
			int32_t lotag = 0;
			int32_t hitag = 0;
			int32_t averageShade = 0;
			int32_t rawAverageShade = 0;
			float rawLightLevel = 0.0f;
			float rawFloorLight = 0.0f;
			float rawCeilingLight = 0.0f;
			float rawAmbientIntensity = 0.0f;
			float rawHemisphereAmount = 0.0f;
			float rawFogAmount = 0.0f;
			float rawResponseBrightness = 0.0f;
			float rawResponseSignal = 0.0f;
			float emitterResponseScale = 1.0f;
			float ambientColor[3] = {};
			float ambientIntensity = 0.0f;
			float hemisphereAmount = 0.0f;
			float fogAmount = 0.0f;
			float pulseScale = 1.0f;
		};

		std::vector<SectorLightRecord> sectors;
		std::vector<uint32_t> activeSectorIndices;
		std::vector<uint32_t> rawActiveSectorIndices;
		std::vector<uint32_t> activeTopologyKeys;
		uint32_t sectorCount = 0;
		uint32_t eligibleSectorCount = 0;
		uint32_t rawActiveSectorCount = 0;
		uint32_t rawNonNeutralSectorCount = 0;
		uint32_t responseBoostSectorCount = 0;
		uint32_t responseDimSectorCount = 0;
		uint32_t responseNeutralSectorCount = 0;
		uint32_t activeSectorCount = 0;
		uint32_t fogSectorCount = 0;
		uint32_t pulsingSectorCount = 0;
		bool topologyChanged = false;
	};

	struct EnvironmentLightingState
	{
	};

	struct SurfaceRecord
	{
		uint64_t identityKey = 0;
		SceneLightRecordSource source = SceneLightRecordSource::None;
		uint32_t materialIndex = UINT32_MAX;
		float center[3] = {};
		float boundsRadius = 0.0f;
		float surfaceArea = 0.0f;
		nri_scene::SurfaceProvenance provenance = {};
		nri_scene::MaterialLightingMetadata material = {};
		uint32_t placedPrimitiveBase = 0;
		uint32_t placedPrimitiveCount = 0;
		uint32_t sceneInstanceIndex = UINT32_MAX;
		uint32_t occurrenceKeyLo = 0;
		uint32_t occurrenceKeyHi = 0;
		uint32_t occurrenceGeneration = 0;
	};

	struct FrameAppendStats
	{
		uint32_t totalRecordCount = 0;
		uint32_t staticRecordCount = 0;
		uint32_t runtimeMutationRecordCount = 0;
		uint32_t dynamicRecordCount = 0;
		uint32_t surfaceLightOverlayRecordCount = 0;
		uint32_t capturedRecordCount = 0;
		uint32_t persistentVoxelRecordCount = 0;
	};

	struct SurfaceRecordIndex
	{
		std::unordered_map<uint32_t, std::vector<uint32_t>> spriteRecordsByTextureId;
		std::unordered_map<int32_t, std::vector<uint32_t>> spriteRecordsByActorIndex;
		std::unordered_map<uint64_t, std::vector<uint32_t>> spriteRecordsByActorTexture;

		void Clear()
		{
			spriteRecordsByTextureId.clear();
			spriteRecordsByActorIndex.clear();
			spriteRecordsByActorTexture.clear();
		}
	};

	struct FrameAssemblyInput
	{
		uint64_t frameSerial = 0;
		uint32_t frameIndex = 0;
		bool voxelStats = false;
		bool usedStaticMapScene = false;
		const StaticMapSceneCache* staticScene = nullptr;
		const nri_scene::SceneView* capturedSceneView = nullptr;
		const nri_scene::MaterialBridgeData* capturedMaterials = nullptr;
		const nri_scene::SceneView* dynamicSceneView = nullptr;
		const nri_scene::MaterialBridgeData* dynamicMaterials = nullptr;
		const nri_scene::SceneView* surfaceLightSceneView = nullptr;
		const nri_scene::MaterialBridgeData* surfaceLightMaterials = nullptr;
		bool appendPersistentVoxelSceneLights = false;
		const std::unordered_set<int32_t>* suppressedActorIndices = nullptr;
	};

	struct FrameAssemblyServices
	{
		void* runtimeMutationUser = nullptr;
		bool (*isRuntimeMutationReplacementActive)(void* user, uint32_t mapChunkIndex) = nullptr;
		void (*appendRuntimeMutationSceneLightRecords)(void* user, SceneLightSystem& sceneLights) = nullptr;
		void* persistentVoxelUser = nullptr;
		void (*appendPersistentVoxelSceneLights)(void* user, SceneLightSystem& sceneLights, uint32_t frameIndex, bool voxelStats) = nullptr;
	};

	struct FrameAssemblyTimingStats
	{
		double staticAppendMs = 0.0;
		double runtimeMutationAppendMs = 0.0;
		double capturedAppendMs = 0.0;
		double dynamicAppendMs = 0.0;
		double surfaceLightOverlayAppendMs = 0.0;
		double persistentVoxelAppendMs = 0.0;
	};

	struct PersistentDynamicSurfaceStats
	{
		uint32_t actorSurfaceCount = 0;
		uint32_t nonActorSurfaceCount = 0;
		uint32_t wallSurfaceCount = 0;
		uint32_t flatSurfaceCount = 0;
		uint32_t spriteSurfaceCount = 0;
		uint32_t actorFacingSpriteCount = 0;
		uint32_t actorVoxelSpriteCount = 0;
	};

	struct PersistentDynamicMergeStats
	{
		uint32_t liveWallSurfaceCount = 0;
		uint32_t liveFlatSurfaceCount = 0;
		uint32_t liveSpriteSurfaceCount = 0;
		uint32_t cacheWallSurfaceCount = 0;
		uint32_t cacheFlatSurfaceCount = 0;
		uint32_t cacheSpriteSurfaceCount = 0;
		uint32_t appendedWallSurfaceCount = 0;
		uint32_t appendedFlatSurfaceCount = 0;
		uint32_t appendedSpriteSurfaceCount = 0;
		uint32_t duplicateWallSurfaceCount = 0;
		uint32_t duplicateFlatSurfaceCount = 0;
		uint32_t duplicateSpriteSurfaceCount = 0;

		uint32_t LiveSurfaceCount() const { return liveWallSurfaceCount + liveFlatSurfaceCount + liveSpriteSurfaceCount; }
		uint32_t CacheSurfaceCount() const { return cacheWallSurfaceCount + cacheFlatSurfaceCount + cacheSpriteSurfaceCount; }
		uint32_t AppendedSurfaceCount() const { return appendedWallSurfaceCount + appendedFlatSurfaceCount + appendedSpriteSurfaceCount; }
		uint32_t DuplicateSurfaceCount() const { return duplicateWallSurfaceCount + duplicateFlatSurfaceCount + duplicateSpriteSurfaceCount; }
	};

	struct PersistentDynamicEmissiveCache
	{
		bool valid = false;
		uint32_t surfaceCount = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialCount = 0;
		PersistentDynamicSurfaceStats surfaceStats = {};
		nri_scene::SceneView sceneView;
		nri_scene::GeometryData geometry;
		nri_scene::MaterialBridgeData materialBridge;
	};

	struct PersistentDynamicEmissiveHighWaterStats
	{
		uint32_t surfaceCount = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialCount = 0;
		PersistentDynamicSurfaceStats surfaceStats = {};
	};

	struct ActorSpriteDebugStats
	{
		uint32_t lastPruneChecks = 0;
		uint32_t lastPruneMatches = 0;
		uint32_t lastPruneDroppedMissingActor = 0;
		uint32_t lastPruneDroppedMissingActorIndex = 0;
		uint32_t lastPruneDroppedNullLiveTexture = 0;
		uint32_t lastPruneDroppedTextureMismatch = 0;
		uint32_t lastPruneDroppedPaletteMismatch = 0;
	};

	struct PersistentDynamicEmissiveCacheBuildServices
	{
		using BuildGeometryFn = void (*)(void* user, const nri_scene::SceneView& sceneView, nri_scene::GeometryData& geometry, const char* label);
		using BuildMaterialsFn = void (*)(void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label);

		void* user = nullptr;
		BuildGeometryFn buildGeometry = nullptr;
		BuildMaterialsFn buildMaterials = nullptr;
		bool traceActorSpriteVerbose = false;
		bool traceActorSpriteMismatch = false;

		void BuildGeometry(const nri_scene::SceneView& sceneView, nri_scene::GeometryData& geometry, const char* label) const;
		void BuildMaterials(nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label) const;
	};

	void Reset();
	void ResetLevelState();
	void BeginFrame(uint64_t frameSerial);
	FrameAssemblyTimingStats AssembleFrameSurfaceRecords(
		const FrameAssemblyInput& input,
		const FrameAssemblyServices& services);
	void AppendSceneView(
		const nri_scene::SceneView& sceneView,
		const nri_scene::MaterialBridgeData& materials,
		SceneLightRecordSource source,
		uint32_t materialIndexBase = 0,
		uint32_t materialLookupIndexBase = 0,
		const SurfaceIdentityOverrides* identityOverrides = nullptr);
	void AppendSpriteSurfaces(
		const std::vector<nri_scene::SurfaceRef>& surfaces,
		const nri_scene::MaterialBridgeData& materials,
		SceneLightRecordSource source,
		uint32_t materialIndexBase = 0,
		uint32_t materialLookupIndexBase = 0,
		const std::vector<uint64_t>* identityOverrides = nullptr);
	void AppendSurfaceRecords(
		const std::vector<SurfaceRecord>& records,
		uint32_t materialIndexBase = 0);
	void MarkActorPublishedForOverlayActivation(int32_t actorIndex);
	bool HasActorAppearanceEvidence(int32_t actorIndex) const;
	SurfaceRecord BuildSurfaceRecord(
		const nri_scene::SurfaceRef& surface,
		const nri_scene::MaterialBridgeData& materials,
		SceneLightRecordSource source,
		uint32_t materialIndex = 0,
		uint32_t materialLookupIndex = 0,
		uint64_t identityOverride = 0) const;
	void RebuildAnalyticLights(
		uint32_t flickerTimeIndex,
		uint32_t renderFrameIndex,
		uint32_t maxActiveLights,
		const float* currentCameraPos = nullptr,
		const std::unordered_map<int32_t, std::vector<AnalyticLightRegistry::ActorOverlayRule>>* actorOverlayRules = nullptr,
		const std::unordered_map<uint32_t, AnalyticLightRegistry::ActorOverlayRule>* actorOverlayRulesById = nullptr,
		const std::vector<AnalyticLightRegistry::MapOverlayRule>* mapOverlayRules = nullptr);
	void RebuildEmissiveSurfaces(
		uint32_t maxActiveSurfaces,
		const std::vector<EmissiveOverrideRule>* overrideRules = nullptr,
		const std::vector<EmissiveOverrideRule>* surfaceLightFixtureRules = nullptr,
		const std::vector<EmissiveMaterialResponseRule>* materialResponseRules = nullptr);
	void RebuildSectorLighting(uint32_t frameIndex, uint32_t sectorCount);
	void BuildRuntimePointLightUpload(std::vector<NRIRuntimePointLightGpuData>& outLights) const;
	uint64_t BuildRuntimeLightPayloadHash() const;
	uint64_t BuildRuntimeLightClusterCameraHash(const RuntimeLightClusterBuildInput& input) const;
	void BuildRuntimeLightClusterUpload(
		const RuntimeLightClusterBuildInput& input,
		std::vector<NRIRuntimeLightTileHeaderGpuData>& outHeaders,
		std::vector<uint32_t>& outIndices,
		uint32_t& outTileCountX,
		uint32_t& outTileCountY,
		uint32_t& outTileIndexCount,
		uint32_t& outMaxTileOccupancy) const;

	struct EmissiveSamplingUploadStats
	{
		uint32_t surfaceStatic = 0;
		uint32_t surfaceCaptured = 0;
		uint32_t surfaceRuntimeMutation = 0;
		uint32_t surfaceDynamic = 0;
		uint32_t surfaceLightOverlay = 0;
		uint32_t surfacePersistentVoxel = 0;
		uint32_t outputStaticRecords = 0;
		uint32_t outputDynamicRecords = 0;
		uint32_t outputPersistentVoxelRecords = 0;
		uint64_t outputPersistentVoxelPrimitivesRepresented = 0;
		uint32_t skippedPersistentVoxelSurfaces = 0;
		uint32_t proposalBoundGrowthCount = 0;
		uint64_t lastProposalBoundGrowthStableKey = 0;
		float lastProposalBoundGrowthOldWeight = 0.0f;
		float lastProposalBoundGrowthNewWeight = 0.0f;
		bool lastProposalBoundGrowthWasAuthored = false;
		uint32_t proposalActiveCount = 0;
		uint32_t proposalRetainedDarkCount = 0;
		uint32_t proposalReactivatedCount = 0;
		uint32_t proposalRetiredMissingCount = 0;
		uint32_t proposalRetiredReplacedCount = 0;
		uint32_t proposalRecordCount = 0;
	};

	void BuildEmissiveSamplingUpload(
		const EmissiveSamplingBuildContext& context,
		NRIEmissivePrimitiveHeaderGpuData& outHeader,
		std::vector<NRIEmissivePrimitiveGpuData>& outPrimitives,
		std::vector<float>& outCdf,
		std::vector<NRIEmissiveMaterialResponseGpuData>& outMaterialResponses,
		std::vector<NRIEmissivePrimitiveDebugRecord>& outDebugRecords,
		EmissiveSamplingUploadStats* outStats = nullptr);
	uint64_t BuildEmissiveSamplingPayloadHash(const EmissiveSamplingBuildContext& context) const;
	void BuildSectorLightingUpload(
		float sectorLightMultiplier,
		bool sectorLightingEnabled,
		NRISectorLightHeaderGpuData& outHeader,
		std::vector<NRISectorLightGpuData>& outSectors) const;
	NRISectorLightingBoundState BuildSectorLightingBoundState(float sectorLightMultiplier) const;
	uint64_t BuildSectorLightingPayloadHash(float sectorLightMultiplier, bool sectorLightingEnabled) const;

	bool AddRuntimePointLight(const float position[3], const float color[3], float intensity, float radius, uint32_t maxLights, uint32_t& outId);
	bool UpdateRuntimePointLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius);
	bool RemoveRuntimePointLight(uint32_t id);
	bool ClearRuntimePointLights();
	void ResetRuntimePointLights();
	void PrintRuntimePointLights(uint32_t maxLights) const;
	void RefreshResolvedMuzzleFlashRuleLookup(const ResolvedLightOverlaySet& resolvedLightOverlays);
	void ResetMuzzleFlashOverlayState(const char* reason, uint32_t discardedEventCount, bool debug);
	size_t GetResolvedMuzzleFlashRuleCount() const { return mResolvedMuzzleFlashRuleLookup.size(); }
	std::string FormatResolvedMuzzleFlashRuleIdList(size_t limit = 16) const;
	void RefreshTransientMuzzleFlashLights(double currentTimeSeconds, const TArray<PathTracingWeaponLightEvent>& pendingEvents, bool debug);
	bool IsEmissiveSurfaceSectorResponseEligible(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface) const;
	bool IsEmissiveSurfaceMaterialResponseEligible(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface) const;
	float ResolveSectorEmissionScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, bool& outApplied) const;
	float ResolveSectorEmissionIntensityScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, float scale) const;
	float ResolveSectorEmissionReachScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, float scale) const;
	float ResolveEmissiveMaterialResponseScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, bool& outApplied) const;
	uint64_t BuildEmissiveSectorResponsePayloadHash() const;
	void ResetEmissiveSectorResponseCaches();
	void NotifyEmissiveSectorResponseEditModeChanges(uint32_t frameIndex, const float currentCameraPos[3]);
	void TraceEmissiveSectorResponseChange(uint32_t frameIndex, const float currentCameraPos[3], bool traceEnabled);
	bool AddManualAnalyticLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius);
	bool UpdateManualAnalyticLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius);
	bool RemoveManualAnalyticLight(uint32_t id);
	void ClearManualAnalyticLights();
	uint32_t GetManualAnalyticLightCount() const { return (uint32_t)mAnalyticLights.manualLights.size(); }
	void SetTransientAnalyticLights(const std::vector<SceneAnalyticLight>& lights);

	bool AddSpriteTileHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId);
	bool ClearSpriteTileHeuristics();
	void PrintSpriteTileLightHeuristics() const;

	bool AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId);
	bool ClearTextureEmissiveHeuristics();
	void PrintTextureEmissiveHeuristics() const;
	void PrintEmissiveSurfaceDump(
		const std::vector<NRIEmissivePrimitiveDebugRecord>& boundPrimitiveRecords,
		float boundTotalPower,
		const float currentCameraPos[3],
		float radius,
		uint32_t limit) const;
	void PrintSectorLightDump(
		const float currentCameraPos[3],
		float sectorLightMultiplier,
		float radius,
		uint32_t limit) const;
	void PrintSceneLightDump(
		const float currentCameraPos[3],
		const nri_scene::PTMapWorld& mapWorld,
		uint32_t frameIndex,
		float radius,
		uint32_t limit) const;
	bool MaterialWouldEmit(const nri_scene::MaterialLightingMetadata& metadata) const;
	bool ApplyEmissiveMaterialSettings(const nri_scene::MaterialLightingMetadata& metadata, nri_scene::MaterialData& inOutMaterial) const;
	bool IsEmissiveStableForSampling(uint64_t stableKey, uint32_t requiredFrames, const float center[3], float boundsRadius, float surfaceArea);
	void PruneEmissiveStableSurfaceStates();
	void ResetPersistentDynamicEmissiveCache();
	void ResetPersistentDynamicEmissiveHighWaterStats();
	void PrunePersistentDynamicEmissiveCacheToLiveActors(const PersistentDynamicEmissiveCacheBuildServices& services);
	bool RebuildPersistentDynamicEmissiveCache(
		const nri_scene::SceneView& sceneView,
		const nri_scene::MaterialBridgeData& materials,
		const PersistentDynamicEmissiveCacheBuildServices& services);
	PersistentDynamicSurfaceStats GatherPersistentDynamicEmissiveSurfaceStats() const;
	void UpdatePersistentDynamicEmissiveHighWaterStats(const PersistentDynamicSurfaceStats& currentStats);
	PersistentDynamicMergeStats MergePersistentDynamicEmissiveCacheIntoSceneView(nri_scene::SceneView& inOutSceneView) const;
	const PersistentDynamicEmissiveCache& GetPersistentDynamicEmissiveCache() const { return mPersistentDynamicEmissiveCache; }
	const PersistentDynamicEmissiveHighWaterStats& GetPersistentDynamicEmissiveHighWaterStats() const { return mPersistentDynamicEmissiveHighWaterStats; }
	const ActorSpriteDebugStats& GetActorSpriteDebugStats() const { return mActorSpriteDebugStats; }

	bool HasRecords() const { return !mSurfaceRecords.empty(); }
	uint64_t GetFrameSerial() const { return mFrameSerial; }
	const std::vector<SurfaceRecord>& GetSurfaceRecords() const { return mSurfaceRecords; }
	const FrameAppendStats& GetFrameAppendStats() const { return mFrameAppendStats; }
	static uint64_t ComputeSurfaceIdentityKey(
		SceneLightRecordSource source,
		const nri_scene::SurfaceProvenance& provenance,
		const float center[3]);

	const AnalyticLightRegistry& GetAnalyticLights() const { return mAnalyticLights; }
	const EmissiveSurfaceRegistry& GetEmissiveSurfaces() const { return mEmissiveSurfaces; }
	const SectorLightingRegistry& GetSectorLighting() const { return mSectorLighting; }
	const EnvironmentLightingState& GetEnvironmentLighting() const { return mEnvironmentLighting; }

	bool ConsumeAnalyticLightTopologyChanged();
	bool ConsumeAnalyticLightPropertiesChanged();
	bool ConsumeEmissiveSurfaceTopologyChanged();
	bool ConsumeEmissiveSurfacePropertiesChanged();
	bool ConsumeEmissiveMaterialBindingChanged();
	bool ConsumeEmissiveMaterialPropertiesChanged();
	bool ConsumeSectorLightingTopologyChanged();

private:
	struct TransientMuzzleFlashSlot
	{
		uint64_t stableKey = 0;
		uint32_t slotIndex = 0;
		uint32_t ruleId = 0;
		uint64_t sourceEventSerial = 0;
		int32_t emitterActorIndex = -1;
		float renderPosition[3] = {};
		float color[3] = { 1.0f, 1.0f, 1.0f };
		float peakIntensity = 0.0f;
		float radius = 0.0f;
		double activationTimeSeconds = 0.0;
		double endTimeSeconds = 0.0;
		bool occupied = false;
	};

	struct EmissiveStableSurfaceState
	{
		uint64_t lastFrameSerial = 0;
		uint32_t consecutiveFrames = 0;
		float center[3] = {};
		float boundsRadius = 0.0f;
		float surfaceArea = 0.0f;
	};

	static NRILightingSettings CaptureSettings();
	bool IsActorPublishedForOverlayActivation(int32_t actorIndex) const;
	bool IsActorSuppressedForFrame(int32_t actorIndex) const;
	void AppendSurfaceRecord(SurfaceRecord record, uint32_t materialIndexBase);
	void AppendSurfaceList(
		const std::vector<nri_scene::SurfaceRef>& surfaces,
		const nri_scene::MaterialBridgeData& materials,
		SceneLightRecordSource source,
		uint32_t materialIndexBase,
		uint32_t materialLookupIndexBase,
		uint32_t& inOutLocalMaterialIndex,
		const std::vector<uint64_t>* identityOverrides);

	AnalyticLightRegistry mAnalyticLights = {};
	EmissiveSurfaceRegistry mEmissiveSurfaces = {};
	SectorLightingRegistry mSectorLighting = {};
	EnvironmentLightingState mEnvironmentLighting = {};
	PersistentDynamicEmissiveCache mPersistentDynamicEmissiveCache = {};
	PersistentDynamicEmissiveHighWaterStats mPersistentDynamicEmissiveHighWaterStats = {};
	ActorSpriteDebugStats mActorSpriteDebugStats = {};
	NRIEmissiveSamplingDistribution mEmissiveSamplingDistribution;
	std::vector<SurfaceRecord> mSurfaceRecords;
	SurfaceRecordIndex mSurfaceRecordIndex = {};
	FrameAppendStats mFrameAppendStats = {};
	uint64_t mFrameSerial = 0;
	uint32_t mNextRuntimePointLightId = 1;
	std::unordered_set<uint64_t> mActivatedActorOverlayKeys;
	std::unordered_set<int32_t> mPublishedActorOverlayIndices;
	std::unordered_set<int32_t> mSuppressedActorIndices;
	std::unordered_map<uint64_t, EmissiveStableSurfaceState> mEmissiveStableSurfaceStates;
	std::unordered_map<std::string, ResolvedLightOverlayMuzzleFlashRule> mResolvedMuzzleFlashRuleLookup;
	std::vector<TransientMuzzleFlashSlot> mTransientMuzzleFlashSlots;
	std::vector<SceneAnalyticLight> mTransientMuzzleFlashLights;
	uint64_t mLastMuzzleFlashEventSerial = 0;
	bool mEmissiveSectorResponseTraceCacheValid = false;
	uint64_t mEmissiveSectorResponseTraceHash = 0;
	bool mEmissiveSectorResponseNotifyCacheValid = false;
	uint32_t mLastEmissiveSectorResponseNotifyFrame = 0;
	std::vector<float> mEmissiveSectorResponseNotifyScales;
	bool mSectorLightingEditNotifyCacheValid = false;
	uint32_t mLastSectorLightingEditNotifyFrame = 0;
	std::vector<uint64_t> mSectorLightingEditNotifyHashes;
};
