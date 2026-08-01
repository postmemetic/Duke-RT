#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

enum class NRIVoxelComputeAlgorithm : uint32_t
{
	SerialV1 = 0,
	ParallelVoxelAdjacencyCoalescedV3 = 3,
};

constexpr uint32_t NRI_VOXEL_COMPUTE_PARALLEL_THREADS = 128;

struct NRIVoxelComputeParallelJobShape
{
	uint32_t slabCount = 0;
	uint32_t expectedPrimitiveCount = 0;
};

struct NRIVoxelComputeParallelJobSpan
{
	uint32_t scratchOffset = 0;
	uint32_t scratchCount = 0;
};

struct NRIVoxelComputeParallelDispatchPlan
{
	bool valid = false;
	uint32_t scratchRecordCount = 0;
	uint32_t maxSlabsPerJob = 0;
	uint32_t classifyEmitGroupCountX = 0;
	uint32_t jobCount = 0;
	uint64_t expectedPrimitiveCount = 0;
	std::vector<NRIVoxelComputeParallelJobSpan> jobs;
};

inline NRIVoxelComputeParallelDispatchPlan BuildNRIVoxelComputeParallelDispatchPlan(
	const std::vector<NRIVoxelComputeParallelJobShape>& shapes)
{
	NRIVoxelComputeParallelDispatchPlan plan = {};
	if (shapes.empty() || shapes.size() > std::numeric_limits<uint32_t>::max())
	{
		return plan;
	}

	uint64_t scratchRecordCount = 0;
	plan.jobs.reserve(shapes.size());
	for (const NRIVoxelComputeParallelJobShape& shape : shapes)
	{
		if (shape.slabCount == 0 || scratchRecordCount + shape.slabCount > std::numeric_limits<uint32_t>::max())
		{
			plan.jobs.clear();
			return plan;
		}

		plan.jobs.push_back({ (uint32_t)scratchRecordCount, shape.slabCount });
		scratchRecordCount += shape.slabCount;
		plan.maxSlabsPerJob = std::max(plan.maxSlabsPerJob, shape.slabCount);
		plan.expectedPrimitiveCount += shape.expectedPrimitiveCount;
	}

	plan.valid = true;
	plan.scratchRecordCount = (uint32_t)scratchRecordCount;
	plan.classifyEmitGroupCountX = (plan.maxSlabsPerJob + NRI_VOXEL_COMPUTE_PARALLEL_THREADS - 1u) /
		NRI_VOXEL_COMPUTE_PARALLEL_THREADS;
	plan.jobCount = (uint32_t)shapes.size();
	return plan;
}
