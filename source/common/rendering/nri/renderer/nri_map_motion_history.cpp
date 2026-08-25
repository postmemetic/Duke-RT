#include "nri_map_motion_history.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	template <typename Callback>
	void ForEachSurface(nri_scene::SceneView& view, const Callback& callback)
	{
		for (nri_scene::SurfaceRef& surface : view.opaqueWalls) callback(surface);
		for (nri_scene::SurfaceRef& surface : view.opaqueFlats) callback(surface);
	}

	template <typename Callback>
	void ForEachSurface(const nri_scene::SceneView& view, const Callback& callback)
	{
		for (const nri_scene::SurfaceRef& surface : view.opaqueWalls) callback(surface);
		for (const nri_scene::SurfaceRef& surface : view.opaqueFlats) callback(surface);
	}

	void SeedPreviousFromCurrent(nri_scene::SurfaceRef& surface)
	{
		for (nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			std::memcpy(vertex.prevPosition, vertex.position, sizeof(vertex.prevPosition));
		}
	}
}

bool NRIMapMotionHistory::PayloadEqual(
	const nri_scene::PTMapTemporalSurfacePayload& a,
	const nri_scene::PTMapTemporalSurfacePayload& b)
{
	if (a.occurrenceId != b.occurrenceId || a.topologyKey != b.topologyKey ||
		a.generation != b.generation || a.chunkIndex != b.chunkIndex ||
		a.corners.size() != b.corners.size())
	{
		return false;
	}
	for (size_t i = 0; i < a.corners.size(); ++i)
	{
		if (a.corners[i].key != b.corners[i].key ||
			std::memcmp(a.corners[i].position, b.corners[i].position, sizeof(a.corners[i].position)) != 0)
		{
			return false;
		}
	}
	return true;
}

NRIMapMotionHistory::DiagnosticRecord NRIMapMotionHistory::FindCommitted(uint64_t occurrenceId) const
{
	DiagnosticRecord result;
	const auto found = m_committed.find(occurrenceId);
	if (found == m_committed.end() || !found->second.valid)
		return result;
	result.valid = true;
	result.occurrenceId = occurrenceId;
	result.topologyKey = found->second.payload.topologyKey;
	result.generation = found->second.payload.generation;
	result.historyAge = found->second.historyAge;
	result.chunkIndex = found->second.payload.chunkIndex;
	result.provenance = found->second.payload.provenance;
	return result;
}

bool NRIMapMotionHistory::SurfaceHasMotion(const nri_scene::SurfaceRef& surface)
{
	for (const nri_scene::CapturedVertex& vertex : surface.vertices)
	{
		for (uint32_t axis = 0; axis < 3; ++axis)
		{
			if (std::fabs(vertex.position[axis] - vertex.prevPosition[axis]) > 1.0e-5f)
			{
				return true;
			}
		}
	}
	return false;
}

void NRIMapMotionHistory::Reset(const char* reason)
{
	m_committed.clear();
	m_staged.clear();
	m_settleChunks.clear();
	m_mapEpoch = 0;
	m_stageSerial = 0;
	m_stageOpen = false;
	m_stageFinalized = false;
	m_stats = {};
	m_lastResetReason = reason != nullptr ? reason : "unspecified";
}

void NRIMapMotionHistory::BeginMapEpoch(uint64_t mapEpoch)
{
	if (m_mapEpoch == mapEpoch)
	{
		return;
	}
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
	const auto found = m_committed.find(surface.temporal.occurrenceId);
	if (found == m_committed.end() || !found->second.valid)
	{
		m_stats.seededCount++;
		return;
	}
	nri_scene::MotionValidityReason reason = nri_scene::MotionValidityReason::NoHistory;
	if (!nri_scene::ApplyMapTemporalSurfacePayload(found->second.payload, surface, reason))
	{
		surface.temporal.reason = reason;
		if (reason == nri_scene::MotionValidityReason::TopologyMismatch) m_stats.topologyRejectCount++;
		return;
	}
	surface.temporal.correspondenceValid = true;
	surface.temporal.reason = nri_scene::MotionValidityReason::Valid;
	surface.temporal.historyAge = found->second.historyAge;
	m_stats.appliedCount++;
}

void NRIMapMotionHistory::ApplyCommitted(nri_scene::SceneView& sceneView, uint64_t mapEpoch)
{
	BeginMapEpoch(mapEpoch);
	ForEachSurface(sceneView, [&](nri_scene::SurfaceRef& surface) { ApplySurface(surface); });
}

void NRIMapMotionHistory::BeginStage(uint64_t proposedSerial, uint64_t mapEpoch)
{
	BeginMapEpoch(mapEpoch);
	m_staged.clear();
	m_stageSerial = proposedSerial;
	m_stageOpen = true;
	m_stageFinalized = false;
	m_stats.stagedSurfaceCount = 0;
}

bool NRIMapMotionHistory::StageSurface(const nri_scene::SurfaceRef& surface)
{
	if (!surface.temporal.identityValid)
	{
		return true;
	}
	nri_scene::PTMapTemporalSurfacePayload payload;
	nri_scene::MotionValidityReason reason;
	if (!nri_scene::BuildMapTemporalSurfacePayload(surface, payload, reason))
	{
		return false;
	}
	const auto staged = m_staged.find(payload.occurrenceId);
	if (staged != m_staged.end())
	{
		if (!PayloadEqual(staged->second.payload, payload))
		{
			staged->second.valid = false;
			m_stats.duplicatePublicationCount++;
			return false;
		}
		staged->second.publishedWithMotion |= SurfaceHasMotion(surface);
		return true;
	}
	Record record;
	record.payload = std::move(payload);
	record.publishedWithMotion = SurfaceHasMotion(surface);
	const auto committed = m_committed.find(record.payload.occurrenceId);
	record.historyAge = committed != m_committed.end() ? committed->second.historyAge + 1u : 1u;
	m_staged.emplace(record.payload.occurrenceId, std::move(record));
	return true;
}

bool NRIMapMotionHistory::StagePublishedView(const nri_scene::SceneView& sceneView)
{
	if (!m_stageOpen)
	{
		return false;
	}
	bool valid = true;
	ForEachSurface(sceneView, [&](const nri_scene::SurfaceRef& surface)
	{
		valid &= StageSurface(surface);
	});
	m_stats.stagedSurfaceCount = (uint32_t)m_staged.size();
	return valid;
}

void NRIMapMotionHistory::FinalizeStage()
{
	if (m_stageOpen) m_stageFinalized = true;
}

bool NRIMapMotionHistory::CommitSubmitted(uint64_t serial)
{
	if (!m_stageOpen || !m_stageFinalized || serial != m_stageSerial)
	{
		DiscardStaged();
		return false;
	}
	for (const auto& entry : m_staged)
	{
		const Record& current = entry.second;
		const auto previous = m_committed.find(entry.first);
		if (current.payload.chunkIndex == UINT32_MAX) continue;
		if (previous != m_committed.end() && previous->second.valid && current.valid)
		{
			const bool moved = !PayloadEqual(previous->second.payload, current.payload);
			if (moved)
			{
				m_settleChunks.insert(current.payload.chunkIndex);
			}
			else if (!current.publishedWithMotion)
			{
				m_settleChunks.erase(current.payload.chunkIndex);
			}
		}
	}
	m_committed = std::move(m_staged);
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

void NRIMapMotionHistory::DiscardStaged()
{
	if (m_stageOpen) m_stats.discardCount++;
	m_staged.clear();
	m_stageSerial = 0;
	m_stageOpen = false;
	m_stageFinalized = false;
	m_stats.stagedSurfaceCount = 0;
}

bool NRIMapMotionHistory::NeedsSettle(uint32_t chunkIndex) const
{
	return m_settleChunks.find(chunkIndex) != m_settleChunks.end();
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
	if (failureReason != nullptr) failureReason->clear();
	return true;
}
