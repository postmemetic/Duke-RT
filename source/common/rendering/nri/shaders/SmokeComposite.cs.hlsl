#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint2 dimensions = uint2(gSmokeConstants.OutputWidth, gSmokeConstants.OutputHeight);
	if (dispatchThreadId.x >= dimensions.x || dispatchThreadId.y >= dimensions.y)
		return;
	const uint2 pixel = dispatchThreadId.xy;
	const float4 scene = gSmokeSceneInput.Load(int3(pixel, 0));
	const float4 volume = gSmokeVolumeResolvedInput.Load(int3(pixel, 0));
	const float4 meta = gSmokeVolumeResolvedMetaInput.Load(int3(pixel, 0));
	const float transmittance = exp(-clamp(volume.a, 0.0, 16.0));
	float3 color = scene.rgb * transmittance + max(volume.rgb, 0.0);
	if (SmokeDebugMode(gSmokeConstants.DebugMode) >= 12u)
		color = max(volume.rgb, 0.0);
	else if (SmokeDebugMode(gSmokeConstants.DebugMode) == 1u)
		color = meta.x.xxx;
	else if (SmokeDebugMode(gSmokeConstants.DebugMode) == 2u)
		color = transmittance.xxx;
	else if (SmokeDebugMode(gSmokeConstants.DebugMode) == 3u)
	{
		const float2 stableUv = SmokePrimarySampleUv(pixel);
		const float2 cell = frac(stableUv * float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight));
		const float edge = step(min(min(cell.x, 1.0 - cell.x), min(cell.y, 1.0 - cell.y)), 0.04);
		color = lerp(float3(cell, meta.y), 1.0.xxx, edge);
	}
	else if (SmokeDebugMode(gSmokeConstants.DebugMode) == 4u)
		color = float3(meta.y, meta.z, 0.0);
	else if (SmokeDebugMode(gSmokeConstants.DebugMode) == 5u)
		color = max(volume.rgb, 0.0);
	else if (SmokeDebugMode(gSmokeConstants.DebugMode) == 6u)
		color = meta.w.xxx;
	else if (SmokeDebugMode(gSmokeConstants.DebugMode) == 7u)
		color = meta.x.xxx;
	gSmokeOutput[pixel] = float4(color, scene.a);
}
