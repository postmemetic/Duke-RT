#ifndef NRI_FLUID_SURFACE_CONTRACTS_HLSLI
#define NRI_FLUID_SURFACE_CONTRACTS_HLSLI

#define NRI_FLUID_INVALID_BINDING 0xffffffffu
#define NRI_FLUID_MAX_LAYERS 4u

#define NRI_FLUID_BINDING_FLAG_ENABLED (1u << 0u)
#define NRI_FLUID_BINDING_FLAG_TWO_SIDED (1u << 1u)
#define NRI_FLUID_BINDING_FLAG_DISABLE_VIRTUAL_HEIGHT (1u << 2u)
#define NRI_FLUID_BINDING_FLAG_FLIP_SIDE (1u << 3u)

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
	float4 spectrum;
	float4 motion;
	float4 shaping;
	float4 crestColorOpacity;
	float4 emissionColorIntensity;
	float4 crust;
	float4 virtualParams;
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
	uint reserved0;
	uint reserved1;
};

StructuredBuffer<FluidBindingData> gFluidBindings : register(t28, space2);
StructuredBuffer<FluidFrameData> gFluidFrames : register(t29, space2);

#endif
