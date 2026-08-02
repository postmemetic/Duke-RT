#include "Include/SmokeGridLightingResources.hlsli"
#include "Include/SmokePhase.hlsli"

float3 SmokeGridScatterLoadIterationBounce(uint probeIndex, uint iteration)
{
	return iteration == 0u ? 0.0 :
		((iteration & 1u) != 0u ? max(gSmokeGridScatterBounceB[probeIndex].rgb, 0.0) :
			max(gSmokeGridScatterBounceA[probeIndex].rgb, 0.0));
}

void SmokeGridScatterStoreIterationBounce(uint probeIndex, uint iteration, float3 value)
{
	if ((iteration & 1u) == 0u)
		gSmokeGridScatterBounceB[probeIndex] = float4(value, 1.0);
	else
		gSmokeGridScatterBounceA[probeIndex] = float4(value, 1.0);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint iteration = gSmokeConstants.Pass >> 16u;
	if (dispatchThreadId.x == 0u)
	{
		gSmokeGridLightControl[0].ScatterIterations = iteration + 1u;
		gSmokeGridLightControl[0].ScatterFinalPing = (iteration + 1u) & 1u;
	}
	uint activeCapacity, ignoredStride;
	gSmokeGridScatterActive.GetDimensions(activeCapacity, ignoredStride);
	const uint activeCount = min(gSmokeGridLightControl[0].ScatterActiveCount, activeCapacity);
	if (dispatchThreadId.x >= activeCount)
		return;
	const uint probeIndex = gSmokeGridScatterActive[dispatchThreadId.x];
	uint probeCapacity;
	gSmokeGridScatterMetadata.GetDimensions(probeCapacity, ignoredStride);
	if (probeIndex >= probeCapacity)
		return;
	const uint brickIndex = probeIndex / NRI_SMOKE_GRID_SCATTER_PROBES_PER_BRICK;
	uint brickCapacity;
	gSmokeRenderGridBricks.GetDimensions(brickCapacity, ignoredStride);
	if (brickIndex >= brickCapacity)
		return;
	const SmokeGridBrick brick = gSmokeRenderGridBricks[brickIndex];
	const SmokeGridScatterMetadata metadata = gSmokeGridScatterMetadata[probeIndex];
	if (brick.State != NRI_SMOKE_GRID_RESIDENT || !SmokeGridScatterMetadataValid(metadata, brick.Generation))
	{
		SmokeGridScatterStoreIterationBounce(probeIndex, iteration, 0.0);
		InterlockedAdd(gSmokeGridLightControl[0].ScatterNeighborsStale, 1u);
		return;
	}
	const uint3 localProbe = SmokeGridScatterLocalProbe(probeIndex % NRI_SMOKE_GRID_SCATTER_PROBES_PER_BRICK);
	const int3 probe = SmokeGridScatterProbeCoordinate(brick, localProbe);
	float3 sigmaS = 0.0;
	float sigmaT = 0.0;
	float anisotropyNumerator = 0.0;
	float anisotropyDenominator = 0.0;
	[unroll]
	for (uint corner = 0u; corner < 8u; ++corner)
	{
		float4 scalar, optical;
		const int3 offset = int3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
		if (!SmokeGridScatterLoadCell(probe * 2 + offset, scalar, optical))
			continue;
		sigmaS += max(optical.rgb * gSmokeConstants.DensityScale, 0.0) * 0.125;
		sigmaT += max(scalar.z * gSmokeConstants.DensityScale, 0.0) * 0.125;
		anisotropyNumerator += scalar.w * gSmokeConstants.DensityScale * 0.125;
		anisotropyDenominator += max(optical.w * gSmokeConstants.DensityScale, 0.0) * 0.125;
	}
	if (sigmaT <= 1e-6 || !any(sigmaS > 0.0))
	{
		SmokeGridScatterStoreIterationBounce(probeIndex, iteration, 0.0);
		return;
	}
	const float bodyAnisotropy = anisotropyDenominator > 1e-6 ?
		clamp(anisotropyNumerator / anisotropyDenominator, -0.95, 0.95) : 0.0;
	const float effectiveAnisotropy = SmokePhaseEffectiveAnisotropy(bodyAnisotropy);
	const float3 reducedSigmaS = min(sigmaS * clamp(1.0 - effectiveAnisotropy, 0.0, 2.0), sigmaT.xxx);
	const float3 reducedAlbedo = saturate(reducedSigmaS / max(sigmaT, 1e-6));
	const float cellSize = max(asfloat(gSmokeRenderGridControl[0].CellSizeBits), 0.0001);
	const float halfSegment = cellSize;
	float3 incoming = 0.0;
	[unroll]
	for (uint face = 0u; face < 6u; ++face)
	{
		InterlockedAdd(gSmokeGridLightControl[0].ScatterNeighborTests, 1u);
		if (!SmokeGridScatterProbeFaceMetadataOpen(probe, face))
		{
			InterlockedAdd(gSmokeGridLightControl[0].ScatterNeighborsBlocked, 1u);
			continue;
		}
		uint neighborIndex, neighborGeneration;
		const int3 neighborProbe = probe + NRI_SMOKE_GRID_LIGHT_LOBE_AXES[face];
		if (!SmokeGridScatterProbeAddress(neighborProbe, neighborIndex, neighborGeneration))
		{
			InterlockedAdd(gSmokeGridLightControl[0].ScatterNeighborsStale, 1u);
			continue;
		}
		const float4 neighborSeed = gSmokeGridScatterSeed[neighborIndex];
		const float3 neighborSource = max(neighborSeed.rgb, 0.0) +
			SmokeGridScatterLoadIterationBounce(neighborIndex, iteration);
		incoming += neighborSource * exp(-max(neighborSeed.w, 0.0) * halfSegment);
		InterlockedAdd(gSmokeGridLightControl[0].ScatterNeighborsAccepted, 1u);
	}
	incoming *= (1.0 / 6.0);
	const float interaction = 1.0 - exp(-sigmaT * halfSegment);
	float3 bounce = reducedAlbedo * interaction * incoming;
	if (!all(isfinite(bounce)))
	{
		bounce = 0.0;
		InterlockedAdd(gSmokeGridLightControl[0].ScatterNanRejects, 1u);
	}
	SmokeGridScatterStoreIterationBounce(probeIndex, iteration, max(bounce, 0.0));
	InterlockedAdd(gSmokeGridLightControl[0].ScatterReceiverApplications, 1u);
	InterlockedAdd(gSmokeGridLightControl[0].ScatterTransportedEnergyQ,
		(uint)min(dot(max(bounce, 0.0), float3(0.2126, 0.7152, 0.0722)) * 1024.0, 4294967295.0));
}
