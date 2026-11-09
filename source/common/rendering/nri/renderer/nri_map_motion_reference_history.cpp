#include "nri_map_motion_reference_history.h"

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
		surface.temporal.materialUvReactive = 0.0f;
		for (nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			std::memcpy(vertex.prevPosition, vertex.position, sizeof(vertex.prevPosition));
		}
	}
}

bool NRIMapMotionReferenceHistory::PayloadEqual(
	const nri_scene::PTMapTemporalSurfacePayload& a,
	const nri_scene::PTMapTemporalSurfacePayload& b)
{
	if (a.occurrenceId != b.occurrenceId || a.topologyKey != b.topologyKey ||
		a.generation != b.generation || a.chunkIndex != b.chunkIndex ||
		a.materialUvAnchorValid != b.materialUvAnchorValid ||
		(a.materialUvAnchorValid &&
			(std::memcmp(a.materialUvAnchor, b.materialUvAnchor, sizeof(a.materialUvAnchor)) != 0 ||
				std::memcmp(a.materialUvTexelScale, b.materialUvTexelScale, sizeof(a.materialUvTexelScale)) != 0)) ||
		a.materialVerticalReferenceValid != b.materialVerticalReferenceValid ||
		(a.materialVerticalReferenceValid &&
			std::memcmp(&a.materialVerticalReference, &b.materialVerticalReference,
				sizeof(a.materialVerticalReference)) != 0) ||
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

NRIMapMotionReferenceHistory::DiagnosticRecord NRIMapMotionReferenceHistory::FindCommitted(uint64_t occurrenceId) const
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

bool NRIMapMotionReferenceHistory::SurfaceHasMotion(const nri_scene::SurfaceRef& surface)
{
	if (surface.temporal.materialUvReactive > 0.0f) return true;
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

void NRIMapMotionReferenceHistory::Reset(const char* reason)
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

void NRIMapMotionReferenceHistory::BeginMapEpoch(uint64_t mapEpoch)
{
	if (m_mapEpoch == mapEpoch)
	{
		return;
	}
	Reset("map-epoch-change");
	m_mapEpoch = mapEpoch;
	m_stats.mapEpoch = mapEpoch;
}

void NRIMapMotionReferenceHistory::ApplySurface(nri_scene::SurfaceRef& surface)
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

void NRIMapMotionReferenceHistory::ApplyCommitted(nri_scene::SceneView& sceneView, uint64_t mapEpoch)
{
	BeginMapEpoch(mapEpoch);
	ForEachSurface(sceneView, [&](nri_scene::SurfaceRef& surface) { ApplySurface(surface); });
}

void NRIMapMotionReferenceHistory::BeginStage(uint64_t proposedSerial, uint64_t mapEpoch)
{
	BeginMapEpoch(mapEpoch);
	m_staged.clear();
	m_stageSerial = proposedSerial;
	m_stageOpen = true;
	m_stageFinalized = false;
	m_stats.stagedSurfaceCount = 0;
}

bool NRIMapMotionReferenceHistory::StageSurface(const nri_scene::SurfaceRef& surface)
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

bool NRIMapMotionReferenceHistory::StagePublishedView(const nri_scene::SceneView& sceneView)
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

void NRIMapMotionReferenceHistory::FinalizeStage()
{
	if (m_stageOpen) m_stageFinalized = true;
}

bool NRIMapMotionReferenceHistory::CommitSubmitted(uint64_t serial)
{
	if (!m_stageOpen || !m_stageFinalized || serial != m_stageSerial)
	{
		DiscardStaged();
		return false;
	}
	struct ChunkSettleObservation
	{
		bool comparable = false;
		bool moved = false;
		bool publishedWithMotion = false;
	};
	std::map<uint32_t, ChunkSettleObservation> chunkObservations;
	for (const auto& entry : m_staged)
	{
		const Record& current = entry.second;
		const auto previous = m_committed.find(entry.first);
		if (current.payload.chunkIndex == UINT32_MAX) continue;
		ChunkSettleObservation& observation = chunkObservations[current.payload.chunkIndex];
		observation.publishedWithMotion |= current.publishedWithMotion;
		if (previous != m_committed.end() && previous->second.valid && current.valid)
		{
			observation.comparable = true;
			observation.moved |= !PayloadEqual(previous->second.payload, current.payload);
		}
	}
	for (const auto& entry : chunkObservations)
	{
		const uint32_t chunkIndex = entry.first;
		const ChunkSettleObservation& observation = entry.second;
		if (observation.moved)
		{
			m_settleChunks.insert(chunkIndex);
		}
		else if (observation.comparable && !observation.publishedWithMotion)
		{
			m_settleChunks.erase(chunkIndex);
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

void NRIMapMotionReferenceHistory::DiscardStaged()
{
	if (m_stageOpen) m_stats.discardCount++;
	m_staged.clear();
	m_stageSerial = 0;
	m_stageOpen = false;
	m_stageFinalized = false;
	m_stats.stagedSurfaceCount = 0;
}

bool NRIMapMotionReferenceHistory::NeedsSettle(uint32_t chunkIndex) const
{
	return m_settleChunks.find(chunkIndex) != m_settleChunks.end();
}
