#pragma once

#include "nri_map_motion_history.h"

#include <memory>
#include <vector>

// One synchronous motion-history evaluation. Inputs remain owned and frozen by
// the renderer until Evaluate returns; only the owner may merge/commit history.
// There is no cross-frame queue, engine callback, or GPU work on these workers.
class NRIMapMotionStageBatch
{
public:
	static constexpr size_t MaxSurfaces = 65536;
	static constexpr size_t MinWorkerSurfaces = 1024;
	static size_t GrowthCapacity(size_t current, size_t required);
	NRIMapMotionStageBatch();
	~NRIMapMotionStageBatch();
	NRIMapMotionStageBatch(const NRIMapMotionStageBatch&) = delete;
	NRIMapMotionStageBatch& operator=(const NRIMapMotionStageBatch&) = delete;

private:
	friend class NRIMapMotionHistory;
	struct Result
	{
		const nri_scene::SurfaceRef* surface = nullptr;
		const NRIMapMotionHistory::Record* record = nullptr;
		const NRIMapMotionViewCertificate* certificate = nullptr;
		uint64_t comparedBytes = 0;
		uint64_t validationComparedBytes = 0;
		uint32_t ordinal = 0;
		uint32_t certificateChecks = 0;
		int matchedSlot = -1;
		bool hasMotion = false;
		bool certificateHit = false;
		bool certificateMismatch = false;
	};
	struct Timings
	{
		uint64_t dispatchNs = 0, mainNs = 0, joinNs = 0;
		uint64_t workerSumNs = 0, workerMaxNs = 0;
		uint64_t queueSumNs = 0, queueMaxNs = 0;
		uint32_t workers = 0;
		bool initializationFailed = false;
	};
	struct WorkerPool;
	bool Gather(const NRIMapMotionHistory& history, const std::vector<const nri_scene::SceneView*>& views,
		const std::vector<NRIMapMotionViewCertificate>* certificates = nullptr);
	bool Evaluate(const NRIMapMotionHistory& history, int workers, bool collectTiming);
	bool LeaseMatches(const NRIMapMotionHistory& history) const;
	void EvaluateRange(const NRIMapMotionHistory& history, size_t first, size_t end);
	void ReleaseInputs();
	size_t CapacityBytes() const;
	std::vector<Result> mResults;
	std::unique_ptr<WorkerPool> mPool;
	Timings mTimings;
	uint64_t mMapEpoch = 0, mStageSerial = 0, mLeaseGeneration = 0;
	unsigned mWorkerLimit = 0;
	bool mLeaseActive = false;
	bool mWorkerInitializationFailed = false;
	bool mValidateCertificates = false;
};
