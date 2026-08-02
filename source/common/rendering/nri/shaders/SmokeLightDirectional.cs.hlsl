#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeLighting.hlsli"
#include "Include/SmokeDirectCache.hlsli"
#include "Include/SmokeGridTransmittance.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
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
	if (froxelIndex >= SmokeFroxelCount() || froxelIndex >= mediumCount || froxelIndex >= phaseCount || froxelIndex >= sourceCount)
		return;
	const float4 medium = gSmokeFroxelMedium[froxelIndex];
	if (medium.a <= 0.0 || !any(medium.rgb > 0.0))
		return;

	const bool diagnostics = (gSmokeConstants.Flags & 2u) != 0u;
	if (gSmokeConstants.LightMode == 0u || (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL) == 0u)
		return;
	if (diagnostics)
		InterlockedAdd(gSmokeControl[0].DirectionalFroxelsProcessed, 1u);

	const uint3 froxelCoordinates = SmokeFroxelCoordinates(froxelIndex);
	const float3 ray = SmokeFroxelRay(froxelCoordinates.xy);
	const float3 viewRay = normalize(ray);
	const float4 phaseRecord = gSmokeFroxelPhase[froxelIndex];
	const bool gridDirect = SmokeDirectFroxelIsGrid(phaseRecord);
	if (gridDirect)
	{
		if (froxelIndex >= directCurrentCount)
			return;
		const float3 centerDirection = SmokeDirectionalDirection();
		const bool castsShadow = (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL_SHADOW) != 0u;
		if (gSmokeConstants.LightMode >= 2u && castsShadow && !SmokeShadowTracingReady())
			return;
		const uint sampleCount = SmokeDirectReceiverSampleCount();
		const uint directionalKey = SmokeDirectDirectionalKey();
		float visibleSamples = 0.0;
		float transmittanceSamples = 0.0;
		float combinedSamples = 0.0;
		[loop]
		for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
		{
			const float3 receiverPosition = SmokeDirectReceiverPosition(froxelCoordinates, sampleIndex, sampleCount);
			uint randomState = SmokeDirectRandomSeed(receiverPosition, directionalKey,
				gSmokeConstants.DirectionalColorPacked, sampleIndex);
			const bool sampleAngularFootprint = gSmokeConstants.LightMode >= 3u ||
				(sampleCount > 1u && gSmokeConstants.DirectionalAngularSize > 0.0);
			const float3 lightDirection = sampleAngularFootprint
				? SmokeSampleDirectionalCone(centerDirection, gSmokeConstants.DirectionalAngularSize, randomState)
				: centerDirection;
			float visibility = 1.0;
			if (gSmokeConstants.LightMode >= 2u && castsShadow)
			{
				if (diagnostics)
				{
					InterlockedAdd(gSmokeControl[0].DirectionalSamples, 1u);
					InterlockedAdd(gSmokeControl[0].DirectionalShadowRays, 1u);
				}
				visibility = (SmokeFilteredVisibilityEffective()
					? SmokePointLightVisibleFiltered(receiverPosition, lightDirection, 100000.0, diagnostics)
					: SmokePointLightVisible(receiverPosition, lightDirection, 100000.0, diagnostics)) ? 1.0 : 0.0;
				if (diagnostics)
				{
					if (visibility > 0.0)
						InterlockedAdd(gSmokeControl[0].DirectionalShadowVisible, 1u);
					else
						InterlockedAdd(gSmokeControl[0].DirectionalShadowOccluded, 1u);
				}
			}
			visibleSamples += visibility;
			uint marchSteps;
			bool marchTruncated;
			const float mediumTransmittance = SmokeGridDirectionalTransmittance(receiverPosition,
				lightDirection, marchSteps, marchTruncated);
			transmittanceSamples += mediumTransmittance;
			combinedSamples += visibility * mediumTransmittance;
		}
		const float directionalVisibility = visibleSamples / (float)sampleCount;
		const float directionalTransmittance = transmittanceSamples / (float)sampleCount;
		const float directionalCombined = combinedSamples / (float)sampleCount;
		// Receiver/emitter samples reconstruct visibility only. Radiometry and
		// phase stay on the current occupied froxel so camera rotation cannot
		// resize the apparent carrier envelope or import an old view direction.
		float3 directionalSource = medium.rgb * SmokeDirectionalColor() *
			SmokePhaseResponse(dot(centerDirection, viewRay), phaseRecord.x);
		if (diagnostics && any(directionalSource > 32.0))
			InterlockedAdd(gSmokeControl[0].DirectionalRadianceClamps, 1u);
		directionalSource = min(max(directionalSource, 0.0), 32.0);
		SmokeDirectCacheRecord record = gSmokeDirectCurrent[froxelIndex];
		if (!SmokeDirectRecordValid(record))
		{
			record.Radiance = 0.0;
			record.SigmaT = medium.a;
			record.WorldPosition = SmokeFroxelCenter(froxelCoordinates, ray);
			record.MediumTransmittance = 1.0;
			record.MediumMetadata = SmokeDirectPackMediumMetadata(0u, gSmokeConstants.FrameIndex,
				SmokeDirectSelfShadowBlock(record.WorldPosition));
		}
		const float oldLuminance = dot(max(record.Radiance, 0.0), float3(0.2126, 0.7152, 0.0722));
		const float newLuminance = dot(directionalSource, float3(0.2126, 0.7152, 0.0722));
		const float combinedVisibility = oldLuminance + newLuminance > 1e-6
			? (SmokeDirectRecordVisibility(record) * oldLuminance + directionalVisibility * newLuminance) /
				(oldLuminance + newLuminance)
			: directionalVisibility;
		const float combinedTransmittance = oldLuminance + newLuminance > 1e-6
			? (record.MediumTransmittance * oldLuminance + directionalTransmittance * newLuminance) /
				(oldLuminance + newLuminance)
			: directionalTransmittance;
		const float combinedProduct = oldLuminance + newLuminance > 1e-6
			? (SmokeDirectRecordCombinedVisibility(record) * oldLuminance + directionalCombined * newLuminance) /
				(oldLuminance + newLuminance)
			: directionalCombined;
		record.Radiance += directionalSource;
		record.Metadata = SmokeDirectPackMetadata(1u, gSmokeConstants.FrameIndex, combinedVisibility, combinedProduct);
		record.MediumTransmittance = saturate(combinedTransmittance);
		record.MediumMetadata = SmokeDirectPackMediumMetadata(0u, gSmokeConstants.FrameIndex,
			SmokeDirectSelfShadowBlock(record.WorldPosition));
		gSmokeDirectCurrent[froxelIndex] = record;
		SmokeDirectAccumulateVisibilityDiagnostics(directionalVisibility, sampleCount, diagnostics);
		return;
	}
	uint scratchCount, scratchStride;
	gSmokeIndirectScratch.GetDimensions(scratchCount, scratchStride);
	if (froxelIndex >= scratchCount)
		return;
	const float3 visibleDirectionalScattering = max(gSmokeIndirectScratch[froxelIndex].Radiance, 0.0);
	const float3 centerDirection = SmokeDirectionalDirection();
	const float anisotropy = phaseRecord.x;
	const bool castsShadow = (gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL_SHADOW) != 0u;
	if (gSmokeConstants.LightMode >= 2u && castsShadow && !SmokeShadowTracingReady())
		return;
	const float3 unclamped = visibleDirectionalScattering * SmokeDirectionalColor() *
		SmokePhaseResponse(dot(centerDirection, viewRay), anisotropy);
	if (diagnostics && any(unclamped > 32.0))
		InterlockedAdd(gSmokeControl[0].DirectionalRadianceClamps, 1u);
	const float3 source = gSmokeFroxelSource[froxelIndex].rgb + min(unclamped, 32.0) * gSmokeConstants.RadianceScale;
	uint metadata = SmokeFroxelMetadata(gSmokeFroxelSource[froxelIndex].w);
	metadata = SmokeFroxelResolveRadiance(metadata, gSmokeConstants.SimulationEpoch,
		NRI_SMOKE_FALLBACK_ENVIRONMENT, 0u);
	gSmokeFroxelSource[froxelIndex] = float4(source, SmokeFroxelMetadataValue(metadata));
}
