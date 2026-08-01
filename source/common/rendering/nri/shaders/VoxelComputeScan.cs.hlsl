#include "Include/VoxelComputeConstants.hlsli"

StructuredBuffer<NRIVoxelComputeJob> VoxelComputeJobs : register(t0, space0);
RWStructuredBuffer<NRIVoxelComputeResult> VoxelComputeResults : register(u0, space1);
RWStructuredBuffer<NRIVoxelComputeSlabScratch> VoxelComputeScratch : register(u4, space1);

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint jobIndex = dispatchThreadId.x;
	if (jobIndex >= gVoxelComputeConstants.JobCount)
	{
		return;
	}

	const NRIVoxelComputeJob job = VoxelComputeJobs[jobIndex];
	uint faceCount = 0u;
	uint voxelCount = 0u;
	uint mismatchMask = gVoxelComputeConstants.AlgorithmVersion == 2u ? 0u : NRI_VOXEL_COMPUTE_MISMATCH_ALGORITHM;
	if (job.ScratchCount != job.SlabCount || job.ScratchOffset > gVoxelComputeConstants.ScratchRecordCount ||
		job.ScratchCount > gVoxelComputeConstants.ScratchRecordCount - job.ScratchOffset)
	{
		mismatchMask |= NRI_VOXEL_COMPUTE_MISMATCH_SCRATCH_RANGE;
	}
	else
	{
		for (uint localSlab = 0u; localSlab < job.ScratchCount; ++localSlab)
		{
			const uint scratchIndex = job.ScratchOffset + localSlab;
			NRIVoxelComputeSlabScratch scratch = VoxelComputeScratch[scratchIndex];
			scratch.FaceOffset = faceCount;
			VoxelComputeScratch[scratchIndex] = scratch;
			mismatchMask |= scratch.StatusMask;
			if (scratch.FaceCount > 0xffffffffu - faceCount || scratch.VoxelCount > 0xffffffffu - voxelCount)
			{
				mismatchMask |= NRI_VOXEL_COMPUTE_MISMATCH_ARITHMETIC_OVERFLOW;
			}
			else
			{
				faceCount += scratch.FaceCount;
				voxelCount += scratch.VoxelCount;
			}
		}
	}

	const bool arithmeticFits = faceCount <= 0xffffffffu / 6u;
	const uint indexCount = arithmeticFits ? faceCount * 6u : 0u;
	const uint vertexCount = faceCount <= 0xffffffffu / 4u ? faceCount * 4u : 0u;
	const uint primitiveCount = faceCount <= 0xffffffffu / 2u ? faceCount * 2u : 0u;
	if (!arithmeticFits || faceCount > 0xffffffffu / 4u || faceCount > 0xffffffffu / 2u)
	{
		mismatchMask |= NRI_VOXEL_COMPUTE_MISMATCH_ARITHMETIC_OVERFLOW;
	}
	if (faceCount > job.VertexCapacity / 4u || faceCount > job.IndexCapacity / 6u || faceCount > job.PrimitiveCapacity / 2u)
	{
		mismatchMask |= NRI_VOXEL_COMPUTE_MISMATCH_OUTPUT_OVERFLOW;
	}
	if (job.VertexOffset > gVoxelComputeConstants.VertexRecordCount ||
		job.VertexCapacity > gVoxelComputeConstants.VertexRecordCount - job.VertexOffset ||
		job.IndexOffset > gVoxelComputeConstants.IndexRecordCount ||
		job.IndexCapacity > gVoxelComputeConstants.IndexRecordCount - job.IndexOffset ||
		job.PrimitiveOffset > gVoxelComputeConstants.PrimitiveRecordCount ||
		job.PrimitiveCapacity > gVoxelComputeConstants.PrimitiveRecordCount - job.PrimitiveOffset)
	{
		mismatchMask |= NRI_VOXEL_COMPUTE_MISMATCH_OUTPUT_OVERFLOW;
	}

	mismatchMask |= faceCount == job.ExpectedFaces ? 0u : 1u;
	mismatchMask |= indexCount == job.ExpectedIndices ? 0u : 2u;
	mismatchMask |= vertexCount == job.ExpectedVerticesNoDedupe ? 0u : 4u;
	mismatchMask |= voxelCount == job.ExpectedVoxels ? 0u : 8u;
	mismatchMask |= primitiveCount == job.ExpectedFaces * 2u ? 0u : 16u;

	NRIVoxelComputeResult result;
	result.FaceCount = faceCount;
	result.IndexCount = indexCount;
	result.VertexCountNoDedupe = vertexCount;
	result.VoxelCount = voxelCount;
	result.SlabCount = job.SlabCount;
	result.PrimitiveCount = primitiveCount;
	result.MismatchMask = mismatchMask;
	result.JobId = job.JobId;
	result.Status = mismatchMask == 0u ? NRI_VOXEL_COMPUTE_STATUS_SCAN_OK : NRI_VOXEL_COMPUTE_STATUS_SCAN_MISMATCH;
	result.VertexHash = 0u;
	result.IndexHash = 0u;
	result.PrimitiveHash = 0u;
	VoxelComputeResults[jobIndex] = result;
}
