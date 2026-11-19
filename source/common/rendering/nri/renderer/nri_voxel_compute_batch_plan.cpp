#include "nri_voxel_compute_batch_plan.h"

#include <algorithm>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace
{
	using MeshPlanKey = std::tuple<uint64_t, uint64_t, uint64_t>;

	bool RangeFits(const NRIVoxelComputeBatchOutputRange& range)
	{
		return range.count != 0 && range.count <= range.capacity;
	}

	bool SameGeometryContract(
		const NRIVoxelComputeBatchPlannedJob& job,
		const NRIVoxelComputeBatchPlanRequest& request)
	{
		return
			job.source.slabCount == request.source.slabCount &&
			job.source.colorRunCount == request.source.colorRunCount &&
			job.vertices.count == request.vertices.count &&
			job.indices.count == request.indices.count &&
			job.primitives.count == request.primitives.count;
	}

	bool RequestRanksBefore(
		const NRIVoxelComputeBatchPlanRequest& a,
		uint32_t aIndex,
		const NRIVoxelComputeBatchPlanRequest& b,
		uint32_t bIndex)
	{
		if (a.priority != b.priority)
		{
			return a.priority > b.priority;
		}
		if (a.age != b.age)
		{
			return a.age > b.age;
		}
		if (a.requestKey != b.requestKey)
		{
			return a.requestKey < b.requestKey;
		}
		return aIndex < bIndex;
	}

	void Reject(
		NRIVoxelComputeBatchPlan& plan,
		uint32_t requestIndex,
		NRIVoxelComputeBatchRejectReason reason)
	{
		plan.rejected.push_back({ requestIndex, reason });
		plan.stats.rejectedRequests++;
		plan.stats.staleGenerationRequests += reason == NRIVoxelComputeBatchRejectReason::StaleGeneration ? 1u : 0u;
		plan.stats.capacityRejectedRequests += reason == NRIVoxelComputeBatchRejectReason::Capacity ? 1u : 0u;
	}

	bool AddWouldOverflow(uint64_t a, uint64_t b)
	{
		return b > std::numeric_limits<uint64_t>::max() - a;
	}
}

NRIVoxelComputeBatchPlan BuildNRIVoxelComputeBatchPlan(
	const std::vector<NRIVoxelComputeBatchPlanRequest>& requests,
	const NRIVoxelComputeBatchPlanSettings& settings)
{
	NRIVoxelComputeBatchPlan plan = {};
	plan.stats.inputRequests = (uint32_t)requests.size();
	if (requests.empty())
	{
		return plan;
	}

	if (settings.maxJobsPerBatch == 0)
	{
		for (uint32_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex)
		{
			Reject(plan, requestIndex, NRIVoxelComputeBatchRejectReason::BatchCapacity);
		}
		return plan;
	}

	std::vector<uint32_t> order(requests.size());
	for (uint32_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex)
	{
		order[requestIndex] = requestIndex;
	}
	std::sort(order.begin(), order.end(), [&requests](uint32_t aIndex, uint32_t bIndex)
	{
		const NRIVoxelComputeBatchPlanRequest& a = requests[aIndex];
		const NRIVoxelComputeBatchPlanRequest& b = requests[bIndex];
		if (a.levelGeneration != b.levelGeneration)
		{
			return a.levelGeneration < b.levelGeneration;
		}
		if (a.meshKey != b.meshKey)
		{
			return a.meshKey < b.meshKey;
		}
		return RequestRanksBefore(a, aIndex, b, bIndex);
	});

	std::map<MeshPlanKey, uint32_t> jobByMesh;
	for (uint32_t requestIndex : order)
	{
		const NRIVoxelComputeBatchPlanRequest& request = requests[requestIndex];
		if (request.meshKey == 0 || request.levelGeneration == 0 || request.requestKey == 0 ||
			request.source.slabCount == 0 || request.source.colorRunCount == 0 || request.reservationBytes == 0)
		{
			Reject(plan, requestIndex, NRIVoxelComputeBatchRejectReason::InvalidRequest);
			continue;
		}
		if (settings.activeLevelGeneration != 0 && request.levelGeneration != settings.activeLevelGeneration)
		{
			Reject(plan, requestIndex, NRIVoxelComputeBatchRejectReason::StaleGeneration);
			continue;
		}
		if (!RangeFits(request.vertices) || !RangeFits(request.indices) || !RangeFits(request.primitives))
		{
			Reject(plan, requestIndex, NRIVoxelComputeBatchRejectReason::Capacity);
			continue;
		}

		const MeshPlanKey meshKey = { request.levelGeneration, request.meshKey, request.overflowGeneration };
		auto found = jobByMesh.find(meshKey);
		if (found == jobByMesh.end())
		{
			NRIVoxelComputeBatchPlannedJob job = {};
			job.meshKey = request.meshKey;
			job.overflowGeneration = request.overflowGeneration;
			job.levelGeneration = request.levelGeneration;
			job.ownerRequestIndex = requestIndex;
			job.source = request.source;
			job.vertices = request.vertices;
			job.indices = request.indices;
			job.primitives = request.primitives;
			job.reservationBytes = request.reservationBytes;
			job.priority = request.priority;
			job.age = request.age;
			job.bindings.push_back({ requestIndex, request.requestKey, request.materialBindingKey });
			const uint32_t jobIndex = (uint32_t)plan.jobs.size();
			plan.jobs.push_back(std::move(job));
			jobByMesh.emplace(meshKey, jobIndex);
			continue;
		}

		NRIVoxelComputeBatchPlannedJob& job = plan.jobs[found->second];
		if (!SameGeometryContract(job, request))
		{
			Reject(plan, requestIndex, NRIVoxelComputeBatchRejectReason::IncompatibleDuplicate);
			continue;
		}

		plan.stats.dedupeHits++;
		job.bindings.push_back({ requestIndex, request.requestKey, request.materialBindingKey });
	}

	std::sort(plan.jobs.begin(), plan.jobs.end(), [&requests](const NRIVoxelComputeBatchPlannedJob& a, const NRIVoxelComputeBatchPlannedJob& b)
	{
		const NRIVoxelComputeBatchPlanRequest& aOwner = requests[a.ownerRequestIndex];
		const NRIVoxelComputeBatchPlanRequest& bOwner = requests[b.ownerRequestIndex];
		if (RequestRanksBefore(aOwner, a.ownerRequestIndex, bOwner, b.ownerRequestIndex))
		{
			return true;
		}
		if (RequestRanksBefore(bOwner, b.ownerRequestIndex, aOwner, a.ownerRequestIndex))
		{
			return false;
		}
		if (a.levelGeneration != b.levelGeneration)
		{
			return a.levelGeneration < b.levelGeneration;
		}
		return a.meshKey < b.meshKey;
	});

	for (uint32_t jobIndex = 0; jobIndex < plan.jobs.size(); ++jobIndex)
	{
		NRIVoxelComputeBatchPlannedJob& job = plan.jobs[jobIndex];
		job.oversizedExclusive = settings.maxBytesPerBatch != 0 && job.reservationBytes > settings.maxBytesPerBatch;
		if (job.oversizedExclusive)
		{
			NRIVoxelComputeBatchPlanBatch batch = {};
			batch.jobIndices.push_back(jobIndex);
			batch.reservationBytes = job.reservationBytes;
			batch.oversizedExclusive = true;
			plan.batches.push_back(std::move(batch));
			plan.stats.oversizedExclusiveBatches++;
			continue;
		}

		NRIVoxelComputeBatchPlanBatch* batch = plan.batches.empty() ? nullptr : &plan.batches.back();
		const bool needsBatch =
			batch == nullptr || batch->oversizedExclusive ||
			batch->jobIndices.size() >= settings.maxJobsPerBatch ||
			AddWouldOverflow(batch->reservationBytes, job.reservationBytes) ||
			(settings.maxBytesPerBatch != 0 && batch->reservationBytes + job.reservationBytes > settings.maxBytesPerBatch);
		if (needsBatch)
		{
			plan.batches.push_back({});
			batch = &plan.batches.back();
		}
		batch->jobIndices.push_back(jobIndex);
		batch->reservationBytes += job.reservationBytes;
	}

	plan.stats.uniqueJobs = (uint32_t)plan.jobs.size();
	for (const NRIVoxelComputeBatchPlannedJob& job : plan.jobs)
	{
		plan.stats.materialBindings += (uint32_t)job.bindings.size();
	}
	return plan;
}
