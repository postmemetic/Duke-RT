#pragma once

#include "nri_map_motion_payload_cache.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <set>
#include <string>

class NRIMapMotionReferenceHistory;
class NRIMapMotionStageBatch;

class NRIMapMotionHistory
{
public:
	NRIMapMotionHistory();
	~NRIMapMotionHistory();
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
	// Synchronous immutable lease: no input/history mutation until this returns.
	bool StagePublishedBatch(const std::vector<const nri_scene::SceneView*>& views, int workers);
	void FinalizeStage();
	bool CommitSubmitted(uint64_t serial);
	void DiscardStaged();
	bool NeedsSettle(uint32_t chunkIndex) const;
	const std::set<uint32_t>& GetSettleChunks() const;
	bool HasFinalizedStage() const;
	const Stats& GetStats() const;
	DiagnosticRecord FindCommitted(uint64_t occurrenceId) const;
	const char* GetLastResetReason() const;

private:
	friend class NRIMapMotionStageBatch;
	struct Record
	{
		uint64_t occurrenceId = 0;
		NRIMapMotionPayloadSnapshot snapshots[2];
		int committedSlot = -1;
		int stagedSlot = -1;
		uint32_t historyAge = 0;
		uint32_t stagedAge = 0;
		bool valid = true;
		bool stagedValid = true;
		bool publishedWithMotion = false;
	};
	struct ChunkObservation
	{
		uint32_t chunkIndex = UINT32_MAX;
		bool comparable = false;
		bool moved = false;
		bool publishedWithMotion = false;
	};
	using RecordMap = std::unordered_map<uint64_t, Record>;
	static bool SurfaceHasMotion(const nri_scene::SurfaceRef& surface);
	void ApplySurface(nri_scene::SurfaceRef& surface);
	bool StageSurface(const nri_scene::SurfaceRef& surface);
	void ClearStage(bool countDiscard);
	void EraseRecord(Record& record);
	void ReportAuditStorage() const;
	void EnsureReference();
	bool CheckReference(const char* operation);
	void Quarantine(const char* operation);
	bool CommitOptimized(uint64_t serial);
	void DisableStageCensus();

	RecordMap m_records;
	std::vector<Record*> m_committed;
	std::vector<Record*> m_staged;
	std::vector<ChunkObservation> m_chunkObservations;
	NRIMapMotionPayloadSnapshot m_buildScratch;
	std::set<uint32_t> m_settleChunks;
	uint64_t m_mapEpoch = 0;
	uint64_t m_stageSerial = 0;
	bool m_stageOpen = false;
	bool m_stageFinalized = false;
	Stats m_stats;
	std::string m_lastResetReason;
	size_t m_payloadCapacityBytes = 0;
	size_t m_inputCapacityBytes = 0;
	std::unique_ptr<NRIMapMotionReferenceHistory> m_reference;
	std::unique_ptr<NRIMapMotionStageBatch> m_stageBatch;
	uint64_t m_leaseGeneration = 0;
	bool m_quarantined = false;
};

bool RunNRIMapMotionHistorySelfTests(std::string* failureReason = nullptr);
