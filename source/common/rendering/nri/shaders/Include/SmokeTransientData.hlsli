#ifndef NRI_SMOKE_TRANSIENT_DATA_HLSLI
#define NRI_SMOKE_TRANSIENT_DATA_HLSLI

#define NRI_SMOKE_TRANSIENT_MAX_GROUPS 64u
#define NRI_SMOKE_TRANSIENT_MAX_LOBES 256u
#define NRI_SMOKE_TRANSIENT_MAX_LOBES_PER_GROUP 16u
#define NRI_SMOKE_TRANSIENT_ANCHOR_COUNT 4u
#define NRI_SMOKE_TRANSIENT_BIN_SIZE_XY 4u
#define NRI_SMOKE_TRANSIENT_BIN_SIZE_Z 4u
#define NRI_SMOKE_TRANSIENT_MAX_GROUPS_PER_BIN 64u

#define NRI_SMOKE_TRANSIENT_CLASS_EXPLOSION 0u
#define NRI_SMOKE_TRANSIENT_CLASS_TRAIL 1u
#define NRI_SMOKE_TRANSIENT_CLASS_FIRE 2u
#define NRI_SMOKE_TRANSIENT_CLASS_MUZZLE 3u
#define NRI_SMOKE_TRANSIENT_CLASS_IMPACT 4u
#define NRI_SMOKE_TRANSIENT_CLASS_DIAGNOSTIC 5u

#define NRI_SMOKE_TRANSIENT_GROUP_ACTIVE 0x1u
#define NRI_SMOKE_TRANSIENT_GROUP_FULL_BUILD 0x2u
#define NRI_SMOKE_TRANSIENT_GROUP_FALLBACK_BUILD 0x4u
#define NRI_SMOKE_TRANSIENT_GROUP_SLOW_REFRESH 0x8u
#define NRI_SMOKE_TRANSIENT_LOBE_ACTIVE 0x1u

#define NRI_SMOKE_TRANSIENT_LIGHT_BANK_B 0x1u
#define NRI_SMOKE_TRANSIENT_LIGHT_FULL 0x2u
#define NRI_SMOKE_TRANSIENT_LIGHT_FALLBACK 0x4u
#define NRI_SMOKE_TRANSIENT_LIGHT_POINT 0x10u
#define NRI_SMOKE_TRANSIENT_LIGHT_DIRECTIONAL 0x20u
#define NRI_SMOKE_TRANSIENT_LIGHT_EMISSIVE 0x40u
#define NRI_SMOKE_TRANSIENT_LIGHT_ENVIRONMENT 0x80u
#define NRI_SMOKE_TRANSIENT_LIGHT_UNSHADOWED 0x100u
#define NRI_SMOKE_TRANSIENT_LIGHT_VALID 0x80000000u
#define NRI_SMOKE_TRANSIENT_ANCHOR_WRITTEN 0x40000000u
#define NRI_SMOKE_FROXEL_CARRIER_TRANSIENT 0x10u
#define NRI_SMOKE_TRANSIENT_SELF_SHADOW 0x200u

// Transient passes have no particle simulation, grid command generation, or
// receiver-light sampling. Their CPU pass constants deliberately reuse these
// established 216-byte ABI lanes rather than widening the root signature.
#define NRI_SMOKE_TRANSIENT_GROUP_COUNT gSmokeConstants.CommandCount
#define NRI_SMOKE_TRANSIENT_LOBE_COUNT gSmokeConstants.ParticleCapacity
#define NRI_SMOKE_TRANSIENT_FULL_BUILD_BUDGET gSmokeConstants.MaxLightCandidates
#define NRI_SMOKE_TRANSIENT_POINT_BUDGET gSmokeConstants.LightSamples

// Keep synchronized with NRISmokeTransientGroupGpu (96 bytes).
struct SmokeTransientGroup
{
	float3 BoundsMin;
	float AgeSeconds;
	float3 BoundsMax;
	float GroupLifetimeSeconds;
	uint FirstLobe;
	uint LobeCount;
	uint Slot;
	uint Generation;
	uint Epoch;
	uint Flags;
	uint AnchorCount;
	uint SamplesPerAnchor;
	float3 Center;
	float RefreshIntervalSeconds;
	uint SourceId;
	uint TransientClass;
	uint RequiredAnchorMask;
	uint Reserved;
};

// Keep synchronized with NRISmokeTransientLobeGpu (96 bytes).
struct SmokeTransientLobe
{
	float3 Position;
	float Radius;
	float3 HalfAxisU;
	uint Shape;
	float3 HalfAxisV;
	uint StyleIndex;
	float DensityScale;
	float EmissionScale;
	uint GroupSlot;
	uint GroupGeneration;
	uint Epoch;
	uint Flags;
	uint DeterministicSeed;
	uint TransientClass;
	float CorePlateau;
	float EdgeErosion;
	float NoiseScale;
	float NoiseStrength;
};

struct SmokeTransientBinHeader
{
	uint Count;
	uint Overflow;
};

// Six packed FP16 RGB incident-radiance lobes and identity. Non-fire records
// retain the original anchor position in Data2.yzw. Fire records reuse the
// otherwise-unconsumed Data2.yzw words for directional scene visibility ONLY
// (local smoke self-transmittance is applied during materialization), the group
// age at which this complete bank was built, and its packed shape/lighting
// revision; the buffer remains 64 bytes.
struct SmokeTransientLightAnchor
{
	uint4 Data0;
	uint4 Data1;
	uint4 Data2;
	uint4 Data3;
};

// PublishedState is the sole production validity word and is written last.
// Keep synchronized with the 64-byte CPU upload/readback contract.
struct SmokeTransientLightHeader
{
	uint GroupSlot;
	uint GroupGeneration;
	uint Epoch;
	uint PublishedState;
	uint RequiredAnchorMask;
	uint PublishedAnchorMask;
	uint ShapeRevision;
	uint LightingBoundsRevision;
	uint BuildFrame;
	uint ObservedFrame;
	uint SelectedPointKeyLo;
	uint SelectedPointKeyHi;
	uint SelectedEmissiveKeyLo;
	uint SelectedEmissiveKeyHi;
	uint FamilyAttemptMask;
	uint FamilySuccessMask;
};

uint3 SmokeTransientBinCount(uint3 froxelCount)
{
	return uint3(
		(froxelCount.x + NRI_SMOKE_TRANSIENT_BIN_SIZE_XY - 1u) /
			NRI_SMOKE_TRANSIENT_BIN_SIZE_XY,
		(froxelCount.y + NRI_SMOKE_TRANSIENT_BIN_SIZE_XY - 1u) /
			NRI_SMOKE_TRANSIENT_BIN_SIZE_XY,
		(froxelCount.z + NRI_SMOKE_TRANSIENT_BIN_SIZE_Z - 1u) /
			NRI_SMOKE_TRANSIENT_BIN_SIZE_Z);
}

uint SmokeTransientBinIndex(uint3 bin, uint3 binCount)
{
	return (bin.z * binCount.y + bin.y) * binCount.x + bin.x;
}

uint SmokeTransientShapeRevision(SmokeTransientGroup group) { return group.Reserved & 0xffffu; }
uint SmokeTransientLightingBoundsRevision(SmokeTransientGroup group) { return group.Reserved >> 16u; }

#ifndef NRI_SMOKE_TRANSIENT_RESOURCES_DECLARED
#define NRI_SMOKE_TRANSIENT_RESOURCES_DECLARED
StructuredBuffer<SmokeTransientGroup> gSmokeTransientGroups : register(t3, space0);
StructuredBuffer<SmokeTransientLobe> gSmokeTransientLobes : register(t4, space0);
RWStructuredBuffer<SmokeTransientBinHeader> gSmokeTransientBinHeaders : register(u61, space1);
RWStructuredBuffer<uint> gSmokeTransientBinIndices : register(u62, space1);
RWStructuredBuffer<float4> gSmokeTransientFroxelMedium : register(u63, space1);
RWStructuredBuffer<SmokeTransientLightAnchor> gSmokeTransientLightAnchorsA : register(u64, space1);
RWStructuredBuffer<SmokeTransientLightAnchor> gSmokeTransientLightAnchorsB : register(u65, space1);
RWStructuredBuffer<SmokeTransientLightHeader> gSmokeTransientLightHeaders : register(u66, space1);
#endif

#endif
