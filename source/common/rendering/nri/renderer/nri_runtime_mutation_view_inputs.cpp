#include "nri_runtime_mutation_view_inputs.h"
#include "nri_emissive_tail_composition.h"
#include "perf_audit.h"
#include "printf.h"

namespace
{
bool SameStats(const nri_scene::PTMapWorldStats& a, const nri_scene::PTMapWorldStats& b)
{
	return a.sectorCount == b.sectorCount &&
		a.sectionCount == b.sectionCount &&
		a.chunkCount == b.chunkCount &&
		a.localSpaceCount == b.localSpaceCount &&
		a.surfaceCount == b.surfaceCount &&
		a.wallSurfaceCount == b.wallSurfaceCount &&
		a.flatSurfaceCount == b.flatSurfaceCount &&
		a.portalSurfaceCount == b.portalSurfaceCount &&
		a.skySurfaceCount == b.skySurfaceCount &&
		a.triangleCount == b.triangleCount &&
		a.portalCount == b.portalCount &&
		a.portalTargetCount == b.portalTargetCount &&
		a.wallPortalCount == b.wallPortalCount &&
		a.sectorPortalCount == b.sectorPortalCount &&
		a.mirrorPortalCount == b.mirrorPortalCount &&
		a.runtimePortalCount == b.runtimePortalCount;
}
uint64_t Capacity(const nri_scene::SceneView& view)
{
	uint64_t bytes = 0;
	const auto add = [&](const auto& surfaces)
	{
		bytes += surfaces.capacity() * sizeof(nri_scene::SurfaceRef);
		for (const auto& surface : surfaces)
			bytes += surface.vertices.capacity() * sizeof(nri_scene::CapturedVertex) +
				surface.indices.capacity() * sizeof(uint32_t) + surface.primitiveLocalMaterialSlots.capacity();
	};
	add(view.opaqueWalls); add(view.opaqueFlats); add(view.opaqueSprites);
	return bytes;
}
}
void NRIRuntimeMutationViewInputs::Reset()
{
	m_records.clear(); m_probe = {}; m_buildSerial = 0; m_capacityBytes = 0; m_quarantined = false;
}
void NRIRuntimeMutationViewInputs::Begin(uint64_t buildSerial, size_t chunkCount)
{
	if (m_buildSerial != buildSerial || m_records.size() != chunkCount)
	{
		Reset(); m_buildSerial = buildSerial; m_records.resize(chunkCount);
	}
	PerfAuditSet(PerfAuditMetric::MutationInputsNoopCapacityBytes, m_capacityBytes + m_records.capacity() * sizeof(Record) +
		m_probe.facts.capacity() * sizeof(uint64_t) + m_probe.authored.walls.capacity() * sizeof(nri_scene::PTMapWallMutationSnapshot) +
		m_probe.authored.sectionIndices.capacity() * sizeof(int32_t));
}
bool NRIRuntimeMutationViewInputs::TryReuse(const nri_scene::PTMapChunk& chunk, nri_scene::PTMapChunkViewInputs& current,
	nri_scene::SceneView& outRawView, nri_scene::PTMapWorldStats& outStats, bool validate)
{
	PerfAuditScope audit(PerfAuditMetric::MutationInputsNoopProbeNs);
	PerfAuditAdd(PerfAuditMetric::MutationInputsNoopProbes);
	current.valid = false;
	if (m_quarantined || chunk.chunkIndex >= m_records.size() ||
		!nri_scene::CapturePlainMapChunkViewInputs(chunk, current))
	{
		PerfAuditAdd(PerfAuditMetric::MutationInputsNoopUnsupported);
		return false;
	}
	auto& record = m_records[chunk.chunkIndex];
	if (!nri_scene::SamePlainMapChunkViewInputs(current, record.inputs))
	{
		PerfAuditAdd(PerfAuditMetric::MutationInputsNoopChangedInputs);
		return false;
	}
	outRawView = record.rawView; outStats = record.stats;
	PerfAuditAdd(PerfAuditMetric::MutationInputsNoopRawViewReuses);
	if (validate)
	{
		PerfAuditScope validationAudit(PerfAuditMetric::MutationInputsNoopValidationNs);
		nri_scene::SceneView reference;
		nri_scene::PTMapWorldStats referenceStats;
		const bool valid = nri_scene::BuildLiveMapChunkSceneView(chunk, reference, &referenceStats);
		PerfAuditAdd(PerfAuditMetric::MutationInputsNoopValidationChecks);
		// Reuse the existing padding-free SceneView comparator. This diagnostic
		// compares the original producer before nudges, motion or policy stamps.
		if (!valid || !NRIEmissiveTailViewsEqual(outRawView, reference) || !SameStats(outStats, referenceStats))
		{
			m_quarantined = true;
			PerfAuditAdd(PerfAuditMetric::MutationInputsNoopValidationMismatches);
			PerfAuditAdd(PerfAuditMetric::MutationInputsNoopQuarantines);
			Printf("NRI mutation input no-op validation mismatch: chunk=%u quarantine=1\n", chunk.chunkIndex);
			current.valid = false;
			return false;
		}
	}
	return true;
}
void NRIRuntimeMutationViewInputs::Observe(const nri_scene::PTMapChunk& chunk, const nri_scene::PTMapChunkViewInputs& current,
	const nri_scene::SceneView& rawView, const nri_scene::PTMapWorldStats& stats)
{
	if (m_quarantined || !current.valid || chunk.chunkIndex >= m_records.size()) return;
	PerfAuditScope audit(PerfAuditMetric::MutationInputsNoopStoreNs);
	auto& record = m_records[chunk.chunkIndex];
	m_capacityBytes -= record.capacityBytes;
	record.inputs = current; record.rawView = rawView; record.stats = stats;
	record.capacityBytes = Capacity(record.rawView) + record.inputs.facts.capacity() * sizeof(uint64_t) +
		record.inputs.authored.walls.capacity() * sizeof(nri_scene::PTMapWallMutationSnapshot) +
		record.inputs.authored.sectionIndices.capacity() * sizeof(int32_t);
	m_capacityBytes += record.capacityBytes;
	PerfAuditAdd(PerfAuditMetric::MutationInputsNoopStores);
	PerfAuditSet(PerfAuditMetric::MutationInputsNoopCapacityBytes, m_capacityBytes + m_records.capacity() * sizeof(Record) +
		m_probe.facts.capacity() * sizeof(uint64_t) + m_probe.authored.walls.capacity() * sizeof(nri_scene::PTMapWallMutationSnapshot) +
		m_probe.authored.sectionIndices.capacity() * sizeof(int32_t));
}
