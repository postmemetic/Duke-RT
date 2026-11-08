#pragma once

#include "../scene/nri_material_bridge_ranges.h"

struct NRIVoxelMaterialBridgeResourceView
{
	uint64_t key = 0;
	uint64_t slotGeneration = 0;
	uint64_t contentRevision = 0;
	uint32_t offset = 0;
	uint32_t count = 0;
	bool dirty = false;
	const nri_scene::MaterialBridgeData* bridge = nullptr;
};

// Owns only the material table's update history. Residency remains authority for
// active resources, global offsets, actor bindings and GPU upload transactions.
class NRIPersistentVoxelMaterialBridge
{
public:
	void Update(const std::vector<NRIVoxelMaterialBridgeResourceView>& resources,
		uint32_t reserveRows, uint64_t beforeGeneration, uint64_t generation,
		nri_scene::MaterialBridgeData& bridge, bool validate = false);
	const nri_scene::MaterialBridgeChanges& Changes() const { return mChanges; }
	const std::vector<nri_scene::MaterialRowRange>& UniformDefaultRanges() const { return mUniformDefaultRanges; }

private:
	struct ResourceState
	{
		uint64_t key = 0;
		uint64_t slotGeneration = 0;
		uint64_t contentRevision = 0;
		uint32_t offset = 0;
		uint32_t count = 0;
	};
	std::vector<ResourceState> mResources;
	nri_scene::MaterialBridgeChanges mChanges;
	std::vector<nri_scene::MaterialRowRange> mUniformDefaultRanges;
	bool mValid = false;
	bool mQuarantined = false;
};
