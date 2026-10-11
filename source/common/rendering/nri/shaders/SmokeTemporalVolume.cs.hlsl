#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeIndirectCache.hlsli"
#include "Include/SmokeTransientHistory.hlsli"

#define NRI_SMOKE_VOLUME_HISTORY_VALID 0x1000u
#define NRI_SMOKE_VOLUME_HISTORY_ENABLED 0x2000u

float3 SmokeNormalizedVolumeRadiance(float4 volume)
{
	return max(volume.rgb, 0.0) / max(1.0 - exp(-max(volume.a, 0.0)), 1e-5);
}

bool SmokeLoadTransientHistory(float2 previousUv, float expectedSmokeDepth,
	float expectedOpaqueDepth, out float4 filtered, out float age)
{
	filtered = 0.0;
	age = 0.0;
	const uint2 dimensions = uint2(gSmokeConstants.RenderWidth, gSmokeConstants.RenderHeight);
	const float2 coordinate = previousUv * float2(dimensions) - 0.5;
	const int2 base = int2(floor(coordinate));
	const float2 blend = frac(coordinate);
	const uint slice = SmokeDepthSlice(min(expectedSmokeDepth, gSmokeConstants.FroxelMaxDistance));
	const float sliceWidth = SmokeSliceFarDepth(slice) - SmokeSliceNearDepth(slice);
	const float smokeTolerance = max(2.0, expectedSmokeDepth * 0.015 + sliceWidth * 0.1);
	const float opaqueTolerance = max(4.0, expectedOpaqueDepth * 0.02);
	float acceptedWeight = 0.0;
	[unroll]
	for (uint tap = 0u; tap < 4u; ++tap)
	{
		const int2 previousPixel = clamp(base + int2(tap & 1u, tap >> 1u), int2(0, 0), int2(dimensions) - 1);
		const float weight = ((tap & 1u) != 0u ? blend.x : 1.0 - blend.x) *
			((tap & 2u) != 0u ? blend.y : 1.0 - blend.y);
		const float4 history = gSmokeVolumeHistoryInput.Load(int3(previousPixel, 0));
		const float4 metadata = gSmokeVolumeMetaInput.Load(int3(previousPixel, 0));
		// The separate age tag prevents borrowing an old radiance-only/Grid
		// history record when a transient first becomes motion-trackable.
		if (!all(isfinite(history)) || !all(isfinite(metadata)) || metadata.w <= 2.0 ||
			metadata.w > 3.01 || history.a <= 1e-6 || metadata.x <= 1e-6) continue;
		const float smokeDepth = metadata.y * gSmokeConstants.FroxelMaxDistance;
		const float opaqueDepth = metadata.z * gSmokeConstants.FroxelMaxDistance;
		if (abs(smokeDepth - expectedSmokeDepth) > smokeTolerance ||
			abs(opaqueDepth - expectedOpaqueDepth) > opaqueTolerance ||
			opaqueDepth + max(2.0, opaqueDepth * 0.001) < expectedSmokeDepth) continue;
		filtered += float4(max(history.rgb, 0.0), SmokeTransientHistoryOpacity(history.a)) * weight;
		age += saturate(metadata.w - 2.0) * weight;
		acceptedWeight += weight;
	}
	if (acceptedWeight < 0.5) return false;
	filtered /= acceptedWeight;
	age /= acceptedWeight;
	return all(isfinite(filtered));
}

void SmokeTransientCurrentBounds(uint2 pixel, float4 current, float4 currentMeta,
	out float4 minimumValue, out float4 maximumValue)
{
	minimumValue = maximumValue = float4(max(current.rgb, 0.0), SmokeTransientHistoryOpacity(current.a));
	const int2 dimensions = int2(gSmokeConstants.RenderWidth, gSmokeConstants.RenderHeight);
	// These taps bound accepted HISTORY, never blur the current image. A few
	// pixels across the coarse froxel footprint allow sub-froxel shimmer to be
	// corrected without an unbounded neighborhood or propagation across walls.
	const int2 stride = clamp(int2(ceil(float2(dimensions) /
		float2(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight) * 0.25)), int2(1, 1), int2(6, 6));
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			const int2 neighborPixel = clamp(int2(pixel) + int2(x, y) * stride, int2(0, 0), dimensions - 1);
			const float4 neighbor = gSmokeVolumeCurrentInput.Load(int3(neighborPixel, 0));
			if (!all(isfinite(neighbor))) continue;
			float4 value = 0.0;
			if (neighbor.a > 1e-6)
			{
				const float4 metadata = gSmokeVolumeCurrentMetaInput.Load(int3(neighborPixel, 0));
				if (!all(isfinite(metadata)) || abs(metadata.z - currentMeta.z) > max(0.001, currentMeta.z * 0.02) ||
					abs(metadata.y - currentMeta.y) > max(0.001, currentMeta.y * 0.03)) continue;
				value = float4(max(neighbor.rgb, 0.0), SmokeTransientHistoryOpacity(neighbor.a));
			}
			minimumValue = min(minimumValue, value);
			maximumValue = max(maximumValue, value);
		}
	}
}

void SmokeResolveTransientHistory(uint2 pixel, float4 current, float4 currentMeta)
{
	const float currentOpacity = SmokeTransientHistoryOpacity(current.a);
	float4 resolved = float4(max(current.rgb, 0.0), currentOpacity);
	float age = 0.125;
	bool accepted = (gSmokeConstants.Flags & NRI_SMOKE_VOLUME_HISTORY_VALID) != 0u;
	const float2 stableUv = SmokePrimarySampleUv(pixel);
	const float representativeDepth = currentMeta.y * gSmokeConstants.FroxelMaxDistance;
	const SmokeTransientHistoryMotion motion = SmokeTransientResolveHistoryMotion(stableUv, SmokeDepthSlice(representativeDepth));
	const float confidence = min(saturate(currentMeta.w), motion.Confidence);
	accepted = accepted && confidence > 0.1;
	if (accepted)
	{
		const float3 worldPosition = SmokeWorldPosition(stableUv, representativeDepth);
		const float3 previousWorldPosition = worldPosition + motion.Displacement;
		float2 previousStableUv, previousStorageUv;
		accepted = SmokePreviousUv(previousWorldPosition, previousStableUv, previousStorageUv);
		if (accepted)
		{
			const SmokeReprojectionData reprojection = gSmokeReprojectionData[0];
			const float expectedSmokeDepth = -SmokeMultiplyMatrixPoint(float4(previousWorldPosition, 1.0),
				reprojection.previousWorldToViewMatrix).z;
			const float opaqueDepth = currentMeta.z * gSmokeConstants.FroxelMaxDistance;
			const float3 opaqueWorldPosition = SmokeWorldPosition(stableUv, opaqueDepth);
			const float expectedOpaqueDepth = currentMeta.z >= 0.999 ? gSmokeConstants.FroxelMaxDistance :
				-SmokeMultiplyMatrixPoint(float4(opaqueWorldPosition, 1.0), reprojection.previousWorldToViewMatrix).z;
			float4 history;
			float historyAge;
			accepted = isfinite(expectedSmokeDepth) && isfinite(expectedOpaqueDepth) &&
				expectedOpaqueDepth > 0.0 && expectedSmokeDepth > 0.0 &&
				SmokeLoadTransientHistory(previousStorageUv, expectedSmokeDepth, expectedOpaqueDepth, history, historyAge);
			if (accepted)
			{
				float4 minimumValue, maximumValue;
				SmokeTransientCurrentBounds(pixel, current, currentMeta, minimumValue, maximumValue);
				history = clamp(history, minimumValue, maximumValue);
				const float screenMotion = length((previousStableUv - stableUv) *
					float2(gSmokeConstants.RenderWidth, gSmokeConstants.RenderHeight));
				const float motionTrust = 1.0 - smoothstep(32.0, 96.0, screenMotion);
				const float currentLuma = dot(resolved.rgb, float3(0.2126, 0.7152, 0.0722));
				const float historyLuma = dot(history.rgb, float3(0.2126, 0.7152, 0.0722));
				const float radianceChange = abs(currentLuma - historyLuma) / max(max(currentLuma, historyLuma), 0.05);
				const float radianceTrust = 1.0 - smoothstep(0.35, 0.85, radianceChange);
				const float opacityTrust = 1.0 - smoothstep(0.20, 0.50, abs(history.a - currentOpacity));
				const float historyWeight = min(historyAge + 0.125, 0.65) * confidence *
					motionTrust * radianceTrust * opacityTrust;
				// Both channels are linear coverage quantities: opacity (=1-T) and
				// premultiplied source radiance. No optical-depth average or density
				// retuning, and no reconstruction outside current analytic support.
				resolved = lerp(resolved, history, historyWeight);
				age = min(historyAge + 0.125, 1.0);
			}
		}
	}
	gSmokeVolumeHistoryOutput[pixel] = float4(max(resolved.rgb, 0.0), SmokeTransientHistoryTau(resolved.a));
	gSmokeVolumeMetaOutput[pixel] = float4(saturate(resolved.a), currentMeta.yz, 2.0 + age);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint2 dimensions = uint2(gSmokeConstants.RenderWidth, gSmokeConstants.RenderHeight);
	if (dispatchThreadId.x >= dimensions.x || dispatchThreadId.y >= dimensions.y)
		return;
	const uint2 pixel = dispatchThreadId.xy;
	const float4 current = gSmokeVolumeCurrentInput.Load(int3(pixel, 0));
	const float4 currentMeta = gSmokeVolumeCurrentMetaInput.Load(int3(pixel, 0));
	if (!all(isfinite(current)) || current.a <= 1e-6 || currentMeta.x <= 1e-6)
	{
		gSmokeVolumeHistoryOutput[pixel] = 0.0;
		gSmokeVolumeMetaOutput[pixel] = 0.0;
		return;
	}
	if (SmokeTransientHistoryEnabled() && currentMeta.w > 0.1)
	{
		SmokeResolveTransientHistory(pixel, current, currentMeta);
		return;
	}

	bool accepted = (gSmokeConstants.Flags & (NRI_SMOKE_VOLUME_HISTORY_VALID | NRI_SMOKE_VOLUME_HISTORY_ENABLED)) ==
		(NRI_SMOKE_VOLUME_HISTORY_VALID | NRI_SMOKE_VOLUME_HISTORY_ENABLED);
	float4 history = current;
	float4 historyMeta = 0.0;
	if (accepted)
	{
		const float2 stableUv = SmokePrimarySampleUv(pixel);
		const float representativeDepth = currentMeta.y * gSmokeConstants.FroxelMaxDistance;
		const float3 worldPosition = SmokeWorldPosition(stableUv, representativeDepth);
		float2 previousStableUv, previousStorageUv;
		accepted = SmokePreviousUv(worldPosition, previousStableUv, previousStorageUv);
		if (accepted)
		{
			const uint2 previousPixel = min((uint2)(previousStorageUv * float2(dimensions)), dimensions - 1u);
			history = gSmokeVolumeHistoryInput.Load(int3(previousPixel, 0));
			historyMeta = gSmokeVolumeMetaInput.Load(int3(previousPixel, 0));
			accepted = all(isfinite(history)) && all(isfinite(historyMeta)) && history.a > 0.0 && historyMeta.x > 0.0 &&
				abs(historyMeta.x - currentMeta.x) <= 0.35 &&
				abs(historyMeta.y - currentMeta.y) <= 2.0 / max((float)gSmokeConstants.FroxelDepth, 1.0) &&
				abs(historyMeta.z - currentMeta.z) <= 0.05;
		}
	}

	float historyWeight = 0.0;
	if (accepted)
	{
		float3 minimumNormalized = SmokeNormalizedVolumeRadiance(current);
		float3 maximumNormalized = minimumNormalized;
		[unroll]
		for (int y = -1; y <= 1; ++y)
		{
			[unroll]
			for (int x = -1; x <= 1; ++x)
			{
				const int2 neighborPixel = clamp(int2(pixel) + int2(x, y), int2(0, 0), int2(dimensions) - 1);
				const float4 neighbor = gSmokeVolumeCurrentInput.Load(int3(neighborPixel, 0));
				if (neighbor.a <= 1e-6 || !all(isfinite(neighbor)))
					continue;
				const float3 normalized = SmokeNormalizedVolumeRadiance(neighbor);
				minimumNormalized = min(minimumNormalized, normalized);
				maximumNormalized = max(maximumNormalized, normalized);
			}
		}
		const float3 clampedNormalized = clamp(SmokeNormalizedVolumeRadiance(history), minimumNormalized, maximumNormalized);
		// Current optical depth owns the smoke footprint. History reconstructs
		// incident radiance only and cannot make a fringe denser than this frame.
		history = float4(clampedNormalized * (1.0 - exp(-current.a)), current.a);
		historyWeight = min(max(historyMeta.w, 0.125), 0.75);
	}
	float4 resolved = lerp(current, history, historyWeight);
	resolved.a = current.a;
	const float age = accepted ? min(historyMeta.w + 0.125, 1.0) : 0.125;
	gSmokeVolumeHistoryOutput[pixel] = resolved;
	gSmokeVolumeMetaOutput[pixel] = float4(currentMeta.xyz, age);
}
