#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeTransientData.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint groupCapacity, groupStride;
	gSmokeTransientGroups.GetDimensions(groupCapacity, groupStride);
	const uint groupIndex = dispatchThreadId.x;
	if (groupIndex >= min(NRI_SMOKE_TRANSIENT_GROUP_COUNT,
		min(groupCapacity, NRI_SMOKE_TRANSIENT_MAX_GROUPS)))
		return;
	const SmokeTransientGroup group = gSmokeTransientGroups[groupIndex];
	if ((group.Flags & NRI_SMOKE_TRANSIENT_GROUP_ACTIVE) == 0u ||
		group.Epoch != gSmokeConstants.SimulationEpoch || group.LobeCount == 0u ||
		!all(isfinite(group.BoundsMin)) || !all(isfinite(group.BoundsMax)) ||
		any(group.BoundsMax <= group.BoundsMin))
		return;

	const float3 center = (group.BoundsMin + group.BoundsMax) * 0.5;
	const float3 extent = (group.BoundsMax - group.BoundsMin) * 0.5;
	const float projectionRadius = length(extent);
	int2 minimumColumn, maximumColumn;
	if (!SmokeProjectSphereToFroxelBounds(center, projectionRadius,
		minimumColumn, maximumColumn))
		return;
	if (SmokeTransientCoverageEnabled())
	{
		// The materializer considers area support, including cell-edge overlap.
		// A guard column also covers conservative local-plane projection roundoff.
		minimumColumn = max(minimumColumn - 1, int2(0, 0));
		maximumColumn = min(maximumColumn + 1,
			int2(gSmokeConstants.FroxelWidth - 1u, gSmokeConstants.FroxelHeight - 1u));
	}

	const float centerViewDepth = dot(center - gSmokeConstants.CameraPosition,
		gSmokeConstants.CameraForward);
	const float depthExtent = dot(extent, abs(gSmokeConstants.CameraForward));
	const float minimumDepth = max(centerViewDepth - depthExtent, 0.0);
	const float maximumDepth = min(centerViewDepth + depthExtent,
		gSmokeConstants.FroxelMaxDistance);
	if (maximumDepth <= 0.0 || maximumDepth < minimumDepth)
		return;
	const uint minimumSlice = SmokeDepthSlice(minimumDepth);
	const uint maximumSlice = SmokeDepthSlice(maximumDepth);
	const uint3 binCount = SmokeTransientBinCount(uint3(gSmokeConstants.FroxelWidth,
		gSmokeConstants.FroxelHeight, gSmokeConstants.FroxelDepth));
	if (any(binCount == 0u))
		return;
	const uint3 minimumBin = uint3((uint2)minimumColumn /
		NRI_SMOKE_TRANSIENT_BIN_SIZE_XY, minimumSlice / NRI_SMOKE_TRANSIENT_BIN_SIZE_Z);
	const uint3 maximumBin = min(uint3((uint2)maximumColumn /
		NRI_SMOKE_TRANSIENT_BIN_SIZE_XY, maximumSlice / NRI_SMOKE_TRANSIENT_BIN_SIZE_Z),
		binCount - 1u);
	uint headerCapacity, headerStride;
	uint indexCapacity, indexStride;
	gSmokeTransientBinHeaders.GetDimensions(headerCapacity, headerStride);
	gSmokeTransientBinIndices.GetDimensions(indexCapacity, indexStride);
	[loop]
	for (uint z = minimumBin.z; z <= maximumBin.z; ++z)
	{
		[loop]
		for (uint y = minimumBin.y; y <= maximumBin.y; ++y)
		{
			[loop]
			for (uint x = minimumBin.x; x <= maximumBin.x; ++x)
			{
				const uint binIndex = SmokeTransientBinIndex(uint3(x, y, z), binCount);
				if (binIndex >= headerCapacity)
					continue;
				uint slot;
				InterlockedAdd(gSmokeTransientBinHeaders[binIndex].Count, 1u, slot);
				if (slot < NRI_SMOKE_TRANSIENT_MAX_GROUPS_PER_BIN)
				{
					const uint outputIndex = binIndex * NRI_SMOKE_TRANSIENT_MAX_GROUPS_PER_BIN + slot;
					if (outputIndex < indexCapacity)
					{
						gSmokeTransientBinIndices[outputIndex] = groupIndex;
						InterlockedAdd(gSmokeControl[0].TransientBinCandidates, 1u);
					}
					else
					{
						InterlockedAdd(gSmokeTransientBinHeaders[binIndex].Overflow, 1u);
						InterlockedAdd(gSmokeControl[0].TransientBinOverflow, 1u);
					}
				}
				else
				{
					InterlockedAdd(gSmokeTransientBinHeaders[binIndex].Overflow, 1u);
					InterlockedAdd(gSmokeControl[0].TransientBinOverflow, 1u);
				}
				InterlockedAdd(gSmokeControl[0].TransientBinsTouched, 1u);
			}
		}
	}
}
