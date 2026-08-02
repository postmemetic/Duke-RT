#include "Include/SmokeEmissiveReservoir.hlsli"

void SmokeAccumulateReference(
	uint froxelIndex,
	uint3 froxel,
	float4 medium,
	float anisotropy,
	float3 receiverPosition,
	float3 viewRay,
	bool diagnostics)
{
	const uint sampleCount = 32u;
	float3 estimate = 0.0;
	[loop]
	for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
	{
		uint randomState = SmokeStableEmissiveReferenceSeed(froxel, sampleIndex);
		const uint candidateIndex = SmokeSampleEmissivePrimitive(randomState);
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveReferenceSamples, 1u);
		if (candidateIndex == 0xffffffffu)
			continue;
		const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[candidateIndex];
		SmokeEmissiveReservoirRecord record = SmokeEmptyEmissiveReservoir();
		record.CandidateIndex = candidateIndex;
		record.SampleSeed = randomState;
		record.StableKeyLo = candidate.stableKeyLo;
		record.StableKeyHi = candidate.stableKeyHi;
		record.Generation = gSmokeConstants.CommandCount;
		float3 integrand, lightDirection;
		float distanceToLight;
		if (!SmokeEvaluateEmissiveCandidate(record, receiverPosition, viewRay, anisotropy, diagnostics,
			integrand, lightDirection, distanceToLight))
			continue;
		float visibility = 1.0;
		if (gSmokeConstants.LightMode >= 2u)
		{
			if (diagnostics)
			{
				InterlockedAdd(gSmokeControl[0].EmissiveReferenceRays, 1u);
				InterlockedAdd(gSmokeControl[0].EmissiveShadowRays, 1u);
			}
			visibility = (SmokeFilteredVisibilityEffective()
				? SmokeEmissiveVisibleFiltered(receiverPosition, lightDirection, distanceToLight, diagnostics)
				: SmokeEmissiveVisible(receiverPosition, lightDirection, distanceToLight, diagnostics)) ? 1.0 : 0.0;
		}
		estimate += integrand * visibility / max(candidate.selectionPdf, 1e-6);
	}
	estimate /= (float)sampleCount;
	const float3 sourceContribution = medium.rgb * estimate * gSmokeConstants.RadianceScale;
	uint sourceMetadata = SmokeFroxelMetadata(gSmokeFroxelSource[froxelIndex].w);
	sourceMetadata = SmokeFroxelResolveRadiance(sourceMetadata, gSmokeConstants.SimulationEpoch,
		NRI_SMOKE_FALLBACK_EMISSIVE, 0u);
	gSmokeFroxelSource[froxelIndex] = float4(gSmokeFroxelSource[froxelIndex].rgb + sourceContribution,
		SmokeFroxelMetadataValue(sourceMetadata));
}

void SmokeResolveLegacyEmissive(
	uint froxelIndex,
	uint3 froxel,
	float4 medium,
	float anisotropy,
	float3 receiverPosition,
	float3 viewRay,
	bool diagnostics)
{
	SmokeEmissiveReservoirRecord reservoir = SmokeUnpackEmissiveReservoir(gSmokeEmissiveTemporal[froxelIndex]);
	const uint reuseMode = SmokeEmissiveReuseMode();
	if (reuseMode >= 2u && (gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_LEGACY_GATHER_DISABLED) == 0u && SmokeEmissiveRecordValid(reservoir))
	{
		static const int3 offsets[6] = {
			int3(1, 0, 0), int3(-1, 0, 0), int3(0, 1, 0),
			int3(0, -1, 0), int3(0, 0, 1), int3(0, 0, -1)
		};
		const uint neighborCount = 2u + 2u * min((gSmokeConstants.Flags >> 5u) & 3u, 2u);
		uint randomState = SmokeLightingRandomSeed(froxel, 0u, 0x7e1c83a5u);
		[loop]
		for (uint i = 0u; i < neighborCount; ++i)
		{
			const int3 neighbor = int3(froxel) + offsets[(i + (gSmokeConstants.FrameIndex & 1u) * 3u) % 6u];
			if (any(neighbor < 0) || any(neighbor >= int3(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight, gSmokeConstants.FroxelDepth)))
				continue;
			const uint neighborIndex = SmokeFroxelIndex((uint)neighbor.x, (uint)neighbor.y, (uint)neighbor.z);
			const SmokeEmissiveReservoirRecord candidate = SmokeUnpackEmissiveReservoir(gSmokeEmissiveTemporal[neighborIndex]);
			if (!SmokeEmissiveReservoirCompatible(candidate, medium, anisotropy, gSmokeConstants.FrameIndex,
				receiverPosition, SmokeIndirectWorldTolerance(froxel) * 2.0))
			{
				if (diagnostics)
					InterlockedAdd(gSmokeControl[0].EmissiveSpatialRejected, 1u);
				continue;
			}
			float3 integrand, lightDirection;
			float distanceToLight;
			if (!SmokeEvaluateEmissiveCandidate(candidate, receiverPosition, viewRay, anisotropy, diagnostics,
				integrand, lightDirection, distanceToLight))
				continue;
			const float target = SmokeEmissiveLuminance(integrand);
			const uint retainedSamples = min(SmokeEmissiveRecordM(candidate), 16u);
			const float adjustedWeight = SmokeRetargetedEmissiveWeight(candidate, target, retainedSamples);
			SmokeReservoirMerge(reservoir, candidate, target, adjustedWeight, retainedSamples,
				SmokeEmissiveMediumHash(medium, anisotropy),
				max(SmokeEmissiveRecordAge(reservoir), SmokeEmissiveRecordAge(candidate)), randomState);
			if (diagnostics)
				InterlockedAdd(gSmokeControl[0].EmissiveSpatialAccepted, 1u);
		}
	}

	if (!SmokeEmissiveRecordValid(reservoir))
	{
		gSmokeEmissiveHistory[froxelIndex] = SmokePackEmissiveReservoir(SmokeEmptyEmissiveReservoir());
		return;
	}
	float3 integrand, lightDirection;
	float distanceToLight;
	if (!SmokeEvaluateEmissiveCandidate(reservoir, receiverPosition, viewRay, anisotropy, diagnostics,
		integrand, lightDirection, distanceToLight))
	{
		gSmokeEmissiveHistory[froxelIndex] = SmokePackEmissiveReservoir(SmokeEmptyEmissiveReservoir());
		return;
	}
	float visibility = 1.0;
	if (gSmokeConstants.LightMode >= 2u)
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveShadowRays, 1u);
		visibility = (SmokeFilteredVisibilityEffective()
			? SmokeEmissiveVisibleFiltered(receiverPosition, lightDirection, distanceToLight, diagnostics)
			: SmokeEmissiveVisible(receiverPosition, lightDirection, distanceToLight, diagnostics)) ? 1.0 : 0.0;
		if (diagnostics)
		{
			if (visibility > 0.0)
				InterlockedAdd(gSmokeControl[0].EmissiveShadowVisible, 1u);
			else
				InterlockedAdd(gSmokeControl[0].EmissiveShadowOccluded, 1u);
		}
	}
	const float normalization = reservoir.WeightSum /
		max((float)SmokeEmissiveRecordM(reservoir) * reservoir.Target, 1e-8);
	float3 incidentContribution = integrand * (normalization * visibility * gSmokeConstants.RadianceScale);
	const float unclampedLuminance = SmokeEmissiveLuminance(incidentContribution);
	if (unclampedLuminance > gSmokeConstants.DeltaTime)
	{
		incidentContribution *= gSmokeConstants.DeltaTime / unclampedLuminance;
		if (diagnostics)
		{
			InterlockedAdd(gSmokeControl[0].EmissiveSourceClamps, 1u);
			InterlockedAdd(gSmokeControl[0].EmissiveRemovedEnergy,
				(uint)min((unclampedLuminance - gSmokeConstants.DeltaTime) * 1024.0, 4294967295.0));
		}
	}
	uint spatialMetadata = SmokeFroxelMetadata(gSmokeFroxelSource[froxelIndex].w);
	spatialMetadata = SmokeFroxelResolveRadiance(spatialMetadata, gSmokeConstants.SimulationEpoch,
		NRI_SMOKE_FALLBACK_EMISSIVE, 0u);
	gSmokeFroxelSource[froxelIndex] = float4(gSmokeFroxelSource[froxelIndex].rgb + medium.rgb * incidentContribution,
		SmokeFroxelMetadataValue(spatialMetadata));
	reservoir.Metadata = SmokePackEmissiveMetadata(SmokeEmissiveRecordM(reservoir),
		SmokeEmissiveMediumHash(medium, anisotropy), SmokeEmissiveRecordAge(reservoir));
	reservoir.ReceiverPosition = receiverPosition;
	reservoir.SigmaT = medium.a;
	gSmokeEmissiveHistory[froxelIndex] = SmokePackEmissiveReservoir(reservoir);
	if (diagnostics)
	{
		InterlockedAdd(gSmokeControl[0].EmissiveFinalEvaluations, 1u);
		if (visibility > 0.0)
			InterlockedAdd(gSmokeControl[0].EmissiveContributed, 1u);
	}
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint controlCount, occupiedCapacity, mediumCount, phaseCount, sourceCount, currentCount, temporalCount, historyCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceCount, ignoredStride);
	gSmokeEmissiveCurrent.GetDimensions(currentCount, ignoredStride);
	gSmokeEmissiveTemporal.GetDimensions(temporalCount, ignoredStride);
	gSmokeEmissiveHistory.GetDimensions(historyCount, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >= min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;
	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	if (froxelIndex >= SmokeFroxelCount() || froxelIndex >= mediumCount || froxelIndex >= phaseCount ||
		froxelIndex >= sourceCount || froxelIndex >= currentCount || froxelIndex >= temporalCount || froxelIndex >= historyCount)
		return;
	const float4 phase = gSmokeFroxelPhase[froxelIndex];
	if (SmokeAnalyticCarrierReservoirOwns(phase))
		return;
	if (SmokeEmissiveWorldFieldOwnsGrid(phase))
		return;
	const float4 medium = SmokeEmissiveReceiverMedium(froxelIndex, phase,
		gSmokeFroxelMedium[froxelIndex]);
	if (medium.a <= 0.0 || !any(medium.rgb > 0.0))
		return;
	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	const uint3 froxel = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxel.xy);
	const float3 viewRay = normalize(ray);
	const float3 receiverPosition = SmokeFroxelCenter(froxel, ray);
	const float anisotropy = phase.x;
	if ((gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_REFERENCE) != 0u)
	{
		SmokeAccumulateReference(froxelIndex, froxel, medium, anisotropy, receiverPosition, viewRay, diagnostics);
		if (SmokeEmissiveGridFroxel(phase))
		{
			SmokeEmissiveMomentRecord emptyMoment = (SmokeEmissiveMomentRecord)0;
			gSmokeEmissiveHistory[froxelIndex] = SmokePackEmissiveMoment(emptyMoment);
		}
		else
			gSmokeEmissiveHistory[froxelIndex] = gSmokeEmissiveTemporal[froxelIndex];
		return;
	}

	if (!SmokeEmissiveGridFroxel(phase))
	{
		SmokeResolveLegacyEmissive(froxelIndex, froxel, medium, anisotropy, receiverPosition, viewRay, diagnostics);
		return;
	}

	SmokeEmissiveMomentRecord center = SmokeUnpackEmissiveMoment(gSmokeEmissiveCurrent[froxelIndex]);
	if (!SmokeEmissiveMomentValid(center))
	{
		SmokeEmissiveMomentRecord emptyMoment = (SmokeEmissiveMomentRecord)0;
		gSmokeEmissiveHistory[froxelIndex] = SmokePackEmissiveMoment(emptyMoment);
		return;
	}

	float3 reconstructedMean = center.MeanRadiance;
	float3 reconstructedSecond = center.SecondMoment;
	float3 directionSum = SmokeUnpackEmissiveDirection(center.Direction) *
		max(SmokeEmissiveLuminance(center.MeanRadiance), 1e-5);
	float weightSum = 1.0;
	float3 neighborhoodMinimum = center.MeanRadiance;
	float3 neighborhoodMaximum = center.MeanRadiance;
	const uint laneCount = SmokeEmissiveLaneCount();
	if (SmokeEmissiveReuseMode() >= 2u && (gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_LEGACY_GATHER_DISABLED) == 0u)
	{
		static const int3 offsets[6] = {
			int3(1, 0, 0), int3(-1, 0, 0), int3(0, 1, 0),
			int3(0, -1, 0), int3(0, 0, 1), int3(0, 0, -1)
		};
		[unroll]
		for (uint i = 0u; i < 6u; ++i)
		{
			const int3 neighbor = int3(froxel) + offsets[i];
			if (any(neighbor < 0) || any(neighbor >= int3(gSmokeConstants.FroxelWidth, gSmokeConstants.FroxelHeight, gSmokeConstants.FroxelDepth)))
				continue;
			const uint neighborIndex = SmokeFroxelIndex((uint)neighbor.x, (uint)neighbor.y, (uint)neighbor.z);
			const SmokeEmissiveMomentRecord candidate = SmokeUnpackEmissiveMoment(gSmokeEmissiveCurrent[neighborIndex]);
			if (!SmokeEmissiveMomentCompatible(candidate, medium, anisotropy, gSmokeConstants.FrameIndex,
				center.ReceiverPosition, SmokeIndirectWorldTolerance(froxel) * 2.0, laneCount))
			{
				if (diagnostics)
					InterlockedAdd(gSmokeControl[0].EmissiveSpatialRejected, 1u);
				continue;
			}
			const float centerLuminance = SmokeEmissiveLuminance(center.MeanRadiance);
			const float candidateLuminance = SmokeEmissiveLuminance(candidate.MeanRadiance);
			const float3 centerSigma = sqrt(max(center.SecondMoment - center.MeanRadiance * center.MeanRadiance, 0.0));
			const float3 candidateSigma = sqrt(max(candidate.SecondMoment - candidate.MeanRadiance * candidate.MeanRadiance, 0.0));
			const float signalTolerance = 0.025 + 3.0 * max(SmokeEmissiveLuminance(centerSigma), SmokeEmissiveLuminance(candidateSigma));
			if (abs(centerLuminance - candidateLuminance) > signalTolerance + max(centerLuminance, candidateLuminance) * 0.75)
			{
				if (diagnostics)
					InterlockedAdd(gSmokeControl[0].EmissiveSpatialRejected, 1u);
				continue;
			}
			const float spatialWeight = 0.25 * SmokeEmissiveMomentConfidence(candidate);
			if (spatialWeight <= 0.0)
				continue;
			reconstructedMean += candidate.MeanRadiance * spatialWeight;
			reconstructedSecond += candidate.SecondMoment * spatialWeight;
			directionSum += SmokeUnpackEmissiveDirection(candidate.Direction) *
				(spatialWeight * max(candidateLuminance, 1e-5));
			weightSum += spatialWeight;
			neighborhoodMinimum = min(neighborhoodMinimum, candidate.MeanRadiance);
			neighborhoodMaximum = max(neighborhoodMaximum, candidate.MeanRadiance);
			if (diagnostics)
				InterlockedAdd(gSmokeControl[0].EmissiveSpatialAccepted, 1u);
		}
	}

	reconstructedMean /= weightSum;
	reconstructedSecond /= weightSum;
	reconstructedMean = clamp(reconstructedMean, neighborhoodMinimum, neighborhoodMaximum);
	reconstructedSecond = max(reconstructedSecond, reconstructedMean * reconstructedMean);
	const float3 incidentDirection = SmokeUnpackEmissiveDirection(SmokePackEmissiveDirection(directionSum));
	const float phaseResponse = SmokePhaseResponse(dot(incidentDirection, viewRay), anisotropy);
	float3 incidentContribution = reconstructedMean * (phaseResponse * gSmokeConstants.RadianceScale);
	const float unclampedLuminance = SmokeEmissiveLuminance(incidentContribution);
	if (unclampedLuminance > gSmokeConstants.DeltaTime)
	{
		incidentContribution *= gSmokeConstants.DeltaTime / unclampedLuminance;
		if (diagnostics)
		{
			InterlockedAdd(gSmokeControl[0].EmissiveSourceClamps, 1u);
			InterlockedAdd(gSmokeControl[0].EmissiveRemovedEnergy,
				(uint)min((unclampedLuminance - gSmokeConstants.DeltaTime) * 1024.0, 4294967295.0));
		}
	}
	uint resolvedMetadata = SmokeFroxelMetadata(gSmokeFroxelSource[froxelIndex].w);
	resolvedMetadata = SmokeFroxelResolveRadiance(resolvedMetadata, gSmokeConstants.SimulationEpoch,
		NRI_SMOKE_FALLBACK_EMISSIVE, 0u);
	gSmokeFroxelSource[froxelIndex] = float4(gSmokeFroxelSource[froxelIndex].rgb + medium.rgb * incidentContribution,
		SmokeFroxelMetadataValue(resolvedMetadata));

	SmokeEmissiveMomentRecord resolved = center;
	resolved.MeanRadiance = reconstructedMean;
	resolved.SecondMoment = reconstructedSecond;
	resolved.ReceiverPosition = receiverPosition;
	resolved.SigmaT = medium.a;
	resolved.Direction = SmokePackEmissiveDirection(directionSum);
	resolved.Metadata = SmokePackEmissiveMomentMetadata(SmokeEmissiveMomentConfidence(center),
		SmokeEmissiveMediumHash(medium, anisotropy), SmokeEmissiveMomentAge(center), laneCount);
	gSmokeEmissiveHistory[froxelIndex] = SmokePackEmissiveMoment(resolved);
	if (diagnostics)
	{
		InterlockedAdd(gSmokeControl[0].EmissiveFinalEvaluations, 1u);
		if (SmokeEmissiveLuminance(incidentContribution) > 0.0)
			InterlockedAdd(gSmokeControl[0].EmissiveContributed, 1u);
	}
}
