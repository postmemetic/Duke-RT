#pragma once

#include "../scene/nri_material_bridge.h"
#include "../scene/nri_material_bridge_ranges.h"
#include "nri_scene_material_texture_slots.h"

#include <cstdint>

struct NRISceneMaterialFrameCacheStats
{
	uint32_t residentRebuilds = 0;
	uint32_t residentHits = 0;
	uint32_t staticRowsCopied = 0;
	uint32_t persistentRowsAppended = 0;
	uint32_t overlayRowsAppended = 0;
	uint32_t residentRowsReused = 0;
};

class NRISceneMaterialFrameCache
{
public:
	const nri_scene::MaterialBridgeData& Build(
		const nri_scene::MaterialBridgeData& staticMaterials,
		uint64_t staticMaterialIdentity,
		uint64_t staticMaterialGeneration,
		const nri_scene::MaterialBridgeData* persistentMaterials,
		uint64_t persistentMaterialGeneration,
		const nri_scene::MaterialBridgeData& overlayMaterials,
		NRISceneMaterialFrameCacheStats& outStats,
		const nri_scene::MaterialBridgeChanges* persistentChanges = nullptr,
		const std::vector<nri_scene::MaterialRowRange>* persistentUniformDefaultRanges = nullptr,
		bool validateRangeUpdates = false);

	void Reset();
	nri_scene::MaterialBridgeData& Materials() { return mCombinedMaterials; }
	uint32_t PersistentMaterialCount() const { return mPersistentMaterialCount; }
	uint32_t StaticMaterialCount() const { return mStaticMaterialCount; }
	const nri_scene::MaterialBridgeData& ResolveTextureSlots(const NRISceneTextureSlotTable& slotTable);
	const std::vector<nri_scene::MaterialRowRange>& UniformDefaultRanges() const { return mUniformDefaultRanges; }

private:
	bool TryPatchPersistentMaterials(const nri_scene::MaterialBridgeData& source,
		uint64_t generation, const nri_scene::MaterialBridgeChanges& changes,
		NRISceneMaterialFrameCacheStats& outStats);
	nri_scene::MaterialBridgeData mCombinedMaterials;
	nri_scene::MaterialBridgeData mResolvedMaterials;
	uint64_t mStaticMaterialIdentity = 0;
	uint64_t mStaticMaterialGeneration = 0;
	uint64_t mPersistentMaterialGeneration = 0;
	uint32_t mResidentMaterialCount = 0;
	uint32_t mResidentLightMetadataCount = 0;
	uint32_t mResidentTextureCount = 0;
	uint32_t mStaticMaterialCount = 0;
	uint32_t mStaticLightMetadataCount = 0;
	uint32_t mStaticTextureCount = 0;
	uint32_t mPersistentMaterialCount = 0;
	bool mHasPersistentMaterials = false;
	bool mResidentValid = false;
	bool mResolvedResidentValid = false;
	// The combined static prefix changed without changing its texture namespace
	// or the persistent segment. Resolve must copy clean indices before remapping.
	bool mResolvedStaticPrefixDirty = false;
	bool mValidateRangeUpdates = false;
	bool mRangeUpdatesQuarantined = false;
	std::vector<nri_scene::MaterialRowRange> mResolvedDirtyRows;
	std::vector<nri_scene::MaterialRowRange> mUniformDefaultRanges;
	uint64_t mResolvedTextureSlotRevision = 0;
};
