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
};

struct EmissiveMaterialResponseData
{
	uint dataSource;
	uint primitiveIndex;
	float materialScale;
	uint flags;
};

#endif
