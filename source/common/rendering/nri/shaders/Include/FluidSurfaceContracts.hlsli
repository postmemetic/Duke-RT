#ifndef NRI_FLUID_SURFACE_CONTRACTS_HLSLI
#define NRI_FLUID_SURFACE_CONTRACTS_HLSLI

#define NRI_FLUID_INVALID_BINDING 0xffffffffu
#define NRI_FLUID_MAX_LAYERS 4u

#define NRI_FLUID_BINDING_FLAG_ENABLED (1u << 0u)
#define NRI_FLUID_BINDING_FLAG_TWO_SIDED (1u << 1u)
#define NRI_FLUID_BINDING_FLAG_DISABLE_VIRTUAL_HEIGHT (1u << 2u)
#define NRI_FLUID_BINDING_FLAG_FLIP_SIDE (1u << 3u)
#define NRI_FLUID_BINDING_FLAG_SPECULAR_FRESNEL (1u << 4u)

#define NRI_FLUID_STYLE_FLOW (1u << 0u)
#define NRI_FLUID_STYLE_FALL (1u << 1u)
#define NRI_FLUID_STYLE_VISCOUS (1u << 2u)
#define NRI_FLUID_STYLE_CRUST_EMISSION (1u << 3u)
#define NRI_FLUID_STYLE_AUTHORED_MACRO (1u << 4u)

#define NRI_FLUID_FRAME_FLAG_ENABLED (1u << 0u)
#define NRI_FLUID_FRAME_FLAG_VIRTUAL_HEIGHT (1u << 1u)

// Mirrors NRIFluidBindingGpuData. This is intentionally one flattened row per
// resolved material binding: profile, motion, and surface response remain one
// coherent upload/versioning unit.
struct FluidBindingData
{
	uint flags;
	uint styleFlags;
	uint layerCount;
	uint seed;

	float4 domainOriginScale;
	float4 domainAxisUFlowU;
	float4 domainAxisVFlowV;
	float4 topColorBaseMix;
	float4 undersideColorBaseMix;
	float4 surfaceResponse;
	// amplitude, frequency, lacunarity, gain
	float4 spectrum;
	// phase speed, time scale, anisotropy, value-noise phase warp
	float4 motion;
	// choppiness, choppy relaxation, noise scale, octave rotation radians
	float4 shaping;
	float4 crestColorOpacity;
	float4 emissionColorIntensity;
	float4 crust;
	// virtual half-height, maximum travel, crest-color gain, minimum view cosine
	float4 virtualParams;
	// foam color + amount; roughness + birth shaping; breakup + trail shaping
	float4 foamColorAmount;
	float4 foamBirthParams;
	float4 foamTrailParams;
	float4 foamStyleParams;
	// x = foam brightness; yzw reserved.
	float4 foamPresentationParams;
};

// Uploaded once per frame. Gameplay time, quality caps, and the runtime
// virtual-height switch live here so the trace root constants remain fixed.
struct FluidFrameData
{
	float currentGameplaySeconds;
	float previousGameplaySeconds;
	float globalTimeScale;
	float globalAmplitudeScale;

	uint flags;
	uint primaryLayerCap;
	uint firstSpecularLayerCap;
	uint deeperLayerCap;

	uint virtualStepCount;
	uint debugMode;
	// 0 = disabled, 1 = instantaneous crest birth, 2 = analytic trails.
	uint foamMode;
	uint foamStyleMode;
};

StructuredBuffer<FluidBindingData> gFluidBindings : register(t31, space2);
StructuredBuffer<FluidFrameData> gFluidFrames : register(t32, space2);

#endif
