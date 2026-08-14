#ifndef NRI_SMOKE_GRID_DATA_HLSLI
#define NRI_SMOKE_GRID_DATA_HLSLI

#define NRI_SMOKE_GRID_BRICK_AXIS 8
#define NRI_SMOKE_GRID_CELLS_PER_BRICK 512u
#define NRI_SMOKE_PROMPT_FALLBACK_QUANTITY 8u
#define NRI_SMOKE_PROMPT_LEDGER_CAPACITY NRI_SMOKE_PROMPT_FALLBACK_QUANTITY
#define NRI_SMOKE_PROMPT_OUTCOME_NONE 0u
#define NRI_SMOKE_PROMPT_OUTCOME_FALLBACK 1u
#define NRI_SMOKE_PROMPT_OUTCOME_GRID_NEW 2u
#define NRI_SMOKE_PROMPT_OUTCOME_GRID_COMMITTED 3u
#define NRI_SMOKE_PROMPT_OUTCOME_INTERNAL_ERROR 4u

struct SmokePromptOutcome
{
	uint PulseIdLow;
	uint PulseIdHigh;
	uint RangeBegin;
	uint RangeCount;
	uint CommandIndex;
	uint Outcome;
	uint RequestedBricks;
	uint AdmittedBricks;
};

struct SmokePromptLedger
{
	uint PulseIdLow;
	uint PulseIdHigh;
	uint RangeBegin;
	uint RangeCount;
	uint Epoch;
	uint Committed;
	uint2 Padding;
};
#define NRI_SMOKE_GRID_HASH_PROBES 24u
#define NRI_SMOKE_GRID_EMPTY 0u
#define NRI_SMOKE_GRID_CLAIMED 1u
#define NRI_SMOKE_GRID_RESIDENT 2u
#define NRI_SMOKE_GRID_NEW 3u
#define NRI_SMOKE_GRID_TOMBSTONE 4u
#define NRI_SMOKE_GRID_BRICK_CONTENT 1u
#define NRI_SMOKE_GRID_BRICK_HALO 2u
#define NRI_SMOKE_GRID_BRICK_BORROWED_FIRST_USE 4u
#define NRI_SMOKE_GRID_BRICK_OPTICAL_CONTENT 0x100u
#define NRI_SMOKE_GRID_BRICK_PROMPT_PROVISIONAL 8u
#define NRI_SMOKE_GRID_BRICK_PROMPT_SLOT_SHIFT 4u
#define NRI_SMOKE_GRID_BRICK_PROMPT_SLOT_MASK 0xf0u
#define NRI_SMOKE_GRID_FIRST_USE_CORE_MINIMUM 8u
#define NRI_SMOKE_GRID_FIRST_USE_CORE_DIVISOR 16u
#define NRI_SMOKE_GRID_FIRST_USE_BLOCKED_NONE 0u
#define NRI_SMOKE_GRID_FIRST_USE_BLOCKED_NO_BORROWED 1u
#define NRI_SMOKE_GRID_FIRST_USE_BLOCKED_VISIBLE 2u
#define NRI_SMOKE_GRID_FIRST_USE_BLOCKED_PROBE 3u
#define NRI_SMOKE_GRID_FIRST_USE_BLOCKED_INVALID 4u
#define NRI_SMOKE_GRID_FLAG_HASH_HEALTH 1u
#define NRI_SMOKE_GRID_FLAG_COMPACT_DRAINED_HASH 2u

struct SmokeGridHashEntry
{
	int3 Coordinate;
	uint BrickIndex;
	uint Generation;
	uint State;
	uint2 Padding;
};

struct SmokeGridBrick
{
	int3 Coordinate;
	uint HashSlot;
	uint Generation;
	uint State;
	uint IdleFrames;
	uint Flags;
};

struct SmokeGridControl
{
	uint ActiveCountA;
	uint ActiveCountB;
	uint ResidentCount;
	uint FreeCount;
	uint Allocated;
	uint Reclaimed;
	uint AllocationFailures;
	uint ProbeFailures;
	uint MaximumProbe;
	uint CommandsProcessed;
	uint RequestedMassQ;
	uint DepositedMassQ;
	uint RejectedMassQ;
	uint SaturatedDeposits;
	uint HaloAllocations;
	uint OccupiedBricks;
	uint EmptyBricks;
	uint CflClamps;
	uint BacktraceClamps;
	uint NanRejects;
	uint VorticityClamps;
	uint FieldHashLo;
	uint FieldHashHi;
	uint DepositionCells;
	uint DepositionRejected;
	uint Generation;
	uint FrameStamp;
	uint BrickCapacity;
	uint HashCapacity;
	uint CellCapacity;
	uint ActivePing;
	uint FieldPing;
	uint CellSizeBits;
	uint AdmissionSourceCount;
	uint AdmissionRequested;
	uint AdmissionExisting;
	uint AdmissionAdmitted;
	uint AdmissionRejected;
	uint AdmissionCapacityRejected;
	uint AdmissionProbeRejected;
	uint AdmissionInvalidRejected;
	uint AdmissionFootprintCulled;
	uint HashEmpty;
	uint HashClaimed;
	uint HashResident;
	uint HashNew;
	uint HashTombstone;
	uint HashInvalidState;
	uint HashInvalidMapping;
	uint ControlProbeTotal;
	uint ControlProbeBin1;
	uint ControlProbeBin2To4;
	uint ControlProbeBin5To8;
	uint ControlProbeBin9To16;
	uint ControlProbeBin17To24;
	uint LookupProbeTotal;
	uint InsertionProbeTotal;
	uint LookupProbeLimitFailures;
	uint InsertionProbeLimitFailures;
	uint InsertionCapacityFailures;
	uint InsertionActiveFailures;
	uint ReclaimInvalidMappingFailures;
	uint HashRebuildAttempts;
	uint HashRebuildSuccesses;
	uint HashRebuildFailures;
	uint FirstUseCoreCapacity;
	uint BorrowedResident;
	uint BorrowedAllocations;
	uint BorrowedReturns;
	uint BorrowedPromotions;
	uint BorrowedReclaims;
	uint FirstUseReplacementAdmissions;
	uint FirstUseBlockedNoBorrowed;
	uint FirstUseBlockedVisible;
	uint FirstUseBlockedProbe;
	uint FirstUseBlockedInvalid;
	uint FirstUseCapacityFailures;
};

struct SmokeGridSourceStats
{
	uint SourceId;
	uint SourceClass;
	uint Priority;
	uint Commands;
	uint RequestedBricks;
	uint ExistingHits;
	uint AdmittedNew;
	uint RejectedCapacity;
	uint RejectedProbe;
	uint RejectedInvalid;
	uint DepositionCells;
	uint FootprintCulled;
	uint RequestedMassQ;
	uint DepositedMassQ;
	uint RejectedMassQ;
	uint AdmittedKeyHash;
};

struct SmokeGridConstants
{
	uint Pass;
	uint FrameIndex;
	uint SimulationEpoch;
	uint CommandCount;

	uint StyleCount;
	uint BrickCapacity;
	uint HashCapacity;
	uint CellCapacity;

	uint ActivePing;
	uint FieldPing;
	uint Flags;
	uint Representation;

	float CellSize;
	float DeltaTime;
	float TimeScale;
	float MaxBacktrace;

	float3 Wind;
	float Buoyancy;

	float VelocityDamping;
	float WindCoupling;
	float DensityHalfLifeScale;
	float CoolingScale;

	float MaxVelocity;
	float ActiveThreshold;
	uint ReclaimGrace;
	float MassQuantization;

	float MomentumQuantization;
	float CurlTime;
	float CurlEvolution;
	float VorticityConfinement;
};

int SmokeGridFloorDiv8(int value)
{
	const int quotient = value / NRI_SMOKE_GRID_BRICK_AXIS;
	const int remainder = value - quotient * NRI_SMOKE_GRID_BRICK_AXIS;
	return quotient - (remainder < 0 ? 1 : 0);
}

int3 SmokeGridBrickCoordinate(int3 cell)
{
	return int3(SmokeGridFloorDiv8(cell.x), SmokeGridFloorDiv8(cell.y), SmokeGridFloorDiv8(cell.z));
}

uint3 SmokeGridLocalCoordinate(int3 cell, int3 brick)
{
	return (uint3)(cell - brick * NRI_SMOKE_GRID_BRICK_AXIS);
}

uint SmokeGridLocalIndex(uint3 local)
{
	return local.x + local.y * NRI_SMOKE_GRID_BRICK_AXIS + local.z * NRI_SMOKE_GRID_BRICK_AXIS * NRI_SMOKE_GRID_BRICK_AXIS;
}

int3 SmokeGridCellCoordinate(int3 brick, uint3 local)
{
	return brick * NRI_SMOKE_GRID_BRICK_AXIS + (int3)local;
}

float3 SmokeGridCellCenter(int3 brick, uint3 local, float cellSize)
{
	return ((float3)SmokeGridCellCoordinate(brick, local) + 0.5) * cellSize;
}

uint SmokeGridHashCoordinate(int3 coordinate)
{
	uint value = asuint(coordinate.x) * 0x8da6b343u;
	value ^= asuint(coordinate.y) * 0xd8163841u;
	value ^= asuint(coordinate.z) * 0xcb1ab31fu;
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	return value;
}

#endif
