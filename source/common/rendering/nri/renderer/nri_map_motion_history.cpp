#include "nri_map_motion_history.h"
#include "nri_map_motion_reference_history.h"
#include "nri_cvars.h"
#include "perf_audit.h"
#include "printf.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	template <typename View, typename Callback>
	void ForEachSurface(View& view, const Callback& callback)
	{
		for (auto& surface : view.opaqueWalls) callback(surface);
		for (auto& surface : view.opaqueFlats) callback(surface);
	}

	void SeedPreviousFromCurrent(nri_scene::SurfaceRef& surface)
	{
		surface.temporal.materialUvReactive = 0.0f;
		for (auto& vertex : surface.vertices)
			std::memcpy(vertex.prevPosition, vertex.position, sizeof(vertex.prevPosition));
	}

	bool AppliedMotionEqual(const nri_scene::SceneView& a, const nri_scene::SceneView& b)
	{
		const auto same = [](const auto& first, const auto& second)
		{
			if (first.size() != second.size()) return false;
			for (size_t i = 0; i < first.size(); ++i)
			{
				const auto& x = first[i];
				const auto& y = second[i];
				if (x.vertices.size() != y.vertices.size() ||
					x.temporal.correspondenceValid != y.temporal.correspondenceValid ||
					x.temporal.historyAge != y.temporal.historyAge || x.temporal.reason != y.temporal.reason ||
					std::memcmp(&x.temporal.materialUvReactive, &y.temporal.materialUvReactive, sizeof(float)) != 0)
					return false;
				for (size_t vertex = 0; vertex < x.vertices.size(); ++vertex)
					if (std::memcmp(x.vertices[vertex].prevPosition, y.vertices[vertex].prevPosition,
						sizeof(x.vertices[vertex].prevPosition)) != 0) return false;
			}
			return true;
		};
		return same(a.opaqueWalls, b.opaqueWalls) && same(a.opaqueFlats, b.opaqueFlats);
	}
}

NRIMapMotionHistory::NRIMapMotionHistory() = default;
NRIMapMotionHistory::~NRIMapMotionHistory() = default;

const NRIMapMotionHistory::Stats& NRIMapMotionHistory::GetStats() const
{
	return m_quarantined ? m_reference->GetStats() : m_stats;
}

const std::set<uint32_t>& NRIMapMotionHistory::GetSettleChunks() const
{
	return m_quarantined ? m_reference->GetSettleChunks() : m_settleChunks;
}

bool NRIMapMotionHistory::HasFinalizedStage() const
{
	return m_quarantined ? m_reference->HasFinalizedStage() : m_stageFinalized;
}

const char* NRIMapMotionHistory::GetLastResetReason() const
{
	return m_quarantined ? m_reference->GetLastResetReason() : m_lastResetReason.c_str();
}

NRIMapMotionHistory::DiagnosticRecord NRIMapMotionHistory::FindCommitted(uint64_t occurrenceId) const
{
	if (m_quarantined) return m_reference->FindCommitted(occurrenceId);
	DiagnosticRecord result;
	const auto found = m_records.find(occurrenceId);
	if (found == m_records.end() || found->second.committedSlot < 0 || !found->second.valid) return result;
	const Record& record = found->second;
	const auto& payload = record.snapshots[record.committedSlot].payload;
	result.valid = true;
	result.occurrenceId = occurrenceId;
	result.topologyKey = payload.topologyKey;
	result.generation = payload.generation;
	result.historyAge = record.historyAge;
	result.chunkIndex = payload.chunkIndex;
	result.provenance = payload.provenance;
	return result;
}

bool NRIMapMotionHistory::SurfaceHasMotion(const nri_scene::SurfaceRef& surface)
{
	if (surface.temporal.materialUvReactive > 0.0f) return true;
	for (const auto& vertex : surface.vertices)
		for (uint32_t axis = 0; axis < 3; ++axis)
			if (std::fabs(vertex.position[axis] - vertex.prevPosition[axis]) > 1.0e-5f) return true;
	return false;
}

void NRIMapMotionHistory::Reset(const char* reason)
{
	PerfAuditScope retireScope(PerfAuditMetric::MapMotionRetireNs);
	PerfAuditAdd(PerfAuditMetric::MapMotionRetiredRecords, m_records.size());
	RecordMap{}.swap(m_records);
	std::vector<Record*>{}.swap(m_committed);
	std::vector<Record*>{}.swap(m_staged);
	std::vector<ChunkObservation>{}.swap(m_chunkObservations);
	m_buildScratch = {};
	m_settleChunks.clear();
	m_mapEpoch = 0;
	m_stageSerial = 0;
	m_stageOpen = false;
	m_stageFinalized = false;
	m_stats = {};
	m_lastResetReason = reason != nullptr ? reason : "unspecified";
	m_payloadCapacityBytes = 0;
	m_inputCapacityBytes = 0;
	m_reference.reset();
	m_quarantined = false;
	ReportAuditStorage();
}

void NRIMapMotionHistory::ReportAuditStorage() const
{
	if (!PerfAuditActive()) return;
	PerfAuditSet(PerfAuditMetric::MapMotionCommittedRecords, m_committed.size());
	PerfAuditSet(PerfAuditMetric::MapMotionStagedRecords, m_staged.size());
	// Allocator/node headers and unordered-map bucket storage are not measured.
	PerfAuditSet(PerfAuditMetric::MapMotionRecordValueBytes, m_records.size() * sizeof(RecordMap::value_type));
	PerfAuditSet(PerfAuditMetric::MapMotionPayloadCapacityBytes, m_payloadCapacityBytes + m_buildScratch.PayloadCapacityBytes());
	PerfAuditSet(PerfAuditMetric::MapMotionInputCapacityBytes, m_inputCapacityBytes + m_buildScratch.InputCapacityBytes());
	PerfAuditSet(PerfAuditMetric::MapMotionScratchCapacityBytes,
		(m_committed.capacity() + m_staged.capacity()) * sizeof(Record*) +
		m_chunkObservations.capacity() * sizeof(ChunkObservation));
}

void NRIMapMotionHistory::BeginMapEpoch(uint64_t mapEpoch)
{
	if (m_mapEpoch == mapEpoch) return;
	Reset("map-epoch-change");
	m_mapEpoch = mapEpoch;
	m_stats.mapEpoch = mapEpoch;
}

void NRIMapMotionHistory::ApplySurface(nri_scene::SurfaceRef& surface)
{
	SeedPreviousFromCurrent(surface);
	surface.temporal.correspondenceValid = false;
	surface.temporal.historyAge = 0;
	surface.temporal.reason = nri_scene::MotionValidityReason::NoHistory;
	if (!surface.temporal.identityValid)
	{
		surface.temporal.reason = nri_scene::MotionValidityReason::UnsupportedSource;
		return;
	}
	const auto found = m_records.find(surface.temporal.occurrenceId);
	if (found == m_records.end() || found->second.committedSlot < 0 || !found->second.valid)
	{
		m_stats.seededCount++;
		return;
	}
	const Record& record = found->second;
	nri_scene::MotionValidityReason reason = nri_scene::MotionValidityReason::NoHistory;
	if (!nri_scene::ApplyMapTemporalSurfacePayload(record.snapshots[record.committedSlot].payload, surface, reason))
	{
		surface.temporal.reason = reason;
		if (reason == nri_scene::MotionValidityReason::TopologyMismatch) m_stats.topologyRejectCount++;
		return;
	}
	surface.temporal.correspondenceValid = true;
	surface.temporal.reason = nri_scene::MotionValidityReason::Valid;
	surface.temporal.historyAge = record.historyAge;
	m_stats.appliedCount++;
}

void NRIMapMotionHistory::ApplyCommitted(nri_scene::SceneView& sceneView, uint64_t mapEpoch)
{
	PerfAuditScope applyScope(PerfAuditMetric::MapMotionApplyNs);
	BeginMapEpoch(mapEpoch);
	EnsureReference();
	if (m_quarantined) { m_reference->ApplyCommitted(sceneView, mapEpoch); return; }
	nri_scene::SceneView referenceView;
	if (m_reference)
	{
		PerfAuditScope referenceScope(PerfAuditMetric::MapMotionReferenceNs);
		referenceView = sceneView;
		m_reference->ApplyCommitted(referenceView, mapEpoch);
	}
	ForEachSurface(sceneView, [&](nri_scene::SurfaceRef& surface) { ApplySurface(surface); });
	if (m_reference)
	{
		PerfAuditScope referenceScope(PerfAuditMetric::MapMotionReferenceNs);
		PerfAuditAdd(PerfAuditMetric::MapMotionValidationChecks);
		if (!AppliedMotionEqual(sceneView, referenceView))
		{
			sceneView = std::move(referenceView);
			Quarantine("apply");
		}
	}
}

void NRIMapMotionHistory::EraseRecord(Record& record)
{
	for (const auto& snapshot : record.snapshots)
	{
		m_payloadCapacityBytes -= snapshot.PayloadCapacityBytes();
		m_inputCapacityBytes -= snapshot.InputCapacityBytes();
	}
	const uint64_t occurrenceId = record.occurrenceId;
	m_records.erase(occurrenceId);
	PerfAuditAdd(PerfAuditMetric::MapMotionRetiredRecords);
}

void NRIMapMotionHistory::ClearStage(bool countDiscard)
{
	PerfAuditScope retireScope(PerfAuditMetric::MapMotionRetireNs);
	if (countDiscard && m_stageOpen) m_stats.discardCount++;
	for (Record* record : m_staged)
	{
		if (record->committedSlot < 0) EraseRecord(*record);
		else record->stagedSlot = -1;
	}
	m_staged.clear();
	m_stageSerial = 0;
	m_stageOpen = false;
	m_stageFinalized = false;
	m_stats.stagedSurfaceCount = 0;
}

void NRIMapMotionHistory::BeginStage(uint64_t proposedSerial, uint64_t mapEpoch)
{
	BeginMapEpoch(mapEpoch);
	EnsureReference();
	if (m_reference)
	{
		PerfAuditScope referenceScope(PerfAuditMetric::MapMotionReferenceNs);
		m_reference->BeginStage(proposedSerial, mapEpoch);
	}
	if (m_quarantined) return;
	ClearStage(false);
	m_stageSerial = proposedSerial;
	m_stageOpen = true;
	m_stageFinalized = false;
}

bool NRIMapMotionHistory::StageSurface(const nri_scene::SurfaceRef& surface)
{
	if (!surface.temporal.identityValid) return true;
	const auto found = m_records.find(surface.temporal.occurrenceId);
	Record* record = found != m_records.end() ? &found->second : nullptr;
	int candidateSlot = -1;
	uint64_t comparedBytes = 0;
	if (record != nullptr)
	{
		// The current proposal is most useful for duplicate publications, then
		// the committed payload, then the bounded spare (e.g. recurring motion).
		const int candidates[] = { record->stagedSlot, record->committedSlot, 0, 1 };
		unsigned tested = 0;
		for (int slot : candidates)
		{
			if (slot < 0 || (tested & (1u << slot))) continue;
			tested |= 1u << slot;
			if (record->snapshots[slot].Matches(surface, comparedBytes)) { candidateSlot = slot; break; }
		}
	}
	PerfAuditAdd(PerfAuditMetric::MapMotionSnapshotComparedBytes, comparedBytes);
	if (candidateSlot < 0)
	{
		PerfAuditScope payloadScope(PerfAuditMetric::MapMotionPayloadBuildNs);
		PerfAuditAdd(PerfAuditMetric::MapMotionPayloadBuilds);
		PerfAuditAdd(PerfAuditMetric::MapMotionInputVertices, surface.vertices.size());
		nri_scene::MotionValidityReason reason;
		const bool valid = m_buildScratch.Build(surface, reason);
		PerfAuditAdd(PerfAuditMetric::MapMotionBuiltCorners, m_buildScratch.payload.corners.size());
		if (!valid) return false;
	}
	else PerfAuditAdd(PerfAuditMetric::MapMotionPayloadReuses);
	const auto& candidate = candidateSlot >= 0 ? record->snapshots[candidateSlot].payload : m_buildScratch.payload;
	if (record != nullptr && record->stagedSlot >= 0)
	{
		if (!NRIMapMotionPayloadEqual(record->snapshots[record->stagedSlot].payload, candidate))
		{
			record->stagedValid = false;
			m_stats.duplicatePublicationCount++;
			return false;
		}
		record->publishedWithMotion |= SurfaceHasMotion(surface);
		return true;
	}
	if (record == nullptr)
	{
		auto inserted = m_records.try_emplace(candidate.occurrenceId);
		record = &inserted.first->second;
		record->occurrenceId = candidate.occurrenceId;
		PerfAuditAdd(PerfAuditMetric::MapMotionRecordInsertions);
	}
	else PerfAuditAdd(PerfAuditMetric::MapMotionOccurrenceReuses);
	if (candidateSlot < 0)
	{
		candidateSlot = record->committedSlot < 0 ? 0 : 1 - record->committedSlot;
		auto& destination = record->snapshots[candidateSlot];
		m_payloadCapacityBytes -= destination.PayloadCapacityBytes();
		m_inputCapacityBytes -= destination.InputCapacityBytes();
		std::swap(destination, m_buildScratch);
		m_payloadCapacityBytes += destination.PayloadCapacityBytes();
		m_inputCapacityBytes += destination.InputCapacityBytes();
	}
	record->stagedSlot = candidateSlot;
	record->stagedAge = record->committedSlot >= 0 ? record->historyAge + 1u : 1u;
	record->stagedValid = true;
	record->publishedWithMotion = SurfaceHasMotion(surface);
	m_staged.push_back(record);
	return true;
}

bool NRIMapMotionHistory::StagePublishedView(const nri_scene::SceneView& sceneView)
{
	EnsureReference();
	bool referenceValid = false;
	if (m_reference)
	{
		PerfAuditScope referenceScope(PerfAuditMetric::MapMotionReferenceNs);
		referenceValid = m_reference->StagePublishedView(sceneView);
	}
	if (m_quarantined) return referenceValid;
	bool valid = m_stageOpen;
	if (m_stageOpen)
		ForEachSurface(sceneView, [&](const nri_scene::SurfaceRef& surface) { valid &= StageSurface(surface); });
	m_stats.stagedSurfaceCount = (uint32_t)m_staged.size();
	if (m_reference)
	{
		PerfAuditAdd(PerfAuditMetric::MapMotionValidationChecks);
		if (valid != referenceValid) { Quarantine("stage-result"); return referenceValid; }
	}
	return valid;
}

void NRIMapMotionHistory::FinalizeStage()
{
	EnsureReference();
	if (m_reference) m_reference->FinalizeStage();
	if (m_quarantined) return;
	if (m_stageOpen) m_stageFinalized = true;
	ReportAuditStorage();
	CheckReference("finalize");
}

bool NRIMapMotionHistory::CommitOptimized(uint64_t serial)
{
	if (!m_stageOpen || !m_stageFinalized || serial != m_stageSerial)
	{
		ClearStage(true);
		return false;
	}
	m_chunkObservations.clear();
	for (const Record* record : m_staged)
	{
		const auto& current = record->snapshots[record->stagedSlot].payload;
		if (current.chunkIndex == UINT32_MAX) continue;
		ChunkObservation observation;
		observation.chunkIndex = current.chunkIndex;
		observation.publishedWithMotion = record->publishedWithMotion;
		observation.comparable = record->committedSlot >= 0 && record->valid && record->stagedValid;
		observation.moved = observation.comparable && record->committedSlot != record->stagedSlot &&
			!NRIMapMotionPayloadEqual(record->snapshots[record->committedSlot].payload, current);
		m_chunkObservations.push_back(observation);
	}
	std::sort(m_chunkObservations.begin(), m_chunkObservations.end(),
		[](const ChunkObservation& a, const ChunkObservation& b) { return a.chunkIndex < b.chunkIndex; });
	for (size_t first = 0; first < m_chunkObservations.size();)
	{
		ChunkObservation observation = m_chunkObservations[first++];
		while (first < m_chunkObservations.size() && m_chunkObservations[first].chunkIndex == observation.chunkIndex)
		{
			observation.comparable |= m_chunkObservations[first].comparable;
			observation.moved |= m_chunkObservations[first].moved;
			observation.publishedWithMotion |= m_chunkObservations[first].publishedWithMotion;
			++first;
		}
		if (observation.moved) m_settleChunks.insert(observation.chunkIndex);
		else if (observation.comparable && !observation.publishedWithMotion) m_settleChunks.erase(observation.chunkIndex);
	}
	{
		PerfAuditScope retireScope(PerfAuditMetric::MapMotionRetireNs);
		for (Record* record : m_committed)
			if (record->stagedSlot < 0) EraseRecord(*record);
	}
	for (Record* record : m_staged)
	{
		record->committedSlot = record->stagedSlot;
		record->stagedSlot = -1;
		record->historyAge = record->stagedAge;
		record->valid = record->stagedValid;
	}
	m_committed.swap(m_staged);
	m_staged.clear();
	m_stageOpen = false;
	m_stageFinalized = false;
	m_stageSerial = 0;
	m_stats.mainTemporalSerial = serial;
	m_stats.committedSurfaceCount = (uint32_t)m_committed.size();
	m_stats.stagedSurfaceCount = 0;
	m_stats.settleChunkCount = (uint32_t)m_settleChunks.size();
	m_stats.commitCount++;
	return true;
}

bool NRIMapMotionHistory::CommitSubmitted(uint64_t serial)
{
	PerfAuditScope commitScope(PerfAuditMetric::MapMotionCommitNs);
	EnsureReference();
	bool referenceValid = false;
	if (m_reference)
	{
		PerfAuditScope referenceScope(PerfAuditMetric::MapMotionReferenceNs);
		referenceValid = m_reference->CommitSubmitted(serial);
	}
	if (m_quarantined) return referenceValid;
	const bool valid = CommitOptimized(serial);
	ReportAuditStorage();
	if (m_reference && valid != referenceValid) { Quarantine("commit-result"); return referenceValid; }
	CheckReference("commit");
	return m_quarantined ? referenceValid : valid;
}

void NRIMapMotionHistory::DiscardStaged()
{
	EnsureReference();
	if (m_reference)
	{
		PerfAuditScope referenceScope(PerfAuditMetric::MapMotionReferenceNs);
		m_reference->DiscardStaged();
	}
	if (m_quarantined) return;
	ClearStage(true);
	ReportAuditStorage();
	CheckReference("discard");
}

bool NRIMapMotionHistory::NeedsSettle(uint32_t chunkIndex) const
{
	return GetSettleChunks().find(chunkIndex) != GetSettleChunks().end();
}

void NRIMapMotionHistory::EnsureReference()
{
	if (m_quarantined) return;
	if (!(bool)nri_ptmapmotionvalidate) { m_reference.reset(); return; }
	if (m_reference) return;
	PerfAuditScope referenceScope(PerfAuditMetric::MapMotionReferenceNs);
	m_reference = std::make_unique<NRIMapMotionReferenceHistory>();
	// A diagnostic enable in a running map must begin from the actual submitted
	// history and any open proposal, never an empty synthetic previous frame.
	m_reference->m_mapEpoch = m_mapEpoch;
	m_reference->m_stageSerial = m_stageSerial;
	m_reference->m_stageOpen = m_stageOpen;
	m_reference->m_stageFinalized = m_stageFinalized;
	m_reference->m_stats = m_stats;
	m_reference->m_lastResetReason = m_lastResetReason;
	m_reference->m_settleChunks = m_settleChunks;
	for (const Record* record : m_committed)
	{
		NRIMapMotionReferenceHistory::Record copy;
		copy.payload = record->snapshots[record->committedSlot].payload;
		copy.historyAge = record->historyAge;
		copy.valid = record->valid;
		copy.publishedWithMotion = record->publishedWithMotion;
		m_reference->m_committed.emplace(record->occurrenceId, std::move(copy));
	}
	for (const Record* record : m_staged)
	{
		NRIMapMotionReferenceHistory::Record copy;
		copy.payload = record->snapshots[record->stagedSlot].payload;
		copy.historyAge = record->stagedAge;
		copy.valid = record->stagedValid;
		copy.publishedWithMotion = record->publishedWithMotion;
		m_reference->m_staged.emplace(record->occurrenceId, std::move(copy));
	}
}

void NRIMapMotionHistory::Quarantine(const char* operation)
{
	if (m_quarantined) return;
	m_quarantined = true;
	PerfAuditAdd(PerfAuditMetric::MapMotionValidationMismatches);
	PerfAuditAdd(PerfAuditMetric::MapMotionQuarantines);
	Printf("NRI PT map motion validation: operation=%s serial=%llu mismatch=1 action=reference-until-reset\n",
		operation, (unsigned long long)m_stats.mainTemporalSerial);
}

bool NRIMapMotionHistory::CheckReference(const char* operation)
{
	if (!m_reference || m_quarantined) return !m_quarantined;
	PerfAuditScope referenceScope(PerfAuditMetric::MapMotionReferenceNs);
	PerfAuditAdd(PerfAuditMetric::MapMotionValidationChecks);
	bool equal = m_committed.size() == m_reference->m_committed.size() &&
		m_staged.size() == m_reference->m_staged.size() && m_settleChunks == m_reference->m_settleChunks &&
		m_mapEpoch == m_reference->m_mapEpoch && m_stageSerial == m_reference->m_stageSerial &&
		m_stageOpen == m_reference->m_stageOpen && m_stageFinalized == m_reference->m_stageFinalized;
	const Stats& a = m_stats;
	const Stats& b = m_reference->m_stats;
	equal &= a.mainTemporalSerial == b.mainTemporalSerial && a.mapEpoch == b.mapEpoch &&
		a.committedSurfaceCount == b.committedSurfaceCount && a.stagedSurfaceCount == b.stagedSurfaceCount &&
		a.appliedCount == b.appliedCount && a.seededCount == b.seededCount &&
		a.topologyRejectCount == b.topologyRejectCount && a.duplicatePublicationCount == b.duplicatePublicationCount &&
		a.settleChunkCount == b.settleChunkCount && a.commitCount == b.commitCount && a.discardCount == b.discardCount;
	for (const Record* record : m_committed)
	{
		const auto found = m_reference->m_committed.find(record->occurrenceId);
		if (found == m_reference->m_committed.end() || found->second.historyAge != record->historyAge ||
			found->second.valid != record->valid ||
			!NRIMapMotionPayloadExactEqual(record->snapshots[record->committedSlot].payload, found->second.payload))
		{ equal = false; break; }
	}
	for (const Record* record : m_staged)
	{
		const auto found = m_reference->m_staged.find(record->occurrenceId);
		if (found == m_reference->m_staged.end() || found->second.historyAge != record->stagedAge ||
			found->second.valid != record->stagedValid || found->second.publishedWithMotion != record->publishedWithMotion ||
			!NRIMapMotionPayloadExactEqual(record->snapshots[record->stagedSlot].payload, found->second.payload))
		{ equal = false; break; }
	}
	if (!equal) Quarantine(operation);
	return equal;
}

bool RunNRIMapMotionHistorySelfTests(std::string* failureReason)
{
	auto fail = [&](const char* reason)
	{
		if (failureReason != nullptr) *failureReason = reason;
		return false;
	};
	std::string correspondenceFailure;
	if (!nri_scene::RunNRIMapMotionCorrespondenceSelfTests(&correspondenceFailure))
	{
		if (failureReason != nullptr) *failureReason = correspondenceFailure;
		return false;
	}
	nri_scene::PTMapSurface mapSurface;
	mapSurface.kind = nri_scene::PTMapSurfaceKind::WallMiddle;
	mapSurface.key = { 11u, 2u };
	mapSurface.chunkIndex = 4u;
	mapSurface.surface.provenance.sourceType = nri_scene::SurfaceSourceType::MapWallBand;
	mapSurface.surface.provenance.mapChunkIndex = 4;
	mapSurface.surface.provenance.wallIndex = 11;
	for (uint64_t i = 0; i < 4; ++i)
	{
		nri_scene::CapturedVertex vertex;
		vertex.position[0] = (float)i;
		vertex.temporalCornerKey = i;
		mapSurface.surface.vertices.push_back(vertex);
	}
	nri_scene::InitializeMapTemporalSurface(mapSurface, 3u, mapSurface.surface);
	nri_scene::SceneView seed;
	seed.opaqueWalls.push_back(mapSurface.surface);
	NRIMapMotionHistory history;
	history.ApplyCommitted(seed, 3u);
	if (seed.opaqueWalls[0].temporal.correspondenceValid) return fail("seed was valid");
	history.BeginStage(1u, 3u);
	history.StagePublishedView(seed);
	history.FinalizeStage();
	if (!history.CommitSubmitted(1u)) return fail("seed commit failed");
	nri_scene::SceneView moved = seed;
	for (auto& vertex : moved.opaqueWalls[0].vertices) vertex.position[1] += 2.0f;
	history.ApplyCommitted(moved, 3u);
	if (!moved.opaqueWalls[0].temporal.correspondenceValid || moved.opaqueWalls[0].vertices[0].prevPosition[1] != 0.0f)
		return fail("move correspondence failed");
	history.BeginStage(2u, 3u);
	history.StagePublishedView(moved);
	history.FinalizeStage();
	if (!history.CommitSubmitted(2u) || !history.NeedsSettle(4u)) return fail("settle was not armed");
	nri_scene::SceneView unchanged = moved;
	history.ApplyCommitted(unchanged, 3u);
	for (const auto& vertex : unchanged.opaqueWalls[0].vertices)
		for (uint32_t axis = 0; axis < 3u; ++axis)
			if (std::fabs(vertex.position[axis] - vertex.prevPosition[axis]) > 1.0e-5f)
				return fail("settle frame retained motion");
	history.BeginStage(3u, 3u);
	history.StagePublishedView(unchanged);
	history.FinalizeStage();
	if (!history.CommitSubmitted(3u) || history.NeedsSettle(4u)) return fail("settle was not cleared");
	nri_scene::SceneView discarded = unchanged;
	for (auto& vertex : discarded.opaqueWalls[0].vertices) vertex.position[1] += 5.0f;
	history.BeginStage(4u, 3u);
	history.StagePublishedView(discarded);
	history.FinalizeStage();
	history.DiscardStaged();
	nri_scene::SceneView afterDiscard = unchanged;
	history.ApplyCommitted(afterDiscard, 3u);
	if (afterDiscard.opaqueWalls[0].vertices[0].prevPosition[1] != unchanged.opaqueWalls[0].vertices[0].position[1])
		return fail("discarded publication advanced history");
	history.BeginStage(4u, 3u);
	if (!history.StagePublishedView(unchanged) || !history.StagePublishedView(unchanged))
		return fail("identical duplicate publication rejected");
	history.FinalizeStage();
	if (!history.CommitSubmitted(4u)) return fail("duplicate publication commit failed");
	auto validateMixedChunkSettle = [&](bool movingSurfaceSortsFirst)
	{
		nri_scene::SurfaceRef movingSurface = mapSurface.surface;
		nri_scene::SurfaceRef stationarySurface = mapSurface.surface;
		movingSurface.temporal.occurrenceId = movingSurfaceSortsFirst ? 100u : 200u;
		stationarySurface.temporal.occurrenceId = movingSurfaceSortsFirst ? 200u : 100u;
		stationarySurface.temporal.topologyKey ^= 0x9e3779b97f4a7c15ull;
		for (auto& vertex : stationarySurface.vertices) vertex.position[0] += 32.0f;
		nri_scene::SceneView mixedSeed;
		mixedSeed.opaqueWalls.push_back(movingSurface);
		mixedSeed.opaqueWalls.push_back(stationarySurface);
		NRIMapMotionHistory mixedHistory;
		mixedHistory.ApplyCommitted(mixedSeed, 3u);
		mixedHistory.BeginStage(1u, 3u);
		if (!mixedHistory.StagePublishedView(mixedSeed)) return false;
		mixedHistory.FinalizeStage();
		if (!mixedHistory.CommitSubmitted(1u)) return false;

		nri_scene::SceneView mixedMoved = mixedSeed;
		for (auto& vertex : mixedMoved.opaqueWalls[0].vertices) vertex.position[1] += 2.0f;
		mixedHistory.ApplyCommitted(mixedMoved, 3u);
		mixedHistory.BeginStage(2u, 3u);
		if (!mixedHistory.StagePublishedView(mixedMoved)) return false;
		mixedHistory.FinalizeStage();
		if (!mixedHistory.CommitSubmitted(2u) || !mixedHistory.NeedsSettle(4u)) return false;

		// Republishing the moved payload without the required resident refresh
		// must preserve settlement even though current positions are unchanged.
		mixedHistory.BeginStage(3u, 3u);
		if (!mixedHistory.StagePublishedView(mixedMoved)) return false;
		mixedHistory.FinalizeStage();
		if (!mixedHistory.CommitSubmitted(3u) || !mixedHistory.NeedsSettle(4u)) return false;

		nri_scene::SceneView mixedSettled = mixedMoved;
		mixedHistory.ApplyCommitted(mixedSettled, 3u);
		mixedHistory.BeginStage(4u, 3u);
		if (!mixedHistory.StagePublishedView(mixedSettled)) return false;
		mixedHistory.FinalizeStage();
		return mixedHistory.CommitSubmitted(4u) && !mixedHistory.NeedsSettle(4u);
	};
	if (!validateMixedChunkSettle(true) || !validateMixedChunkSettle(false))
		return fail("mixed-surface chunk settlement was order-dependent");

	nri_scene::SceneView panSeed;
	panSeed.opaqueFlats.push_back(mapSurface.surface);
	auto& panSurface = panSeed.opaqueFlats[0];
	panSurface.provenance.sourceType = nri_scene::SurfaceSourceType::MapFloorSection;
	panSurface.temporal.materialUvAnchorValid = true;
	panSurface.temporal.materialUvTexelScale[0] = 64.0f;
	panSurface.temporal.materialUvTexelScale[1] = 64.0f;
	NRIMapMotionHistory panHistory;
	const auto commitPan = [&](const nri_scene::SceneView& view, uint64_t serial)
	{
		panHistory.BeginStage(serial, 3u);
		if (!panHistory.StagePublishedView(view)) return false;
		panHistory.FinalizeStage();
		return panHistory.CommitSubmitted(serial);
	};
	panHistory.ApplyCommitted(panSeed, 3u);
	if (!commitPan(panSeed, 1u)) return fail("flat pan seed commit failed");
	nri_scene::SceneView panMoved = panSeed;
	panMoved.opaqueFlats[0].temporal.materialUvAnchor[0] = 1.0f / 128.0f;
	panHistory.ApplyCommitted(panMoved, 3u);
	if (panMoved.opaqueFlats[0].temporal.materialUvReactive != 0.5f ||
		!commitPan(panMoved, 2u) || !panHistory.NeedsSettle(4u))
		return fail("flat pan did not publish reactivity and request settlement");
	if (!commitPan(panMoved, 3u) || !panHistory.NeedsSettle(4u))
		return fail("stale published pan reactivity cleared settlement");
	nri_scene::SceneView panSettled = panMoved;
	panHistory.ApplyCommitted(panSettled, 3u);
	if (panSettled.opaqueFlats[0].temporal.materialUvReactive != 0.0f ||
		!commitPan(panSettled, 4u) || panHistory.NeedsSettle(4u))
		return fail("paused flat did not clear published pan reactivity");
	nri_scene::SceneView panDiscarded = panSettled;
	panDiscarded.opaqueFlats[0].temporal.materialUvAnchor[0] += 1.0f / 64.0f;
	panHistory.BeginStage(5u, 3u);
	if (!panHistory.StagePublishedView(panDiscarded)) return fail("flat pan staging failed");
	panHistory.FinalizeStage();
	panHistory.DiscardStaged();
	panHistory.ApplyCommitted(panSettled, 3u);
	if (panSettled.opaqueFlats[0].temporal.materialUvReactive != 0.0f)
		return fail("discarded flat pan advanced submitted history");
	panHistory.Reset("pan-test-reset");
	panMoved.opaqueFlats[0].temporal.materialUvReactive = 1.0f;
	panHistory.ApplyCommitted(panMoved, 3u);
	if (panMoved.opaqueFlats[0].temporal.correspondenceValid ||
		panMoved.opaqueFlats[0].temporal.materialUvReactive != 0.0f)
		return fail("reset retained flat pan reactivity");
	if (failureReason != nullptr) failureReason->clear();
	return true;
}
