#ifndef RAZE_NRI_EMISSIVE_LIGHT_CONTRACTS_HLSLI
#define RAZE_NRI_EMISSIVE_LIGHT_CONTRACTS_HLSLI

struct SceneVertex
{
	float3 position;
	float3 prevPosition;
	float2 uv;
};

struct EmissivePrimitiveHeaderData
{
	uint activeCount;
	uint dominantIndex;
	uint flags;
	float totalPower;
};

struct EmissivePrimitiveData
{
	uint dataSource;
	uint primitiveIndex;
	float primitiveArea;
	float selectionPdf;
	float emissionScale;
	uint stableKeyLo;
	uint stableKeyHi;
	uint sceneInstanceIndex;
	uint primitiveCount;
	uint occurrenceKeyLo;
	uint occurrenceKeyHi;
	uint occurrenceGeneration;
	float3 boundsCenter;
	float boundsRadius;
	float materialResponseScale;
	float4 emissionColorResponse;
};

struct EmissiveMaterialResponseData
{
	uint dataSource;
	uint primitiveIndex;
	float materialScale;
	uint flags;
	float4 emissionColorResponse;
};

float3 ApplyEmissionColorResponse(float3 emission, float4 response)
{
	// RGB replaces hue; W retains authored color. A zero response stays dark.
	return emission * response.w + response.rgb * dot(max(emission, 0.0), float3(0.2126, 0.7152, 0.0722));
}

#endif
