#pragma once

#include "nri_persistent_voxel_material_range_allocator.h"
#include "../scene/nri_material_bridge.h"

#include <unordered_map>
#include <unordered_set>

// Only a class with no potentially matching actor material rule may share.
// The generation covers the complete resolved rule set; the world separates
// actor-index namespaces. Neither a current zero override nor a hash is proof.
struct NRIVoxelMaterialSharingCertificate
{
	uint64_t world = 0;
	uint32_t rules = 0;
	bool invariant = false;
	uint64_t actorWorld = 0;
};

struct NRIVoxelMaterialPayloadStats
{
	uint64_t bindings = 0, payloads = 0, retiring = 0;
	uint64_t physicalRows = 0, retiringRows = 0, aliasRows = 0;
	uint64_t descriptorBytes = 0, retiringDescriptorBytes = 0;
	uint64_t eligibleBindings = 0, excludedBindings = 0, excludedRows = 0;
	uint64_t lookups = 0, hits = 0, comparisons = 0, bytesHashed = 0;
};

// Actor-specific bridges remain in occurrence resources. This owner holds the
// immutable, actor-independent payload and its single physical row range.
class NRIVoxelMaterialPayloadOwner
{
public:
	using HashFn = uint64_t (*)(const nri_scene::MaterialBridgeData&, uint64_t*);
	explicit NRIVoxelMaterialPayloadOwner(HashFn hash = nullptr) : hashFunction(hash) {}
	bool AcquireBinding(uint64_t key, const NRIVoxelMaterialSharingCertificate& certificate,
		const nri_scene::MaterialBridgeData& source, NRIPersistentVoxelMaterialRangeAllocator& allocator,
		NRIPersistentVoxelMaterialRangeHandle& result, NRIPersistentVoxelMaterialRangeHandle& retired);
	NRIPersistentVoxelMaterialRangeHandle FindBinding(uint64_t key) const;
	std::vector<uint64_t> BindingsForActor(int32_t actorIndex) const;
	bool BindingIsShared(uint64_t key) const;
	// The last alias leaves the lookup immediately. Retirement cannot resurrect.
	NRIPersistentVoxelMaterialRangeHandle ReleaseBinding(uint64_t key);
	bool ReleaseRetiredRange(NRIPersistentVoxelMaterialRangeHandle range,
		NRIPersistentVoxelMaterialRangeAllocator& allocator);
	bool Compact(NRIPersistentVoxelMaterialRangeAllocator& allocator, uint64_t& movedRows);
	NRIVoxelMaterialPayloadStats Snapshot() const;
	const NRIVoxelMaterialPayloadStats& Counters() const { return counters; }
	void Reset();

private:
	struct Payload
	{
		NRIPersistentVoxelMaterialRangeHandle range;
		NRIVoxelMaterialSharingCertificate certificate;
		nri_scene::MaterialBridgeData bridge;
		uint64_t hash = 0, aliases = 0;
	};
	HashFn hashFunction = nullptr;
	uint64_t nextIdentity = 1;
	std::unordered_map<uint64_t, uint64_t> bindings;
	std::unordered_map<uint64_t, int32_t> bindingActors;
	std::unordered_map<int32_t, std::unordered_set<uint64_t>> actorBindings;
	std::unordered_map<uint64_t, Payload> payloads;
	std::unordered_map<uint64_t, uint64_t> ranges;
	std::unordered_multimap<uint64_t, uint64_t> lookup;
	NRIVoxelMaterialPayloadStats counters;
};
