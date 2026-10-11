#pragma once

#include "nri_runtime_mutation_worklist.h"

#include <cstdint>
#include <set>
#include <vector>

class NRIRuntimeMutationSystem;
namespace nri_scene { struct PTMapWorld; }

// Current map state only: entries are removed immediately when their predicate
// clears. No resource history or runtime/static publication authority lives here.
class NRIRuntimeMutationDiscovery
{
public:
	struct Stats
	{
		uint32_t discoveryInputCount = 0;
		uint32_t dispatchInputCount = 0;
		uint32_t sourcePolls = 0;
		uint32_t dedupeHits = 0;
		uint32_t reconciledChunks = 0;
		uint32_t activeCount = 0;
		uint32_t materialDeferredCount = 0;
		uint32_t structuralDeferredCount = 0;
		uint32_t watchCount = 0;
		uint32_t inputHighWater = 0;
		uint32_t activeHighWater = 0;
		uint32_t deferredHighWater = 0;
		uint32_t shadowMisses = 0;
		uint32_t stateMismatches = 0;
		uint32_t budgetMismatches = 0;
		uint32_t budgetCandidates = 0;
		uint32_t budgetSelected = 0;
		uint32_t budgetValidationSorted = 0;
		uint32_t accountingVisits = 0;
		uint64_t maxDeferredAge = 0;
		double accountingMs = 0.0;
		double dispatchPrepareMs = 0.0;
	};

	struct BudgetCandidate
	{
		uint64_t deferredFrame = 0;
		uint32_t chunkListIndex = 0;
	};
	struct BudgetScratch
	{
		NRIRuntimeMutationWorklist allowed;
		std::vector<BudgetCandidate> nearStructural;
		std::vector<BudgetCandidate> farStructural;
		std::vector<BudgetCandidate> nearMaterial;
	};

	void Reset();
	void NoteTouchedChunk(uint32_t chunkIndex);
	const std::vector<uint32_t>& GatherInputs(
		const nri_scene::PTMapWorld& world,
		const NRIRuntimeMutationSystem& mutation,
		const std::vector<uint32_t>& visibleWords,
		const std::set<uint32_t>& settleChunks);
	void RefreshTouched(const NRIRuntimeMutationSystem& mutation);
	void ValidateCurrentState(const NRIRuntimeMutationSystem& mutation, bool repair);
	const std::vector<uint32_t>& BuildDispatchInputs(
		const std::vector<uint32_t>& candidates, bool fullScan);
	const std::vector<uint32_t>& MaterialDeferredChunks() const { return m_materialDeferred.entries; }
	const std::vector<uint32_t>& StructuralDeferredChunks() const { return m_structuralDeferred.entries; }
	BudgetScratch& BeginBudget(uint32_t chunkCount, bool validate);
	void SelectOldest(std::vector<BudgetCandidate>& candidates, uint32_t budget);
	Stats& GetStats() { return m_stats; }
	const Stats& GetStats() const { return m_stats; }

private:
	struct CurrentSet
	{
		std::vector<uint32_t> entries;
		std::vector<uint32_t> positions;
		void Reset(uint32_t count);
		void Set(uint32_t index, bool present);
		void Clear();
	};

	void AddInput(uint32_t chunkListIndex);
	void UpdateSectionMembership(uint32_t chunkListIndex, const std::vector<int32_t>& sectionIndices);

	bool m_initialized = false;
	uint64_t m_buildSerial = 0;
	uint32_t m_chunkCount = 0;
	uint32_t m_inputHighWater = 0;
	uint32_t m_activeHighWater = 0;
	uint32_t m_deferredHighWater = 0;
	CurrentSet m_touched;
	CurrentSet m_active;
	CurrentSet m_materialDeferred;
	CurrentSet m_structuralDeferred;
	std::vector<std::vector<uint32_t>> m_sectorChunks;
	std::vector<std::vector<uint32_t>> m_sectionChunks;
	std::vector<std::vector<int32_t>> m_chunkSections;
	std::vector<uint32_t> m_mapChunkToList;
	std::vector<uint32_t> m_previousVisible;
	NRIRuntimeMutationWorklist m_inputs;
	NRIRuntimeMutationWorklist m_dispatch;
	BudgetScratch m_budget;
	bool m_validateBudget = false;
	Stats m_stats;
};
