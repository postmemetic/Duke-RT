#pragma once

#include "tarray.h"
#include "zstring.h"

#include <cstdint>

enum class FluidOverlayStyle : uint8_t
{
	Still,
	Waves,
	Flow,
	Fall,
	Viscous,
	Authored,
	Lava,
};

enum class FluidOverlaySurfaceClass : uint8_t
{
	Any,
	Flat,
	Wall,
};

enum FluidOverlayParameterBits : uint64_t
{
	FluidOverlayParameter_None = 0,
	FluidOverlayParameter_Style = 1ull << 0,
	FluidOverlayParameter_BodyColor = 1ull << 1,
	FluidOverlayParameter_UndersideColor = 1ull << 2,
	FluidOverlayParameter_CrestColor = 1ull << 3,
	FluidOverlayParameter_EmissionColor = 1ull << 4,
	FluidOverlayParameter_Emission = 1ull << 5,
	FluidOverlayParameter_Roughness = 1ull << 6,
	FluidOverlayParameter_UndersideRoughness = 1ull << 7,
	FluidOverlayParameter_Opacity = 1ull << 8,
	FluidOverlayParameter_Scale = 1ull << 9,
	FluidOverlayParameter_Amplitude = 1ull << 10,
	FluidOverlayParameter_Frequency = 1ull << 11,
	FluidOverlayParameter_TimeScale = 1ull << 12,
	FluidOverlayParameter_Speed = 1ull << 13,
	FluidOverlayParameter_Flow = 1ull << 14,
	FluidOverlayParameter_Anisotropy = 1ull << 15,
	FluidOverlayParameter_Warp = 1ull << 16,
	FluidOverlayParameter_Choppiness = 1ull << 17,
	FluidOverlayParameter_NormalStrength = 1ull << 18,
	FluidOverlayParameter_VirtualHeight = 1ull << 19,
	FluidOverlayParameter_Seed = 1ull << 20,
	FluidOverlayParameter_Octaves = 1ull << 21,
	FluidOverlayParameter_Lacunarity = 1ull << 22,
	FluidOverlayParameter_Gain = 1ull << 23,
	FluidOverlayParameter_OctaveRotation = 1ull << 24,
	FluidOverlayParameter_ChoppyRelaxation = 1ull << 25,
	FluidOverlayParameter_NoiseScale = 1ull << 26,
	FluidOverlayParameter_MaxSlope = 1ull << 27,
	FluidOverlayParameter_CrestGain = 1ull << 28,
	FluidOverlayParameter_FoamColor = 1ull << 29,
	FluidOverlayParameter_FoamAmount = 1ull << 30,
	FluidOverlayParameter_FoamRoughness = 1ull << 31,
	FluidOverlayParameter_FoamThreshold = 1ull << 32,
	FluidOverlayParameter_FoamSoftness = 1ull << 33,
	FluidOverlayParameter_FoamBreakupScale = 1ull << 34,
	FluidOverlayParameter_FoamBreakupStrength = 1ull << 35,
	FluidOverlayParameter_FoamLifetime = 1ull << 36,
	FluidOverlayParameter_FoamTrailGain = 1ull << 37,
	FluidOverlayParameter_FoamUndersideStrength = 1ull << 38,
	FluidOverlayParameter_FoamContrast = 1ull << 39,
	FluidOverlayParameter_FoamEdgeWidth = 1ull << 40,
	FluidOverlayParameter_FoamCutoutScale = 1ull << 41,
	FluidOverlayParameter_FoamCutoutStrength = 1ull << 42,
	FluidOverlayParameter_FoamBrightness = 1ull << 43,
	FluidOverlayParameter_CrustSoftness = 1ull << 44,
	FluidOverlayParameter_CrustDarkening = 1ull << 45,
	FluidOverlayParameter_TextureMix = 1ull << 46,
	FluidOverlayParameter_SpecularFresnel = 1ull << 47,
};

struct FluidOverlaySourceLocation
{
	FString sourceName;
	int lumpNum = -1;
	int lineStart = 0;
	int lineEnd = 0;
	uint32_t orderIndex = 0;
};

struct FluidOverlayTileRange
{
	int first = -1;
	int last = -1;
};

// A parsed parameter block uses presentMask to distinguish omitted fields from
// explicitly authored values. A resolved profile/binding has every field
// populated and uses presentMask == 0.
struct FluidOverlayParameters
{
	uint64_t presentMask = FluidOverlayParameter_None;
	FluidOverlayStyle style = FluidOverlayStyle::Still;
	float bodyColor[3] = { 0.025f, 0.11f, 0.16f };
	float undersideColor[3] = { 0.018f, 0.065f, 0.09f };
	float crestColor[3] = { 0.18f, 0.28f, 0.32f };
	float emissionColor[3] = {};
	float emission = 0.0f;
	float roughness = 0.08f;
	float undersideRoughness = 0.16f;
	// Opt in to angle-dependent dielectric reflection; preserve legacy fluid
	// specular response for existing profiles unless explicitly authored.
	bool specularFresnel = false;
	float opacity = 0.94f;
	// Negative preserves the legacy opacity-derived texture mix; zero selects
	// a fully procedural body color before the profile's opacity is applied.
	float textureMix = -1.0f;
	float scale = 96.0f;
	float amplitude = 0.10f;
	float frequency = 1.0f;
	float lacunarity = 1.90f;
	float gain = 0.28f;
	float timeScale = 1.0f;
	float speed = 0.25f;
	float flow[2] = { 1.0f, 0.0f };
	float anisotropy = 1.0f;
	float warp = 0.0f;
	float choppiness = 1.0f;
	float octaveRotation = 37.0f;
	float choppyRelaxation = 0.20f;
	float noiseScale = 0.65f;
	float maxSlope = 1.25f;
	float crestGain = 0.35f;
	// Lava's crest-to-crust transition and body-color darkening. Defaults match
	// the original fixed GPU response; other styles do not use the crust mask.
	float crustSoftness = 0.16f;
	float crustDarkening = 0.72f;
	float foamColor[3] = { 0.82f, 0.90f, 0.95f };
	// Zero is the exact default-off policy. The remaining values define the
	// response used only when an authored profile opts in.
	float foamAmount = 0.0f;
	float foamRoughness = 0.85f;
	float foamThreshold = 0.65f;
	float foamSoftness = 0.10f;
	float foamBreakupScale = 2.0f;
	float foamBreakupStrength = 0.50f;
	float foamLifetime = 0.0f;
	float foamTrailGain = 0.0f;
	float foamUndersideStrength = 0.0f;
	float foamContrast = 1.0f;
	float foamEdgeWidth = 0.10f;
	float foamCutoutScale = 2.0f;
	float foamCutoutStrength = 0.0f;
	float foamBrightness = 1.0f;
	float normalStrength = 0.75f;
	// Optional Phase 2C visual-only half-height in renderer world units.
	// Zero disables virtual-height refinement for this profile.
	float virtualHeight = 0.0f;
	int32_t seed = 0;
	uint32_t octaves = 3;
};

struct ParsedFluidOverlayProfile
{
	FString id;
	FluidOverlaySourceLocation source;
	FluidOverlayParameters parameters;
};

struct ParsedFluidOverlayMaterialRule
{
	FString mapName;
	FString id;
	FluidOverlaySourceLocation source;
	TArray<int> tileFilters;
	TArray<FluidOverlayTileRange> tileRanges;
	TArray<int> tileFamilies;
	TArray<FString> textureNames;
	bool hasPalette = false;
	int palette = 0;
	bool hasSurfaceClass = false;
	FluidOverlaySurfaceClass surfaceClass = FluidOverlaySurfaceClass::Any;
	bool hasProfile = false;
	FString profileId;
	bool hasDisable = false;
	bool disable = false;
	FluidOverlayParameters overrides;
};

struct ParsedFluidOverlaySourceFile
{
	FString sourceName;
	int lumpNum = -1;
	bool hadParseErrors = false;
};

struct ParsedFluidOverlayDatabase
{
	uint32_t generation = 0;
	uint64_t contentHash = 0;
	bool hadParseErrors = false;
	TArray<ParsedFluidOverlaySourceFile> sourceFiles;
	TArray<ParsedFluidOverlayProfile> profiles;
	TArray<ParsedFluidOverlayMaterialRule> materialRules;
};

struct ResolvedFluidOverlayProfile
{
	FString id;
	FluidOverlaySourceLocation source;
	bool builtIn = false;
	uint64_t contentKey = 0;
	FluidOverlayParameters parameters;
};

struct ResolvedFluidOverlayBinding
{
	uint32_t bindingIndex = UINT32_MAX;
	uint64_t contentKey = 0;
	FluidOverlayParameters parameters;
};

struct ResolvedFluidOverlayMaterialRule : ParsedFluidOverlayMaterialRule
{
	bool valid = false;
	bool resolvedDisable = false;
	uint32_t profileIndex = UINT32_MAX;
	uint32_t bindingIndex = UINT32_MAX;
	uint64_t bindingContentKey = 0;
	FluidOverlayParameters effectiveParameters;
};

struct ResolvedFluidOverlaySet
{
	uint32_t parsedGeneration = 0;
	uint32_t resolvedGeneration = 0;
	uint64_t contentHash = 0;
	FString activeMapName;
	bool currentMapAvailable = false;
	TArray<ResolvedFluidOverlayProfile> profiles;
	TArray<ResolvedFluidOverlayMaterialRule> materialRules;
	TArray<ResolvedFluidOverlayBinding> bindings;
};

// The core matcher deliberately consumes authored material identity. The NRI
// scene bridge is responsible for deriving tileFamily from engine animation
// metadata and for preserving these values while the sampled frame animates.
struct FluidOverlayMaterialIdentity
{
	int tile = -1;
	int tileFamily = -1;
	FString textureName;
	int palette = 0;
	FluidOverlaySurfaceClass surfaceClass = FluidOverlaySurfaceClass::Any;
};

struct FluidOverlayMaterialMatch
{
	bool matched = false;
	bool disabled = false;
	uint32_t ruleIndex = UINT32_MAX;
	uint32_t profileIndex = UINT32_MAX;
	uint32_t bindingIndex = UINT32_MAX;
	uint64_t bindingContentKey = 0;
};

const ParsedFluidOverlayDatabase& GetParsedFluidOverlayDatabase();
const ResolvedFluidOverlaySet& GetResolvedFluidOverlaySet();
const ResolvedFluidOverlaySet& ResolveFluidOverlaysForMap(const char* mapName);
bool ResolveFluidOverlayMaterial(
	const ResolvedFluidOverlaySet& resolved,
	const FluidOverlayMaterialIdentity& identity,
	FluidOverlayMaterialMatch& outMatch);
bool ParseFluidOverlays(bool verbose = false);
bool ApplyParsedFluidOverlayDatabase(const ParsedFluidOverlayDatabase& database, bool verbose = false);
FString SerializeFluidOverlayDatabase(const ParsedFluidOverlayDatabase& database);
