#pragma once

#include "nri_map_builder.h"

namespace nri_scene
{
// Dirty bits are consumed by SectionGeometry during reconstruction. They are
// deliberately observed at each comparison, using that baseline's section list.
struct PTMapMutationLiveFlags
{
	uint32_t sectionDirtyCount = 0;
	bool sectorDirty = false;
	bool dragged = false;
};

bool SamePTMapMutationBaseline(const PTMapChunkMutationBaseline& lhs, const PTMapChunkMutationBaseline& rhs);
bool AnalyzePTMapMutationSnapshot(const PTMapChunkMutationBaseline& current,
	const PTMapChunkMutationBaseline& baseline, const PTMapMutationLiveFlags& flags,
	PTMapChunkMutationAnalysis& result);

class PTMapMutationInputScope;
class PTMapMutationInputStore
{
public:
	void Reset();
private:
	friend class PTMapMutationInputScope;
	struct Entry
	{
		uint64_t epoch = 0;
		int32_t sectorIndex = -1;
		PTMapChunkKind kind = {};
		uint64_t signature = 0;
		bool signatureReady = false;
		bool baselineReady = false;
		bool baselineValid = false;
		PTMapChunkMutationBaseline baseline;
	};
	uint64_t epoch = 0;
	uint64_t capacityBytes = 0;
	std::vector<Entry> entries;
};

// One synchronous mutation invocation, never a tick/frame-wide assumption.
// Nested invocations suspend the parent and use independent temporary storage.
class PTMapMutationInputScope
{
public:
	using SignatureFn = uint64_t (*)(const PTMapChunk&);
	using CaptureFn = bool (*)(const PTMapChunk&, PTMapChunkMutationBaseline&, bool);
	using AnalyzeFn = bool (*)(const PTMapChunk&, const PTMapChunkMutationBaseline&, PTMapChunkMutationAnalysis&);
	using FlagsFn = PTMapMutationLiveFlags (*)(const PTMapChunk&, const PTMapChunkMutationBaseline&);
	PTMapMutationInputScope(PTMapMutationInputStore& store, size_t chunkCount, bool enabled, bool validate);
	~PTMapMutationInputScope();
	PTMapMutationInputScope(const PTMapMutationInputScope&) = delete;
	PTMapMutationInputScope& operator=(const PTMapMutationInputScope&) = delete;
	static PTMapMutationInputScope* Current();
	bool IsNested() const { return m_previous != nullptr; }
	uint64_t Signature(const PTMapChunk& chunk, SignatureFn original);
	bool Capture(const PTMapChunk& chunk, PTMapChunkMutationBaseline& out, CaptureFn original);
	bool Analyze(const PTMapChunk& chunk, const PTMapChunkMutationBaseline& baseline,
		PTMapChunkMutationAnalysis& out, CaptureFn capture, AnalyzeFn original, FlagsFn flags);
private:
	PTMapMutationInputStore::Entry* Entry(const PTMapChunk& chunk);
	bool Snapshot(const PTMapChunk& chunk, PTMapMutationInputStore::Entry& entry, CaptureFn capture);
	void Mismatch(const char* reason, uint32_t chunkIndex);
	PTMapMutationInputStore m_nestedStore;
	PTMapMutationInputStore* m_store = nullptr;
	PTMapMutationInputScope* m_previous = nullptr;
	bool m_enabled = false;
	bool m_validate = false;
	bool m_bypass = false;
	bool m_quarantined = false;
};
}
