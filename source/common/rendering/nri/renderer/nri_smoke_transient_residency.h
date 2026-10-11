#pragma once

#include "nri_smoke_transient_clouds.h"

// Plane interiors satisfy dot(normal, position) + d >= 0. Raw plane normals
// are normalized by the owner. Invalid/uncertain views are conservatively Hot.
struct NRISmokeTransientView
{
	float position[3] = {};
	float planes[6][4] = {};
	uint32_t planeCount = 0u;
	float hotPadding = 64.0f;
	float warmPadding = 256.0f;
	float nearDistance = 128.0f;
	bool valid = false;
};

struct NRISmokeTransientResidencySnapshot
{
	uint64_t submittedGroups = 0u;
	uint64_t duplicateGroups = 0u;
	uint64_t invalidGroups = 0u;
	uint64_t staleGroups = 0u;
	uint64_t historyRejectedGroups = 0u;
	uint64_t historyEvictedGroups = 0u;
	uint64_t expiredGroups = 0u;
	uint64_t admittedGroups = 0u;
	uint64_t reenteredGroups = 0u;
	uint64_t releasedGroups = 0u;
	uint64_t reducedGroups = 0u;
	uint64_t unsupportedLoadFrames = 0u;
	uint64_t allocatedHistoryBytes = 0u;
	uint64_t compactExplosionEvents = 0u;
	uint64_t firstPresentedExplosionEvents = 0u;
	uint32_t maximumExplosionFirstAgeMilliseconds = 0u;
	uint32_t maximumExplosionAdmissionDelayMilliseconds = 0u;
	uint32_t epoch = 0u;
	uint32_t historyGroups = 0u;
	uint32_t hotGroups = 0u;
	uint32_t warmGroups = 0u;
	uint32_t dormantGroups = 0u;
	uint32_t hotFireSources = 0u;
	uint32_t supportedFireSources = 0u;
	uint32_t unsupportedFireSources = 0u;
	uint32_t overBudgetResidentGroups = 0u;
	uint32_t overBudgetResidentLobes = 0u;
	uint32_t residentFireGroups = 0u;
	uint32_t residentFireLobes = 0u;
	uint32_t residentBurstGroups = 0u;
	uint32_t residentBurstLobes = 0u;
	uint32_t hotFireDeferredGroups = 0u;
	uint32_t hotBurstDeferredGroups = 0u;
	uint32_t largestDeferredBirthSpanMilliseconds = 0u;
	uint32_t firstVisibleGroups = 0u;
	uint32_t hiddenResidentGroups = 0u;
	uint32_t fireGroupBudget = 0u;
	uint32_t fireLobeBudget = 0u;
	uint32_t burstGroupBudget = 0u;
	uint32_t burstLobeBudget = 0u;
	uint32_t residentBorrowExplosionGroups = 0u;
	uint32_t residentBorrowExplosionLobes = 0u;
	uint32_t deferredBorrowExplosionGroups = 0u;
	uint32_t borrowExplosionGroupBudget = 0u;
	uint32_t borrowExplosionLobeBudget = 0u;
	uint32_t protectedFireGroups = 0u;
	uint32_t protectedFireLobes = 0u;
	uint32_t loanedBurstGroups = 0u;
	uint32_t loanedBurstLobes = 0u;
};

// Bounded analytic event history is independent of the smaller detailed pool.
// It never synthesizes source emissions: deletion simply stops new submissions,
// while existing descriptors keep their original lifetime, transform, and seed.
class NRISmokeTransientResidency
{
public:
	static constexpr uint32_t MaximumHistoryGroups = 1024u;
	static constexpr uint32_t ReservedFireHistoryGroups = 512u;
	static constexpr uint32_t MaximumFireCohortsPerSource = 12u;

	void BeginFrame(double gameplaySeconds, uint32_t epoch,
		const NRISmokeTransientProfile& profile, const NRISmokeTransientView& view,
		NRISmokeTransientClouds& clouds);
	bool SubmitBatch(const NRISmokeTransientLobeRequest* requests, uint32_t count);
	void Resolve(NRISmokeTransientClouds& clouds);
	bool RetireLatest(uint64_t replacementKey, NRISmokeTransientClouds& clouds);
	void Reset(uint32_t epoch);
	const NRISmokeTransientResidencySnapshot& GetSnapshot() const { return mSnapshot; }

private:
	struct Entry
	{
		std::array<NRISmokeTransientLobeRequest,
			NRISmokeTransientClouds::FixedMaximumLobesPerGroup> requests = {};
		NRISmokeTransientHandle handle = {};
		float boundsMin[3] = {};
		float boundsMax[3] = {};
		double lastHotSeconds = -1.0e30;
		double submittedGameplaySeconds = 0.0;
		uint64_t admissionOrdinal = 0u;
		uint32_t count = 0u;
		uint32_t residentLobes = 0u;
		uint32_t borrowDetail = 0u;
		bool firstPresented = false;
		NRISmokeTransientInterest interest = NRISmokeTransientInterest::Dormant;
	};

	void Classify(Entry& entry) const;
	void BuildBounds(Entry& entry) const;
	bool Intersects(const Entry& entry, float padding) const;
	void RefreshSnapshot(const NRISmokeTransientClouds& clouds);
	void StampExplosionEpisodes();
	std::vector<Entry> mHistory;
	NRISmokeTransientResidencySnapshot mSnapshot = {};
	NRISmokeTransientProfile mProfile = {};
	NRISmokeTransientView mView = {};
	double mTime = 0.0;
	double mLastExplosionAuthoredSeconds = -1.0e30;
	uint64_t mAdmissionOrdinal = 0u;
	bool mPrepared = false;
};
