#include "Include/SmokeEmissiveReservoir.hlsli"

static const float3 NRI_SMOKE_ANALYTIC_LIGHT_LOBE_AXES[6] = {
	float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0),
	float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0),
	float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0)
};

float SmokeAnalyticEmissiveRectangleKernel(SmokeAnalyticCarrier carrier,
	float3 ray, float nearDepth, float farDepth)
{
	float integral = 0.0;
	[unroll]
	for (uint sampleIndex = 0u; sampleIndex < 4u; ++sampleIndex)
	{
		const float depth = lerp(nearDepth, farDepth, ((float)sampleIndex + 0.5) * 0.25);
		const float3 position = gSmokeConstants.CameraPosition + ray * depth;
		const float3 closest = SmokeInjectionClosestRectanglePoint(position, carrier.Position,
			carrier.HalfAxisU, carrier.HalfAxisV);
		const float normalized = saturate(1.0 - distance(position, closest) /
			max(carrier.Radius, 0.001));
		integral += normalized * normalized * (3.0 - 2.0 * normalized);
	}
	return integral * 0.25;
}

bool SmokeLoadAnalyticLightAnchor(SmokeAnalyticCarrier carrier, uint anchorIndex,
	out SmokeAnalyticEmissiveStorageRecord record, out bool missing, out bool identityRejected)
{
	record = (SmokeAnalyticEmissiveStorageRecord)0;
	missing = false;
	identityRejected = false;
	const uint bankIndex = carrier.LightGroupSlot * NRI_SMOKE_ANALYTIC_LIGHT_ANCHORS_PER_BANK +
		(anchorIndex % NRI_SMOKE_ANALYTIC_LIGHT_ANCHORS_PER_BANK);
	uint capacity, stride;
	if (anchorIndex < NRI_SMOKE_ANALYTIC_LIGHT_ANCHORS_PER_BANK)
	{
		gSmokeAnalyticEmissiveCurrent.GetDimensions(capacity, stride);
		if (bankIndex >= capacity) { missing = true; return false; }
		record = gSmokeAnalyticEmissiveCurrent[bankIndex];
	}
	else
	{
		gSmokeAnalyticEmissiveHistory.GetDimensions(capacity, stride);
		if (bankIndex >= capacity) { missing = true; return false; }
		record = gSmokeAnalyticEmissiveHistory[bankIndex];
	}
	const bool matches = SmokeAnalyticLightIdentityMatches(record, carrier.LightGroupSlot,
		carrier.LightGroupGeneration, carrier.Epoch, anchorIndex);
	identityRejected = !matches;
	return matches;
}

float3 SmokeEvaluateAnalyticLightField(SmokeAnalyticCarrier carrier,
	float3 receiverPosition, float3 viewRay, float anisotropy,
	out uint blendTaps, out uint missingRecords, out uint identityRejects)
{
	float3 source = 0.0;
	float weightSum = 0.0;
	blendTaps = 0u;
	missingRecords = 0u;
	identityRejects = 0u;
	const uint anchorCount = min(carrier.LightAnchorCount, 4u);
	[loop]
	for (uint anchorIndex = 0u; anchorIndex < anchorCount; ++anchorIndex)
	{
		SmokeAnalyticEmissiveStorageRecord record;
		bool missing, identityRejected;
		if (!SmokeLoadAnalyticLightAnchor(carrier, anchorIndex, record,
			missing, identityRejected))
		{
			missingRecords += missing ? 1u : 0u;
			identityRejects += identityRejected ? 1u : 0u;
			continue;
		}
		blendTaps++;
		const float3 offset = receiverPosition - SmokeAnalyticLightAnchorPosition(record);
		const float weight = rcp(max(dot(offset, offset), 1.0));
		float3 anchorSource = 0.0;
		[unroll]
		for (uint lobe = 0u; lobe < 6u; ++lobe)
			anchorSource += SmokeAnalyticLightLobe(record, lobe) * SmokePhaseResponse(
				dot(NRI_SMOKE_ANALYTIC_LIGHT_LOBE_AXES[lobe], viewRay), anisotropy);
		source += anchorSource * weight;
		weightSum += weight;
	}
	return weightSum > 0.0 ? source / weightSum : 0.0;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint controlCount, occupiedCapacity, phaseCapacity, sourceCapacity,
		analyticCapacity, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCapacity, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceCapacity, ignoredStride);
	gSmokeAnalyticFroxelMedium.GetDimensions(analyticCapacity, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >=
		min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;
	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	if (froxelIndex >= min(min(phaseCapacity, sourceCapacity), analyticCapacity)) return;
	const float4 phase = gSmokeFroxelPhase[froxelIndex];
	if (!SmokeAnalyticCarrierReservoirOwns(phase)) return;
	const float4 analyticMedium = gSmokeAnalyticFroxelMedium[froxelIndex];
	if (analyticMedium.a <= 0.0 || !any(analyticMedium.rgb > 0.0)) return;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	if (diagnostics) InterlockedAdd(gSmokeControl[0].AnalyticLightApplyFroxelsTested, 1u);

	const uint3 froxel = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxel.xy);
	const float nearDepth = SmokeSliceNearDepth(froxel.z);
	const float farDepth = SmokeSliceFarDepth(froxel.z);
	const float3 receiverPosition = SmokeFroxelCenter(froxel, ray);
	const uint2 tileCount = SmokeAnalyticTileCount(
		gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight);
	const uint tileIndex = SmokeAnalyticTileIndex(
		froxel.xy / NRI_SMOKE_ANALYTIC_TILE_SIZE, tileCount);
	uint headerCapacity, headerStride;
	gSmokeAnalyticTileHeaders.GetDimensions(headerCapacity, headerStride);
	if (tileIndex >= headerCapacity) return;
	const uint candidateCount = min(gSmokeAnalyticTileHeaders[tileIndex].Count,
		NRI_SMOKE_ANALYTIC_MAX_CARRIERS_PER_TILE);
	uint carrierCapacity, carrierStride, indexCapacity, indexStride;
	gSmokeAnalyticCarriers.GetDimensions(carrierCapacity, carrierStride);
	gSmokeAnalyticTileIndices.GetDimensions(indexCapacity, indexStride);
	float3 analyticSource = 0.0;
	[loop]
	for (uint candidate = 0u; candidate < candidateCount; ++candidate)
	{
		const uint listIndex = tileIndex * NRI_SMOKE_ANALYTIC_MAX_CARRIERS_PER_TILE + candidate;
		if (listIndex >= indexCapacity) break;
		const uint carrierIndex = gSmokeAnalyticTileIndices[listIndex];
		if (carrierIndex >= min(carrierCapacity, gSmokeConstants.ParticleCapacity)) continue;
		const SmokeAnalyticCarrier carrier = gSmokeAnalyticCarriers[carrierIndex];
		if ((carrier.Flags & NRI_SMOKE_ANALYTIC_CARRIER_ACTIVE) == 0u ||
			carrier.Epoch != gSmokeConstants.SimulationEpoch ||
			carrier.StyleIndex >= gSmokeConstants.StyleCount || carrier.LightAnchorCount == 0u)
			continue;
		const SmokeStyle style = gSmokeStyles[carrier.StyleIndex];
		const float kernel = carrier.Shape == NRI_SMOKE_INJECTION_SHAPE_RECTANGLE
			? SmokeAnalyticEmissiveRectangleKernel(carrier, ray, nearDepth, farDepth)
			: SmokeSphereSegmentKernelAverage(carrier.Position, carrier.Radius,
				ray, nearDepth, farDepth);
		if (kernel <= 0.0) continue;
		const float density = max(style.Density * carrier.DensityScale, 0.0) *
			(float)min(carrier.RangeCount, 256u);
		const float sigmaT = kernel * density * max(style.Extinction, 0.0) *
			gSmokeConstants.DensityScale;
		if (sigmaT <= 1e-6) continue;
		uint blendTaps, missingRecords, identityRejects;
		const float3 incident = SmokeEvaluateAnalyticLightField(carrier,
			receiverPosition, normalize(ray), clamp(style.Anisotropy, -0.95, 0.95),
			blendTaps, missingRecords, identityRejects);
		if (diagnostics)
		{
			InterlockedAdd(gSmokeControl[0].AnalyticLightAnchorBlendTaps, blendTaps);
			InterlockedAdd(gSmokeControl[0].AnalyticLightMissingGroupRecords, missingRecords);
			InterlockedAdd(gSmokeControl[0].AnalyticLightIdentityRejects, identityRejects);
			if (blendTaps > 0u) InterlockedAdd(gSmokeControl[0].AnalyticLightGroupCacheHits, 1u);
			InterlockedAdd(gSmokeControl[0].AnalyticLightCarrierContributions, 1u);
		}
		analyticSource += sigmaT * saturate(style.Albedo) * incident *
			gSmokeConstants.RadianceScale;
	}
	const float luminance = SmokeEmissiveLuminance(analyticSource);
	if (luminance > gSmokeConstants.DeltaTime)
		analyticSource *= gSmokeConstants.DeltaTime / luminance;
	if (!any(analyticSource > 0.0)) return;
	if (diagnostics) InterlockedAdd(gSmokeControl[0].AnalyticLightApplyFroxelsApplied, 1u);
	uint metadata = SmokeFroxelMetadata(gSmokeFroxelSource[froxelIndex].w);
	metadata = SmokeFroxelResolveRadiance(metadata, gSmokeConstants.SimulationEpoch,
		NRI_SMOKE_FALLBACK_EMISSIVE, 0u);
	gSmokeFroxelSource[froxelIndex] = float4(
		gSmokeFroxelSource[froxelIndex].rgb + analyticSource,
		SmokeFroxelMetadataValue(metadata));
}
