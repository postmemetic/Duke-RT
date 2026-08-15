#ifndef RAZE_NRI_PT_SHARED_HLSLI
#define RAZE_NRI_PT_SHARED_HLSLI

#include "NRI.hlsl"
#include "NRD.hlsli"
#include "TraceConstants.hlsli"
#include "SceneShadowContracts.hlsli"
#include "EmissiveLightContracts.hlsli"

#define SET_SAMPLERS 0
#define SET_SCENE_TEXTURES 1
#define SET_SCENE_DATA 2
#define SET_INPUTS 3
#define SET_OUTPUTS 4
#define SET_ROOT 5

#define MAX_SCENE_TEXTURES 512
#define NRI_FLAG_USE_JITTER 0x40u
#define NRI_JITTER_PHASE_SHIFT 16u
#define NRI_VOXEL_NORMAL_BLEND_SHIFT 24u
#define NRI_FLAG_FAST_EMISSIVE_SHADOW 0x100u
#define NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS 0x200u
#define NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW 0x400u
#define NRI_FLAG_TRACE_SHADER_STATS 0x800u
#define NRI_FLAG_PROBABILISTIC_INDIRECT 0x1000u
#define NRI_FLAG_INDIRECT_RADIANCE_CACHE 0x2000u
#define NRI_FLAG_INDIRECT_RADIANCE_CACHE_ACCEPT 0x4000u
#define NRI_FLAG_SPATIAL_ABSENCE_GATE 0x8000u
#define NRI_FLAG_SPATIAL_ACTOR_OCCURRENCE_GATE 0x800000u
#define NRI_TRACE_AUX_FILTER_COMPARE 0x4000u
#define NRI_TRACE_AUX_FILTER_QUERY 0x8000u
#define NRI_JITTER_PHASE_MASK 0x7fu
#define NRI_TAA_JITTER_PHASE_COUNT 8u

struct SectorLightHeaderData
{
	uint sectorCount;
	uint activeCount;
	uint pulsingCount;
	uint flags;
};

struct SectorLightData
{
	float3 ambientColor;
	float ambientIntensity;
	float3 hemisphereColor;
	float hemisphereAmount;
	float fogAmount;
	float pulseScale;
	uint sourceFlags;
	int paletteIndex;
	int lotag;
	int hitag;
};

struct ReprojectionData
{
	float4 currentViewToClipMatrix[4];
	float4 previousViewToClipMatrix[4];
	float4 currentWorldToViewMatrix[4];
	float4 previousWorldToViewMatrix[4];
	float2 currentJitter;
	float2 previousJitter;
};

struct SpatialAbsenceRecord
{
	uint Flags;
	uint Data0;
	uint Data1;
	uint Data2;
	float4 Payload0;
	float4 Payload1;
	float4 Payload2;
	float4 Payload3;
};

NRI_ROOT_CONSTANTS(NRITraceSceneConstants, gTraceConstants, 0, SET_ROOT);

float3 GetDirectionalPlaceholderColor()
{
	const uint packed = gTraceConstants.BounceCounts >> 8u;
	return float3(
		(float)(packed & 0xffu),
		(float)((packed >> 8u) & 0xffu),
		(float)((packed >> 16u) & 0xffu)) * (8.0 / 255.0);
}

float GetDirectionalPlaceholderAngularSize()
{
	const float normalized = (float)((gTraceConstants.ReservedTrace1 >> 16u) & 0xffffu) * (1.0 / 65535.0);
	return max(normalized * 1.2, 1e-4);
}

float GetDirectionalPlaceholderTanAngularSize()
{
	return tan(GetDirectionalPlaceholderAngularSize());
}

float GetTemporalHaltonSample(uint index, uint base)
{
	float inverseBase = 1.0 / float(base);
	float fraction = inverseBase;
	float result = 0.0;

	while (index > 0u)
	{
		result += fraction * float(index % base);
		index /= base;
		fraction *= inverseBase;
	}

	return result;
}

float2 GetTemporalJitterForFrame(uint frameIndex)
{
	if ((gTraceConstants.Flags & NRI_FLAG_USE_JITTER) == 0u)
	{
		return 0.0;
	}

	const uint phaseCount = max((gTraceConstants.Flags >> NRI_JITTER_PHASE_SHIFT) & NRI_JITTER_PHASE_MASK, 1u);
	const uint sampleIndex = (frameIndex % phaseCount) + 1u;
	return float2(
		GetTemporalHaltonSample(sampleIndex, 2u) - 0.5,
		GetTemporalHaltonSample(sampleIndex, 3u) - 0.5);
}

float2 GetCurrentTemporalJitter()
{
	return GetTemporalJitterForFrame(gTraceConstants.FrameIndex);
}

float2 GetPreviousTemporalJitter()
{
	const uint previousFrameIndex = gTraceConstants.FrameIndex > 0u ? gTraceConstants.FrameIndex - 1u : 0u;
	return GetTemporalJitterForFrame(previousFrameIndex);
}

RaytracingAccelerationStructure gWorldTlas : register(t0, space5);
StructuredBuffer<SceneVertex> gStaticVertices : register(t0, space2);
StructuredBuffer<uint> gStaticIndices : register(t1, space2);
StructuredBuffer<PrimitiveData> gStaticPrimitives : register(t2, space2);
StructuredBuffer<MaterialData> gStaticMaterials : register(t3, space2);
StructuredBuffer<SceneVertex> gDynamicVertices : register(t4, space2);
StructuredBuffer<uint> gDynamicIndices : register(t5, space2);
StructuredBuffer<PrimitiveData> gDynamicPrimitives : register(t6, space2);
StructuredBuffer<MaterialData> gDynamicMaterials : register(t7, space2);
StructuredBuffer<SceneInstanceData> gSceneInstances : register(t8, space2);
StructuredBuffer<PortalData> gScenePortals : register(t9, space2);
StructuredBuffer<RuntimePointLightData> gRuntimePointLights : register(t10, space2);
StructuredBuffer<RuntimeLightTileHeaderData> gRuntimeLightTileHeaders : register(t11, space2);
StructuredBuffer<uint> gRuntimeLightTileIndices : register(t12, space2);
StructuredBuffer<EmissivePrimitiveHeaderData> gEmissivePrimitiveHeaders : register(t13, space2);
StructuredBuffer<EmissivePrimitiveData> gEmissivePrimitives : register(t14, space2);
StructuredBuffer<float> gEmissivePrimitiveCdf : register(t15, space2);
StructuredBuffer<SectorLightHeaderData> gSectorLightHeaders : register(t16, space2);
StructuredBuffer<SectorLightData> gSectorLights : register(t17, space2);
StructuredBuffer<ReprojectionData> gReprojectionDataBuffer : register(t18, space2);
StructuredBuffer<uint> gVisibleChunkWords : register(t19, space2);
StructuredBuffer<uint> gVisibleFlatPlaneWords : register(t20, space2);
#if defined(NRI_ENABLE_PERSISTENT_VOXEL_SCENE)
StructuredBuffer<SceneVertex> gPersistentVoxelVertices : register(t21, space2);
StructuredBuffer<uint> gPersistentVoxelIndices : register(t22, space2);
StructuredBuffer<PrimitiveData> gPersistentVoxelPrimitives : register(t23, space2);
StructuredBuffer<MaterialData> gPersistentVoxelMaterials : register(t24, space2);
StructuredBuffer<EmissiveMaterialResponseData> gEmissiveMaterialResponses : register(t25, space2);
#endif
StructuredBuffer<SpatialAbsenceRecord> gSpatialAbsenceRecords : register(t26, space2);

SamplerState gLinearWrap : register(s0, space0);
SamplerState gLinearClamp : register(s1, space0);
SamplerState gPointWrap : register(s2, space0);
SamplerState gPointClamp : register(s3, space0);
Texture2D<float4> gPaletteLookup : register(t0, space1);
TextureCube<float4> gSkyTexture : register(t1, space1);
Texture2D<float4> gSceneTextures[MAX_SCENE_TEXTURES] : register(t2, space1);

Texture2D<float4> gHistoryInput : register(t0, space3);
// PT motion contract shared by TraceOpaque, NRD, TAA, and the upscaler:
// - xy: screen-space motion in pixel units, excluding temporal jitter, with oldUv = newUv + motion.xy / renderSize
// - z: 2.5D depth delta, viewZPrev - viewZ
// - w: Raze-local history/validity metadata. Positive values allow local history consumers to reproject;
//   current hit paths store current viewZ here, while bootstrap/miss paths write a negative sentinel.
// - NRD consumes xyz only and is configured in screen-space 2.5D mode through motionVectorScale.
// Target policy during the MV rebuild:
// - opaque hits and sky/miss paths should both eventually follow the same canonical reprojection story
// - sky/background should not remain on unconditional zero motion once the dedicated miss path lands
Texture2D<float4> gMotionInput : register(t1, space3);
Texture2D<float4> gViewZInput : register(t2, space3);
Texture2D<float4> gNormalRoughnessInput : register(t3, space3);
Texture2D<float4> gBaseColorInput : register(t4, space3);
Texture2D<float4> gComposedInput : register(t5, space3);
Texture2D<float4> gUpscaledInput : register(t6, space3);
Texture2D<float4> gValidationInput : register(t7, space3);
Texture2D<float4> gGuideDiffuseInput : register(t8, space3);
Texture2D<float4> gGuideSpecularInput : register(t9, space3);
Texture2D<float4> gGuideSpecHitInput : register(t10, space3);
Texture2D<float4> gShadowInput : register(t11, space3);
Texture2D<float4> gDirectLightingInput : register(t12, space3);
Texture2D<float4> gDirectEmissionInput : register(t13, space3);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gTraceOutput, u, 0, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gComposedOutput, u, 1, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float3>, gFinalOutput, u, 2, SET_OUTPUTS);
// See gMotionInput above for the authoritative PT motion-buffer contract.
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gMotionOutput, u, 3, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gViewZOutput, u, 4, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gNormalRoughnessOutput, u, 5, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gBaseColorOutput, u, 6, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gHistoryOutput, u, 7, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gUpscaledOutput, u, 8, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gGuideDiffuseOutput, u, 9, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gGuideSpecularOutput, u, 10, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gGuideSpecHitOutput, u, 11, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gShadowPenumbraOutput, u, 12, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gDirectLightingOutput, u, 13, SET_OUTPUTS);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gDirectEmissionOutput, u, 14, SET_OUTPUTS);
RWStructuredBuffer<uint> gTraceShaderStats : register(u15, space4);

#if defined(NRI_INDIRECT_RADIANCE_CACHE)
#include "IndirectRadianceCache.hlsli"
#endif

#endif
