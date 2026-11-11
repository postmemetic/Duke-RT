#pragma once

#include "nri_static_scene.h"
#include "perf_audit.h"
#include "printf.h"

// Allocation bookkeeping only. Equal counts never certify geometry/material
// content, slot order, visibility, or whether an upload may be omitted.
namespace nri_static_atlas_allocation
{
struct Counts
{
	uint32_t vertices = 0, indices = 0, primitives = 0, materials = 0;
};

inline bool RangeFits(uint32_t offset, uint32_t count, uint32_t used, uint32_t capacity)
{
	return offset != UINT32_MAX && used <= capacity && offset <= used && count <= used - offset;
}

inline bool TryRetain(const StaticMapChunkAtlas& atlas, uint64_t buildSerial,
	uint32_t chunkIndex, uint32_t chunkListIndex, bool hasResidentGeometry,
	const Counts& counts, StaticMapChunkAtlas::ChunkEntry& out)
{
	if (!hasResidentGeometry || !atlas.valid || atlas.buildSerial != buildSerial ||
		atlas.chunkCount != atlas.chunks.size() || chunkListIndex >= atlas.chunks.size())
		return false;
	const auto& old = atlas.chunks[chunkListIndex];
	if (!old.valid || old.chunkIndex != chunkIndex || old.staticSceneChunkListIndex != chunkListIndex ||
		old.vertexCount != counts.vertices || old.indexCount != counts.indices ||
		old.primitiveCount != counts.primitives || old.materialCount != counts.materials ||
		!RangeFits(old.vertexOffset, old.vertexCount, atlas.vertexCount, atlas.vertexCapacity) ||
		!RangeFits(old.indexOffset, old.indexCount, atlas.indexCount, atlas.indexCapacity) ||
		!RangeFits(old.primitiveOffset, old.primitiveCount, atlas.primitiveCount, atlas.primitiveCapacity) ||
		!RangeFits(old.materialOffset, old.materialCount, atlas.materialCount, atlas.materialCapacity))
		return false;

	// Reconstruct the complete entry just as the ordinary allocation path does.
	out = {};
	out.valid = true;
	out.chunkIndex = chunkIndex;
	out.staticSceneChunkListIndex = chunkListIndex;
	out.vertexOffset = old.vertexOffset;
	out.vertexCount = counts.vertices;
	out.indexOffset = old.indexOffset;
	out.indexCount = counts.indices;
	out.primitiveOffset = old.primitiveOffset;
	out.primitiveCount = counts.primitives;
	out.materialOffset = old.materialOffset;
	out.materialCount = counts.materials;
	return true;
}

// Semantic equality, including order. Object padding/vector capacities are not
// allocator state and intentionally differ when the ordinary path clones it.
inline bool SameEntry(const StaticMapChunkAtlas::ChunkEntry& a, const StaticMapChunkAtlas::ChunkEntry& b)
{
	return a.chunkIndex == b.chunkIndex && a.staticSceneChunkListIndex == b.staticSceneChunkListIndex &&
		a.vertexOffset == b.vertexOffset && a.vertexCount == b.vertexCount &&
		a.indexOffset == b.indexOffset && a.indexCount == b.indexCount &&
		a.primitiveOffset == b.primitiveOffset && a.primitiveCount == b.primitiveCount &&
		a.materialOffset == b.materialOffset && a.materialCount == b.materialCount && a.valid == b.valid;
}

inline bool SameRanges(const std::vector<StaticMapChunkAtlas::FreeRange>& a,
	const std::vector<StaticMapChunkAtlas::FreeRange>& b)
{
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].offset != b[i].offset || a[i].count != b[i].count) return false;
	return true;
}

inline bool SameState(const StaticMapChunkAtlas& a, const StaticMapChunkAtlas& b)
{
	if (a.valid != b.valid || a.buildSerial != b.buildSerial || a.chunkCount != b.chunkCount ||
		a.vertexCount != b.vertexCount || a.indexCount != b.indexCount ||
		a.primitiveCount != b.primitiveCount || a.materialCount != b.materialCount ||
		a.vertexCapacity != b.vertexCapacity || a.indexCapacity != b.indexCapacity ||
		a.primitiveCapacity != b.primitiveCapacity || a.materialCapacity != b.materialCapacity ||
		a.chunks.size() != b.chunks.size() ||
		!SameRanges(a.freeVertexRanges, b.freeVertexRanges) || !SameRanges(a.freeIndexRanges, b.freeIndexRanges) ||
		!SameRanges(a.freePrimitiveRanges, b.freePrimitiveRanges) || !SameRanges(a.freeMaterialRanges, b.freeMaterialRanges))
		return false;
	for (size_t i = 0; i < a.chunks.size(); ++i)
		if (!SameEntry(a.chunks[i], b.chunks[i])) return false;
	return true;
}

inline uint64_t LogicalCloneBytes(const StaticMapChunkAtlas& atlas)
{
	return atlas.chunks.size() * sizeof(StaticMapChunkAtlas::ChunkEntry) +
		(atlas.freeVertexRanges.size() + atlas.freeIndexRanges.size() +
		 atlas.freePrimitiveRanges.size() + atlas.freeMaterialRanges.size()) * sizeof(StaticMapChunkAtlas::FreeRange);
}

// Only a failed independent comparison is quarantined. Conservative eligibility
// rejection is ordinary fallback. A new atlas build starts a new validation domain.
class ValidationState
{
	uint64_t mBuildSerial = UINT64_MAX;
	bool mQuarantined = false;
public:
	void Begin(uint64_t buildSerial)
	{
		if (mBuildSerial != buildSerial) { mBuildSerial = buildSerial; mQuarantined = false; }
	}
	bool Allowed() const { return !mQuarantined; }
	bool Quarantine()
	{
		const bool first = !mQuarantined;
		mQuarantined = true;
		return first;
	}
};

// One synchronous allocation transaction. The caller still runs its unchanged
// original allocator when requested and commits at its original boundary.
class RetainedPlan
{
	ValidationState& mValidation;
	StaticMapChunkAtlas::ChunkEntry mRetainedEntry = {};
	uint32_t mChunkIndex, mChunkListIndex;
	uint64_t mReferenceStart = 0;
	bool mReuse = false, mValidate = false;
	void Quarantine(const char* stage)
	{
		PerfAuditAdd(PerfAuditMetric::MutationAtlasAllocationValidationMismatches);
		if (mValidation.Quarantine()) PerfAuditAdd(PerfAuditMetric::MutationAtlasAllocationQuarantines);
		Printf("PERF pt atlas allocation reference NRI: chunk=%u stage=%s mismatches=1 recovery=original\n", mChunkIndex, stage);
	}
public:
	RetainedPlan(ValidationState& validation, const StaticMapChunkAtlas& atlas,
		uint64_t buildSerial, uint32_t chunkIndex, uint32_t chunkListIndex,
		bool hasResidentGeometry, const Counts& counts, bool enabled, bool validate)
		: mValidation(validation), mChunkIndex(chunkIndex), mChunkListIndex(chunkListIndex)
	{
		mValidation.Begin(atlas.buildSerial);
		mReuse = enabled && mValidation.Allowed() &&
			TryRetain(atlas, buildSerial, chunkIndex, chunkListIndex, hasResidentGeometry, counts, mRetainedEntry);
		mValidate = mReuse && validate;
		if (mReuse) PerfAuditAdd(PerfAuditMetric::MutationAtlasAllocationReuseCandidates);
		else if (enabled) PerfAuditAdd(PerfAuditMetric::MutationAtlasAllocationFallbacks);
		mReferenceStart = mValidate ? PerfAuditClockNs() : 0;
	}
	bool NeedsOriginalPlan() const { return !mReuse || mValidate; }

	void Complete(const StaticMapChunkAtlas& atlas, const StaticMapChunkAtlas& originalState,
		StaticMapChunkAtlas::ChunkEntry& entry, bool& keptGeometry, bool& keptMaterial)
	{
		if (!mReuse) return;
		if (mValidate)
		{
			PerfAuditAdd(PerfAuditMetric::MutationAtlasAllocationValidationChecks);
			if (!keptGeometry || !keptMaterial || !SameEntry(mRetainedEntry, entry) || !SameState(atlas, originalState))
			{
				mReuse = false;
				Quarantine("plan");
			}
			PerfAuditAdd(PerfAuditMetric::MutationAtlasAllocationReferenceNs, PerfAuditClockNs() - mReferenceStart);
		}
		else
		{
			entry = mRetainedEntry;
			keptGeometry = true;
			keptMaterial = true;
		}
	}

	void Commit(StaticMapChunkAtlas& atlas, StaticMapChunkAtlas& originalState,
		const StaticMapChunkAtlas::ChunkEntry& entry)
	{
		if (!mReuse) atlas = std::move(originalState);
		atlas.chunks[mChunkListIndex] = entry;
		if (mReuse && mValidate)
		{
			const uint64_t start = PerfAuditClockNs();
			originalState.chunks[mChunkListIndex] = entry;
			PerfAuditAdd(PerfAuditMetric::MutationAtlasAllocationValidationCommitChecks);
			if (!SameState(atlas, originalState))
			{
				atlas = std::move(originalState);
				mReuse = false;
				Quarantine("commit");
			}
			PerfAuditAdd(PerfAuditMetric::MutationAtlasAllocationReferenceNs, PerfAuditClockNs() - start);
		}
		if (mReuse)
		{
			PerfAuditAdd(PerfAuditMetric::MutationAtlasAllocationReuses);
			// Validation deliberately makes the original clone; never count those
			// diagnostic copies as avoided allocation traffic.
			if (!mValidate) PerfAuditAdd(PerfAuditMetric::MutationAtlasAllocationAvoidedCloneBytes, LogicalCloneBytes(atlas));
		}
	}
};
}
