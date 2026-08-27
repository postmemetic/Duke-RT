#define NRI_ENABLE_PERSISTENT_VOXEL_SCENE 1
#include "Include/Shared.hlsli"
#include "Include/RaytracingShared.hlsli"
#include "Include/AnalyticLightSampling.hlsli"
#include "Include/DirectionalLightSampling.hlsli"
#if defined(NRI_INDIRECT_RADIANCE_CACHE)
#include "Include/IndirectRadianceCacheTrace.hlsli"
#endif

uint Hash32(uint value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

float RandomFloat01(inout uint state)
{
	state = Hash32(state);
	return (float)(state & 0x00ffffffu) * (1.0 / 16777216.0);
}

float3 BuildOrthonormalTangent(float3 n)
{
	const float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
	return normalize(cross(up, n));
}

float3 SampleSunDirection(float3 lightDir, uint2 pixelPos, uint frameIndex)
{
	// SIGMA expects a high-frequency spatial distribution and only a short temporal
	// sequence. Match NRD's reference shadow sampler: a 128x128 ranked Owen-scrambled
	// Sobol tile with four animated samples.
	const uint phase = frameIndex & 3u;
	const uint4 scramblingRanking = gBlueNoiseScramblingRanking[pixelPos & 127u];
	const uint rankedSampleIndex = phase ^ scramblingRanking.z;
	const uint4 sobol = gBlueNoiseSobol[uint2(rankedSampleIndex, 0u)];
	const float2 uniformSample = (float2(sobol.xy ^ scramblingRanking.xy) + 0.5) * (1.0 / 256.0);
	return SampleUniformDirectionalCone(
		lightDir,
		GetDirectionalPlaceholderAngularSize(),
		uniformSample);
}

float3 SampleCosineHemisphere(float3 normal, inout uint rngState)
{
	const float u1 = RandomFloat01(rngState);
	const float u2 = RandomFloat01(rngState);
	const float r = sqrt(u1);
	const float phi = 6.28318530718 * u2;
	const float x = r * cos(phi);
	const float y = r * sin(phi);
	const float z = sqrt(saturate(1.0 - u1));
	const float3 tangent = BuildOrthonormalTangent(normal);
	const float3 bitangent = normalize(cross(normal, tangent));
	return normalize(tangent * x + bitangent * y + normal * z);
}

float2 SampleUniformDisk(inout uint rngState)
{
	const float radius = sqrt(RandomFloat01(rngState));
	const float phi = 6.28318530718 * RandomFloat01(rngState);
	return float2(cos(phi), sin(phi)) * radius;
}

float3 SampleRuntimePointEmitter(RuntimePointLightData light, float3 centerDir, inout uint rngState)
{
	const float emitterRadius = max(light.emitterRadius, 0.0);
	return SampleAnalyticReceiverFacingDisk(light.position, emitterRadius, centerDir, SampleUniformDisk(rngState));
}

float3 SampleSpecularLobe(float3 reflectionDir, float roughness, inout uint rngState)
{
	if (roughness <= 0.02)
	{
		return reflectionDir;
	}

	const float3 blurred = SampleCosineHemisphere(reflectionDir, rngState);
	const float blurAmount = saturate(roughness * roughness * 1.5);
	return normalize(lerp(reflectionDir, blurred, blurAmount));
}

bool IsFacingBillboardMaterial(MaterialData material)
{
	return (material.flags & MATERIAL_FLAG_FACING_BILLBOARD) != 0;
}

float3 ResolveViewFacingShadingNormal(MaterialData material, float3 geometricNormal, float3 viewDir)
{
	float3 resolvedNormal = normalize(geometricNormal);
	if (!IsFacingBillboardMaterial(material))
	{
		return resolvedNormal;
	}

	return dot(resolvedNormal, viewDir) >= 0.0 ? resolvedNormal : -resolvedNormal;
}

float3 ResolveLightFacingShadingNormal(MaterialData material, float3 baseNormal, float3 lightDir)
{
	float3 resolvedNormal = normalize(baseNormal);
	if (!IsFacingBillboardMaterial(material))
	{
		return resolvedNormal;
	}

	return dot(resolvedNormal, lightDir) >= 0.0 ? resolvedNormal : -resolvedNormal;
}

static const float kSkyVirtualMotionDistance = 65536.0;

void ComputeSkyVirtualMotion(float3 rayOrigin, float3 rayDirection, out bool currentUvValid, out bool prevUvValid, out float2 currentUvRaw, out float2 prevUvRaw, out float2 motionPixels, out float currentViewZ, out float previousViewZ)
{
	const float3 virtualPosition = rayOrigin + rayDirection * kSkyVirtualMotionDistance;
	currentUvValid = ProjectWorldToUvMatrixRaw(virtualPosition, false, currentUvRaw);
	prevUvValid = ProjectWorldToUvMatrixRaw(virtualPosition, true, prevUvRaw);
	motionPixels = 0.0;
	currentViewZ = dot(virtualPosition - gTraceConstants.CameraPos, gTraceConstants.CameraForward);
	previousViewZ = dot(virtualPosition - gTraceConstants.PrevCameraPos, gTraceConstants.PrevCameraForward);

	if (currentUvValid && prevUvValid)
	{
		motionPixels = (prevUvRaw - currentUvRaw) * float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
	}
}

float4 VisualizeMotionVector(float4 motionSample, float viewZ)
{
	const bool isSky = abs(viewZ) >= NRD_INF * 0.5;
	const bool valid = motionSample.w > 0.0 || isSky;
	if (!valid)
	{
		return float4(1.0, 0.0, 1.0, 1.0);
	}

	const float2 motionPixels = motionSample.xy;
	const float2 signedMagnitude = sign(motionPixels) * sqrt(saturate(abs(motionPixels) / 8.0));
	const float magnitude = saturate(log2(1.0 + length(motionPixels)) / 3.0);
	float3 color = float3(signedMagnitude * 0.5 + 0.5, magnitude);
	if (isSky)
	{
		color = lerp(color, float3(0.35, 0.45, 0.75), 0.18);
	}

	return float4(saturate(color), 1.0);
}

float4 VisualizeMotionDepth(float4 motionSample)
{
	if (motionSample.w <= 0.0)
	{
		return float4(1.0, 0.0, 1.0, 1.0);
	}
	const float magnitude = saturate(log2(1.0 + abs(motionSample.z)) / 5.0);
	return motionSample.z >= 0.0 ? float4(magnitude, 0.15 * magnitude, 0.0, 1.0) : float4(0.0, 0.15 * magnitude, magnitude, 1.0);
}

float2 EncodeTemporalNormal(float3 normal)
{
	float3 oct = normalize(normal);
	oct /= max(abs(oct.x) + abs(oct.y) + abs(oct.z), 1e-6);
	if (oct.z < 0.0)
	{
		oct.xy = (1.0 - abs(oct.yx)) * float2(oct.x < 0.0 ? -1.0 : 1.0, oct.y < 0.0 ? -1.0 : 1.0);
	}
	return oct.xy;
}

float3 DecodeTemporalNormal(float2 encoded)
{
	float3 normal = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
	if (normal.z < 0.0)
	{
		normal.xy = (1.0 - abs(normal.yx)) * float2(normal.x < 0.0 ? -1.0 : 1.0, normal.y < 0.0 ? -1.0 : 1.0);
	}
	return normalize(normal);
}

uint4 GetPrimaryTemporalIdentity(HitData hit)
{
	const PrimitiveData primitive = GetPrimitiveData(hit.dataSource, hit.primitiveIndex);
	return uint4(primitive.temporalSurfaceId, primitive.temporalGeneration, primitive.temporalFlags);
}

bool IsTemporalHistoryReset()
{
	return (gTraceConstants.Flags & NRI_FLAG_RESET_HISTORY) != 0u;
}

uint EvaluateTemporalCorrespondence(
	uint2 pixelPos,
	uint4 currentIdentity,
	float3 motion,
	float previousViewZ,
	float3 predictedPreviousNormal,
	bool currentProjectionValid,
	uint currentProjectionReason,
	bool previousProjectionValid,
	uint previousProjectionReason)
{
	if (IsTemporalHistoryReset())
	{
		return MOTION_VALIDITY_GLOBAL_RESET;
	}
	if (!currentProjectionValid)
	{
		return currentProjectionReason;
	}
	if (!previousProjectionValid)
	{
		return previousProjectionReason;
	}
	if ((currentIdentity.w & TEMPORAL_SURFACE_FLAG_IDENTITY_VALID) == 0u)
	{
		return (currentIdentity.w & TEMPORAL_SURFACE_FLAG_LEGACY_FALLBACK) != 0u ?
			MOTION_VALIDITY_VALID : MOTION_VALIDITY_UNSUPPORTED;
	}
	if ((currentIdentity.w & TEMPORAL_SURFACE_FLAG_CORRESPONDENCE_VALID) == 0u)
	{
		return (currentIdentity.w >> TEMPORAL_SURFACE_REASON_SHIFT) & 0xffu;
	}

	const float2 previousPixel = float2(pixelPos) + motion.xy + GetCurrentTemporalJitter() - GetPreviousTemporalJitter();
	const int2 previousPixelIndex = int2(floor(previousPixel + 0.5));
	if (any(previousPixelIndex < 0) || previousPixelIndex.x >= (int)gTraceConstants.RenderWidth || previousPixelIndex.y >= (int)gTraceConstants.RenderHeight)
	{
		return MOTION_VALIDITY_OUTSIDE_VIEWPORT;
	}

	const uint4 previousIdentity = gTemporalSurfaceIdInput.Load(int3(previousPixelIndex, 0));
	if (any(previousIdentity.xyz != currentIdentity.xyz))
	{
		return MOTION_VALIDITY_ID_MISMATCH;
	}
	const float4 previousGuide = gTemporalGuideInput.Load(int3(previousPixelIndex, 0));
	const float depthTolerance = max(0.05, abs(previousViewZ) * 0.02);
	if (!isfinite(previousGuide.x) || abs(previousGuide.x - previousViewZ) > depthTolerance)
	{
		return MOTION_VALIDITY_DEPTH_MISMATCH;
	}
	const float3 previousNormal = DecodeTemporalNormal(previousGuide.yz);
	if (dot(previousNormal, predictedPreviousNormal) < 0.85)
	{
		return MOTION_VALIDITY_NORMAL_MISMATCH;
	}
	return MOTION_VALIDITY_VALID;
}

uint EvaluateSkyTemporalCorrespondence(uint2 pixelPos, float2 motionPixels, bool projectionValid)
{
	if (IsTemporalHistoryReset())
	{
		return MOTION_VALIDITY_GLOBAL_RESET;
	}
	if (!projectionValid)
	{
		return MOTION_VALIDITY_PREVIOUS_BEHIND;
	}
	const float2 previousPixel = float2(pixelPos) + motionPixels + GetCurrentTemporalJitter() - GetPreviousTemporalJitter();
	const int2 previousPixelIndex = int2(floor(previousPixel + 0.5));
	if (any(previousPixelIndex < 0) || previousPixelIndex.x >= (int)gTraceConstants.RenderWidth || previousPixelIndex.y >= (int)gTraceConstants.RenderHeight)
	{
		return MOTION_VALIDITY_OUTSIDE_VIEWPORT;
	}
	const uint4 previousIdentity = gTemporalSurfaceIdInput.Load(int3(previousPixelIndex, 0));
	return all(previousIdentity.xyz == uint3(0xffffffffu, 0xffffffffu, 1u)) ?
		MOTION_VALIDITY_VALID : MOTION_VALIDITY_ID_MISMATCH;
}

float4 VisualizeMotionValidity(uint reason)
{
	if (reason == MOTION_VALIDITY_VALID) return float4(0.0, 1.0, 0.0, 1.0);
	if (reason == MOTION_VALIDITY_ID_MISMATCH) return float4(1.0, 0.0, 0.0, 1.0);
	if (reason == MOTION_VALIDITY_DEPTH_MISMATCH) return float4(1.0, 0.55, 0.0, 1.0);
	if (reason == MOTION_VALIDITY_NORMAL_MISMATCH) return float4(0.1, 0.35, 1.0, 1.0);
	if (reason == MOTION_VALIDITY_OUTSIDE_VIEWPORT) return float4(0.0, 0.8, 1.0, 1.0);
	if (reason == MOTION_VALIDITY_GLOBAL_RESET || reason == MOTION_VALIDITY_NO_HISTORY) return float4(0.65, 0.0, 0.9, 1.0);
	return float4(1.0, 0.0, 1.0, 1.0);
}

void WriteTemporalOutputs(uint2 pixelPos, uint4 identity, float viewZ, float3 normal, uint validityReason)
{
	const bool historyValid = validityReason == MOTION_VALIDITY_VALID;
	const float2 encodedNormal = EncodeTemporalNormal(normal);
	gTemporalSurfaceIdOutput[pixelPos] = identity;
	gTemporalGuideOutput[pixelPos] = float4(viewZ, encodedNormal, 1.0);
	gTemporalValidityOutput[pixelPos] = float4(historyValid ? 1.0 : 0.0, (float)validityReason, 0.0, 1.0);
	const bool forceReactive = (gTraceConstants.Flags & NRI_FLAG_MOTION_ALL_REACTIVE) != 0u;
	gTemporalReactiveOutput[pixelPos] = float4(forceReactive || !historyValid ? 1.0 : 0.0, 0.0, 0.0, 1.0);
}

uint ClassifyMotionSource(HitData hit, uint4 identity, uint validityReason)
{
	if (validityReason == MOTION_VALIDITY_NO_HISTORY || validityReason == MOTION_VALIDITY_TOPOLOGY_MISMATCH ||
		validityReason == MOTION_VALIDITY_REAPPEARED || validityReason == MOTION_VALIDITY_DUPLICATE_PUBLICATION)
	{
		return 3u; // newly seeded or discontinuous
	}
	if ((identity.w & TEMPORAL_SURFACE_FLAG_CORRESPONDENCE_VALID) != 0u)
	{
		const PrimitiveData primitive = GetPrimitiveData(hit.dataSource, hit.primitiveIndex);
		const SceneVertex v0 = GetVertexData(hit.dataSource, primitive.indices.x);
		const SceneVertex v1 = GetVertexData(hit.dataSource, primitive.indices.y);
		const SceneVertex v2 = GetVertexData(hit.dataSource, primitive.indices.z);
		const float3 weights = ResolveHitBarycentricWeights(hit);
		const float3 currentLocal = v0.position * weights.x + v1.position * weights.y + v2.position * weights.z;
		const float3 previousLocal = v0.prevPosition * weights.x + v1.prevPosition * weights.y + v2.prevPosition * weights.z;
		if (any(abs(currentLocal - previousLocal) > 1e-5))
		{
			return 0u; // exact vertex history
		}
		if (hit.instanceId != 0xffffffffu)
		{
			const SceneInstanceData instanceData = GetSceneInstanceData(hit.instanceId);
			const float3 currentInstancePoint = TransformSceneInstancePoint(instanceData, currentLocal, false);
			const float3 previousInstancePoint = TransformSceneInstancePoint(instanceData, previousLocal, true);
			if (any(abs(currentInstancePoint - previousInstancePoint) > 1e-5))
			{
				return 1u; // exact instance history
			}
		}
	}
	return 2u; // static or legacy fallback
}

void RecordMotionAudit(
	uint2 pixelPos,
	HitData hit,
	float3 currentWorld,
	float3 previousWorld,
	float2 currentUv,
	float2 previousUv,
	float currentViewZ,
	float previousViewZ,
	float4 motion,
	uint4 identity,
	uint validityReason,
	uint currentProjectionReason,
	uint previousProjectionReason)
{
	if (!MotionAuditStatsEnabled())
	{
		return;
	}
	const uint motionSource = ClassifyMotionSource(hit, identity, validityReason);
	TraceShaderStatAdd(TRACE_STAT_MOTION_SOURCE_COUNTER_BASE + min(motionSource, 3u), 1u);
	TraceShaderStatAdd(TRACE_STAT_MOTION_REASON_COUNTER_BASE + min(validityReason, 16u), 1u);
	if (any(pixelPos != uint2(gTraceConstants.RenderWidth / 2u, gTraceConstants.RenderHeight / 2u)))
	{
		return;
	}

	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 1u] = hit.dataSource;
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 2u] = hit.instanceId;
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 3u] = hit.primitiveIndex;
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 4u] = asuint(currentWorld.x);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 5u] = asuint(currentWorld.y);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 6u] = asuint(currentWorld.z);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 7u] = asuint(previousWorld.x);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 8u] = asuint(previousWorld.y);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 9u] = asuint(previousWorld.z);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 10u] = asuint(currentUv.x);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 11u] = asuint(currentUv.y);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 12u] = asuint(previousUv.x);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 13u] = asuint(previousUv.y);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 14u] = asuint(currentViewZ);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 15u] = asuint(previousViewZ);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 16u] = asuint(motion.x);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 17u] = asuint(motion.y);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 18u] = asuint(motion.z);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 19u] = asuint(motion.w);
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 20u] = identity.x;
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 21u] = identity.y;
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 22u] = identity.z;
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 23u] = identity.w;
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 24u] = validityReason;
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 25u] = currentProjectionReason;
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 26u] = previousProjectionReason;
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_BASE + 27u] = motionSource;
	DeviceMemoryBarrier();
	gTraceShaderStats[TRACE_STAT_MOTION_AUDIT_VALID] = 1u;
}

float3 ResolvePrimaryFootprintVertexPosition(HitData hit, float3 localPosition)
{
	if (hit.instanceId != 0xffffffffu)
	{
		return TransformSceneInstancePoint(
			GetSceneInstanceData(hit.instanceId), localPosition, false);
	}
	return localPosition;
}

bool TryResolvePrimaryBaseColorLod(
	HitData hit,
	MaterialData material,
	float3 viewRayDirection,
	out float selectedLod,
	out uint mipCount)
{
	selectedLod = 0.0;
	mipCount = 0u;
	if (material.textureIndex == 0xffffffffu)
	{
		return false;
	}

	const uint textureIndex = min(
		material.textureIndex, MAX_SCENE_TEXTURES - 1u);
	uint textureWidth = 0u;
	uint textureHeight = 0u;
	gSceneTextures[textureIndex].GetDimensions(
		0u, textureWidth, textureHeight, mipCount);
	if (textureWidth == 0u || textureHeight == 0u || mipCount == 0u)
	{
		mipCount = 0u;
		return false;
	}
	if (mipCount == 1u ||
		(material.flags & MATERIAL_FLAG_POINT_SAMPLED) != 0u)
	{
		return true;
	}

	const PrimitiveData primitive = GetPrimitiveData(
		hit.dataSource, hit.primitiveIndex);
	const float3 p0 = ResolvePrimaryFootprintVertexPosition(
		hit, GetVertexData(hit.dataSource, primitive.indices.x).position);
	const float3 p1 = ResolvePrimaryFootprintVertexPosition(
		hit, GetVertexData(hit.dataSource, primitive.indices.y).position);
	const float3 p2 = ResolvePrimaryFootprintVertexPosition(
		hit, GetVertexData(hit.dataSource, primitive.indices.z).position);
	const float3 edge1 = p1 - p0;
	const float3 edge2 = p2 - p0;
	const float3 areaVector = cross(edge1, edge2);
	const float areaSq = dot(areaVector, areaVector);
	if (areaSq <= 1.0e-12 ||
		gTraceConstants.RenderHeight == 0u ||
		hit.distance <= 0.0 ||
		!all(isfinite(p0)) || !all(isfinite(p1)) || !all(isfinite(p2)) ||
		!isfinite(hit.distance))
	{
		return true;
	}

	// Gradients of the UV coordinates over the placed triangle. Portal-path
	// distance remains an approximation until HitData carries the accumulated
	// presentation transform, but discrete LOD selection is stable and safe.
	const float3 barycentricGradient1 = cross(edge2, areaVector) / areaSq;
	const float3 barycentricGradient2 = cross(areaVector, edge1) / areaSq;
	const float2 uvEdge1 = primitive.uv1 - primitive.uv0;
	const float2 uvEdge2 = primitive.uv2 - primitive.uv0;
	const float3 texelGradientU =
		(uvEdge1.x * barycentricGradient1 +
			uvEdge2.x * barycentricGradient2) * (float)textureWidth;
	const float3 texelGradientV =
		(uvEdge1.y * barycentricGradient1 +
			uvEdge2.y * barycentricGradient2) * (float)textureHeight;
	const float gradientUU = dot(texelGradientU, texelGradientU);
	const float gradientVV = dot(texelGradientV, texelGradientV);
	const float gradientUV = dot(texelGradientU, texelGradientV);
	const float gradientDiscriminant = sqrt(max(
		(gradientUU - gradientVV) * (gradientUU - gradientVV) +
		4.0 * gradientUV * gradientUV, 0.0));
	const float texelsPerWorld = sqrt(max(
		0.5 * (gradientUU + gradientVV + gradientDiscriminant), 0.0));

	const float3 geometricNormal = normalize(areaVector);
	const float viewCosine = abs(dot(
		geometricNormal, normalize(-viewRayDirection)));
	const float primaryPixelCone =
		2.0 * abs(gTraceConstants.TanHalfFovY) * hit.distance /
		(float)gTraceConstants.RenderHeight;
	const float surfaceFootprint = primaryPixelCone / max(viewCosine, 0.125);
	const float texelFootprint = surfaceFootprint * texelsPerWorld;
	if (!isfinite(texelFootprint) || texelFootprint <= 0.0)
	{
		return true;
	}

	const float maximumLod = (float)(mipCount - 1u);
	selectedLod = round(clamp(
		log2(max(texelFootprint, 1.0)), 0.0, maximumLod));
	return true;
}

uint GetLightBounceCount()
{
	return gTraceConstants.BounceCounts & 0xfu;
}

uint GetMirrorBounceCount()
{
	return (gTraceConstants.BounceCounts >> 4u) & 0xfu;
}

bool UseSplitShadowDenoiser()
{
	return (gTraceConstants.Flags & 0x20u) != 0;
}

bool UseDirectionalPlaceholderLight()
{
	return (gTraceConstants.Flags & 0x80u) != 0;
}

bool UseDirectionalPlaceholderShadow()
{
	return (gTraceConstants.Flags & NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW) != 0;
}

bool UseRelaxDenoiser()
{
	return (gTraceConstants.ReservedTrace1 & 0xffu) == 1u;
}

uint GetEmissiveDirectSampleCount()
{
	return clamp((gTraceConstants.ReservedTrace1 >> 8u) & 0x3fu, 1u, 4u);
}

bool UseProbabilisticIndirectSampling()
{
	return (gTraceConstants.Flags & NRI_FLAG_PROBABILISTIC_INDIRECT) != 0u;
}

float GetLuminance(float3 value)
{
	return dot(max(value, 0.0), float3(0.2126, 0.7152, 0.0722));
}

float GetDiffuseIndirectSelectionProbability(float3 diffuseFactor, float3 specularFactor)
{
	const float diffuseEnergy = GetLuminance(diffuseFactor);
	const float specularEnergy = GetLuminance(specularFactor);
	if (diffuseEnergy <= 1e-5)
	{
		return 0.0;
	}
	if (specularEnergy <= 1e-5)
	{
		return 1.0;
	}

	const float boundedProbability = clamp(diffuseEnergy / (diffuseEnergy + specularEnergy), 0.25, 0.75);
	return round(boundedProbability * 16.0) * (1.0 / 16.0);
}

uint GetIndirectBayer4x4Index(uint2 pixelPos, uint frameIndex)
{
	const uint2 wrapped = pixelPos & 3u;
	const uint a = 2068378560u * (1u - (wrapped.x >> 1u)) + 1500172770u * (wrapped.x >> 1u);
	const uint b = (wrapped.y + ((wrapped.x & 1u) << 2u)) << 2u;
	return ((a >> b) + frameIndex) & 0xfu;
}

bool SelectDiffuseIndirectLobe(uint2 pixelPos, uint frameIndex, float diffuseProbability)
{
	const uint diffuseSampleCount = (uint)round(saturate(diffuseProbability) * 16.0);
	return GetIndirectBayer4x4Index(pixelPos, frameIndex) < diffuseSampleCount;
}

float3 EvaluateSunDiffuseLighting(float3 normal, float3 lightDir, float shadow)
{
	const float lambert = max(dot(normal, lightDir), 0.0);
	const float lighting = shadow * lambert * 0.80;
	return lighting.xxx;
}

float GetPackedAmbientMultiplier(uint shift)
{
	return (float)((gTraceConstants.PortalDepth >> shift) & 0xfffu) * (1.0 / 1024.0);
}

float GetBaseAmbientMultiplier()
{
	return GetPackedAmbientMultiplier(8u);
}

float GetMetalAmbientMultiplier()
{
	return GetPackedAmbientMultiplier(20u);
}

float3 EvaluateAmbientDiffuse(float3 albedo)
{
	return albedo * GetBaseAmbientMultiplier();
}

float3 EvaluateAmbientMetal(float3 albedo, float metalness)
{
	return albedo * saturate(metalness) * GetMetalAmbientMultiplier();
}

float3 EvaluateAmbientSurface(float3 albedo, float3 diffuseAlbedo, float metalness)
{
	return max(EvaluateAmbientDiffuse(diffuseAlbedo), EvaluateAmbientMetal(albedo, metalness));
}

float3 GetSurfaceDiffuseColor(float3 albedo, float metalness)
{
	return albedo * (1.0 - metalness);
}

float3 EvaluateDirectSunDiffuse(float3 albedo, float3 normal, float3 lightDir)
{
	const float lambert = max(dot(normal, lightDir), 0.0);
	return albedo * (lambert * 0.80);
}

float3 EvaluateSunSpecular(float3 albedo, float metalness, float3 normal, float3 viewDir, float3 lightDir, float shadow)
{
	const float lambert = max(dot(normal, lightDir), 0.0);
	const float3 halfVector = normalize(lightDir + viewDir);
	const float ndoth = max(dot(normal, halfVector), 0.0);
	const float vdoth = max(dot(viewDir, halfVector), 0.0);
	const float fresnel = pow(1.0 - vdoth, 5.0);
	const float3 dielectricF0 = lerp(float3(0.04, 0.04, 0.04), albedo, metalness);
	const float3 specularColor = lerp(dielectricF0, float3(1.0, 1.0, 1.0), fresnel);
	const float specularTerm = pow(ndoth, 12.0) * shadow * (0.5 + 0.5 * lambert);
	return specularColor * specularTerm * 0.85;
}

RuntimeLightTileHeaderData GetRuntimeLightTileHeader(uint2 pixelPos)
{
	RuntimeLightTileHeaderData header = { 0u, 0u };
	const uint2 tileCounts = uint2(gTraceConstants.ReservedTrace0 & 0xffffu, gTraceConstants.ReservedTrace0 >> 16u);
	static const uint kRuntimeLightTileSize = 64u;
	if (gTraceConstants.RuntimeLightCount == 0u ||
		tileCounts.x == 0u ||
		tileCounts.y == 0u)
	{
		return header;
	}

	const uint tileX = min(pixelPos.x / kRuntimeLightTileSize, tileCounts.x - 1u);
	const uint tileY = min(pixelPos.y / kRuntimeLightTileSize, tileCounts.y - 1u);
	return gRuntimeLightTileHeaders[tileY * tileCounts.x + tileX];
}

float GetSurfaceRoughness(MaterialData material, float2 uv)
{
	return SampleMaterialRoughness(material, uv);
}

static const float4 kReblurHitDistanceParams = float4(3.0, 0.1, 20.0, -25.0);

float GetNormalizedReblurHitDistance(float hitDistance, float viewZ, float roughness)
{
	const float trimmedHitDistance = NRD_FrontEnd_TrimHitDistance(max(hitDistance, 0.0), 0.001);
	return REBLUR_FrontEnd_GetNormHitDist(trimmedHitDistance, abs(viewZ), kReblurHitDistanceParams, roughness);
}

float4 PackReblurDiffuseRadiance(float3 radiance, float hitDistance, float viewZ)
{
	const float normHitDistance = GetNormalizedReblurHitDistance(hitDistance, viewZ, 1.0);
	return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDistance, true);
}

float4 PackReblurSpecularRadiance(float3 radiance, float hitDistance, float viewZ, float roughness)
{
	const float normHitDistance = GetNormalizedReblurHitDistance(hitDistance, viewZ, roughness);
	return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDistance, true);
}

float4 PackDiffuseRadiance(float3 radiance, float hitDistance, float viewZ)
{
	if (UseRelaxDenoiser())
	{
		return RELAX_FrontEnd_PackRadianceAndHitDist(radiance, max(hitDistance, 0.0), true);
	}

	return PackReblurDiffuseRadiance(radiance, hitDistance, viewZ);
}

float4 PackSpecularRadiance(float3 radiance, float hitDistance, float viewZ, float roughness)
{
	if (UseRelaxDenoiser())
	{
		return RELAX_FrontEnd_PackRadianceAndHitDist(radiance, max(hitDistance, 0.0), true);
	}

	return PackReblurSpecularRadiance(radiance, hitDistance, viewZ, roughness);
}

float GetSurfaceMetalness(MaterialData material, float2 uv)
{
	return SampleMaterialMetalness(material, uv);
}

float GetSurfaceMaterialID(MaterialData material)
{
	return min((float)material.materialClass, 3.0);
}

bool IsMaterialEmissive(MaterialData material)
{
	return material.emissiveMode != 0u && material.emissiveIntensity > 0.0;
}

float3 OverlayBlend(float3 target, float3 blend)
{
	target = saturate(target);
	blend = saturate(blend);
	const float3 low = (2.0 * target) * blend;
	const float3 high = 1.0 - 2.0 * (1.0 - target) * (1.0 - blend);
	return lerp(low, high, step(0.5.xxx, target));
}

float GetEmissiveMaterialResponseScale(uint dataSource, uint primitiveIndex)
{
	const uint responseCount = gEmissiveMaterialResponses[0].dataSource;
	[loop]
	for (uint i = 1u; i <= responseCount; ++i)
	{
		const EmissiveMaterialResponseData response = gEmissiveMaterialResponses[i];
		if (response.dataSource == dataSource && response.primitiveIndex == primitiveIndex)
		{
			return max(response.materialScale, 0.0);
		}
	}
	return 1.0;
}

float3 EvaluateMaterialEmission(uint materialIndex, uint dataSource, uint primitiveIndex, MaterialData material, float2 uv)
{
	if (material.emissiveMode != 0u)
	{
		return SampleMaterialEmissionSource(materialIndex, dataSource, uv) * material.emissiveIntensity * GetEmissiveMaterialResponseScale(dataSource, primitiveIndex);
	}
	return 0.0;
}

float3 EvaluateVisibleMaterialEmission(uint materialIndex, uint dataSource, uint primitiveIndex, MaterialData material, float3 albedo, float2 uv)
{
	const float materialResponseScale = GetEmissiveMaterialResponseScale(dataSource, primitiveIndex);
	if (material.emissiveMode == 3u)
	{
		const float3 glow = SampleMaterialEmissionSource(materialIndex, dataSource, uv);
		// A glowmap's black background is a mask, but OverlayBlend can produce a
		// non-zero result for black blend texels when the base color is above 0.5.
		// Preserve the authored overlay response while keeping masked texels dark.
		const float glowCoverage = saturate(max(glow.r, max(glow.g, glow.b)));
		const float visibleBlend = max(material.emissiveReserved, 0.0);
		return OverlayBlend(albedo, glow) * glowCoverage * material.emissiveIntensity * visibleBlend * materialResponseScale;
	}

	return EvaluateMaterialEmission(materialIndex, dataSource, primitiveIndex, material, uv);
}

uint GetEmissivePrimitiveCount()
{
	return gEmissivePrimitiveHeaders[0].activeCount;
}

uint GetSectorLightCount()
{
	return gSectorLightHeaders[0].sectorCount;
}

uint SampleEmissivePrimitiveIndex(inout uint rngState)
{
	const uint emissiveCount = GetEmissivePrimitiveCount();
	if (emissiveCount == 0u)
	{
		return 0xffffffffu;
	}

	const float r = RandomFloat01(rngState);
	uint low = 0u;
	uint high = emissiveCount - 1u;
	[unroll]
	for (uint i = 0u; i < 14u && low < high; ++i)
	{
		const uint mid = (low + high) >> 1u;
		if (r <= gEmissivePrimitiveCdf[mid])
		{
			high = mid;
		}
		else
		{
			low = mid + 1u;
		}
	}

	return low;
}

bool SamplePointOnEmissiveCandidate(
	EmissivePrimitiveData candidate,
	inout uint rngState,
	out uint outPrimitiveIndex,
	out uint outMaterialIndex,
	out float3 outPosition,
	out float2 outUv,
	out float3 outNormal,
	out float outEffectiveArea)
{
	outPrimitiveIndex = candidate.primitiveIndex;
	outMaterialIndex = 0xffffffffu;
	outPosition = 0.0;
	outUv = 0.0;
	outNormal = 0.0;
	outEffectiveArea = 0.0;
	const bool placedRange = candidate.sceneInstanceIndex != 0xffffffffu;
	SceneInstanceData instanceData = (SceneInstanceData)0;
	if (placedRange)
	{
		if (candidate.dataSource != SCENE_DATA_SOURCE_PERSISTENT_VOXEL || candidate.primitiveCount == 0u || candidate.sceneInstanceIndex >= gTraceConstants.SceneInstanceCount)
			return false;
		instanceData = GetSceneInstanceData(candidate.sceneInstanceIndex);
		if (instanceData.dataSource != SCENE_DATA_SOURCE_PERSISTENT_VOXEL ||
			instanceData.primitiveBase != candidate.primitiveIndex ||
			instanceData.metadata0 != candidate.occurrenceKeyLo ||
			instanceData.metadata1 != candidate.occurrenceKeyHi ||
			instanceData.metadata2 != candidate.occurrenceGeneration)
			return false;
		const uint persistentPrimitiveCount = GetPersistentVoxelPrimitiveCount();
		if (candidate.primitiveIndex >= persistentPrimitiveCount || candidate.primitiveCount > persistentPrimitiveCount - candidate.primitiveIndex)
			return false;
		outPrimitiveIndex = candidate.primitiveIndex + min((uint)(RandomFloat01(rngState) * candidate.primitiveCount), candidate.primitiveCount - 1u);
	}
	const PrimitiveData primitive = GetPrimitiveData(candidate.dataSource, outPrimitiveIndex);
	outMaterialIndex = placedRange ? ResolvePrimitiveMaterialIndex(instanceData, primitive) : primitive.materialIndex;
	SceneVertex v0 = GetVertexData(candidate.dataSource, primitive.indices.x);
	SceneVertex v1 = GetVertexData(candidate.dataSource, primitive.indices.y);
	SceneVertex v2 = GetVertexData(candidate.dataSource, primitive.indices.z);
	if (placedRange)
	{
		v0.position = TransformSceneInstancePoint(instanceData, v0.position, false);
		v1.position = TransformSceneInstancePoint(instanceData, v1.position, false);
		v2.position = TransformSceneInstancePoint(instanceData, v2.position, false);
		const float3 areaVector = cross(v1.position - v0.position, v2.position - v0.position);
		const float areaVectorLength = length(areaVector);
		if (areaVectorLength <= 1e-8)
			return false;
		outNormal = areaVector / areaVectorLength;
		outEffectiveArea = 0.5 * areaVectorLength * candidate.primitiveCount;
	}
	else
	{
		outNormal = normalize(primitive.normal);
		outEffectiveArea = candidate.primitiveArea;
	}
	const float u1 = RandomFloat01(rngState);
	const float u2 = RandomFloat01(rngState);
	const float rootU1 = sqrt(saturate(u1));
	const float3 barycentrics = float3(1.0 - rootU1, rootU1 * (1.0 - u2), rootU1 * u2);
	outUv = primitive.uv0 * barycentrics.x + primitive.uv1 * barycentrics.y + primitive.uv2 * barycentrics.z;
	outPosition = v0.position * barycentrics.x + v1.position * barycentrics.y + v2.position * barycentrics.z;
	return true;
}

bool ResolveSampledEmissiveTransportGeometry(
	float3 position,
	MaterialData receiverMaterial,
	float3 normal,
	float3 lightPosition,
	float3 lightNormal,
	out float outLightDistanceSq,
	out float outLightDistance,
	out float3 outLightDir,
	out float3 outReceiverLightNormal,
	out float outLambert,
	out float outEmitterLambert)
{
	outLightDistanceSq = 0.0;
	outLightDistance = 0.0;
	outLightDir = 0.0;
	outReceiverLightNormal = 0.0;
	outLambert = 0.0;
	outEmitterLambert = 0.0;
	const float3 toLight = lightPosition - position;
	const float lightDistanceSq = dot(toLight, toLight);
	if (lightDistanceSq <= 0.0001)
	{
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_DISTANCE_REJECT, 1u);
		return false;
	}

	const float lightDistance = sqrt(lightDistanceSq);
	const float3 lightDir = toLight / lightDistance;
	const float3 receiverLightNormal = ResolveLightFacingShadingNormal(receiverMaterial, normal, lightDir);
	const float lambert = max(dot(receiverLightNormal, lightDir), 0.0);
	if (lambert <= 0.0)
	{
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_RECEIVER_LAMBERT_REJECT, 1u);
		return false;
	}

	const float emitterLambert = max(dot(lightNormal, -lightDir), 0.0);
	if (emitterLambert <= 0.0)
	{
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_EMITTER_LAMBERT_REJECT, 1u);
		return false;
	}

	outLightDistanceSq = lightDistanceSq;
	outLightDistance = lightDistance;
	outLightDir = lightDir;
	outReceiverLightNormal = receiverLightNormal;
	outLambert = lambert;
	outEmitterLambert = emitterLambert;
	return true;
}

void EvaluateSampledEmissiveLighting(
	float3 position,
	MaterialData receiverMaterial,
	float3 normal,
	float3 viewDir,
	float3 albedo,
	float metalness,
	inout uint rngState,
	bool traceVisibility,
	bool forceSpatialProbeRay,
	out float3 outDiffuse,
	out float3 outSpecular,
	out bool outOccluded)
{
	outDiffuse = 0.0;
	outSpecular = 0.0;
	outOccluded = false;

	const uint candidateIndex = SampleEmissivePrimitiveIndex(rngState);
	if (candidateIndex == 0xffffffffu)
	{
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_CANDIDATE_NONE, 1u);
		return;
	}

	const EmissivePrimitiveData candidate = gEmissivePrimitives[candidateIndex];
	uint primitiveIndex = 0xffffffffu;
	uint materialIndex = 0xffffffffu;
	float3 lightPosition = 0.0;
	float2 lightUv = 0.0;
	float3 lightNormal = 0.0;
	float effectiveArea = 0.0;
	if (!SamplePointOnEmissiveCandidate(candidate, rngState, primitiveIndex, materialIndex, lightPosition, lightUv, lightNormal, effectiveArea))
	{
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_CANDIDATE_NONE, 1u);
		return;
	}

	float lightDistanceSq = 0.0;
	float lightDistance = 0.0;
	float3 lightDir = 0.0;
	float3 receiverLightNormal = 0.0;
	float lambert = 0.0;
	float emitterLambert = 0.0;
	if (!ResolveSampledEmissiveTransportGeometry(
		position,
		receiverMaterial,
		normal,
		lightPosition,
		lightNormal,
		lightDistanceSq,
		lightDistance,
		lightDir,
		receiverLightNormal,
		lambert,
		emitterLambert))
	{
		return;
	}

	const MaterialData lightMaterial = GetMaterialData(materialIndex, candidate.dataSource);
	const float3 lightColor = SampleMaterialEmissionSource(materialIndex, candidate.dataSource, lightUv) * lightMaterial.emissiveIntensity * max(candidate.emissionScale, 0.0);
	const float falloffScale = max(lightMaterial.emissiveMaskScale, 0.25);
	if (all(lightColor <= 0.0))
	{
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_LIGHT_ZERO, 1u);
		return;
	}

	if (traceVisibility)
	{
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_SHADOW_RAYS, 1u);
		if (!UseFastEmissiveShadow())
		{
			TraceShaderStatAdd(TRACE_STAT_TRACED_EMISSIVE_SHADOW_CALLS, 1u);
		}
		const float visibility = UseFastEmissiveShadow() ?
			ComputeFastPointLightShadow(position, receiverLightNormal, lightDir, lightDistance, forceSpatialProbeRay) :
			ComputePointLightShadowTagged(position, receiverLightNormal, lightDir, lightDistance, TRACE_STATS_KIND_EMISSIVE, forceSpatialProbeRay);
		if (visibility <= 0.0)
		{
			TraceShaderStatAdd(TRACE_STAT_EMISSIVE_VISIBILITY_OCCLUDED, 1u);
			outOccluded = true;
			return;
		}
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_VISIBILITY_VISIBLE, 1u);
	}

	const float pdf = max(candidate.selectionPdf, 1e-4);
	const float projectedArea = max(effectiveArea * emitterLambert, 0.001);
	const float attenuatedDistanceSq = pow(max(lightDistanceSq, 0.01), falloffScale);
	const float solidAngleEstimate = min(projectedArea / max(12.56637061436 * attenuatedDistanceSq, 0.01), 1.0);
	const float sampleWeight = min(solidAngleEstimate / pdf, 16.0);
	outDiffuse = GetSurfaceDiffuseColor(albedo, metalness) * (lambert * 0.80) * lightColor * sampleWeight;
	outSpecular = EvaluateSunSpecular(albedo, metalness, receiverLightNormal, viewDir, lightDir, 1.0) * lightColor * sampleWeight;
	TraceShaderStatAdd(TRACE_STAT_EMISSIVE_CONTRIBUTED, 1u);
}

void EvaluateSampledEmissiveMirrorRadiance(
	float3 position,
	MaterialData receiverMaterial,
	float3 normal,
	inout uint rngState,
	bool traceVisibility,
	out bool outOccluded,
	out float3 outEmitterRadiance)
{
	outOccluded = false;
	outEmitterRadiance = 0.0;

	const uint candidateIndex = SampleEmissivePrimitiveIndex(rngState);
	if (candidateIndex == 0xffffffffu)
	{
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_CANDIDATE_NONE, 1u);
		return;
	}

	const EmissivePrimitiveData candidate = gEmissivePrimitives[candidateIndex];
	uint primitiveIndex = 0xffffffffu;
	uint materialIndex = 0xffffffffu;
	float3 lightPosition = 0.0;
	float2 lightUv = 0.0;
	float3 lightNormal = 0.0;
	float effectiveArea = 0.0;
	if (!SamplePointOnEmissiveCandidate(candidate, rngState, primitiveIndex, materialIndex, lightPosition, lightUv, lightNormal, effectiveArea))
	{
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_CANDIDATE_NONE, 1u);
		return;
	}

	const MaterialData lightMaterial = GetMaterialData(materialIndex, candidate.dataSource);
	const float3 lightColor = SampleMaterialEmissionSource(materialIndex, candidate.dataSource, lightUv) * lightMaterial.emissiveIntensity * max(candidate.emissionScale, 0.0);
	outEmitterRadiance = lightColor;
	if (all(lightColor <= 0.0))
	{
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_LIGHT_ZERO, 1u);
		return;
	}

	float lightDistanceSq = 0.0;
	float lightDistance = 0.0;
	float3 lightDir = 0.0;
	float3 receiverLightNormal = 0.0;
	float lambert = 0.0;
	float emitterLambert = 0.0;
	if (!ResolveSampledEmissiveTransportGeometry(
		position,
		receiverMaterial,
		normal,
		lightPosition,
		lightNormal,
		lightDistanceSq,
		lightDistance,
		lightDir,
		receiverLightNormal,
		lambert,
		emitterLambert))
	{
		return;
	}

	if (traceVisibility)
	{
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_SHADOW_RAYS, 1u);
		if (!UseFastEmissiveShadow())
		{
			TraceShaderStatAdd(TRACE_STAT_TRACED_EMISSIVE_SHADOW_CALLS, 1u);
		}
		const float visibility = UseFastEmissiveShadow() ?
			ComputeFastPointLightShadow(position, receiverLightNormal, lightDir, lightDistance, false) :
			ComputePointLightShadowTagged(position, receiverLightNormal, lightDir, lightDistance, TRACE_STATS_KIND_EMISSIVE, false);
		if (visibility <= 0.0)
		{
			TraceShaderStatAdd(TRACE_STAT_EMISSIVE_VISIBILITY_OCCLUDED, 1u);
			outOccluded = true;
			return;
		}
		TraceShaderStatAdd(TRACE_STAT_EMISSIVE_VISIBILITY_VISIBLE, 1u);
	}

	TraceShaderStatAdd(TRACE_STAT_EMISSIVE_CONTRIBUTED, 1u);
}

float3 EvaluateSectorLightingSource(MaterialData material, float3 normal)
{
	if ((gSectorLightHeaders[0].flags & 0x1u) == 0u)
	{
		return 0.0;
	}

	const uint sectorIndex = material.sectorIndex;
	if (sectorIndex == 0xffffffffu || sectorIndex >= GetSectorLightCount())
	{
		return 0.0;
	}

	const SectorLightData sectorLight = gSectorLights[sectorIndex];
	const float contribution = sectorLight.ambientIntensity + abs(sectorLight.hemisphereAmount) + sectorLight.fogAmount;
	if (contribution <= 0.0)
	{
		return 0.0;
	}

	const float upFactor = saturate(normal.z * 0.5 + 0.5);
	const float hemisphereTerm = max(1.0 + sectorLight.hemisphereAmount * lerp(-1.0, 1.0, upFactor), 0.0);
	const float fogTerm = sectorLight.fogAmount * lerp(0.35, 1.0, upFactor);
	const float intensity = sectorLight.ambientIntensity * hemisphereTerm + fogTerm;
	return sectorLight.ambientColor * intensity;
}

float3 EvaluateSectorLighting(MaterialData material, float3 normal, float3 albedo)
{
	const float3 sourceLighting = EvaluateSectorLightingSource(material, normal);
	const float neutralAlbedo = dot(albedo, float3(0.2126, 0.7152, 0.0722));
	return sourceLighting * neutralAlbedo;
}

float3 GetSurfaceSpecularColor(float3 albedo, float metalness)
{
	return lerp(float3(0.04, 0.04, 0.04), albedo, metalness);
}

void GetNrdPrimaryMaterialFactors(float3 normal, float3 viewDir, float3 baseColor, float metalness, float roughness, out float3 diffuseFactor, out float3 specularFactor)
{
	const float3 albedo = GetSurfaceDiffuseColor(baseColor, metalness);
	const float3 rf0 = GetSurfaceSpecularColor(baseColor, metalness);
	NRD_MaterialFactors(normalize(normal), normalize(viewDir), albedo, rf0, roughness, diffuseFactor, specularFactor);
}

float3 TraceIndirectDiffuse(HitData surfaceHit, float3 surfaceAlbedo, uint2 pixelPos, uint frameIndex, uint bounceCount, out float outHitDistance)
{
	outHitDistance = 0.0;
	if (bounceCount == 0u)
	{
		return 0.0;
	}
	TraceShaderStatAdd(TRACE_STAT_INDIRECT_DIFFUSE_CALLS, 1u);

	uint rngState = pixelPos.x * 73856093u ^ pixelPos.y * 19349663u ^ (frameIndex + 1u) * 83492791u ^ 0x9e3779b9u;
	float3 throughput = surfaceAlbedo;
	float3 indirectRadiance = 0.0;
	float3 origin = surfaceHit.position + surfaceHit.normal * 0.05;
	float3 direction = SampleCosineHemisphere(surfaceHit.normal, rngState);
	bool hasSecondaryHitDistance = false;
#if defined(NRI_INDIRECT_RADIANCE_CACHE)
	HitData cacheWriteHit = MakeEmptyHitData();
	MaterialData cacheWriteMaterial = (MaterialData)0;
	float3 cacheWriteThroughput = 0.0;
	float3 cacheContinuationBaseline = 0.0;
	bool cacheWritePending = false;
#endif

	[loop]
	for (uint bounce = 0u; bounce < bounceCount; ++bounce)
	{
		TraceShaderStatAdd(TRACE_STAT_INDIRECT_DIFFUSE_BOUNCES, 1u);
		float3 tracedDirection = direction;
		const HitData bounceHit = TraceIndirectUngated(origin, direction, tracedDirection);
		if (!bounceHit.hit)
		{
			TraceShaderStatAdd(TRACE_STAT_INDIRECT_DIFFUSE_MISSES, 1u);
			if (!hasSecondaryHitDistance)
			{
				outHitDistance = NRD_INF;
				hasSecondaryHitDistance = true;
			}
			indirectRadiance += throughput * GetMissColor(tracedDirection);
			break;
		}

		if (!hasSecondaryHitDistance)
		{
			// NRD expects the first in-lobe secondary distance. Summing later path
			// segments makes unrelated topology publication look like a guide change.
			outHitDistance = bounceHit.distance;
			hasSecondaryHitDistance = true;
		}
		const MaterialData bounceMaterial = GetMaterialData(bounceHit.materialIndex, bounceHit.dataSource);
		const bool bounceReceivesShadow = MaterialReceivesShadow(bounceMaterial);
		if ((bounceMaterial.flags & (MATERIAL_FLAG_MIRROR | MATERIAL_FLAG_PORTAL)) != 0)
		{
			break;
		}

		const float bounceMetalness = GetSurfaceMetalness(bounceMaterial, bounceHit.uv);
		const float4 bounceAlbedo = SampleMaterialBaseColor(bounceHit.materialIndex, bounceHit.dataSource, bounceHit.uv);
		if (IsMaterialEmissive(bounceMaterial))
		{
			indirectRadiance += throughput * EvaluateMaterialEmission(bounceHit.materialIndex, bounceHit.dataSource, bounceHit.primitiveIndex, bounceMaterial, bounceHit.uv);
			break;
		}

		const float3 bounceDiffuseColor = GetSurfaceDiffuseColor(bounceAlbedo.rgb, bounceMetalness);
		const float3 bounceViewDir = normalize(-tracedDirection);
		const float3 bounceShadingNormal = ResolveViewFacingShadingNormal(bounceMaterial, bounceHit.normal, bounceViewDir);
		indirectRadiance += throughput * EvaluateSectorLighting(bounceMaterial, bounceShadingNormal, bounceDiffuseColor);
		float3 bounceEmissiveDiffuse = 0.0;
		float3 bounceEmissiveSpecular = 0.0;
		bool bounceEmissiveOccluded = false;
		EvaluateSampledEmissiveLighting(
			bounceHit.position,
			bounceMaterial,
			bounceShadingNormal,
			bounceViewDir,
			bounceAlbedo.rgb,
			bounceMetalness,
			rngState,
			bounceReceivesShadow,
			false,
			bounceEmissiveDiffuse,
			bounceEmissiveSpecular,
			bounceEmissiveOccluded);
		indirectRadiance += throughput * (bounceEmissiveDiffuse + bounceEmissiveSpecular);

		if (UseDirectionalPlaceholderLight())
		{
			const float3 bounceLightDir = SampleSunDirection(normalize(gTraceConstants.LightDirection), pixelPos + uint2(bounce + 1u, bounce * 3u + 1u), frameIndex + bounce + 1u);
			const float3 bounceDirectionalNormal = ResolveLightFacingShadingNormal(bounceMaterial, bounceShadingNormal, bounceLightDir);
			const float bounceShadow = (bounceReceivesShadow && UseDirectionalPlaceholderShadow()) ? ComputeSunShadow(bounceHit.position, bounceDirectionalNormal, bounceLightDir) : 1.0;
			indirectRadiance += throughput * bounceDiffuseColor * EvaluateSunDiffuseLighting(bounceDirectionalNormal, bounceLightDir, bounceShadow) * GetDirectionalPlaceholderColor();
		}
		throughput *= bounceDiffuseColor * 0.65;
		if (max(throughput.r, max(throughput.g, throughput.b)) < 0.01)
		{
			break;
		}

#if defined(NRI_INDIRECT_RADIANCE_CACHE)
		if (bounce == 0u && bounceCount > 1u)
		{
			float3 cachedIncidentRadiance = 0.0;
			if (TryReadIndirectRadianceCache(bounceHit, bounceMaterial, cachedIncidentRadiance))
			{
				indirectRadiance += throughput * cachedIncidentRadiance;
				break;
			}

			InterlockedAdd(gIndirectRadianceCacheTelemetry[NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_EXACT_FALLBACK], 1u);
			cacheWriteHit = bounceHit;
			cacheWriteMaterial = bounceMaterial;
			cacheWriteThroughput = throughput;
			cacheContinuationBaseline = indirectRadiance;
			cacheWritePending = true;
		}
#endif

		origin = bounceHit.position + bounceHit.normal * 0.05;
		direction = SampleCosineHemisphere(bounceHit.normal, rngState);
	}

#if defined(NRI_INDIRECT_RADIANCE_CACHE)
	if (cacheWritePending &&
		(gTraceConstants.Flags & NRI_FLAG_INDIRECT_RADIANCE_CACHE_ACCEPT) != 0u)
	{
		const float3 continuationRadiance = indirectRadiance - cacheContinuationBaseline;
		const float3 demodulatedContinuation = float3(
			cacheWriteThroughput.r > 1e-6 ? continuationRadiance.r / cacheWriteThroughput.r : 0.0,
			cacheWriteThroughput.g > 1e-6 ? continuationRadiance.g / cacheWriteThroughput.g : 0.0,
			cacheWriteThroughput.b > 1e-6 ? continuationRadiance.b / cacheWriteThroughput.b : 0.0);
		WriteIndirectRadianceCache(cacheWriteHit, cacheWriteMaterial, demodulatedContinuation);
	}
#endif

	return indirectRadiance;
}

float3 TraceIndirectSpecular(HitData surfaceHit, float4 surfaceAlbedo, float3 viewDir, uint2 pixelPos, uint frameIndex, float roughness, float metalness, uint bounceCount, out float outHitDistance)
{
	outHitDistance = 0.0;
	if (bounceCount == 0u)
	{
		return 0.0;
	}
	TraceShaderStatAdd(TRACE_STAT_INDIRECT_SPECULAR_CALLS, 1u);

	uint rngState = pixelPos.x * 73856093u ^ pixelPos.y * 19349663u ^ (frameIndex + 1u) * 83492791u ^ 0x85ebca6bu;
	float3 throughput = GetSurfaceSpecularColor(surfaceAlbedo.rgb, metalness);
	float3 indirectRadiance = 0.0;
	float3 origin = surfaceHit.position + surfaceHit.normal * 0.05;
	float3 direction = SampleSpecularLobe(reflect(-viewDir, surfaceHit.normal), roughness, rngState);
	bool hasSecondaryHitDistance = false;

	[loop]
	for (uint bounce = 0u; bounce < bounceCount; ++bounce)
	{
		TraceShaderStatAdd(TRACE_STAT_INDIRECT_SPECULAR_BOUNCES, 1u);
		float3 tracedDirection = direction;
		const HitData bounceHit = TraceReflectionUngated(origin, direction, tracedDirection);
		if (!bounceHit.hit)
		{
			TraceShaderStatAdd(TRACE_STAT_INDIRECT_SPECULAR_MISSES, 1u);
			if (!hasSecondaryHitDistance)
			{
				outHitDistance = NRD_INF;
				hasSecondaryHitDistance = true;
			}
			indirectRadiance += throughput * GetMissColor(tracedDirection);
			break;
		}

		if (!hasSecondaryHitDistance)
		{
			outHitDistance = bounceHit.distance;
			hasSecondaryHitDistance = true;
		}
		const MaterialData bounceMaterial = GetMaterialData(bounceHit.materialIndex, bounceHit.dataSource);
		const bool bounceReceivesShadow = MaterialReceivesShadow(bounceMaterial);
		if ((bounceMaterial.flags & MATERIAL_FLAG_PORTAL) != 0)
		{
			break;
		}

		const float bounceRoughness = GetSurfaceRoughness(bounceMaterial, bounceHit.uv);
		const float bounceMetalness = GetSurfaceMetalness(bounceMaterial, bounceHit.uv);
		const float4 bounceAlbedo = SampleMaterialBaseColor(bounceHit.materialIndex, bounceHit.dataSource, bounceHit.uv);
		if ((bounceMaterial.flags & MATERIAL_FLAG_FULLBRIGHT) != 0u)
		{
			const float3 visibleRadiance =
				IsMaterialEmissive(bounceMaterial) && (bounceMaterial.flags & MATERIAL_FLAG_TINT_EMISSION) != 0u ?
					EvaluateVisibleMaterialEmission(bounceHit.materialIndex, bounceHit.dataSource, bounceHit.primitiveIndex, bounceMaterial, bounceAlbedo.rgb, bounceHit.uv) :
					bounceAlbedo.rgb * max(bounceMaterial.emissiveReserved, 1.0);
			indirectRadiance += throughput * visibleRadiance;
			break;
		}
		if (IsMaterialEmissive(bounceMaterial))
		{
			indirectRadiance += throughput * EvaluateMaterialEmission(bounceHit.materialIndex, bounceHit.dataSource, bounceHit.primitiveIndex, bounceMaterial, bounceHit.uv);
			break;
		}

		const float3 bounceDiffuseColor = GetSurfaceDiffuseColor(bounceAlbedo.rgb, bounceMetalness);
		const float3 bounceViewDir = normalize(-tracedDirection);
		const float3 bounceShadingNormal = ResolveViewFacingShadingNormal(bounceMaterial, bounceHit.normal, bounceViewDir);
		indirectRadiance += throughput * EvaluateSectorLighting(bounceMaterial, bounceShadingNormal, bounceDiffuseColor);
		float3 bounceEmissiveDiffuse = 0.0;
		float3 bounceEmissiveSpecular = 0.0;
		bool bounceEmissiveOccluded = false;
		EvaluateSampledEmissiveLighting(
			bounceHit.position,
			bounceMaterial,
			bounceShadingNormal,
			bounceViewDir,
			bounceAlbedo.rgb,
			bounceMetalness,
			rngState,
			bounceReceivesShadow,
			false,
			bounceEmissiveDiffuse,
			bounceEmissiveSpecular,
			bounceEmissiveOccluded);
		indirectRadiance += throughput * (bounceEmissiveDiffuse + bounceEmissiveSpecular);

		if (UseDirectionalPlaceholderLight())
		{
			const float3 bounceLightDir = SampleSunDirection(normalize(gTraceConstants.LightDirection), pixelPos + uint2(bounce * 5u + 1u, bounce * 7u + 3u), frameIndex + bounce + 1u);
			const float3 bounceDirectionalNormal = ResolveLightFacingShadingNormal(bounceMaterial, bounceShadingNormal, bounceLightDir);
			const float bounceShadow = (bounceReceivesShadow && UseDirectionalPlaceholderShadow()) ? ComputeSunShadow(bounceHit.position, bounceDirectionalNormal, bounceLightDir) : 1.0;
			indirectRadiance += throughput * (
				(bounceDiffuseColor * EvaluateSunDiffuseLighting(bounceDirectionalNormal, bounceLightDir, bounceShadow) +
				EvaluateSunSpecular(bounceAlbedo.rgb, bounceMetalness, bounceDirectionalNormal, bounceViewDir, bounceLightDir, bounceShadow)) * GetDirectionalPlaceholderColor());
		}

		throughput *= GetSurfaceSpecularColor(bounceAlbedo.rgb, bounceMetalness) * (0.9 - bounceRoughness * 0.35);
		if (max(throughput.r, max(throughput.g, throughput.b)) < 0.01)
		{
			break;
		}

		origin = bounceHit.position + bounceHit.normal * 0.05;
		direction = SampleSpecularLobe(reflect(tracedDirection, bounceHit.normal), bounceRoughness, rngState);
	}

	return indirectRadiance;
}

float3 ReflectPointAcrossPlane(float3 position, float3 planePoint, float3 planeNormal)
{
	return position - planeNormal * (2.0 * dot(position - planePoint, planeNormal));
}

float3 ReflectVectorAcrossPlane(float3 value, float3 planeNormal)
{
	return value - planeNormal * (2.0 * dot(value, planeNormal));
}

bool TryApplyPlainMirrorPrimaryReplacement(inout HitData hit, float3 primaryRayDirection, inout float3 visibleRayDirection, out float3 mirrorThroughput, out float3 mirrorPlanePosition, out float3 mirrorPlaneNormal)
{
	mirrorThroughput = 1.0;
	mirrorPlanePosition = 0.0;
	mirrorPlaneNormal = 0.0;

	if (!hit.hit)
	{
		return false;
	}

	const MaterialData mirrorMaterial = GetMaterialData(hit.materialIndex, hit.dataSource);
	if (!IsPlainMirrorMaterial(mirrorMaterial))
	{
		return false;
	}

	const float4 mirrorAlbedo = SampleMaterialBaseColor(hit.materialIndex, hit.dataSource, hit.uv);
	const float mirrorMetalness = GetSurfaceMetalness(mirrorMaterial, hit.uv);
	mirrorPlanePosition = hit.position;
	mirrorPlaneNormal = dot(hit.normal, -primaryRayDirection) >= 0.0 ? normalize(hit.normal) : -normalize(hit.normal);
	const float3 mirrorSpecularColor = GetSurfaceSpecularColor(mirrorAlbedo.rgb, mirrorMetalness);
	const float mirrorNoV = saturate(dot(mirrorPlaneNormal, -primaryRayDirection));
	const float mirrorFresnel = pow(1.0 - mirrorNoV, 5.0);
	mirrorThroughput = lerp(mirrorSpecularColor, 1.0, mirrorFresnel);

	const float3 reflectedDirection = normalize(reflect(primaryRayDirection, mirrorPlaneNormal));
	const float3 reflectedOrigin = hit.position + mirrorPlaneNormal * 0.05;
	float3 tracedReflectedDirection = reflectedDirection;
	const uint rootTemporalFlags = hit.temporalFlags;
	hit = TracePlainMirrorReplacement(reflectedOrigin, reflectedDirection, tracedReflectedDirection);
	hit.temporalFlags |= rootTemporalFlags;
	visibleRayDirection = tracedReflectedDirection;
	return true;
}

float3 EvaluatePlainMirrorSurfaceGlint(HitData mirrorHit, float3 mirrorPlaneNormal, float3 primaryRayDirection, uint2 pixelPos)
{
	const MaterialData mirrorMaterial = GetMaterialData(mirrorHit.materialIndex, mirrorHit.dataSource);
	const float4 mirrorAlbedo = SampleMaterialBaseColor(mirrorHit.materialIndex, mirrorHit.dataSource, mirrorHit.uv);
	const float mirrorMetalness = GetSurfaceMetalness(mirrorMaterial, mirrorHit.uv);
	const float3 mirrorViewDir = normalize(-primaryRayDirection);
	const float mirrorNoV = saturate(dot(mirrorPlaneNormal, mirrorViewDir));
	const float edgeFactor = pow(1.0 - mirrorNoV, 1.5) * (1.0 - smoothstep(0.25, 0.85, mirrorNoV));
	if (edgeFactor <= 0.0)
	{
		return 0.0;
	}

	const float3 mirrorSpecularColor = GetSurfaceSpecularColor(mirrorAlbedo.rgb, mirrorMetalness);
	const float3 mirrorFresnel = lerp(mirrorSpecularColor, 1.0, pow(1.0 - mirrorNoV, 5.0));
	const bool receivesShadow = MaterialReceivesShadow(mirrorMaterial);
	float3 sourceRadiance = 0.0;

	if (UseDirectionalPlaceholderLight())
	{
		const float3 lightDir = SampleSunDirection(normalize(gTraceConstants.LightDirection), pixelPos + uint2(17u, 29u), gTraceConstants.FrameIndex);
		const float3 lightNormal = ResolveLightFacingShadingNormal(mirrorMaterial, mirrorPlaneNormal, lightDir);
		const float lightFacing = saturate(dot(lightNormal, lightDir));
		const float shadow = (receivesShadow && UseDirectionalPlaceholderShadow()) ? ComputeSunShadow(mirrorHit.position, lightNormal, lightDir) : 1.0;
		sourceRadiance += GetDirectionalPlaceholderColor() * (0.35 + 0.65 * lightFacing) * shadow;
	}

	const RuntimeLightTileHeaderData runtimeLightTile = GetRuntimeLightTileHeader(pixelPos);
	[loop]
	for (uint runtimeLightCandidate = 0u; runtimeLightCandidate < runtimeLightTile.indexCount; ++runtimeLightCandidate)
	{
		const uint runtimeLightIndex = gRuntimeLightTileIndices[runtimeLightTile.indexOffset + runtimeLightCandidate];
		if (runtimeLightIndex >= gTraceConstants.RuntimeLightCount)
		{
			continue;
		}

		const RuntimePointLightData runtimeLight = gRuntimePointLights[runtimeLightIndex];
		const float3 toLight = runtimeLight.position - mirrorHit.position;
		const float lightDistanceSq = dot(toLight, toLight);
		if (lightDistanceSq <= 0.0001)
		{
			continue;
		}

		const float lightDistance = sqrt(lightDistanceSq);
		if (lightDistance >= runtimeLight.radius)
		{
			continue;
		}

		const float3 lightDir = toLight / lightDistance;
		const float3 lightNormal = ResolveLightFacingShadingNormal(mirrorMaterial, mirrorPlaneNormal, lightDir);
		const float lightFacing = saturate(dot(lightNormal, lightDir));
		const bool runtimeLightCastsShadow = (runtimeLight.flags & RUNTIME_POINT_LIGHT_FLAG_CASTS_SHADOW) != 0u;
		const float runtimeShadow = (receivesShadow && runtimeLightCastsShadow) ? ComputePointLightShadow(mirrorHit.position, lightNormal, lightDir, lightDistance) : 1.0;
		if (runtimeShadow <= 0.0)
		{
			continue;
		}

		const float attenuation = EvaluateAnalyticPointLightAttenuation(lightDistance, runtimeLight.radius, runtimeLight.intensity);
		sourceRadiance += runtimeLight.color * attenuation * (0.35 + 0.65 * lightFacing) * runtimeShadow;
	}

	bool emissiveSampleOccluded = false;
	float3 emissiveSampleRadiance = 0.0;
	uint emissiveSampleRng = pixelPos.x ^ (pixelPos.y << 16u) ^ (gTraceConstants.FrameIndex + 1u) * 0x51ed270bu;
	EvaluateSampledEmissiveMirrorRadiance(
		mirrorHit.position,
		mirrorMaterial,
		mirrorPlaneNormal,
		emissiveSampleRng,
		receivesShadow,
		emissiveSampleOccluded,
		emissiveSampleRadiance);
	if (!emissiveSampleOccluded)
	{
		sourceRadiance += emissiveSampleRadiance * 0.25;
	}

	sourceRadiance = min(sourceRadiance, float3(4.0, 4.0, 4.0));
	const float sourceLuma = dot(sourceRadiance, float3(0.2126, 0.7152, 0.0722));
	const float sourceGate = smoothstep(0.03, 0.40, sourceLuma);
	return sourceRadiance * mirrorFresnel * (edgeFactor * sourceGate * 0.55);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const bool spatialProbeTargetPixel = IsSpatialAbsenceProbeTargetPixel(pixelPos);
	float3 visibleRayDirection = GeneratePrimaryRay(pixelPos);
	const float3 primaryRayDirection = visibleRayDirection;
	float3 rayOrigin = gTraceConstants.CameraPos;
	const uint bootstrapMode = gTraceConstants.BootstrapMode;
	const bool bootstrapSceneDirect = bootstrapMode == 11 || bootstrapMode == 12;
	const bool directSceneTrace = bootstrapSceneDirect || ((gTraceConstants.Flags & 0x8u) != 0);
	const bool bootstrapFlat = bootstrapMode == 11;
	const bool bootstrapBaseColor = bootstrapMode == 12;

	HitData hit = (HitData)0;
	if (directSceneTrace)
	{
		hit = TraceBootstrapGeometry(rayOrigin, visibleRayDirection);
	}
	else
	{
		float3 tracedVisibleDirection = visibleRayDirection;
		hit = TracePrimary(
			rayOrigin,
			visibleRayDirection,
			ShouldGatePrimaryVisibleChunks(),
			spatialProbeTargetPixel,
			tracedVisibleDirection);
		visibleRayDirection = tracedVisibleDirection;
	}
	RecordSpatialAbsenceProbePrimaryPixel(pixelPos, hit);

	float3 plainMirrorThroughput = 1.0;
	float3 plainMirrorPlanePosition = 0.0;
	float3 plainMirrorPlaneNormal = 0.0;
	HitData plainMirrorHit = hit;
	const bool plainMirrorPrimaryReplacement = !directSceneTrace && TryApplyPlainMirrorPrimaryReplacement(hit, primaryRayDirection, visibleRayDirection, plainMirrorThroughput, plainMirrorPlanePosition, plainMirrorPlaneNormal);

	float4 color = 0.0;
	if (!hit.hit)
	{
		TraceShaderStatAdd(TRACE_STAT_PRIMARY_MISS_PIXELS, 1u);
		if (bootstrapFlat || bootstrapBaseColor)
		{
			const float3 sentinel = bootstrapFlat ? float3(1.0, 0.0, 1.0) : float3(1.0, 0.5, 0.0);
			const uint bootstrapReason = MOTION_VALIDITY_GLOBAL_RESET;
			color = gTraceConstants.DebugMode == 5 ? VisualizeMotionVector(float4(0.0, 0.0, 0.0, -1.0), 1.0) :
				(gTraceConstants.DebugMode == 20 ? VisualizeMotionDepth(float4(0.0, 0.0, 0.0, -1.0)) :
				(gTraceConstants.DebugMode == 47 ? VisualizeMotionValidity(bootstrapReason) : float4(sentinel, 1.0)));
			gMotionOutput[pixelPos] = float4(0.0, 0.0, 0.0, -1.0);
			WriteTemporalOutputs(pixelPos, uint4(0u, 0u, 0u, TEMPORAL_SURFACE_FLAG_LEGACY_FALLBACK), 1.0, float3(0.0, 0.0, 1.0), bootstrapReason);
			gViewZOutput[pixelPos] = float4(1.0, 0.0, 0.0, 1.0);
			gNormalRoughnessOutput[pixelPos] = 0.0;
			gBaseColorOutput[pixelPos] = float4(sentinel, 1.0);
			gGuideSpecularOutput[pixelPos] = float4(0.0, 0.0, 0.0, 1.0);
			gGuideSpecHitOutput[pixelPos] = float4(0.0, 0.0, 0.0, 1.0);
			gShadowPenumbraOutput[pixelPos] = float4(SIGMA_FrontEnd_PackPenumbra(NRD_FP16_MAX, GetDirectionalPlaceholderTanAngularSize()), 0.0, 0.0, 0.0);
			gDirectLightingOutput[pixelPos] = 0.0;
			gDirectEmissionOutput[pixelPos] = float4(sentinel, 1.0);
		}
		else
		{
			const float3 missColor = GetMissColor(visibleRayDirection) * plainMirrorThroughput;
			const float4 packedDiffuse = PackDiffuseRadiance(missColor, NRD_INF, NRD_INF);
			color = (gTraceConstants.DebugMode >= 1 && gTraceConstants.DebugMode <= 4) ? float4(missColor, 1.0) : packedDiffuse;
			bool missCurrentUvValid = false;
			bool missPrevUvValid = false;
			float2 missCurrentUvRaw = 0.0;
			float2 missPrevUvRaw = 0.0;
			float2 missMotionPixels = 0.0;
			float missCurrentViewZ = 0.0;
			float missPreviousViewZ = 0.0;
			ComputeSkyVirtualMotion(rayOrigin, visibleRayDirection, missCurrentUvValid, missPrevUvValid, missCurrentUvRaw, missPrevUvRaw, missMotionPixels, missCurrentViewZ, missPreviousViewZ);
			const float missMotionZ = (missCurrentUvValid && missPrevUvValid) ? (missPreviousViewZ - missCurrentViewZ) : 0.0;
			const bool missProjectionValid = missCurrentUvValid && missPrevUvValid;
			const uint missValidityReason = EvaluateSkyTemporalCorrespondence(pixelPos, missMotionPixels, missProjectionValid);
			const float4 missMotion = float4(missMotionPixels, missMotionZ, missProjectionValid ? NRD_INF : -NRD_INF);
			color = gTraceConstants.DebugMode == 5 ? VisualizeMotionVector(missMotion, NRD_INF) :
				(gTraceConstants.DebugMode == 20 ? VisualizeMotionDepth(missMotion) :
				(gTraceConstants.DebugMode == 47 ? VisualizeMotionValidity(missValidityReason) : color));
			gMotionOutput[pixelPos] = missMotion;
			WriteTemporalOutputs(pixelPos, uint4(0xffffffffu, 0xffffffffu, 1u, TEMPORAL_SURFACE_FLAG_IDENTITY_VALID | TEMPORAL_SURFACE_FLAG_CORRESPONDENCE_VALID), NRD_INF, float3(0.0, 0.0, 1.0), missValidityReason);
			gViewZOutput[pixelPos] = float4(NRD_INF, 0.0, 0.0, 1.0);
			gNormalRoughnessOutput[pixelPos] = 0.0;
			gBaseColorOutput[pixelPos] = float4(missColor, 0.0);
			// Keep sky misses out of NRD's ordinary noisy radiance inputs and composite them via direct emission.
			gGuideSpecularOutput[pixelPos] = 0.0;
			gGuideSpecHitOutput[pixelPos] = 0.0;
			gShadowPenumbraOutput[pixelPos] = float4(SIGMA_FrontEnd_PackPenumbra(NRD_FP16_MAX, GetDirectionalPlaceholderTanAngularSize()), 0.0, 0.0, 0.0);
			gDirectLightingOutput[pixelPos] = 0.0;
			gDirectEmissionOutput[pixelPos] = float4(missColor, 1.0);
		}
	}
	else
	{
		TraceShaderStatAdd(TRACE_STAT_PRIMARY_HIT_PIXELS, 1u);
		TraceShaderStatSource(TRACE_STAT_PRIMARY_HIT_STATIC, TRACE_STAT_PRIMARY_HIT_DYNAMIC, TRACE_STAT_PRIMARY_HIT_VOXEL, hit.dataSource);
		float3 currentHitPosition = ResolveHitVertexPosition(hit, false);
		float3 previousHitPosition = ResolveHitVertexPosition(hit, true);
		float3 guideNormal = hit.normal;
		if (plainMirrorPrimaryReplacement)
		{
			currentHitPosition = ReflectPointAcrossPlane(currentHitPosition, plainMirrorPlanePosition, plainMirrorPlaneNormal);
			previousHitPosition = ReflectPointAcrossPlane(previousHitPosition, plainMirrorPlanePosition, plainMirrorPlaneNormal);
			guideNormal = normalize(ReflectVectorAcrossPlane(hit.normal, plainMirrorPlaneNormal));
		}
		const float currentViewZ = dot(currentHitPosition - gTraceConstants.CameraPos, gTraceConstants.CameraForward);
		float2 currentUvRaw = 0.0;
		float2 prevUvRaw = 0.0;
		uint currentProjectionReason = MOTION_VALIDITY_VALID;
		uint previousProjectionReason = MOTION_VALIDITY_VALID;
		const bool currentUvValid = ProjectWorldToUvMatrixRaw(currentHitPosition, false, currentUvRaw, currentProjectionReason);
		const bool prevUvValid = ProjectWorldToUvMatrixRaw(previousHitPosition, true, prevUvRaw, previousProjectionReason);
		const float previousViewZ = dot(previousHitPosition - gTraceConstants.PrevCameraPos, gTraceConstants.PrevCameraForward);
		float3 motion = 0.0;
		if (currentUvValid && prevUvValid)
		{
			motion.xy = (prevUvRaw - currentUvRaw) * float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
			motion.z = previousViewZ - currentViewZ;
		}

		float4 albedo = 1.0;
		float3 diffuse = 0.0;
		float3 specular = 0.0;
		float3 directLighting = 0.0;
		float3 directEmission = 0.0;
		// Explicit lighting ownership after phase-2 demodulation:
		// - directLighting: stable direct-composition bucket
		//   * ambient / sector ambient
		//   * placeholder directional sun diffuse/specular when split-shadow composition is inactive
		//   * runtime point-light direct terms
		// - deferredDirectionalLighting: exact unshadowed sun RGB for split-shadow composition
		// - diffuse/specular: NRD-facing primary-hit demodulated radiance
		//   * sampled emissive and indirect transport are divided by primary material factors
		//   * composition remodulates them later using the same factor model
		// - directEmission: additive self-emission term plus any legacy fullbright override
		float3 ambientDirectLighting = 0.0;
		float3 runtimePointDirectLighting = 0.0;
		float3 sunTransportDiffuse = 0.0;
		float3 sunTransportSpecular = 0.0;
		float3 deferredDirectionalLighting = 0.0;
		float3 sampledEmissiveTransportDiffuse = 0.0;
		float3 sampledEmissiveTransportSpecular = 0.0;
		float3 sampledAnalyticTransportDiffuse = 0.0;
		float3 sampledAnalyticTransportSpecular = 0.0;
		float3 indirectTransportDiffuse = 0.0;
		float3 indirectTransportSpecular = 0.0;
		float3 analyticDirectLighting = 0.0;
		float3 emissiveDirectLighting = 0.0;
		float emissiveSampleVisibleFraction = 0.0;
		float emissiveSampleOccludedFraction = 0.0;
		float3 sectorSourceLighting = 0.0;
		float3 sectorAmbientLighting = 0.0;
		float diffuseHitDistance = 0.0;
		float specularHitDistance = 0.0;
		float indirectDiffuseSelectionProbability = 0.0;
		bool indirectDiffuseSelected = false;
		bool indirectSpecularSelected = false;
		// Default to fully lit when this surface does not issue a SIGMA shadow ray (for
		// example, materials which opt out of receiving directional shadows).
		float shadowPenumbra = SIGMA_FrontEnd_PackPenumbra(NRD_FP16_MAX, GetDirectionalPlaceholderTanAngularSize());
		float roughness = 1.0;
		bool smokeForeground = false;
		float primaryBaseColorLod = 0.0;
		uint primaryBaseColorMipCount = 0u;
		if (bootstrapFlat)
		{
			const float primitiveHash = (float)(hit.primitiveIndex % 31u) / 30.0;
			diffuse = float3(frac(primitiveHash * 1.7), frac(primitiveHash * 2.3), frac(primitiveHash * 3.1));
		}
		else
		{
			const MaterialData material = GetMaterialData(hit.materialIndex, hit.dataSource);
			TryResolvePrimaryBaseColorLod(
				hit,
				material,
				visibleRayDirection,
				primaryBaseColorLod,
				primaryBaseColorMipCount);
			albedo = SampleMaterialBaseColorLevel(
				hit.materialIndex,
				hit.dataSource,
				hit.uv,
				primaryBaseColorLod);
			RecordSurfaceProbePrimaryPixel(pixelPos, hit, material);
			smokeForeground = (material.lightingFlags & MATERIAL_LIGHTING_FLAG_SMOKE_FOREGROUND) != 0u;
			const bool fullbright = (material.flags & MATERIAL_FLAG_FULLBRIGHT) != 0;
			const bool emissiveMaterial = IsMaterialEmissive(material);
			const bool plainMirrorMaterial = IsPlainMirrorMaterial(material);
			if (fullbright)
			{
				TraceShaderStatAdd(TRACE_STAT_MATERIAL_FULLBRIGHT, 1u);
			}
			if (emissiveMaterial)
			{
				TraceShaderStatAdd(TRACE_STAT_MATERIAL_EMISSIVE, 1u);
			}
			const bool receivesShadow = MaterialReceivesShadow(material);
			roughness = GetSurfaceRoughness(material, hit.uv);
			const float metalness = GetSurfaceMetalness(material, hit.uv);
			const float3 diffuseAlbedo = GetSurfaceDiffuseColor(albedo.rgb, metalness);
			const float materialID = GetSurfaceMaterialID(material);
			if (bootstrapBaseColor)
			{
				diffuse = albedo.rgb;
				directEmission = albedo.rgb;
			}
			else if (fullbright)
			{
				diffuse = 0.0;
				specular = 0.0;
				if (emissiveMaterial && (material.flags & MATERIAL_FLAG_TINT_EMISSION) != 0u)
				{
					directEmission = EvaluateVisibleMaterialEmission(hit.materialIndex, hit.dataSource, hit.primitiveIndex, material, albedo.rgb, hit.uv);
				}
				else
				{
					directEmission = albedo.rgb * max(material.emissiveReserved, 1.0);
				}
			}
			else
			{
				const bool useDirectionalLight = UseDirectionalPlaceholderLight();
				const bool useDirectionalShadow = UseDirectionalPlaceholderShadow() && receivesShadow;
				const float3 directionalLightColor = GetDirectionalPlaceholderColor();
				const float3 viewDir = normalize(-visibleRayDirection);
				const float3 nrdViewDir = plainMirrorPrimaryReplacement ? normalize(-primaryRayDirection) : viewDir;
				const float3 shadingNormal = ResolveViewFacingShadingNormal(material, hit.normal, viewDir);
				float3 nrdDiffuseFactor = 1.0;
				float3 nrdSpecularFactor = 1.0;
				GetNrdPrimaryMaterialFactors(guideNormal, nrdViewDir, albedo.rgb, metalness, roughness, nrdDiffuseFactor, nrdSpecularFactor);
				const bool useProbabilisticIndirect = UseProbabilisticIndirectSampling() && !plainMirrorPrimaryReplacement;
				if (useProbabilisticIndirect)
				{
					indirectDiffuseSelectionProbability = GetDiffuseIndirectSelectionProbability(nrdDiffuseFactor, nrdSpecularFactor);
					indirectDiffuseSelected = SelectDiffuseIndirectLobe(pixelPos, gTraceConstants.FrameIndex, indirectDiffuseSelectionProbability);
					indirectSpecularSelected = !indirectDiffuseSelected;
				}
				const float3 centerLightDir = normalize(gTraceConstants.LightDirection);
				const float3 shadowSampleDir = directSceneTrace ? centerLightDir : SampleSunDirection(centerLightDir, pixelPos, gTraceConstants.FrameIndex);
				const float3 shadowShadingNormal = ResolveLightFacingShadingNormal(material, shadingNormal, shadowSampleDir);
				const float3 directionalShadingNormal = ResolveLightFacingShadingNormal(material, shadingNormal, centerLightDir);
				float shadowHitDistance = 0.0;
				if (useDirectionalLight && useDirectionalShadow && !directSceneTrace)
				{
					TraceShaderStatAdd(TRACE_STAT_DIRECTIONAL_SHADOW_TESTS, 1u);
				}
				const float shadow = useDirectionalLight ? ((directSceneTrace || !useDirectionalShadow) ? 1.0 : ComputeSunShadow(hit.position, shadowShadingNormal, shadowSampleDir, shadowHitDistance, spatialProbeTargetPixel)) : 0.0;
				if (useDirectionalLight && useDirectionalShadow && !directSceneTrace)
				{
					shadowPenumbra = SIGMA_FrontEnd_PackPenumbra(shadowHitDistance, GetDirectionalPlaceholderTanAngularSize());
				}
				if (!plainMirrorMaterial)
				{
					sectorSourceLighting = EvaluateSectorLightingSource(material, shadingNormal);
					sectorAmbientLighting = EvaluateSectorLighting(material, shadingNormal, diffuseAlbedo);
					ambientDirectLighting = EvaluateAmbientSurface(albedo.rgb, diffuseAlbedo, metalness) + sectorAmbientLighting;
					sunTransportDiffuse = useDirectionalLight ? EvaluateDirectSunDiffuse(diffuseAlbedo, directionalShadingNormal, centerLightDir) * directionalLightColor : 0.0;
					sunTransportSpecular = useDirectionalLight ? EvaluateSunSpecular(albedo.rgb, metalness, directionalShadingNormal, viewDir, centerLightDir, 1.0) * directionalLightColor : 0.0;
				}

				const RuntimeLightTileHeaderData runtimeLightTile = GetRuntimeLightTileHeader(pixelPos);
				const uint runtimeLightCandidateCount = plainMirrorMaterial ? 0u : runtimeLightTile.indexCount;
				if (runtimeLightCandidateCount > 0u)
				{
					TraceShaderStatAdd(TRACE_STAT_RUNTIME_TILE_NONEMPTY, 1u);
					TraceShaderStatMax(TRACE_STAT_RUNTIME_TILE_MAX, runtimeLightCandidateCount);
				}
				[loop]
				for (uint runtimeLightCandidate = 0u; runtimeLightCandidate < runtimeLightCandidateCount; ++runtimeLightCandidate)
				{
					TraceShaderStatAdd(TRACE_STAT_RUNTIME_CANDIDATES, 1u);
					const uint runtimeLightIndex = gRuntimeLightTileIndices[runtimeLightTile.indexOffset + runtimeLightCandidate];
					if (runtimeLightIndex >= gTraceConstants.RuntimeLightCount)
					{
						continue;
					}

					const RuntimePointLightData runtimeLight = gRuntimePointLights[runtimeLightIndex];
					const float3 toLightCenter = runtimeLight.position - hit.position;
					const float centerLightDistanceSq = dot(toLightCenter, toLightCenter);
					if (centerLightDistanceSq <= 0.0001)
					{
						continue;
					}

					const float centerLightDistance = sqrt(centerLightDistanceSq);
					if (centerLightDistance >= runtimeLight.radius)
					{
						continue;
					}
					TraceShaderStatAdd(TRACE_STAT_RUNTIME_DISTANCE, 1u);

					const float3 centerLightDir = toLightCenter / centerLightDistance;
					const bool runtimeLightCastsShadow = (runtimeLight.flags & RUNTIME_POINT_LIGHT_FLAG_CASTS_SHADOW) != 0u;
					const bool useSoftRuntimeShadow = !directSceneTrace && receivesShadow && runtimeLightCastsShadow && runtimeLight.emitterRadius > 0.0;
					const uint runtimeLightStableSeed = Hash32(runtimeLight.stableKeyLo ^ Hash32(runtimeLight.stableKeyHi + 0x9e3779b9u));
					uint runtimeLightRng = pixelPos.x * 1973u ^ pixelPos.y * 9277u ^ (gTraceConstants.FrameIndex + 1u) * 26699u ^ runtimeLightStableSeed;
					float3 sampledLightPosition = runtimeLight.position;
					if (useSoftRuntimeShadow)
					{
						sampledLightPosition = SampleRuntimePointEmitter(runtimeLight, centerLightDir, runtimeLightRng);
					}
					const float3 toSampledLight = sampledLightPosition - hit.position;
					const float lightDistanceSq = dot(toSampledLight, toSampledLight);
					if (lightDistanceSq <= 0.0001)
					{
						continue;
					}

					const float lightDistance = sqrt(lightDistanceSq);
					const float3 runtimeLightDir = toSampledLight / lightDistance;
					const float3 runtimeShadingNormal = ResolveLightFacingShadingNormal(material, shadingNormal, runtimeLightDir);
					const float lambert = max(dot(runtimeShadingNormal, runtimeLightDir), 0.0);
					if (lambert <= 0.0)
					{
						continue;
					}
					TraceShaderStatAdd(TRACE_STAT_RUNTIME_LAMBERT, 1u);

					if (!directSceneTrace && receivesShadow && runtimeLightCastsShadow)
					{
						TraceShaderStatAdd(TRACE_STAT_RUNTIME_SHADOW_RAYS, 1u);
					}
					const float runtimeShadow = (directSceneTrace || !receivesShadow || !runtimeLightCastsShadow) ? 1.0 : ComputePointLightShadow(hit.position, runtimeShadingNormal, runtimeLightDir, lightDistance, spatialProbeTargetPixel);
					if (runtimeShadow <= 0.0)
					{
						TraceShaderStatAdd(TRACE_STAT_RUNTIME_SHADOW_OCCLUDED, 1u);
						continue;
					}
					TraceShaderStatAdd(TRACE_STAT_RUNTIME_SHADOW_VISIBLE, 1u);
					if (useSoftRuntimeShadow)
					{
						TraceShaderStatAdd(TRACE_STAT_RUNTIME_SOFT_SHADOW_SAMPLES, 1u);
					}

					const float attenuation = EvaluateAnalyticPointLightAttenuation(centerLightDistance, runtimeLight.radius, runtimeLight.intensity);
					if (attenuation <= 0.0)
					{
						continue;
					}

					const float3 lightColor = runtimeLight.color * attenuation;
					const float3 analyticDiffuse = diffuseAlbedo * (lambert * 0.80) * lightColor * runtimeShadow;
					const float3 analyticSpecular = EvaluateSunSpecular(albedo.rgb, metalness, runtimeShadingNormal, viewDir, runtimeLightDir, 1.0) * lightColor * runtimeShadow;
					analyticDirectLighting += analyticDiffuse + analyticSpecular;
					if (useSoftRuntimeShadow)
					{
						sampledAnalyticTransportDiffuse += analyticDiffuse / nrdDiffuseFactor;
						sampledAnalyticTransportSpecular += analyticSpecular / nrdSpecularFactor;
						TraceShaderStatAdd(TRACE_STAT_RUNTIME_SOFT_TRANSPORT, 1u);
					}
					else
					{
						runtimePointDirectLighting += analyticDiffuse + analyticSpecular;
					}
				}

				float3 emissiveSampleDiffuse = 0.0;
				float3 emissiveSampleSpecular = 0.0;
				uint emissiveSampleRng = pixelPos.x ^ (pixelPos.y << 16u) ^ (gTraceConstants.FrameIndex + 1u) * 0x9e3779b9u;
				const uint emissiveSampleCount = directSceneTrace ? 1u : GetEmissiveDirectSampleCount();
				[loop]
				for (uint emissiveSampleIndex = 0u; emissiveSampleIndex < emissiveSampleCount; ++emissiveSampleIndex)
				{
					TraceShaderStatAdd(TRACE_STAT_EMISSIVE_SAMPLES, 1u);
					float3 sampleDiffuse = 0.0;
					float3 sampleSpecular = 0.0;
					bool sampleOccluded = false;
					EvaluateSampledEmissiveLighting(
						hit.position,
						material,
						shadingNormal,
						viewDir,
						albedo.rgb,
						metalness,
						emissiveSampleRng,
						!directSceneTrace && receivesShadow,
						spatialProbeTargetPixel,
						sampleDiffuse,
						sampleSpecular,
						sampleOccluded);

					const bool sampleContributed = any((sampleDiffuse + sampleSpecular) > 0.0);
					if (sampleOccluded)
					{
						emissiveSampleOccludedFraction += 1.0;
					}
					else if (sampleContributed)
					{
						emissiveSampleVisibleFraction += 1.0;
					}

					emissiveSampleDiffuse += sampleDiffuse;
					emissiveSampleSpecular += sampleSpecular;
				}

				const float invEmissiveSampleCount = rcp((float)emissiveSampleCount);
				emissiveSampleVisibleFraction *= invEmissiveSampleCount;
				emissiveSampleOccludedFraction *= invEmissiveSampleCount;
				emissiveSampleDiffuse *= invEmissiveSampleCount;
				emissiveSampleSpecular *= invEmissiveSampleCount;
				emissiveDirectLighting += emissiveSampleDiffuse + emissiveSampleSpecular;
				sampledEmissiveTransportDiffuse = emissiveSampleDiffuse / nrdDiffuseFactor;
				sampledEmissiveTransportSpecular = emissiveSampleSpecular / nrdSpecularFactor;

				const uint lightBounceCount = GetLightBounceCount();
				if (!directSceneTrace && lightBounceCount > 0u)
				{
					if (!useProbabilisticIndirect || indirectDiffuseSelected)
					{
						indirectTransportDiffuse = TraceIndirectDiffuse(hit, diffuseAlbedo, pixelPos, gTraceConstants.FrameIndex, lightBounceCount, diffuseHitDistance) / nrdDiffuseFactor;
					}
					if (!useProbabilisticIndirect || indirectSpecularSelected)
					{
						indirectTransportSpecular = TraceIndirectSpecular(hit, albedo, viewDir, pixelPos, gTraceConstants.FrameIndex, roughness, metalness, lightBounceCount, specularHitDistance) / nrdSpecularFactor;
					}
				}

				const float3 stochasticDiffuse = sampledEmissiveTransportDiffuse + sampledAnalyticTransportDiffuse + indirectTransportDiffuse;
				const float3 stochasticSpecular = sampledEmissiveTransportSpecular + sampledAnalyticTransportSpecular + indirectTransportSpecular;
				if (useProbabilisticIndirect)
				{
					if (indirectDiffuseSelected)
					{
						diffuse += stochasticDiffuse / indirectDiffuseSelectionProbability;
					}
					else
					{
						specular += stochasticSpecular / (1.0 - indirectDiffuseSelectionProbability);
					}
				}
				else
				{
					diffuse += stochasticDiffuse;
					specular += stochasticSpecular;
				}
				// Keep the placeholder sun out of the primary-hit demodulated NRD bucket so its
				// hard shadow structure does not get spatially mixed back into REBLUR/RELAX history.
				const bool deferDirectionalComposition = UseSplitShadowDenoiser() && useDirectionalLight && useDirectionalShadow;
				directLighting += ambientDirectLighting + runtimePointDirectLighting;
				if (!deferDirectionalComposition)
				{
					directLighting += (sunTransportDiffuse + sunTransportSpecular) * shadow;
				}
				else
				{
					deferredDirectionalLighting = sunTransportDiffuse + sunTransportSpecular;
				}
				if (emissiveMaterial)
				{
					directEmission = EvaluateVisibleMaterialEmission(hit.materialIndex, hit.dataSource, hit.primitiveIndex, material, albedo.rgb, hit.uv);
				}
			}

			if (plainMirrorPrimaryReplacement)
			{
				directLighting += EvaluatePlainMirrorSurfaceGlint(plainMirrorHit, plainMirrorPlaneNormal, primaryRayDirection, pixelPos);
				diffuse *= plainMirrorThroughput;
				specular *= plainMirrorThroughput;
				directLighting *= plainMirrorThroughput;
				directEmission *= plainMirrorThroughput;
				deferredDirectionalLighting *= plainMirrorThroughput;
			}
			gNormalRoughnessOutput[pixelPos] = NRD_FrontEnd_PackNormalAndRoughness(guideNormal, roughness, materialID);
			gBaseColorOutput[pixelPos] = float4(bootstrapFlat ? diffuse : albedo.rgb, metalness);
		}
		// A primary traversal which skipped a player-census-absent actor exposes
		// a newly visible background sample. Keep the raw wall hit, but reject
		// application history at this pixel so the prior actor cannot be
		// reprojected into the foreign locality for one frame.
		const bool actorCensusHistoryInvalid =
			(hit.temporalFlags & HIT_TEMPORAL_FLAG_ACTOR_CENSUS_REJECTED) != 0u;
		const uint4 currentTemporalIdentity = GetPrimaryTemporalIdentity(hit);
		const bool producerCorrespondenceValid =
			currentUvValid && prevUvValid &&
			((currentTemporalIdentity.w & TEMPORAL_SURFACE_FLAG_LEGACY_FALLBACK) != 0u ||
				(currentTemporalIdentity.w & TEMPORAL_SURFACE_FLAG_CORRESPONDENCE_VALID) != 0u) &&
			!actorCensusHistoryInvalid;
		uint temporalValidityReason = EvaluateTemporalCorrespondence(
			pixelPos,
			currentTemporalIdentity,
			motion,
			previousViewZ,
			ResolveHitGeometricNormal(hit, true),
			currentUvValid,
			currentProjectionReason,
			prevUvValid,
			previousProjectionReason);
		if (actorCensusHistoryInvalid)
		{
			temporalValidityReason = MOTION_VALIDITY_ACTOR_CENSUS;
		}
		const float4 motionOutput = float4(motion, producerCorrespondenceValid ? currentViewZ : -1.0);
		gMotionOutput[pixelPos] = motionOutput;
		WriteTemporalOutputs(pixelPos, currentTemporalIdentity, currentViewZ, ResolveHitGeometricNormal(hit, false), temporalValidityReason);
		RecordMotionAudit(
			pixelPos,
			hit,
			currentHitPosition,
			previousHitPosition,
			currentUvRaw,
			prevUvRaw,
			currentViewZ,
			previousViewZ,
			motionOutput,
			currentTemporalIdentity,
			temporalValidityReason,
			currentProjectionReason,
			previousProjectionReason);
		gViewZOutput[pixelPos] = float4(currentViewZ, smokeForeground ? 1.0 : 0.0, 0.0, 1.0);
		const float4 packedDiffuse = PackDiffuseRadiance(diffuse, diffuseHitDistance, currentViewZ);
		const float4 packedSpecular = PackSpecularRadiance(specular, specularHitDistance, currentViewZ, roughness);
		if (bootstrapFlat)
		{
			gNormalRoughnessOutput[pixelPos] = NRD_FrontEnd_PackNormalAndRoughness(guideNormal, 1.0, 0.0);
			gBaseColorOutput[pixelPos] = float4(diffuse, 0.0);
		}
		gGuideSpecularOutput[pixelPos] = packedSpecular;
		gGuideSpecHitOutput[pixelPos] = float4(specular, packedSpecular.w);
		// SIGMA consumes x only. Preserve the exact, producer-owned unshadowed sun term in yzw
		// so Composition can replace stochastic visibility without reconstructing BRDF or normal policy.
		gShadowPenumbraOutput[pixelPos] = float4(shadowPenumbra, deferredDirectionalLighting);
		gDirectLightingOutput[pixelPos] = float4(directLighting, 1.0);
		gDirectEmissionOutput[pixelPos] = float4(directEmission, 1.0);

		if (gTraceConstants.DebugMode == 1)
		{
			color = float4(guideNormal * 0.5 + 0.5, 1.0);
		}
		else if (gTraceConstants.DebugMode == 2)
		{
			color = float4(frac(hit.uv), 0.0, 1.0);
		}
		else if (gTraceConstants.DebugMode == 3)
		{
			float id = (float)(hit.materialIndex % 19u) / 18.0;
			color = float4(id, frac(id * 1.7), frac(id * 2.3), 1.0);
		}
		else if (gTraceConstants.DebugMode == 4)
		{
			float id = (float)(hit.primitiveIndex % 29u) / 28.0;
			color = float4(frac(id * 1.1), frac(id * 1.9), frac(id * 2.7), 1.0);
		}
		else if (gTraceConstants.DebugMode == 5)
		{
			color = VisualizeMotionVector(motionOutput, currentViewZ);
		}
		else if (gTraceConstants.DebugMode == 20)
		{
			color = VisualizeMotionDepth(motionOutput);
		}
		else if (gTraceConstants.DebugMode == 26)
		{
			color = float4(analyticDirectLighting, 1.0);
		}
		else if (gTraceConstants.DebugMode == 27)
		{
			color = float4(directEmission, 1.0);
		}
		else if (gTraceConstants.DebugMode == 28)
		{
			color = float4(emissiveDirectLighting, 1.0);
		}
		else if (gTraceConstants.DebugMode == 29)
		{
			color = float4(sectorSourceLighting, 1.0);
		}
		else if (gTraceConstants.DebugMode == 33)
		{
			const float rejectedFraction = saturate(1.0 - emissiveSampleVisibleFraction - emissiveSampleOccludedFraction);
			color = float4(emissiveSampleOccludedFraction, emissiveSampleVisibleFraction, rejectedFraction, 1.0);
		}
		else if (gTraceConstants.DebugMode == 46)
		{
			color = float4(indirectDiffuseSelected ? 1.0 : 0.0, indirectSpecularSelected ? 1.0 : 0.0, indirectDiffuseSelectionProbability, 1.0);
		}
		else if (gTraceConstants.DebugMode == 47)
		{
			color = VisualizeMotionValidity(temporalValidityReason);
		}
		else
		{
			color = packedDiffuse;
		}
	}

	gTraceOutput[pixelPos] = color;
	if (directSceneTrace && !bootstrapFlat && !bootstrapBaseColor)
	{
		gFinalOutput[pixelPos] = saturate(color.rgb);
	}
}
