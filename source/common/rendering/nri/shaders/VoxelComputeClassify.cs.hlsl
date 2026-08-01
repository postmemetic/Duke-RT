#include "Include/VoxelComputeConstants.hlsli"

StructuredBuffer<NRIVoxelComputeJob> VoxelComputeJobs : register(t0, space0);
StructuredBuffer<NRIVoxelComputeSlabRecord> VoxelComputeSlabs : register(t1, space0);
StructuredBuffer<NRIVoxelComputeColorRunRecord> VoxelComputeColorRuns : register(t3, space0);
RWStructuredBuffer<NRIVoxelComputeSlabScratch> VoxelComputeScratch : register(u4, space1);

[numthreads(128, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint localSlab = dispatchThreadId.x;
	const uint jobIndex = dispatchThreadId.y;
	if (jobIndex >= gVoxelComputeConstants.JobCount)
	{
		return;
	}

	const NRIVoxelComputeJob job = VoxelComputeJobs[jobIndex];
	if (localSlab >= job.SlabCount || localSlab >= job.ScratchCount)
	{
		return;
	}

	if (job.ScratchOffset > gVoxelComputeConstants.ScratchRecordCount ||
		localSlab >= gVoxelComputeConstants.ScratchRecordCount - job.ScratchOffset)
	{
		return;
	}

	NRIVoxelComputeSlabScratch scratch;
	scratch.FaceCount = 0u;
	scratch.FaceOffset = 0u;
	scratch.VoxelCount = 0u;
	scratch.StatusMask = gVoxelComputeConstants.AlgorithmVersion == 2u ? 0u : NRI_VOXEL_COMPUTE_MISMATCH_ALGORITHM;

	const uint scratchIndex = job.ScratchOffset + localSlab;
	if (job.SlabOffset > gVoxelComputeConstants.SlabRecordCount ||
		localSlab >= gVoxelComputeConstants.SlabRecordCount - job.SlabOffset)
	{
		scratch.StatusMask |= NRI_VOXEL_COMPUTE_MISMATCH_SOURCE_RANGE;
		VoxelComputeScratch[scratchIndex] = scratch;
		return;
	}

	const NRIVoxelComputeSlabRecord slab = VoxelComputeSlabs[job.SlabOffset + localSlab];
	scratch.VoxelCount = slab.ZLength;
	const uint sideDirections =
		((slab.CullMask & 1u) != 0u ? 1u : 0u) +
		((slab.CullMask & 2u) != 0u ? 1u : 0u) +
		((slab.CullMask & 4u) != 0u ? 1u : 0u) +
		((slab.CullMask & 8u) != 0u ? 1u : 0u);

	uint faceCount = 0u;
	if (slab.ColorRunCount != 0u)
	{
		if (slab.ColorRunOffset > gVoxelComputeConstants.ColorRunRecordCount ||
			slab.ColorRunCount > gVoxelComputeConstants.ColorRunRecordCount - slab.ColorRunOffset)
		{
			scratch.StatusMask |= NRI_VOXEL_COMPUTE_MISMATCH_COLOR_RUN_RANGE;
		}
		else
		{
			const uint capFaceCount =
				((slab.CullMask & 16u) != 0u ? 1u : 0u) +
				((slab.CullMask & 32u) != 0u ? 1u : 0u);
			const bool sideCountFits = sideDirections == 0u || slab.ZLength <= 0x3fffffffu;
			if (!sideCountFits)
			{
				scratch.StatusMask |= NRI_VOXEL_COMPUTE_MISMATCH_ARITHMETIC_OVERFLOW;
			}
			else
			{
				faceCount = sideDirections * slab.ZLength + capFaceCount;
			}
		}
	}

	scratch.FaceCount = faceCount;
	VoxelComputeScratch[scratchIndex] = scratch;
}
