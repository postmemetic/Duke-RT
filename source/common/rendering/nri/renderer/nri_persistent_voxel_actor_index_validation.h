#pragma once

#include "nri_persistent_voxels.h"
#include "nri_cvars.h"
#include "engineerrors.h"
#include "printf.h"

#include <optional>

// Session-only, independent original full-vector oracles. Failure never repairs
// the candidate: reference runs must fail rather than mask an indexing defect.
namespace nri_voxel_actor_reference
{
enum Stage { Lookup, PreviousActive, Instances, MaterialKeys, BatchState, ResourceReferences, Publication, MaterialRebind, Count };
inline void Check(bool equal, Stage stage, const char* reason, uint64_t rows)
{
	static const char* names[] = { "lookup", "previous-active", "instances", "material-keys", "batch-state", "resource-references", "publication", "material-rebind" };
	if (!equal) I_FatalError("NRI PT voxel actor index reference mismatch: stage=%s reason=%s", names[stage], reason);
	static uint64_t calls[Count] = {}, visited[Count] = {};
	const uint64_t n = ++calls[stage]; visited[stage] += rows;
	if ((n & (n - 1)) == 0)
		Printf("PERF pt voxel actor index reference NRI: stage=%s checks=%llu rows=%llu mismatches=0\n",
			names[stage], (unsigned long long)n, (unsigned long long)visited[stage]);
}

inline void Index(const PersistentVoxelBatch& batch)
{
	if (!(bool)nri_ptvoxelactorindexvalidate) return;
	std::unordered_map<uint64_t, uint32_t> slots;
	std::vector<uint32_t> active;
	for (uint32_t i = 0; i < batch.actors.size(); ++i)
	{
		slots[batch.actors[i].identityKey] = i;
		if (batch.actors[i].active) active.push_back(i);
	}
	auto bindings = batch.actorIndex.PendingMaterialBindings();
	std::sort(bindings.begin(), bindings.end());
	bool completeBindings = std::adjacent_find(bindings.begin(), bindings.end()) == bindings.end() &&
		(bindings.empty() || bindings.back() < batch.actors.size());
	for (uint32_t i = 0; i < batch.actors.size(); ++i)
		if (batch.actors[i].active || batch.actors[i].materialOffset || batch.actors[i].materialSlotGeneration)
			completeBindings = completeBindings && std::binary_search(bindings.begin(), bindings.end(), i);
	Check(slots == batch.actorIndex.Slots() && active == batch.actorIndex.Active() && completeBindings,
		Lookup, "lookup-membership-binding-coverage", batch.actors.size());
}

class Setup
{
	struct Snapshot
	{
		std::unordered_set<uint64_t> previous;
		std::unordered_map<uint64_t, uint32_t> expectedSlots;
		std::unordered_map<uint64_t, bool> expectedInstances;
		uint64_t rows = 0;
	};
public:
	Setup(const PersistentVoxelBatch& batch, const std::unordered_map<uint64_t, PersistentVoxelInstanceRecord>& instances)
	{
		if (!(bool)nri_ptvoxelactorindexvalidate) return;
		// MSVC unordered containers allocate even when default-constructed empty.
		// Construct the complete original-path snapshot only for validation.
		auto& snapshot = mSnapshot.emplace();
		for (const auto& pair : instances) snapshot.expectedInstances.emplace(pair.first, pair.second.active);
		for (uint32_t slot = 0; slot < batch.actors.size(); ++slot)
		{
			const auto& actor = batch.actors[slot];
			snapshot.expectedSlots[actor.identityKey] = slot;
			if (actor.active) snapshot.previous.insert(actor.identityKey);
			const auto found = snapshot.expectedInstances.find(actor.identityKey);
			if (found != snapshot.expectedInstances.end()) found->second = false;
		}
		snapshot.rows = batch.actors.size();
	}
	void VerifyLookup(uint64_t identity, uint32_t actualSlot) const
	{
		if (!mSnapshot) return;
		const auto found = mSnapshot->expectedSlots.find(identity);
		const uint32_t expected = found == mSnapshot->expectedSlots.end() ? UINT32_MAX : found->second;
		Check(expected == actualSlot, Lookup, "snapshot-existing-actor-slot", 1);
	}
	void Verify(const std::unordered_set<uint64_t>& actualPrevious,
		const std::unordered_map<uint64_t, PersistentVoxelInstanceRecord>& instances) const
	{
		if (!mSnapshot) return;
		Check(mSnapshot->previous == actualPrevious, PreviousActive, "previous-active-identities", mSnapshot->rows);
		bool equal = mSnapshot->expectedInstances.size() == instances.size();
		for (const auto& expected : mSnapshot->expectedInstances)
		{
			const auto actual = instances.find(expected.first);
			equal = equal && actual != instances.end() && actual->second.active == expected.second;
		}
		Check(equal, Instances, "historical-instance-deactivation", instances.size());
	}
private:
	std::optional<Snapshot> mSnapshot;
};

template<class Current> inline void Materials(const PersistentVoxelBatch& batch,
	const std::unordered_set<uint64_t>& actual, Current current)
{
	if (!(bool)nri_ptvoxelactorindexvalidate) return;
	std::unordered_set<uint64_t> expected;
	for (const auto& actor : batch.actors)
		if (actor.active && current(actor) && actor.materialKeyHash != 0) expected.insert(actor.materialKeyHash);
	Check(expected == actual, MaterialKeys, "active-material-keys", batch.actors.size());
}

template<class Current> inline void State(const PersistentVoxelBatch& batch, Current current)
{
	if (!(bool)nri_ptvoxelactorindexvalidate) return;
	uint32_t active = 0, primitives = 0, indices = 0;
	for (const auto& actor : batch.actors) if (actor.active && current(actor))
	{
		++active; primitives += actor.primitiveCount; indices += actor.indexCount;
	}
	Check(active == batch.activeActorCount && primitives == batch.primitiveCount && indices == batch.indexCount,
		BatchState, "active-state-totals", batch.actors.size());
}

template<class Current> inline void References(const PersistentVoxelBatch& batch,
	const std::unordered_map<uint64_t, uint32_t>& meshes,
	const std::unordered_map<uint64_t, uint32_t>& materials, Current current)
{
	if (!(bool)nri_ptvoxelactorindexvalidate) return;
	std::unordered_map<uint64_t, uint32_t> expectedMeshes, expectedMaterials;
	for (const auto& actor : batch.actors) if (actor.active && current(actor))
	{
		if (actor.meshResourceKey != 0) ++expectedMeshes[actor.meshResourceKey];
		if (actor.materialKeyHash != 0) ++expectedMaterials[actor.materialKeyHash];
	}
	Check(expectedMeshes == meshes && expectedMaterials == materials,
		ResourceReferences, "resource-reference-maps", batch.actors.size());
}

inline void Published(const PersistentVoxelBatch& batch,
	const std::unordered_map<uint64_t, uint32_t>& actual, bool duplicate)
{
	if (!(bool)nri_ptvoxelactorindexvalidate) return;
	std::unordered_map<uint64_t, uint32_t> expected;
	bool expectedDuplicate = false;
	for (uint32_t i = 0; i < batch.actors.size(); ++i)
		if (batch.actors[i].active && !expected.emplace(batch.actors[i].identityKey, i).second)
		{
			expectedDuplicate = true;
			break;
		}
	Check(expected == actual && expectedDuplicate == duplicate,
		Publication, "publication-prefix-and-duplicate", batch.actors.size());
}

class Bindings
{
	struct Row { uint32_t offset, count; uint64_t generation; nri_scene::MaterialBridgeData bridge; };
	struct Snapshot
	{
		std::vector<Row> expected;
		uint32_t rebinds = 0;
	};
public:
	template<class Resources, class Current> Bindings(const PersistentVoxelBatch& batch,
		const Resources& resources, Current current)
	{
		if (!(bool)nri_ptvoxelactorindexvalidate) return;
		auto& snapshot = mSnapshot.emplace();
		auto& expected = snapshot.expected;
		expected.reserve(batch.actors.size());
		for (const auto& actor : batch.actors)
		{
			Row row{actor.materialOffset, actor.materialCount, actor.materialSlotGeneration, actor.materialBridge};
			if (!actor.active || !current(actor)) { row.offset = 0; row.generation = 0; }
			else
			{
				const auto resource = resources.find(actor.materialKeyHash);
				if (resource != resources.end() && resource->second.materialCount != 0)
				{
					const auto& value = resource->second;
					if (row.offset != value.materialOffset || row.count != value.materialCount || row.generation != value.materialSlotGeneration)
					{
						row.offset = value.materialOffset; row.count = value.materialCount; row.generation = value.materialSlotGeneration;
						row.bridge = value.materialBridge; ++snapshot.rebinds;
					}
				}
			}
			expected.push_back(std::move(row));
		}
	}
	void Verify(const PersistentVoxelBatch& batch, uint32_t actualRebinds) const
	{
		if (!mSnapshot) return;
		const auto& expected = mSnapshot->expected;
		bool equal = expected.size() == batch.actors.size() && mSnapshot->rebinds == actualRebinds;
		for (size_t i = 0; equal && i < expected.size(); ++i)
		{
			const auto& row = expected[i]; const auto& actor = batch.actors[i];
			equal = row.offset == actor.materialOffset && row.count == actor.materialCount && row.generation == actor.materialSlotGeneration &&
				nri_scene::MaterialBridgesEquivalent(row.bridge, actor.materialBridge);
		}
		Check(equal, MaterialRebind, "complete-material-binding-products", batch.actors.size());
	}
private:
	std::optional<Snapshot> mSnapshot;
};
}
