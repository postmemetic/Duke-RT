#include "Include/SmokeResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeGridLightingResources.hlsli"
#include "Include/SmokeLighting.hlsli"
#include "Include/SmokeVisuals.hlsli"

#define NRI_SMOKE_GRID_MAX_FOOTPRINT_SAMPLES 2u
#define NRI_SMOKE_GRID_MAX_DEPTH_SAMPLES 8u
#define NRI_SMOKE_LIGHT_SOURCE_DORMANT_GRID 0x100u

bool SmokeRenderGridLookup(int3 coordinate, out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	uint hashCount, ignoredStride;
	gSmokeRenderGridHash.GetDimensions(hashCount, ignoredStride);
	if (hashCount == 0u || (hashCount & (hashCount - 1u)) != 0u)
		return false;
	const uint mask = hashCount - 1u;
	const uint base = SmokeGridHashCoordinate(coordinate) & mask;
	[loop]
	for (uint probe = 0u; probe < NRI_SMOKE_GRID_HASH_PROBES; ++probe)
	{
		const SmokeGridHashEntry entry = gSmokeRenderGridHash[(base + probe) & mask];
		if (entry.State == NRI_SMOKE_GRID_EMPTY)
			return false;
		if (entry.State == NRI_SMOKE_GRID_RESIDENT && all(entry.Coordinate == coordinate))
		{
			uint brickCount;
			gSmokeRenderGridBricks.GetDimensions(brickCount, ignoredStride);
			if (entry.BrickIndex < brickCount &&
				gSmokeRenderGridBricks[entry.BrickIndex].Generation == entry.Generation &&
				gSmokeRenderGridBricks[entry.BrickIndex].State == NRI_SMOKE_GRID_RESIDENT)
			{
				brickIndex = entry.BrickIndex;
				return true;
			}
		}
	}
	return false;
}

bool SmokeRenderDormantLookup(int3 coordinate, out uint archiveIndex)
{
	archiveIndex = 0xffffffffu;
	if ((gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DORMANT_GRID) == 0u)
		return false;
	uint controlCount, ignoredStride;
	gSmokeDormantControl.GetDimensions(controlCount, ignoredStride);
	if (controlCount == 0u || gSmokeDormantControl[0].Epoch !=
		gSmokeConstants.SimulationEpoch) return false;
	uint hashCount;
	gSmokeDormantHash.GetDimensions(hashCount, ignoredStride);
	if (hashCount == 0u || (hashCount & (hashCount - 1u)) != 0u) return false;
	const uint mask = hashCount - 1u;
	const uint base = SmokeDormantGridHashCoordinate(coordinate) & mask;
	[loop]
	for (uint probe = 0u; probe < NRI_SMOKE_DORMANT_GRID_HASH_PROBES; ++probe)
	{
		const SmokeDormantGridHashEntry entry = gSmokeDormantHash[(base + probe) & mask];
		if (entry.State == NRI_SMOKE_DORMANT_EMPTY) return false;
		if (entry.State != NRI_SMOKE_DORMANT_RESIDENT ||
			!all(entry.Coordinate == coordinate)) continue;
		uint recordCount;
		gSmokeDormantRecords.GetDimensions(recordCount, ignoredStride);
		if (entry.ArchiveIndex >= recordCount) return false;
		const SmokeDormantGridRecord record = gSmokeDormantRecords[entry.ArchiveIndex];
		if (record.State == NRI_SMOKE_DORMANT_RESIDENT &&
			record.Generation == entry.Generation && record.Epoch ==
			gSmokeConstants.SimulationEpoch && all(record.Coordinate == coordinate))
		{
			archiveIndex = entry.ArchiveIndex;
			return true;
		}
		return false;
	}
	return false;
}

void SmokeRenderGridLoadCell(int3 cell, out float4 scalar, out float4 optical,
	out bool dormant)
{
	scalar = 0.0;
	optical = 0.0;
	dormant = false;
	const int3 brickCoordinate = SmokeGridBrickCoordinate(cell);
	uint brickIndex;
	const uint localIndex = SmokeGridLocalIndex(
		SmokeGridLocalCoordinate(cell, brickCoordinate));
	if (SmokeRenderGridLookup(brickCoordinate, brickIndex))
	{
		const uint cellIndex = brickIndex * NRI_SMOKE_GRID_CELLS_PER_BRICK + localIndex;
		const bool fieldB = gSmokeRenderGridControl[0].FieldPing != 0u;
		scalar = fieldB ? gSmokeRenderGridScalarB[cellIndex] : gSmokeRenderGridScalarA[cellIndex];
		optical = fieldB ? gSmokeRenderGridOpticalB[cellIndex] : gSmokeRenderGridOpticalA[cellIndex];
		return;
	}
	uint archiveIndex;
	if (!SmokeRenderDormantLookup(brickCoordinate, archiveIndex)) return;
	const uint archiveCell = archiveIndex * NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK + localIndex;
	scalar = gSmokeDormantScalar[archiveCell];
	optical = gSmokeDormantOptical[archiveCell];
	dormant = true;
}

void SmokeRenderGridSample(float3 worldPosition, float cellSize,
	out int3 lower, out float3 blend, out float4 scalarCorners[8], out float4 opticalCorners[8],
	out float4 scalar, out float4 optical, out bool dormant)
{
	const float3 gridPosition = worldPosition / cellSize - 0.5;
	lower = (int3)floor(gridPosition);
	blend = frac(gridPosition);
	dormant = false;
	[unroll]
	for (uint i = 0u; i < 8u; ++i)
	{
		const int3 offset = int3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
		bool cornerDormant;
		SmokeRenderGridLoadCell(lower + offset, scalarCorners[i], opticalCorners[i], cornerDormant);
		dormant = dormant || cornerDormant;
	}
	const float4 s00 = lerp(scalarCorners[0], scalarCorners[1], blend.x);
	const float4 s10 = lerp(scalarCorners[2], scalarCorners[3], blend.x);
	const float4 s01 = lerp(scalarCorners[4], scalarCorners[5], blend.x);
	const float4 s11 = lerp(scalarCorners[6], scalarCorners[7], blend.x);
	const float4 o00 = lerp(opticalCorners[0], opticalCorners[1], blend.x);
	const float4 o10 = lerp(opticalCorners[2], opticalCorners[3], blend.x);
	const float4 o01 = lerp(opticalCorners[4], opticalCorners[5], blend.x);
	const float4 o11 = lerp(opticalCorners[6], opticalCorners[7], blend.x);
	scalar = lerp(lerp(s00, s10, blend.y), lerp(s01, s11, blend.y), blend.z);
	optical = lerp(lerp(o00, o10, blend.y), lerp(o01, o11, blend.y), blend.z);
}

bool SmokeRenderGridCorrelatedWorldSource(int3 lower, float3 blend,
	float4 scalarCorners[8], float4 opticalCorners[8], float3 viewRay,
	out float3 correlatedSource, out float3 lobes[6], out float confidence, out float3 phaseApplied)
{
	correlatedSource = 0.0;
	[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe) lobes[lobe] = 0.0;
	confidence = 0.0;
	phaseApplied = 0.0;
	float weightSum = 0.0;
	[unroll]
	for (uint corner = 0u; corner < 8u; ++corner)
	{
		const uint3 offset = uint3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
		const float3 cornerWeight = lerp(1.0 - blend, blend, (float3)offset);
		const float weight = cornerWeight.x * cornerWeight.y * cornerWeight.z;
		if (weight <= 0.0)
			continue;
		uint cellIndex, generation;
		if (!SmokeGridLightCellAddress(lower + (int3)offset, cellIndex, generation) ||
			!SmokeGridLightCornerReachable(lower, offset))
			continue;
		const SmokeGridLightRecord record = SmokeGridLightLoadShadingRecord(cellIndex);
		if (!SmokeGridLightRecordValid(record, generation, gSmokeConstants.SimulationEpoch))
			continue;

		const float4 cornerScalar = scalarCorners[corner];
		const float4 cornerOptical = opticalCorners[corner];
		const float anisotropy = cornerOptical.w > 1e-6 ?
			clamp(cornerScalar.w / cornerOptical.w, -0.95, 0.95) : 0.0;
		float3 cornerPhaseApplied = 0.0;
		[unroll]
		for (uint lobe = 0u; lobe < 6u; ++lobe)
		{
			const float3 mean = SmokeGridLightMean(record, lobe);
			lobes[lobe] += mean * weight;
			cornerPhaseApplied += mean * SmokeHenyeyGreenstein(
				dot((float3)NRI_SMOKE_GRID_LIGHT_LOBE_AXES[lobe], viewRay), anisotropy);
		}
		phaseApplied += cornerPhaseApplied * weight;
		confidence += SmokeGridLightConfidence(record) * weight;
		weightSum += weight;

		const float3 cornerIncident = SmokeGridClampControlledSource(
			cornerPhaseApplied * gSmokeConstants.RadianceScale);
		// Form sigma_s * Li at the authoritative sparse-grid cell. Multiplying
		// independently reconstructed fields attenuates co-located sparse energy
		// and creates false energy between unrelated density/light corners.
		correlatedSource += weight * max(cornerOptical.rgb * gSmokeConstants.DensityScale, 0.0) * cornerIncident;
	}
	if (weightSum <= 0.0)
		return false;
	// Incident-domain debug views retain the established normalized light-field
	// reconstruction. The physical source product above intentionally does not
	// renormalize missing corners: absent or unreachable light is zero energy.
	[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe) lobes[lobe] /= weightSum;
	confidence /= weightSum;
	phaseApplied /= weightSum;
	return true;
}

void SmokeRenderGridIntegrateFroxel(uint3 froxel, float cellSize, out float4 scalar,
	out float4 optical, out float3 source, out bool dormant)
{
	const float sliceNearDepth = SmokeSliceNearDepth(froxel.z);
	const float sliceFarDepth = SmokeSliceFarDepth(froxel.z);
	const float sliceCenterDepth = (sliceNearDepth + sliceFarDepth) * 0.5;
	const float footprintWidth = 2.0 * sliceCenterDepth * gSmokeConstants.TanHalfFovX /
		max((float)gSmokeConstants.FroxelWidth, 1.0);
	const float footprintHeight = 2.0 * sliceCenterDepth * gSmokeConstants.TanHalfFovY /
		max((float)gSmokeConstants.FroxelHeight, 1.0);
	const uint2 footprintSampleCount = uint2(
		footprintWidth > cellSize ? NRI_SMOKE_GRID_MAX_FOOTPRINT_SAMPLES : 1u,
		footprintHeight > cellSize ? NRI_SMOKE_GRID_MAX_FOOTPRINT_SAMPLES : 1u);
	const float3 centerRay = SmokeFroxelRay(froxel.xy);
	const float segmentWorldLength = SmokeWorldSegmentLength(centerRay, sliceNearDepth, sliceFarDepth);
	const uint depthSampleCount = clamp((uint)ceil(segmentWorldLength / cellSize),
		1u, NRI_SMOKE_GRID_MAX_DEPTH_SAMPLES);
	float4 integratedScalar = 0.0;
	float4 integratedOptical = 0.0;
	float3 integratedSource = 0.0;
	float3 integratedWorldDebug = 0.0;
	float3 integratedScatterDebug = 0.0;
	float3 integratedFieldDebug = 0.0;
	dormant = false;
	const uint worldDebugMode = (gSmokeConstants.Flags >> NRI_SMOKE_GRID_LIGHT_DEBUG_SHIFT) & 7u;
	const uint smokeDebugMode = SmokeDebugMode(gSmokeConstants.DebugMode);
	const uint fieldDebugMode = smokeDebugMode >= 12u ? smokeDebugMode - 11u : 0u;
	[loop]
	for (uint footprintY = 0u; footprintY < footprintSampleCount.y; ++footprintY)
	{
		[loop]
		for (uint footprintX = 0u; footprintX < footprintSampleCount.x; ++footprintX)
		{
			const float2 footprintUnit = (float2(footprintX, footprintY) + 0.5) /
				(float2)footprintSampleCount;
			const float2 sampleUv = (float2(froxel.xy) + footprintUnit) /
				float2(max(gSmokeConstants.FroxelWidth, 1u), max(gSmokeConstants.FroxelHeight, 1u));
			[loop]
			for (uint depthSample = 0u; depthSample < depthSampleCount; ++depthSample)
			{
				const float sampleDepthUnit = ((float)depthSample + 0.5) / (float)depthSampleCount;
				const float sampleViewDepth = lerp(sliceNearDepth, sliceFarDepth, sampleDepthUnit);
				const float3 samplePosition = SmokeWorldPosition(sampleUv, sampleViewDepth);
				float4 sampleScalar;
				float4 sampleOptical;
				int3 gridLower;
				float3 gridBlend;
				float4 scalarCorners[8];
				float4 opticalCorners[8];
				bool sampleDormant;
				SmokeRenderGridSample(samplePosition, cellSize, gridLower, gridBlend,
					scalarCorners, opticalCorners, sampleScalar, sampleOptical, sampleDormant);
				dormant = dormant || sampleDormant;
				integratedScalar += sampleScalar;
				integratedOptical += sampleOptical;
				const float sampleExtinction = max(sampleScalar.z * gSmokeConstants.DensityScale, 0.0);
				if (fieldDebugMode >= NRI_SMOKE_FIELD_DEBUG_MASS &&
					fieldDebugMode <= NRI_SMOKE_FIELD_DEBUG_GRADIENT_ORIENTATION &&
					sampleExtinction > 0.0)
				{
					integratedFieldDebug += SmokeVisualLocalDiagnostic(fieldDebugMode,
						sampleScalar, sampleOptical, scalarCorners, gridBlend, cellSize) * sampleExtinction;
				}
				if ((gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_WORLD_ENABLED) != 0u && any(sampleOptical.rgb > 0.0))
				{
					float3 lobes[6];
					float confidence;
					float3 phaseApplied;
					float3 correlatedSource;
					const float3 viewRay = normalize(samplePosition - gSmokeConstants.CameraPosition);
					if (SmokeRenderGridCorrelatedWorldSource(gridLower, gridBlend,
						scalarCorners, opticalCorners, viewRay,
						correlatedSource, lobes, confidence, phaseApplied))
					{
						float3 lobeSum = 0.0;
						[unroll] for (uint lobe = 0u; lobe < 6u; ++lobe) lobeSum += lobes[lobe];
						const float3 incidentContribution = SmokeGridClampControlledSource(
							phaseApplied * gSmokeConstants.RadianceScale);
						integratedSource += correlatedSource;
						if (fieldDebugMode >= NRI_SMOKE_FIELD_DEBUG_LOBE_SUM &&
							fieldDebugMode <= NRI_SMOKE_FIELD_DEBUG_LOBE_CONFIDENCE)
						{
							integratedFieldDebug += SmokeVisualLobeDiagnostic(fieldDebugMode,
								lobes, confidence) * sampleExtinction;
						}
						if (worldDebugMode == 1u) integratedWorldDebug += min(lobeSum * 0.05, 4.0);
						else if (worldDebugMode == 2u) integratedWorldDebug += confidence.xxx;
						else if (worldDebugMode == 3u) integratedWorldDebug += min(abs(lobes[0] - lobes[1]) + abs(lobes[2] - lobes[3]) + abs(lobes[4] - lobes[5]), 4.0);
						else if (worldDebugMode == 4u) integratedWorldDebug += float3(0.0, 1.0, 0.0);
						else if (worldDebugMode == 5u) integratedWorldDebug += min(phaseApplied, 4.0);
						else if (worldDebugMode == 6u) integratedWorldDebug += min(incidentContribution, 4.0);
						else if (worldDebugMode == 7u)
						{
							const int3 keyCell = (int3)floor(samplePosition / cellSize);
							const uint key = SmokeHash(asuint(keyCell.x) ^ SmokeHash(asuint(keyCell.y)) ^ SmokeHash(asuint(keyCell.z)));
							integratedWorldDebug += float3(key & 255u, (key >> 8u) & 255u, (key >> 16u) & 255u) / 255.0;
						}
					}
				}
				if (any(sampleOptical.rgb > 0.0) &&
					(SmokeMultipleScatterScale(gSmokeConstants.DebugMode) > 0.0 || SmokeMultipleScatterDebug(gSmokeConstants.DebugMode) != 0u))
				{
					float3 scatterSource;
					float scatterConfidence;
					const bool seedDebug = SmokeMultipleScatterDebug(gSmokeConstants.DebugMode) == 1u;
					if (SmokeGridScatterSample(samplePosition, cellSize, seedDebug, scatterSource, scatterConfidence))
					{
						if (SmokeMultipleScatterDebug(gSmokeConstants.DebugMode) == 0u)
							integratedSource += scatterSource * SmokeMultipleScatterScale(gSmokeConstants.DebugMode);
						else if (SmokeMultipleScatterDebug(gSmokeConstants.DebugMode) <= 2u)
							integratedScatterDebug += min(scatterSource, 4.0);
						else
							integratedScatterDebug += float3(0.0, scatterConfidence, 0.0);
					}
					else if (SmokeMultipleScatterDebug(gSmokeConstants.DebugMode) == 3u)
						integratedScatterDebug += float3(1.0, 0.0, 0.0);
				}
			}
		}
	}
	const float sampleWeight = rcp((float)(footprintSampleCount.x * footprintSampleCount.y * depthSampleCount));
	scalar = integratedScalar * sampleWeight;
	optical = integratedOptical * sampleWeight;
	if (fieldDebugMode != 0u)
		source = integratedFieldDebug;
	else if (SmokeMultipleScatterDebug(gSmokeConstants.DebugMode) != 0u)
		source = integratedScatterDebug;
	else
		source = worldDebugMode == 0u ? integratedSource : integratedWorldDebug;
	source *= sampleWeight;
	// If a froxel straddles a fine/coarse authority boundary, route its complete
	// medium through the shared receiver-light path. Retaining the partial fine
	// source here would count that fraction a second time.
	if (dormant && fieldDebugMode == 0u && worldDebugMode == 0u &&
		SmokeMultipleScatterDebug(gSmokeConstants.DebugMode) == 0u)
		source = 0.0;
}

void SmokeEvaluateGridFroxel(uint3 dispatchThreadId)
{
	uint controlCount, ignoredStride;
	gSmokeRenderGridControl.GetDimensions(controlCount, ignoredStride);
	const bool dormantEnabled = (gSmokeConstants.LightSourceFlags &
		NRI_SMOKE_LIGHT_SOURCE_DORMANT_GRID) != 0u;
	if (controlCount == 0u || (gSmokeRenderGridControl[0].ResidentCount == 0u &&
		!dormantEnabled))
		return;
	const float cellSize = asfloat(gSmokeRenderGridControl[0].CellSizeBits);
	if (!isfinite(cellSize) || cellSize <= 0.0)
		return;
	const uint froxelIndex = SmokeFroxelIndex(dispatchThreadId.x, dispatchThreadId.y, dispatchThreadId.z);
	float4 scalar;
	float4 optical;
	float3 source;
	bool dormant;
	SmokeRenderGridIntegrateFroxel(dispatchThreadId, cellSize, scalar, optical, source, dormant);
	// Deposition stores density-weighted sigma_t and sigma_s coefficients in
	// inverse world units. Cell size controls sampling support only; dividing the
	// coefficients by it again made the canonical eight-unit grid 8x too faint.
	const float baseExtinction = max(scalar.z * gSmokeConstants.DensityScale, 0.0);
	const float3 baseScattering = max(optical.rgb * gSmokeConstants.DensityScale, 0.0);
	const uint smokeDebugMode = SmokeDebugMode(gSmokeConstants.DebugMode);
	const bool fieldDebug = smokeDebugMode >= 12u;
	float extinction;
	float3 scattering;
	if (fieldDebug)
	{
		extinction = baseExtinction;
		scattering = 0.0;
	}
	else
	{
		float3 shapedSource;
		SmokeVisualShapeMedium(baseExtinction, baseScattering, source,
			extinction, scattering, shapedSource);
		source = shapedSource;
	}
	if (extinction <= 1e-6)
		return;
	const float anisotropy = optical.w > 1e-6 ? clamp(scalar.w / optical.w, -0.95, 0.95) : 0.0;
	gSmokeFroxelMedium[froxelIndex] = float4(scattering, extinction);
	// The fourth phase lane identifies grid materialization to the shared direct
	// light passes. Particle evaluation retains the value 1.
	gSmokeFroxelPhase[froxelIndex] = float4(anisotropy, optical.w, 1.0,
		dormant ? NRI_SMOKE_FROXEL_CARRIER_DORMANT : NRI_SMOKE_FROXEL_CARRIER_GRID);
	uint carrierMetadata = SmokeFroxelCarrierMetadata(gSmokeConstants.SimulationEpoch);
	if (any(source > 0.0))
		carrierMetadata = SmokeFroxelResolveRadiance(carrierMetadata, gSmokeConstants.SimulationEpoch,
			NRI_SMOKE_FALLBACK_WORLD, 0u);
	gSmokeFroxelSource[froxelIndex] = float4(source, SmokeFroxelMetadataValue(carrierMetadata));
	uint occupiedCapacity;
	gSmokeOccupiedFroxelIndices.GetDimensions(occupiedCapacity, ignoredStride);
	uint occupiedSlot = 0u;
	InterlockedAdd(gSmokeControl[0].OccupiedCount, 1u, occupiedSlot);
	if (occupiedSlot < occupiedCapacity)
		gSmokeOccupiedFroxelIndices[occupiedSlot] = froxelIndex;
	else
		InterlockedAdd(gSmokeControl[0].OccupiedOverflow, 1u);
}

#ifndef NRI_SMOKE_EVALUATE_GRID_LIBRARY
[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gSmokeConstants.FroxelWidth ||
		dispatchThreadId.y >= gSmokeConstants.FroxelHeight ||
		dispatchThreadId.z >= gSmokeConstants.FroxelDepth)
		return;
	if ((gSmokeConstants.Flags & NRI_SMOKE_FLAG_COMPARE_REPRESENTATION) != 0u &&
		dispatchThreadId.x < gSmokeConstants.FroxelWidth / 2u)
		return;
	if ((gSmokeConstants.Flags & NRI_SMOKE_FLAG_VIEW_MASK) != 0u)
	{
		const uint columnIndex = dispatchThreadId.y * gSmokeConstants.FroxelWidth + dispatchThreadId.x;
		const uint2 mask = gSmokeViewColumnMasks[columnIndex];
		if ((mask[dispatchThreadId.z >> 5u] & (1u << (dispatchThreadId.z & 31u))) == 0u)
			return;
	}
	SmokeEvaluateGridFroxel(dispatchThreadId);
}
#endif
