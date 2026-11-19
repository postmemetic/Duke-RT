#pragma once

#include <cstdint>
#include <vector>

enum class NRIVoxelComputeBatchRejectReason : uint8_t
{
	None,
	InvalidRequest,
	StaleGeneration,
	Capacity,
	IncompatibleDuplicate,
	BatchCapacity,
};

struct NRIVoxelComputeBatchSourceRange
{
	uint32_t slabOffset = 0;
	uint32_t slabCount = 0;
	uint32_t colorRunOffset = 0;
	uint32_t colorRunCount = 0;
};

struct NRIVoxelComputeBatchOutputRange
{
	uint32_t offset = 0;
	uint32_t count = 0;
	uint32_t capacity = 0;
};

struct NRIVoxelComputeBatchPlanRequest
{
	uint64_t meshKey = 0;
	uint64_t levelGeneration = 0;
	uint64_t requestKey = 0;
	uint64_t materialBindingKey = 0;
	NRIVoxelComputeBatchSourceRange source;
	NRIVoxelComputeBatchOutputRange vertices;
	NRIVoxelComputeBatchOutputRange indices;
	NRIVoxelComputeBatchOutputRange primitives;
	uint64_t reservationBytes = 0;
	uint32_t priority = 0;
	uint64_t age = 0;
	uint64_t overflowGeneration = 0;
};

struct NRIVoxelComputeBatchPlanSettings
{
	uint64_t activeLevelGeneration = 0;
	uint32_t maxJobsPerBatch = 0;
	uint64_t maxBytesPerBatch = 0;
};

struct NRIVoxelComputeBatchMaterialBinding
{
	uint32_t requestIndex = 0;
	uint64_t requestKey = 0;
	uint64_t materialBindingKey = 0;
};

struct NRIVoxelComputeBatchPlannedJob
{
	uint64_t meshKey = 0;
	uint64_t levelGeneration = 0;
	uint32_t ownerRequestIndex = 0;
	NRIVoxelComputeBatchSourceRange source;
	NRIVoxelComputeBatchOutputRange vertices;
	NRIVoxelComputeBatchOutputRange indices;
	NRIVoxelComputeBatchOutputRange primitives;
	uint64_t reservationBytes = 0;
	uint32_t priority = 0;
	uint64_t age = 0;
	bool oversizedExclusive = false;
	std::vector<NRIVoxelComputeBatchMaterialBinding> bindings;
	uint64_t overflowGeneration = 0;
};

struct NRIVoxelComputeBatchPlanBatch
{
	std::vector<uint32_t> jobIndices;
	uint64_t reservationBytes = 0;
	bool oversizedExclusive = false;
};

struct NRIVoxelComputeBatchRejectedRequest
{
	uint32_t requestIndex = 0;
	NRIVoxelComputeBatchRejectReason reason = NRIVoxelComputeBatchRejectReason::None;
};

struct NRIVoxelComputeBatchPlanStats
{
	uint32_t inputRequests = 0;
	uint32_t uniqueJobs = 0;
	uint32_t dedupeHits = 0;
	uint32_t materialBindings = 0;
	uint32_t rejectedRequests = 0;
	uint32_t staleGenerationRequests = 0;
	uint32_t capacityRejectedRequests = 0;
	uint32_t oversizedExclusiveBatches = 0;
};

struct NRIVoxelComputeBatchPlan
{
	std::vector<NRIVoxelComputeBatchPlannedJob> jobs;
	std::vector<NRIVoxelComputeBatchPlanBatch> batches;
	std::vector<NRIVoxelComputeBatchRejectedRequest> rejected;
	NRIVoxelComputeBatchPlanStats stats;
};

NRIVoxelComputeBatchPlan BuildNRIVoxelComputeBatchPlan(
	const std::vector<NRIVoxelComputeBatchPlanRequest>& requests,
	const NRIVoxelComputeBatchPlanSettings& settings);
