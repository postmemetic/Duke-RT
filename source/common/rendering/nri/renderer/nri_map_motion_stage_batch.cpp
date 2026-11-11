#include "nri_map_motion_stage_batch.h"
#include "nri_map_motion_reference_history.h"
#include "perf_audit.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>

struct NRIMapMotionStageBatch::WorkerPool
{
	explicit WorkerPool(NRIMapMotionStageBatch& owner) : batch(owner) {}
	~WorkerPool()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		start.notify_all();
		for (auto& thread : threads) if (thread.joinable()) thread.join();
	}
	void EnsureThreads(unsigned requested)
	{
		for (; started < requested; ++started)
			threads[started] = std::thread(&WorkerPool::Run, this, started);
	}
	void Run(unsigned index)
	{
		uint64_t observed = 0;
		std::unique_lock<std::mutex> lock(mutex);
		for (;;)
		{
			start.wait(lock, [&]() { return stopping || generation != observed; });
			if (stopping) return;
			observed = generation;
			if (index >= active) continue;
			const auto* inputHistory = history;
			const size_t first = batch.mResults.size() * index / (active + 1);
			const size_t end = batch.mResults.size() * (index + 1) / (active + 1);
			const bool timed = collectTiming;
			const uint64_t published = publishedNs;
			lock.unlock();
			const uint64_t begin = timed ? PerfAuditClockNs() : 0;
			batch.EvaluateRange(*inputHistory, first, end);
			const uint64_t elapsed = timed ? PerfAuditClockNs() - begin : 0;
			lock.lock();
			workerNs[index] = elapsed;
			queueNs[index] = timed ? begin - published : 0;
			if (--pending == 0) done.notify_one();
		}
	}
	NRIMapMotionStageBatch& batch;
	std::array<std::thread, 2> threads;
	std::mutex mutex;
	std::condition_variable start, done;
	const NRIMapMotionHistory* history = nullptr;
	std::array<uint64_t, 2> workerNs = {};
	std::array<uint64_t, 2> queueNs = {};
	uint64_t publishedNs = 0;
	uint64_t generation = 0;
	unsigned started = 0, active = 0, pending = 0;
	bool stopping = false, collectTiming = false;
};

NRIMapMotionStageBatch::NRIMapMotionStageBatch()
{
	// Reported logical concurrency is a ceiling, not a reason to consume it all.
	// Reserve an owner participant; an unknown or single-thread host stays serial.
	const unsigned available = std::thread::hardware_concurrency();
	mWorkerLimit = available > 1 ? std::min(2u, available - 1) : 0;
}
NRIMapMotionStageBatch::~NRIMapMotionStageBatch() = default;

size_t NRIMapMotionStageBatch::GrowthCapacity(size_t current, size_t required)
{
	if (current >= MaxSurfaces) return MaxSurfaces;
	if (required <= current) return current;
	return std::min(MaxSurfaces, std::max(required, current + current / 2));
}

bool NRIMapMotionStageBatch::Gather(const NRIMapMotionHistory& history,
	const std::vector<const nri_scene::SceneView*>& views,
	const std::vector<NRIMapMotionViewCertificate>* certificates)
{
	ReleaseInputs();
	mTimings = {};
	if (!history.m_stageOpen) return false;
	size_t count = 0;
	for (const auto* view : views)
	{
		if (view == nullptr || view->opaqueWalls.size() > MaxSurfaces - count) return false;
		count += view->opaqueWalls.size();
		if (view->opaqueFlats.size() > MaxSurfaces - count) return false;
		count += view->opaqueFlats.size();
	}
	if (count > mResults.capacity())
	{
		const size_t next = GrowthCapacity(mResults.capacity(), count);
		mResults.reserve(next); // cap requested growth instead of implicit resize growth
		PerfAuditAdd(PerfAuditMetric::MapMotionBatchResultReserveAllocations);
		// New backing-store capacity allocated, not net growth or current memory.
		PerfAuditAdd(PerfAuditMetric::MapMotionBatchResultReserveBytes, mResults.capacity() * sizeof(Result));
	}
	mResults.resize(count);
	size_t index = 0;
	for (size_t viewIndex = 0; viewIndex < views.size(); ++viewIndex)
	{
		const auto* view = views[viewIndex];
		const auto* certificate = certificates && certificates->size() == views.size() &&
			(*certificates)[viewIndex].Valid() ? &(*certificates)[viewIndex] : nullptr;
		uint32_t ordinal = 0;
		const auto append = [&](const auto& surfaces)
		{
			for (const auto& surface : surfaces)
			{
				auto& result = mResults[index++];
				result = {};
				result.surface = &surface;
				result.certificate = certificate;
				result.ordinal = ordinal++;
			}
		};
		append(view->opaqueWalls);
		append(view->opaqueFlats);
	}
	mMapEpoch = history.m_mapEpoch;
	mStageSerial = history.m_stageSerial;
	mLeaseGeneration = history.m_leaseGeneration;
	mLeaseActive = true;
	mValidateCertificates = history.m_reference != nullptr;
	return true;
}

bool NRIMapMotionStageBatch::LeaseMatches(const NRIMapMotionHistory& history) const
{
	return mLeaseActive && history.m_stageOpen && history.m_mapEpoch == mMapEpoch &&
		history.m_stageSerial == mStageSerial && history.m_leaseGeneration == mLeaseGeneration;
}

void NRIMapMotionStageBatch::EvaluateRange(const NRIMapMotionHistory& history, size_t first, size_t end)
{
	// Every access to history is const and all output indices are disjoint. The
	// caller cannot insert, erase or stage any history until all ranges finish.
	for (size_t index = first; index < end; ++index)
	{
		auto& result = mResults[index];
		const auto& surface = *result.surface;
		if (!surface.temporal.identityValid) continue;
		const auto found = history.m_records.find(surface.temporal.occurrenceId);
		if (found == history.m_records.end()) continue;
		const auto& record = found->second;
		result.record = &record;
		const int candidates[] = { record.stagedSlot, record.committedSlot, 0, 1 };
		unsigned tested = 0;
		for (int slot : candidates)
		{
			if (slot < 0 || (tested & (1u << slot))) continue;
			tested |= 1u << slot;
			const auto& snapshot = record.snapshots[slot];
			if (result.certificate && snapshot.valid)
			{
				++result.certificateChecks;
				if (snapshot.certificate.ordinal == result.ordinal &&
					snapshot.certificate.view.Equals(*result.certificate))
				{
					result.certificateHit = true;
					result.matchedSlot = slot;
					result.hasMotion = snapshot.certificate.hasMotion;
					// Validation deliberately reads every original input, including
					// motion-only previous positions and UV reactivity. No worker
					// mutates a snapshot or emits shared audit counters.
					if (mValidateCertificates)
					{
						const bool matches = snapshot.Matches(surface, result.validationComparedBytes);
						const bool hasMotion = NRIMapMotionHistory::SurfaceHasMotion(surface);
						result.certificateMismatch = !matches || hasMotion != result.hasMotion;
					}
					break;
				}
			}
			if (snapshot.Matches(surface, result.comparedBytes))
			{
				result.matchedSlot = slot;
				result.hasMotion = NRIMapMotionHistory::SurfaceHasMotion(surface);
				break;
			}
		}
	}
}

bool NRIMapMotionStageBatch::Evaluate(const NRIMapMotionHistory& history, int workers, bool collectTiming)
{
	if (!LeaseMatches(history)) { ReleaseInputs(); return false; }
	unsigned count = std::min(mWorkerLimit, (unsigned)std::clamp(workers, 0, 2));
	if (mResults.size() < MinWorkerSurfaces) count = 0;
	const uint64_t dispatchStart = collectTiming ? PerfAuditClockNs() : 0;
	if (count != 0 && !mWorkerInitializationFailed)
	{
		try
		{
			if (!mPool) mPool = std::make_unique<WorkerPool>(*this);
			mPool->EnsureThreads(count);
		}
		catch (...)
		{
			// A partial launch is joined before releasing its state. Keep the
			// complete serial evaluation available until this owner is reset.
			mPool.reset();
			mWorkerInitializationFailed = true;
			mTimings.initializationFailed = true;
		}
	}
	if (mWorkerInitializationFailed) count = 0;
	mTimings.workers = count;
	if (count != 0)
	{
		{
			std::lock_guard<std::mutex> lock(mPool->mutex);
			mPool->history = &history;
			mPool->active = mPool->pending = count;
			mPool->collectTiming = collectTiming;
			mPool->workerNs = {};
			mPool->queueNs = {};
			mPool->publishedNs = collectTiming ? PerfAuditClockNs() : 0;
			++mPool->generation;
		}
		mPool->start.notify_all();
	}
	if (collectTiming) mTimings.dispatchNs = PerfAuditClockNs() - dispatchStart;
	const uint64_t mainStart = collectTiming ? PerfAuditClockNs() : 0;
	EvaluateRange(history, mResults.size() * count / (count + 1), mResults.size());
	if (collectTiming) mTimings.mainNs = PerfAuditClockNs() - mainStart;
	if (count != 0)
	{
		const uint64_t joinStart = collectTiming ? PerfAuditClockNs() : 0;
		std::unique_lock<std::mutex> lock(mPool->mutex);
		mPool->done.wait(lock, [&]() { return mPool->pending == 0; });
		if (collectTiming) mTimings.joinNs = PerfAuditClockNs() - joinStart;
		for (unsigned index = 0; index < count; ++index)
		{
			mTimings.workerSumNs += mPool->workerNs[index];
			mTimings.workerMaxNs = std::max(mTimings.workerMaxNs, mPool->workerNs[index]);
			mTimings.queueSumNs += mPool->queueNs[index];
			mTimings.queueMaxNs = std::max(mTimings.queueMaxNs, mPool->queueNs[index]);
		}
		mPool->history = nullptr;
		mPool->active = 0;
	}
	if (!LeaseMatches(history)) { ReleaseInputs(); return false; }
	return true;
}

void NRIMapMotionStageBatch::ReleaseInputs()
{
	// Results own no engine objects. Destroy their active spans after the join;
	// only bounded vector capacity and idle worker threads survive the call.
	mResults.clear();
	mLeaseActive = false;
}

size_t NRIMapMotionStageBatch::CapacityBytes() const
{
	// Does not include allocator headers, platform thread stacks or OS objects.
	return mResults.capacity() * sizeof(Result) + (mPool ? sizeof(WorkerPool) : 0);
}

bool NRIMapMotionHistory::StagePublishedBatch(const std::vector<const nri_scene::SceneView*>& views, int workers,
	const std::vector<NRIMapMotionViewCertificate>* certificates)
{
	PerfAuditScope batchScope(PerfAuditMetric::MapMotionBatchNs);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchCalls);
	EnsureReference();
	// The old census micro-clocks are specific to the immediate serial path.
	// Both new scheduling modes use the same aggregate owner instrumentation.
	DisableStageCensus();
	if (m_quarantined)
	{
		bool valid = m_reference->m_stageOpen;
		for (const auto* view : views) valid &= view && m_reference->StagePublishedView(*view);
		return valid;
	}
	if (!m_stageOpen) return false;
	if (!m_stageBatch) m_stageBatch = std::make_unique<NRIMapMotionStageBatch>();
	auto& batch = *m_stageBatch;
	PerfAuditSet(PerfAuditMetric::MapMotionBatchWorkers, 0);
	PerfAuditSet(PerfAuditMetric::MapMotionBatchWorkerLimit, batch.mWorkerLimit);
	PerfAuditSet(PerfAuditMetric::MapMotionBatchResidentWorkers, batch.mPool ? batch.mPool->started : 0);
	bool gathered;
	{
		PerfAuditScope scope(PerfAuditMetric::MapMotionBatchGatherNs);
		gathered = batch.Gather(*this, views, certificates);
	}
	if (!gathered)
	{
		PerfAuditAdd(PerfAuditMetric::MapMotionBatchCapacityFallbacks);
		bool valid = true;
		for (const auto* view : views) valid &= view && StagePublishedView(*view);
		return valid;
	}
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchSurfaces, batch.mResults.size());
	bool evaluated;
	{
		PerfAuditScope scope(PerfAuditMetric::MapMotionBatchEvaluateNs);
		evaluated = batch.Evaluate(*this, workers, PerfAuditActive());
	}
	if (!evaluated)
	{
		PerfAuditAdd(PerfAuditMetric::MapMotionBatchLeaseRejects);
		return false;
	}
	const auto& times = batch.mTimings;
	PerfAuditAdd(times.workers ? PerfAuditMetric::MapMotionBatchWorkerCalls : PerfAuditMetric::MapMotionBatchSerialCalls);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchWorkerTasks, times.workers);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchInitializationFailures, times.initializationFailed ? 1 : 0);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchDispatchNs, times.dispatchNs);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchMainEvaluateNs, times.mainNs);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchJoinNs, times.joinNs);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchWorkerWorkNs, times.workerSumNs);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchWorkerMaxNs, times.workerMaxNs);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchWorkerQueueNs, times.queueSumNs);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchWorkerQueueMaxNs, times.queueMaxNs);
	PerfAuditSet(PerfAuditMetric::MapMotionBatchCapacityBytes, batch.CapacityBytes());
	PerfAuditSet(PerfAuditMetric::MapMotionBatchWorkers, times.workers);
	PerfAuditSet(PerfAuditMetric::MapMotionBatchResidentWorkers, batch.mPool ? batch.mPool->started : 0);
	uint64_t compared = 0, fast = 0, fallback = 0, duplicates = 0;
	uint64_t certificateEligible = 0, certificateChecks = 0, certificateHits = 0;
	uint64_t certificateSavedBytes = 0, certificateValidations = 0, certificateValidationBytes = 0;
	bool valid = true;
	size_t index = 0;
	{
		PerfAuditScope mergeScope(PerfAuditMetric::MapMotionBatchMergeNs);
		for (const auto* view : views)
		{
			if (m_quarantined)
			{
				valid &= m_reference->StagePublishedView(*view);
				index += view->opaqueWalls.size() + view->opaqueFlats.size();
				continue;
			}
			bool referenceValid = false;
			if (m_reference)
			{
				PerfAuditScope referenceScope(PerfAuditMetric::MapMotionReferenceNs);
				referenceValid = m_reference->StagePublishedView(*view);
			}
			bool viewValid = true;
			const size_t end = index + view->opaqueWalls.size() + view->opaqueFlats.size();
			for (; index < end; ++index)
			{
				const auto& result = batch.mResults[index];
				const auto& surface = *result.surface;
				compared += result.comparedBytes;
				// Fold counters into the existing ordered pass; do not add a
				// second full result scan merely to observe cheap certificates.
				certificateEligible += result.certificate && surface.temporal.identityValid ? 1 : 0;
				certificateChecks += result.certificateChecks;
				certificateValidations += result.certificateHit && batch.mValidateCertificates ? 1 : 0;
				certificateValidationBytes += result.validationComparedBytes;
				if (result.certificateMismatch)
				{
					PerfAuditAdd(PerfAuditMetric::MapMotionCertificateValidationMismatches);
					PerfAuditAdd(PerfAuditMetric::MapMotionCertificateQuarantines);
					Quarantine("publication-certificate");
					viewValid = referenceValid;
					index = end;
					break;
				}
				if (!surface.temporal.identityValid) continue;
				auto* record = const_cast<Record*>(result.record);
				// An earlier publication may have replaced the spare snapshot.
				// Re-evaluate duplicates in original order using the full serial path.
				if (record == nullptr || result.matchedSlot < 0 || record->stagedSlot >= 0)
				{
					++fallback;
					duplicates += record && record->stagedSlot >= 0 ? 1 : 0;
					viewValid &= StageSurface(surface);
					continue;
				}
				record->stagedSlot = result.matchedSlot;
				record->stagedAge = record->committedSlot >= 0 ? record->historyAge + 1u : 1u;
				record->stagedValid = true;
				record->publishedWithMotion = result.hasMotion;
				if (result.certificate)
				{
					auto& snapshot = record->snapshots[result.matchedSlot];
					// Owner-only publication after duplicate checks: workers may
					// have borrowed the same record for several occurrences.
					if (!result.certificateHit)
						snapshot.certificate = { *result.certificate, result.ordinal, result.hasMotion };
					if (result.certificateHit)
					{
						++certificateHits;
						certificateSavedBytes += snapshot.sourceCorners.size() *
							(sizeof(nri_scene::PTMapTemporalCorner::key) + sizeof(nri_scene::PTMapTemporalCorner::position));
					}
				}
				m_staged.push_back(record);
				++fast;
			}
			m_stats.stagedSurfaceCount = (uint32_t)m_staged.size();
			if (m_reference)
			{
				PerfAuditAdd(PerfAuditMetric::MapMotionValidationChecks);
				if (viewValid != referenceValid)
				{
					PerfAuditAdd(PerfAuditMetric::MapMotionBatchValidationMismatches);
					Quarantine("batch-stage-result");
					viewValid = referenceValid;
				}
			}
			valid &= viewValid;
		}
	}
	PerfAuditAdd(PerfAuditMetric::MapMotionSnapshotComparedBytes, compared);
	PerfAuditAdd(PerfAuditMetric::MapMotionPayloadReuses, fast);
	PerfAuditAdd(PerfAuditMetric::MapMotionOccurrenceReuses, fast);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchComparedBytes, compared);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchFastSurfaces, fast);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchFallbackSurfaces, fallback);
	PerfAuditAdd(PerfAuditMetric::MapMotionBatchDuplicateFallbacks, duplicates);
	PerfAuditAdd(PerfAuditMetric::MapMotionCertificateEligibleSurfaces, certificateEligible);
	PerfAuditAdd(PerfAuditMetric::MapMotionCertificateChecks, certificateChecks);
	PerfAuditAdd(PerfAuditMetric::MapMotionCertificateHits, certificateHits);
	PerfAuditAdd(PerfAuditMetric::MapMotionCertificateFallbackSurfaces, certificateEligible - certificateHits);
	PerfAuditAdd(PerfAuditMetric::MapMotionCertificateSavedCornerBytes, certificateSavedBytes);
	PerfAuditAdd(PerfAuditMetric::MapMotionCertificateValidationChecks, certificateValidations);
	PerfAuditAdd(PerfAuditMetric::MapMotionCertificateValidationComparedBytes, certificateValidationBytes);
	if (m_reference && !m_quarantined)
	{
		PerfAuditAdd(PerfAuditMetric::MapMotionBatchValidationChecks);
		if (times.workers) PerfAuditAdd(PerfAuditMetric::MapMotionBatchWorkerValidationChecks);
		// Full independent original payload/provenance/history comparison, not
		// merely the per-view boolean or the optimized evaluator repeated.
		if (!CheckReference("batch")) PerfAuditAdd(PerfAuditMetric::MapMotionBatchValidationMismatches);
	}
	batch.ReleaseInputs();
	return valid;
}
