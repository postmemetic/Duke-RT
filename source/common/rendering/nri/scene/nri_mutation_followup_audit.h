#pragma once

#include "perf_audit.h"
#include <array>
#include <cstddef>

// Defined by the audit owner. The detail gate permits same-binary observer-cost
// controls without enabling these probes in ordinary production timing.
bool PerfAuditCpuDetailActive();

namespace nri_scene::mutation_followup_audit
{
#define NRI_MUTATION_FOLLOWUP_COUNTERS(X) \
	X(FluidSignatureCalls) X(FluidSignatureNs) X(FluidSignatureEligible) \
	X(FluidSignatureResolveFloorCalls) X(FluidSignatureSlopeCalls) \
	X(FluidSignatureCornerCalls) X(FluidSignatureFanEdges) X(FluidSignatureFanSectors) X(FluidSignatureFanOverflows) \
	X(MutationNormalizedChanged) X(MutationNormalizedUnchanged) X(MutationNormalizedDirtyNoiseRemoved) X(MutationNormalizedSettleRequests) \
	X(MutationMaterialCacheHits) X(MutationMaterialCacheMisses) X(MutationMaterialCacheStores) \
	X(MutationMaterialCacheReplacements) X(MutationMaterialCacheEvictions) X(MutationMaterialCacheEntriesCleared) \
	X(MutationMaterialCacheClearInvalidAnalysis) X(MutationMaterialCacheClearUnchanged) X(MutationMaterialCacheClearInvalidReplacement) \
	X(MutationMaterialCacheClearResidentNoop) X(MutationMaterialCacheClearResidentApply) X(MutationMaterialCacheClearStartup) \
	X(MutationAtlasClones) X(MutationAtlasCloneBytes) X(MutationAtlasCloneNs) X(MutationAtlasCloneBothRangesRetained) \
	X(MutationResidentBlasRecreates) X(MutationResidentBlasUpdates) X(MutationResidentBlasBuildCommands) \
	X(MutationResidentBlasScratchQueries) X(MutationResidentBlasScratchGrowths) X(MutationResidentBlasBarrierCalls) \
	X(MutationResidentBlasBarrierBuffers) X(MutationResidentGeometryUploadRanges) \
	X(ResidentBlasBatchCalls) X(ResidentBlasBatchJobs) X(ResidentBlasBatchBatches) X(ResidentBlasBatchBatchedJobs) \
	X(ResidentBlasBatchSerialJobs) X(ResidentBlasBatchFallbackJobs) X(ResidentBlasBatchCountFallbacks) \
	X(ResidentBlasBatchBudgetFallbacks) X(ResidentBlasBatchAliasFallbacks) X(ResidentBlasBatchInvalidFallbacks) \
	X(ResidentBlasBatchAllocationFailures) X(ResidentBlasBatchScratchGrowths) X(ResidentBlasBatchBuildCalls) \
	X(ResidentBlasBatchScratchBarrierCalls) X(ResidentBlasBatchPlanNs) X(ResidentBlasBatchRecordNs) \
	X(ResidentBlasBatchScratchAllocationNs) X(ResidentBlasBatchScratchRetirements) \
	X(ResidentBlasBatchScratchRetiredBytes) X(ResidentBlasBatchNoRetirementOwnerFallbacks)

enum class Counter : size_t
{
#define NRI_COUNTER_ENUM(name) name,
	NRI_MUTATION_FOLLOWUP_COUNTERS(NRI_COUNTER_ENUM)
#undef NRI_COUNTER_ENUM
	Count
};
inline constexpr PerfAuditMetric Metrics[] = {
#define NRI_COUNTER_METRIC(name) PerfAuditMetric::name,
	NRI_MUTATION_FOLLOWUP_COUNTERS(NRI_COUNTER_METRIC)
#undef NRI_COUNTER_METRIC
};
#undef NRI_MUTATION_FOLLOWUP_COUNTERS

struct Counts { std::array<uint64_t, (size_t)Counter::Count> values; };
inline thread_local Counts* current = nullptr;
inline void Add(Counter counter, uint64_t value = 1)
{
	if (current != nullptr) current->values[(size_t)counter] += value;
}

// Nested helpers share one batch. There are no per-wall atomic operations.
class Batch
{
	Counts mCounts;
	bool mOwns = false;
public:
	Batch()
	{
		if (current == nullptr && PerfAuditCpuDetailActive()) { mCounts.values.fill(0); current = &mCounts; mOwns = true; }
	}
	~Batch()
	{
		if (!mOwns) return;
		current = nullptr;
		for (size_t i = 0; i < mCounts.values.size(); ++i)
			if (mCounts.values[i] != 0) PerfAuditAdd(Metrics[i], mCounts.values[i]);
	}
	Batch(const Batch&) = delete;
	Batch& operator=(const Batch&) = delete;
};

class Timer
{
	Counter mCounter;
	uint64_t mStart;
public:
	explicit Timer(Counter counter) : mCounter(counter), mStart(current != nullptr ? PerfAuditClockNs() : 0) {}
	~Timer() { if (mStart != 0) Add(mCounter, PerfAuditClockNs() - mStart); }
};
}
