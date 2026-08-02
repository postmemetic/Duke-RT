#include "Include/SmokeGridLightingResources.hlsli"
#include "Include/SmokeGridTransmittance.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeLighting.hlsli"

#define NRI_SMOKE_SCATTER_PHASE_L0 0.07957747154594767

uint SmokeGridScatterSeed(int3 probe, uint familySalt)
{
	uint seed = SmokeHash(asuint(probe.x) ^ SmokeHash(asuint(probe.y)) ^ SmokeHash(asuint(probe.z)));
	seed ^= SmokeHash(gSmokeConstants.SimulationEpoch) ^ SmokeHash(gSmokeConstants.FrameIndex);
	return SmokeHash(seed ^ familySalt);
}

float3 SmokeGridScatterPointIncident(float3 receiver, int3 probe, out bool contributed)
{
	contributed = false;
	if (gSmokeConstants.LightMode == 0u ||
		(gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_POINT) == 0u)
		return 0.0;

	uint lightCapacity, ignoredStride;
	gSmokeRuntimePointLights.GetDimensions(lightCapacity, ignoredStride);
	const uint lightCount = min(gSmokeConstants.RuntimeLightCount, lightCapacity);
	const uint selectionCapacity = min(gSmokeConstants.MaxLightCandidates, NRI_SMOKE_MAX_SELECTED_LIGHTS);
	uint selectedIndices[NRI_SMOKE_MAX_SELECTED_LIGHTS];
	float selectedScores[NRI_SMOKE_MAX_SELECTED_LIGHTS];
	uint selectedCount = 0u;
	[loop]
	for (uint lightIndex = 0u; lightIndex < lightCount && selectionCapacity > 0u; ++lightIndex)
	{
		const RuntimePointLightData light = gSmokeRuntimePointLights[lightIndex];
		const float distance = length(light.position - receiver);
		const float attenuation = EvaluateAnalyticPointLightAttenuation(distance, light.radius, light.intensity);
		const float score = attenuation * dot(max(light.color, 0.0), float3(0.2126, 0.7152, 0.0722));
		if (score <= 0.0)
			continue;
		if (selectedCount < selectionCapacity)
		{
			selectedIndices[selectedCount] = lightIndex;
			selectedScores[selectedCount++] = score;
		}
		else
		{
			uint weakest = 0u;
			[unroll]
			for (uint selected = 1u; selected < NRI_SMOKE_MAX_SELECTED_LIGHTS; ++selected)
				if (selected < selectedCount && selectedScores[selected] < selectedScores[weakest]) weakest = selected;
			if (score > selectedScores[weakest])
			{
				selectedIndices[weakest] = lightIndex;
				selectedScores[weakest] = score;
			}
		}
	}

	float3 incident = 0.0;
	[loop]
	for (uint selected = 0u; selected < selectedCount; ++selected)
	{
		const RuntimePointLightData light = gSmokeRuntimePointLights[selectedIndices[selected]];
		const float3 toCenter = light.position - receiver;
		const float centerDistance = length(toCenter);
		if (centerDistance <= 1e-4)
			continue;
		const float attenuation = EvaluateAnalyticPointLightAttenuation(centerDistance, light.radius, light.intensity);
		if (attenuation <= 0.0)
			continue;
		const float3 centerDirection = toCenter / centerDistance;
		float3 sampledPosition = light.position;
		if (gSmokeConstants.LightMode >= 3u && light.emitterRadius > 0.0)
		{
			uint randomState = SmokeGridScatterSeed(probe, light.stableKeyLo ^ SmokeHash(light.stableKeyHi));
			sampledPosition = SmokeSampleReceiverFacingEmitter(light, centerDirection, randomState);
		}
		const float3 toLight = sampledPosition - receiver;
		const float distance = length(toLight);
		if (distance <= 1e-4)
			continue;
		const float3 direction = toLight / distance;
		float visibility = 1.0;
		if (gSmokeConstants.LightMode >= 2u &&
			(light.flags & NRI_SMOKE_RUNTIME_LIGHT_FLAG_CASTS_SHADOW) != 0u)
		{
			if (!SmokeShadowTracingReady())
				continue;
			visibility = (SmokeFilteredVisibilityEffective()
				? SmokePointLightVisibleFiltered(receiver, direction, distance, false)
				: SmokePointLightVisible(receiver, direction, distance, false)) ? 1.0 : 0.0;
		}
		uint marchSteps;
		bool truncated;
		const float mediumTransmittance = SmokeGridMediumTransmittance(receiver, direction, distance, marchSteps, truncated);
		if (SmokeSelfShadowEnabled(gSmokeConstants.DebugMode))
		{
			InterlockedAdd(gSmokeGridLightControl[0].SelfShadowSamples, 1u);
			InterlockedAdd(gSmokeGridLightControl[0].SelfShadowSteps, marchSteps);
			if (truncated) InterlockedAdd(gSmokeGridLightControl[0].SelfShadowTruncated, 1u);
		}
		incident += min(max(light.color, 0.0) * attenuation, 32.0) * (visibility * mediumTransmittance);
	}
	contributed = any(incident > 0.0);
	return incident * (gSmokeConstants.RadianceScale * NRI_SMOKE_SCATTER_PHASE_L0);
}

float3 SmokeGridScatterDirectionalIncident(float3 receiver, int3 probe, out bool contributed)
{
	contributed = false;
	if (gSmokeConstants.LightMode == 0u ||
		(gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL) == 0u)
		return 0.0;
	const float3 centerDirection = SmokeDirectionalDirection();
	float3 direction = centerDirection;
	if (gSmokeConstants.LightMode >= 3u && gSmokeConstants.DirectionalAngularSize > 0.0)
	{
		uint randomState = SmokeGridScatterSeed(probe, gSmokeConstants.DirectionalColorPacked);
		direction = SmokeSampleDirectionalCone(centerDirection, gSmokeConstants.DirectionalAngularSize, randomState);
	}
	float visibility = 1.0;
	const bool castsShadow = (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL_SHADOW) != 0u;
	if (gSmokeConstants.LightMode >= 2u && castsShadow)
	{
		if (!SmokeShadowTracingReady())
			return 0.0;
		visibility = (SmokeFilteredVisibilityEffective()
			? SmokePointLightVisibleFiltered(receiver, direction, 100000.0, false)
			: SmokePointLightVisible(receiver, direction, 100000.0, false)) ? 1.0 : 0.0;
	}
	uint marchSteps;
	bool truncated;
	const float mediumTransmittance = SmokeGridDirectionalTransmittance(receiver, direction, marchSteps, truncated);
	if (SmokeSelfShadowEnabled(gSmokeConstants.DebugMode))
	{
		InterlockedAdd(gSmokeGridLightControl[0].SelfShadowSamples, 1u);
		InterlockedAdd(gSmokeGridLightControl[0].SelfShadowSteps, marchSteps);
		if (truncated) InterlockedAdd(gSmokeGridLightControl[0].SelfShadowTruncated, 1u);
	}
	const float3 incident = SmokeDirectionalColor() * (visibility * mediumTransmittance) *
		(gSmokeConstants.RadianceScale * NRI_SMOKE_SCATTER_PHASE_L0);
	contributed = any(incident > 0.0);
	return incident;
}

float3 SmokeGridScatterEnvironmentIncident(float3 receiver, int3 probe, out bool contributed)
{
	contributed = false;
	if (gSmokeConstants.IndirectScale <= 0.0 ||
		(gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_INDIRECT) == 0u)
		return 0.0;
	const float3 worldUp = float3(0.0, -1.0, 0.0);
	const SmokeIndirectHit upHit = SmokeTraceIndirectClosest(receiver, worldUp, gSmokeConstants.FroxelMaxDistance);
	const SmokeIndirectHit downHit = SmokeTraceIndirectClosest(receiver, -worldUp, gSmokeConstants.FroxelMaxDistance);
	uint upSector, downSector;
	if (!SmokeResolveStaticFlatSector(upHit, upSector) ||
		!SmokeResolveStaticFlatSector(downHit, downSector) || upSector != downSector)
		return 0.0;
	float3 incident = SmokeSectorAmbientIncidentRadiance(upSector);
	const uint sampleCount = clamp(gSmokeConstants.LightSamples, 1u, 4u);
	[loop]
	for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
	{
		const float3 direction = SmokeIndirectReferenceDirection(sampleIndex, sampleCount, asuint(probe));
		const SmokeIndirectHit hit = SmokeTraceIndirectClosest(receiver, direction, gSmokeConstants.FroxelMaxDistance);
		// Surface emission already enters through the accepted six-lobe world field.
		// Only environment misses are admitted here, avoiding a second emissive estimator.
		if (hit.hit == 0u)
		{
			uint marchSteps;
			bool truncated;
			const float mediumTransmittance = SmokeGridDirectionalTransmittance(receiver, direction, marchSteps, truncated);
			incident += max(gSmokeSkyTexture.SampleLevel(gSmokeLinearWrap, direction, 0.0).rgb, 0.0) *
				(mediumTransmittance / (float)sampleCount);
		}
	}
	incident = min(max(incident, 0.0), 32.0) *
		(gSmokeConstants.IndirectScale * gSmokeConstants.RadianceScale);
	contributed = any(incident > 0.0);
	return incident;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint probeCapacity, ignoredStride;
	gSmokeGridScatterSeed.GetDimensions(probeCapacity, ignoredStride);
	if (dispatchThreadId.x >= probeCapacity)
		return;
	const uint probeIndex = dispatchThreadId.x;
	const float4 previousSeed = gSmokeGridScatterSeed[probeIndex];
	const SmokeGridScatterMetadata previousMetadata = gSmokeGridScatterMetadata[probeIndex];
	const uint phaseSignature = SmokePhaseSettingsSignature();
	gSmokeGridScatterSeed[probeIndex] = 0.0;
	gSmokeGridScatterBounceA[probeIndex] = 0.0;
	gSmokeGridScatterBounceB[probeIndex] = 0.0;
	gSmokeGridScatterMetadata[probeIndex] = (SmokeGridScatterMetadata)0;
	if (probeIndex == 0u)
	{
		gSmokeGridLightControl[0].ScatterProbeCapacity = probeCapacity;
		gSmokeGridLightControl[0].ScatterProbesPerBrick = NRI_SMOKE_GRID_SCATTER_PROBES_PER_BRICK;
		gSmokeGridLightControl[0].ScatterFrameStamp = gSmokeConstants.FrameIndex;
		gSmokeGridLightControl[0].ScatterEpoch = gSmokeConstants.SimulationEpoch;
		gSmokeGridLightControl[0].ScatterFinalPing = 0u;
	}

	const uint brickIndex = probeIndex / NRI_SMOKE_GRID_SCATTER_PROBES_PER_BRICK;
	uint brickCapacity;
	gSmokeRenderGridBricks.GetDimensions(brickCapacity, ignoredStride);
	if (brickIndex >= brickCapacity)
		return;
	const SmokeGridBrick brick = gSmokeRenderGridBricks[brickIndex];
	if (brick.State != NRI_SMOKE_GRID_RESIDENT ||
		(brick.Flags & (NRI_SMOKE_GRID_BRICK_CONTENT | NRI_SMOKE_GRID_BRICK_HALO)) == 0u)
		return;
	const uint3 localProbe = SmokeGridScatterLocalProbe(probeIndex % NRI_SMOKE_GRID_SCATTER_PROBES_PER_BRICK);
	const int3 probe = SmokeGridScatterProbeCoordinate(brick, localProbe);
	const int3 baseCell = probe * 2;
	float3 sigmaS = 0.0;
	float3 cornerSigmaS[8];
	float sigmaT = 0.0;
	[loop]
	for (uint corner = 0u; corner < 8u; ++corner)
	{
		const int3 offset = int3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
		float4 scalar, optical;
		if (!SmokeGridScatterLoadCell(baseCell + offset, scalar, optical))
			return;
		cornerSigmaS[corner] = max(optical.rgb * gSmokeConstants.DensityScale, 0.0);
		sigmaS += cornerSigmaS[corner] * 0.125;
		sigmaT += max(scalar.z * gSmokeConstants.DensityScale, 0.0) * 0.125;
	}
	const uint faceMask = SmokeGridScatterProbeFaceMask(probe);
	const bool internalOpen = SmokeGridScatterProbeInternalOpen(probe);
	const uint blockOffset = SmokeHash(asuint(probe.x) ^ SmokeHash(asuint(probe.y)) ^ SmokeHash(asuint(probe.z))) & 7u;
	const uint historyBlock = (gSmokeConstants.FrameIndex + blockOffset) >> 3u;
	if (sigmaT <= 1e-6 || !any(sigmaS > 1e-6) || !internalOpen)
	{
		SmokeGridScatterMetadata zeroMetadata;
		zeroMetadata.BrickGeneration = brick.Generation;
		zeroMetadata.SimulationEpoch = gSmokeConstants.SimulationEpoch;
		zeroMetadata.FrameStamp = gSmokeConstants.FrameIndex;
		zeroMetadata.Flags = NRI_SMOKE_GRID_SCATTER_VALID | NRI_SMOKE_GRID_SCATTER_EXPLICIT_ZERO |
			(internalOpen ? (faceMask << NRI_SMOKE_GRID_SCATTER_FACE_SHIFT) : NRI_SMOKE_GRID_SCATTER_SPLIT_BLOCKED);
		zeroMetadata.HistoryBlock = historyBlock;
		zeroMetadata.HistoryCount = 0u;
		zeroMetadata.TransmittanceQ = 0u;
		zeroMetadata.Reserved = phaseSignature;
		gSmokeGridScatterMetadata[probeIndex] = zeroMetadata;
		InterlockedAdd(gSmokeGridLightControl[0].ExplicitZeroProbes, 1u);
		if (!internalOpen)
		{
			InterlockedAdd(gSmokeGridLightControl[0].ScatterInternalBlocked, 1u);
			InterlockedAdd(gSmokeGridLightControl[0].SplitBlockedProbes, 1u);
		}
		else
			InterlockedAdd(gSmokeGridLightControl[0].ScatterExactZero, 1u);
		return;
	}

	const float cellSize = max(asfloat(gSmokeRenderGridControl[0].CellSizeBits), 0.0001);
	float3 seed = 0.0;
	bool probePointContribution = false;
	bool probeDirectionalContribution = false;
	bool probeEnvironmentContribution = false;
	bool probeEmissiveContribution = false;
	[loop]
	for (uint corner = 0u; corner < 8u; ++corner)
	{
		if (!any(cornerSigmaS[corner] > 0.0))
			continue;
		const int3 offset = int3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
		const int3 cell = baseCell + offset;
		const float3 receiver = SmokeGridLightCellCenter(cell, cellSize);
		float3 controlledSource = 0.0;
		bool pointContribution, directionalContribution, environmentContribution;
		controlledSource += SmokeGridScatterPointIncident(receiver, cell, pointContribution);
		controlledSource += SmokeGridScatterDirectionalIncident(receiver, cell, directionalContribution);
		controlledSource += SmokeGridScatterEnvironmentIncident(receiver, cell, environmentContribution);
		probePointContribution = probePointContribution || pointContribution;
		probeDirectionalContribution = probeDirectionalContribution || directionalContribution;
		probeEnvironmentContribution = probeEnvironmentContribution || environmentContribution;
		if ((gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_EMISSIVE) != 0u &&
			(gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_WORLD_ENABLED) != 0u)
		{
			float3 lobes[6];
			float confidence;
			if (SmokeGridLightSample(receiver, cellSize, lobes, confidence))
			{
				float3 lobeSum = 0.0;
				[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe) lobeSum += lobes[lobe];
				const float3 emissiveSource = SmokeGridClampControlledSource(
					lobeSum * (gSmokeConstants.RadianceScale * NRI_SMOKE_SCATTER_PHASE_L0));
				controlledSource += emissiveSource;
				probeEmissiveContribution = probeEmissiveContribution || any(emissiveSource > 0.0);
			}
		}
		// Downsample the complete source-times-scattering product. Averaging Li
		// and sigma_s separately would correlate light with empty neighboring cells.
		seed += cornerSigmaS[corner] * controlledSource * 0.125;
	}
	if (probePointContribution) InterlockedAdd(gSmokeGridLightControl[0].ScatterPointCells, 1u);
	if (probeDirectionalContribution) InterlockedAdd(gSmokeGridLightControl[0].ScatterDirectionalCells, 1u);
	if (probeEnvironmentContribution) InterlockedAdd(gSmokeGridLightControl[0].ScatterEnvironmentCells, 1u);
	if (probeEmissiveContribution) InterlockedAdd(gSmokeGridLightControl[0].ScatterEmissiveCells, 1u);
	if (!all(isfinite(seed)))
	{
		InterlockedAdd(gSmokeGridLightControl[0].ScatterNanRejects, 1u);
		return;
	}
	uint historyCount = 1u;
	if (SmokeSelfShadowEnabled(gSmokeConstants.DebugMode) &&
		previousMetadata.BrickGeneration == brick.Generation &&
		previousMetadata.SimulationEpoch == gSmokeConstants.SimulationEpoch &&
		previousMetadata.FrameStamp + 1u == gSmokeConstants.FrameIndex &&
		previousMetadata.Reserved == phaseSignature &&
		previousMetadata.HistoryBlock == historyBlock && previousMetadata.HistoryCount > 0u &&
		previousMetadata.HistoryCount < 8u &&
		(previousMetadata.Flags & (NRI_SMOKE_GRID_SCATTER_VALID | NRI_SMOKE_GRID_SCATTER_FACE_MASK)) ==
			(NRI_SMOKE_GRID_SCATTER_VALID | (faceMask << NRI_SMOKE_GRID_SCATTER_FACE_SHIFT)) &&
		all(isfinite(previousSeed)) &&
		abs(previousSeed.w - sigmaT) <= max(max(previousSeed.w, sigmaT) * 0.25, 0.002))
	{
		historyCount = previousMetadata.HistoryCount + 1u;
		seed = lerp(max(previousSeed.rgb, 0.0), seed, rcp((float)historyCount));
		InterlockedAdd(gSmokeGridLightControl[0].SelfShadowHistoryAccepted, 1u);
		InterlockedMax(gSmokeGridLightControl[0].SelfShadowMaximumAge, historyCount - 1u);
	}
	else if (SmokeSelfShadowEnabled(gSmokeConstants.DebugMode))
		InterlockedAdd(gSmokeGridLightControl[0].SelfShadowHistoryRestarted, 1u);
	SmokeGridScatterMetadata metadata;
	metadata.BrickGeneration = brick.Generation;
	metadata.SimulationEpoch = gSmokeConstants.SimulationEpoch;
	metadata.FrameStamp = gSmokeConstants.FrameIndex;
	metadata.Flags = NRI_SMOKE_GRID_SCATTER_VALID | (faceMask << NRI_SMOKE_GRID_SCATTER_FACE_SHIFT);
	metadata.HistoryBlock = historyBlock;
	metadata.HistoryCount = historyCount;
	metadata.TransmittanceQ = 0u;
	metadata.Reserved = phaseSignature;
	gSmokeGridScatterMetadata[probeIndex] = metadata;
	gSmokeGridScatterSeed[probeIndex] = float4(max(seed, 0.0), sigmaT);
	uint activeCapacity;
	gSmokeGridScatterActive.GetDimensions(activeCapacity, ignoredStride);
	uint activeSlot;
	InterlockedAdd(gSmokeGridLightControl[0].ScatterActiveCount, 1u, activeSlot);
	if (activeSlot < activeCapacity)
		gSmokeGridScatterActive[activeSlot] = probeIndex;
	else
		InterlockedAdd(gSmokeGridLightControl[0].ScatterActiveOverflow, 1u);
	InterlockedAdd(gSmokeGridLightControl[0].ScatterSeededCount, 1u);
	if (any(seed > 0.0)) InterlockedAdd(gSmokeGridLightControl[0].ScatterNonzeroSourceCount, 1u);
	else InterlockedAdd(gSmokeGridLightControl[0].ScatterZeroSourceCount, 1u);
	InterlockedAdd(gSmokeGridLightControl[0].ScatterSourceEnergyQ,
		(uint)min(dot(max(seed, 0.0), float3(0.2126, 0.7152, 0.0722)) * 1024.0, 4294967295.0));
}
