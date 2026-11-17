#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

struct NRIPersistentVoxelMaterialRangeHandle
{
	uint32_t offset = 0;
	uint32_t capacity = 0;
	uint64_t generation = 0;

	explicit operator bool() const { return generation != 0; }
};

struct NRIPersistentVoxelMaterialRangeStats
{
	uint64_t cursorRows = 0;
	uint64_t liveRows = 0;
	uint64_t holeRows = 0;
	uint64_t highWaterRows = 0;
	uint64_t allocations = 0;
	uint64_t releases = 0;
	uint64_t reuseAllocations = 0;
	uint64_t reusedRows = 0;
};

class NRIPersistentVoxelMaterialRangeAllocator
{
public:
	NRIPersistentVoxelMaterialRangeHandle Allocate(uint32_t capacity);
	// Reserve only within existing physical row capacity. Failure changes no
	// allocator state; an out-of-bounds hole is not eligible for this request.
	NRIPersistentVoxelMaterialRangeHandle AllocateWithin(uint32_t capacity, uint32_t maximumRows);
	bool Reallocate(
		const NRIPersistentVoxelMaterialRangeHandle& current,
		uint32_t capacity,
		NRIPersistentVoxelMaterialRangeHandle& outHandle,
		bool& outMoved);
	bool Release(const NRIPersistentVoxelMaterialRangeHandle& handle);
	bool Owns(const NRIPersistentVoxelMaterialRangeHandle& handle) const;
	void Reset();

	const NRIPersistentVoxelMaterialRangeStats& Stats() const { return stats; }
	bool ValidateInvariants() const;

private:
	struct FreeRange
	{
		uint32_t offset = 0;
		uint32_t capacity = 0;
	};

	uint64_t AcquireGeneration();
	void InsertFreeRange(uint32_t offset, uint32_t capacity);
	void TrimFreeTail();

	std::vector<FreeRange> freeRanges;
	std::unordered_map<uint64_t, NRIPersistentVoxelMaterialRangeHandle> liveRanges;
	NRIPersistentVoxelMaterialRangeStats stats;
	uint64_t nextGeneration = 1;
};
