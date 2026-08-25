#pragma once

#include "tarray.h"
#include "zstring.h"

class PClassActor;

enum class LightOverlayAnchorType : uint8_t
{
	None,
	Position,
	Sector,
	Wall,
};

enum class LightOverlayActorActivationPolicy : uint8_t
{
	Surface,
	Immediate,
};

enum class LightOverlaySmokeTrigger : uint8_t
{
	Spawn,
	Interval,
};

enum class LightOverlaySmokeDirectionPolicy : uint8_t
{
	Aim,
	Normal,
	Incoming,
};

enum class LightOverlaySmokeRepresentation : uint8_t
{
	Grid,
	Analytic,
};

enum class LightOverlaySmokeQueuePolicy : uint8_t
{
	Retry,
	Drop,
	Latest,
};

struct LightOverlaySourceLocation
{
	FString sourceName;
	int lumpNum = -1;
	int lineStart = 0;
	int lineEnd = 0;
	uint32_t orderIndex = 0;
};

struct ParsedLightOverlayDefaults
{
	bool present = false;
	LightOverlaySourceLocation source;
};

struct ParsedLightOverlayActorRule
{
	FString id;
	LightOverlaySourceLocation source;
	FString actorClassName;
	bool hasShadowReceive = false;
	bool shadowReceive = true;
	bool hasShadowCast = false;
	bool shadowCast = true;
	bool hasLightShadowCast = false;
	bool lightShadowCast = true;
	bool hasFullbright = false;
	bool fullbright = false;
	bool hasEmissiveStableFrames = false;
	uint32_t emissiveStableFrames = 0;
	bool hasActivationPolicy = false;
	LightOverlayActorActivationPolicy activationPolicy = LightOverlayActorActivationPolicy::Surface;
	bool hasTileFilter = false;
	int tileFilter = -1;
	FString lightType;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRange = false;
	float range = 0.0f;
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
	bool hasNudgeFromSurface = false;
	float nudgeFromSurfaceDistance = 0.0f;
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, 0.0f };
	bool hasFlicker = false;
	uint32_t flickerFrames = 0;
	bool hasRandom = false;
	float randomIntensityRange[2] = { 0.0f, 0.0f };
	bool hasLocalSpacePolicy = false;
	FString localSpacePolicy;
};

struct ParsedLightOverlayDirectionalRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, -1.0f };
	bool hasAngularSize = false;
	float angularSize = 0.0f;
	bool hasShadow = false;
	bool shadow = true;
};

struct ParsedLightOverlayMuzzleFlashRule
{
	FString id;
	LightOverlaySourceLocation source;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasIntensityRandom = false;
	float intensityRandomRange[2] = { 1.0f, 1.0f };
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRadiusRandom = false;
	float radiusRandomRange[2] = { 1.0f, 1.0f };
	bool hasDelaySeconds = false;
	float delaySeconds = 0.0f;
	bool hasDelayRandomSeconds = false;
	float delayRandomSecondsRange[2] = { 0.0f, 0.0f };
	bool hasDurationSeconds = false;
	float durationSeconds = 0.0f;
	bool hasDurationRandomSeconds = false;
	float durationRandomSecondsRange[2] = { 0.0f, 0.0f };
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
};

struct ParsedLightOverlayMapLightRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	FString lightType;
	LightOverlayAnchorType anchorType = LightOverlayAnchorType::None;
	bool hasAnchorPosition = false;
	float anchorPosition[3] = { 0.0f, 0.0f, 0.0f };
	int anchorIndex = -1;
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, 0.0f };
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRange = false;
	float range = 0.0f;
	bool hasFlicker = false;
	uint32_t flickerFrames = 0;
};

struct ParsedLightOverlayEmissiveOverrideRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	bool hasSectorFilter = false;
	int sectorFilter = -1;
	bool hasWallFilter = false;
	int wallFilter = -1;
	bool hasTileFilter = false;
	int tileFilter = -1;
	bool hasIntensityScale = false;
	float intensityScale = 1.0f;
	bool hasReachScale = false;
	float reachScale = 1.0f;
	bool hasSectorResponse = false;
	bool sectorResponse = true;
	bool hasSignalSector = false;
	int signalSector = -1;
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

struct LightOverlayTileRange
{
	int first = -1;
	int last = -1;
};

struct ParsedLightOverlayEmissiveMaterialResponseRule
{
	FString id;
	LightOverlaySourceLocation source;
	TArray<int> tileFilters;
	TArray<LightOverlayTileRange> tileRanges;
	TArray<FString> textureNames;
	bool hasMaterialResponse = false;
	bool materialResponse = true;
	bool hasMaterialResponseMin = false;
	float materialResponseMin = 0.0f;
	bool hasMaterialResponseMax = false;
	float materialResponseMax = 1.0f;
	bool hasVisibleGlowBlend = false;
	float visibleGlowBlend = 1.0f;
};

struct ParsedLightOverlaySurfaceLightRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	bool hasPosition = false;
	float position[3] = { 0.0f, 0.0f, 0.0f };
	bool hasNormal = false;
	float normal[3] = { 0.0f, 1.0f, 0.0f };
	bool hasSize = false;
	float size[2] = { 32.0f, 32.0f };
	bool hasRotation = false;
	float rotation = 0.0f;
	bool hasOffset = false;
	float offset = 0.5f;
	bool hasSector = false;
	int sector = -1;
	bool hasWall = false;
	int wall = -1;
	bool hasTile = false;
	int tile = -1;
	bool hasFixtureTexture = false;
	FString fixtureTexture;
	bool hasFixtureMaterialResponse = false;
	bool fixtureMaterialResponse = true;
	FString lightType;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 4.0f;
	bool hasRadius = false;
	float radius = 256.0f;
	bool hasSectorResponse = false;
	bool sectorResponse = true;
	bool hasSignalSector = false;
	int signalSector = -1;
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
	bool hasMaterialResponseMin = false;
	float materialResponseMin = 0.0f;
	bool hasMaterialResponseMax = false;
	float materialResponseMax = 1.0f;
};

struct ParsedLightOverlayActorOverrideRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	FString actorClassName;
	bool hasShadowReceive = false;
	bool shadowReceive = true;
	bool hasShadowCast = false;
	bool shadowCast = true;
};

struct ParsedLightOverlaySmokeStyle
{
	FString id;
	LightOverlaySourceLocation source;
	float density = 1.0f;
	float extinction = 0.04f;
	float albedo[3] = { 0.5f, 0.5f, 0.5f };
	float anisotropy = 0.0f;
	float radius = 8.0f;
	float expansionVelocity = 0.0f;
	float lifetime = 2.0f;
	float densityHalfLife = 1.0f;
	float riseVelocity = 0.0f;
	float velocityRandom = 0.0f;
	float velocityInherit = 0.0f;
	float buoyancy = 0.0f;
	float drag = 0.0f;
	float turbulence = 0.0f;
	float turbulenceScale = 0.0f;
	float temperature = 1.0f;
	float momentumScale = 1.0f;
	float coolingHalfLife = 2.0f;
};

struct ParsedLightOverlaySmokeActorRule
{
	FString id;
	LightOverlaySourceLocation source;
	FString actorClassName;
	FString ownerClassName;
	FString excludeOwnerClassName;
	LightOverlaySmokeTrigger trigger = LightOverlaySmokeTrigger::Spawn;
	LightOverlayActorActivationPolicy activationPolicy = LightOverlayActorActivationPolicy::Immediate;
	LightOverlaySmokeRepresentation representation = LightOverlaySmokeRepresentation::Grid;
	LightOverlaySmokeQueuePolicy queuePolicy = LightOverlaySmokeQueuePolicy::Retry;
	bool hasMaxLatencySeconds = false;
	float maxLatencySeconds = 0.0f;
	uint32_t analyticCarrierCount = 1;
	bool emitterForeground = false;
	FString styleId;
	uint32_t count = 1;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
	float spawnRadius = 0.0f;
	float densityScale = 1.0f;
	float radiusScale = 1.0f;
	float velocityCone = 0.0f;
	float velocityScale = 1.0f;
	float intervalSeconds = 0.1f;
	float pulseAmount = 0.0f;
	uint32_t pulsePeriodCadences = 1;
	float pulsePhase = 0.0f;
	float startTime = 0.0f;
	float startDistance = 0.0f;
	float spacing = 0.0f;
	uint32_t maxSegmentsPerFrame = 1;
};

struct ParsedLightOverlaySmokeEventRule
{
	FString id;
	LightOverlaySourceLocation source;
	FString styleId;
	LightOverlaySmokeRepresentation representation = LightOverlaySmokeRepresentation::Grid;
	LightOverlaySmokeQueuePolicy queuePolicy = LightOverlaySmokeQueuePolicy::Retry;
	bool hasMaxLatencySeconds = false;
	float maxLatencySeconds = 0.0f;
	uint32_t analyticCarrierCount = 1;
	uint32_t count = 1;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
	float offsetRandom[3] = { 0.0f, 0.0f, 0.0f };
	float spawnRadius = 0.0f;
	float densityScale = 1.0f;
	float radiusScale = 1.0f;
	float velocityCone = 0.0f;
	float velocityScale = 1.0f;
	float normalOffset = 0.0f;
	LightOverlaySmokeDirectionPolicy directionPolicy = LightOverlaySmokeDirectionPolicy::Aim;
};

struct ParsedLightOverlayMapSmokeEmitterRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	FString styleId;
	// Position and normal are authored in Build/world coordinates.
	bool hasPosition = false;
	float position[3] = { 0.0f, 0.0f, 0.0f };
	bool hasNormal = false;
	float normal[3] = { 0.0f, 0.0f, -1.0f };
	bool hasSize = false;
	// Width and length are clamped to [1, 256] Build/world units.
	float size[2] = { 1.0f, 1.0f };
	float rotation = 0.0f;
	float offset = 0.0f;
	uint32_t count = 1;
	float intervalSeconds = 0.1f;
	float spawnRadius = 0.0f;
	float densityScale = 1.0f;
	float radiusScale = 1.0f;
	float velocityScale = 0.0f;
	float velocityCone = 0.0f;
	uint32_t maxSegmentsPerFrame = 1;
};

struct LightOverlayMapSmokeEmitterRectangle
{
	float center[3] = {};
	float normal[3] = {};
	float halfAxisU[3] = {};
	float halfAxisV[3] = {};
};

bool BuildLightOverlayMapSmokeEmitterRectangle(
	const ParsedLightOverlayMapSmokeEmitterRule& rule,
	LightOverlayMapSmokeEmitterRectangle& outRectangle);

struct ParsedLightOverlaySourceFile
{
	FString sourceName;
	int lumpNum = -1;
	bool hadParseErrors = false;
};

struct ParsedLightOverlayDatabase
{
	uint32_t generation = 0;
	uint64_t contentHash = 0;
	bool hadParseErrors = false;
	ParsedLightOverlayDefaults defaults;
	TArray<ParsedLightOverlaySourceFile> sourceFiles;
	TArray<ParsedLightOverlayActorRule> actorRules;
	TArray<ParsedLightOverlayDirectionalRule> directionalRules;
	TArray<ParsedLightOverlayMuzzleFlashRule> muzzleFlashRules;
	TArray<ParsedLightOverlayMapLightRule> mapLightRules;
	TArray<ParsedLightOverlayEmissiveOverrideRule> emissiveOverrideRules;
	TArray<ParsedLightOverlayEmissiveMaterialResponseRule> emissiveMaterialResponseRules;
	TArray<ParsedLightOverlaySurfaceLightRule> surfaceLightRules;
	TArray<ParsedLightOverlayActorOverrideRule> actorOverrideRules;
	TArray<ParsedLightOverlaySmokeStyle> smokeStyles;
	TArray<ParsedLightOverlaySmokeActorRule> smokeActorRules;
	TArray<ParsedLightOverlaySmokeEventRule> smokeEventRules;
	TArray<ParsedLightOverlayMapSmokeEmitterRule> mapSmokeEmitterRules;
};

struct ResolvedLightOverlayActorRule
{
	FString id;
	LightOverlaySourceLocation source;
	FString actorClassName;
	PClassActor* actorClass = nullptr;
	bool actorClassResolved = false;
	bool hasShadowReceive = false;
	bool shadowReceive = true;
	bool hasShadowCast = false;
	bool shadowCast = true;
	bool hasLightShadowCast = false;
	bool lightShadowCast = true;
	bool hasFullbright = false;
	bool fullbright = false;
	bool hasEmissiveStableFrames = false;
	uint32_t emissiveStableFrames = 0;
	bool hasActivationPolicy = false;
	LightOverlayActorActivationPolicy activationPolicy = LightOverlayActorActivationPolicy::Surface;
	bool hasTileFilter = false;
	int tileFilter = -1;
	FString lightType;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRange = false;
	float range = 0.0f;
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
	bool hasNudgeFromSurface = false;
	float nudgeFromSurfaceDistance = 0.0f;
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, 0.0f };
	bool hasFlicker = false;
	uint32_t flickerFrames = 0;
	bool hasRandom = false;
	float randomIntensityRange[2] = { 0.0f, 0.0f };
	bool hasLocalSpacePolicy = false;
	FString localSpacePolicy;
};

struct ResolvedLightOverlayDirectionalRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, -1.0f };
	bool hasAngularSize = false;
	float angularSize = 0.0f;
	bool hasShadow = false;
	bool shadow = true;
};

struct ResolvedLightOverlayMuzzleFlashRule
{
	FString id;
	LightOverlaySourceLocation source;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasIntensityRandom = false;
	float intensityRandomRange[2] = { 1.0f, 1.0f };
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRadiusRandom = false;
	float radiusRandomRange[2] = { 1.0f, 1.0f };
	bool hasDelaySeconds = false;
	float delaySeconds = 0.0f;
	bool hasDelayRandomSeconds = false;
	float delayRandomSecondsRange[2] = { 0.0f, 0.0f };
	bool hasDurationSeconds = false;
	float durationSeconds = 0.0f;
	bool hasDurationRandomSeconds = false;
	float durationRandomSecondsRange[2] = { 0.0f, 0.0f };
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
};

struct ResolvedLightOverlayMapLightRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	FString lightType;
	LightOverlayAnchorType anchorType = LightOverlayAnchorType::None;
	bool hasAnchorPosition = false;
	float anchorPosition[3] = { 0.0f, 0.0f, 0.0f };
	int anchorIndex = -1;
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, 0.0f };
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRange = false;
	float range = 0.0f;
	bool hasFlicker = false;
	uint32_t flickerFrames = 0;
};

struct ResolvedLightOverlayEmissiveOverrideRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	bool hasSectorFilter = false;
	int sectorFilter = -1;
	bool hasWallFilter = false;
	int wallFilter = -1;
	bool hasTileFilter = false;
	int tileFilter = -1;
	bool hasIntensityScale = false;
	float intensityScale = 1.0f;
	bool hasReachScale = false;
	float reachScale = 1.0f;
	bool hasSectorResponse = false;
	bool sectorResponse = true;
	bool hasSignalSector = false;
	int signalSector = -1;
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

struct ResolvedLightOverlayEmissiveMaterialResponseRule
{
	FString id;
	LightOverlaySourceLocation source;
	TArray<int> tileFilters;
	TArray<LightOverlayTileRange> tileRanges;
	TArray<FString> textureNames;
	bool hasMaterialResponse = false;
	bool materialResponse = true;
	bool hasMaterialResponseMin = false;
	float materialResponseMin = 0.0f;
	bool hasMaterialResponseMax = false;
	float materialResponseMax = 1.0f;
	bool hasVisibleGlowBlend = false;
	float visibleGlowBlend = 1.0f;
};

struct ResolvedLightOverlaySurfaceLightRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	bool hasPosition = false;
	float position[3] = { 0.0f, 0.0f, 0.0f };
	bool hasNormal = false;
	float normal[3] = { 0.0f, 1.0f, 0.0f };
	bool hasSize = false;
	float size[2] = { 32.0f, 32.0f };
	bool hasRotation = false;
	float rotation = 0.0f;
	bool hasOffset = false;
	float offset = 0.5f;
	bool hasSector = false;
	int sector = -1;
	bool hasWall = false;
	int wall = -1;
	bool hasTile = false;
	int tile = -1;
	bool hasFixtureTexture = false;
	FString fixtureTexture;
	bool hasFixtureMaterialResponse = false;
	bool fixtureMaterialResponse = true;
	FString lightType;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 4.0f;
	bool hasRadius = false;
	float radius = 256.0f;
	bool hasSectorResponse = false;
	bool sectorResponse = true;
	bool hasSignalSector = false;
	int signalSector = -1;
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
	bool hasMaterialResponseMin = false;
	float materialResponseMin = 0.0f;
	bool hasMaterialResponseMax = false;
	float materialResponseMax = 1.0f;
};

struct ResolvedLightOverlayActorOverrideRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	FString actorClassName;
	PClassActor* actorClass = nullptr;
	bool actorClassResolved = false;
	bool hasShadowReceive = false;
	bool shadowReceive = true;
	bool hasShadowCast = false;
	bool shadowCast = true;
};

struct ResolvedLightOverlaySmokeStyle : ParsedLightOverlaySmokeStyle
{
	uint32_t styleIndex = 0;
};

struct ResolvedLightOverlaySmokeActorRule : ParsedLightOverlaySmokeActorRule
{
	PClassActor* actorClass = nullptr;
	bool actorClassResolved = false;
	PClassActor* ownerClass = nullptr;
	bool ownerClassResolved = false;
	PClassActor* excludeOwnerClass = nullptr;
	bool excludeOwnerClassResolved = false;
	uint32_t styleIndex = 0;
	bool styleResolved = false;
};

struct ResolvedLightOverlaySmokeEventRule : ParsedLightOverlaySmokeEventRule
{
	uint32_t styleIndex = 0;
	bool styleResolved = false;
};

struct ResolvedLightOverlayMapSmokeEmitterRule : ParsedLightOverlayMapSmokeEmitterRule
{
	uint32_t styleIndex = 0;
	bool styleResolved = false;
};

struct ResolvedLightOverlaySet
{
	uint32_t parsedGeneration = 0;
	uint32_t resolvedGeneration = 0;
	FString activeMapName;
	bool currentMapAvailable = false;
	TArray<ResolvedLightOverlayActorRule> actorRules;
	TArray<ResolvedLightOverlayDirectionalRule> directionalRules;
	TArray<ResolvedLightOverlayMuzzleFlashRule> muzzleFlashRules;
	TArray<ResolvedLightOverlayMapLightRule> mapLightRules;
	TArray<ResolvedLightOverlayEmissiveOverrideRule> emissiveOverrideRules;
	TArray<ResolvedLightOverlayEmissiveMaterialResponseRule> emissiveMaterialResponseRules;
	TArray<ResolvedLightOverlaySurfaceLightRule> surfaceLightRules;
	TArray<ResolvedLightOverlayActorOverrideRule> actorOverrideRules;
	TArray<ResolvedLightOverlaySmokeStyle> smokeStyles;
	TArray<ResolvedLightOverlaySmokeActorRule> smokeActorRules;
	TArray<ResolvedLightOverlaySmokeEventRule> smokeEventRules;
	TArray<ResolvedLightOverlayMapSmokeEmitterRule> mapSmokeEmitterRules;
};

enum class LightOverlayRuleKind : uint8_t
{
	ActorRule,
	Directional,
	MuzzleFlash,
	MapLight,
	EmissiveOverride,
	EmissiveMaterialResponse,
	SurfaceLight,
	ActorOverride,
	SmokeStyle,
	SmokeActorRule,
	SmokeEventRule,
	MapSmokeEmitter,
};

const ParsedLightOverlayDatabase& GetParsedLightOverlayDatabase();
const ResolvedLightOverlaySet& GetResolvedLightOverlaySet();
const ResolvedLightOverlaySet& ResolveLightOverlaysForMap(const char* mapName);
bool ParseLightOverlays(bool verbose = false);
bool ApplyParsedLightOverlayDatabase(const ParsedLightOverlayDatabase& database, bool verbose = false);
FString SerializeLightOverlayDatabase(const ParsedLightOverlayDatabase& database);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayActorRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayDirectionalRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayMuzzleFlashRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayMapLightRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayEmissiveOverrideRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayEmissiveMaterialResponseRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlaySurfaceLightRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayActorOverrideRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlaySmokeStyle& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlaySmokeActorRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlaySmokeEventRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayMapSmokeEmitterRule& rule, bool* outReplaced = nullptr);
bool RemoveLightOverlayRule(ParsedLightOverlayDatabase& database, LightOverlayRuleKind kind, const char* id, const char* mapName = nullptr);
