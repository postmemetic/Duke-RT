#pragma once

#include <cstddef>
#include <cstdint>

// GPU-facing fluid contracts mirrored by shaders/Include/FluidSurfaceContracts.hlsli.
// Keep every field scalar or float4-aligned so StructuredBuffer layout is
// identical under DXIL and -fvk-use-dx-layout SPIR-V compilation.

constexpr uint32_t NRI_FLUID_INVALID_BINDING = UINT32_MAX;
constexpr uint32_t NRI_FLUID_MAX_LAYERS = 4u;

enum NRIFluidBindingFlags : uint32_t
{
	NRI_FLUID_BINDING_FLAG_NONE = 0u,
	NRI_FLUID_BINDING_FLAG_ENABLED = 1u << 0u,
	NRI_FLUID_BINDING_FLAG_TWO_SIDED = 1u << 1u,
	NRI_FLUID_BINDING_FLAG_DISABLE_VIRTUAL_HEIGHT = 1u << 2u,
	NRI_FLUID_BINDING_FLAG_FLIP_SIDE = 1u << 3u,
};

enum NRIFluidStyleFlags : uint32_t
{
	NRI_FLUID_STYLE_NONE = 0u,
	NRI_FLUID_STYLE_FLOW = 1u << 0u,
	NRI_FLUID_STYLE_FALL = 1u << 1u,
	NRI_FLUID_STYLE_VISCOUS = 1u << 2u,
	NRI_FLUID_STYLE_CRUST_EMISSION = 1u << 3u,
	NRI_FLUID_STYLE_AUTHORED_MACRO = 1u << 4u,
};

enum NRIFluidFrameFlags : uint32_t
{
	NRI_FLUID_FRAME_FLAG_NONE = 0u,
	NRI_FLUID_FRAME_FLAG_ENABLED = 1u << 0u,
	NRI_FLUID_FRAME_FLAG_VIRTUAL_HEIGHT = 1u << 1u,
};

struct NRIFluidBindingGpuData
{
	uint32_t flags = NRI_FLUID_BINDING_FLAG_NONE;
	uint32_t styleFlags = NRI_FLUID_STYLE_NONE;
	uint32_t layerCount = 0u;
	uint32_t seed = 0u;

	// xyz = world-space origin, w = world-to-domain scale.
	float domainOriginScale[4] = {};
	// xyz = preferred world-space U axis, w = authored U flow speed in
	// domain units per gameplay second.
	float domainAxisUFlowU[4] = {};
	// xyz = preferred world-space V axis, w = authored V flow speed in
	// domain units per gameplay second.
	float domainAxisVFlowV[4] = {};

	// rgb = terminal body color, a = contribution from sampled base texture.
	float topColorBaseMix[4] = {};
	float undersideColorBaseMix[4] = {};
	// x/y = top/underside roughness, z = normal strength, w = maximum slope.
	float surfaceResponse[4] = {};
	// x = base amplitude, y = base angular frequency, z = lacunarity,
	// w = amplitude gain.
	float spectrum[4] = {};
	// x = phase speed, y = gameplay-time scale, z = anisotropy,
	// w = deterministic value-noise phase-warp strength.
	float motion[4] = {};
	// x = choppiness, y = per-octave relaxation toward a sine wave,
	// z = value-noise scale, w = per-octave domain rotation in radians.
	float shaping[4] = {};
	// rgb = crest color, a = fluid body opacity/body-color blend.
	float crestColorOpacity[4] = {};
	// rgb = surface-only emission color, w = intensity.
	float emissionColorIntensity[4] = {};
	// x = hot-crack threshold, y = softness, z = response power,
	// w = body-color crust darkening.
	float crust[4] = {};
	// x = authored virtual half-height in world units, y = maximum ray travel
	// in world units, z = crest-color gain,
	// w = minimum absolute view/carrier cosine.
	float virtualParams[4] = {};
	// rgb = foam color, w = authored foam amount (zero is exact off).
	float foamColorAmount[4] = {};
	// x = foam roughness, y = breaking threshold, z = threshold softness,
	// w = underside strength.
	float foamBirthParams[4] = {};
	// x = breakup scale, y = breakup strength, z = analytic lifetime,
	// w = analytic trail gain.
	float foamTrailParams[4] = {};
	// x = contrast, y = edge width, z = cutout scale, w = cutout strength.
	float foamStyleParams[4] = {};
	// x = bubble scale, y = bubble strength, z = bubble rim width,
	// w = foam brightness.
	float foamBubbleParams[4] = {};
};

struct NRIFluidFrameGpuData
{
	float currentGameplaySeconds = 0.0f;
	float previousGameplaySeconds = 0.0f;
	float globalTimeScale = 1.0f;
	float globalAmplitudeScale = 1.0f;

	uint32_t flags = NRI_FLUID_FRAME_FLAG_NONE;
	uint32_t primaryLayerCap = NRI_FLUID_MAX_LAYERS;
	uint32_t firstSpecularLayerCap = 2u;
	uint32_t deeperLayerCap = 1u;

	uint32_t virtualStepCount = 1u;
	uint32_t debugMode = 0u;
	// 0 = disabled, 1 = instantaneous crest birth, 2 = analytic trails.
	uint32_t foamMode = 0u;
	uint32_t foamStyleMode = 0u;
};

static_assert(sizeof(NRIFluidBindingGpuData) == 304u,
	"NRIFluidBindingGpuData must match FluidBindingData in HLSL.");
static_assert(offsetof(NRIFluidBindingGpuData, domainOriginScale) == 16u,
	"NRIFluidBindingGpuData header layout mismatch.");
static_assert(offsetof(NRIFluidBindingGpuData, crestColorOpacity) == 160u,
	"NRIFluidBindingGpuData crest layout mismatch.");
static_assert(offsetof(NRIFluidBindingGpuData, virtualParams) == 208u,
	"NRIFluidBindingGpuData tail layout mismatch.");
static_assert(offsetof(NRIFluidBindingGpuData, foamColorAmount) == 224u,
	"NRIFluidBindingGpuData foam-color layout mismatch.");
static_assert(offsetof(NRIFluidBindingGpuData, foamBirthParams) == 240u,
	"NRIFluidBindingGpuData foam-birth layout mismatch.");
static_assert(offsetof(NRIFluidBindingGpuData, foamTrailParams) == 256u,
	"NRIFluidBindingGpuData foam-trail layout mismatch.");
static_assert(offsetof(NRIFluidBindingGpuData, foamStyleParams) == 272u,
	"NRIFluidBindingGpuData foam-style layout mismatch.");
static_assert(offsetof(NRIFluidBindingGpuData, foamBubbleParams) == 288u,
	"NRIFluidBindingGpuData foam-bubble layout mismatch.");
static_assert(sizeof(NRIFluidFrameGpuData) == 48u,
	"NRIFluidFrameGpuData must match FluidFrameData in HLSL.");
