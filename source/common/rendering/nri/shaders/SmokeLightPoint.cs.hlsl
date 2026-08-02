#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeLighting.hlsli"
#include "Include/SmokeDirectCache.hlsli"
#include "Include/SmokeGridTransmittance.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (gSmokeConstants.FroxelWidth == 0u || gSmokeConstants.FroxelHeight == 0u || gSmokeConstants.FroxelDepth == 0u)
		return;

	uint controlCount, occupiedCapacity, mediumCount, phaseCount, sourceCount, directCurrentCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceCount, ignoredStride);
	gSmokeDirectCurrent.GetDimensions(directCurrentCount, ignoredStride);
	if (controlCount == 0u || dispatchThreadId.x >= min(gSmokeControl[0].OccupiedCount, occupiedCapacity))
		return;

	const uint froxelIndex = gSmokeOccupiedFroxelIndices[dispatchThreadId.x];
	const uint froxelCount = SmokeFroxelCount();
	if (froxelIndex >= froxelCount || froxelIndex >= mediumCount || froxelIndex >= phaseCount || froxelIndex >= sourceCount)
		return;
	const uint3 froxelPositionIndex = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxelPositionIndex.xy);
	const float3 froxelPosition = SmokeFroxelCenter(froxelPositionIndex, ray);
	const float4 medium = gSmokeFroxelMedium[froxelIndex];
	const float4 phaseRecord = gSmokeFroxelPhase[froxelIndex];
	const float anisotropy = phaseRecord.x;
	const bool gridDirect = SmokeDirectFroxelIsGrid(phaseRecord);
	if (gridDirect && froxelIndex >= directCurrentCount)
		return;
	const bool lightingDiagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	if (lightingDiagnostics)
		InterlockedAdd(gSmokeControl[0].PointFroxelsProcessed, 1u);

	// Start with extinction only. A fixed neutral ambient source makes sufficiently
	// dark surfaces appear uniformly brighter anywhere smoke overlaps them.
	// Explicit point, directional, and emissive families add real in-scattering.
	float3 scattering = 0.0;
	float visibleWeight = 0.0;
	float mediumWeight = 0.0;
	float combinedWeight = 0.0;
	float unshadowedWeight = 0.0;
	uint receiverSamples = 0u;
	if (medium.a > 0.0 && any(medium.rgb > 0.0) &&
		gSmokeConstants.LightMode > 0u && (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_POINT) != 0u)
	{
		const float3 viewRay = normalize(ray);
		const RuntimeLightTileHeaderData tileHeader = SmokeGetRuntimeLightTileHeader(froxelPositionIndex.xy);
		uint lightCount, lightStride, lightIndexCount, lightIndexStride;
		gSmokeRuntimePointLights.GetDimensions(lightCount, lightStride);
		gSmokeRuntimeLightTileIndices.GetDimensions(lightIndexCount, lightIndexStride);
		const uint runtimeLightCount = min(gSmokeConstants.RuntimeLightCount, lightCount);
		const uint selectionCapacity = min(gSmokeConstants.MaxLightCandidates, NRI_SMOKE_MAX_SELECTED_LIGHTS);
		uint selectedLightIndices[NRI_SMOKE_MAX_SELECTED_LIGHTS];
		float selectedLightScores[NRI_SMOKE_MAX_SELECTED_LIGHTS];
		uint selectedLightCount = 0u;

		[loop]
		for (uint tileCandidate = 0u; tileCandidate < tileHeader.indexCount && selectionCapacity > 0u; ++tileCandidate)
		{
			const uint packedIndex = tileHeader.indexOffset + tileCandidate;
			if (packedIndex >= lightIndexCount)
				break;
			const uint lightIndex = gSmokeRuntimeLightTileIndices[packedIndex];
			if (lightIndex >= runtimeLightCount)
				continue;
			if (lightingDiagnostics)
				InterlockedAdd(gSmokeControl[0].LightCandidatesTested, 1u);
			const RuntimePointLightData light = gSmokeRuntimePointLights[lightIndex];
			const float centerDistance = length(light.position - froxelPosition);
			const float attenuation = EvaluateAnalyticPointLightAttenuation(centerDistance, light.radius, light.intensity);
			const float score = attenuation * dot(max(light.color, 0.0), float3(0.2126, 0.7152, 0.0722));
			if (score <= 0.0)
			{
				if (lightingDiagnostics)
					InterlockedAdd(gSmokeControl[0].LightDistanceRejected, 1u);
				continue;
			}
			if (selectedLightCount < selectionCapacity)
			{
				selectedLightIndices[selectedLightCount] = lightIndex;
				selectedLightScores[selectedLightCount] = score;
				selectedLightCount++;
			}
			else
			{
				uint weakestIndex = 0u;
				[unroll]
				for (uint selectedIndex = 1u; selectedIndex < NRI_SMOKE_MAX_SELECTED_LIGHTS; ++selectedIndex)
				{
					if (selectedIndex < selectedLightCount && selectedLightScores[selectedIndex] < selectedLightScores[weakestIndex])
						weakestIndex = selectedIndex;
				}
				if (score > selectedLightScores[weakestIndex])
				{
					selectedLightIndices[weakestIndex] = lightIndex;
					selectedLightScores[weakestIndex] = score;
				}
			}
		}

		[loop]
		for (uint selectedIndex = 0u; selectedIndex < selectedLightCount; ++selectedIndex)
		{
			const RuntimePointLightData light = gSmokeRuntimePointLights[selectedLightIndices[selectedIndex]];
			const float3 toLightCenter = light.position - froxelPosition;
			const float centerDistanceSquared = dot(toLightCenter, toLightCenter);
			if (centerDistanceSquared <= 1e-4)
				continue;
			const float centerDistance = sqrt(centerDistanceSquared);
			const float attenuation = EvaluateAnalyticPointLightAttenuation(centerDistance, light.radius, light.intensity);
			if (attenuation <= 0.0)
				continue;
			const float3 centerDirection = toLightCenter / centerDistance;
			const uint sampleCount = gridDirect ? SmokeDirectReceiverSampleCount() :
				(gSmokeConstants.LightMode >= 3u ? clamp(gSmokeConstants.LightSamples, 1u, 4u) : 1u);
			if (gridDirect)
				receiverSamples += sampleCount;
			float visibleSamples = 0.0;
			float transmittanceSamples = 0.0;
			float combinedSamples = 0.0;
			float3 particleContribution = 0.0;
			[loop]
			for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
			{
				const float3 receiverPosition = gridDirect
					? SmokeDirectReceiverPosition(froxelPositionIndex, sampleIndex, sampleCount)
					: froxelPosition;
				const float3 receiverToLightCenter = light.position - receiverPosition;
				const float receiverCenterDistanceSquared = dot(receiverToLightCenter, receiverToLightCenter);
				if (receiverCenterDistanceSquared <= 1e-4)
					continue;
				const float receiverCenterDistance = sqrt(receiverCenterDistanceSquared);
				const float3 receiverCenterDirection = receiverToLightCenter / receiverCenterDistance;
				float3 sampledPosition = light.position;
				if (gSmokeConstants.LightMode >= 3u || (gridDirect && sampleCount > 1u && light.emitterRadius > 0.0))
				{
					uint randomState = gridDirect
						? SmokeDirectRandomSeed(receiverPosition, light.stableKeyLo, light.stableKeyHi, sampleIndex)
						: SmokeLightRandomSeed(froxelPositionIndex, light, sampleIndex);
					sampledPosition = SmokeSampleReceiverFacingEmitter(light, receiverCenterDirection, randomState);
					if (lightingDiagnostics && light.emitterRadius > 0.0)
						InterlockedAdd(gSmokeControl[0].LightSoftSamples, 1u);
				}
				const float3 toSampledLight = sampledPosition - receiverPosition;
				const float sampledDistanceSquared = dot(toSampledLight, toSampledLight);
				if (sampledDistanceSquared <= 1e-4)
					continue;
				const float sampledDistance = sqrt(sampledDistanceSquared);
				const float3 lightDirection = toSampledLight / sampledDistance;
				float visibility = 1.0;
				const bool castsShadow = (light.flags & NRI_SMOKE_RUNTIME_LIGHT_FLAG_CASTS_SHADOW) != 0u;
				if (gSmokeConstants.LightMode >= 2u && castsShadow)
				{
					if (!SmokeShadowTracingReady())
						continue;
					if (lightingDiagnostics)
						InterlockedAdd(gSmokeControl[0].LightShadowRays, 1u);
					visibility = (SmokeFilteredVisibilityEffective()
						? SmokePointLightVisibleFiltered(receiverPosition, lightDirection, sampledDistance, lightingDiagnostics)
						: SmokePointLightVisible(receiverPosition, lightDirection, sampledDistance, lightingDiagnostics)) ? 1.0 : 0.0;
					if (lightingDiagnostics)
					{
						if (visibility > 0.0)
							InterlockedAdd(gSmokeControl[0].LightShadowVisible, 1u);
						else
							InterlockedAdd(gSmokeControl[0].LightShadowOccluded, 1u);
					}
				}
				if (gridDirect)
				{
					uint marchSteps;
					bool marchTruncated;
					const float mediumTransmittance = SmokeGridMediumTransmittance(receiverPosition,
						lightDirection, sampledDistance, marchSteps, marchTruncated);
					visibleSamples += visibility;
					transmittanceSamples += mediumTransmittance;
					combinedSamples += visibility * mediumTransmittance;
				}
				else
				{
					const float phase = SmokePhaseResponse(dot(lightDirection, viewRay), anisotropy);
					const float3 unclampedLightRadiance = max(light.color, 0.0) * attenuation;
					if (lightingDiagnostics && any(unclampedLightRadiance > 32.0))
						InterlockedAdd(gSmokeControl[0].LightRadianceClamps, 1u);
					particleContribution += min(unclampedLightRadiance, 32.0) * (phase * visibility);
				}
			}
			if (!gridDirect)
			{
				scattering += medium.rgb * (particleContribution / (float)sampleCount);
				continue;
			}
			const float fractionalVisibility = visibleSamples / (float)sampleCount;
			const float fractionalTransmittance = transmittanceSamples / (float)sampleCount;
			const float fractionalCombined = combinedSamples / (float)sampleCount;
			const float phase = SmokePhaseResponse(dot(centerDirection, viewRay), anisotropy);
			const float3 unclampedLightRadiance = max(light.color, 0.0) * attenuation;
			if (lightingDiagnostics && any(unclampedLightRadiance > 32.0))
				InterlockedAdd(gSmokeControl[0].LightRadianceClamps, 1u);
			const float3 unshadowedContribution = min(unclampedLightRadiance, 32.0) * phase;
			const float3 unshadowedScattering = medium.rgb * unshadowedContribution;
			scattering += unshadowedScattering;
			const float sampleWeight = dot(max(unshadowedScattering, 0.0), float3(0.2126, 0.7152, 0.0722));
			unshadowedWeight += sampleWeight;
			visibleWeight += sampleWeight * fractionalVisibility;
			mediumWeight += sampleWeight * fractionalTransmittance;
			combinedWeight += sampleWeight * fractionalCombined;
		}
	}
	if (gridDirect)
	{
		const float visibility = unshadowedWeight > 1e-6 ? visibleWeight / unshadowedWeight : 0.0;
		const float mediumTransmittance = unshadowedWeight > 1e-6 ? mediumWeight / unshadowedWeight : 1.0;
		const float combinedVisibility = unshadowedWeight > 1e-6 ? combinedWeight / unshadowedWeight : 0.0;
		SmokeDirectCacheRecord record;
		record.Radiance = max(scattering, 0.0);
		record.SigmaT = medium.a;
		record.WorldPosition = froxelPosition;
		record.Metadata = SmokeDirectPackMetadata(1u, gSmokeConstants.FrameIndex, visibility, combinedVisibility);
		record.MediumTransmittance = saturate(mediumTransmittance);
		record.MediumMetadata = SmokeDirectPackMediumMetadata(0u, gSmokeConstants.FrameIndex,
			SmokeDirectSelfShadowBlock(froxelPosition));
		gSmokeDirectCurrent[froxelIndex] = record;
		if (receiverSamples > 0u)
			SmokeDirectAccumulateVisibilityDiagnostics(visibility, receiverSamples, lightingDiagnostics);
	}
	else
	{
		const float3 source = gSmokeFroxelSource[froxelIndex].rgb + scattering * gSmokeConstants.RadianceScale;
		uint metadata = SmokeFroxelMetadata(gSmokeFroxelSource[froxelIndex].w);
		metadata = SmokeFroxelResolveRadiance(metadata, gSmokeConstants.SimulationEpoch,
			NRI_SMOKE_FALLBACK_ANALYTIC, 0u);
		gSmokeFroxelSource[froxelIndex] = float4(source, SmokeFroxelMetadataValue(metadata));
	}
}
