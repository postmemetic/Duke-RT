#ifndef RAZE_NRI_SCENE_SHADOW_CONTRACTS_HLSLI
#define RAZE_NRI_SCENE_SHADOW_CONTRACTS_HLSLI

#define NRI_TLAS_MASK_MAIN 0x01u
#define NRI_TLAS_MASK_SHADOW 0x02u
#define NRI_TLAS_MASK_REFLECTION 0x04u
#define NRI_TLAS_MASK_GI 0x08u
#define NRI_TLAS_MASK_EMISSIVE 0x10u
#define NRI_TLAS_MASK_DEBUG 0x20u
#define NRI_TLAS_MASK_ALL_WORKLOADS 0xFFu

#define MATERIAL_FLAG_INDEXED 1
#define MATERIAL_FLAG_FULLBRIGHT 2
#define MATERIAL_FLAG_FLAT 4
#define MATERIAL_FLAG_SPRITE 8
#define MATERIAL_FLAG_MIRROR 16
#define MATERIAL_FLAG_SKY 32
#define MATERIAL_FLAG_PORTAL 64
#define MATERIAL_FLAG_ONE_WAY 128
#define MATERIAL_FLAG_ALPHA_CLIP 256
#define MATERIAL_FLAG_FACING_BILLBOARD 512
#define MATERIAL_FLAG_POINT_SAMPLED 1024
#define MATERIAL_FLAG_PLAIN_MIRROR 2048
#define MATERIAL_FLAG_TINT_EMISSION 4096
#define PRIMITIVE_FLAG_REFLECTION_ONLY 65536u

#define MATERIAL_LIGHTING_FLAG_NO_SHADOW_RECEIVE 32
#define MATERIAL_LIGHTING_FLAG_NO_SHADOW_CAST 64
#define MATERIAL_LIGHTING_FLAG_SMOKE_FOREGROUND 128

struct PrimitiveData
{
	uint3 indices;
	uint materialIndex;
	float2 uv0;
	float2 uv1;
	float2 uv2;
	float3 normal;
	uint flags;
	uint portalIndex;
	uint reserved0;
	uint2 smoothNormals;
	uint2 temporalSurfaceId;
	uint temporalGeneration;
	uint temporalFlags;
};

struct MaterialData
{
	uint textureIndex;
	uint paletteIndex;
	uint flags;
	uint materialClass;
	uint lightingFlags;
	uint normalTextureIndex;
	uint metallicTextureIndex;
	uint roughnessTextureIndex;
	uint sectorIndex;
	uint emissiveTextureIndex;
	float lightLevel;
	float alpha;
	float roughnessHint;
	float metalnessHint;
	float3 emissiveColor;
	float emissiveIntensity;
	float emissiveMaskScale;
	uint emissiveMode;
	float emissiveReserved;
};

struct SceneInstanceData
{
	uint primitiveBase;
	uint dataSource;
	uint materialBase;
	uint materialCount;
	uint visibilityChunk;
	uint metadata0;
	uint metadata1;
	uint metadata2;
	float4 currentTransformRow0;
	float4 currentTransformRow1;
	float4 currentTransformRow2;
	float4 previousTransformRow0;
	float4 previousTransformRow1;
	float4 previousTransformRow2;
};

struct PortalData
{
	uint traversalClass;
	uint kind;
	uint targetLocalSpaceIndex;
	uint flags;
	float3 delta;
	uint reserved0;
};

struct RuntimePointLightData
{
	float3 position;
	float radius;
	float3 color;
	float intensity;
	uint flags;
	float emitterRadius;
	uint stableKeyLo;
	uint stableKeyHi;
};

#define RUNTIME_POINT_LIGHT_FLAG_CASTS_SHADOW 0x1u

struct RuntimeLightTileHeaderData
{
	uint indexOffset;
	uint indexCount;
};

#endif
