#include "Include/SmokeResources.hlsli"

SmokeCellHeader SmokeEmptyCell()
{
	SmokeCellHeader cell = (SmokeCellHeader)0;
	cell.Head = NRI_SMOKE_REFERENCE_END;
	return cell;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	const uint froxelCount = gSmokeConstants.FroxelWidth * gSmokeConstants.FroxelHeight * gSmokeConstants.FroxelDepth;
	const uint expectedWideCellCount = NRI_SMOKE_WIDE_CELL_COUNT * gSmokeConstants.FroxelDepth;
	uint controlCount, particleCount, fineCellCount, wideCellCount;
	uint globalDepthCount, mediumFroxelCount, phaseFroxelCount, sourceFroxelCount;
	uint indirectHistoryCount, indirectScratchCount, emissiveCurrentCount, emissiveTemporalCount, emissiveHistoryCount;
	uint directCurrentCount, directHistoryCount, ignoredStride;
	gSmokeControl.GetDimensions(controlCount, ignoredStride);
	gSmokeParticles.GetDimensions(particleCount, ignoredStride);
	gSmokeFineCells.GetDimensions(fineCellCount, ignoredStride);
	gSmokeWideCells.GetDimensions(wideCellCount, ignoredStride);
	gSmokeGlobalDepthCells.GetDimensions(globalDepthCount, ignoredStride);
	gSmokeFroxelMedium.GetDimensions(mediumFroxelCount, ignoredStride);
	gSmokeFroxelPhase.GetDimensions(phaseFroxelCount, ignoredStride);
	gSmokeFroxelSource.GetDimensions(sourceFroxelCount, ignoredStride);
	gSmokeIndirectHistory.GetDimensions(indirectHistoryCount, ignoredStride);
	gSmokeIndirectScratch.GetDimensions(indirectScratchCount, ignoredStride);
	gSmokeEmissiveCurrent.GetDimensions(emissiveCurrentCount, ignoredStride);
	gSmokeEmissiveTemporal.GetDimensions(emissiveTemporalCount, ignoredStride);
	gSmokeEmissiveHistory.GetDimensions(emissiveHistoryCount, ignoredStride);
	gSmokeDirectCurrent.GetDimensions(directCurrentCount, ignoredStride);
	gSmokeDirectHistory.GetDimensions(directHistoryCount, ignoredStride);

	const bool clearWorld = (gSmokeConstants.Flags & 1u) != 0u;
	const bool particleResourcesAvailable =
		(gSmokeConstants.Flags & NRI_SMOKE_FLAG_GRID_REPRESENTATION) == 0u ||
		(gSmokeConstants.Flags & NRI_SMOKE_FLAG_COMPARE_REPRESENTATION) != 0u;
	const bool clearIndirectCache = clearWorld || (gSmokeConstants.Flags & 0x80u) != 0u;
	const bool clearEmissiveHistory = clearWorld || (gSmokeConstants.Flags & 0x100u) == 0u;
	const bool clearDirectHistory = clearWorld || (gSmokeConstants.Flags & NRI_SMOKE_DIRECT_HISTORY_VALID) == 0u;
	if (index == 0u && controlCount != 0u)
	{
		if (clearWorld)
		{
			const bool preserveInnerRis = (gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_WORLD_ENABLED) != 0u;
			const uint innerRisSets = preserveInnerRis ? gSmokeControl[0].EmissiveInnerRisSets : 0u;
			const uint innerPointProposals = preserveInnerRis ? gSmokeControl[0].EmissiveInnerPointProposals : 0u;
			const uint innerZeroProposals = preserveInnerRis ? gSmokeControl[0].EmissiveInnerZeroProposals : 0u;
			const uint innerRisRejects = preserveInnerRis ? gSmokeControl[0].EmissiveInnerRisRejects : 0u;
			const uint innerSelections = preserveInnerRis ? gSmokeControl[0].EmissiveInnerSelections : 0u;
			const uint innerVisibilityRays = preserveInnerRis ? gSmokeControl[0].EmissiveInnerVisibilityRays : 0u;
			const uint innerSourceVisibilityRays = preserveInnerRis ? gSmokeControl[0].EmissiveInnerSourceVisibilityRays : 0u;
			const uint innerVisibilityVisible = preserveInnerRis ? gSmokeControl[0].EmissiveInnerVisibilityVisible : 0u;
			const uint innerBlockerReceiverImmediate = preserveInnerRis ? gSmokeControl[0].EmissiveInnerBlockerReceiverImmediate : 0u;
			const uint innerBlockerReceiverCell = preserveInnerRis ? gSmokeControl[0].EmissiveInnerBlockerReceiverCell : 0u;
			const uint innerBlockerEmitterCell = preserveInnerRis ? gSmokeControl[0].EmissiveInnerBlockerEmitterCell : 0u;
			const uint innerBlockerInterior = preserveInnerRis ? gSmokeControl[0].EmissiveInnerBlockerInterior : 0u;
			const uint innerSourceSelections = preserveInnerRis ? gSmokeControl[0].EmissiveInnerSourceSelections : 0u;
			const uint innerSourceOverflow = preserveInnerRis ? gSmokeControl[0].EmissiveInnerSourceOverflow : 0u;
			const uint emissiveTargetVisibilityRays = preserveInnerRis ? gSmokeControl[0].EmissiveTargetVisibilityRays : 0u;
			const uint emissiveTargetVisibilityVisible = preserveInnerRis ? gSmokeControl[0].EmissiveTargetVisibilityVisible : 0u;
			const uint emissiveTargetBlockerExact = preserveInnerRis ? gSmokeControl[0].EmissiveTargetBlockerExact : 0u;
			const uint emissiveTargetBlockerRange = preserveInnerRis ? gSmokeControl[0].EmissiveTargetBlockerRange : 0u;
			const uint emissiveTargetBlockerOther = preserveInnerRis ? gSmokeControl[0].EmissiveTargetBlockerOther : 0u;
			const uint emissiveTargetWitnessClaim = preserveInnerRis ? gSmokeControl[0].EmissiveTargetWitnessClaim : 0u;
			const uint emissiveTargetWitnessCandidate = preserveInnerRis ? gSmokeControl[0].EmissiveTargetWitnessCandidate : 0xffffffffu;
			const uint emissiveTargetWitnessRelation = preserveInnerRis ? gSmokeControl[0].EmissiveTargetWitnessRelation : 0u;
			const uint emissiveTargetWitnessSamplePrimitive = preserveInnerRis ? gSmokeControl[0].EmissiveTargetWitnessSamplePrimitive : 0xffffffffu;
			const uint emissiveTargetWitnessSampleMaterial = preserveInnerRis ? gSmokeControl[0].EmissiveTargetWitnessSampleMaterial : 0xffffffffu;
			const uint emissiveTargetWitnessBlockerDataSource = preserveInnerRis ? gSmokeControl[0].EmissiveTargetWitnessBlockerDataSource : 0xffffffffu;
			const uint emissiveTargetWitnessBlockerInstance = preserveInnerRis ? gSmokeControl[0].EmissiveTargetWitnessBlockerInstance : 0xffffffffu;
			const uint emissiveTargetWitnessBlockerPrimitive = preserveInnerRis ? gSmokeControl[0].EmissiveTargetWitnessBlockerPrimitive : 0xffffffffu;
			const uint emissiveTargetWitnessBlockerMaterial = preserveInnerRis ? gSmokeControl[0].EmissiveTargetWitnessBlockerMaterial : 0xffffffffu;
			const uint emissiveTargetWitnessDistanceBits = preserveInnerRis ? gSmokeControl[0].EmissiveTargetWitnessDistanceBits : 0u;
			SmokeControl control = (SmokeControl)0;
			control.Epoch = gSmokeConstants.SimulationEpoch;
			control.MaximumCandidatesPerFroxel = 0u;
			control.EmissiveInnerRisSets = innerRisSets;
			control.EmissiveInnerPointProposals = innerPointProposals;
			control.EmissiveInnerZeroProposals = innerZeroProposals;
			control.EmissiveInnerRisRejects = innerRisRejects;
			control.EmissiveInnerSelections = innerSelections;
			control.EmissiveInnerVisibilityRays = innerVisibilityRays;
			control.EmissiveInnerSourceVisibilityRays = innerSourceVisibilityRays;
			control.EmissiveInnerVisibilityVisible = innerVisibilityVisible;
			control.EmissiveInnerBlockerReceiverImmediate = innerBlockerReceiverImmediate;
			control.EmissiveInnerBlockerReceiverCell = innerBlockerReceiverCell;
			control.EmissiveInnerBlockerEmitterCell = innerBlockerEmitterCell;
			control.EmissiveInnerBlockerInterior = innerBlockerInterior;
			control.EmissiveInnerSourceSelections = innerSourceSelections;
			control.EmissiveInnerSourceOverflow = innerSourceOverflow;
			control.EmissiveTargetVisibilityRays = emissiveTargetVisibilityRays;
			control.EmissiveTargetVisibilityVisible = emissiveTargetVisibilityVisible;
			control.EmissiveTargetBlockerExact = emissiveTargetBlockerExact;
			control.EmissiveTargetBlockerRange = emissiveTargetBlockerRange;
			control.EmissiveTargetBlockerOther = emissiveTargetBlockerOther;
			control.EmissiveTargetWitnessClaim = emissiveTargetWitnessClaim;
			control.EmissiveTargetWitnessCandidate = emissiveTargetWitnessCandidate;
			control.EmissiveTargetWitnessRelation = emissiveTargetWitnessRelation;
			control.EmissiveTargetWitnessSamplePrimitive = emissiveTargetWitnessSamplePrimitive;
			control.EmissiveTargetWitnessSampleMaterial = emissiveTargetWitnessSampleMaterial;
			control.EmissiveTargetWitnessBlockerDataSource = emissiveTargetWitnessBlockerDataSource;
			control.EmissiveTargetWitnessBlockerInstance = emissiveTargetWitnessBlockerInstance;
			control.EmissiveTargetWitnessBlockerPrimitive = emissiveTargetWitnessBlockerPrimitive;
			control.EmissiveTargetWitnessBlockerMaterial = emissiveTargetWitnessBlockerMaterial;
			control.EmissiveTargetWitnessDistanceBits = emissiveTargetWitnessDistanceBits;
			gSmokeControl[0] = control;
		}
		else
		{
			gSmokeControl[0].WideParticlesProjected = 0u;
			gSmokeControl[0].WideGlobalDrops = 0u;
			gSmokeControl[0].FineColumnReferences = 0u;
			gSmokeControl[0].WideCellReferences = 0u;
			gSmokeControl[0].GlobalDepthReferences = 0u;
			gSmokeControl[0].ReferenceInvalidLinks = 0u;
			gSmokeControl[0].ReferenceTraversalLimitExits = 0u;
			gSmokeControl[0].FineTierParticles = 0u;
			gSmokeControl[0].WideTierParticles = 0u;
			gSmokeControl[0].GlobalTierParticles = 0u;
			gSmokeControl[0].FineOccupiedCells = 0u;
			gSmokeControl[0].WideOccupiedCells = 0u;
			gSmokeControl[0].GlobalOccupiedSlices = 0u;
			gSmokeControl[0].FineMaximumCellReferences = 0u;
			gSmokeControl[0].WideMaximumCellReferences = 0u;
			gSmokeControl[0].GlobalMaximumCellReferences = 0u;
			gSmokeControl[0].MaximumDepthSpan = 0u;
			gSmokeControl[0].DepthSpanOne = 0u;
			gSmokeControl[0].DepthSpanTwoToFour = 0u;
			gSmokeControl[0].DepthSpanFiveToSixteen = 0u;
			gSmokeControl[0].DepthSpanOverSixteen = 0u;
			gSmokeControl[0].MaximumCandidatesPerFroxel = 0u;
			gSmokeControl[0].OccupiedCount = 0u;
			gSmokeControl[0].OccupiedOverflow = 0u;
			gSmokeControl[0].MediumCandidateTests = 0u;
			gSmokeControl[0].PointFroxelsProcessed = 0u;
			gSmokeControl[0].DirectionalFroxelsProcessed = 0u;
			gSmokeControl[0].DirectionalSamples = 0u;
			gSmokeControl[0].DirectionalShadowRays = 0u;
			gSmokeControl[0].DirectionalShadowVisible = 0u;
			gSmokeControl[0].DirectionalShadowOccluded = 0u;
			gSmokeControl[0].DirectionalRadianceClamps = 0u;
			gSmokeControl[0].EmissiveFroxelsProcessed = 0u;
			gSmokeControl[0].EmissiveSamples = 0u;
			gSmokeControl[0].EmissiveCandidateMisses = 0u;
			gSmokeControl[0].EmissiveDistanceRejected = 0u;
			gSmokeControl[0].EmissiveFacingRejected = 0u;
			gSmokeControl[0].EmissiveShadowRays = 0u;
			gSmokeControl[0].EmissiveShadowVisible = 0u;
			gSmokeControl[0].EmissiveShadowOccluded = 0u;
			gSmokeControl[0].EmissiveContributed = 0u;
			gSmokeControl[0].EmissiveRadianceClamps = 0u;
			gSmokeControl[0].EmissiveReservoirInitial = 0u;
			gSmokeControl[0].EmissiveReservoirInvalid = 0u;
			gSmokeControl[0].EmissiveTemporalAccepted = 0u;
			gSmokeControl[0].EmissiveTemporalRejected = 0u;
			gSmokeControl[0].EmissiveSpatialAccepted = 0u;
			gSmokeControl[0].EmissiveSpatialRejected = 0u;
			gSmokeControl[0].EmissiveFinalEvaluations = 0u;
			gSmokeControl[0].EmissiveSourceClamps = 0u;
			gSmokeControl[0].EmissiveRemovedEnergy = 0u;
			gSmokeControl[0].EmissiveMaximumAge = 0u;
			gSmokeControl[0].EmissiveReferenceSamples = 0u;
			gSmokeControl[0].EmissiveReferenceRays = 0u;
			gSmokeControl[0].EmissiveIdentityRejects = 0u;
			gSmokeControl[0].IndirectFroxelsProcessed = 0u;
			gSmokeControl[0].IndirectLocalityRays = 0u;
			gSmokeControl[0].IndirectLocalityAgreement = 0u;
			gSmokeControl[0].IndirectLocalityOneSided = 0u;
			gSmokeControl[0].IndirectLocalityMismatch = 0u;
			gSmokeControl[0].IndirectLocalityInvalid = 0u;
			gSmokeControl[0].IndirectReferenceRays = 0u;
			gSmokeControl[0].IndirectReferenceHits = 0u;
			gSmokeControl[0].IndirectReferenceMisses = 0u;
			gSmokeControl[0].IndirectSectorContributions = 0u;
			gSmokeControl[0].IndirectSkyContributions = 0u;
			gSmokeControl[0].IndirectEmissionContributions = 0u;
			gSmokeControl[0].IndirectRadianceClamps = 0u;
			gSmokeControl[0].IndirectNanRejects = 0u;
			gSmokeControl[0].IndirectTemporalAccepted = 0u;
			gSmokeControl[0].IndirectTemporalRejected = 0u;
			gSmokeControl[0].IndirectSpatialAccepted = 0u;
			gSmokeControl[0].IndirectSpatialRejected = 0u;
			gSmokeControl[0].IndirectCacheMaximumAge = 0u;
			gSmokeControl[0].IndirectCacheClamps = 0u;
			gSmokeControl[0].IndirectCacheResolved = 0u;
			gSmokeControl[0].DirectReceiverSamples = 0u;
			gSmokeControl[0].DirectFractionalVisibility = 0u;
			gSmokeControl[0].DirectVisibilityZero = 0u;
			gSmokeControl[0].DirectVisibilityOne = 0u;
			gSmokeControl[0].DirectTemporalAccepted = 0u;
			gSmokeControl[0].DirectTemporalRejected = 0u;
			gSmokeControl[0].DirectSpatialAccepted = 0u;
			gSmokeControl[0].DirectSpatialRejected = 0u;
			gSmokeControl[0].DirectHistoryMaximumAge = 0u;
			gSmokeControl[0].DirectHistoryResolved = 0u;
			gSmokeControl[0].DirectHistoryClamps = 0u;
			gSmokeControl[0].DirectNanRejects = 0u;
			gSmokeControl[0].AnalyticLightBuildEvents = 0u;
			gSmokeControl[0].AnalyticLightAnchorsBuilt = 0u;
			gSmokeControl[0].AnalyticLightAnchorsValid = 0u;
			gSmokeControl[0].AnalyticLightAnchorsInvalid = 0u;
			gSmokeControl[0].AnalyticLightSamplesRequested = 0u;
			gSmokeControl[0].AnalyticLightSamplesExecuted = 0u;
			gSmokeControl[0].AnalyticLightEvaluations = 0u;
			gSmokeControl[0].AnalyticLightBuildVisibilityRays = 0u;
			gSmokeControl[0].AnalyticLightGridSeedHits = 0u;
			gSmokeControl[0].AnalyticLightGridSeedMisses = 0u;
			gSmokeControl[0].AnalyticLightApplyFroxelsTested = 0u;
			gSmokeControl[0].AnalyticLightApplyFroxelsApplied = 0u;
			gSmokeControl[0].AnalyticLightCarrierContributions = 0u;
			gSmokeControl[0].AnalyticLightAnchorBlendTaps = 0u;
			gSmokeControl[0].AnalyticLightGroupCacheHits = 0u;
			gSmokeControl[0].AnalyticLightMissingGroupRecords = 0u;
			gSmokeControl[0].AnalyticLightIdentityRejects = 0u;
			gSmokeControl[0].AnalyticLightApplyVisibilityRays = 0u;
			gSmokeControl[0].LightCandidatesTested = 0u;
			gSmokeControl[0].LightDistanceRejected = 0u;
			gSmokeControl[0].LightShadowRays = 0u;
			gSmokeControl[0].LightShadowVisible = 0u;
			gSmokeControl[0].LightShadowOccluded = 0u;
			gSmokeControl[0].LightSoftSamples = 0u;
			gSmokeControl[0].LightRadianceClamps = 0u;
			gSmokeControl[0].FilterCandidateHits = 0u;
			gSmokeControl[0].FilterAlphaRejects = 0u;
			gSmokeControl[0].FilterNoShadowRejects = 0u;
			gSmokeControl[0].FilterOneWayRejects = 0u;
			gSmokeControl[0].FilterReflectionRejects = 0u;
			gSmokeControl[0].FilterPortalContinuations = 0u;
			gSmokeControl[0].FilterAcceptedBlockers = 0u;
			gSmokeControl[0].FilterMisses = 0u;
			gSmokeControl[0].FilterSkipLimitExits = 0u;
			gSmokeControl[0].FilterContinuationLimitExits = 0u;
			gSmokeControl[0].FilterResourceDowngrades = 0u;
			if ((gSmokeConstants.FilteredVisibilityEnabled & NRI_SMOKE_VISIBILITY_FILTERED_REQUESTED) != 0u &&
				(gSmokeConstants.FilteredVisibilityEnabled & NRI_SMOKE_VISIBILITY_FILTERED_EFFECTIVE) == 0u && gSmokeConstants.LightMode >= 2u)
				gSmokeControl[0].FilterResourceDowngrades = 1u;
		}
	}
	if (particleResourcesAvailable && clearWorld && index < min(gSmokeConstants.ParticleCapacity, particleCount))
	{
		SmokeParticle particle = (SmokeParticle)0;
		particle.Epoch = gSmokeConstants.SimulationEpoch;
		gSmokeParticles[index] = particle;
	}
	if (particleResourcesAvailable && index < min(froxelCount, fineCellCount))
		gSmokeFineCells[index] = SmokeEmptyCell();
	if (particleResourcesAvailable && index < min(expectedWideCellCount, wideCellCount))
		gSmokeWideCells[index] = SmokeEmptyCell();
	if (particleResourcesAvailable && index < min(gSmokeConstants.FroxelDepth, globalDepthCount))
		gSmokeGlobalDepthCells[index] = SmokeEmptyCell();
	if (index < min(froxelCount, min(mediumFroxelCount, min(phaseFroxelCount, sourceFroxelCount))))
	{
		gSmokeFroxelMedium[index] = 0.0;
		gSmokeFroxelPhase[index] = 0.0;
		gSmokeFroxelSource[index] = 0.0;
	}
	if (clearIndirectCache && index < min(froxelCount, min(indirectHistoryCount, indirectScratchCount)))
	{
		SmokeIndirectCacheRecord emptyRecord = (SmokeIndirectCacheRecord)0;
		gSmokeIndirectHistory[index] = emptyRecord;
		gSmokeIndirectScratch[index] = emptyRecord;
	}
	if (index < min(froxelCount, min(emissiveCurrentCount, emissiveTemporalCount)))
	{
		SmokeEmissiveStorageRecord emptyRecord = (SmokeEmissiveStorageRecord)0;
		emptyRecord.Data0.x = 0xffffffffu;
		gSmokeEmissiveCurrent[index] = emptyRecord;
		gSmokeEmissiveTemporal[index] = emptyRecord;
	}
	if (clearEmissiveHistory && index < min(froxelCount, emissiveHistoryCount))
	{
		SmokeEmissiveStorageRecord emptyRecord = (SmokeEmissiveStorageRecord)0;
		emptyRecord.Data0.x = 0xffffffffu;
		gSmokeEmissiveHistory[index] = emptyRecord;
	}
	if (index < min(froxelCount, directCurrentCount))
	{
		SmokeDirectCacheRecord emptyRecord = (SmokeDirectCacheRecord)0;
		gSmokeDirectCurrent[index] = emptyRecord;
	}
	if (clearDirectHistory && index < min(froxelCount, directHistoryCount))
	{
		SmokeDirectCacheRecord emptyRecord = (SmokeDirectCacheRecord)0;
		gSmokeDirectHistory[index] = emptyRecord;
	}
}
