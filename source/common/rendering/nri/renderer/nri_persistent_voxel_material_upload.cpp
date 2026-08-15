#include "nri_persistent_voxel_material_upload.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace
{
constexpr uint64_t kMaterialUploadCoalesceMaxGapBytes = 4ull * 1024ull;
constexpr uint64_t kMaterialUploadCoalesceMaxByteExpansion = 2ull;

bool RangeIsValid(const NRIPersistentVoxelMaterialUploadRange& range, uint64_t availableBytes)
{
	return range.size != 0 && range.dirtySize == range.size &&
		range.byteOffset <= availableBytes &&
		range.size <= availableBytes - range.byteOffset;
}
}

bool NRIPersistentVoxelMaterialUploadMirror::Prepare(
	uint64_t arenaSize,
	const uint8_t* activeArenaData,
	uint64_t activeArenaSize,
	const std::vector<NRIPersistentVoxelMaterialUploadRange>& dirtyRanges,
	NRIPersistentVoxelMaterialUploadPlan& outPlan)
{
	outPlan = {};
	if (updatePending || activeArenaData == nullptr ||
		arenaSize > std::numeric_limits<size_t>::max())
	{
		return false;
	}

	std::vector<NRIPersistentVoxelMaterialUploadRange> sortedRanges = dirtyRanges;
	for (const NRIPersistentVoxelMaterialUploadRange& range : sortedRanges)
	{
		if (!RangeIsValid(range, activeArenaSize) || !RangeIsValid(range, arenaSize))
		{
			return false;
		}
	}
	std::sort(sortedRanges.begin(), sortedRanges.end(),
		[](const auto& left, const auto& right)
		{
			return left.byteOffset < right.byteOffset;
		});
	for (size_t i = 1; i < sortedRanges.size(); i++)
	{
		if (sortedRanges[i].byteOffset < sortedRanges[i - 1].byteOffset + sortedRanges[i - 1].size)
		{
			return false;
		}
	}

	outPlan.ranges.reserve(sortedRanges.size());
	for (const NRIPersistentVoxelMaterialUploadRange& range : sortedRanges)
	{
		if (outPlan.ranges.empty())
		{
			outPlan.ranges.push_back(range);
			continue;
		}

		NRIPersistentVoxelMaterialUploadRange& tail = outPlan.ranges.back();
		const uint64_t tailEnd = tail.byteOffset + tail.size;
		const uint64_t rangeEnd = range.byteOffset + range.size;
		const uint64_t gapBytes = range.byteOffset > tailEnd ? range.byteOffset - tailEnd : 0;
		const uint64_t candidateSize = rangeEnd > tailEnd ? rangeEnd - tail.byteOffset : tail.size;
		const bool dirtySizeOverflow = range.size > std::numeric_limits<uint64_t>::max() - tail.dirtySize;
		const uint64_t candidateDirtySize = dirtySizeOverflow ? 0 : tail.dirtySize + range.size;
		const bool acceptableByteExpansion = !dirtySizeOverflow &&
			(candidateDirtySize > std::numeric_limits<uint64_t>::max() / kMaterialUploadCoalesceMaxByteExpansion ||
			candidateSize <= candidateDirtySize * kMaterialUploadCoalesceMaxByteExpansion);
		if (gapBytes <= kMaterialUploadCoalesceMaxGapBytes && acceptableByteExpansion)
		{
			tail.size = candidateSize;
			tail.dirtySize = candidateDirtySize;
			continue;
		}

		outPlan.rejectedMerges++;
		outPlan.ranges.push_back(range);
	}

	rollbackSize = bytes.size();
	rollbackRanges.clear();
	rollbackBytes.clear();
	rollbackRanges.reserve(sortedRanges.size());
	uint64_t rollbackByteCount = 0;
	for (const NRIPersistentVoxelMaterialUploadRange& range : sortedRanges)
	{
		const uint64_t retainedEnd = std::min<uint64_t>(range.byteOffset + range.size, rollbackSize);
		if (retainedEnd > range.byteOffset)
		{
			rollbackByteCount += retainedEnd - range.byteOffset;
		}
	}
	rollbackBytes.reserve((size_t)rollbackByteCount);
	for (const NRIPersistentVoxelMaterialUploadRange& range : sortedRanges)
	{
		const uint64_t retainedEnd = std::min<uint64_t>(range.byteOffset + range.size, rollbackSize);
		if (retainedEnd > range.byteOffset)
		{
			const uint64_t rollbackOffset = rollbackBytes.size();
			rollbackBytes.insert(
				rollbackBytes.end(),
				bytes.begin() + (size_t)range.byteOffset,
				bytes.begin() + (size_t)retainedEnd);
			rollbackRanges.push_back({
				range.byteOffset,
				rollbackOffset,
				retainedEnd - range.byteOffset });
		}
	}

	bytes.resize((size_t)arenaSize);
	for (const NRIPersistentVoxelMaterialUploadRange& range : sortedRanges)
	{
		std::memcpy(
			bytes.data() + (size_t)range.byteOffset,
			activeArenaData + range.byteOffset,
			(size_t)range.size);
	}
	updatePending = true;
	return true;
}

void NRIPersistentVoxelMaterialUploadMirror::Commit()
{
	rollbackRanges.clear();
	rollbackBytes.clear();
	rollbackSize = 0;
	updatePending = false;
}

void NRIPersistentVoxelMaterialUploadMirror::Rollback()
{
	if (!updatePending)
	{
		return;
	}
	for (const RollbackRange& rollback : rollbackRanges)
	{
		if (rollback.size != 0)
		{
			std::memcpy(
				bytes.data() + (size_t)rollback.byteOffset,
				rollbackBytes.data() + (size_t)rollback.rollbackOffset,
				(size_t)rollback.size);
		}
	}
	bytes.resize((size_t)rollbackSize);
	Commit();
}

void NRIPersistentVoxelMaterialUploadMirror::Reset()
{
	bytes.clear();
	rollbackRanges.clear();
	rollbackBytes.clear();
	rollbackSize = 0;
	updatePending = false;
}
