#ifndef RAZE_NRI_WALL_DECALS_HLSLI
#define RAZE_NRI_WALL_DECALS_HLSLI

#include "RaytracingShared.hlsli"

float4 SampleWallDecalColor(WallDecalData decal, float2 uv)
{
	if (decal.textureIndex >= MAX_SCENE_TEXTURES)
	{
		return 0.0;
	}

	const bool indexed = (decal.flags & MATERIAL_FLAG_INDEXED) != 0u;
	const bool pointSampled = indexed || (decal.flags & MATERIAL_FLAG_POINT_SAMPLED) != 0u;
	const uint textureIndex = decal.textureIndex;
	// The receiver's mip footprint belongs to its own UV mapping. Reusing that
	// LOD here can erase thin crack/handprint details. Use the source-art level
	// for both coverage and color, with clamp addressing at the decal boundary.
	float4 color = pointSampled
		? gSceneTextures[textureIndex].SampleLevel(gPointClamp, uv, 0.0)
		: gSceneTextures[textureIndex].SampleLevel(gLinearClamp, uv, 0.0);
	float coverage = saturate(color.a);
	if (indexed)
	{
		// Indexed alpha is the transparent source index, not palette-table alpha.
		const uint paletteIndex = (uint)round(saturate(color.r) * 255.0);
		if (paletteIndex == 0u)
		{
			return 0.0;
		}
		const float2 paletteUv = float2(
			((float)paletteIndex + 0.5) / 256.0,
			((float)min(decal.paletteIndex, 255u) + 0.5) / 256.0);
		color = gPaletteLookup.SampleLevel(gPointClamp, paletteUv, 0.0);
		coverage = 1.0;
	}
	return float4(color.rgb * decal.lightLevel, coverage * saturate(decal.alpha));
}

float4 ApplyWallDecals(HitData hit, float4 receiverColor)
{
	if (!hit.hit || hit.dataSource == SCENE_DATA_SOURCE_PERSISTENT_VOXEL)
	{
		return receiverColor;
	}
	const MaterialData receiver = GetMaterialData(hit.materialIndex, hit.dataSource);
	const uint excludedFlags = MATERIAL_FLAG_SPRITE | MATERIAL_FLAG_FLAT |
		MATERIAL_FLAG_SKY | MATERIAL_FLAG_PORTAL | MATERIAL_FLAG_MIRROR;
	if (receiver.wallIndex == 0xffffffffu || (receiver.flags & excludedFlags) != 0u)
	{
		return receiverColor;
	}

	uint headerCount = 0u;
	uint headerStride = 0u;
	gWallDecalHeaders.GetDimensions(headerCount, headerStride);
	if (receiver.wallIndex >= headerCount)
	{
		return receiverColor;
	}
	const uint2 range = gWallDecalHeaders[receiver.wallIndex];
	if (range.y == 0u)
	{
		return receiverColor;
	}
	uint recordCount = 0u;
	uint recordStride = 0u;
	gWallDecals.GetDimensions(recordCount, recordStride);
	if (range.x >= recordCount)
	{
		return receiverColor;
	}

	const PrimitiveData primitive = GetPrimitiveData(hit.dataSource, hit.primitiveIndex);
	const float3 receiverNormal = hit.instanceId == 0xffffffffu
		? normalize(primitive.normal)
		: TransformSceneInstanceNormal(GetSceneInstanceData(hit.instanceId), primitive.normal, false);
	const float4 receiverPosition = float4(hit.position, 1.0);
	const uint rangeEnd = range.x + min(range.y, recordCount - range.x);
	[loop]
	for (uint index = range.x; index < rangeEnd; ++index)
	{
		const WallDecalData decal = gWallDecals[index];
		if (decal.wallIndex != receiver.wallIndex || decal.sectorIndex != receiver.sectorIndex ||
			dot(receiverNormal, decal.plane.xyz) <= 0.0 ||
			abs(dot(receiverPosition, decal.plane)) > 0.05)
		{
			continue;
		}
		const float2 uv = float2(dot(receiverPosition, decal.worldToU), dot(receiverPosition, decal.worldToV));
		if (any(uv < decal.uvBounds.xy) || any(uv > decal.uvBounds.zw))
		{
			continue;
		}
		const float4 decalColor = SampleWallDecalColor(decal, uv);
		receiverColor.rgb = lerp(receiverColor.rgb, decalColor.rgb, decalColor.a);
	}
	return receiverColor;
}

float4 SampleHitBaseColor(HitData hit)
{
	return ApplyWallDecals(hit, SampleMaterialBaseColor(hit.materialIndex, hit.dataSource, hit.uv));
}

float4 SampleHitBaseColorLevel(HitData hit, float lod)
{
	return ApplyWallDecals(hit, SampleMaterialBaseColorLevel(hit.materialIndex, hit.dataSource, hit.uv, lod));
}

#endif
