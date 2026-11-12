#pragma once

#include "nri_runtime_mutation_worklist.h"
#include "nri_runtime_mutation_cause_trace.h"
#include "map_mutation_notifications.h"

#include <algorithm>
#include <vector>

// Producer notices only select existing truth analysis. They never publish a
// baseline or authorize reuse. Retain both applied and current dependency edges
// until application succeeds, including neighbors that are outside the view.
class NRIRuntimeMutationNotifications
{
public:
	void Reset()
	{
		mEnabled = false;
		mSectorOwners.clear(); mMapIds.clear(); mSectorConsumers.clear(); mWallConsumers.clear();
		mDependencies.clear(); mCandidates.Reset();
		mEvents.clear(); mChanges.clear();
	}
	bool Configure(const nri_scene::PTMapWorld& world, bool enabled)
	{
		if (!enabled) { if (mEnabled) Reset(); return false; }
		if (mEnabled && mBuild == world.buildSerial && mDependencies.size() == world.chunks.size() &&
			mSectorConsumers.size() == sector.Size() && mWallConsumers.size() == wall.Size()) return false;
		Reset(); mEnabled = true; mBuild = world.buildSerial;
		mDependencies.resize(world.chunks.size());
		mSectorConsumers.resize(sector.Size()); mWallConsumers.resize(wall.Size());
		mSectorOwners.resize(world.chunks.size());
		mMapIds.resize(world.chunks.size());
		for (size_t i = 0; i < world.chunks.size(); ++i)
		{
			mSectorOwners[i] = world.chunks[i].sectorIndex;
			mMapIds[i] = world.chunks[i].chunkIndex;
		}
		return true;
	}
	void UpdateDependencies(uint32_t chunk, const nri_scene::PTMapChunkMutationBaseline& applied)
	{
		if (!mEnabled || chunk >= mDependencies.size()) return;
		std::vector<uint64_t> next;
		const auto add = [&](MapMutationDomain domain, int64_t index)
		{
			const size_t count = domain == MapMutationDomain::Sector ? mSectorConsumers.size() : mWallConsumers.size();
			if (index >= 0 && (uint64_t)index < count) next.push_back(Key(domain, (uint32_t)index));
		};
		add(MapMutationDomain::Sector, applied.sectorIndex);
		for (const auto& input : applied.walls)
		{
			add(MapMutationDomain::Sector, input.nextsector);
			add(MapMutationDomain::Wall, input.nextwall);
		}
		const int32_t owner = mSectorOwners[chunk];
		add(MapMutationDomain::Sector, owner);
		if ((uint32_t)owner < sector.Size())
			for (const auto& input : sector[owner].walls)
			{
				add(MapMutationDomain::Wall, wall.IndexOf(&input));
				add(MapMutationDomain::Wall, input.nextwall);
				add(MapMutationDomain::Sector, input.nextsector);
			}
		std::sort(next.begin(), next.end()); next.erase(std::unique(next.begin(), next.end()), next.end());
		auto& previous = mDependencies[chunk];
		if (previous == next) return;
		for (uint64_t key : previous)
		{
			auto& consumers = Consumers(key);
			consumers.erase(std::remove(consumers.begin(), consumers.end(), chunk), consumers.end());
		}
		previous = std::move(next);
		for (uint64_t key : previous) Consumers(key).push_back(chunk);
	}
	const std::vector<uint32_t>& Gather(uint32_t chunkCount, bool enabled, bool trace,
		uint64_t build, uint64_t frame, int focusChunk, const char* focusList)
	{
		mCandidates.BeginFrame(chunkCount);
		auto& journal = GetMapMutationNotifications();
		const auto consumed = journal.Consume(mEvents, trace ? &mChanges : nullptr);
		journal.SetTraceEnabled(trace);
		mSourceCount = mEvents.size(); mWriteCount = mDependencyVisits = 0;
		for (const auto& event : mEvents)
		{
			mWriteCount += event.writeCount;
			if (!enabled || !mEnabled || event.worldEpoch != consumed.worldEpoch) continue;
			const auto& reverse = event.domain == MapMutationDomain::Sector ? mSectorConsumers : mWallConsumers;
			if (event.index >= reverse.size()) continue;
			for (uint32_t chunk : reverse[event.index])
			{
				++mDependencyVisits;
				mCandidates.MarkCandidate(chunk, 1u);
			}
		}
		if (trace)
		{
			uint32_t focusedChanges = 0;
			for (const auto& change : mChanges)
			{
				const auto& reverse = change.domain == MapMutationDomain::Sector ? mSectorConsumers : mWallConsumers;
				bool focused = false;
				if (change.index < reverse.size()) for (uint32_t chunk : reverse[change.index])
					focused |= nri_mutation_cause::IsFocusedChunk(mMapIds[chunk], focusChunk, focusList);
				if (!focused) continue;
				++focusedChanges;
				Printf("PERF pt mutation writer NRI: world_epoch=%llu build=%llu frame=%llu renderer_frame=%llu serial=%llu tick=%d domain=%u index=%u field=%u writer=%u field_name=%s writer_name=%s type=%u before=0x%llx after=0x%llx repair=%u\n",
					(unsigned long long)change.worldEpoch, (unsigned long long)build, (unsigned long long)frame,
					(unsigned long long)(frame + 1), (unsigned long long)change.serial, change.clock,
					(uint32_t)change.domain, change.index, (uint32_t)change.field, (uint32_t)change.writer,
					MapMutationFieldName(change.field), MapMutationWriterName(change.writer),
					(uint32_t)change.type, (unsigned long long)change.before, (unsigned long long)change.after, enabled);
			}
			Printf("PERF pt mutation notification NRI: world_epoch=%llu build=%llu frame=%llu renderer_frame=%llu serial=%llu sources=%llu writes=%llu dependency_visits=%llu candidates=%llu traced=%u suppressed=%llu rejected=%llu repair=%u recovery=0\n",
				(unsigned long long)consumed.worldEpoch, (unsigned long long)build, (unsigned long long)frame,
				(unsigned long long)(frame + 1), (unsigned long long)consumed.latestSerial,
				(unsigned long long)mSourceCount, (unsigned long long)mWriteCount, (unsigned long long)mDependencyVisits,
				(unsigned long long)mCandidates.GetCandidates().size(), focusedChanges,
				(unsigned long long)consumed.suppressedChanges, (unsigned long long)consumed.rejectedWrites, enabled);
		}
		return mCandidates.GetCandidates();
	}
	bool Selected(uint32_t chunk) const { return mCandidates.GetSourceMask(chunk) != 0; }
	uint64_t SourceCount() const { return mSourceCount; }
	uint64_t WriteCount() const { return mWriteCount; }
	uint64_t DependencyVisits() const { return mDependencyVisits; }
	uint64_t CandidateCount() const { return mCandidates.GetCandidates().size(); }

private:
	static uint64_t Key(MapMutationDomain domain, uint32_t index) { return ((uint64_t)domain << 32u) | index; }
	std::vector<uint32_t>& Consumers(uint64_t key)
	{
		return key >> 32u ? mWallConsumers[(uint32_t)key] : mSectorConsumers[(uint32_t)key];
	}
	bool mEnabled = false;
	uint64_t mBuild = 0, mSourceCount = 0, mWriteCount = 0, mDependencyVisits = 0;
	std::vector<int32_t> mSectorOwners;
	std::vector<uint32_t> mMapIds;
	std::vector<std::vector<uint64_t>> mDependencies;
	std::vector<std::vector<uint32_t>> mSectorConsumers, mWallConsumers;
	std::vector<MapMutationSourceEvent> mEvents;
	std::vector<MapMutationSourceChange> mChanges;
	NRIRuntimeMutationWorklist mCandidates;
};
