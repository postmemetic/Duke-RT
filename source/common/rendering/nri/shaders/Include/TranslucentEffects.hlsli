#ifndef RAZE_NRI_TRANSLUCENT_EFFECTS_HLSLI
#define RAZE_NRI_TRANSLUCENT_EFFECTS_HLSLI

#include "Shared.hlsli"

// Effect sprites share the live scene's geometry/material uploads, but do not
// own opaque depth, lighting or denoiser guides. Resolve their alpha layers
// after opaque lighting, in both the NRD and ray-reconstruction compositions.
float4 SampleTranslucentEffect(MaterialData material, float2 uv)
{
	if (material.textureIndex >= MAX_SCENE_TEXTURES) return 0.0;
	const bool indexed = (material.flags & MATERIAL_FLAG_INDEXED) != 0u;
	const uint textureIndex = NonUniformResourceIndex(material.textureIndex);
	float4 color = indexed
		? gSceneTextures[textureIndex].SampleLevel(gPointClamp, uv, 0.0)
		: gSceneTextures[textureIndex].SampleLevel(gLinearClamp, uv, 0.0);
	float coverage = saturate(color.a);
	if (indexed)
	{
		const uint paletteIndex = (uint)round(saturate(color.r) * 255.0);
		if (paletteIndex == 0u) return 0.0;
		const float2 paletteUv = float2((paletteIndex + 0.5) / 256.0,
			(min(material.paletteIndex, 255u) + 0.5) / 256.0);
		color = gPaletteLookup.SampleLevel(gPointClamp, paletteUv, 0.0);
		coverage = 1.0;
	}
	return float4(color.rgb * material.lightLevel, coverage * saturate(material.alpha));
}

float3 CompositeTranslucentEffects(uint2 pixelPos, float3 background)
{
	// Composition publishes scene counts only when this frame has effects.
	if (gTraceConstants.SceneInstanceCount == 0u) return background;
	const float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
	const float2 jitter = gReprojectionDataBuffer[0].currentJitter;
	float2 ndc = ((float2(pixelPos) + 0.5 + jitter) / resolution) * 2.0 - 1.0;
	ndc.y = -ndc.y;
	const float3 direction = normalize(gTraceConstants.CameraForward +
		ndc.x * gTraceConstants.TanHalfFovX * gTraceConstants.CameraRight +
		ndc.y * gTraceConstants.TanHalfFovY * gTraceConstants.CameraUp);
	// ViewZ is axial depth stored in half precision. Allow half an ULP of
	// rounding plus the wall attachment nudge, including distant receivers.
	const float viewZ = abs(gViewZInput.Load(int3(pixelPos, 0)).x);
	const float depthAllowance = exp2(floor(log2(max(viewZ, 1.0))) - 11.0) + 0.02;
	const float maxDistance = (viewZ + depthAllowance) / max(dot(direction, gTraceConstants.CameraForward), 1e-4);
	// BuildGeometry assigns a separate material row to each captured surface.
	// Remember occurrences rather than advancing ray depth: separate impact
	// rings can occupy exactly the same plane, while their two triangles must
	// still blend only once at a shared edge.
	uint2 blendedSurfaces[32];
	float transmittance = 1.0;
	float3 radiance = 0.0;
	[loop]
	for (uint layer = 0u; layer < 32u && transmittance > 1.0 / 255.0 && 1e-4 < maxDistance; ++layer)
	{
		RayDesc ray = { gTraceConstants.CameraPos, 1e-4, direction, maxDistance };
		RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
		query.TraceRayInline(gWorldTlas, RAY_FLAG_NONE, NRI_TLAS_MASK_MAIN, ray);
		float4 nearestColor = 0.0;
		uint2 nearestSurface = 0u;
		float nearestDistance = maxDistance;
		while (query.Proceed())
		{
			if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
			const uint instanceId = query.CandidateInstanceID();
			if (instanceId >= gTraceConstants.SceneInstanceCount) continue;
			const SceneInstanceData instance = gSceneInstances[instanceId];
			// Captured effects are dynamic geometry; persistent voxels and static
			// map surfaces remain entirely owned by the opaque trace.
			if (instance.dataSource != 1u) continue;
			const uint primitiveIndex = instance.primitiveBase + query.CandidatePrimitiveIndex();
			if (primitiveIndex >= gTraceConstants.DynamicPrimitiveCount) continue;
			const PrimitiveData primitive = gDynamicPrimitives[primitiveIndex];
			if ((primitive.flags & PRIMITIVE_FLAG_REFLECTION_ONLY) != 0u) continue;
			const uint materialIndex = instance.materialBase + primitive.materialIndex;
			if (materialIndex >= gTraceConstants.DynamicMaterialCount) continue;
			const MaterialData material = gDynamicMaterials[materialIndex];
			if ((material.flags & MATERIAL_FLAG_TRANSLUCENT_EFFECT) == 0u) continue;
			const uint2 surface = uint2(instanceId, materialIndex);
			bool alreadyBlended = false;
			[loop]
			for (uint previous = 0u; previous < layer; ++previous)
				alreadyBlended = alreadyBlended || all(blendedSurfaces[previous] == surface);
			if (alreadyBlended) continue;
			const float distance = query.CandidateTriangleRayT();
			if (distance > nearestDistance) continue;
			// Select in software so committing a hit cannot prune equal-depth
			// candidates before the stable surface-key tie-break sees them.
			if (nearestColor.a > 0.0 && distance == nearestDistance &&
				(surface.x > nearestSurface.x || (surface.x == nearestSurface.x && surface.y >= nearestSurface.y))) continue;
			const float2 bary = query.CandidateTriangleBarycentrics();
			const float2 uv = primitive.uv0 * (1.0 - bary.x - bary.y) + primitive.uv1 * bary.x + primitive.uv2 * bary.y;
			const float4 color = SampleTranslucentEffect(material, uv);
			if (color.a <= 0.0) continue;
			nearestColor = color;
			nearestSurface = surface;
			nearestDistance = distance;
		}
		if (nearestColor.a <= 0.0) break;
		radiance += transmittance * nearestColor.a * nearestColor.rgb;
		transmittance *= 1.0 - nearestColor.a;
		blendedSurfaces[layer] = nearestSurface;
	}
	if (transmittance < 1.0)
	{
		const float existingReactive = gTemporalReactiveOutput[pixelPos].x;
		gTemporalReactiveOutput[pixelPos] = float4(max(existingReactive, 1.0 - transmittance), 0.0, 0.0, 1.0);
	}
	return radiance + transmittance * background;
}

#endif
