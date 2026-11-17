#include "nri_persistent_voxel_material_range_allocator.h"

#include <algorithm>
#include <limits>

namespace
{
uint64_t RangeEnd(uint32_t offset, uint32_t capacity)
{
	return uint64_t(offset) + capacity;
}
}

NRIPersistentVoxelMaterialRangeHandle NRIPersistentVoxelMaterialRangeAllocator::Allocate(uint32_t capacity)
{
	return AllocateWithin(capacity, std::numeric_limits<uint32_t>::max());
}

NRIPersistentVoxelMaterialRangeHandle NRIPersistentVoxelMaterialRangeAllocator::AllocateWithin(uint32_t capacity, uint32_t maximumRows)
{
	if (capacity == 0 || nextGeneration == 0)
	{
		return {};
	}

	size_t bestIndex = freeRanges.size();
	for (size_t i = 0; i < freeRanges.size(); i++)
	{
		if (freeRanges[i].capacity < capacity || RangeEnd(freeRanges[i].offset, capacity) > maximumRows)
		{
			continue;
		}
		if (bestIndex == freeRanges.size() ||
			freeRanges[i].capacity < freeRanges[bestIndex].capacity)
		{
			bestIndex = i;
		}
	}

	uint32_t offset = 0;
	const bool reused = bestIndex != freeRanges.size();
	if (reused)
	{
		FreeRange& range = freeRanges[bestIndex];
		offset = range.offset;
		range.offset += capacity;
		range.capacity -= capacity;
		if (range.capacity == 0)
		{
			freeRanges.erase(freeRanges.begin() + bestIndex);
		}
	}
	else
	{
		if (stats.cursorRows > maximumRows || capacity > uint64_t(maximumRows) - stats.cursorRows)
		{
			return {};
		}
		offset = (uint32_t)stats.cursorRows;
		stats.cursorRows += capacity;
		stats.highWaterRows = std::max(stats.highWaterRows, stats.cursorRows);
	}

	NRIPersistentVoxelMaterialRangeHandle handle;
	handle.offset = offset;
	handle.capacity = capacity;
	handle.generation = AcquireGeneration();
	liveRanges.emplace(handle.generation, handle);
	stats.liveRows += capacity;
	stats.allocations++;
	if (reused)
	{
		stats.holeRows -= capacity;
		stats.reuseAllocations++;
		stats.reusedRows += capacity;
	}
	return handle;
}

bool NRIPersistentVoxelMaterialRangeAllocator::Reallocate(
	const NRIPersistentVoxelMaterialRangeHandle& current,
	uint32_t capacity,
	NRIPersistentVoxelMaterialRangeHandle& outHandle,
	bool& outMoved)
{
	outHandle = {};
	outMoved = false;
	if (capacity == 0)
	{
		return false;
	}
	if (current && current.capacity == capacity)
	{
		if (!Owns(current))
		{
			return false;
		}
		outHandle = current;
		return true;
	}

	const NRIPersistentVoxelMaterialRangeHandle replacement = Allocate(capacity);
	if (!replacement)
	{
		return false;
	}
	if (current && !Release(current))
	{
		Release(replacement);
		return false;
	}
	outHandle = replacement;
	outMoved = true;
	return true;
}

bool NRIPersistentVoxelMaterialRangeAllocator::Release(
	const NRIPersistentVoxelMaterialRangeHandle& handle)
{
	if (!handle || handle.capacity == 0)
	{
		return false;
	}
	const auto live = liveRanges.find(handle.generation);
	if (live == liveRanges.end() ||
		live->second.offset != handle.offset ||
		live->second.capacity != handle.capacity)
	{
		return false;
	}

	liveRanges.erase(live);
	stats.liveRows -= handle.capacity;
	stats.holeRows += handle.capacity;
	stats.releases++;
	InsertFreeRange(handle.offset, handle.capacity);
	TrimFreeTail();
	return true;
}

bool NRIPersistentVoxelMaterialRangeAllocator::Owns(
	const NRIPersistentVoxelMaterialRangeHandle& handle) const
{
	if (!handle || handle.capacity == 0)
	{
		return false;
	}
	const auto live = liveRanges.find(handle.generation);
	return live != liveRanges.end() &&
		live->second.offset == handle.offset &&
		live->second.capacity == handle.capacity;
}

void NRIPersistentVoxelMaterialRangeAllocator::Reset()
{
	freeRanges.clear();
	liveRanges.clear();
	stats = {};
}

bool NRIPersistentVoxelMaterialRangeAllocator::ValidateInvariants() const
{
	uint64_t freeRows = 0;
	uint64_t previousEnd = 0;
	bool hasPrevious = false;
	for (const FreeRange& range : freeRanges)
	{
		const uint64_t end = RangeEnd(range.offset, range.capacity);
		if (range.capacity == 0 || end > stats.cursorRows ||
			(hasPrevious && range.offset <= previousEnd))
		{
			return false;
		}
		freeRows += range.capacity;
		previousEnd = end;
		hasPrevious = true;
	}

	std::vector<FreeRange> sortedLive;
	sortedLive.reserve(liveRanges.size());
	uint64_t liveRows = 0;
	for (const auto& entry : liveRanges)
	{
		const NRIPersistentVoxelMaterialRangeHandle& handle = entry.second;
		if (entry.first == 0 || entry.first != handle.generation ||
			handle.capacity == 0 || RangeEnd(handle.offset, handle.capacity) > stats.cursorRows)
		{
			return false;
		}
		sortedLive.push_back({ handle.offset, handle.capacity });
		liveRows += handle.capacity;
	}
	std::sort(sortedLive.begin(), sortedLive.end(), [](const FreeRange& left, const FreeRange& right)
	{
		return left.offset < right.offset;
	});
	for (size_t i = 1; i < sortedLive.size(); i++)
	{
		if (RangeEnd(sortedLive[i - 1].offset, sortedLive[i - 1].capacity) > sortedLive[i].offset)
		{
			return false;
		}
	}

	for (const FreeRange& hole : freeRanges)
	{
		const auto overlap = std::lower_bound(
			sortedLive.begin(), sortedLive.end(), hole.offset,
			[](const FreeRange& range, uint32_t offset)
			{
				return RangeEnd(range.offset, range.capacity) <= offset;
			});
		if (overlap != sortedLive.end() && overlap->offset < RangeEnd(hole.offset, hole.capacity))
		{
			return false;
		}
	}

	return freeRows == stats.holeRows &&
		liveRows == stats.liveRows &&
		stats.liveRows + stats.holeRows == stats.cursorRows &&
		stats.cursorRows <= stats.highWaterRows &&
		stats.allocations >= stats.releases &&
		stats.allocations - stats.releases == liveRanges.size() &&
		stats.reuseAllocations <= stats.allocations;
}

uint64_t NRIPersistentVoxelMaterialRangeAllocator::AcquireGeneration()
{
	const uint64_t generation = nextGeneration;
	nextGeneration++;
	return generation;
}

void NRIPersistentVoxelMaterialRangeAllocator::InsertFreeRange(uint32_t offset, uint32_t capacity)
{
	auto next = std::lower_bound(
		freeRanges.begin(), freeRanges.end(), offset,
		[](const FreeRange& range, uint32_t value) { return range.offset < value; });

	if (next != freeRanges.begin())
	{
		auto previous = next - 1;
		if (RangeEnd(previous->offset, previous->capacity) == offset)
		{
			previous->capacity += capacity;
			if (next != freeRanges.end() &&
				RangeEnd(previous->offset, previous->capacity) == next->offset)
			{
				previous->capacity += next->capacity;
				freeRanges.erase(next);
			}
			return;
		}
	}

	if (next != freeRanges.end() && RangeEnd(offset, capacity) == next->offset)
	{
		next->offset = offset;
		next->capacity += capacity;
		return;
	}
	freeRanges.insert(next, { offset, capacity });
}

void NRIPersistentVoxelMaterialRangeAllocator::TrimFreeTail()
{
	while (!freeRanges.empty())
	{
		const FreeRange& tail = freeRanges.back();
		if (RangeEnd(tail.offset, tail.capacity) != stats.cursorRows)
		{
			break;
		}
		stats.cursorRows = tail.offset;
		stats.holeRows -= tail.capacity;
		freeRanges.pop_back();
	}
}
