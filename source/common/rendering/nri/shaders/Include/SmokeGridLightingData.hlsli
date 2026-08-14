#ifndef NRI_SMOKE_GRID_LIGHTING_DATA_HLSLI
#define NRI_SMOKE_GRID_LIGHTING_DATA_HLSLI

#define NRI_SMOKE_GRID_LIGHT_LOBE_COUNT 6u
#define NRI_SMOKE_GRID_LIGHT_RECORD_WORDS 24u
#define NRI_SMOKE_GRID_LIGHT_MAX_HISTORY 64u
#define NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY 16u
#define NRI_SMOKE_GRID_LIGHT_LOCAL_MIX 0.75
#define NRI_SMOKE_GRID_SCATTER_PROBE_AXIS 4u
#define NRI_SMOKE_GRID_SCATTER_PROBES_PER_BRICK 64u
#define NRI_SMOKE_GRID_SCATTER_VALID 0x1u
#define NRI_SMOKE_GRID_SCATTER_FACE_SHIFT 1u
#define NRI_SMOKE_GRID_SCATTER_FACE_MASK 0x7eu
#define NRI_SMOKE_GRID_SCATTER_EXPLICIT_ZERO 0x80u
#define NRI_SMOKE_GRID_SCATTER_SPLIT_BLOCKED 0x100u

#define NRI_SMOKE_GRID_LIGHT_EVIDENCE_SUPPORT 0x1u
#define NRI_SMOKE_GRID_LIGHT_EVIDENCE_PHYSICAL_ZERO 0x2u
#define NRI_SMOKE_GRID_LIGHT_EVIDENCE_VISIBLE 0x4u
#define NRI_SMOKE_GRID_LIGHT_EVIDENCE_VALID 0x80u

struct SmokeGridLightRecord
{
	uint Words[NRI_SMOKE_GRID_LIGHT_RECORD_WORDS];
};

struct SmokeGridLightControl
{
	uint ActiveCount;
	uint SupportCount;
	uint SourceCount;
	uint SupportOnlyCount;
	uint DuplicateCount;
	uint SupportOverflowCount;
	uint ScheduledCount;
	uint Samples;
	uint Visible;
	uint PhysicalZero;
	uint Missing;
	uint StructuralErrors;
	uint OverflowRejects;
	uint TemporalAccepted;
	uint TemporalRejected;
	uint LinksOpen;
	uint LinksBlocked;
	uint LinksStale;
	uint CornerAccepted;
	uint CornerRejected;
	uint FilterAccepted;
	uint FilterRejected;
	uint MaximumAge;
	uint FrameStamp;
	uint SimulationEpoch;
	uint FieldPing;
	uint Flags;
	uint ProposalListsBuilt;
	uint ProposalCandidatesTested;
	uint ProposalCandidatesAccepted;
	uint ProposalLocalSamples;
	uint ProposalGlobalSamples;
	uint ProposalFallbacks;
	uint ProposalTruncations;
	uint ProposalMaximumCount;
	uint ScatterActiveCount;
	uint ScatterSeededCount;
	uint ScatterNonzeroSourceCount;
	uint ScatterZeroSourceCount;
	uint ScatterPointCells;
	uint ScatterDirectionalCells;
	uint ScatterEmissiveCells;
	uint ScatterEnvironmentCells;
	uint ScatterIterations;
	uint ScatterFinalPing;
	uint ScatterNeighborTests;
	uint ScatterNeighborsAccepted;
	uint ScatterNeighborsBlocked;
	uint ScatterNeighborsStale;
	uint ScatterInternalBlocked;
	uint ScatterNanRejects;
	uint ScatterActiveOverflow;
	uint ScatterReconstructionAccepted;
	uint ScatterReconstructionRejected;
	uint ScatterSourceEnergyQ;
	uint ScatterTransportedEnergyQ;
	uint ScatterRemovedEnergyQ;
	uint ScatterReceiverApplications;
	uint ScatterExactZero;
	uint ScatterProbeCapacity;
	uint ScatterProbesPerBrick;
	uint ScatterFrameStamp;
	uint ScatterEpoch;
	uint ScatterFlags;
	uint3 ScatterPadding;
	uint SelfShadowSamples;
	uint SelfShadowSteps;
	uint SelfShadowTruncated;
	uint SelfShadowTransmittanceZero;
	uint SelfShadowTransmittancePartial;
	uint SelfShadowTransmittanceOne;
	uint SelfShadowNanRejects;
	uint SelfShadowHistoryAccepted;
	uint SelfShadowHistoryRestarted;
	uint SelfShadowMaximumAge;
	uint TopologyMissingTlas;
	uint TopologyAsymmetric;
	uint ExplicitZeroProbes;
	uint SplitBlockedProbes;
	uint2 SelfShadowPadding;
	uint RadiancePartitionCount;
	uint RadianceNewInvalidQuantity;
	uint RadianceMaintenanceQuantity;
	uint RadianceMaximumAge;
	uint RadianceNewInvalidRequested;
	uint RadianceNewInvalidScheduled;
	uint RadianceNewInvalidDeferred;
	uint RadianceMaintenanceRequested;
	uint RadianceMaintenanceScheduled;
	uint RadianceMaintenanceDeferred;
	uint RadianceHistoryRetained;
	uint RadianceHistoryMissing;
	uint RadianceAgeOverflows;
	uint RadianceNewInvalidTickets;
	uint RadianceMaintenanceTickets;
};

struct SmokeGridLightProposal
{
	uint CandidateIndices[NRI_SMOKE_GRID_LIGHT_PROPOSAL_CAPACITY];
	uint Count;
	uint BrickGeneration;
	uint SimulationEpoch;
	uint FrameStamp;
};

struct SmokeGridLightSupportStamp
{
	uint BrickGeneration;
	uint FrameStamp;
};

struct SmokeGridScatterMetadata
{
	uint BrickGeneration;
	uint SimulationEpoch;
	uint FrameStamp;
	uint Flags;
	uint HistoryBlock;
	uint HistoryCount;
	uint TransmittanceQ;
	uint Reserved;
};

uint SmokeGridLightPackHalf2(float2 value)
{
	return f32tof16(value.x) | (f32tof16(value.y) << 16u);
}

float2 SmokeGridLightUnpackHalf2(uint value)
{
	return float2(f16tof32(value & 0xffffu), f16tof32(value >> 16u));
}

float SmokeGridLightLoadHalf(SmokeGridLightRecord record, uint halfIndex)
{
	const float2 pair = SmokeGridLightUnpackHalf2(record.Words[halfIndex >> 1u]);
	return (halfIndex & 1u) == 0u ? pair.x : pair.y;
}

void SmokeGridLightStoreHalf(inout SmokeGridLightRecord record, uint halfIndex, float value)
{
	const uint wordIndex = halfIndex >> 1u;
	float2 pair = SmokeGridLightUnpackHalf2(record.Words[wordIndex]);
	if ((halfIndex & 1u) == 0u) pair.x = value; else pair.y = value;
	record.Words[wordIndex] = SmokeGridLightPackHalf2(pair);
}

float3 SmokeGridLightMean(SmokeGridLightRecord record, uint lobe)
{
	const uint base = min(lobe, 5u) * 3u;
	return float3(SmokeGridLightLoadHalf(record, base), SmokeGridLightLoadHalf(record, base + 1u), SmokeGridLightLoadHalf(record, base + 2u));
}

float3 SmokeGridLightSecondMoment(SmokeGridLightRecord record, uint lobe)
{
	const uint base = min(lobe, 5u) * 3u;
	const uint word = 12u + (base >> 1u);
	const float2 first = SmokeGridLightUnpackHalf2(record.Words[word]);
	const float2 second = SmokeGridLightUnpackHalf2(record.Words[12u + ((base + 2u) >> 1u)]);
	return (base & 1u) == 0u ? float3(first.x, first.y, second.x) : float3(first.y, second.x, second.y);
}

void SmokeGridLightStoreLobe(inout SmokeGridLightRecord record, uint lobe, float3 mean, float3 secondMoment)
{
	const uint meanBase = min(lobe, 5u) * 3u;
	[unroll]
	for (uint channel = 0u; channel < 3u; ++channel)
	{
		SmokeGridLightStoreHalf(record, meanBase + channel, mean[channel]);
		const uint momentHalf = meanBase + channel;
		const uint momentWord = 12u + (momentHalf >> 1u);
		float2 pair = SmokeGridLightUnpackHalf2(record.Words[momentWord]);
		if ((momentHalf & 1u) == 0u) pair.x = secondMoment[channel]; else pair.y = secondMoment[channel];
		record.Words[momentWord] = SmokeGridLightPackHalf2(pair);
	}
}

uint SmokeGridLightBrickGeneration(SmokeGridLightRecord record) { return record.Words[9]; }
uint SmokeGridLightSimulationEpoch(SmokeGridLightRecord record) { return record.Words[10]; }
uint SmokeGridLightSampleCount(SmokeGridLightRecord record) { return record.Words[11] & 0xffu; }
uint SmokeGridLightSequence(SmokeGridLightRecord record) { return (record.Words[11] >> 8u) & 0xffu; }
float SmokeGridLightConfidence(SmokeGridLightRecord record) { return (float)((record.Words[11] >> 16u) & 0xffu) / 255.0; }
uint SmokeGridLightEvidence(SmokeGridLightRecord record) { return (record.Words[11] >> 24u) & 0xffu; }
uint SmokeGridLightLastUpdate(SmokeGridLightRecord record) { return record.Words[21] & 0xffffu; }
uint SmokeGridLightAge(SmokeGridLightRecord record) { return record.Words[21] >> 16u; }
uint SmokeGridLightSelfShadowBlock(SmokeGridLightRecord record) { return record.Words[22]; }
float SmokeGridLightMeanTransmittance(SmokeGridLightRecord record) { return SmokeGridLightUnpackHalf2(record.Words[23]).x; }

void SmokeGridLightSetSelfShadowEvidence(inout SmokeGridLightRecord record, uint block, float meanTransmittance)
{
	record.Words[22] = block;
	record.Words[23] = SmokeGridLightPackHalf2(float2(saturate(meanTransmittance), 0.0));
}

void SmokeGridLightSetMetadata(inout SmokeGridLightRecord record, uint generation, uint epoch,
	uint sampleCount, uint sequence, float confidence, uint evidence, uint frameIndex, uint age)
{
	record.Words[9] = generation;
	record.Words[10] = epoch;
	record.Words[11] = min(sampleCount, 255u) | (min(sequence, 255u) << 8u) |
		((uint)round(saturate(confidence) * 255.0) << 16u) | ((evidence & 0xffu) << 24u);
	record.Words[21] = (frameIndex & 0xffffu) | (min(age, 65535u) << 16u);
	record.Words[22] = 0u;
	record.Words[23] = 0u;
}

bool SmokeGridLightRecordValid(SmokeGridLightRecord record, uint generation, uint epoch)
{
	return SmokeGridLightBrickGeneration(record) == generation && SmokeGridLightSimulationEpoch(record) == epoch &&
		(SmokeGridLightEvidence(record) & (NRI_SMOKE_GRID_LIGHT_EVIDENCE_SUPPORT | NRI_SMOKE_GRID_LIGHT_EVIDENCE_VALID)) ==
		(NRI_SMOKE_GRID_LIGHT_EVIDENCE_SUPPORT | NRI_SMOKE_GRID_LIGHT_EVIDENCE_VALID);
}

#endif
