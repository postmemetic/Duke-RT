#include "nri_emissive_sampling_distribution.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
	using AuditClock = std::chrono::steady_clock;
	uint64_t AuditElapsedNs(AuditClock::time_point start)
	{
		return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(AuditClock::now() - start).count();
	}

	uint64_t HashCandidateIdentity(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u));
	}
}

void NRIEmissiveSamplingDistribution::Reset()
{
	// Do not reuse an earlier destination identity after reset, even when the
	// old record set was empty. A wrapped (zero) revision stays uncertified.
	AdvanceProposalRevision();
	mProposalRecords.clear();
	mTopologyValid = false;
	mTopologyDuplicateCount = 0;
	mAuditPreviousBuildValid = false;
	std::vector<TopologyKey>{}.swap(mTopologyKeys);
}

void NRIEmissiveSamplingDistribution::Build(
	const std::vector<NRIEmissiveSamplingDistributionCandidate>& candidates,
	uint64_t frameIndex,
	size_t maxCandidateCount,
	std::vector<NRIEmissiveSamplingDistributionEntry>& outEntries,
	std::vector<float>& outCdf,
	NRIEmissiveSamplingDistributionStats* outStats,
	bool allowTopologyReuse,
	bool collectCpuDetail)
{
	NRIEmissiveSamplingDistributionStats stats = {};
	const bool audit = collectCpuDetail && allowTopologyReuse;
	const auto topologyStart = audit ? AuditClock::now() : AuditClock::time_point{};
	bool proposalHistoryChanged = false;
	const size_t oldCapacities[] = { mOrderedIndices.capacity(), mUniqueIndices.capacity(),
		mResolvedStableKeys.capacity(), mLiveKeys.capacity(), mResolvedWeights.capacity(), mTopologyKeys.capacity() };
	const auto recordScratchGrowth = [&]()
	{
		const size_t capacities[] = { mOrderedIndices.capacity(), mUniqueIndices.capacity(),
			mResolvedStableKeys.capacity(), mLiveKeys.capacity(), mResolvedWeights.capacity(), mTopologyKeys.capacity() };
		const size_t strides[] = { sizeof(size_t), sizeof(size_t), sizeof(uint64_t), sizeof(uint64_t), sizeof(float), sizeof(TopologyKey) };
		for (size_t i = 0; i < 6; ++i)
		{
			if (capacities[i] > oldCapacities[i])
			{
				stats.scratchCapacityGrowths++;
				stats.scratchCapacityGrowthBytes += (capacities[i] - oldCapacities[i]) * strides[i];
			}
		}
	};
	stats.inputCount = (uint32_t)candidates.size();
	outEntries.clear();
	outCdf.clear();

	auto& orderedIndices = mOrderedIndices;
	auto& uniqueIndices = mUniqueIndices;
	auto& resolvedStableKeys = mResolvedStableKeys;
	bool sameTopology = allowTopologyReuse && mTopologyValid && mTopologyKeys.size() == candidates.size();
	for (size_t i = 0; sameTopology && i < candidates.size(); ++i)
	{
		++stats.topologyComparedCandidates;
		const auto& previous = mTopologyKeys[i];
		const auto& current = candidates[i];
		sameTopology = previous.stableKey == current.stableKey &&
			previous.bindingKey == current.bindingKey && previous.tieBreakKey == current.tieBreakKey;
	}
	if (sameTopology)
	{
		// Restore the complete pre-cap order. The selected tail of the previous
		// build must not determine which candidates update proposal history now.
		uniqueIndices.assign(orderedIndices.begin(), orderedIndices.end());
		stats.duplicateCount = mTopologyDuplicateCount;
		stats.uniqueCount = (uint32_t)uniqueIndices.size();
		stats.topologyReuses = 1;
	}
	else
	{
		stats.topologyBuilds = 1;
		orderedIndices.resize(candidates.size());
		for (size_t i = 0; i < candidates.size(); ++i)
		{
			orderedIndices[i] = i;
		}
		std::sort(orderedIndices.begin(), orderedIndices.end(), [&](size_t aIndex, size_t bIndex)
		{
			const auto& a = candidates[aIndex];
			const auto& b = candidates[bIndex];
			if (a.stableKey != b.stableKey)
			{
				return a.stableKey < b.stableKey;
			}
			if (a.bindingKey != b.bindingKey)
			{
				return a.bindingKey < b.bindingKey;
			}
			if (a.tieBreakKey != b.tieBreakKey)
			{
				return a.tieBreakKey < b.tieBreakKey;
			}
			return aIndex < bIndex;
		});

		uniqueIndices.clear();
		uniqueIndices.reserve(orderedIndices.size());
		resolvedStableKeys.assign(candidates.size(), 0);
		for (size_t first = 0; first < orderedIndices.size();)
		{
			size_t last = first + 1u;
			while (last < orderedIndices.size() && candidates[orderedIndices[last]].stableKey == candidates[orderedIndices[first]].stableKey)
			{
				last++;
			}
			const bool duplicateGroup = last - first > 1u;
			if (duplicateGroup)
			{
				stats.duplicateCount += (uint32_t)(last - first - 1u);
			}
			for (size_t i = first; i < last; ++i)
			{
				const size_t index = orderedIndices[i];
				resolvedStableKeys[index] = duplicateGroup ?
					HashCandidateIdentity(candidates[index].stableKey, (uint64_t)(i - first)) :
					candidates[index].stableKey;
				uniqueIndices.push_back(index);
			}
			first = last;
		}
		stats.uniqueCount = (uint32_t)uniqueIndices.size();
		auto& liveKeys = mLiveKeys;
		liveKeys.clear();
		liveKeys.reserve(uniqueIndices.size());
		for (size_t index : uniqueIndices)
		{
			liveKeys.push_back(resolvedStableKeys[index]);
		}
		std::sort(liveKeys.begin(), liveKeys.end());
		for (auto it = mProposalRecords.begin(); it != mProposalRecords.end();)
		{
			if (!std::binary_search(liveKeys.begin(), liveKeys.end(), it->first))
			{
				stats.retiredMissingCount++;
				proposalHistoryChanged = true;
				it = mProposalRecords.erase(it);
			}
			else
			{
				++it;
			}
		}
		mTopologyKeys.resize(candidates.size());
		for (size_t i = 0; i < candidates.size(); ++i)
			mTopologyKeys[i] = { candidates[i].stableKey, candidates[i].bindingKey, candidates[i].tieBreakKey };
		mTopologyDuplicateCount = stats.duplicateCount;
		mTopologyValid = allowTopologyReuse;
	}

	if (audit) stats.topologyNs = AuditElapsedNs(topologyStart);
	const auto proposalStart = audit ? AuditClock::now() : AuditClock::time_point{};
	auto& resolvedWeights = mResolvedWeights;
	resolvedWeights.assign(candidates.size(), 0.0f);
	constexpr float BoundAbsoluteTolerance = 1e-6f;
	constexpr float BoundRelativeTolerance = 1e-4f;
	for (size_t index : uniqueIndices)
	{
		const auto& candidate = candidates[index];
		const float currentWeight = std::isfinite(candidate.proposalWeight) ? std::max(candidate.proposalWeight, 0.0f) : 0.0f;
		const float authoredWeight =
			candidate.hasReferenceProposalWeight && std::isfinite(candidate.referenceProposalWeight) ?
				std::max(candidate.referenceProposalWeight, 0.0f) : 0.0f;
		const float initialWeight = std::max(currentWeight, authoredWeight);
		const uint64_t stableKey = resolvedStableKeys[index];
		auto [recordIt, inserted] = mProposalRecords.emplace(stableKey, ProposalRecord{});
		stats.proposalRecordsInserted += audit && inserted;
		auto& record = recordIt->second;
		if (inserted || record.bindingKey != candidate.bindingKey)
		{
			proposalHistoryChanged = true;
			if (!inserted)
			{
				stats.retiredReplacedCount++;
				stats.proposalBindingsReset += audit;
			}
			record.bindingKey = candidate.bindingKey;
			record.referenceWeight = initialWeight;
			record.authored = candidate.hasReferenceProposalWeight;
			record.wasDark = false;
		}
		else
		{
			const float requestedWeight = std::max(currentWeight, authoredWeight);
			const float growthTolerance = BoundAbsoluteTolerance + record.referenceWeight * BoundRelativeTolerance;
			if (requestedWeight > record.referenceWeight + growthTolerance)
			{
				proposalHistoryChanged = true;
				stats.boundGrowthCount++;
				stats.lastBoundGrowthStableKey = stableKey;
				stats.lastBoundGrowthOldWeight = record.referenceWeight;
				stats.lastBoundGrowthNewWeight = requestedWeight;
				stats.lastBoundGrowthWasAuthored = authoredWeight >= currentWeight;
				record.referenceWeight = requestedWeight;
				record.authored = stats.lastBoundGrowthWasAuthored;
			}
		}
		if (candidate.live)
		{
			stats.activeCount++;
			if (record.wasDark)
			{
				stats.reactivatedCount++;
			}
			record.lastActiveFrame = frameIndex;
		}
		record.wasDark = !candidate.live && record.referenceWeight > 0.0f;
		if (record.wasDark)
		{
			stats.retainedDarkCount++;
		}
		resolvedWeights[index] = record.referenceWeight;
	}
	if (proposalHistoryChanged) AdvanceProposalRevision();
	stats.recordCount = (uint32_t)mProposalRecords.size();
	if (audit)
	{
		stats.proposalNs = AuditElapsedNs(proposalStart);
		stats.observedBuilds = 1;
		stats.proposalRecordsRetired = stats.retiredMissingCount;
		// This predicts unchanged ordered selection/PDF/CDF, not unchanged
		// candidate payloads or permission to skip live/history bookkeeping.
		stats.selectionReuseOpportunities = mAuditPreviousBuildValid && sameTopology &&
			!proposalHistoryChanged && mProposalRevision != 0 &&
			mAuditPreviousMaxCandidateCount == maxCandidateCount ? 1u : 0u;
		mAuditPreviousMaxCandidateCount = maxCandidateCount;
	}
	mAuditPreviousBuildValid = audit;

	const auto selectionStart = audit ? AuditClock::now() : AuditClock::time_point{};
	if (uniqueIndices.size() > maxCandidateCount)
	{
		std::sort(uniqueIndices.begin(), uniqueIndices.end(), [&](size_t aIndex, size_t bIndex)
		{
			const float aWeight = resolvedWeights[aIndex];
			const float bWeight = resolvedWeights[bIndex];
			if (aWeight != bWeight)
			{
				return aWeight > bWeight;
			}
			return resolvedStableKeys[aIndex] < resolvedStableKeys[bIndex];
		});
		stats.cappedCount = (uint32_t)(uniqueIndices.size() - maxCandidateCount);
		uniqueIndices.resize(maxCandidateCount);
		std::sort(uniqueIndices.begin(), uniqueIndices.end(), [&](size_t aIndex, size_t bIndex)
		{
			return resolvedStableKeys[aIndex] < resolvedStableKeys[bIndex];
		});
	}

	if (audit) stats.selectionNs = AuditElapsedNs(selectionStart);
	const auto emitStart = audit ? AuditClock::now() : AuditClock::time_point{};
	float totalWeight = 0.0f;
	outEntries.reserve(uniqueIndices.size());
	for (size_t index : uniqueIndices)
	{
		const auto& candidate = candidates[index];
		NRIEmissiveSamplingDistributionEntry entry = {};
		entry.inputIndex = index;
		entry.stableKey = resolvedStableKeys[index];
		entry.bindingKey = candidate.bindingKey;
		entry.proposalWeight = resolvedWeights[index];
		totalWeight += entry.proposalWeight;
		outEntries.push_back(entry);
	}

	if (outEntries.empty())
	{
		outCdf.assign(1, 1.0f);
		if (audit) stats.emitNs = AuditElapsedNs(emitStart);
		recordScratchGrowth();
		if (outStats != nullptr)
		{
			*outStats = stats;
		}
		return;
	}

	const float uniformPdf = 1.0f / (float)outEntries.size();
	const float invTotalWeight = totalWeight > 0.0f ? 1.0f / totalWeight : 0.0f;
	float runningCdf = 0.0f;
	outCdf.reserve(outEntries.size());
	for (size_t i = 0; i < outEntries.size(); ++i)
	{
		auto& entry = outEntries[i];
		entry.selectionPdf = totalWeight > 0.0f ? entry.proposalWeight * invTotalWeight : uniformPdf;
		runningCdf += entry.selectionPdf;
		outCdf.push_back(i + 1u == outEntries.size() ? 1.0f : std::min(runningCdf, 1.0f));
	}

	if (audit) stats.emitNs = AuditElapsedNs(emitStart);
	recordScratchGrowth();
	if (outStats != nullptr)
	{
		*outStats = stats;
	}
}
