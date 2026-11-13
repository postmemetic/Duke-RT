#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct NRIEmissiveSamplingDistributionCandidate
{
	uint64_t stableKey = 0;
	uint64_t bindingKey = 0;
	uint64_t tieBreakKey = 0;
	float proposalWeight = 0.0f;
	float referenceProposalWeight = 0.0f;
	bool hasReferenceProposalWeight = false;
	bool live = true;
};

struct NRIEmissiveSamplingDistributionEntry
{
	size_t inputIndex = 0;
	uint64_t stableKey = 0;
	uint64_t bindingKey = 0;
	float proposalWeight = 0.0f;
	float selectionPdf = 0.0f;
};

struct NRIEmissiveSamplingDistributionStats
{
	// Observational CPU detail only; these do not authorize retained output.
	uint64_t topologyNs = 0;
	uint64_t proposalNs = 0;
	uint64_t selectionNs = 0;
	uint64_t emitNs = 0;
	uint32_t observedBuilds = 0;
	uint32_t selectionReuseOpportunities = 0;
	uint32_t proposalRecordsInserted = 0;
	uint32_t proposalBindingsReset = 0;
	uint32_t proposalRecordsRetired = 0;
	uint32_t scratchCapacityGrowths = 0;
	uint64_t scratchCapacityGrowthBytes = 0;
	uint32_t inputCount = 0;
	uint32_t uniqueCount = 0;
	uint32_t duplicateCount = 0;
	uint32_t cappedCount = 0;
	uint32_t boundGrowthCount = 0;
	uint64_t lastBoundGrowthStableKey = 0;
	float lastBoundGrowthOldWeight = 0.0f;
	float lastBoundGrowthNewWeight = 0.0f;
	bool lastBoundGrowthWasAuthored = false;
	uint32_t activeCount = 0;
	uint32_t retainedDarkCount = 0;
	uint32_t reactivatedCount = 0;
	uint32_t retiredMissingCount = 0;
	uint32_t retiredReplacedCount = 0;
	uint32_t recordCount = 0;
	uint32_t topologyBuilds = 0;
	uint32_t topologyReuses = 0;
	uint64_t topologyComparedCandidates = 0;
};

class NRIEmissiveSamplingDistribution
{
public:
	void Reset();
	// Zero permanently withholds reuse certification after counter wrap. Frame
	// bookkeeping does not change this output-affecting proposal identity.
	uint64_t ProposalRevision() const { return mProposalRevision; }
	uint64_t TopologyCapacityBytes() const { return mTopologyKeys.capacity() * sizeof(TopologyKey); }
	void Build(
		const std::vector<NRIEmissiveSamplingDistributionCandidate>& candidates,
		uint64_t frameIndex,
		size_t maxCandidateCount,
		std::vector<NRIEmissiveSamplingDistributionEntry>& outEntries,
		std::vector<float>& outCdf,
		NRIEmissiveSamplingDistributionStats* outStats = nullptr,
		bool allowTopologyReuse = true,
		bool collectCpuDetail = false);

private:
	void AdvanceProposalRevision()
	{
		if (mProposalRevision != 0) ++mProposalRevision;
	}
	uint64_t mProposalRevision = 1;
	struct ProposalRecord
	{
		uint64_t bindingKey = 0;
		uint64_t lastActiveFrame = 0;
		float referenceWeight = 0.0f;
		bool authored = false;
		bool wasDark = false;
	};

	std::unordered_map<uint64_t, ProposalRecord> mProposalRecords;
	std::vector<size_t> mOrderedIndices;
	std::vector<size_t> mUniqueIndices;
	std::vector<uint64_t> mResolvedStableKeys;
	std::vector<uint64_t> mLiveKeys;
	std::vector<float> mResolvedWeights;
	struct TopologyKey
	{
		uint64_t stableKey = 0;
		uint64_t bindingKey = 0;
		uint64_t tieBreakKey = 0;
	};
	std::vector<TopologyKey> mTopologyKeys;
	uint32_t mTopologyDuplicateCount = 0;
	bool mTopologyValid = false;
	// An audit opportunity compares consecutive observed builds only. A reset,
	// reference build or detail-off interval invalidates this observation.
	bool mAuditPreviousBuildValid = false;
	size_t mAuditPreviousMaxCandidateCount = 0;
};
