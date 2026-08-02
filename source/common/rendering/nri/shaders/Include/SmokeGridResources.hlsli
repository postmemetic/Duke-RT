#ifndef NRI_SMOKE_GRID_RESOURCES_HLSLI
#define NRI_SMOKE_GRID_RESOURCES_HLSLI

#include "NRI.hlsl"
#include "SmokeData.hlsli"
#include "SmokeGridData.hlsli"
#include "SmokeSourceShaping.hlsli"

StructuredBuffer<SmokeStyle> gSmokeGridStyles : register(t0, space0);
StructuredBuffer<SmokeInjectionCommand> gSmokeGridCommands : register(t1, space0);

RWStructuredBuffer<SmokeGridControl> gSmokeGridControl : register(u0, space1);
RWStructuredBuffer<SmokeGridHashEntry> gSmokeGridHash : register(u1, space1);
RWStructuredBuffer<SmokeGridBrick> gSmokeGridBricks : register(u2, space1);
RWStructuredBuffer<uint> gSmokeGridFreeList : register(u3, space1);
RWStructuredBuffer<uint> gSmokeGridActiveA : register(u4, space1);
RWStructuredBuffer<uint> gSmokeGridActiveB : register(u5, space1);
RWStructuredBuffer<uint> gSmokeGridDispatch : register(u6, space1);
RWStructuredBuffer<float4> gSmokeGridScalarA : register(u7, space1);
RWStructuredBuffer<float4> gSmokeGridScalarB : register(u8, space1);
RWStructuredBuffer<float4> gSmokeGridVelocityA : register(u9, space1);
RWStructuredBuffer<float4> gSmokeGridVelocityB : register(u10, space1);
RWStructuredBuffer<float4> gSmokeGridOpticalA : register(u11, space1);
RWStructuredBuffer<float4> gSmokeGridOpticalB : register(u12, space1);
RWStructuredBuffer<float4> gSmokeGridDynamicsA : register(u13, space1);
RWStructuredBuffer<float4> gSmokeGridDynamicsB : register(u14, space1);
RWStructuredBuffer<int4> gSmokeGridDeposit0 : register(u15, space1);
RWStructuredBuffer<int4> gSmokeGridDeposit1 : register(u16, space1);
RWStructuredBuffer<int4> gSmokeGridDeposit2 : register(u17, space1);
RWStructuredBuffer<int4> gSmokeGridDeposit3 : register(u18, space1);
RWStructuredBuffer<SmokeGridSourceStats> gSmokeGridSourceStats : register(u19, space1);
RWStructuredBuffer<SmokePromptOutcome> gSmokePromptOutcomes : register(u20, space1);
RWStructuredBuffer<SmokePromptLedger> gSmokePromptLedger : register(u21, space1);
RWStructuredBuffer<float4> gSmokeGridVorticity : register(u22, space1);

NRI_ROOT_CONSTANTS(SmokeGridConstants, gSmokeGridConstants, 0, 2);

uint SmokeGridActiveCount()
{
	return gSmokeGridConstants.ActivePing == 0u ? gSmokeGridControl[0].ActiveCountA : gSmokeGridControl[0].ActiveCountB;
}

uint SmokeGridActiveBrick(uint index)
{
	if (gSmokeGridConstants.ActivePing == 0u)
		return gSmokeGridActiveA[index];
	return gSmokeGridActiveB[index];
}

uint SmokeGridActiveCountForPing(uint ping)
{
	return ping == 0u ? gSmokeGridControl[0].ActiveCountA : gSmokeGridControl[0].ActiveCountB;
}

// Halos improve advection at brick boundaries, but they must never consume the
// entire sparse pool ahead of future source commands. Keep a bounded reserve
// for content allocation; missing halo samples safely resolve to empty space.
uint SmokeGridEmissionReserve()
{
	return min(gSmokeGridConstants.BrickCapacity, max(8u, gSmokeGridConstants.BrickCapacity / 4u));
}

uint SmokeGridFirstUseCoreCapacity()
{
	return min(gSmokeGridConstants.BrickCapacity,
		max(NRI_SMOKE_GRID_FIRST_USE_CORE_MINIMUM,
			gSmokeGridConstants.BrickCapacity / NRI_SMOKE_GRID_FIRST_USE_CORE_DIVISOR));
}

bool SmokeGridIsFirstUseClass(uint sourceClass)
{
	return sourceClass != NRI_SMOKE_SOURCE_CLASS_AMBIENT;
}

bool SmokeGridTryAppendActive(uint ping, uint brickIndex)
{
	uint destination = 0u;
	if (ping == 0u)
		InterlockedAdd(gSmokeGridControl[0].ActiveCountA, 1u, destination);
	else
		InterlockedAdd(gSmokeGridControl[0].ActiveCountB, 1u, destination);
	if (destination >= gSmokeGridConstants.BrickCapacity)
	{
		uint ignored = 0u;
		if (ping == 0u)
			InterlockedMin(gSmokeGridControl[0].ActiveCountA, gSmokeGridConstants.BrickCapacity, ignored);
		else
			InterlockedMin(gSmokeGridControl[0].ActiveCountB, gSmokeGridConstants.BrickCapacity, ignored);
		return false;
	}
	if (ping == 0u)
		gSmokeGridActiveA[destination] = brickIndex;
	else
		gSmokeGridActiveB[destination] = brickIndex;
	return true;
}

bool SmokeGridAppendActive(uint brickIndex)
{
	return SmokeGridTryAppendActive(gSmokeGridConstants.ActivePing, brickIndex);
}

bool SmokeGridAppendNextActive(uint brickIndex)
{
	return SmokeGridTryAppendActive(1u - min(gSmokeGridConstants.ActivePing, 1u), brickIndex);
}

bool SmokeGridValidateEntry(SmokeGridHashEntry entry, int3 coordinate, bool acceptNew, out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	if (!all(entry.Coordinate == coordinate) || entry.BrickIndex >= gSmokeGridConstants.BrickCapacity)
		return false;
	const SmokeGridBrick brick = gSmokeGridBricks[entry.BrickIndex];
	const bool stateValid = brick.State == NRI_SMOKE_GRID_RESIDENT || (acceptNew && brick.State == NRI_SMOKE_GRID_NEW);
	if (!stateValid || brick.Generation != entry.Generation || !all(brick.Coordinate == coordinate))
		return false;
	brickIndex = entry.BrickIndex;
	return true;
}

// Sampling readers accept only fully-cleared, published resident bricks. NEW,
// CLAIMED, and TOMBSTONE entries keep probing but never expose field data.
bool SmokeGridLookupBrick(int3 coordinate, out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	if (gSmokeGridConstants.HashCapacity == 0u)
		return false;
	const uint mask = gSmokeGridConstants.HashCapacity - 1u;
	const uint base = SmokeGridHashCoordinate(coordinate) & mask;
	const uint probeLimit = min(NRI_SMOKE_GRID_HASH_PROBES, gSmokeGridConstants.HashCapacity);
	[loop]
	for (uint probe = 0u; probe < probeLimit; ++probe)
	{
		const uint slot = (base + probe) & mask;
		const SmokeGridHashEntry entry = gSmokeGridHash[slot];
		if (entry.State == NRI_SMOKE_GRID_EMPTY)
			return false;
		if (entry.State == NRI_SMOKE_GRID_RESIDENT && SmokeGridValidateEntry(entry, coordinate, false, brickIndex))
			return true;
	}
	return false;
}

bool SmokeGridPopFreeSerial(out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	const uint freeCount = min(gSmokeGridControl[0].FreeCount, gSmokeGridConstants.BrickCapacity);
	if (freeCount == 0u)
		return false;
	gSmokeGridControl[0].FreeCount = freeCount - 1u;
	brickIndex = gSmokeGridFreeList[freeCount - 1u];
	return brickIndex < gSmokeGridConstants.BrickCapacity;
}

bool SmokeGridPushFree(uint brickIndex)
{
	uint destination = 0u;
	InterlockedAdd(gSmokeGridControl[0].FreeCount, 1u, destination);
	if (destination >= gSmokeGridConstants.BrickCapacity)
	{
		uint ignored = 0u;
		InterlockedMin(gSmokeGridControl[0].FreeCount, gSmokeGridConstants.BrickCapacity, ignored);
		return false;
	}
	gSmokeGridFreeList[destination] = brickIndex;
	return true;
}

void SmokeGridRecordControlProbe(uint probeCount, bool insertion)
{
	probeCount = min(probeCount, NRI_SMOKE_GRID_HASH_PROBES);
	gSmokeGridControl[0].ControlProbeTotal += probeCount;
	if (insertion)
		gSmokeGridControl[0].InsertionProbeTotal += probeCount;
	else
		gSmokeGridControl[0].LookupProbeTotal += probeCount;
	if (probeCount == 1u) gSmokeGridControl[0].ControlProbeBin1++;
	else if (probeCount <= 4u) gSmokeGridControl[0].ControlProbeBin2To4++;
	else if (probeCount <= 8u) gSmokeGridControl[0].ControlProbeBin5To8++;
	else if (probeCount <= 16u) gSmokeGridControl[0].ControlProbeBin9To16++;
	else gSmokeGridControl[0].ControlProbeBin17To24++;
	gSmokeGridControl[0].MaximumProbe = max(gSmokeGridControl[0].MaximumProbe, probeCount);
}

// Allocation-only lookup variant. Rendering keeps using SmokeGridLookupBrick
// above and therefore never performs telemetry atomics per medium sample.
bool SmokeGridLookupBrickControlSerial(int3 coordinate, out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	if (gSmokeGridConstants.HashCapacity == 0u)
		return false;
	const uint mask = gSmokeGridConstants.HashCapacity - 1u;
	const uint base = SmokeGridHashCoordinate(coordinate) & mask;
	const uint probeLimit = min(NRI_SMOKE_GRID_HASH_PROBES, gSmokeGridConstants.HashCapacity);
	[loop]
	for (uint probe = 0u; probe < probeLimit; ++probe)
	{
		const SmokeGridHashEntry entry = gSmokeGridHash[(base + probe) & mask];
		if (entry.State == NRI_SMOKE_GRID_EMPTY)
		{
			SmokeGridRecordControlProbe(probe + 1u, false);
			return false;
		}
		if (entry.State == NRI_SMOKE_GRID_RESIDENT &&
			SmokeGridValidateEntry(entry, coordinate, false, brickIndex))
		{
			SmokeGridRecordControlProbe(probe + 1u, false);
			return true;
		}
	}
	SmokeGridRecordControlProbe(probeLimit, false);
	gSmokeGridControl[0].LookupProbeLimitFailures++;
	return false;
}

void SmokeGridPromoteBorrowedBrickSerial(uint brickIndex)
{
	if (brickIndex >= gSmokeGridConstants.BrickCapacity ||
		(gSmokeGridBricks[brickIndex].Flags & NRI_SMOKE_GRID_BRICK_BORROWED_FIRST_USE) == 0u)
		return;
	gSmokeGridBricks[brickIndex].Flags &= ~NRI_SMOKE_GRID_BRICK_BORROWED_FIRST_USE;
	gSmokeGridControl[0].BorrowedReturns++;
	gSmokeGridControl[0].BorrowedPromotions++;
}

bool SmokeGridTryReplaceBorrowedDormantSerial(int3 coordinate, uint flags,
	out uint brickIndex, out uint blockedReason)
{
	brickIndex = 0xffffffffu;
	blockedReason = NRI_SMOKE_GRID_FIRST_USE_BLOCKED_NONE;
	const uint brickCapacity = gSmokeGridConstants.BrickCapacity;
	if (brickCapacity == 0u || gSmokeGridConstants.HashCapacity == 0u)
	{
		blockedReason = NRI_SMOKE_GRID_FIRST_USE_BLOCKED_NO_BORROWED;
		return false;
	}

	uint candidateIndex = 0xffffffffu;
	uint borrowedSeen = 0u;
	uint visibleSeen = 0u;
	const uint candidateStart = SmokeGridHashCoordinate(coordinate) % brickCapacity;
	[loop]
	for (uint ordinal = 0u; ordinal < brickCapacity; ++ordinal)
	{
		const uint index = (candidateStart + ordinal) % brickCapacity;
		const SmokeGridBrick candidate = gSmokeGridBricks[index];
		if ((candidate.Flags & NRI_SMOKE_GRID_BRICK_BORROWED_FIRST_USE) == 0u)
			continue;
		borrowedSeen++;
		const bool opticallyDormant = candidate.State == NRI_SMOKE_GRID_RESIDENT &&
			(candidate.Flags & NRI_SMOKE_GRID_BRICK_HALO) != 0u &&
			(candidate.Flags & NRI_SMOKE_GRID_BRICK_CONTENT) == 0u && candidate.IdleFrames > 0u;
		if (!opticallyDormant)
		{
			visibleSeen++;
			continue;
		}
		bool mappingValid = candidate.HashSlot < gSmokeGridConstants.HashCapacity;
		if (mappingValid)
		{
			const SmokeGridHashEntry entry = gSmokeGridHash[candidate.HashSlot];
			mappingValid = entry.State == NRI_SMOKE_GRID_RESIDENT && entry.BrickIndex == index &&
				entry.Generation == candidate.Generation && all(entry.Coordinate == candidate.Coordinate);
		}
		if (mappingValid)
		{
			candidateIndex = index;
			break;
		}
	}
	if (candidateIndex == 0xffffffffu)
	{
		blockedReason = borrowedSeen == 0u ? NRI_SMOKE_GRID_FIRST_USE_BLOCKED_NO_BORROWED :
			(visibleSeen != 0u ? NRI_SMOKE_GRID_FIRST_USE_BLOCKED_VISIBLE :
				NRI_SMOKE_GRID_FIRST_USE_BLOCKED_INVALID);
		return false;
	}

	const SmokeGridBrick candidate = gSmokeGridBricks[candidateIndex];
	const uint mask = gSmokeGridConstants.HashCapacity - 1u;
	const uint base = SmokeGridHashCoordinate(coordinate) & mask;
	const uint probeLimit = min(NRI_SMOKE_GRID_HASH_PROBES, gSmokeGridConstants.HashCapacity);
	uint destination = 0xffffffffu;
	uint probesVisited = probeLimit;
	[loop]
	for (uint probe = 0u; probe < probeLimit; ++probe)
	{
		const uint slot = (base + probe) & mask;
		const SmokeGridHashEntry entry = gSmokeGridHash[slot];
		if (slot == candidate.HashSlot || entry.State == NRI_SMOKE_GRID_TOMBSTONE)
		{
			if (destination == 0xffffffffu)
				destination = slot;
		}
		if (entry.State == NRI_SMOKE_GRID_EMPTY)
		{
			if (destination == 0xffffffffu)
				destination = slot;
			probesVisited = probe + 1u;
			break;
		}
	}
	SmokeGridRecordControlProbe(probesVisited, true);
	if (destination == 0xffffffffu)
	{
		blockedReason = NRI_SMOKE_GRID_FIRST_USE_BLOCKED_PROBE;
		return false;
	}

	if (candidate.HashSlot != destination)
		gSmokeGridHash[candidate.HashSlot].State = NRI_SMOKE_GRID_TOMBSTONE;
	uint generation = candidate.Generation + 1u;
	if (generation == 0u)
		generation = 1u;
	gSmokeGridHash[destination].State = NRI_SMOKE_GRID_CLAIMED;
	SmokeGridBrick replacement = (SmokeGridBrick)0;
	replacement.Coordinate = coordinate;
	replacement.HashSlot = destination;
	replacement.Generation = generation;
	replacement.State = NRI_SMOKE_GRID_NEW;
	replacement.Flags = flags & ~NRI_SMOKE_GRID_BRICK_BORROWED_FIRST_USE;
	gSmokeGridBricks[candidateIndex] = replacement;
	gSmokeGridHash[destination].Coordinate = coordinate;
	gSmokeGridHash[destination].BrickIndex = candidateIndex;
	gSmokeGridHash[destination].Generation = generation;
	DeviceMemoryBarrier();
	gSmokeGridHash[destination].State = NRI_SMOKE_GRID_NEW;
	gSmokeGridControl[0].Allocated++;
	gSmokeGridControl[0].Reclaimed++;
	gSmokeGridControl[0].BorrowedReturns++;
	gSmokeGridControl[0].BorrowedReclaims++;
	gSmokeGridControl[0].FirstUseReplacementAdmissions++;
	brickIndex = candidateIndex;
	return true;
}

bool SmokeGridAllocateAtSlotSerial(int3 coordinate, uint flags, uint slot, uint probe, out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	if (!SmokeGridPopFreeSerial(brickIndex))
	{
		InterlockedAdd(gSmokeGridControl[0].AllocationFailures, 1u);
		gSmokeGridControl[0].InsertionCapacityFailures++;
		return false;
	}

	uint generation = gSmokeGridBricks[brickIndex].Generation + 1u;
	if (generation == 0u)
		generation = 1u;
	gSmokeGridHash[slot].State = NRI_SMOKE_GRID_CLAIMED;
	SmokeGridBrick brick = (SmokeGridBrick)0;
	brick.Coordinate = coordinate;
	brick.HashSlot = slot;
	brick.Generation = generation;
	brick.State = NRI_SMOKE_GRID_NEW;
	brick.Flags = flags;
	gSmokeGridBricks[brickIndex] = brick;
	gSmokeGridHash[slot].Coordinate = coordinate;
	gSmokeGridHash[slot].BrickIndex = brickIndex;
	gSmokeGridHash[slot].Generation = generation;
	DeviceMemoryBarrier();
	gSmokeGridHash[slot].State = NRI_SMOKE_GRID_NEW;
	if (!SmokeGridAppendActive(brickIndex))
	{
		gSmokeGridHash[slot].State = NRI_SMOKE_GRID_TOMBSTONE;
		gSmokeGridBricks[brickIndex].State = NRI_SMOKE_GRID_EMPTY;
		SmokeGridPushFree(brickIndex);
		InterlockedAdd(gSmokeGridControl[0].AllocationFailures, 1u);
		gSmokeGridControl[0].InsertionActiveFailures++;
		brickIndex = 0xffffffffu;
		return false;
	}
	InterlockedAdd(gSmokeGridControl[0].ResidentCount, 1u);
	InterlockedAdd(gSmokeGridControl[0].Allocated, 1u);
	return true;
}

// Allocation passes are deliberately single-threaded. That makes NEW-key
// publication deterministic and avoids cross-threadgroup spin or duplicate
// insertion while retaining parallel preparation/deposition/simulation.
bool SmokeGridFindOrAllocateBrickSerial(int3 coordinate, uint flags, out uint brickIndex, out bool newlyAllocated)
{
	brickIndex = 0xffffffffu;
	newlyAllocated = false;
	if (gSmokeGridConstants.HashCapacity == 0u)
		return false;
	const uint mask = gSmokeGridConstants.HashCapacity - 1u;
	const uint base = SmokeGridHashCoordinate(coordinate) & mask;
	const uint probeLimit = min(NRI_SMOKE_GRID_HASH_PROBES, gSmokeGridConstants.HashCapacity);
	uint firstTombstone = 0xffffffffu;
	uint firstTombstoneProbe = 0u;
	[loop]
	for (uint probe = 0u; probe < probeLimit; ++probe)
	{
		const uint slot = (base + probe) & mask;
		const SmokeGridHashEntry entry = gSmokeGridHash[slot];
		if ((entry.State == NRI_SMOKE_GRID_RESIDENT || entry.State == NRI_SMOKE_GRID_NEW) && all(entry.Coordinate == coordinate))
		{
			if (SmokeGridValidateEntry(entry, coordinate, true, brickIndex))
			{
				gSmokeGridBricks[brickIndex].Flags |= flags;
				gSmokeGridBricks[brickIndex].IdleFrames = 0u;
				SmokeGridRecordControlProbe(probe + 1u, true);
				return true;
			}
			gSmokeGridHash[slot].State = NRI_SMOKE_GRID_TOMBSTONE;
			if (firstTombstone == 0xffffffffu)
			{
				firstTombstone = slot;
				firstTombstoneProbe = probe;
			}
		}
		else if (entry.State == NRI_SMOKE_GRID_TOMBSTONE && firstTombstone == 0xffffffffu)
		{
			firstTombstone = slot;
			firstTombstoneProbe = probe;
		}
		if (entry.State == NRI_SMOKE_GRID_EMPTY)
		{
			const uint destination = firstTombstone != 0xffffffffu ? firstTombstone : slot;
			const uint destinationProbe = firstTombstone != 0xffffffffu ? firstTombstoneProbe : probe;
			SmokeGridRecordControlProbe(probe + 1u, true);
			newlyAllocated = SmokeGridAllocateAtSlotSerial(coordinate, flags, destination, destinationProbe, brickIndex);
			return newlyAllocated;
		}
	}
	if (firstTombstone != 0xffffffffu)
	{
		SmokeGridRecordControlProbe(probeLimit, true);
		newlyAllocated = SmokeGridAllocateAtSlotSerial(coordinate, flags, firstTombstone, firstTombstoneProbe, brickIndex);
		return newlyAllocated;
	}
	InterlockedAdd(gSmokeGridControl[0].ProbeFailures, 1u);
	gSmokeGridControl[0].InsertionProbeLimitFailures++;
	SmokeGridRecordControlProbe(probeLimit, true);
	return false;
}

// The source-fair serial admission walk must distinguish a resident/NEW hit
// from a new-key decision before it charges a source turn.
bool SmokeGridFindBrickSerial(int3 coordinate, out uint brickIndex)
{
	brickIndex = 0xffffffffu;
	if (gSmokeGridConstants.HashCapacity == 0u)
		return false;
	const uint mask = gSmokeGridConstants.HashCapacity - 1u;
	const uint base = SmokeGridHashCoordinate(coordinate) & mask;
	const uint probeLimit = min(NRI_SMOKE_GRID_HASH_PROBES, gSmokeGridConstants.HashCapacity);
	[loop]
	for (uint probe = 0u; probe < probeLimit; ++probe)
	{
		const uint slot = (base + probe) & mask;
		const SmokeGridHashEntry entry = gSmokeGridHash[slot];
		if (entry.State == NRI_SMOKE_GRID_EMPTY)
		{
			SmokeGridRecordControlProbe(probe + 1u, false);
			return false;
		}
		if ((entry.State == NRI_SMOKE_GRID_RESIDENT || entry.State == NRI_SMOKE_GRID_NEW) &&
			SmokeGridValidateEntry(entry, coordinate, true, brickIndex))
		{
			SmokeGridRecordControlProbe(probe + 1u, false);
			return true;
		}
	}
	SmokeGridRecordControlProbe(probeLimit, false);
	gSmokeGridControl[0].LookupProbeLimitFailures++;
	return false;
}

uint SmokeGridCellIndex(uint brickIndex, uint3 local)
{
	return brickIndex * NRI_SMOKE_GRID_CELLS_PER_BRICK + SmokeGridLocalIndex(local);
}

float4 SmokeGridLoadScalar(uint ping, uint index)
{
	if (ping == 0u) return gSmokeGridScalarA[index];
	return gSmokeGridScalarB[index];
}

float4 SmokeGridLoadVelocity(uint ping, uint index)
{
	if (ping == 0u) return gSmokeGridVelocityA[index];
	return gSmokeGridVelocityB[index];
}

float4 SmokeGridLoadOptical(uint ping, uint index)
{
	if (ping == 0u) return gSmokeGridOpticalA[index];
	return gSmokeGridOpticalB[index];
}

float4 SmokeGridLoadDynamics(uint ping, uint index)
{
	if (ping == 0u) return gSmokeGridDynamicsA[index];
	return gSmokeGridDynamicsB[index];
}

void SmokeGridStoreScalar(uint ping, uint index, float4 value)
{
	if (ping == 0u) gSmokeGridScalarA[index] = value;
	else gSmokeGridScalarB[index] = value;
}

void SmokeGridStoreVelocity(uint ping, uint index, float4 value)
{
	if (ping == 0u) gSmokeGridVelocityA[index] = value;
	else gSmokeGridVelocityB[index] = value;
}

void SmokeGridStoreOptical(uint ping, uint index, float4 value)
{
	if (ping == 0u) gSmokeGridOpticalA[index] = value;
	else gSmokeGridOpticalB[index] = value;
}

void SmokeGridStoreDynamics(uint ping, uint index, float4 value)
{
	if (ping == 0u) gSmokeGridDynamicsA[index] = value;
	else gSmokeGridDynamicsB[index] = value;
}

float3 SmokeGridLoadCellVelocity(int3 cell, uint fieldPing)
{
	const int3 brickCoordinate = SmokeGridBrickCoordinate(cell);
	uint brickIndex;
	if (!SmokeGridLookupBrick(brickCoordinate, brickIndex))
		return gSmokeGridConstants.Wind;
	const uint cellIndex = SmokeGridCellIndex(brickIndex,
		SmokeGridLocalCoordinate(cell, brickCoordinate));
	return cellIndex < gSmokeGridConstants.CellCapacity ?
		SmokeGridLoadVelocity(fieldPing, cellIndex).xyz : gSmokeGridConstants.Wind;
}

float SmokeGridLoadCellVorticityMagnitude(int3 cell)
{
	const int3 brickCoordinate = SmokeGridBrickCoordinate(cell);
	uint brickIndex;
	if (!SmokeGridLookupBrick(brickCoordinate, brickIndex))
		return 0.0;
	const uint cellIndex = SmokeGridCellIndex(brickIndex,
		SmokeGridLocalCoordinate(cell, brickCoordinate));
	return cellIndex < gSmokeGridConstants.CellCapacity ?
		max(SmokeSourceFinite(gSmokeGridVorticity[cellIndex].w, 0.0), 0.0) : 0.0;
}

void SmokeGridSampleFields(float3 worldPosition, uint fieldPing,
	out float4 scalar, out float4 velocity, out float4 optical, out float4 dynamics)
{
	scalar = 0.0;
	velocity = 0.0;
	optical = 0.0;
	dynamics = 0.0;
	const float cellSize = max(gSmokeGridConstants.CellSize, 0.0001);
	const float3 gridPosition = worldPosition / cellSize - 0.5;
	const int3 lowerCell = (int3)floor(gridPosition);
	const float3 blend = saturate(gridPosition - (float3)lowerCell);
	[unroll]
	for (uint corner = 0u; corner < 8u; ++corner)
	{
		const uint3 offset = uint3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
		const int3 cell = lowerCell + (int3)offset;
		const int3 brickCoordinate = SmokeGridBrickCoordinate(cell);
		const float3 cornerWeight = lerp(1.0 - blend, blend, (float3)offset);
		const float weight = cornerWeight.x * cornerWeight.y * cornerWeight.z;
		float4 cornerScalar = 0.0;
		float4 cornerVelocity = float4(gSmokeGridConstants.Wind, 0.0);
		float4 cornerOptical = 0.0;
		float4 cornerDynamics = 0.0;
		uint brickIndex;
		if (SmokeGridLookupBrick(brickCoordinate, brickIndex))
		{
			const uint cellIndex = SmokeGridCellIndex(brickIndex, SmokeGridLocalCoordinate(cell, brickCoordinate));
			if (cellIndex < gSmokeGridConstants.CellCapacity)
			{
				cornerScalar = SmokeGridLoadScalar(fieldPing, cellIndex);
				cornerVelocity = SmokeGridLoadVelocity(fieldPing, cellIndex);
				cornerOptical = SmokeGridLoadOptical(fieldPing, cellIndex);
				cornerDynamics = SmokeGridLoadDynamics(fieldPing, cellIndex);
			}
		}
		scalar += cornerScalar * weight;
		velocity += cornerVelocity * weight;
		optical += cornerOptical * weight;
		dynamics += cornerDynamics * weight;
	}
}

float3 SmokeGridBacktrace(float3 worldPosition, float3 velocity, float deltaTime, out bool cflEvent, out bool clampEvent)
{
	float3 displacement = velocity * max(deltaTime, 0.0);
	const float distance = length(displacement);
	cflEvent = distance > max(gSmokeGridConstants.CellSize, 0.0001);
	const float maximum = min(max(gSmokeGridConstants.MaxBacktrace, 0.0),
		max(gSmokeGridConstants.CellSize, 0.0001) * (float)NRI_SMOKE_GRID_BRICK_AXIS);
	clampEvent = distance > maximum && distance > 1e-8;
	if (clampEvent)
		displacement *= maximum / distance;
	return worldPosition - displacement;
}

#endif
