#include "nri_runtime_mutation_discovery.h"

#include "nri_runtime_mutation.h"
#include "hw_sections.h"

#include <algorithm>

void NRIRuntimeMutationDiscovery::CurrentSet::Reset(uint32_t count)
{
	entries.clear();
	entries.reserve(count);
	positions.assign(count, UINT32_MAX);
}

void NRIRuntimeMutationDiscovery::CurrentSet::Set(uint32_t index, bool present)
{
	if (index >= positions.size()) return;
	const uint32_t position = positions[index];
	if (present && position == UINT32_MAX)
	{
		positions[index] = (uint32_t)entries.size();
		entries.push_back(index);
	}
	else if (!present && position != UINT32_MAX)
	{
		const uint32_t moved = entries.back();
		entries[position] = moved;
		positions[moved] = position;
		entries.pop_back();
		positions[index] = UINT32_MAX;
	}
}

void NRIRuntimeMutationDiscovery::CurrentSet::Clear()
{
	for (uint32_t index : entries) positions[index] = UINT32_MAX;
	entries.clear();
}

void NRIRuntimeMutationDiscovery::Reset()
{
	m_initialized = false;
	m_buildSerial = 0;
	m_chunkCount = 0;
	m_inputHighWater = m_activeHighWater = m_deferredHighWater = 0;
	m_touched.Clear();
	m_active.Clear();
	m_materialDeferred.Clear();
	m_structuralDeferred.Clear();
	m_previousVisible.clear();
	m_inputs.Reset();
	m_dispatch.Reset();
	m_budget.allowed.Reset();
	m_budget.nearStructural.clear();
	m_budget.farStructural.clear();
	m_budget.nearMaterial.clear();
	m_stats = {};
}

void NRIRuntimeMutationDiscovery::NoteTouchedChunk(uint32_t chunkIndex)
{
	if (m_initialized) m_touched.Set(chunkIndex, true);
}

void NRIRuntimeMutationDiscovery::UpdateSectionMembership(
	uint32_t chunkListIndex, const std::vector<int32_t>& sectionIndices)
{
	auto& previous = m_chunkSections[chunkListIndex];
	if (previous == sectionIndices) return;
	for (int32_t sectionIndex : previous)
	{
		if ((uint32_t)sectionIndex >= m_sectionChunks.size()) continue;
		auto& reverse = m_sectionChunks[(uint32_t)sectionIndex];
		reverse.erase(std::remove(reverse.begin(), reverse.end(), chunkListIndex), reverse.end());
	}
	previous = sectionIndices;
	for (int32_t sectionIndex : previous)
	{
		if ((uint32_t)sectionIndex < m_sectionChunks.size())
			m_sectionChunks[(uint32_t)sectionIndex].push_back(chunkListIndex);
	}
}

void NRIRuntimeMutationDiscovery::RefreshTouched(const NRIRuntimeMutationSystem& mutation)
{
	if (!m_initialized) return;
	for (uint32_t chunkIndex : m_touched.entries)
	{
		const auto* replacement = mutation.FindReplacement(chunkIndex);
		if (replacement == nullptr) continue;
		m_active.Set(chunkIndex, replacement->active || (replacement->valid && !replacement->residentAuthoritative));
		m_materialDeferred.Set(chunkIndex, replacement->deferredMaterialRefresh);
		m_structuralDeferred.Set(chunkIndex, replacement->deferredStructuralRebuild);
		UpdateSectionMembership(chunkIndex, replacement->baseline.sectionIndices);
		m_stats.reconciledChunks++;
	}
	m_touched.Clear();
	m_stats.activeCount = (uint32_t)m_active.entries.size();
	m_stats.materialDeferredCount = (uint32_t)m_materialDeferred.entries.size();
	m_stats.structuralDeferredCount = (uint32_t)m_structuralDeferred.entries.size();
	m_stats.watchCount = mutation.GetSignatureWatchlistSeedCount();
	m_activeHighWater = std::max(m_activeHighWater, m_stats.activeCount);
	m_deferredHighWater = std::max(m_deferredHighWater, m_stats.materialDeferredCount + m_stats.structuralDeferredCount);
	m_stats.activeHighWater = m_activeHighWater;
	m_stats.deferredHighWater = m_deferredHighWater;
}

void NRIRuntimeMutationDiscovery::AddInput(uint32_t chunkListIndex)
{
	if (!m_inputs.MarkCandidate(chunkListIndex, 1u) && chunkListIndex < m_chunkCount)
		m_stats.dedupeHits++;
}

void NRIRuntimeMutationDiscovery::ValidateCurrentState(const NRIRuntimeMutationSystem& mutation, bool repair)
{
	for (uint32_t i = 0; i < m_chunkCount; ++i)
	{
		const auto* replacement = mutation.FindReplacement(i);
		if (replacement == nullptr) continue;
		const bool active = replacement->active || (replacement->valid && !replacement->residentAuthoritative);
		if ((m_active.positions[i] != UINT32_MAX) != active ||
			(m_materialDeferred.positions[i] != UINT32_MAX) != replacement->deferredMaterialRefresh ||
			(m_structuralDeferred.positions[i] != UINT32_MAX) != replacement->deferredStructuralRebuild ||
			m_chunkSections[i] != replacement->baseline.sectionIndices)
		{
			m_stats.stateMismatches++;
			if (repair) m_touched.Set(i, true);
		}
	}
	// During rollout the legacy full state remains budget authority even if a
	// compact membership fault was diagnosed. Preserve the mismatch telemetry.
	if (repair) RefreshTouched(mutation);
}

const std::vector<uint32_t>& NRIRuntimeMutationDiscovery::GatherInputs(
	const nri_scene::PTMapWorld& world,
	const NRIRuntimeMutationSystem& mutation,
	const std::vector<uint32_t>& visibleWords,
	const std::set<uint32_t>& settleChunks)
{
	m_stats = {};
	const uint32_t chunkCount = (uint32_t)world.chunks.size();
	const bool rebuild = !m_initialized || m_buildSerial != world.buildSerial ||
		m_chunkCount != chunkCount || m_sectorChunks.size() != sector.Size() ||
		m_sectionChunks.size() != sections.Size();
	if (rebuild)
	{
		if (m_buildSerial != world.buildSerial || m_chunkCount != chunkCount)
			m_inputHighWater = m_activeHighWater = m_deferredHighWater = 0;
		m_initialized = true;
		m_buildSerial = world.buildSerial;
		m_chunkCount = chunkCount;
		m_touched.Reset(chunkCount);
		m_active.Reset(chunkCount);
		m_materialDeferred.Reset(chunkCount);
		m_structuralDeferred.Reset(chunkCount);
		m_sectorChunks.assign(sector.Size(), {});
		m_sectionChunks.assign(sections.Size(), {});
		m_chunkSections.assign(chunkCount, {});
		m_mapChunkToList.assign(chunkCount, UINT32_MAX);
		m_previousVisible.clear();
		m_previousVisible.reserve(chunkCount);
		for (uint32_t i = 0; i < chunkCount; ++i)
		{
			const auto& chunk = world.chunks[i];
			if (chunk.chunkIndex < m_mapChunkToList.size()) m_mapChunkToList[chunk.chunkIndex] = i;
			if ((uint32_t)chunk.sectorIndex < m_sectorChunks.size())
				m_sectorChunks[(uint32_t)chunk.sectorIndex].push_back(i);
			m_touched.Set(i, true);
		}
	}
	RefreshTouched(mutation);
	m_inputs.BeginFrame(chunkCount);
	// Initialization includes all chunks so pending startup visibility flags and
	// any existing resident visibility state get their normal maintenance pass.
	if (rebuild)
		for (uint32_t i = 0; i < chunkCount; ++i) AddInput(i);
	for (uint32_t i : m_active.entries) AddInput(i);
	for (uint32_t i : m_materialDeferred.entries) AddInput(i);
	for (uint32_t i : m_structuralDeferred.entries) AddInput(i);
	for (uint32_t i : mutation.GetSignatureWatchlistChunks()) AddInput(i);
	for (uint32_t i : m_previousVisible) AddInput(i);
	m_previousVisible.clear();
	for (uint32_t wordIndex = 0; wordIndex < visibleWords.size(); ++wordIndex)
	{
		uint32_t word = visibleWords[wordIndex];
		for (uint32_t bit = 0; word != 0; ++bit, word >>= 1u)
		{
			if ((word & 1u) == 0) continue;
			const uint32_t mapIndex = wordIndex * 32u + bit;
			if (mapIndex >= m_mapChunkToList.size()) continue;
			const uint32_t listIndex = m_mapChunkToList[mapIndex];
			if (listIndex == UINT32_MAX) continue;
			AddInput(listIndex);
			m_previousVisible.push_back(listIndex);
		}
	}
	for (uint32_t mapIndex : settleChunks)
	{
		if (mapIndex < m_mapChunkToList.size()) AddInput(m_mapChunkToList[mapIndex]);
	}
	// Engine dirty bits are not published as an event stream. Poll their source
	// arrays, then fan out only dirty sources through current reverse membership.
	for (uint32_t i = 0; i < sector.Size(); ++i)
	{
		m_stats.sourcePolls++;
		if (sector[i].dirty != 0 || (sector[i].exflags & SECTOREX_DRAGGED) != 0)
			for (uint32_t chunkIndex : m_sectorChunks[i]) AddInput(chunkIndex);
	}
	for (uint32_t i = 0; i < sections.Size(); ++i)
	{
		m_stats.sourcePolls++;
		if (sections[i].dirty != 0)
			for (uint32_t chunkIndex : m_sectionChunks[i]) AddInput(chunkIndex);
	}
	m_stats.discoveryInputCount = (uint32_t)m_inputs.GetCandidates().size();
	m_inputHighWater = std::max(m_inputHighWater, m_stats.discoveryInputCount);
	m_stats.inputHighWater = m_inputHighWater;
	return m_inputs.GetCandidates();
}

const std::vector<uint32_t>& NRIRuntimeMutationDiscovery::BuildDispatchInputs(
	const std::vector<uint32_t>& candidates, bool fullScan)
{
	m_dispatch.BeginFrame(m_chunkCount);
	if (fullScan)
		for (uint32_t i = 0; i < m_chunkCount; ++i) m_dispatch.MarkCandidate(i, 1u);
	else
	{
		// Current/prior visible inputs also need visibility bookkeeping even when
		// they do not require truth analysis. Their ordering remains chunk order.
		for (uint32_t i : m_inputs.GetCandidates()) m_dispatch.MarkCandidate(i, 1u);
		for (uint32_t i : candidates) m_dispatch.MarkCandidate(i, 1u);
	}
	m_dispatch.SortCandidates();
	m_stats.dispatchInputCount = (uint32_t)m_dispatch.GetCandidates().size();
	return m_dispatch.GetCandidates();
}

NRIRuntimeMutationDiscovery::BudgetScratch& NRIRuntimeMutationDiscovery::BeginBudget(uint32_t chunkCount, bool validate)
{
	m_validateBudget = validate;
	m_budget.allowed.BeginFrame(chunkCount);
	m_budget.nearStructural.clear();
	m_budget.farStructural.clear();
	m_budget.nearMaterial.clear();
	return m_budget;
}

void NRIRuntimeMutationDiscovery::SelectOldest(std::vector<BudgetCandidate>& candidates, uint32_t budget)
{
	m_stats.budgetCandidates += (uint32_t)candidates.size();
	const auto oldest = [](const BudgetCandidate& a, const BudgetCandidate& b)
	{
		return a.deferredFrame != b.deferredFrame ? a.deferredFrame < b.deferredFrame :
			a.chunkListIndex < b.chunkListIndex;
	};
	std::vector<BudgetCandidate> reference;
	if (m_validateBudget)
	{
		reference = candidates;
		m_stats.budgetValidationSorted += (uint32_t)reference.size();
		std::sort(reference.begin(), reference.end(), oldest);
		reference.resize(std::min<size_t>(budget, reference.size()));
	}
	if (candidates.size() > budget)
	{
		std::nth_element(candidates.begin(), candidates.begin() + budget, candidates.end(), oldest);
		candidates.resize(budget);
	}
	if (m_validateBudget)
	{
		// The legacy policy sorted all deferred entries. Compare the selected
		// identities against that policy only in requested shadow validation.
		for (const auto& candidate : reference)
		{
			if (std::find_if(candidates.begin(), candidates.end(), [&](const BudgetCandidate& selected)
				{ return selected.chunkListIndex == candidate.chunkListIndex; }) == candidates.end())
				m_stats.budgetMismatches++;
		}
	}
	m_stats.budgetSelected += (uint32_t)candidates.size();
}
