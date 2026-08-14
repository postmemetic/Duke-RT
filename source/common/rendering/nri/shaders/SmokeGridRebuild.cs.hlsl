#include "Include/SmokeGridResources.hlsli"

groupshared uint gSmokeGridOccupied[NRI_SMOKE_GRID_CELLS_PER_BRICK];
groupshared uint gSmokeGridHashLo[NRI_SMOKE_GRID_CELLS_PER_BRICK];
groupshared uint gSmokeGridHashHi[NRI_SMOKE_GRID_CELLS_PER_BRICK];
groupshared uint gSmokeGridReclaimDecision;

uint SmokeGridMixFieldHash(uint hash, uint value)
{
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	hash ^= value;
	hash *= 0x846ca68bu;
	return hash ^ (hash >> 16u);
}

void SmokeGridCellFieldHash(int3 cellCoordinate, float4 scalar, float4 velocity,
	float4 optical, float4 dynamics, out uint hashLo, out uint hashHi)
{
	hashLo = SmokeGridHashCoordinate(cellCoordinate) ^ 0x9e3779b9u;
	hashHi = SmokeGridHashCoordinate(cellCoordinate.zxy) ^ 0x85ebca6bu;
	const uint4 scalarBits = asuint(scalar);
	const uint4 velocityBits = asuint(velocity);
	const uint4 opticalBits = asuint(optical);
	const uint4 dynamicsBits = asuint(dynamics);
	[unroll]
	for (uint component = 0u; component < 4u; ++component)
	{
		hashLo = SmokeGridMixFieldHash(hashLo, scalarBits[component]);
		hashLo = SmokeGridMixFieldHash(hashLo, opticalBits[component]);
		hashHi = SmokeGridMixFieldHash(hashHi, velocityBits[component]);
		hashHi = SmokeGridMixFieldHash(hashHi, dynamicsBits[component]);
	}
}

[numthreads(8, 8, 8)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
	const uint activeIndex = groupId.x;
	if (activeIndex >= min(SmokeGridActiveCount(), gSmokeGridConstants.BrickCapacity))
		return;
	const uint brickIndex = SmokeGridActiveBrick(activeIndex);
	if (brickIndex >= gSmokeGridConstants.BrickCapacity)
		return;
	const SmokeGridBrick brick = gSmokeGridBricks[brickIndex];
	if (brick.State != NRI_SMOKE_GRID_RESIDENT)
		return;

	const uint outputPing = 1u - min(gSmokeGridConstants.FieldPing, 1u);
	const uint localIndex = SmokeGridLocalIndex(groupThreadId);
	const uint cellIndex = SmokeGridCellIndex(brickIndex, groupThreadId);
	const float4 scalar = SmokeGridLoadScalar(outputPing, cellIndex);
	const float4 velocity = SmokeGridLoadVelocity(outputPing, cellIndex);
	const float4 optical = SmokeGridLoadOptical(outputPing, cellIndex);
	const float4 dynamics = SmokeGridLoadDynamics(outputPing, cellIndex);
	const float threshold = max(gSmokeGridConstants.ActiveThreshold, 0.0);
	const bool opticalOccupied = any(abs(optical) > 0.0);
	gSmokeGridOccupied[localIndex] =
		(max(scalar.z, max(optical.x, max(optical.y, optical.z))) > threshold ? 1u : 0u) |
		(opticalOccupied ? 2u : 0u);
	SmokeGridCellFieldHash(SmokeGridCellCoordinate(brick.Coordinate, groupThreadId), scalar,
		velocity, optical, dynamics, gSmokeGridHashLo[localIndex], gSmokeGridHashHi[localIndex]);
	GroupMemoryBarrierWithGroupSync();

	[unroll]
	for (uint stride = NRI_SMOKE_GRID_CELLS_PER_BRICK / 2u; stride > 0u; stride >>= 1u)
	{
		if (localIndex < stride)
		{
			gSmokeGridOccupied[localIndex] |= gSmokeGridOccupied[localIndex + stride];
			gSmokeGridHashLo[localIndex] ^= gSmokeGridHashLo[localIndex + stride];
			gSmokeGridHashHi[localIndex] ^= gSmokeGridHashHi[localIndex + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (localIndex == 0u)
	{
		InterlockedXor(gSmokeGridControl[0].FieldHashLo, gSmokeGridHashLo[0]);
		InterlockedXor(gSmokeGridControl[0].FieldHashHi, gSmokeGridHashHi[0]);
		gSmokeGridReclaimDecision = 0u;
		SmokeGridBrick updated = brick;
		const uint retainedPolicyFlags = updated.Flags & NRI_SMOKE_GRID_BRICK_BORROWED_FIRST_USE;
		const uint opticalFlags = (gSmokeGridOccupied[0] & 2u) != 0u ?
			NRI_SMOKE_GRID_BRICK_OPTICAL_CONTENT : 0u;
		if ((gSmokeGridOccupied[0] & 1u) != 0u)
		{
			updated.IdleFrames = 0u;
			updated.Flags = retainedPolicyFlags | NRI_SMOKE_GRID_BRICK_CONTENT |
				opticalFlags;
			gSmokeGridBricks[brickIndex] = updated;
			if (!SmokeGridAppendNextActive(brickIndex))
				InterlockedAdd(gSmokeGridControl[0].AllocationFailures, 1u);
			InterlockedAdd(gSmokeGridControl[0].OccupiedBricks, 1u);
		}
		else
		{
			updated.IdleFrames = min(updated.IdleFrames + 1u, 0xfffffffeu);
			updated.Flags = retainedPolicyFlags | NRI_SMOKE_GRID_BRICK_HALO | opticalFlags;
			InterlockedAdd(gSmokeGridControl[0].EmptyBricks, 1u);
			const bool graceExpired = updated.IdleFrames >= gSmokeGridConstants.ReclaimGrace;
			// Empty topology is a cache, not source data. Under pressure, release it
			// immediately so the next frame's injection commands retain priority.
			const bool capacityPressure = gSmokeGridControl[0].FreeCount < SmokeGridEmissionReserve();
			const bool reclaimable = graceExpired || capacityPressure;
			bool mappingValid = false;
			if (updated.HashSlot < gSmokeGridConstants.HashCapacity)
			{
				const SmokeGridHashEntry entry = gSmokeGridHash[updated.HashSlot];
				mappingValid = entry.State == NRI_SMOKE_GRID_RESIDENT && entry.BrickIndex == brickIndex &&
					entry.Generation == updated.Generation && all(entry.Coordinate == updated.Coordinate);
			}
			if (reclaimable && mappingValid)
			{
				gSmokeGridBricks[brickIndex] = updated;
				gSmokeGridReclaimDecision = 1u;
			}
			else
			{
				gSmokeGridBricks[brickIndex] = updated;
				if (reclaimable)
				{
					InterlockedAdd(gSmokeGridControl[0].ProbeFailures, 1u);
					InterlockedAdd(gSmokeGridControl[0].ReclaimInvalidMappingFailures, 1u);
				}
				if (!SmokeGridAppendNextActive(brickIndex))
					InterlockedAdd(gSmokeGridControl[0].AllocationFailures, 1u);
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (gSmokeGridReclaimDecision != 0u)
	{
		SmokeGridStoreScalar(outputPing, cellIndex, 0.0);
		SmokeGridStoreVelocity(outputPing, cellIndex, float4(gSmokeGridConstants.Wind, 0.0));
		SmokeGridStoreOptical(outputPing, cellIndex, 0.0);
		SmokeGridStoreDynamics(outputPing, cellIndex, 0.0);
	}
	DeviceMemoryBarrierWithGroupSync();
	if (localIndex == 0u && gSmokeGridReclaimDecision != 0u)
	{
		gSmokeGridHash[brick.HashSlot].State = NRI_SMOKE_GRID_TOMBSTONE;
		DeviceMemoryBarrier();
		SmokeGridBrick reclaimed = brick;
		reclaimed.HashSlot = 0xffffffffu;
		reclaimed.State = NRI_SMOKE_GRID_EMPTY;
		reclaimed.IdleFrames = 0u;
		reclaimed.Flags = 0u;
		gSmokeGridBricks[brickIndex] = reclaimed;
		DeviceMemoryBarrier();
		if (SmokeGridPushFree(brickIndex))
		{
			uint ignoredResidentCount;
			InterlockedAdd(gSmokeGridControl[0].ResidentCount, 0xffffffffu, ignoredResidentCount);
			InterlockedAdd(gSmokeGridControl[0].Reclaimed, 1u);
			if ((brick.Flags & NRI_SMOKE_GRID_BRICK_BORROWED_FIRST_USE) != 0u)
			{
				InterlockedAdd(gSmokeGridControl[0].BorrowedReturns, 1u);
				InterlockedAdd(gSmokeGridControl[0].BorrowedReclaims, 1u);
			}
		}
		else
		{
			InterlockedAdd(gSmokeGridControl[0].AllocationFailures, 1u);
		}
	}
}
