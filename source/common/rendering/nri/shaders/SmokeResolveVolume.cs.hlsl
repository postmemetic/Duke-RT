#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeTransientHistory.hlsli"

struct SmokeBilinearFootprint
{
	uint2 p00;
	uint2 p10;
	uint2 p01;
	uint2 p11;
	float2 blend;
};

SmokeBilinearFootprint SmokeMakeBilinearFootprint(float2 stableUv)
{
	const float2 coordinate = SmokeFroxelCoordinate(stableUv);
	const int2 base = int2(floor(coordinate));
	const int2 maximum = int2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u);
	const int2 p0 = clamp(base, int2(0, 0), maximum);
	const int2 p1 = clamp(base + 1, int2(0, 0), maximum);
	SmokeBilinearFootprint footprint;
	footprint.p00 = uint2(p0.x, p0.y);
	footprint.p10 = uint2(p1.x, p0.y);
	footprint.p01 = uint2(p0.x, p1.y);
	footprint.p11 = uint2(p1.x, p1.y);
	footprint.blend = frac(coordinate);
	return footprint;
}

float SmokeBilinearIntegratedTau(SmokeBilinearFootprint footprint, uint z)
{
	if (SmokeTransientCoverageEnabled())
	{
		const float t00 = saturate(gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p00.x, footprint.p00.y, z)].a);
		const float t10 = saturate(gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p10.x, footprint.p10.y, z)].a);
		const float t01 = saturate(gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p01.x, footprint.p01.y, z)].a);
		const float t11 = saturate(gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p11.x, footprint.p11.y, z)].a);
		const float transmittance = lerp(lerp(t00, t10, footprint.blend.x),
			lerp(t01, t11, footprint.blend.x), footprint.blend.y);
		return min(-log(max(transmittance, 1e-7)), 16.0);
	}
	const float tau00 = min(-log(max(gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p00.x, footprint.p00.y, z)].a, 1e-7)), 16.0);
	const float tau10 = min(-log(max(gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p10.x, footprint.p10.y, z)].a, 1e-7)), 16.0);
	const float tau01 = min(-log(max(gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p01.x, footprint.p01.y, z)].a, 1e-7)), 16.0);
	const float tau11 = min(-log(max(gSmokeFroxelIntegrated[SmokeFroxelIndex(footprint.p11.x, footprint.p11.y, z)].a, 1e-7)), 16.0);
	const float row0 = lerp(tau00, tau10, footprint.blend.x);
	const float row1 = lerp(tau01, tau11, footprint.blend.x);
	return lerp(row0, row1, footprint.blend.y);
}

float4 SmokeResolveColumn(uint2 column, uint depthSlice, float viewDepth)
{
	const uint index = SmokeFroxelIndex(column.x, column.y, depthSlice);
	const float4 prefix = depthSlice == 0u ? float4(0.0, 0.0, 0.0, 1.0) :
		gSmokeFroxelIntegrated[SmokeFroxelIndex(column.x, column.y, depthSlice - 1u)];
	const float4 localMedium = gSmokeFroxelMedium[index];
	const float3 localSource = gSmokeFroxelSource[index].rgb;
	const float sliceNearDepth = SmokeSliceNearDepth(depthSlice);
	const float sliceFarDepth = SmokeSliceFarDepth(depthSlice);
	const float partialDepth = clamp(viewDepth - sliceNearDepth, 0.0, sliceFarDepth - sliceNearDepth);
	const float partialLength = SmokeWorldSegmentLength(SmokeFroxelRay(column), sliceNearDepth, sliceNearDepth + partialDepth);
	const float extinction = max(localMedium.a, 0.0);
	const float segmentTransmittance = exp(-extinction * partialLength);
	const float scatterIntegral = extinction > 0.000001 ? (1.0 - segmentTransmittance) / extinction : partialLength;
	const float3 radiance = prefix.rgb + saturate(prefix.a) * max(localSource, 0.0) * scatterIntegral;
	const float transmittance = saturate(prefix.a) * segmentTransmittance;
	// Keep transmittance linear through the positive spatial reconstruction.
	// Converting to tau before interpolation would make partial coverage denser.
	return float4(max(radiance, 0.0), SmokeTransientCoverageEnabled() ? saturate(transmittance) :
		min(-log(max(transmittance, 1e-7)), 16.0));
}

float SmokeRepresentativeDepth(SmokeBilinearFootprint footprint, uint terminalSlice, float terminalDepth, float terminalTau)
{
	if (terminalTau <= 1e-5)
		return 0.0;
	const float targetTau = terminalTau * 0.5;
	uint low = 0u;
	uint high = terminalSlice;
	[unroll]
	for (uint step = 0u; step < 7u && low < high; ++step)
	{
		const uint mid = (low + high) >> 1u;
		const float tau = SmokeBilinearIntegratedTau(footprint, mid);
		if (tau < targetTau)
			low = mid + 1u;
		else
			high = mid;
	}
	const float nearDepth = SmokeSliceNearDepth(low);
	const float farDepth = min(SmokeSliceFarDepth(low), terminalDepth);
	return clamp((nearDepth + farDepth) * 0.5, 0.0, terminalDepth);
}

uint SmokeResolveCarrierMetadata(SmokeBilinearFootprint footprint, uint depthSlice)
{
	const uint4 metadata = uint4(
		SmokeFroxelMetadata(gSmokeFroxelSource[SmokeFroxelIndex(footprint.p00.x, footprint.p00.y, depthSlice)].w),
		SmokeFroxelMetadata(gSmokeFroxelSource[SmokeFroxelIndex(footprint.p10.x, footprint.p10.y, depthSlice)].w),
		SmokeFroxelMetadata(gSmokeFroxelSource[SmokeFroxelIndex(footprint.p01.x, footprint.p01.y, depthSlice)].w),
		SmokeFroxelMetadata(gSmokeFroxelSource[SmokeFroxelIndex(footprint.p11.x, footprint.p11.y, depthSlice)].w));
	uint resolved = metadata.x;
	[unroll] for (uint i = 1u; i < 4u; ++i)
	{
		if (!SmokeFroxelCarrierValid(resolved) && SmokeFroxelCarrierValid(metadata[i]))
			resolved = metadata[i];
		else if (SmokeFroxelRadianceAge(metadata[i]) > SmokeFroxelRadianceAge(resolved))
			resolved = metadata[i];
		if (SmokeFroxelRadianceUnresolved(metadata[i]))
			resolved |= NRI_SMOKE_RADIANCE_UNRESOLVED;
	}
	return resolved;
}

float3 SmokeCarrierDebugColor(uint metadata, uint debugMode)
{
	if (debugMode == 8u)
	{
		const float age = (float)SmokeFroxelCarrierAge(metadata) / 15.0;
		return SmokeFroxelCarrierValid(metadata) ? lerp(float3(0.0, 1.0, 0.0), float3(1.0, 0.0, 0.0), age) : 0.0;
	}
	if (debugMode == 9u)
	{
		const float confidence = SmokeFroxelRadianceValid(metadata) ?
			1.0 - (float)SmokeFroxelRadianceAge(metadata) / 63.0 : 0.0;
		return float3(1.0 - confidence, confidence, 0.0);
	}
	if (debugMode == 10u)
	{
		const uint fallback = SmokeFroxelFallbackType(metadata);
		const float3 palette[6] = {
			float3(0.0, 0.0, 0.0), float3(0.2, 0.5, 1.0), float3(1.0, 0.8, 0.1),
			float3(1.0, 0.2, 0.8), float3(0.2, 1.0, 1.0), float3(0.4, 1.0, 0.2) };
		return palette[min(fallback, 5u)];
	}
	return SmokeFroxelRadianceUnresolved(metadata) ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint2 dimensions = uint2(gSmokeConstants.RenderWidth, gSmokeConstants.RenderHeight);
	if (dispatchThreadId.x >= dimensions.x || dispatchThreadId.y >= dimensions.y ||
		gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.FroxelDepth == 0u)
		return;
	const uint2 pixel = dispatchThreadId.xy;
	const float4 viewZSample = gSmokeViewZInput.Load(int3(pixel, 0));
	float viewDepth;
	if (viewZSample.y > 0.5 || !SmokeDecodeViewDepth(viewZSample.x, viewDepth))
	{
		gSmokeVolumeCurrentOutput[pixel] = 0.0;
		gSmokeVolumeCurrentMetaOutput[pixel] = 0.0;
		return;
	}
	const float2 primarySampleUv = SmokePrimarySampleUv(pixel);
	const SmokeBilinearFootprint footprint = SmokeMakeBilinearFootprint(primarySampleUv);
	const uint depthSlice = SmokeDepthSlice(viewDepth);
	const float4 v00 = SmokeResolveColumn(footprint.p00, depthSlice, viewDepth);
	const float4 v10 = SmokeResolveColumn(footprint.p10, depthSlice, viewDepth);
	const float4 v01 = SmokeResolveColumn(footprint.p01, depthSlice, viewDepth);
	const float4 v11 = SmokeResolveColumn(footprint.p11, depthSlice, viewDepth);
	const float4 row0 = lerp(v00, v10, footprint.blend.x);
	const float4 row1 = lerp(v01, v11, footprint.blend.x);
	float4 volume = lerp(row0, row1, footprint.blend.y);
	if (SmokeTransientCoverageEnabled())
		volume.a = -log(max(volume.a, 1e-7));
	volume.a = clamp(volume.a, 0.0, 16.0);
	if (!all(isfinite(volume)) || volume.a <= 1e-6)
		volume = 0.0;
	const uint debugMode = SmokeDebugMode(gSmokeConstants.DebugMode);
	if (volume.a > 0.0 && debugMode >= 8u && debugMode <= 11u)
	{
		const uint carrierMetadata = SmokeResolveCarrierMetadata(footprint, depthSlice);
		volume.rgb = SmokeCarrierDebugColor(carrierMetadata, debugMode) * (1.0 - exp(-volume.a));
	}
	const float reactive = 1.0 - exp(-volume.a);
	float representativeDepth = SmokeRepresentativeDepth(footprint, depthSlice, viewDepth, volume.a);
	float historyConfidence = SmokeTransientHistoryEnabled() ? 0.0 : 1.0;
	if (SmokeTransientHistoryEnabled() && volume.a > 0.0 && representativeDepth > 0.0)
	{
		const SmokeTransientHistoryMotion motion = SmokeTransientResolveHistoryMotion(primarySampleUv,
			SmokeDepthSlice(representativeDepth));
		if (motion.Confidence > 0.0)
		{
			representativeDepth = min(motion.Depth, viewDepth);
			historyConfidence = motion.Confidence;
		}
	}
	gSmokeVolumeCurrentOutput[pixel] = volume;
	gSmokeVolumeCurrentMetaOutput[pixel] = volume.a > 0.0 ? float4(
		saturate(reactive), representativeDepth / max(gSmokeConstants.FroxelMaxDistance, 0.001),
		viewDepth / max(gSmokeConstants.FroxelMaxDistance, 0.001), historyConfidence) : 0.0;
}
