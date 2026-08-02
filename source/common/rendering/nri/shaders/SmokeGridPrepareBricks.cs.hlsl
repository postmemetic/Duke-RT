#include "Include/SmokeGridResources.hlsli"

[numthreads(8, 8, 8)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
	const uint activeIndex = groupId.x;
	if (activeIndex >= min(SmokeGridActiveCount(), gSmokeGridConstants.BrickCapacity))
		return;
	const uint brickIndex = SmokeGridActiveBrick(activeIndex);
	if (brickIndex >= gSmokeGridConstants.BrickCapacity || gSmokeGridBricks[brickIndex].State != NRI_SMOKE_GRID_NEW)
		return;
	const uint cellIndex = SmokeGridCellIndex(brickIndex, groupThreadId);
	gSmokeGridScalarA[cellIndex] = 0.0;
	gSmokeGridScalarB[cellIndex] = 0.0;
	gSmokeGridVelocityA[cellIndex] = float4(gSmokeGridConstants.Wind, 0.0);
	gSmokeGridVelocityB[cellIndex] = float4(gSmokeGridConstants.Wind, 0.0);
	gSmokeGridOpticalA[cellIndex] = 0.0;
	gSmokeGridOpticalB[cellIndex] = 0.0;
	gSmokeGridDynamicsA[cellIndex] = 0.0;
	gSmokeGridDynamicsB[cellIndex] = 0.0;
	gSmokeGridDeposit0[cellIndex] = 0;
	gSmokeGridDeposit1[cellIndex] = 0;
	gSmokeGridDeposit2[cellIndex] = 0;
	gSmokeGridDeposit3[cellIndex] = 0;
	gSmokeGridVorticity[cellIndex] = 0.0;
	DeviceMemoryBarrierWithGroupSync();
	if (all(groupThreadId == 0u))
	{
		const SmokeGridBrick brick = gSmokeGridBricks[brickIndex];
		gSmokeGridBricks[brickIndex].State = NRI_SMOKE_GRID_RESIDENT;
		DeviceMemoryBarrier();
		if (brick.HashSlot < gSmokeGridConstants.HashCapacity)
		{
			const SmokeGridHashEntry entry = gSmokeGridHash[brick.HashSlot];
			if (entry.State == NRI_SMOKE_GRID_NEW && entry.BrickIndex == brickIndex &&
				entry.Generation == brick.Generation && all(entry.Coordinate == brick.Coordinate))
			{
				gSmokeGridHash[brick.HashSlot].State = NRI_SMOKE_GRID_RESIDENT;
			}
		}
	}
}
