#pragma once

#include "../scene/nri_map_motion_correspondence.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>

class NRIMapMotionHistory
{
public:
	struct Stats
	{
		uint64_t mainTemporalSerial = 0;
		uint64_t mapEpoch = 0;
		uint32_t committedSurfaceCount = 0;
		uint32_t stagedSurfaceCount = 0;
		uint32_t appliedCount = 0;
		uint32_t seededCount = 0;
		uint32_t topologyRejectCount = 0;
		uint32_t duplicatePublicationCount = 0;
		uint32_t settleChunkCount = 0;
		uint32_t commitCount = 0;
		uint32_t discardCount = 0;
	};
	struct DiagnosticRecord
	{
		bool valid = false;
		uint64_t occurrenceId = 0;
		uint64_t topologyKey = 0;
		uint32_t generation = 0;
		uint32_t historyAge = 0;
		uint32_t chunkIndex = UINT32_MAX;
		nri_scene::SurfaceProvenance provenance;
	};

	void Reset(const char* reason = nullptr);
	void BeginMapEpoch(uint64_t mapEpoch);
	void ApplyCommitted(nri_scene::SceneView& sceneView, uint64_t mapEpoch);
	void BeginStage(uint64_t proposedSerial, uint64_t mapEpoch);
	bool StagePublishedView(const nri_scene::SceneView& sceneView);
	void FinalizeStage();
	bool CommitSubmitted(uint64_t serial);
	void DiscardStaged();
	bool NeedsSettle(uint32_t chunkIndex) const;
	bool HasFinalizedStage() const { return m_stageFinalized; }
	const Stats& GetStats() const { return m_stats; }
	DiagnosticRecord FindCommitted(uint64_t occurrenceId) const;
	const char* GetLastResetReason() const { return m_lastResetReason.c_str(); }

private:
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

bool RunNRIMapMotionHistorySelfTests(std::string* failureReason = nullptr);
