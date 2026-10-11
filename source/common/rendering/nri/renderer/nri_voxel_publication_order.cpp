#include "nri_voxel_publication_order.h"
#include <algorithm>
#include <numeric>

namespace
{
bool Less(const NRIVoxelPublicationOrderKey& a, const NRIVoxelPublicationOrderKey& b, bool tlas)
{
	if (tlas)
	{
		if (a.captured != b.captured) return a.captured;
		if (a.retainedAge != b.retainedAge) return a.retainedAge < b.retainedAge;
		if (a.primitives != b.primitives) return a.primitives < b.primitives;
	}
	else
	{
		if (a.pending != b.pending) return a.pending;
		if (a.resident != b.resident) return !a.resident;
		if (a.primitives != b.primitives) return a.primitives > b.primitives;
	}
	if (a.identity != b.identity) return a.identity < b.identity;
	return a.inputIndex < b.inputIndex;
}

uint64_t Hash(uint64_t key, uint64_t second)
{
	key ^= second + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2);
	key ^= key >> 30; key *= 0xbf58476d1ce4e5b9ull;
	key ^= key >> 27; key *= 0x94d049bb133111ebull;
	return key ^ (key >> 31);
}
}

bool NRIVoxelPublicationOrder::Update(bool tlas, bool validate)
{
	bool membershipChanged = identities.size() != keys.size();
	for (size_t i = 0; !membershipChanged && i < keys.size(); ++i)
		membershipChanged = identities[i] != keys[i].identity || inputIndices[i] != keys[i].inputIndex;
	if (membershipChanged)
	{
		identities.resize(keys.size()); inputIndices.resize(keys.size()); order.resize(keys.size());
		for (uint32_t i = 0; i < keys.size(); ++i)
		{
			identities[i] = keys[i].identity; inputIndices[i] = keys[i].inputIndex; order[i] = i;
		}
	}
	auto less = [&](uint32_t a, uint32_t b) { return Less(keys[a], keys[b], tlas); };
	sorted = !std::is_sorted(order.begin(), order.end(), less);
	if (sorted) std::sort(order.begin(), order.end(), less);
	validationMatched = true;
	if (validate)
	{
		reference.resize(keys.size());
		std::iota(reference.begin(), reference.end(), 0u);
		std::stable_sort(reference.begin(), reference.end(), less);
		validationMatched = reference == order;
		if (!validationMatched) order = reference;
	}
	return membershipChanged;
}

void NRIVoxelPublicationOrder::Reset()
{
	keys.clear(); identities.clear(); inputIndices.clear(); order.clear(); reference.clear();
	sorted = false; validationMatched = true;
}

void NRIVoxelFrameKeyCounts::Begin(uint32_t expected)
{
	size_t capacity = 8;
	while (capacity < (size_t)expected * 2u + 1u) capacity *= 2u;
	if (capacity > slots.size())
	{
		slots.assign(capacity, {});
		stamp = 1;
	}
	else if (++stamp == 0)
	{
		for (auto& slot : slots) slot.stamp = 0;
		stamp = 1;
	}
	size = 0;
}

uint32_t NRIVoxelFrameKeyCounts::Add(uint64_t key, uint64_t second)
{
	if (slots.empty()) Begin(1);
	size_t index = (size_t)Hash(key, second) & (slots.size() - 1u);
	for (;;)
	{
		auto& slot = slots[index];
		if (slot.stamp != stamp)
		{
			slot = {key, second, stamp, 1}; ++size; return 1;
		}
		if (slot.key == key && slot.second == second) return ++slot.count;
		index = (index + 1u) & (slots.size() - 1u);
	}
}

uint32_t NRIVoxelFrameKeyCounts::Count(uint64_t key, uint64_t second) const
{
	if (slots.empty()) return 0;
	size_t index = (size_t)Hash(key, second) & (slots.size() - 1u);
	for (;;)
	{
		const auto& slot = slots[index];
		if (slot.stamp != stamp) return 0;
		if (slot.key == key && slot.second == second) return slot.count;
		index = (index + 1u) & (slots.size() - 1u);
	}
}
