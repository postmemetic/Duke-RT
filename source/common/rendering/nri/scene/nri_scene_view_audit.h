#pragma once

#include "nri_scene_bridge.h"
#include "perf_audit.h"

namespace nri_scene
{
// These are observations of owned vector payload, not allocator events. A
// nonzero capacity destroyed is a release proxy; it does not identify heap
// allocation sizes, allocator bookkeeping, or allocations made while growing.
struct SceneViewStorageAudit
{
	uint64_t surfaces = 0;
	uint64_t liveBytes = 0;
	uint64_t vertexCapacityBytes = 0, indexCapacityBytes = 0, slotCapacityBytes = 0;
	uint64_t vertexCapacities = 0, indexCapacities = 0, slotCapacities = 0;
	uint64_t outerCapacityBytes = 0;

	void Add(const std::vector<SurfaceRef>& values)
	{
		surfaces += values.size();
		outerCapacityBytes += uint64_t(values.capacity()) * sizeof(SurfaceRef);
		for (const SurfaceRef& surface : values)
		{
			liveBytes += uint64_t(surface.vertices.size()) * sizeof(CapturedVertex) +
				uint64_t(surface.indices.size()) * sizeof(uint32_t) + surface.primitiveLocalMaterialSlots.size();
			vertexCapacityBytes += uint64_t(surface.vertices.capacity()) * sizeof(CapturedVertex);
			indexCapacityBytes += uint64_t(surface.indices.capacity()) * sizeof(uint32_t);
			slotCapacityBytes += surface.primitiveLocalMaterialSlots.capacity();
			vertexCapacities += surface.vertices.capacity() != 0;
			indexCapacities += surface.indices.capacity() != 0;
			slotCapacities += surface.primitiveLocalMaterialSlots.capacity() != 0;
		}
	}
	static SceneViewStorageAudit Read(const SceneView& view)
	{
		SceneViewStorageAudit result;
		result.Add(view.opaqueWalls);
		result.Add(view.opaqueFlats);
		result.Add(view.opaqueSprites);
		return result;
	}
};

// Construct before clearing and destroy after all old SurfaceRefs are gone.
// The clear timer excludes this owner's storage census and counter emission.
class SceneViewClearAudit
{
public:
	explicit SceneViewClearAudit(const SceneView& view)
	{
		if (!PerfAuditActive()) return;
		mCensus = PerfAuditCensusRequested();
		if (mCensus)
		{
			const uint64_t start = PerfAuditClockNs();
			mStorage = SceneViewStorageAudit::Read(view);
			PerfAuditAdd(PerfAuditMetric::SceneCaptureStorageCensusNs, PerfAuditClockNs() - start);
		}
		mStart = PerfAuditClockNs();
	}
	~SceneViewClearAudit()
	{
		if (mStart == 0) return;
		const uint64_t elapsed = PerfAuditClockNs() - mStart;
		PerfAuditAdd(PerfAuditMetric::SceneViewClearNs, elapsed);
		PerfAuditAdd(PerfAuditMetric::SceneViewClearCalls);
		if (!mCensus) return;
		PerfAuditAdd(PerfAuditMetric::SceneViewClearCensusCalls);
		PerfAuditAdd(PerfAuditMetric::SceneViewClearCensusSurfaces, mStorage.surfaces);
		PerfAuditAdd(PerfAuditMetric::SceneViewClearCensusNestedLiveBytes, mStorage.liveBytes);
		PerfAuditAdd(PerfAuditMetric::SceneViewClearCensusVertexCapacityDestroys, mStorage.vertexCapacities);
		PerfAuditAdd(PerfAuditMetric::SceneViewClearCensusIndexCapacityDestroys, mStorage.indexCapacities);
		PerfAuditAdd(PerfAuditMetric::SceneViewClearCensusSlotCapacityDestroys, mStorage.slotCapacities);
		PerfAuditAdd(PerfAuditMetric::SceneViewClearCensusVertexCapacityBytes, mStorage.vertexCapacityBytes);
		PerfAuditAdd(PerfAuditMetric::SceneViewClearCensusIndexCapacityBytes, mStorage.indexCapacityBytes);
		PerfAuditAdd(PerfAuditMetric::SceneViewClearCensusSlotCapacityBytes, mStorage.slotCapacityBytes);
		PerfAuditAdd(PerfAuditMetric::SceneViewClearCensusOuterRetainedBytes, mStorage.outerCapacityBytes);
	}
private:
	uint64_t mStart = 0;
	bool mCensus = false;
	SceneViewStorageAudit mStorage;
};

// Capture timers are inclusive, including nested capture/clear calls. Detailed
// output storage is sampled only on census frames and can count a child again
// after composition into a parent. It is work attribution, not a heap census.
class SceneViewCaptureAudit
{
public:
	SceneViewCaptureAudit(const SceneView& view, PerfAuditMetric timer, PerfAuditMetric calls)
		: mView(view), mTimer(timer), mCalls(calls)
	{
		if (!PerfAuditActive()) return;
		mCensus = PerfAuditCensusRequested();
		if (mCensus)
		{
			mOuterBefore[0] = view.opaqueWalls.capacity();
			mOuterBefore[1] = view.opaqueFlats.capacity();
			mOuterBefore[2] = view.opaqueSprites.capacity();
		}
		mStart = PerfAuditClockNs();
	}
	~SceneViewCaptureAudit()
	{
		if (mStart == 0) return;
		const uint64_t elapsed = PerfAuditClockNs() - mStart;
		PerfAuditAdd(mTimer, elapsed);
		PerfAuditAdd(mCalls);
		if (!mCensus) return;
		const uint64_t start = PerfAuditClockNs();
		const auto storage = SceneViewStorageAudit::Read(mView);
		const size_t outerAfter[3] = { mView.opaqueWalls.capacity(), mView.opaqueFlats.capacity(), mView.opaqueSprites.capacity() };
		uint64_t growths = 0, growthBytes = 0;
		for (size_t index = 0; index < 3; ++index)
		{
			if (outerAfter[index] <= mOuterBefore[index]) continue;
			++growths;
			growthBytes += uint64_t(outerAfter[index] - mOuterBefore[index]) * sizeof(SurfaceRef);
		}
		PerfAuditAdd(PerfAuditMetric::SceneCaptureStorageCensusNs, PerfAuditClockNs() - start);
		PerfAuditAdd(PerfAuditMetric::SceneCaptureOutputCensusCalls);
		PerfAuditAdd(PerfAuditMetric::SceneCaptureOutputCensusSurfaces, storage.surfaces);
		PerfAuditAdd(PerfAuditMetric::SceneCaptureOutputCensusNestedLiveBytes, storage.liveBytes);
		PerfAuditAdd(PerfAuditMetric::SceneCaptureOutputCensusNestedCapacityBytes,
			storage.vertexCapacityBytes + storage.indexCapacityBytes + storage.slotCapacityBytes);
		PerfAuditAdd(PerfAuditMetric::SceneCaptureOutputCensusNestedCapacities,
			storage.vertexCapacities + storage.indexCapacities + storage.slotCapacities);
		PerfAuditAdd(PerfAuditMetric::SceneCaptureOutputCensusOuterCapacityBytes, storage.outerCapacityBytes);
		PerfAuditAdd(PerfAuditMetric::SceneCaptureOutputCensusOuterGrowths, growths);
		PerfAuditAdd(PerfAuditMetric::SceneCaptureOutputCensusOuterGrowthBytes, growthBytes);
	}
private:
	const SceneView& mView;
	PerfAuditMetric mTimer, mCalls;
	uint64_t mStart = 0;
	size_t mOuterBefore[3] = {};
	bool mCensus = false;
};
}
