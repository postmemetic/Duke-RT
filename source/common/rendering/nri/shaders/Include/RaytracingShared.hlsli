#ifndef RAZE_NRI_PT_RAYTRACING_SHARED_HLSLI
#define RAZE_NRI_PT_RAYTRACING_SHARED_HLSLI

#include "Shared.hlsli"

struct HitData
{
	bool hit;
	uint dataSource;
	uint primitiveIndex;
	uint portalIndex;
	uint instanceId;
#if defined(NRI_INDIRECT_RADIANCE_CACHE)
	uint pathFlags;
#endif
	float2 barycentrics;
	float distance;
	float3 position;
	float3 normal;
	float2 uv;
	uint materialIndex;
};

static const uint SCENE_DATA_SOURCE_STATIC = 0u;
static const uint SCENE_DATA_SOURCE_DYNAMIC = 1u;
static const uint SCENE_DATA_SOURCE_PERSISTENT_VOXEL = 2u;
static const uint PORTAL_TRAVERSAL_CLASS_NONE = 0u;
static const uint PORTAL_TRAVERSAL_CLASS_REFLECTIVE = 1u;
static const uint PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER = 2u;
static const uint PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND = 3u;
#if defined(NRI_INDIRECT_RADIANCE_CACHE)
static const uint HIT_PATH_FLAG_REFLECTION = 1u;
static const uint HIT_PATH_FLAG_SPACE_TRANSFER = 2u;
#endif
static const float TRACE_MIN_DISTANCE = 1e-4;
static const float TRACE_FILTER_CONTINUE_BIAS = 1e-6;
static const uint TRACE_FILTER_SKIP_LIMIT = 64u;
static const uint TRACE_STATS_KIND_PRIMARY = 0u;
static const uint TRACE_STATS_KIND_UNGATED = 1u;
static const uint TRACE_STATS_KIND_SUN = 2u;
static const uint TRACE_STATS_KIND_POINT = 3u;
static const uint TRACE_STATS_KIND_EMISSIVE = 4u;
static const uint TRACE_STATS_KIND_FAST_EMISSIVE = 5u;
static const uint TRACE_STATS_KIND_COUNT = 6u;
static const uint TRACE_STAT_CALLS = 0u;
static const uint TRACE_STAT_PRIMARY_CALLS = 1u;
static const uint TRACE_STAT_UNGATED_CALLS = 2u;
static const uint TRACE_STAT_SUN_CALLS = 3u;
static const uint TRACE_STAT_POINT_CALLS = 4u;
static const uint TRACE_STAT_EMISSIVE_CALLS = 5u;
static const uint TRACE_STAT_FAST_EMISSIVE_CALLS = 6u;
static const uint TRACE_STAT_COMMITTED = 7u;
static const uint TRACE_STAT_MISS = 8u;
static const uint TRACE_STAT_ACCEPT_STATIC = 9u;
static const uint TRACE_STAT_ACCEPT_DYNAMIC = 10u;
static const uint TRACE_STAT_ACCEPT_VOXEL = 11u;
static const uint TRACE_STAT_FILTER_SKIPS = 12u;
static const uint TRACE_STAT_MAX_SKIP = 13u;
static const uint TRACE_STAT_SKIP_LIMIT = 14u;
static const uint TRACE_STAT_REJECT_REFLECTION = 15u;
static const uint TRACE_STAT_REJECT_VISIBLE = 16u;
static const uint TRACE_STAT_REJECT_HIDDEN_FLAT = 17u;
static const uint TRACE_STAT_REJECT_ONEWAY = 18u;
static const uint TRACE_STAT_REJECT_TRANSPARENT = 19u;
static const uint TRACE_STAT_REJECT_NO_SHADOW = 20u;
static const uint TRACE_STAT_REJECT_STATIC = 21u;
static const uint TRACE_STAT_REJECT_DYNAMIC = 22u;
static const uint TRACE_STAT_REJECT_VOXEL = 23u;
static const uint TRACE_STAT_RUNTIME_CANDIDATES = 24u;
static const uint TRACE_STAT_RUNTIME_DISTANCE = 25u;
static const uint TRACE_STAT_RUNTIME_LAMBERT = 26u;
static const uint TRACE_STAT_RUNTIME_SHADOW_RAYS = 27u;
static const uint TRACE_STAT_EMISSIVE_SAMPLES = 28u;
static const uint TRACE_STAT_EMISSIVE_SHADOW_RAYS = 29u;
static const uint TRACE_STAT_INSTANCE_COMMITTED_OVERFLOW = 30u;
static const uint TRACE_STAT_INSTANCE_ACCEPTED_OVERFLOW = 31u;
static const uint TRACE_STAT_PRIMARY_HIT_PIXELS = 32u;
static const uint TRACE_STAT_PRIMARY_MISS_PIXELS = 33u;
static const uint TRACE_STAT_PRIMARY_HIT_STATIC = 34u;
static const uint TRACE_STAT_PRIMARY_HIT_DYNAMIC = 35u;
static const uint TRACE_STAT_PRIMARY_HIT_VOXEL = 36u;
static const uint TRACE_STAT_MATERIAL_FULLBRIGHT = 37u;
static const uint TRACE_STAT_MATERIAL_EMISSIVE = 38u;
static const uint TRACE_STAT_DIRECTIONAL_SHADOW_TESTS = 39u;
static const uint TRACE_STAT_RUNTIME_TILE_NONEMPTY = 40u;
static const uint TRACE_STAT_RUNTIME_TILE_MAX = 41u;
static const uint TRACE_STAT_RUNTIME_SHADOW_VISIBLE = 42u;
static const uint TRACE_STAT_RUNTIME_SHADOW_OCCLUDED = 43u;
static const uint TRACE_STAT_EMISSIVE_CANDIDATE_NONE = 44u;
static const uint TRACE_STAT_EMISSIVE_LIGHT_ZERO = 45u;
static const uint TRACE_STAT_EMISSIVE_DISTANCE_REJECT = 46u;
static const uint TRACE_STAT_EMISSIVE_RECEIVER_LAMBERT_REJECT = 47u;
static const uint TRACE_STAT_EMISSIVE_EMITTER_LAMBERT_REJECT = 48u;
static const uint TRACE_STAT_EMISSIVE_VISIBILITY_VISIBLE = 49u;
static const uint TRACE_STAT_EMISSIVE_VISIBILITY_OCCLUDED = 50u;
static const uint TRACE_STAT_EMISSIVE_CONTRIBUTED = 51u;
static const uint TRACE_STAT_INDIRECT_DIFFUSE_CALLS = 52u;
static const uint TRACE_STAT_INDIRECT_DIFFUSE_BOUNCES = 53u;
static const uint TRACE_STAT_INDIRECT_SPECULAR_CALLS = 54u;
static const uint TRACE_STAT_INDIRECT_SPECULAR_BOUNCES = 55u;
static const uint TRACE_STAT_INDIRECT_DIFFUSE_MISSES = 56u;
static const uint TRACE_STAT_INDIRECT_SPECULAR_MISSES = 57u;
static const uint TRACE_STAT_FAST_EMISSIVE_SHADOW_CALLS = 58u;
static const uint TRACE_STAT_TRACED_EMISSIVE_SHADOW_CALLS = 59u;
static const uint TRACE_STAT_POINT_SHADOW_CALLS = 60u;
static const uint TRACE_STAT_SUN_SHADOW_CALLS = 61u;
static const uint TRACE_STAT_RUNTIME_SOFT_SHADOW_SAMPLES = 62u;
static const uint TRACE_STAT_RUNTIME_SOFT_TRANSPORT = 63u;
static const uint TRACE_STAT_INSTANCE_BUCKET_COUNT = 1024u;
static const uint TRACE_STAT_INSTANCE_COMMITTED_BASE = 64u;
static const uint TRACE_STAT_INSTANCE_ACCEPTED_BASE = TRACE_STAT_INSTANCE_COMMITTED_BASE + TRACE_STAT_INSTANCE_BUCKET_COUNT;
static const uint TRACE_STAT_INSTANCE_KIND_COMMITTED_BASE = TRACE_STAT_INSTANCE_ACCEPTED_BASE + TRACE_STAT_INSTANCE_BUCKET_COUNT;

bool TraceShaderStatsEnabled()
{
	return (gTraceConstants.Flags & NRI_FLAG_TRACE_SHADER_STATS) != 0u;
}

void TraceShaderStatAdd(uint index, uint value)
{
	if (TraceShaderStatsEnabled())
	{
		InterlockedAdd(gTraceShaderStats[index], value);
	}
}

void TraceShaderStatMax(uint index, uint value)
{
	if (TraceShaderStatsEnabled())
	{
		InterlockedMax(gTraceShaderStats[index], value);
	}
}

void TraceShaderStatCall(uint kind)
{
	TraceShaderStatAdd(TRACE_STAT_CALLS, 1u);
	TraceShaderStatAdd(TRACE_STAT_PRIMARY_CALLS + min(kind, TRACE_STATS_KIND_FAST_EMISSIVE), 1u);
}

void TraceShaderStatSource(uint staticIndex, uint dynamicIndex, uint voxelIndex, uint dataSource)
{
	if (dataSource == SCENE_DATA_SOURCE_DYNAMIC)
	{
		TraceShaderStatAdd(dynamicIndex, 1u);
	}
	else if (dataSource == SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
	{
		TraceShaderStatAdd(voxelIndex, 1u);
	}
	else
	{
		TraceShaderStatAdd(staticIndex, 1u);
	}
}

void TraceShaderStatInstance(uint baseIndex, uint overflowIndex, uint instanceId)
{
	if (instanceId < TRACE_STAT_INSTANCE_BUCKET_COUNT)
	{
		TraceShaderStatAdd(baseIndex + instanceId, 1u);
	}
	else
	{
		TraceShaderStatAdd(overflowIndex, 1u);
	}
}

void TraceShaderStatInstanceKind(uint kind, uint instanceId)
{
	if (instanceId < TRACE_STAT_INSTANCE_BUCKET_COUNT)
	{
		const uint clampedKind = min(kind, TRACE_STATS_KIND_FAST_EMISSIVE);
		TraceShaderStatAdd(TRACE_STAT_INSTANCE_KIND_COMMITTED_BASE + clampedKind * TRACE_STAT_INSTANCE_BUCKET_COUNT + instanceId, 1u);
	}
}

HitData MakeEmptyHitData()
{
	HitData hitData = (HitData)0;
	hitData.primitiveIndex = 0xffffffffu;
	hitData.materialIndex = 0xffffffffu;
	hitData.portalIndex = 0xffffffffu;
	hitData.instanceId = 0xffffffffu;
	return hitData;
}

SceneInstanceData GetSceneInstanceData(uint instanceId)
{
	return gSceneInstances[min(instanceId, max(gTraceConstants.SceneInstanceCount, 1u) - 1u)];
}

float3 TransformSceneInstancePoint(SceneInstanceData instanceData, float3 localPosition, bool previous)
{
	const float4 row0 = previous ? instanceData.previousTransformRow0 : instanceData.currentTransformRow0;
	const float4 row1 = previous ? instanceData.previousTransformRow1 : instanceData.currentTransformRow1;
	const float4 row2 = previous ? instanceData.previousTransformRow2 : instanceData.currentTransformRow2;
	const float4 p = float4(localPosition, 1.0);
	return float3(dot(row0, p), dot(row1, p), dot(row2, p));
}

float3 TransformSceneInstanceVector(SceneInstanceData instanceData, float3 localVector, bool previous)
{
	const float4 row0 = previous ? instanceData.previousTransformRow0 : instanceData.currentTransformRow0;
	const float4 row1 = previous ? instanceData.previousTransformRow1 : instanceData.currentTransformRow1;
	const float4 row2 = previous ? instanceData.previousTransformRow2 : instanceData.currentTransformRow2;
	return float3(dot(row0.xyz, localVector), dot(row1.xyz, localVector), dot(row2.xyz, localVector));
}

float3 TransformSceneInstanceNormal(SceneInstanceData instanceData, float3 localNormal, bool previous)
{
	const float3 transformed = TransformSceneInstanceVector(instanceData, localNormal, previous);
	return dot(transformed, transformed) > 1e-8 ? normalize(transformed) : normalize(localNormal);
}

uint GetPersistentVoxelPrimitiveCount()
{
#if defined(NRI_ENABLE_PERSISTENT_VOXEL_SCENE)
	uint count = 0u;
	uint stride = 0u;
	gPersistentVoxelPrimitives.GetDimensions(count, stride);
	return count;
#else
	return gTraceConstants.DynamicPrimitiveCount;
#endif
}

uint GetPersistentVoxelVertexCount()
{
#if defined(NRI_ENABLE_PERSISTENT_VOXEL_SCENE)
	uint count = 0u;
	uint stride = 0u;
	gPersistentVoxelVertices.GetDimensions(count, stride);
	return count;
#else
	return gTraceConstants.DynamicVertexCount;
#endif
}

uint GetPersistentVoxelMaterialCount()
{
#if defined(NRI_ENABLE_PERSISTENT_VOXEL_SCENE)
	uint count = 0u;
	uint stride = 0u;
	gPersistentVoxelMaterials.GetDimensions(count, stride);
	return count;
#else
	return gTraceConstants.DynamicMaterialCount;
#endif
}

uint GetPrimitiveCount(uint dataSource)
{
	if (dataSource == SCENE_DATA_SOURCE_DYNAMIC)
	{
		return gTraceConstants.DynamicPrimitiveCount;
	}
	if (dataSource == SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
	{
		return GetPersistentVoxelPrimitiveCount();
	}
	return gTraceConstants.StaticPrimitiveCount;
}

uint GetMaterialCount(uint dataSource)
{
	if (dataSource == SCENE_DATA_SOURCE_DYNAMIC)
	{
		return gTraceConstants.DynamicMaterialCount;
	}
	if (dataSource == SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
	{
		return GetPersistentVoxelMaterialCount();
	}
	return gTraceConstants.StaticMaterialCount;
}

PrimitiveData GetPrimitiveData(uint dataSource, uint primitiveIndex)
{
	if (dataSource == SCENE_DATA_SOURCE_DYNAMIC)
	{
		return gDynamicPrimitives[min(primitiveIndex, max(gTraceConstants.DynamicPrimitiveCount, 1u) - 1u)];
	}
	if (dataSource == SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
	{
#if defined(NRI_ENABLE_PERSISTENT_VOXEL_SCENE)
		return gPersistentVoxelPrimitives[min(primitiveIndex, max(GetPersistentVoxelPrimitiveCount(), 1u) - 1u)];
#else
		return gDynamicPrimitives[min(primitiveIndex, max(gTraceConstants.DynamicPrimitiveCount, 1u) - 1u)];
#endif
	}

	return gStaticPrimitives[min(primitiveIndex, max(gTraceConstants.StaticPrimitiveCount, 1u) - 1u)];
}

SceneVertex GetVertexData(uint dataSource, uint vertexIndex)
{
	if (dataSource == SCENE_DATA_SOURCE_DYNAMIC)
	{
		return gDynamicVertices[vertexIndex];
	}
	if (dataSource == SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
	{
#if defined(NRI_ENABLE_PERSISTENT_VOXEL_SCENE)
		return gPersistentVoxelVertices[min(vertexIndex, max(GetPersistentVoxelVertexCount(), 1u) - 1u)];
#else
		return gDynamicVertices[vertexIndex];
#endif
	}

	return gStaticVertices[vertexIndex];
}

MaterialData GetMaterialData(uint materialIndex, uint dataSource)
{
	if (dataSource == SCENE_DATA_SOURCE_DYNAMIC)
	{
		return gDynamicMaterials[min(materialIndex, max(gTraceConstants.DynamicMaterialCount, 1u) - 1u)];
	}
	if (dataSource == SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
	{
#if defined(NRI_ENABLE_PERSISTENT_VOXEL_SCENE)
		return gPersistentVoxelMaterials[min(materialIndex, max(GetPersistentVoxelMaterialCount(), 1u) - 1u)];
#else
		return gDynamicMaterials[min(materialIndex, max(gTraceConstants.DynamicMaterialCount, 1u) - 1u)];
#endif
	}

	return gStaticMaterials[min(materialIndex, max(gTraceConstants.StaticMaterialCount, 1u) - 1u)];
}

bool MaterialReceivesShadow(MaterialData material)
{
	return (material.lightingFlags & MATERIAL_LIGHTING_FLAG_NO_SHADOW_RECEIVE) == 0u;
}

bool MaterialCastsShadow(MaterialData material)
{
	return (material.lightingFlags & MATERIAL_LIGHTING_FLAG_NO_SHADOW_CAST) == 0u;
}

PortalData GetPortalData(uint portalIndex)
{
	PortalData portal = (PortalData)0;
	portal.targetLocalSpaceIndex = 0xffffffffu;
	if (gTraceConstants.PortalCount == 0u || portalIndex == 0xffffffffu)
	{
		return portal;
	}

	return gScenePortals[min(portalIndex, gTraceConstants.PortalCount - 1u)];
}

bool UseFastEmissiveShadow()
{
	return (gTraceConstants.Flags & NRI_FLAG_FAST_EMISSIVE_SHADOW) != 0;
}

float4 SampleMaterialColor(MaterialData material, uint textureIndex, float2 uv, bool indexed, bool applyPaletteLookup, bool applyLightLevel);

bool TryResolveHitTangentFrame(uint dataSource, uint primitiveIndex, float3 geometricNormal, out float3 tangent, out float3 bitangent)
{
	const PrimitiveData primitive = GetPrimitiveData(dataSource, primitiveIndex);
	const SceneVertex v0 = GetVertexData(dataSource, primitive.indices.x);
	const SceneVertex v1 = GetVertexData(dataSource, primitive.indices.y);
	const SceneVertex v2 = GetVertexData(dataSource, primitive.indices.z);
	const float3 p0 = v0.position;
	const float3 p1 = v1.position;
	const float3 p2 = v2.position;
	const float2 uv0 = primitive.uv0;
	const float2 uv1 = primitive.uv1;
	const float2 uv2 = primitive.uv2;
	const float3 edge1 = p1 - p0;
	const float3 edge2 = p2 - p0;
	const float2 duv1 = uv1 - uv0;
	const float2 duv2 = uv2 - uv0;
	const float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
	if (abs(determinant) <= 1e-6)
	{
		tangent = 0.0;
		bitangent = 0.0;
		return false;
	}

	float3 tangentRaw = (edge1 * duv2.y - edge2 * duv1.y) / determinant;
	float3 bitangentRaw = (edge2 * duv1.x - edge1 * duv2.x) / determinant;
	tangentRaw -= geometricNormal * dot(geometricNormal, tangentRaw);
	const float tangentLengthSq = dot(tangentRaw, tangentRaw);
	const float bitangentLengthSq = dot(bitangentRaw, bitangentRaw);
	if (tangentLengthSq <= 1e-8 || bitangentLengthSq <= 1e-8)
	{
		tangent = 0.0;
		bitangent = 0.0;
		return false;
	}

	tangent = tangentRaw * rsqrt(tangentLengthSq);
	bitangent = normalize(cross(geometricNormal, tangent));
	const float handedness = dot(cross(geometricNormal, tangent), bitangentRaw) < 0.0 ? -1.0 : 1.0;
	// Match the raster normal-map orientation: the PT geometric frame was
	// effectively interpreting tangent-space Y upside-down on wall relief.
	bitangent *= -handedness;
	return true;
}

float3 SampleMaterialNormalMap(MaterialData material, float2 uv, float3 geometricNormal, float3 tangent, float3 bitangent)
{
	if (material.normalTextureIndex == 0xffffffffu)
	{
		return normalize(geometricNormal);
	}

	float3 map = SampleMaterialColor(material, material.normalTextureIndex, uv, false, false, false).xyz;
	map = map * (255.0 / 127.0) - (128.0 / 127.0);
	map.y = -map.y;

	const float3 mappedNormal = normalize(tangent * map.x + bitangent * map.y + geometricNormal * map.z);
	if (dot(mappedNormal, mappedNormal) <= 1e-8 || dot(mappedNormal, geometricNormal) <= 0.0)
	{
		return normalize(geometricNormal);
	}

	return mappedNormal;
}

float3 UnpackVoxelNormal(uint packedNormal)
{
	const float2 oct = float2(packedNormal & 255u, (packedNormal >> 8u) & 255u) * (2.0f / 255.0f) - 1.0f;
	float3 normal = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
	if (normal.z < 0.0f)
	{
		normal.xy = (1.0f - abs(normal.yx)) * float2(normal.x < 0.0f ? -1.0f : 1.0f, normal.y < 0.0f ? -1.0f : 1.0f);
	}
	return normalize(normal);
}

bool TryResolveSmoothVertexNormal(PrimitiveData primitive, float3 weights, float3 geometricNormal, out float3 smoothNormal)
{
	if ((primitive.smoothNormals.y & 0x80000000u) == 0u)
	{
		smoothNormal = geometricNormal;
		return false;
	}

	const float3 interpolated =
		UnpackVoxelNormal(primitive.smoothNormals.x & 0xffffu) * weights.x +
		UnpackVoxelNormal(primitive.smoothNormals.x >> 16u) * weights.y +
		UnpackVoxelNormal(primitive.smoothNormals.y & 0xffffu) * weights.z;
	const float lengthSq = dot(interpolated, interpolated);
	if (lengthSq <= 1.0e-8f)
	{
		smoothNormal = geometricNormal;
		return false;
	}

	smoothNormal = interpolated * rsqrt(lengthSq);
	return dot(smoothNormal, geometricNormal) > 1.0e-4f;
}

float3 ResolveHitNormal(uint materialIndex, uint dataSource, uint primitiveIndex, PrimitiveData primitive, float2 uv, float3 weights)
{
	const MaterialData material = GetMaterialData(materialIndex, dataSource);
	float3 resolvedNormal = normalize(primitive.normal);
	float3 smoothNormal = resolvedNormal;
	const float blend = (float)((gTraceConstants.Flags >> NRI_VOXEL_NORMAL_BLEND_SHIFT) & 0xffu) * (1.0f / 255.0f);
	if (blend > 0.0f && TryResolveSmoothVertexNormal(primitive, weights, resolvedNormal, smoothNormal))
	{
		resolvedNormal = normalize(lerp(resolvedNormal, smoothNormal, blend));
	}
	if (material.normalTextureIndex == 0xffffffffu)
	{
		return resolvedNormal;
	}

	float3 tangent = 0.0;
	float3 bitangent = 0.0;
	if (!TryResolveHitTangentFrame(dataSource, primitiveIndex, resolvedNormal, tangent, bitangent))
	{
		return resolvedNormal;
	}

	return SampleMaterialNormalMap(material, uv, resolvedNormal, tangent, bitangent);
}

float3 ResolveHitBarycentricWeights(HitData hit)
{
	return float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics.x, hit.barycentrics.y);
}

float3 ResolveHitVertexPosition(HitData hit, bool previous)
{
	const PrimitiveData primitive = GetPrimitiveData(hit.dataSource, hit.primitiveIndex);
	const SceneVertex v0 = GetVertexData(hit.dataSource, primitive.indices.x);
	const SceneVertex v1 = GetVertexData(hit.dataSource, primitive.indices.y);
	const SceneVertex v2 = GetVertexData(hit.dataSource, primitive.indices.z);
	const float3 weights = ResolveHitBarycentricWeights(hit);
	const float3 p0 = previous ? v0.prevPosition : v0.position;
	const float3 p1 = previous ? v1.prevPosition : v1.position;
	const float3 p2 = previous ? v2.prevPosition : v2.position;
	const float3 localPosition = p0 * weights.x + p1 * weights.y + p2 * weights.z;
	if (hit.instanceId == 0xffffffffu)
	{
		return localPosition;
	}
	return TransformSceneInstancePoint(GetSceneInstanceData(hit.instanceId), localPosition, previous);
}

float3 GeneratePrimaryRay(uint2 pixelPos)
{
	float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
	float2 jitter = GetCurrentTemporalJitter();
	float2 uv = ((float2)pixelPos + 0.5 + jitter) / resolution;
	float2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;

	float3 ray =
		gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp;

	return normalize(ray);
}

float2 ProjectWorldToUvRaw(float3 worldPos, float3 cameraPos, float3 cameraForward, float3 cameraRight, float3 cameraUp, float tanHalfFovX, float tanHalfFovY)
{
	float3 relative = worldPos - cameraPos;
	float viewZ = dot(relative, cameraForward);
	if (viewZ <= 0.001)
	{
		return float2(-1.0, -1.0);
	}

	float ndcX = dot(relative, cameraRight) / max(viewZ * tanHalfFovX, 1e-5);
	float ndcY = dot(relative, cameraUp) / max(viewZ * tanHalfFovY, 1e-5);
	return float2(ndcX * 0.5 + 0.5, 0.5 - ndcY * 0.5);
}

float2 ResolveProjectedUvRaw(float4 clip)
{
	const float2 ndc = clip.xy / clip.w;
	return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float4 MultiplyVsMatrixPoint(float4 v, float4 matrixColumns[4])
{
	return float4(
		dot(v, float4(matrixColumns[0].x, matrixColumns[1].x, matrixColumns[2].x, matrixColumns[3].x)),
		dot(v, float4(matrixColumns[0].y, matrixColumns[1].y, matrixColumns[2].y, matrixColumns[3].y)),
		dot(v, float4(matrixColumns[0].z, matrixColumns[1].z, matrixColumns[2].z, matrixColumns[3].z)),
		dot(v, float4(matrixColumns[0].w, matrixColumns[1].w, matrixColumns[2].w, matrixColumns[3].w)));
}

bool ProjectWorldToUvMatrixRaw(float3 worldPos, bool previousFrame, out float2 uv)
{
	const ReprojectionData reprojection = gReprojectionDataBuffer[0];
	const float4 world = float4(worldPos, 1.0);
	const float4 view = previousFrame
		? MultiplyVsMatrixPoint(world, reprojection.previousWorldToViewMatrix)
		: MultiplyVsMatrixPoint(world, reprojection.currentWorldToViewMatrix);
	const float4 clip = previousFrame
		? MultiplyVsMatrixPoint(view, reprojection.previousViewToClipMatrix)
		: MultiplyVsMatrixPoint(view, reprojection.currentViewToClipMatrix);
	if (clip.w <= 1e-5)
	{
		uv = float2(-1.0, -1.0);
		return false;
	}

	uv = ResolveProjectedUvRaw(clip);
	return all(uv >= 0.0) && all(uv <= 1.0);
}

float4 SampleMaterialColor(MaterialData material, uint textureIndex, float2 uv, bool indexed, bool applyPaletteLookup, bool applyLightLevel)
{
	if (textureIndex == 0xffffffffu)
	{
		return 0.0;
	}

	float4 color = 0.0;
	if (indexed || (material.flags & MATERIAL_FLAG_POINT_SAMPLED) != 0)
	{
		color = gSceneTextures[min(textureIndex, MAX_SCENE_TEXTURES - 1)].SampleLevel(gPointWrap, uv, 0.0);
	}
	else
	{
		color = gSceneTextures[min(textureIndex, MAX_SCENE_TEXTURES - 1)].SampleLevel(gLinearWrap, uv, 0.0);
	}

	if (indexed && applyPaletteLookup)
	{
		float paletteValue = saturate(color.r) * 255.0;
		float2 paletteUv = float2((paletteValue + 0.5) / 256.0, ((float)material.paletteIndex + 0.5) / 256.0);
		color = gPaletteLookup.SampleLevel(gPointClamp, paletteUv, 0.0);
	}

	if (applyLightLevel)
	{
		color.rgb *= material.lightLevel;
	}
	return color;
}

float SampleMaterialScalarChannel(MaterialData material, uint textureIndex, float2 uv, float fallback)
{
	if (textureIndex == 0xffffffffu)
	{
		return fallback;
	}

	return SampleMaterialColor(material, textureIndex, uv, false, false, false).r;
}

bool IsPlainMirrorMaterial(MaterialData material)
{
	return (material.flags & MATERIAL_FLAG_PLAIN_MIRROR) != 0u;
}

float4 SampleMaterialBaseColor(uint materialIndex, uint dataSource, float2 uv)
{
	MaterialData material = GetMaterialData(materialIndex, dataSource);
	if (IsPlainMirrorMaterial(material))
	{
		return float4(0.92, 0.92, 0.92, 1.0);
	}

	const bool indexed = (material.flags & MATERIAL_FLAG_INDEXED) != 0;
	return SampleMaterialColor(material, material.textureIndex, uv, indexed, true, true);
}

float4 SampleMaterialBaseColorRaw(uint materialIndex, uint dataSource, float2 uv)
{
	MaterialData material = GetMaterialData(materialIndex, dataSource);
	if (IsPlainMirrorMaterial(material))
	{
		return float4(0.92, 0.92, 0.92, 1.0);
	}

	const bool indexed = (material.flags & MATERIAL_FLAG_INDEXED) != 0;
	return SampleMaterialColor(material, material.textureIndex, uv, indexed, false, false);
}

float3 SampleMaterialEmissionSource(uint materialIndex, uint dataSource, float2 uv)
{
	const MaterialData material = GetMaterialData(materialIndex, dataSource);
	const float3 emissionTint = (material.flags & MATERIAL_FLAG_TINT_EMISSION) != 0u ? material.emissiveColor : 1.0.xxx;
	if (material.emissiveMode == 1u)
	{
		return SampleMaterialColor(material, material.textureIndex, uv, (material.flags & MATERIAL_FLAG_INDEXED) != 0, true, false).rgb * emissionTint;
	}
	if (material.emissiveMode == 2u)
	{
		return material.emissiveColor;
	}
	if (material.emissiveMode == 3u)
	{
		if (material.emissiveTextureIndex == 0xffffffffu)
		{
			return material.emissiveColor;
		}

		const bool emissiveUsesBaseTexture = material.emissiveTextureIndex == material.textureIndex;
		const bool emissiveIndexed = emissiveUsesBaseTexture && (material.flags & MATERIAL_FLAG_INDEXED) != 0;
		return SampleMaterialColor(material, material.emissiveTextureIndex, uv, emissiveIndexed, emissiveIndexed, false).rgb * emissionTint;
	}

	return 0.0;
}

float SampleMaterialMetalness(MaterialData material, float2 uv)
{
	return saturate(SampleMaterialScalarChannel(material, material.metallicTextureIndex, uv, material.metalnessHint));
}

float SampleMaterialRoughness(MaterialData material, float2 uv)
{
	return clamp(SampleMaterialScalarChannel(material, material.roughnessTextureIndex, uv, material.roughnessHint), 0.02, 1.0);
}

float4 SampleSurfaceColor(uint materialIndex, uint dataSource, float2 uv)
{
	return SampleMaterialBaseColor(materialIndex, dataSource, uv);
}

float4 SampleSurfaceColorRaw(uint materialIndex, uint dataSource, float2 uv)
{
	return SampleMaterialBaseColorRaw(materialIndex, dataSource, uv);
}

bool IsTransparentSurfaceSample(uint materialIndex, uint dataSource, float2 uv)
{
	const MaterialData material = GetMaterialData(materialIndex, dataSource);
	const bool indexed = (material.flags & MATERIAL_FLAG_INDEXED) != 0;
	// Ordinary indexed floors/walls are always opaque. Reject that common case
	// before fetching a raw texel that cannot affect the result.
	if (indexed && (material.flags & MATERIAL_FLAG_ALPHA_CLIP) == 0)
	{
		return false;
	}

	const float4 rawSample = SampleMaterialBaseColorRaw(materialIndex, dataSource, uv);
	if (indexed)
	{
		// In the paletted path, only explicitly alpha-clipped carriers treat
		// color index 0 as transparent. Ordinary indexed floors/walls remain opaque.
		const uint paletteIndex = (uint)round(saturate(rawSample.r) * 255.0);
		return paletteIndex == 0u;
	}

	return rawSample.a < 0.5;
}

uint GetPortalTraversalDepth()
{
	return gTraceConstants.PortalDepth & 0xffu;
}

bool ResolvePortalHit(HitData hit, out PortalData portalData)
{
	portalData = GetPortalData(hit.portalIndex);
	return hit.portalIndex != 0xffffffffu && portalData.traversalClass != PORTAL_TRAVERSAL_CLASS_NONE;
}

bool ShouldGatePrimaryVisibleChunks()
{
	return (gTraceConstants.Flags & NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS) != 0u;
}

bool IsVisibleChunk(uint chunkIndex)
{
	if (chunkIndex == 0xffffffffu)
	{
		return true;
	}

	uint visibleChunkWordCount = 0u;
	uint visibleChunkStride = 0u;
	gVisibleChunkWords.GetDimensions(visibleChunkWordCount, visibleChunkStride);
	const uint wordIndex = chunkIndex >> 5u;
	if (wordIndex >= visibleChunkWordCount)
	{
		return false;
	}

	return (gVisibleChunkWords[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
}

bool IsVisibleFlatPlane(uint sectorIndex, bool ceiling)
{
	if (sectorIndex == 0xffffffffu)
	{
		return true;
	}

	uint visibleFlatPlaneWordCount = 0u;
	uint visibleFlatPlaneStride = 0u;
	gVisibleFlatPlaneWords.GetDimensions(visibleFlatPlaneWordCount, visibleFlatPlaneStride);
	const uint flatPlaneIndex = sectorIndex * 2u + (ceiling ? 1u : 0u);
	const uint wordIndex = flatPlaneIndex >> 5u;
	if (wordIndex >= visibleFlatPlaneWordCount)
	{
		return false;
	}

	return (gVisibleFlatPlaneWords[wordIndex] & (1u << (flatPlaneIndex & 31u))) != 0u;
}

bool ShouldRejectHiddenStaticFlat(uint materialIndex, uint dataSource, PrimitiveData primitive)
{
	if (dataSource != SCENE_DATA_SOURCE_STATIC)
	{
		return false;
	}

	const MaterialData material = GetMaterialData(materialIndex, dataSource);
	const uint flags = material.flags;
	if ((flags & MATERIAL_FLAG_FLAT) == 0 ||
		(flags & (MATERIAL_FLAG_SPRITE | MATERIAL_FLAG_MIRROR | MATERIAL_FLAG_SKY | MATERIAL_FLAG_PORTAL)) != 0)
	{
		return false;
	}

	return !IsVisibleFlatPlane(material.sectorIndex, primitive.normal.y < 0.0);
}

bool ShouldIgnoreOneWayHit(uint materialIndex, uint dataSource, float3 geometricNormal, float3 rayDirection)
{
	if ((GetMaterialData(materialIndex, dataSource).flags & MATERIAL_FLAG_ONE_WAY) == 0)
	{
		return false;
	}

	return dot(normalize(geometricNormal), rayDirection) > 0.0;
}

bool IsReflectionOnlyPrimitive(PrimitiveData primitive)
{
	return (primitive.flags & PRIMITIVE_FLAG_REFLECTION_ONLY) != 0u;
}

uint GetSceneRecordPrimitiveBase(SceneInstanceData instanceData)
{
	return instanceData.primitiveBase;
}

uint ResolveSceneRecordPrimitiveIndex(SceneInstanceData instanceData, uint localPrimitiveIndex)
{
	return GetSceneRecordPrimitiveBase(instanceData) + localPrimitiveIndex;
}

uint GetSceneRecordMaterialBase(SceneInstanceData instanceData)
{
	return instanceData.materialBase;
}

uint GetSceneRecordMaterialCount(SceneInstanceData instanceData)
{
	return instanceData.materialCount;
}

uint ResolvePrimitiveIndex(SceneInstanceData instanceData, uint localPrimitiveIndex)
{
	return ResolveSceneRecordPrimitiveIndex(instanceData, localPrimitiveIndex);
}

uint ResolvePrimitiveMaterialIndex(SceneInstanceData instanceData, PrimitiveData primitive)
{
	uint localMaterialIndex = primitive.materialIndex;
	const uint materialCount = GetSceneRecordMaterialCount(instanceData);
	if (materialCount != 0xffffffffu && materialCount > 0u)
	{
		localMaterialIndex = min(localMaterialIndex, materialCount - 1u);
	}
	return GetSceneRecordMaterialBase(instanceData) + localMaterialIndex;
}

uint ResolveVisibilityChunk(SceneInstanceData instanceData, PrimitiveData primitive)
{
	if (instanceData.dataSource == SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
	{
		return instanceData.visibilityChunk;
	}

	return instanceData.visibilityChunk != 0xffffffffu ? instanceData.visibilityChunk : primitive.reserved0;
}

bool IntersectPrimitiveTriangle(float3 origin, float3 direction, uint primitiveIndex, out float hitT, out float3 barycentrics)
{
	hitT = 0.0;
	barycentrics = 0.0;
	const PrimitiveData primitive = GetPrimitiveData(SCENE_DATA_SOURCE_DYNAMIC, primitiveIndex);
	const SceneVertex v0 = gDynamicVertices[primitive.indices.x];
	const SceneVertex v1 = gDynamicVertices[primitive.indices.y];
	const SceneVertex v2 = gDynamicVertices[primitive.indices.z];
	const float3 edge1 = v1.position - v0.position;
	const float3 edge2 = v2.position - v0.position;
	const float3 p = cross(direction, edge2);
	const float det = dot(edge1, p);
	if (abs(det) < 1e-5)
	{
		return false;
	}

	const float invDet = 1.0 / det;
	const float3 t = origin - v0.position;
	const float u = dot(t, p) * invDet;
	if (u < 0.0 || u > 1.0)
	{
		return false;
	}

	const float3 q = cross(t, edge1);
	const float v = dot(direction, q) * invDet;
	if (v < 0.0 || (u + v) > 1.0)
	{
		return false;
	}

	const float candidateT = dot(edge2, q) * invDet;
	if (candidateT <= TRACE_MIN_DISTANCE)
	{
		return false;
	}

	hitT = candidateT;
	barycentrics = float3(1.0 - u - v, u, v);
	return true;
}

HitData TraceBootstrapGeometry(float3 origin, float3 direction, bool allowReflectionOnlySurfaces)
{
	HitData bestHit = MakeEmptyHitData();
	bestHit.distance = 1e30;

	[loop]
	for (uint primitiveIndex = 0; primitiveIndex < gTraceConstants.DynamicPrimitiveCount; ++primitiveIndex)
	{
		float hitT = 0.0;
		float3 barycentrics = 0.0;
		if (!IntersectPrimitiveTriangle(origin, direction, primitiveIndex, hitT, barycentrics))
		{
			continue;
		}

		if (hitT >= bestHit.distance)
		{
			continue;
		}

		const PrimitiveData primitive = GetPrimitiveData(SCENE_DATA_SOURCE_DYNAMIC, primitiveIndex);
		if (!allowReflectionOnlySurfaces && IsReflectionOnlyPrimitive(primitive))
		{
			continue;
		}

		if (ShouldIgnoreOneWayHit(primitive.materialIndex, SCENE_DATA_SOURCE_DYNAMIC, primitive.normal, direction))
		{
			continue;
		}

		const float2 uv = primitive.uv0 * barycentrics.x + primitive.uv1 * barycentrics.y + primitive.uv2 * barycentrics.z;
		if (IsTransparentSurfaceSample(primitive.materialIndex, SCENE_DATA_SOURCE_DYNAMIC, uv))
		{
			continue;
		}

		bestHit.hit = true;
		bestHit.dataSource = SCENE_DATA_SOURCE_DYNAMIC;
		bestHit.primitiveIndex = primitiveIndex;
		bestHit.portalIndex = primitive.portalIndex;
		bestHit.barycentrics = barycentrics.yz;
		bestHit.distance = hitT;
		bestHit.position = origin + direction * hitT;
		bestHit.uv = uv;
		bestHit.normal = ResolveHitNormal(primitive.materialIndex, SCENE_DATA_SOURCE_DYNAMIC, primitiveIndex, primitive, uv, barycentrics);
		bestHit.materialIndex = primitive.materialIndex;
	}

	return bestHit;
}

HitData TraceBootstrapGeometry(float3 origin, float3 direction)
{
	return TraceBootstrapGeometry(origin, direction, false);
}

bool TraceClosestSurface(float3 startOrigin, float3 direction, float maxDistance, uint traceMask, bool gateVisibleChunks, bool ignoreNoShadowCast, bool allowReflectionOnlySurfaces, uint statsKind, out HitData hitData)
{
	hitData = MakeEmptyHitData();
	float accumulatedDistance = 0.0;
	TraceShaderStatCall(statsKind);

	[loop]
	// Runtime replacement overlays can stack many filtered hits in front of the
	// eventual visible surface. Keep enough budget to walk past hidden chunks,
	// alpha-clipped carriers, and one-way rejects without dropping the ray.
	//
	// Keep the original ray origin across filtered restarts so we only pay one
	// tiny epsilon in TMin instead of shifting the origin forward and then
	// applying another minimum distance. That preserves nearly coplanar follow-up
	// hits after rejecting a frontmost back-side or gate-invisible surface.
	for (uint skipCount = 0u; skipCount < TRACE_FILTER_SKIP_LIMIT; ++skipCount)
	{
		const float traceMinDistance = accumulatedDistance > 0.0 ? (accumulatedDistance + TRACE_FILTER_CONTINUE_BIAS) : TRACE_MIN_DISTANCE;
		if (traceMinDistance >= maxDistance)
		{
			return false;
		}

		RayQuery<RAY_FLAG_FORCE_OPAQUE> rayQuery;
		RayDesc ray = { startOrigin, traceMinDistance, direction, maxDistance };
		rayQuery.TraceRayInline(gWorldTlas, RAY_FLAG_FORCE_OPAQUE, traceMask, ray);

		while (rayQuery.Proceed()) {}

		if (rayQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
		{
			TraceShaderStatAdd(TRACE_STAT_MISS, 1u);
			return false;
		}
		TraceShaderStatAdd(TRACE_STAT_COMMITTED, 1u);

		const uint committedInstanceId = rayQuery.CommittedInstanceID();
		TraceShaderStatInstance(TRACE_STAT_INSTANCE_COMMITTED_BASE, TRACE_STAT_INSTANCE_COMMITTED_OVERFLOW, committedInstanceId);
		TraceShaderStatInstanceKind(statsKind, committedInstanceId);
		const SceneInstanceData instanceData = GetSceneInstanceData(committedInstanceId);
		const uint primitiveIndex = ResolvePrimitiveIndex(instanceData, rayQuery.CommittedPrimitiveIndex());
		const PrimitiveData primitive = GetPrimitiveData(instanceData.dataSource, primitiveIndex);
		const uint materialIndex = ResolvePrimitiveMaterialIndex(instanceData, primitive);
		const float committedDistance = rayQuery.CommittedRayT();
		const bool reflectionOnlyPrimitive = IsReflectionOnlyPrimitive(primitive);
		if (!allowReflectionOnlySurfaces && reflectionOnlyPrimitive)
		{
			TraceShaderStatAdd(TRACE_STAT_FILTER_SKIPS, 1u);
			TraceShaderStatMax(TRACE_STAT_MAX_SKIP, skipCount + 1u);
			TraceShaderStatAdd(TRACE_STAT_REJECT_REFLECTION, 1u);
			TraceShaderStatSource(TRACE_STAT_REJECT_STATIC, TRACE_STAT_REJECT_DYNAMIC, TRACE_STAT_REJECT_VOXEL, instanceData.dataSource);
			accumulatedDistance = committedDistance;
			continue;
		}

		if (gateVisibleChunks && !reflectionOnlyPrimitive && !IsVisibleChunk(ResolveVisibilityChunk(instanceData, primitive)))
		{
			TraceShaderStatAdd(TRACE_STAT_FILTER_SKIPS, 1u);
			TraceShaderStatMax(TRACE_STAT_MAX_SKIP, skipCount + 1u);
			TraceShaderStatAdd(TRACE_STAT_REJECT_VISIBLE, 1u);
			TraceShaderStatSource(TRACE_STAT_REJECT_STATIC, TRACE_STAT_REJECT_DYNAMIC, TRACE_STAT_REJECT_VOXEL, instanceData.dataSource);
			accumulatedDistance = committedDistance;
			continue;
		}

		if (gateVisibleChunks && ShouldRejectHiddenStaticFlat(materialIndex, instanceData.dataSource, primitive))
		{
			TraceShaderStatAdd(TRACE_STAT_FILTER_SKIPS, 1u);
			TraceShaderStatMax(TRACE_STAT_MAX_SKIP, skipCount + 1u);
			TraceShaderStatAdd(TRACE_STAT_REJECT_HIDDEN_FLAT, 1u);
			TraceShaderStatSource(TRACE_STAT_REJECT_STATIC, TRACE_STAT_REJECT_DYNAMIC, TRACE_STAT_REJECT_VOXEL, instanceData.dataSource);
			accumulatedDistance = committedDistance;
			continue;
		}

		const float3 geometricNormal = TransformSceneInstanceNormal(instanceData, primitive.normal, false);
		if (ShouldIgnoreOneWayHit(materialIndex, instanceData.dataSource, geometricNormal, direction))
		{
			TraceShaderStatAdd(TRACE_STAT_FILTER_SKIPS, 1u);
			TraceShaderStatMax(TRACE_STAT_MAX_SKIP, skipCount + 1u);
			TraceShaderStatAdd(TRACE_STAT_REJECT_ONEWAY, 1u);
			TraceShaderStatSource(TRACE_STAT_REJECT_STATIC, TRACE_STAT_REJECT_DYNAMIC, TRACE_STAT_REJECT_VOXEL, instanceData.dataSource);
			accumulatedDistance = committedDistance;
			continue;
		}

		// Shadow visibility only needs to know that this committed material does
		// not block light. Reject it before barycentric/UV and alpha-sample work.
		if (ignoreNoShadowCast && !MaterialCastsShadow(GetMaterialData(materialIndex, instanceData.dataSource)))
		{
			TraceShaderStatAdd(TRACE_STAT_FILTER_SKIPS, 1u);
			TraceShaderStatMax(TRACE_STAT_MAX_SKIP, skipCount + 1u);
			TraceShaderStatAdd(TRACE_STAT_REJECT_NO_SHADOW, 1u);
			TraceShaderStatSource(TRACE_STAT_REJECT_STATIC, TRACE_STAT_REJECT_DYNAMIC, TRACE_STAT_REJECT_VOXEL, instanceData.dataSource);
			accumulatedDistance = committedDistance;
			continue;
		}

		const float2 bary = rayQuery.CommittedTriangleBarycentrics();
		const float3 weights = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
		const float2 uv = primitive.uv0 * weights.x + primitive.uv1 * weights.y + primitive.uv2 * weights.z;
		if (IsTransparentSurfaceSample(materialIndex, instanceData.dataSource, uv))
		{
			TraceShaderStatAdd(TRACE_STAT_FILTER_SKIPS, 1u);
			TraceShaderStatMax(TRACE_STAT_MAX_SKIP, skipCount + 1u);
			TraceShaderStatAdd(TRACE_STAT_REJECT_TRANSPARENT, 1u);
			TraceShaderStatSource(TRACE_STAT_REJECT_STATIC, TRACE_STAT_REJECT_DYNAMIC, TRACE_STAT_REJECT_VOXEL, instanceData.dataSource);
			accumulatedDistance = committedDistance;
			continue;
		}

		const float hitDistance = committedDistance;
		hitData.hit = true;
		hitData.dataSource = instanceData.dataSource;
		hitData.primitiveIndex = primitiveIndex;
		hitData.portalIndex = primitive.portalIndex;
		hitData.instanceId = committedInstanceId;
		hitData.barycentrics = bary;
		hitData.distance = hitDistance;
		hitData.position = startOrigin + direction * hitDistance;
		hitData.uv = uv;
		hitData.normal = TransformSceneInstanceNormal(instanceData, ResolveHitNormal(materialIndex, instanceData.dataSource, primitiveIndex, primitive, uv, weights), false);
		hitData.materialIndex = materialIndex;
		TraceShaderStatSource(TRACE_STAT_ACCEPT_STATIC, TRACE_STAT_ACCEPT_DYNAMIC, TRACE_STAT_ACCEPT_VOXEL, instanceData.dataSource);
		TraceShaderStatInstance(TRACE_STAT_INSTANCE_ACCEPTED_BASE, TRACE_STAT_INSTANCE_ACCEPTED_OVERFLOW, committedInstanceId);
		return true;
	}

	TraceShaderStatAdd(TRACE_STAT_SKIP_LIMIT, 1u);
	return false;
}

bool TraceScenePath(float3 startOrigin, float3 startDirection, float maxDistance, uint traceMask, uint mirrorBudget, uint portalBudget, bool gateVisibleChunks, bool ignoreNoShadowCast, bool allowReflectionOnlySurfaces, uint statsKind, out HitData hitData, out float3 exitDirection)
{
	hitData = MakeEmptyHitData();
	exitDirection = startDirection;
	float3 origin = startOrigin;
	float3 direction = startDirection;
	float remainingDistance = maxDistance;
#if defined(NRI_INDIRECT_RADIANCE_CACHE)
	uint pathFlags = 0u;
#endif

	[loop]
	for (uint continuationStep = 0u; continuationStep < 32u; ++continuationStep)
	{
		if (!TraceClosestSurface(origin, direction, remainingDistance, traceMask, gateVisibleChunks, ignoreNoShadowCast, allowReflectionOnlySurfaces, statsKind, hitData))
		{
			exitDirection = direction;
			return false;
		}

		PortalData portalData = (PortalData)0;
		const bool hasPortalData = ResolvePortalHit(hitData, portalData);
		const bool reflectivePortal = hasPortalData && portalData.traversalClass == PORTAL_TRAVERSAL_CLASS_REFLECTIVE;
		const bool transferPortal = hasPortalData && portalData.traversalClass == PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER;

		if (reflectivePortal && mirrorBudget > 0u)
		{
#if defined(NRI_INDIRECT_RADIANCE_CACHE)
			pathFlags |= HIT_PATH_FLAG_REFLECTION;
#endif
			remainingDistance = max(remainingDistance - hitData.distance, 0.0);
			origin = hitData.position + hitData.normal * 0.05;
			direction = reflect(direction, hitData.normal);
			exitDirection = direction;
			allowReflectionOnlySurfaces = true;
			traceMask = NRI_TLAS_MASK_REFLECTION;
			gateVisibleChunks = false;
			mirrorBudget--;
			continue;
		}

		if (transferPortal && portalBudget > 0u)
		{
#if defined(NRI_INDIRECT_RADIANCE_CACHE)
			pathFlags |= HIT_PATH_FLAG_SPACE_TRANSFER;
#endif
			remainingDistance = max(remainingDistance - hitData.distance, 0.0);
			origin = hitData.position + direction * 0.05 + portalData.delta;
			exitDirection = direction;
			gateVisibleChunks = false;
			portalBudget--;
			continue;
		}

#if defined(NRI_INDIRECT_RADIANCE_CACHE)
		hitData.pathFlags = pathFlags;
#endif
		exitDirection = direction;
		return true;
	}

	exitDirection = direction;
	return false;
}

bool TraceScenePath(float3 startOrigin, float3 startDirection, float maxDistance, uint mirrorBudget, uint portalBudget, out HitData hitData, out float3 exitDirection)
{
	return TraceScenePath(startOrigin, startDirection, maxDistance, NRI_TLAS_MASK_MAIN, mirrorBudget, portalBudget, false, false, false, TRACE_STATS_KIND_UNGATED, hitData, exitDirection);
}

HitData TracePrimary(float3 origin, float3 direction, bool gateVisibleChunks, out float3 exitDirection)
{
	HitData hitData = MakeEmptyHitData();
	const uint mirrorBudget = max(1u, (gTraceConstants.BounceCounts >> 4u) & 0xfu);
	TraceScenePath(origin, direction, 100000.0, NRI_TLAS_MASK_MAIN, mirrorBudget, GetPortalTraversalDepth(), gateVisibleChunks, false, false, TRACE_STATS_KIND_PRIMARY, hitData, exitDirection);
	return hitData;
}

HitData TracePrimary(float3 origin, float3 direction, out float3 exitDirection)
{
	return TracePrimary(origin, direction, ShouldGatePrimaryVisibleChunks(), exitDirection);
}

HitData TracePrimaryUngated(float3 origin, float3 direction, out float3 exitDirection)
{
	HitData hitData = MakeEmptyHitData();
	const uint mirrorBudget = max(1u, (gTraceConstants.BounceCounts >> 4u) & 0xfu);
	TraceScenePath(origin, direction, 100000.0, NRI_TLAS_MASK_REFLECTION, mirrorBudget, GetPortalTraversalDepth(), false, false, true, TRACE_STATS_KIND_UNGATED, hitData, exitDirection);
	return hitData;
}

HitData TraceIndirectUngated(float3 origin, float3 direction, out float3 exitDirection)
{
	HitData hitData = MakeEmptyHitData();
	const uint mirrorBudget = max(1u, (gTraceConstants.BounceCounts >> 4u) & 0xfu);
	TraceScenePath(origin, direction, 100000.0, NRI_TLAS_MASK_GI, mirrorBudget, GetPortalTraversalDepth(), false, false, false, TRACE_STATS_KIND_UNGATED, hitData, exitDirection);
	return hitData;
}

HitData TraceReflectionUngated(float3 origin, float3 direction, out float3 exitDirection)
{
	HitData hitData = MakeEmptyHitData();
	const uint mirrorBudget = max(1u, (gTraceConstants.BounceCounts >> 4u) & 0xfu);
	TraceScenePath(origin, direction, 100000.0, NRI_TLAS_MASK_REFLECTION, mirrorBudget, GetPortalTraversalDepth(), false, false, true, TRACE_STATS_KIND_UNGATED, hitData, exitDirection);
	return hitData;
}

HitData TracePrimary(float3 origin, float3 direction)
{
	float3 exitDirection = direction;
	return TracePrimary(origin, direction, exitDirection);
}

float ComputeSunShadow(float3 position, float3 normal, float3 lightDirection, out float shadowHitDistance)
{
	TraceShaderStatAdd(TRACE_STAT_SUN_SHADOW_CALLS, 1u);
	HitData shadowHit = MakeEmptyHitData();
	float3 ignoredDirection = lightDirection;
	const bool blocked = TraceScenePath(position + normal * 0.05, lightDirection, 100000.0, NRI_TLAS_MASK_SHADOW, 0u, GetPortalTraversalDepth(), false, true, false, TRACE_STATS_KIND_SUN, shadowHit, ignoredDirection);
	shadowHitDistance = blocked ? shadowHit.distance : NRD_FP16_MAX;
	return blocked ? 0.0 : 1.0;
}

float ComputeSunShadow(float3 position, float3 normal, float3 lightDirection)
{
	float shadowHitDistance = 0.0;
	return ComputeSunShadow(position, normal, lightDirection, shadowHitDistance);
}

float ComputePointLightShadowTagged(float3 position, float3 normal, float3 lightDirection, float lightDistance, uint statsKind)
{
	if (lightDistance <= 0.051)
	{
		return 1.0;
	}

	TraceShaderStatAdd(TRACE_STAT_POINT_SHADOW_CALLS, 1u);
	HitData shadowHit = MakeEmptyHitData();
	float3 ignoredDirection = lightDirection;
	const float maxDistance = max(lightDistance - 0.05, 0.001);
	const bool blocked = TraceScenePath(position + normal * 0.05, lightDirection, maxDistance, NRI_TLAS_MASK_SHADOW, 0u, GetPortalTraversalDepth(), false, true, false, statsKind, shadowHit, ignoredDirection);
	return blocked ? 0.0 : 1.0;
}

float ComputePointLightShadow(float3 position, float3 normal, float3 lightDirection, float lightDistance)
{
	return ComputePointLightShadowTagged(position, normal, lightDirection, lightDistance, TRACE_STATS_KIND_POINT);
}

bool TraceClosestSurface(float3 startOrigin, float3 direction, float maxDistance, out HitData hitData)
{
	return TraceClosestSurface(startOrigin, direction, maxDistance, NRI_TLAS_MASK_MAIN, false, false, false, TRACE_STATS_KIND_UNGATED, hitData);
}

float ComputeFastPointLightShadow(float3 position, float3 normal, float3 lightDirection, float lightDistance)
{
	if (lightDistance <= 0.051)
	{
		return 1.0;
	}

	TraceShaderStatAdd(TRACE_STAT_FAST_EMISSIVE_SHADOW_CALLS, 1u);
	HitData shadowHit = MakeEmptyHitData();
	const float maxDistance = max(lightDistance - 0.05, 0.001);
	const bool blocked = TraceClosestSurface(position + normal * 0.05, lightDirection, maxDistance, NRI_TLAS_MASK_SHADOW, false, true, false, TRACE_STATS_KIND_FAST_EMISSIVE, shadowHit);
	return blocked ? 0.0 : 1.0;
}

float3 GetMissColor(float3 direction)
{
	return gSkyTexture.SampleLevel(gLinearClamp, normalize(direction), 0.0).rgb;
}

#endif
