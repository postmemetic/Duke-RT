#pragma once

#include <cstdint>
#include <vector>

class NRIRuntimeMutationWorklist
{
public:
	void Reset();
	void BeginFrame(uint32_t chunkCount);
	bool MarkCandidate(uint32_t chunkIndex, uint32_t sourceMask);
	void SortCandidates();

	uint32_t GetSourceMask(uint32_t chunkIndex) const;
	// Full snapshots are diagnostic-only; ordinary consumers use GetSourceMask.
	const std::vector<uint32_t>& GetSourceMasks() const;
	const std::vector<uint32_t>& GetCandidates() const { return m_candidates; }

private:
	uint32_t m_generation = 0;
	std::vector<uint32_t> m_marks;
	std::vector<uint32_t> m_sourceMasks;
	std::vector<uint32_t> m_candidates;
	mutable std::vector<uint32_t> m_snapshot;
};
