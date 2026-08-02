#ifndef NRI_SMOKE_EMISSIVE_RESERVOIR_HLSLI
#define NRI_SMOKE_EMISSIVE_RESERVOIR_HLSLI

#include "SmokeResources.hlsli"
#include "SmokeFroxel.hlsli"
#include "SmokeLighting.hlsli"
#include "SmokeIndirectCache.hlsli"

#define NRI_SMOKE_EMISSIVE_HISTORY_VALID 0x100u
#define NRI_SMOKE_EMISSIVE_REUSE_SHIFT 9u
#define NRI_SMOKE_EMISSIVE_REUSE_MASK 3u
#define NRI_SMOKE_EMISSIVE_REFERENCE 0x800u
#define NRI_SMOKE_EMISSIVE_RECORD_VALID 0x80000000u
#define NRI_SMOKE_EMISSIVE_MOMENT_RECORD 0x40000000u
#define NRI_SMOKE_EMISSIVE_LEGACY_GATHER_DISABLED 0x1000000u
#define NRI_SMOKE_EMISSIVE_QUARTER_KEY 0x2000000u

struct SmokeEmissiveSampleIdentity
{
	uint CandidateIndex;
	uint DataSource;
	uint SceneInstanceIndex;
	uint RangeBase;
	uint RangeCount;
	uint PrimitiveIndex;
	uint MaterialIndex;
};

SmokeEmissiveSampleIdentity SmokeEmptyEmissiveSampleIdentity()
{
	SmokeEmissiveSampleIdentity identity = (SmokeEmissiveSampleIdentity)0;
	identity.CandidateIndex = 0xffffffffu;
	identity.DataSource = 0xffffffffu;
	identity.SceneInstanceIndex = 0xffffffffu;
	identity.RangeBase = 0xffffffffu;
	identity.PrimitiveIndex = 0xffffffffu;
	identity.MaterialIndex = 0xffffffffu;
	return identity;
}

uint SmokeEmissivePointCandidateCount()
{
	return clamp((gSmokeConstants.FilteredVisibilityEnabled >> NRI_SMOKE_EMISSIVE_POINT_CANDIDATE_SHIFT) &
		NRI_SMOKE_EMISSIVE_POINT_CANDIDATE_MASK, 1u, 8u);
}

uint SmokeEmissiveDiagnosticCandidate()
{
	const uint code = gSmokeConstants.FilteredVisibilityEnabled >> 16u;
	return code != 0u ? code - 1u : 0xffffffffu;
}

bool SmokeEmissiveGridFroxel(float4 phase)
{
	return (gSmokeConstants.Flags & NRI_SMOKE_DIRECT_GRID_ENABLED) != 0u &&
		SmokeFroxelHasGridCarrier(phase) && !SmokeFroxelHasAnalyticCarrier(phase) &&
		!SmokeFroxelHasParticleCarrier(phase);
}

bool SmokeEmissiveWorldFieldOwnsGrid(float4 phase)
{
	return SmokeEmissiveGridFroxel(phase) && (gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_WORLD_ENABLED) != 0u;
}

bool SmokeAnalyticCarrierReservoirOwns(float4 phase)
{
	return (gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_WORLD_ENABLED) != 0u &&
		(gSmokeConstants.Flags & NRI_SMOKE_FLAG_COMPARE_REPRESENTATION) == 0u &&
		(gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_REFERENCE) == 0u &&
		SmokeFroxelHasAnalyticCarrier(phase) && !SmokeFroxelHasParticleCarrier(phase);
}

float4 SmokeEmissiveReceiverMedium(uint froxelIndex, float4 phase, float4 combinedMedium)
{
	// Fine-grid world lighting and analytic admission fields each retain their
	// ownership. Dormant archive medium has no world field, so a mixed dormant /
	// analytic froxel sends only the non-analytic remainder through the shared
	// receiver pass. Pure dormant medium uses the common profile-specific path.
	if (SmokeFroxelHasDormantCarrier(phase) && SmokeFroxelHasAnalyticCarrier(phase))
	{
		uint count, stride;
		gSmokeAnalyticFroxelMedium.GetDimensions(count, stride);
		if (froxelIndex < count)
			return max(combinedMedium - gSmokeAnalyticFroxelMedium[froxelIndex], 0.0);
	}
	// The world field already supplied the fine-grid portion. A mixed froxel
	// therefore runs receiver emissive only for its analytic coefficient.
	if ((gSmokeConstants.Flags & NRI_SMOKE_GRID_LIGHT_WORLD_ENABLED) != 0u &&
		SmokeFroxelHasGridCarrier(phase) && SmokeFroxelHasAnalyticCarrier(phase))
	{
		uint count, stride;
		gSmokeAnalyticFroxelMedium.GetDimensions(count, stride);
		if (froxelIndex < count)
			return gSmokeAnalyticFroxelMedium[froxelIndex];
	}
	return combinedMedium;
}

uint SmokeEmissiveLaneCount()
{
	return 1u << min((gSmokeConstants.Flags >> 5u) & 3u, 2u);
}

SmokeEmissiveStorageRecord SmokePackEmissiveReservoir(SmokeEmissiveReservoirRecord record)
{
	SmokeEmissiveStorageRecord storage;
	storage.Data0 = uint4(record.CandidateIndex, record.SampleSeed, record.StableKeyLo, record.StableKeyHi);
	storage.Data1 = uint4(asuint(record.Target), asuint(record.WeightSum), record.Metadata, record.Generation);
	storage.Data2 = uint4(asuint(record.ReceiverPosition), asuint(record.SigmaT));
	return storage;
}

SmokeEmissiveReservoirRecord SmokeUnpackEmissiveReservoir(SmokeEmissiveStorageRecord storage)
{
	SmokeEmissiveReservoirRecord record;
	record.CandidateIndex = storage.Data0.x;
	record.SampleSeed = storage.Data0.y;
	record.StableKeyLo = storage.Data0.z;
	record.StableKeyHi = storage.Data0.w;
	record.Target = asfloat(storage.Data1.x);
	record.WeightSum = asfloat(storage.Data1.y);
	record.Metadata = storage.Data1.z;
	record.Generation = storage.Data1.w;
	record.ReceiverPosition = asfloat(storage.Data2.xyz);
	record.SigmaT = asfloat(storage.Data2.w);
	return record;
}

SmokeEmissiveLaneRecord SmokeEmptyEmissiveLane()
{
	SmokeEmissiveLaneRecord lane = (SmokeEmissiveLaneRecord)0;
	lane.CandidateIndex = 0xffffffffu;
	return lane;
}

bool SmokeEmissiveLaneValid(SmokeEmissiveLaneRecord lane)
{
	return lane.CandidateIndex != 0xffffffffu && isfinite(lane.Target) && lane.Target > 1e-8 &&
		isfinite(lane.WeightSum) && lane.WeightSum > 0.0;
}

SmokeEmissiveStorageRecord SmokePackEmissiveLanePair(SmokeEmissiveLaneRecord lane0, SmokeEmissiveLaneRecord lane1)
{
	SmokeEmissiveStorageRecord storage;
	storage.Data0 = uint4(lane0.CandidateIndex, lane0.SampleSeed, lane0.StableKeyLo, lane0.StableKeyHi);
	storage.Data1 = uint4(asuint(lane0.Target), asuint(lane0.WeightSum), lane1.CandidateIndex, lane1.SampleSeed);
	storage.Data2 = uint4(lane1.StableKeyLo, lane1.StableKeyHi, asuint(lane1.Target), asuint(lane1.WeightSum));
	return storage;
}

SmokeEmissiveLaneRecord SmokeUnpackEmissiveLane(SmokeEmissiveStorageRecord storage, uint laneInPair)
{
	SmokeEmissiveLaneRecord lane;
	if (laneInPair == 0u)
	{
		lane.CandidateIndex = storage.Data0.x;
		lane.SampleSeed = storage.Data0.y;
		lane.StableKeyLo = storage.Data0.z;
		lane.StableKeyHi = storage.Data0.w;
		lane.Target = asfloat(storage.Data1.x);
		lane.WeightSum = asfloat(storage.Data1.y);
	}
	else
	{
		lane.CandidateIndex = storage.Data1.z;
		lane.SampleSeed = storage.Data1.w;
		lane.StableKeyLo = storage.Data2.x;
		lane.StableKeyHi = storage.Data2.y;
		lane.Target = asfloat(storage.Data2.z);
		lane.WeightSum = asfloat(storage.Data2.w);
	}
	return lane;
}

uint SmokePackEmissiveDirection(float3 direction)
{
	if (!all(isfinite(direction)) || dot(direction, direction) <= 1e-8)
		return 0u;
	float3 oct = normalize(direction);
	oct /= max(abs(oct.x) + abs(oct.y) + abs(oct.z), 1e-6);
	float2 encoded = oct.xy;
	if (oct.z < 0.0)
		encoded = (1.0 - abs(encoded.yx)) * float2(encoded.x >= 0.0 ? 1.0 : -1.0, encoded.y >= 0.0 ? 1.0 : -1.0);
	const uint2 packed = (uint2)round(saturate(encoded * 0.5 + 0.5) * 65535.0);
	return packed.x | (packed.y << 16u);
}

float3 SmokeUnpackEmissiveDirection(uint packed)
{
	if (packed == 0u)
		return 0.0;
	const float2 encoded = float2(packed & 0xffffu, packed >> 16u) / 65535.0 * 2.0 - 1.0;
	float3 direction = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
	if (direction.z < 0.0)
		direction.xy = (1.0 - abs(direction.yx)) * float2(direction.x >= 0.0 ? 1.0 : -1.0, direction.y >= 0.0 ? 1.0 : -1.0);
	return normalize(direction);
}

uint SmokePackEmissiveMomentMetadata(float confidence, uint mediumHash, uint age, uint laneCount)
{
	const uint packedConfidence = (uint)round(saturate(confidence) * 255.0);
	return packedConfidence | ((mediumHash & 0x1fu) << 8u) | (min(age, 15u) << 13u) |
		((gSmokeConstants.FrameIndex & 0x3fu) << 17u) | ((min(laneCount, 7u) & 0x7u) << 23u) |
		NRI_SMOKE_EMISSIVE_MOMENT_RECORD | NRI_SMOKE_EMISSIVE_RECORD_VALID;
}

float SmokeEmissiveMomentConfidence(SmokeEmissiveMomentRecord record) { return (float)(record.Metadata & 0xffu) / 255.0; }
uint SmokeEmissiveMomentMedium(SmokeEmissiveMomentRecord record) { return (record.Metadata >> 8u) & 0x1fu; }
uint SmokeEmissiveMomentAge(SmokeEmissiveMomentRecord record) { return (record.Metadata >> 13u) & 0xfu; }
uint SmokeEmissiveMomentFrame(SmokeEmissiveMomentRecord record) { return (record.Metadata >> 17u) & 0x3fu; }
uint SmokeEmissiveMomentLaneCount(SmokeEmissiveMomentRecord record) { return (record.Metadata >> 23u) & 0x7u; }

bool SmokeEmissiveMomentValid(SmokeEmissiveMomentRecord record)
{
	return (record.Metadata & (NRI_SMOKE_EMISSIVE_RECORD_VALID | NRI_SMOKE_EMISSIVE_MOMENT_RECORD)) ==
		(NRI_SMOKE_EMISSIVE_RECORD_VALID | NRI_SMOKE_EMISSIVE_MOMENT_RECORD) &&
		all(isfinite(record.MeanRadiance)) && all(record.MeanRadiance >= 0.0) &&
		all(isfinite(record.SecondMoment)) && all(record.SecondMoment >= 0.0) &&
		all(isfinite(record.ReceiverPosition)) && isfinite(record.SigmaT) && record.SigmaT > 0.0;
}

SmokeEmissiveStorageRecord SmokePackEmissiveMoment(SmokeEmissiveMomentRecord record)
{
	SmokeEmissiveStorageRecord storage;
	storage.Data0 = uint4(asuint(record.MeanRadiance), asuint(record.SecondMoment.x));
	storage.Data1 = uint4(asuint(record.SecondMoment.yz), asuint(record.ReceiverPosition.xy));
	storage.Data2 = uint4(asuint(record.ReceiverPosition.z), asuint(record.SigmaT), record.Direction, record.Metadata);
	return storage;
}

SmokeEmissiveMomentRecord SmokeUnpackEmissiveMoment(SmokeEmissiveStorageRecord storage)
{
	SmokeEmissiveMomentRecord record;
	record.MeanRadiance = asfloat(storage.Data0.xyz);
	record.SecondMoment = float3(asfloat(storage.Data0.w), asfloat(storage.Data1.xy));
	record.ReceiverPosition = float3(asfloat(storage.Data1.zw), asfloat(storage.Data2.x));
	record.SigmaT = asfloat(storage.Data2.y);
	record.Direction = storage.Data2.z;
	record.Metadata = storage.Data2.w;
	return record;
}

uint SmokeEmissiveMediumHash(float4 medium, float anisotropy);

bool SmokeEmissiveMomentCompatible(SmokeEmissiveMomentRecord record, float4 medium, float anisotropy,
	uint expectedFrame, float3 receiverPosition, float worldTolerance, uint laneCount)
{
	return SmokeEmissiveMomentValid(record) &&
		SmokeEmissiveMomentMedium(record) == SmokeEmissiveMediumHash(medium, anisotropy) &&
		SmokeEmissiveMomentFrame(record) == (expectedFrame & 0x3fu) &&
		SmokeEmissiveMomentLaneCount(record) == laneCount &&
		abs(record.SigmaT - medium.a) <= max(max(record.SigmaT, medium.a) * 0.25, 0.002) &&
		distance(record.ReceiverPosition, receiverPosition) <= worldTolerance;
}

uint SmokeEmissiveWorldKey(float3 receiverPosition)
{
	float cellSize = 1.0;
	uint controlCount, ignoredStride;
	gSmokeRenderGridControl.GetDimensions(controlCount, ignoredStride);
	if (controlCount > 0u)
	{
		const float candidate = asfloat(gSmokeRenderGridControl[0].CellSizeBits);
		if (isfinite(candidate) && candidate > 0.0)
			cellSize = candidate;
	}
	if ((gSmokeConstants.Flags & NRI_SMOKE_EMISSIVE_QUARTER_KEY) != 0u)
		cellSize *= 0.25;
	const int3 cell = (int3)floor(receiverPosition / cellSize);
	return SmokeHash(asuint(cell.x) ^ SmokeHash(asuint(cell.y)) ^ SmokeHash(asuint(cell.z)) ^
		SmokeHash(gSmokeConstants.SimulationEpoch));
}

uint SmokeEmissiveLaneSeed(float3 receiverPosition, uint laneIndex, uint proposal, uint salt)
{
	uint seed = SmokeEmissiveWorldKey(receiverPosition) ^ SmokeHash(laneIndex + 1u) ^
		SmokeHash(proposal + 1u) ^ SmokeHash(salt);
	if (gSmokeConstants.LightMode >= 3u)
		seed ^= SmokeHash(gSmokeConstants.FrameIndex);
	return SmokeHash(seed);
}

uint SmokeEmissiveReuseMode()
{
	return (gSmokeConstants.Flags >> NRI_SMOKE_EMISSIVE_REUSE_SHIFT) & NRI_SMOKE_EMISSIVE_REUSE_MASK;
}

uint SmokeStableEmissiveReferenceSeed(uint3 froxel, uint sampleIndex)
{
	uint seed = SmokeHash(froxel.x ^ SmokeHash(froxel.y + 0x6d2b79f5u));
	seed ^= SmokeHash(froxel.z + 0x9e3779b9u);
	seed ^= SmokeHash(gSmokeConstants.SimulationEpoch + 0x85ebca6bu);
	return SmokeHash(seed ^ SmokeHash(sampleIndex + 0x7c7e6f19u));
}

uint SmokeEmissiveMediumHash(float4 medium, float anisotropy)
{
	const uint3 albedo = (uint3)round(saturate(medium.rgb / max(medium.a, 1e-5)) * 15.0);
	const uint g = (uint)round(saturate(anisotropy * 0.5 + 0.5) * 15.0);
	const uint sigma = (uint)round(saturate(medium.a * 0.25) * 15.0);
	return SmokeHash(albedo.x | (albedo.y << 4u) | (albedo.z << 8u) | (g << 12u) | (sigma << 16u)) & 0x1fu;
}

uint SmokeEmissiveRecordM(SmokeEmissiveReservoirRecord record) { return record.Metadata & 0xffu; }
uint SmokeEmissiveRecordMedium(SmokeEmissiveReservoirRecord record) { return (record.Metadata >> 8u) & 0x1fu; }
uint SmokeEmissiveRecordAge(SmokeEmissiveReservoirRecord record) { return (record.Metadata >> 13u) & 0xfu; }
uint SmokeEmissiveRecordFrame(SmokeEmissiveReservoirRecord record) { return (record.Metadata >> 17u) & 0x3fu; }

uint SmokePackEmissiveMetadata(uint representedSamples, uint mediumHash, uint age)
{
	return min(representedSamples, 255u) | ((mediumHash & 0x1fu) << 8u) |
		(min(age, 15u) << 13u) | ((gSmokeConstants.FrameIndex & 0x3fu) << 17u) |
		NRI_SMOKE_EMISSIVE_RECORD_VALID;
}

SmokeEmissiveReservoirRecord SmokeEmptyEmissiveReservoir()
{
	SmokeEmissiveReservoirRecord record = (SmokeEmissiveReservoirRecord)0;
	record.CandidateIndex = 0xffffffffu;
	return record;
}

bool SmokeEmissiveRecordValid(SmokeEmissiveReservoirRecord record)
{
	return (record.Metadata & NRI_SMOKE_EMISSIVE_RECORD_VALID) != 0u && record.CandidateIndex != 0xffffffffu &&
		record.Generation == gSmokeConstants.CommandCount && SmokeEmissiveRecordM(record) > 0u &&
		isfinite(record.Target) && record.Target > 1e-8 && isfinite(record.WeightSum) && record.WeightSum > 0.0;
}

bool SmokeEmissiveIdentityValid(SmokeEmissiveReservoirRecord record)
{
	uint candidateCount, ignoredStride;
	gSmokeEmissivePrimitives.GetDimensions(candidateCount, ignoredStride);
	// Proposal records deliberately have no target or accumulated weight yet;
	// identity validation must be usable before the first target evaluation.
	if (record.CandidateIndex == 0xffffffffu || record.CandidateIndex >= candidateCount ||
		record.Generation != gSmokeConstants.CommandCount)
		return false;
	const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[record.CandidateIndex];
	return candidate.stableKeyLo == record.StableKeyLo && candidate.stableKeyHi == record.StableKeyHi;
}

float SmokeEmissiveLuminance(float3 value)
{
	return dot(max(value, 0.0), float3(0.2126, 0.7152, 0.0722));
}

bool SmokeEvaluateEmissiveIncidentWithSolidAngleDenominatorAndPrimitive(
	SmokeEmissiveReservoirRecord record,
	float3 receiverPosition,
	bool diagnostics,
	float solidAngleDenominator,
	out float3 incidentRadiance,
	out float3 lightDirection,
	out float distanceToLight,
	out SmokeEmissiveSampleIdentity sampleIdentity)
{
	incidentRadiance = 0.0;
	lightDirection = 0.0;
	distanceToLight = 0.0;
	sampleIdentity = SmokeEmptyEmissiveSampleIdentity();
	if (!SmokeEmissiveIdentityValid(record))
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveIdentityRejects, 1u);
		return false;
	}
	const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[record.CandidateIndex];
	sampleIdentity.CandidateIndex = record.CandidateIndex;
	sampleIdentity.DataSource = candidate.dataSource;
	sampleIdentity.SceneInstanceIndex = candidate.sceneInstanceIndex;
	sampleIdentity.RangeBase = candidate.primitiveIndex;
	sampleIdentity.RangeCount = candidate.primitiveCount;
	uint randomState = record.SampleSeed;
	uint sampledPrimitiveIndex;
	uint sampledMaterialIndex;
	PrimitiveData primitive;
	MaterialData material;
	float2 lightUv;
	float3 lightNormal;
	float3 lightPosition;
	float effectiveArea;
	if (!SmokeSamplePointOnEmissive(candidate, randomState, sampledPrimitiveIndex, primitive, material, sampledMaterialIndex,
		lightPosition, lightUv, lightNormal, effectiveArea))
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveCandidateMisses, 1u);
		return false;
	}
	sampleIdentity.PrimitiveIndex = sampledPrimitiveIndex;
	sampleIdentity.MaterialIndex = sampledMaterialIndex;
	float3 lightRadiance = SmokeSampleMaterialEmission(material, lightUv) * max(material.emissiveIntensity, 0.0);
	lightRadiance *= SmokeResolveEmissiveRadianceScale(candidate, sampledPrimitiveIndex);
	if (!all(isfinite(lightRadiance)) || !any(lightRadiance > 0.0))
		return false;
	const float3 toLight = lightPosition - receiverPosition;
	const float distanceSquared = dot(toLight, toLight);
	if (distanceSquared <= 0.0001 || !isfinite(distanceSquared))
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveDistanceRejected, 1u);
		return false;
	}
	distanceToLight = sqrt(distanceSquared);
	lightDirection = toLight / distanceToLight;
	// Raster-visible emissive sheets behave as two-sided emitters in the world.
	// Preserve that convention for smoke without changing opaque radiometry.
	const float emitterCosine = abs(dot(lightNormal, -lightDirection));
	if (emitterCosine <= 0.0)
	{
		if (diagnostics)
			InterlockedAdd(gSmokeControl[0].EmissiveFacingRejected, 1u);
		return false;
	}
	const float projectedArea = max(effectiveArea * emitterCosine, 0.001);
	const float falloffScale = max(material.emissiveMaskScale, 0.25);
	const float attenuatedDistanceSquared = pow(max(distanceSquared, 0.01), falloffScale);
	const float solidAngle = min(projectedArea / max(solidAngleDenominator * attenuatedDistanceSquared, 0.01), 1.0);
	if (diagnostics && any(lightRadiance > 32.0))
		InterlockedAdd(gSmokeControl[0].EmissiveRadianceClamps, 1u);
	lightRadiance = min(lightRadiance, 32.0);
	incidentRadiance = lightRadiance * solidAngle;
	return all(isfinite(incidentRadiance)) && SmokeEmissiveLuminance(incidentRadiance) > 1e-8;
}

bool SmokeEvaluateEmissiveIncidentWithSolidAngleDenominator(
	SmokeEmissiveReservoirRecord record,
	float3 receiverPosition,
	bool diagnostics,
	float solidAngleDenominator,
	out float3 incidentRadiance,
	out float3 lightDirection,
	out float distanceToLight)
{
	SmokeEmissiveSampleIdentity ignoredIdentity;
	return SmokeEvaluateEmissiveIncidentWithSolidAngleDenominatorAndPrimitive(record, receiverPosition, diagnostics,
		solidAngleDenominator, incidentRadiance, lightDirection, distanceToLight, ignoredIdentity);
}

bool SmokeEvaluateEmissiveIncident(
	SmokeEmissiveReservoirRecord record,
	float3 receiverPosition,
	bool diagnostics,
	out float3 incidentRadiance,
	out float3 lightDirection,
	out float distanceToLight)
{
	// Preserve the established legacy smoke-emissive convention exactly.
	return SmokeEvaluateEmissiveIncidentWithSolidAngleDenominator(record, receiverPosition, diagnostics,
		12.56637061436, incidentRadiance, lightDirection, distanceToLight);
}

bool SmokeEvaluateWorldEmissiveIncidentWithPrimitive(
	SmokeEmissiveReservoirRecord record,
	float3 receiverPosition,
	bool diagnostics,
	out float3 incidentRadiance,
	out float3 lightDirection,
	out float distanceToLight,
	out SmokeEmissiveSampleIdentity sampleIdentity)
{
	// The world field stores incident radiance before HG phase projection. Its
	// area-over-distance term is already a solid-angle estimate, so applying
	// 4*pi here and HG's normalization later would attenuate it twice.
	return SmokeEvaluateEmissiveIncidentWithSolidAngleDenominatorAndPrimitive(record, receiverPosition, diagnostics,
		1.0, incidentRadiance, lightDirection, distanceToLight, sampleIdentity);
}

bool SmokeEvaluateWorldEmissiveIncident(
	SmokeEmissiveReservoirRecord record,
	float3 receiverPosition,
	bool diagnostics,
	out float3 incidentRadiance,
	out float3 lightDirection,
	out float distanceToLight)
{
	SmokeEmissiveSampleIdentity ignoredIdentity;
	return SmokeEvaluateWorldEmissiveIncidentWithPrimitive(record, receiverPosition, diagnostics,
		incidentRadiance, lightDirection, distanceToLight, ignoredIdentity);
}

bool SmokeEvaluateEmissiveCandidate(
	SmokeEmissiveReservoirRecord record,
	float3 receiverPosition,
	float3 viewRay,
	float anisotropy,
	bool diagnostics,
	out float3 integrand,
	out float3 lightDirection,
	out float distanceToLight)
{
	float3 incidentRadiance;
	if (!SmokeEvaluateEmissiveIncident(record, receiverPosition, diagnostics,
		incidentRadiance, lightDirection, distanceToLight))
	{
		integrand = 0.0;
		return false;
	}
	integrand = incidentRadiance * SmokePhaseResponse(dot(lightDirection, viewRay), anisotropy);
	return all(isfinite(integrand)) && SmokeEmissiveLuminance(integrand) > 1e-8;
}

void SmokeReservoirMerge(
	inout SmokeEmissiveReservoirRecord reservoir,
	SmokeEmissiveReservoirRecord candidate,
	float currentTarget,
	float candidateWeight,
	uint representedSamples,
	uint mediumHash,
	uint age,
	inout uint randomState)
{
	if (!isfinite(candidateWeight) || candidateWeight <= 0.0 || !isfinite(currentTarget) || currentTarget <= 1e-8)
		return;
	const float newWeightSum = reservoir.WeightSum + candidateWeight;
	if (reservoir.CandidateIndex == 0xffffffffu || SmokeRandom01(randomState) * newWeightSum < candidateWeight)
	{
		reservoir.CandidateIndex = candidate.CandidateIndex;
		reservoir.SampleSeed = candidate.SampleSeed;
		reservoir.StableKeyLo = candidate.StableKeyLo;
		reservoir.StableKeyHi = candidate.StableKeyHi;
		reservoir.Target = currentTarget;
		reservoir.Generation = candidate.Generation;
	}
	reservoir.WeightSum = newWeightSum;
	reservoir.Metadata = SmokePackEmissiveMetadata(SmokeEmissiveRecordM(reservoir) + representedSamples, mediumHash, age);
}

float SmokeRetargetedEmissiveWeight(
	SmokeEmissiveReservoirRecord record,
	float currentTarget,
	uint retainedSamples)
{
	const uint sourceSamples = SmokeEmissiveRecordM(record);
	if (sourceSamples == 0u || retainedSamples == 0u || !isfinite(currentTarget) || currentTarget <= 1e-8)
		return 0.0;
	// Reuse deliberately bounds represented samples. Preserve average sample
	// mass when truncating M; importing the full WeightSum with a smaller M
	// makes energy multiply across temporal and spatial passes.
	const float retainedFraction = (float)retainedSamples / (float)sourceSamples;
	return record.WeightSum * currentTarget / max(record.Target, 1e-8) * retainedFraction;
}

bool SmokeEmissiveReservoirCompatible(
	SmokeEmissiveReservoirRecord record,
	float4 medium,
	float anisotropy,
	uint expectedPreviousFrame,
	float3 receiverPosition,
	float worldTolerance)
{
	return SmokeEmissiveRecordValid(record) && SmokeEmissiveIdentityValid(record) &&
		SmokeEmissiveRecordMedium(record) == SmokeEmissiveMediumHash(medium, anisotropy) &&
		SmokeEmissiveRecordFrame(record) == (expectedPreviousFrame & 0x3fu) &&
		all(isfinite(record.ReceiverPosition)) && isfinite(record.SigmaT) &&
		abs(record.SigmaT - medium.a) <= max(max(record.SigmaT, medium.a) * 0.25, 0.002) &&
		distance(record.ReceiverPosition, receiverPosition) <= worldTolerance;
}

#endif
