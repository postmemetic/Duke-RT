#include "nri_runtime_mutation_worklist.h"

#include <algorithm>

void NRIRuntimeMutationWorklist::Reset()
{
	m_generation = 0;
	m_marks.clear();
	m_sourceMasks.clear();
	m_candidates.clear();
	m_snapshot.clear();
}

void NRIRuntimeMutationWorklist::BeginFrame(uint32_t chunkCount)
{
	if (m_sourceMasks.size() != chunkCount)
	{
		m_sourceMasks.resize(chunkCount);
		m_marks.assign(chunkCount, 0u);
		m_candidates.reserve(chunkCount);
		m_generation = 0;
	}
	if (++m_generation == 0u)
	{
		std::fill(m_marks.begin(), m_marks.end(), 0u);
		m_generation = 1u;
	}
	m_candidates.clear();
}

bool NRIRuntimeMutationWorklist::MarkCandidate(uint32_t chunkIndex, uint32_t sourceMask)
{
	if (chunkIndex >= m_sourceMasks.size() || sourceMask == 0u)
		return false;

	const bool added = m_marks[chunkIndex] != m_generation;
	if (added)
	{
		m_marks[chunkIndex] = m_generation;
		m_sourceMasks[chunkIndex] = sourceMask;
		m_candidates.push_back(chunkIndex);
	}
	else
		m_sourceMasks[chunkIndex] |= sourceMask;
	return added;
}

uint32_t NRIRuntimeMutationWorklist::GetSourceMask(uint32_t chunkIndex) const
{
	return chunkIndex < m_marks.size() && m_marks[chunkIndex] == m_generation ?
		m_sourceMasks[chunkIndex] : 0u;
}

const std::vector<uint32_t>& NRIRuntimeMutationWorklist::GetSourceMasks() const
{
	m_snapshot.assign(m_sourceMasks.size(), 0u);
	for (uint32_t chunkIndex : m_candidates)
		m_snapshot[chunkIndex] = m_sourceMasks[chunkIndex];
	return m_snapshot;
}

void NRIRuntimeMutationWorklist::SortCandidates()
{
	std::sort(m_candidates.begin(), m_candidates.end());
}
