#ifndef NRI_SMOKE_LIGHT_PARAMETERS_HLSLI
#define NRI_SMOKE_LIGHT_PARAMETERS_HLSLI

// Ray-free light parameters shared by cached transient materialization and the
// scene-shadow builders. Include after SmokeConstants.hlsli declares the buffer.
#define NRI_SMOKE_LIGHT_SOURCE_POINT 0x1u
#define NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL 0x2u
#define NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL_SHADOW 0x4u
#define NRI_SMOKE_LIGHT_SOURCE_EMISSIVE 0x8u
#define NRI_SMOKE_LIGHT_SOURCE_INDIRECT 0x10u

float3 SmokeDirectionalColor()
{
	const uint packed = gSmokeConstants.DirectionalColorPacked;
	return float3(
		(float)(packed & 0xffu),
		(float)((packed >> 8u) & 0xffu),
		(float)((packed >> 16u) & 0xffu)) * (8.0 / 255.0);
}

float3 SmokeDirectionalDirection()
{
	return normalize(float3(
		gSmokeConstants.DirectionalDirectionX,
		gSmokeConstants.DirectionalDirectionY,
		gSmokeConstants.DirectionalDirectionZ));
}

#endif
