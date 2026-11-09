#pragma once

// Independent original tree-based history used only by opt-in validation/fallback.

#include "nri_map_motion_history.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>

class NRIMapMotionReferenceHistory
{
public:
	using Stats = NRIMapMotionHistory::Stats;
	using DiagnosticRecord = NRIMapMotionHistory::DiagnosticRecord;

	void Reset(const char* reason = nullptr);
	void BeginMapEpoch(uint64_t mapEpoch);
	void ApplyCommitted(nri_scene::SceneView& sceneView, uint64_t mapEpoch);
	void BeginStage(uint64_t proposedSerial, uint64_t mapEpoch);
	bool StagePublishedView(const nri_scene::SceneView& sceneView);
	void FinalizeStage();
	bool CommitSubmitted(uint64_t serial);
	void DiscardStaged();
	bool NeedsSettle(uint32_t chunkIndex) const;
	const std::set<uint32_t>& GetSettleChunks() const { return m_settleChunks; }
	bool HasFinalizedStage() const { return m_stageFinalized; }
	const Stats& GetStats() const { return m_stats; }
	DiagnosticRecord FindCommitted(uint64_t occurrenceId) const;
	const char* GetLastResetReason() const { return m_lastResetReason.c_str(); }

private:
	friend class NRIMapMotionHistory;
	struct Record
	{
		nri_scene::PTMapTemporalSurfacePayload payload;
		uint32_t historyAge = 0;
		bool valid = true;
		bool publishedWithMotion = false;
	};

	using RecordMap = std::map<uint64_t, Record>;
	static bool PayloadEqual(
		const nri_scene::PTMapTemporalSurfacePayload& a,
		const nri_scene::PTMapTemporalSurfacePayload& b);
	static bool SurfaceHasMotion(const nri_scene::SurfaceRef& surface);
	void ApplySurface(nri_scene::SurfaceRef& surface);
	bool StageSurface(const nri_scene::SurfaceRef& surface);

	RecordMap m_committed;
	RecordMap m_staged;
	std::set<uint32_t> m_settleChunks;
	uint64_t m_mapEpoch = 0;
	uint64_t m_stageSerial = 0;
	bool m_stageOpen = false;
	bool m_stageFinalized = false;
	Stats m_stats;
	std::string m_lastResetReason;
};
