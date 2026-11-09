#include "nri_map_mutation_inputs.h"
#include "perf_audit.h"
#include "build.h"
#include "printf.h"
#include <algorithm>
#include <cstring>

namespace nri_scene
{
namespace
{
thread_local PTMapMutationInputScope* currentScope = nullptr;
template<class T> bool Same(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }
bool SameWall(const PTMapWallMutationSnapshot& a, const PTMapWallMutationSnapshot& b)
{
	return Same(a.pos.X, b.pos.X) && Same(a.pos.Y, b.pos.Y) &&
		Same(a.point2, b.point2) &&
		Same(a.nextwall, b.nextwall) &&
		Same(a.nextsector, b.nextsector) &&
		Same(a.cstat, b.cstat) &&
		Same(a.portalflags, b.portalflags) &&
		Same(a.walltexture, b.walltexture) &&
		Same(a.overtexture, b.overtexture) &&
		Same(a.xpan, b.xpan) &&
		Same(a.ypan, b.ypan) &&
		Same(a.xrepeat, b.xrepeat) &&
		Same(a.yrepeat, b.yrepeat) &&
		Same(a.pal, b.pal) &&
		Same(a.shade, b.shade) &&
		Same(a.adjacentFloorz, b.adjacentFloorz) &&
		Same(a.adjacentCeilingz, b.adjacentCeilingz) &&
		Same(a.adjacentFloorstat, b.adjacentFloorstat) &&
		Same(a.adjacentCeilingstat, b.adjacentCeilingstat) &&
		Same(a.adjacentFloorheinum, b.adjacentFloorheinum) &&
		Same(a.adjacentCeilingheinum, b.adjacentCeilingheinum) &&
		Same(a.adjacentPortalflags, b.adjacentPortalflags) &&
		Same(a.nextWallCstat, b.nextWallCstat) &&
		Same(a.nextWallTexture, b.nextWallTexture) &&
		Same(a.nextOverTexture, b.nextOverTexture) &&
		Same(a.nextWallPal, b.nextWallPal) &&
		Same(a.nextWallShade, b.nextWallShade);
}
bool SameAnalysis(const PTMapChunkMutationAnalysis& a, const PTMapChunkMutationAnalysis& b)
{
	return a.signature == b.signature && a.reasonMask == b.reasonMask && a.sectionDirtyCount == b.sectionDirtyCount &&
		a.sectorDirty == b.sectorDirty && a.dragged == b.dragged && a.signatureChanged == b.signatureChanged;
}
}
bool SamePTMapMutationBaseline(const PTMapChunkMutationBaseline& a, const PTMapChunkMutationBaseline& b)
{
	if (!(Same(a.sectorIndex, b.sectorIndex) &&
		Same(a.signature, b.signature) &&
		Same(a.fluidNormalSignature, b.fluidNormalSignature) &&
		Same(a.floorz, b.floorz) &&
		Same(a.ceilingz, b.ceilingz) &&
		Same(a.floorstat, b.floorstat) &&
		Same(a.ceilingstat, b.ceilingstat) &&
		Same(a.floorheinum, b.floorheinum) &&
		Same(a.ceilingheinum, b.ceilingheinum) &&
		Same(a.portalflags, b.portalflags) &&
		Same(a.floortexture, b.floortexture) &&
		Same(a.ceilingtexture, b.ceilingtexture) &&
		Same(a.floorxpan, b.floorxpan) &&
		Same(a.floorypan, b.floorypan) &&
		Same(a.ceilingxpan, b.ceilingxpan) &&
		Same(a.ceilingypan, b.ceilingypan) &&
		Same(a.floorpal, b.floorpal) &&
		Same(a.ceilingpal, b.ceilingpal) &&
		Same(a.floorshade, b.floorshade) &&
		Same(a.ceilingshade, b.ceilingshade)) || a.sectionIndices != b.sectionIndices || a.walls.size() != b.walls.size()) return false;
	for (size_t i = 0; i < a.walls.size(); ++i) if (!SameWall(a.walls[i], b.walls[i])) return false;
	return true;
}

// Deliberately keeps numeric != semantics from the original analyzer (including
// NaNs and signed zero); exact-bit baseline equality is a separate oracle.
bool AnalyzePTMapMutationSnapshot(const PTMapChunkMutationBaseline& current,
	const PTMapChunkMutationBaseline& baseline, const PTMapMutationLiveFlags& flags,
	PTMapChunkMutationAnalysis& outAnalysis)
{
	outAnalysis = {};
	outAnalysis.signature = current.signature;
	if (baseline.fluidNormalSignature != current.fluidNormalSignature)
	{
		outAnalysis.reasonMask |= PTMapChunkMutationReason_SectorGeometry;
	}

	if (baseline.sectorIndex != current.sectorIndex ||
		baseline.floorz != current.floorz ||
		baseline.ceilingz != current.ceilingz ||
		baseline.floorheinum != current.floorheinum ||
		baseline.ceilingheinum != current.ceilingheinum)
	{
		outAnalysis.reasonMask |= PTMapChunkMutationReason_SectorGeometry;
	}

	if (baseline.floorstat != (uint16_t)current.floorstat ||
		baseline.ceilingstat != (uint16_t)current.ceilingstat ||
		baseline.portalflags != current.portalflags ||
		baseline.floortexture != current.floortexture ||
		baseline.ceilingtexture != current.ceilingtexture ||
		baseline.floorxpan != current.floorxpan ||
		baseline.floorypan != current.floorypan ||
		baseline.ceilingxpan != current.ceilingxpan ||
		baseline.ceilingypan != current.ceilingypan ||
		baseline.floorpal != current.floorpal ||
		baseline.ceilingpal != current.ceilingpal ||
		baseline.floorshade != current.floorshade ||
		baseline.ceilingshade != current.ceilingshade)
	{
		outAnalysis.reasonMask |= PTMapChunkMutationReason_SectorMaterial;
	}

	outAnalysis.sectorDirty = flags.sectorDirty;
	if (flags.sectorDirty) outAnalysis.reasonMask |= PTMapChunkMutationReason_SectorDirty;
	outAnalysis.dragged = flags.dragged;
	if (flags.dragged) outAnalysis.reasonMask |= PTMapChunkMutationReason_Dragged;
	outAnalysis.sectionDirtyCount = flags.sectionDirtyCount;
	if (outAnalysis.sectionDirtyCount > 0)
	{
		outAnalysis.reasonMask |= PTMapChunkMutationReason_SectionDirty;
	}

	if (baseline.walls.size() != current.walls.size())
	{
		outAnalysis.reasonMask |= PTMapChunkMutationReason_WallGeometry;
	}

	const size_t wallCount = std::min<size_t>(baseline.walls.size(), current.walls.size());
	for (size_t wallIndex = 0; wallIndex < wallCount; ++wallIndex)
	{
		const PTMapWallMutationSnapshot& baselineWall = baseline.walls[wallIndex];
		const PTMapWallMutationSnapshot& liveWall = current.walls[wallIndex];
		const uint16_t baselineCstat = baselineWall.cstat;
		const uint16_t liveCstat = (uint16_t)liveWall.cstat;
		static constexpr uint16_t kWallTopologyMutationCstatMask =
			CSTAT_WALL_MASKED |
			CSTAT_WALL_1WAY;

		if (baselineWall.pos != liveWall.pos ||
			baselineWall.point2 != liveWall.point2 ||
			baselineWall.nextwall != liveWall.nextwall ||
			baselineWall.nextsector != liveWall.nextsector)
		{
			outAnalysis.reasonMask |= PTMapChunkMutationReason_WallGeometry;
		}

		if (((baselineCstat ^ liveCstat) & kWallTopologyMutationCstatMask) != 0 ||
			baselineWall.portalflags != liveWall.portalflags)
		{
			outAnalysis.reasonMask |= PTMapChunkMutationReason_WallGeometry;
		}

		if (baselineCstat != liveCstat ||
			baselineWall.portalflags != liveWall.portalflags ||
			baselineWall.walltexture != liveWall.walltexture ||
			baselineWall.overtexture != liveWall.overtexture ||
			baselineWall.xpan != liveWall.xpan ||
			baselineWall.ypan != liveWall.ypan ||
			baselineWall.xrepeat != liveWall.xrepeat ||
			baselineWall.yrepeat != liveWall.yrepeat ||
			baselineWall.pal != liveWall.pal ||
			baselineWall.shade != liveWall.shade)
		{
			outAnalysis.reasonMask |= PTMapChunkMutationReason_WallMaterial;
		}

		if (baselineWall.adjacentFloorz != liveWall.adjacentFloorz ||
			baselineWall.adjacentCeilingz != liveWall.adjacentCeilingz ||
			baselineWall.adjacentFloorheinum != liveWall.adjacentFloorheinum ||
			baselineWall.adjacentCeilingheinum != liveWall.adjacentCeilingheinum ||
			baselineWall.adjacentFloorstat != liveWall.adjacentFloorstat ||
			baselineWall.adjacentCeilingstat != liveWall.adjacentCeilingstat ||
			baselineWall.adjacentPortalflags != liveWall.adjacentPortalflags)
		{
			outAnalysis.reasonMask |= PTMapChunkMutationReason_WallGeometry;
		}

		if (baselineWall.nextWallCstat != liveWall.nextWallCstat ||
			baselineWall.nextWallTexture != liveWall.nextWallTexture ||
			baselineWall.nextOverTexture != liveWall.nextOverTexture ||
			baselineWall.nextWallPal != liveWall.nextWallPal ||
			baselineWall.nextWallShade != liveWall.nextWallShade)
		{
			outAnalysis.reasonMask |= PTMapChunkMutationReason_WallMaterial;
		}
	}

	outAnalysis.signatureChanged = outAnalysis.signature != baseline.signature;
	return true;
}


void PTMapMutationInputStore::Reset() { entries.clear(); epoch = 0; capacityBytes = 0; }
PTMapMutationInputScope::PTMapMutationInputScope(PTMapMutationInputStore& store, size_t chunkCount, bool enabled, bool validate)
	: m_previous(currentScope), m_enabled(enabled), m_validate(validate)
{
	m_store = m_previous != nullptr ? &m_nestedStore : &store;
	if (m_enabled)
	{
		if (m_store->entries.size() != chunkCount)
		{
			m_store->Reset(); m_store->entries.resize(chunkCount);
			m_store->capacityBytes = m_store->entries.capacity() * sizeof(PTMapMutationInputStore::Entry);
		}
		if (++m_store->epoch == 0)
		{
			for (auto& entry : m_store->entries) entry.epoch = 0;
			m_store->epoch = 1;
		}
	}
	currentScope = this;
	PerfAuditAdd(PerfAuditMetric::MutationInputsOperations);
}
PTMapMutationInputScope::~PTMapMutationInputScope()
{
	currentScope = m_previous;
	// A nested service may change inputs. The outer operation must recapture.
	if (m_previous != nullptr && ++m_previous->m_store->epoch == 0)
	{
		for (auto& entry : m_previous->m_store->entries) entry.epoch = 0;
		m_previous->m_store->epoch = 1;
	}
}
PTMapMutationInputScope* PTMapMutationInputScope::Current()
{
	return currentScope != nullptr && currentScope->m_enabled && !currentScope->m_bypass && !currentScope->m_quarantined ? currentScope : nullptr;
}
PTMapMutationInputStore::Entry* PTMapMutationInputScope::Entry(const PTMapChunk& chunk)
{
	if (chunk.chunkIndex >= m_store->entries.size()) return nullptr;
	auto& entry = m_store->entries[chunk.chunkIndex];
	if (entry.epoch != m_store->epoch || entry.sectorIndex != chunk.sectorIndex || entry.kind != chunk.kind)
	{
		entry.epoch = m_store->epoch; entry.sectorIndex = chunk.sectorIndex; entry.kind = chunk.kind;
		entry.signatureReady = entry.baselineReady = entry.baselineValid = false;
	}
	return &entry;
}
void PTMapMutationInputScope::Mismatch(const char* reason, uint32_t chunkIndex)
{
	PerfAuditAdd(PerfAuditMetric::MutationInputsValidationMismatches);
	if (!m_quarantined) PerfAuditAdd(PerfAuditMetric::MutationInputsQuarantines);
	m_quarantined = true;
	Printf("NRI mutation input validation mismatch: chunk=%u reason=%s quarantine=1\n", chunkIndex, reason);
}
uint64_t PTMapMutationInputScope::Signature(const PTMapChunk& chunk, SignatureFn original)
{
	auto* entry = Entry(chunk);
	if (entry == nullptr) return original(chunk);
	if (!entry->signatureReady)
	{
		entry->signature = original(chunk); entry->signatureReady = true;
		return entry->signature;
	}
	PerfAuditAdd(PerfAuditMetric::MutationInputsSignatureReuses);
	if (m_validate)
	{
		PerfAuditScope audit(PerfAuditMetric::MutationInputsValidationNs);
		m_bypass = true; const uint64_t reference = original(chunk); m_bypass = false;
		PerfAuditAdd(PerfAuditMetric::MutationInputsSignatureReuseValidationChecks);
		if (reference != entry->signature) { Mismatch("signature", chunk.chunkIndex); return reference; }
	}
	return entry->signature;
}
bool PTMapMutationInputScope::Snapshot(const PTMapChunk& chunk, PTMapMutationInputStore::Entry& entry, CaptureFn capture)
{
	if (!entry.baselineReady)
	{
		PerfAuditScope audit(PerfAuditMetric::MutationInputsSnapshotNs);
		PerfAuditAdd(PerfAuditMetric::MutationInputsSnapshots);
		const auto capacity = [&]() { return entry.baseline.walls.capacity() * sizeof(PTMapWallMutationSnapshot) +
			entry.baseline.sectionIndices.capacity() * sizeof(int32_t); };
		m_store->capacityBytes -= capacity();
		entry.baselineValid = capture(chunk, entry.baseline, true);
		m_store->capacityBytes += capacity();
		PerfAuditSet(PerfAuditMetric::MutationInputsSnapshotCapacityBytes, m_store->capacityBytes);
		entry.baselineReady = true;
	}
	else PerfAuditAdd(PerfAuditMetric::MutationInputsSnapshotReuses);
	return entry.baselineValid;
}
bool PTMapMutationInputScope::Capture(const PTMapChunk& chunk, PTMapChunkMutationBaseline& out, CaptureFn original)
{
	auto* entry = Entry(chunk);
	if (entry == nullptr) return original(chunk, out, false);
	const bool valid = Snapshot(chunk, *entry, original);
	out = entry->baseline;
	if (m_validate)
	{
		PerfAuditScope audit(PerfAuditMetric::MutationInputsValidationNs);
		PTMapChunkMutationBaseline reference;
		m_bypass = true; const bool referenceValid = original(chunk, reference, false); m_bypass = false;
		PerfAuditAdd(PerfAuditMetric::MutationInputsBaselineReuseValidationChecks);
		if (valid != referenceValid || !SamePTMapMutationBaseline(out, reference))
		{ Mismatch("baseline", chunk.chunkIndex); out = std::move(reference); return referenceValid; }
	}
	return valid;
}
bool PTMapMutationInputScope::Analyze(const PTMapChunk& chunk, const PTMapChunkMutationBaseline& baseline,
	PTMapChunkMutationAnalysis& out, CaptureFn capture, AnalyzeFn original, FlagsFn flags)
{
	auto* entry = Entry(chunk);
	if (entry == nullptr) return original(chunk, baseline, out);
	bool valid = Snapshot(chunk, *entry, capture);
	out = {};
	if (valid) AnalyzePTMapMutationSnapshot(entry->baseline, baseline, flags(chunk, baseline), out);
	PerfAuditAdd(PerfAuditMetric::MutationInputsSnapshotAnalyses);
	if (m_validate)
	{
		PerfAuditScope audit(PerfAuditMetric::MutationInputsValidationNs);
		PTMapChunkMutationAnalysis reference;
		PTMapChunkMutationBaseline referenceBaseline;
		m_bypass = true;
		const bool referenceValid = original(chunk, baseline, reference);
		const bool referenceCaptured = capture(chunk, referenceBaseline, false);
		m_bypass = false;
		PerfAuditAdd(PerfAuditMetric::MutationInputsAnalysisValidationChecks);
		if (valid != referenceValid || valid != referenceCaptured || !SameAnalysis(out, reference) ||
			!SamePTMapMutationBaseline(entry->baseline, referenceBaseline))
		{ Mismatch("analysis-or-current-baseline", chunk.chunkIndex); out = reference; return referenceValid; }
	}
	return valid;
}
}
