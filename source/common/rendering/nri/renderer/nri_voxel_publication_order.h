#pragma once

#include <cstdint>
#include <vector>

struct NRIVoxelPublicationOrderKey
{
	uint64_t identity = 0;
	uint64_t ownerEpoch = 0;
	uint64_t ownerLifetime = 0;
	uint64_t retainedAge = 0;
	uint32_t inputIndex = 0;
	uint32_t primitives = 0;
	bool pending = false;
	bool resident = false;
	bool captured = false;
};

// Immutable admission policy inputs; only the retained permutation is cached.
// Input index breaks exact ties as stable_sort of the original input would.
class NRIVoxelPublicationOrder
{
public:
	std::vector<NRIVoxelPublicationOrderKey> keys;
	bool Update(bool tlasOrder, bool validate);
	void Reset();
	const std::vector<uint32_t>& Indices() const { return order; }
	bool SortedThisUpdate() const { return sorted; }
	bool ValidationMatched() const { return validationMatched; }
	void ClearUpdateStats() { sorted = false; validationMatched = true; }
private:
	std::vector<uint64_t> identities;
	std::vector<uint32_t> inputIndices;
	std::vector<uint32_t> order;
	std::vector<uint32_t> reference;
	bool sorted = false;
	bool validationMatched = true;
};

// Frame stamps retain buckets without clear/reinsert node allocations.
class NRIVoxelFrameKeyCounts
{
public:
	void Begin(uint32_t expected);
	uint32_t Add(uint64_t key, uint64_t second = 0);
	uint32_t Count(uint64_t key, uint64_t second = 0) const;
	uint32_t Size() const { return size; }
private:
	struct Slot { uint64_t key = 0, second = 0; uint32_t stamp = 0, count = 0; };
	std::vector<Slot> slots;
	uint32_t stamp = 0;
	uint32_t size = 0;
};
