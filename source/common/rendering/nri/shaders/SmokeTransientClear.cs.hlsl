#include "Include/SmokeResources.hlsli"
#include "Include/SmokeTransientData.hlsli"
#include "Include/SmokeTransientHistory.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	uint binCount, binStride;
	uint mediumCount, mediumStride;
	uint headerCount, headerStride;
	gSmokeTransientBinHeaders.GetDimensions(binCount, binStride);
	gSmokeTransientFroxelMedium.GetDimensions(mediumCount, mediumStride);
	gSmokeTransientLightHeaders.GetDimensions(headerCount, headerStride);
	if (index < binCount)
		gSmokeTransientBinHeaders[index] = (SmokeTransientBinHeader)0;
	if (index < mediumCount)
		gSmokeTransientFroxelMedium[index] = 0.0;
	if (SmokeTransientHistoryEnabled())
	{
		uint motionCount, motionStride;
		gSmokeTransientFroxelMotion.GetDimensions(motionCount, motionStride);
		if (index < motionCount) gSmokeTransientFroxelMotion[index] = 0.0;
	}
	if ((gSmokeConstants.Flags & 1u) != 0u && index < headerCount)
	{
		SmokeTransientLightHeader header = (SmokeTransientLightHeader)0;
		header.GroupSlot = 0xffffffffu;
		header.ObservedFrame = 0xffffffffu;
		gSmokeTransientLightHeaders[index] = header;
	}

	if (index == 0u)
	{
		gSmokeControl[0].TransientBinsTouched = 0u;
		gSmokeControl[0].TransientBinCandidates = 0u;
		gSmokeControl[0].TransientBinOverflow = 0u;
		gSmokeControl[0].TransientLightBuildGroups = 0u;
		gSmokeControl[0].TransientLightFullBuildClaims = 0u;
		gSmokeControl[0].TransientLightFullBuilds = 0u;
		gSmokeControl[0].TransientLightFallbackBuilds = 0u;
		gSmokeControl[0].TransientLightAnchorsWritten = 0u;
		gSmokeControl[0].TransientLightPublishedFull = 0u;
		gSmokeControl[0].TransientLightPublishedFallback = 0u;
		gSmokeControl[0].TransientLightPointCandidatesTested = 0u;
		gSmokeControl[0].TransientLightPointSelected = 0u;
		gSmokeControl[0].TransientLightDirectionalSamples = 0u;
		gSmokeControl[0].TransientLightEmissiveSamples = 0u;
		gSmokeControl[0].TransientLightVisibilityRays = 0u;
		gSmokeControl[0].TransientLightSelfTransmittanceTests = 0u;
		gSmokeControl[0].TransientLightObservedGroups = 0u;
		gSmokeControl[0].TransientLightObservedFull = 0u;
		gSmokeControl[0].TransientLightObservedFallback = 0u;
		gSmokeControl[0].TransientLightMissing = 0u;
		gSmokeControl[0].TransientLightIdentityRejects = 0u;
		gSmokeControl[0].TransientMaterializeFroxelsTested = 0u;
		gSmokeControl[0].TransientMaterializeFroxelsApplied = 0u;
		gSmokeControl[0].TransientMaterializeLobeTests = 0u;
		gSmokeControl[0].TransientMaterializeLobeContributions = 0u;
		gSmokeControl[0].TransientLightApplyVisibilityRays = 0u;
	}
}
