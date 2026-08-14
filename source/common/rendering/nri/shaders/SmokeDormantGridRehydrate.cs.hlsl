#include "Include/SmokeDormantGridResources.hlsli"

groupshared uint sArchiveIndex;
groupshared uint sFineIndex;
groupshared uint sFineHashSlot;
groupshared uint sFineGeneration;
groupshared uint sValid;
groupshared uint sOpticalContent;

void RetainCoarse(uint resultIndex, SmokeDormantGridWork work, uint outcome)
{
	SmokeDormantGridResult result = (SmokeDormantGridResult)0;
	result.Coordinate = work.Coordinate;
	result.InputGeneration = work.Generation;
	result.Outcome = outcome;
	result.ArchiveIndex = sArchiveIndex;
	result.FineIndex = 0xffffffffu;
	gDormantResults[resultIndex] = result;
	InterlockedAdd(gDormantControl[0].RehydrateRetainedCoarse, 1u);
}

[numthreads(64, 1, 1)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
	const uint workIndex = groupId.x;
	if (workIndex >= gDormantConstants.PromotionCount)
		return;
	const uint resultIndex = gDormantConstants.DemotionCount + workIndex;
	const SmokeDormantGridWork work = gDormantPromotions[workIndex];
	if (groupThreadId.x == 0u)
	{
		sArchiveIndex = sFineIndex = sFineHashSlot = 0xffffffffu;
		sFineGeneration = 0u;
		sValid = 0u;
		sOpticalContent = 0u;
		if (workIndex == 0u) gDormantControl[0].FrameIndex = gDormantConstants.FrameIndex;
		InterlockedAdd(gDormantControl[0].RehydrateAttempts, 1u);
		InterlockedAdd(gDormantControl[0].RehydrateWorkExecuted, 1u);
		if (work.Epoch != gDormantConstants.SimulationEpoch ||
			gDormantControl[0].Epoch != gDormantConstants.SimulationEpoch)
		{
			RetainCoarse(resultIndex, work, NRI_SMOKE_DORMANT_OUTCOME_STALE_EPOCH);
			InterlockedAdd(gDormantControl[0].RehydrateStale, 1u);
		}
		else if (work.Generation == 0u || gDormantConstants.ArchiveHashCapacity == 0u)
		{
			RetainCoarse(resultIndex, work, NRI_SMOKE_DORMANT_OUTCOME_STALE_GENERATION);
			InterlockedAdd(gDormantControl[0].RehydrateStale, 1u);
		}
		else
		{
			const uint archiveMask = gDormantConstants.ArchiveHashCapacity - 1u;
			const uint archiveBase = SmokeDormantGridHashCoordinate(work.Coordinate) & archiveMask;
			[loop]
			for (uint probe = 0u; probe < min(NRI_SMOKE_DORMANT_GRID_HASH_PROBES,
				gDormantConstants.ArchiveHashCapacity); ++probe)
			{
				const SmokeDormantGridHashEntry entry = gDormantHash[(archiveBase + probe) & archiveMask];
				if (entry.State == NRI_SMOKE_DORMANT_EMPTY) break;
				if (entry.State != NRI_SMOKE_DORMANT_RESIDENT || entry.Generation != work.Generation ||
					!all(entry.Coordinate == work.Coordinate) ||
					entry.ArchiveIndex >= gDormantConstants.ArchiveCapacity) continue;
				const SmokeDormantGridRecord record = gDormantRecords[entry.ArchiveIndex];
				if (record.State == NRI_SMOKE_DORMANT_RESIDENT && record.Generation == work.Generation &&
					record.HashSlot == ((archiveBase + probe) & archiveMask) &&
					record.Epoch == work.Epoch && all(record.Coordinate == work.Coordinate))
					sArchiveIndex = entry.ArchiveIndex;
				break;
			}
			if (sArchiveIndex == 0xffffffffu)
			{
				RetainCoarse(resultIndex, work, NRI_SMOKE_DORMANT_OUTCOME_STALE_GENERATION);
				InterlockedAdd(gDormantControl[0].RehydrateStale, 1u);
			}
			else if (!DormantPopFineFree(sFineIndex))
			{
				RetainCoarse(resultIndex, work, NRI_SMOKE_DORMANT_OUTCOME_FINE_CAPACITY);
				InterlockedAdd(gDormantControl[0].RehydrateFineCapacity, 1u);
			}
			else
			{
				const uint fineMask = gDormantConstants.FineHashCapacity - 1u;
				const uint fineBase = SmokeDormantGridHashCoordinate(work.Coordinate) & fineMask;
				bool coordinateConflict = false;
				[loop]
				for (uint probe = 0u; probe < min(NRI_SMOKE_DORMANT_GRID_HASH_PROBES,
					gDormantConstants.FineHashCapacity); ++probe)
				{
					const uint slot = (fineBase + probe) & fineMask;
					const SmokeGridHashEntry existing = gDormantFineHash[slot];
					if ((existing.State == NRI_SMOKE_GRID_RESIDENT ||
						existing.State == NRI_SMOKE_GRID_NEW ||
						existing.State == NRI_SMOKE_GRID_CLAIMED) &&
						all(existing.Coordinate == work.Coordinate))
					{
						coordinateConflict = true;
						break;
					}
					uint original;
					InterlockedCompareExchange(gDormantFineHash[slot].State,
						NRI_SMOKE_GRID_EMPTY, NRI_SMOKE_GRID_CLAIMED, original);
					if (original == NRI_SMOKE_GRID_TOMBSTONE)
						InterlockedCompareExchange(gDormantFineHash[slot].State,
							NRI_SMOKE_GRID_TOMBSTONE, NRI_SMOKE_GRID_CLAIMED, original);
					if (original == NRI_SMOKE_GRID_EMPTY || original == NRI_SMOKE_GRID_TOMBSTONE)
					{
						sFineHashSlot = slot;
						InterlockedMax(gDormantControl[0].MaximumFineProbe, probe + 1u);
						break;
					}
				}
				if (coordinateConflict || sFineHashSlot == 0xffffffffu)
				{
					DormantPushFineFree(sFineIndex);
					RetainCoarse(resultIndex, work, NRI_SMOKE_DORMANT_OUTCOME_HASH_FAILURE);
					InterlockedAdd(gDormantControl[0].RehydrateHashFailures, 1u);
				}
				else
				{
					sFineGeneration = gDormantFineBricks[sFineIndex].Generation + 1u;
					if (sFineGeneration == 0u) sFineGeneration = 1u;
					SmokeGridBrick brick = (SmokeGridBrick)0;
					brick.Coordinate = work.Coordinate;
					brick.HashSlot = sFineHashSlot;
					brick.Generation = sFineGeneration;
					brick.State = NRI_SMOKE_GRID_CLAIMED;
					brick.Flags = NRI_SMOKE_GRID_BRICK_CONTENT;
					gDormantFineBricks[sFineIndex] = brick;
					gDormantFineHash[sFineHashSlot].Coordinate = work.Coordinate;
					gDormantFineHash[sFineHashSlot].BrickIndex = sFineIndex;
					gDormantFineHash[sFineHashSlot].Generation = sFineGeneration;
					sValid = 1u;
				}
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (sValid == 0u) return;

	const uint archiveBase = sArchiveIndex * NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK;
	const uint fineBase = sFineIndex * NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK;
	for (uint localIndex = groupThreadId.x; localIndex < NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK;
		localIndex += 64u)
	{
		const uint archiveCell = archiveBase + localIndex;
		const uint fineCell = fineBase + localIndex;
		const float4 scalar = gDormantScalar[archiveCell];
		const float4 velocity = gDormantVelocity[archiveCell];
		const float4 optical = gDormantOptical[archiveCell];
		const float4 dynamics = gDormantDynamics[archiveCell];
		if (any(abs(optical) > 0.0))
			InterlockedOr(sOpticalContent, 1u);
		DormantStoreFineFields(0u, fineCell, scalar, velocity, optical, dynamics);
		DormantStoreFineFields(1u, fineCell, scalar, velocity, optical, dynamics);
		gDormantFineDeposit0[fineCell] = 0;
		gDormantFineDeposit1[fineCell] = 0;
		gDormantFineDeposit2[fineCell] = 0;
		gDormantFineDeposit3[fineCell] = 0;
	}
	GroupMemoryBarrierWithGroupSync();
	if (groupThreadId.x == 0u)
	{
		if (sOpticalContent != 0u)
			InterlockedOr(gDormantFineBricks[sFineIndex].Flags, NRI_SMOKE_GRID_BRICK_OPTICAL_CONTENT);
		uint activeDestination;
		if (gDormantConstants.ActivePing == 0u)
			InterlockedAdd(gDormantFineControl[0].ActiveCountA, 1u, activeDestination);
		else
			InterlockedAdd(gDormantFineControl[0].ActiveCountB, 1u, activeDestination);
		if (activeDestination >= gDormantConstants.FineBrickCapacity)
		{
			uint ignored;
			if (gDormantConstants.ActivePing == 0u)
				InterlockedMin(gDormantFineControl[0].ActiveCountA,
					gDormantConstants.FineBrickCapacity, ignored);
			else
				InterlockedMin(gDormantFineControl[0].ActiveCountB,
					gDormantConstants.FineBrickCapacity, ignored);
			gDormantFineHash[sFineHashSlot].State = NRI_SMOKE_GRID_TOMBSTONE;
			gDormantFineBricks[sFineIndex].State = NRI_SMOKE_GRID_EMPTY;
			DormantPushFineFree(sFineIndex);
			RetainCoarse(resultIndex, work, NRI_SMOKE_DORMANT_OUTCOME_FINE_ACTIVE_CAPACITY);
			sValid = 0u;
		}
		else
		{
			if (gDormantConstants.ActivePing == 0u)
				gDormantFineActiveA[activeDestination] = sFineIndex;
			else
				gDormantFineActiveB[activeDestination] = sFineIndex;
			DeviceMemoryBarrier();
			gDormantFineBricks[sFineIndex].State = NRI_SMOKE_GRID_RESIDENT;
			gDormantFineHash[sFineHashSlot].State = NRI_SMOKE_GRID_RESIDENT;
			uint fineResident;
			InterlockedAdd(gDormantFineControl[0].ResidentCount, 1u, fineResident);
			DeviceMemoryBarrier();
			const SmokeDormantGridRecord archive = gDormantRecords[sArchiveIndex];
			gDormantHash[archive.HashSlot].State = NRI_SMOKE_DORMANT_TOMBSTONE;
			gDormantRecords[sArchiveIndex].State = NRI_SMOKE_DORMANT_EMPTY;
			DormantPushArchiveFree(sArchiveIndex);
			uint archiveResident;
			InterlockedAdd(gDormantControl[0].ResidentCount, 0xffffffffu, archiveResident);
			InterlockedAdd(gDormantControl[0].RehydratePublished, 1u);
			SmokeDormantGridResult result = (SmokeDormantGridResult)0;
			result.Coordinate = work.Coordinate;
			result.InputGeneration = work.Generation;
			result.OutputGeneration = sFineGeneration;
			result.Outcome = NRI_SMOKE_DORMANT_OUTCOME_REHYDRATED;
			result.ArchiveIndex = sArchiveIndex;
			result.FineIndex = sFineIndex;
			gDormantResults[resultIndex] = result;
		}
	}
}
