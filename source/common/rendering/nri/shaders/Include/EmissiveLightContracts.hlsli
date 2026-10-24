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
	float3 emissionHue;
};

struct EmissiveMaterialResponseData
{
	uint dataSource;
	uint primitiveIndex;
	float materialScale;
	uint flags;
	float3 emissionHue;
	uint reserved;
};

float3 ReplaceEmissionHue(float3 emission, float3 normalizedHue)
{
	return any(normalizedHue > 0.0) ? normalizedHue * dot(max(emission, 0.0), float3(0.2126, 0.7152, 0.0722)) : emission;
}

#endif
