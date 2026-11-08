#include "nri_scene_material_frame_cache.h"
#include "../scene/nri_cpu_perf_audit.h"

#include <algorithm>

const nri_scene::MaterialBridgeData& NRISceneMaterialFrameCache::Build(
	const nri_scene::MaterialBridgeData& staticMaterials,
	uint64_t staticMaterialIdentity,
	uint64_t staticMaterialGeneration,
	const nri_scene::MaterialBridgeData* persistentMaterials,
	uint64_t persistentMaterialGeneration,
	const nri_scene::MaterialBridgeData& overlayMaterials,
	NRISceneMaterialFrameCacheStats& outStats)
{
	outStats = {};
	const bool hasPersistentMaterials = persistentMaterials != nullptr;
	const bool staticLayoutCompatible = [&]()
	{
		if (!mResidentValid || mStaticMaterialIdentity != staticMaterialIdentity ||
			mHasPersistentMaterials != hasPersistentMaterials ||
			(hasPersistentMaterials && mPersistentMaterialGeneration != persistentMaterialGeneration) ||
			mStaticMaterialCount != staticMaterials.materials.size() ||
			mStaticLightMetadataCount != staticMaterials.lightMetadata.size() ||
			mStaticTextureCount != staticMaterials.textures.size() ||
			mCombinedMaterials.textures.size() < mStaticTextureCount ||
			mCombinedMaterials.paletteWidth != staticMaterials.paletteWidth ||
			mCombinedMaterials.paletteHeight != staticMaterials.paletteHeight ||
			mCombinedMaterials.paletteLookup != staticMaterials.paletteLookup)
		{
			return false;
		}
		for (uint32_t i = 0; i < mStaticTextureCount; ++i)
		{
			if (mCombinedMaterials.textures[i].key != staticMaterials.textures[i].key)
				return false;
		}
		return true;
	}();
	if (staticLayoutCompatible && mStaticMaterialGeneration != staticMaterialGeneration)
	{
		PerfAuditAdd(PerfAuditMetric::MaterialFrameStaticPrefixChanges);
		mCombinedMaterials.materials.resize(mResidentMaterialCount);
		mCombinedMaterials.lightMetadata.resize(mResidentLightMetadataCount);
		mCombinedMaterials.textures.resize(mResidentTextureCount);
		NRIAuditFrameBridgeCopy(staticMaterials);
		std::copy(staticMaterials.materials.begin(), staticMaterials.materials.end(), mCombinedMaterials.materials.begin());
		std::copy(staticMaterials.lightMetadata.begin(), staticMaterials.lightMetadata.end(), mCombinedMaterials.lightMetadata.begin());
		std::copy(staticMaterials.textures.begin(), staticMaterials.textures.end(), mCombinedMaterials.textures.begin());
		mStaticMaterialGeneration = staticMaterialGeneration;
		// Metadata is indexed by material row during slot resolution. Keep the
		// original full path for any bridge whose segment boundaries do not align.
		if (mResolvedResidentValid &&
			mStaticLightMetadataCount == mStaticMaterialCount &&
			mResidentLightMetadataCount == mResidentMaterialCount &&
			mResolvedMaterials.materials.size() >= mResidentMaterialCount &&
			mResolvedMaterials.lightMetadata.size() >= mResidentLightMetadataCount)
		{
			mResolvedStaticPrefixDirty = true;
		}
		else
		{
			mResolvedResidentValid = false;
			mResolvedStaticPrefixDirty = false;
			PerfAuditAdd(PerfAuditMetric::MaterialFrameStaticPrefixFallbacks);
		}
		outStats.residentHits = 1;
		outStats.staticRowsCopied = mStaticMaterialCount;
		outStats.residentRowsReused = mResidentMaterialCount - mStaticMaterialCount;
	}
	const bool rebuildResident =
		!mResidentValid ||
		mStaticMaterialIdentity != staticMaterialIdentity ||
		mStaticMaterialGeneration != staticMaterialGeneration ||
		mHasPersistentMaterials != hasPersistentMaterials ||
		(hasPersistentMaterials && mPersistentMaterialGeneration != persistentMaterialGeneration);

	if (rebuildResident)
	{
		NRIAuditFrameBridgeCopy(staticMaterials);
		mCombinedMaterials = staticMaterials;
		outStats.staticRowsCopied = (uint32_t)staticMaterials.materials.size();
		if (hasPersistentMaterials)
		{
			const uint64_t auditBytesBefore = PerfAuditActive() ? NRIAuditMaterialBridgeBytes(mCombinedMaterials) : 0;
			nri_scene::AppendMaterialBridge(*persistentMaterials, mCombinedMaterials);
			if (PerfAuditActive())
			{
				PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeRowsCopied, persistentMaterials->materials.size());
				PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeBytesCopied, NRIAuditMaterialBridgeBytes(mCombinedMaterials) - auditBytesBefore);
			}
			outStats.persistentRowsAppended = (uint32_t)persistentMaterials->materials.size();
		}

		mStaticMaterialIdentity = staticMaterialIdentity;
		mStaticMaterialGeneration = staticMaterialGeneration;
		mPersistentMaterialGeneration = hasPersistentMaterials ? persistentMaterialGeneration : 0;
		mHasPersistentMaterials = hasPersistentMaterials;
		mPersistentMaterialCount = hasPersistentMaterials ? (uint32_t)persistentMaterials->materials.size() : 0u;
		mResidentMaterialCount = (uint32_t)mCombinedMaterials.materials.size();
		mResidentLightMetadataCount = (uint32_t)mCombinedMaterials.lightMetadata.size();
		mResidentTextureCount = (uint32_t)mCombinedMaterials.textures.size();
		mStaticMaterialCount = (uint32_t)staticMaterials.materials.size();
		mStaticLightMetadataCount = (uint32_t)staticMaterials.lightMetadata.size();
		mStaticTextureCount = (uint32_t)staticMaterials.textures.size();
		mResidentValid = true;
		mResolvedResidentValid = false;
		mResolvedStaticPrefixDirty = false;
		outStats.residentRebuilds = 1;
	}
	else if (outStats.residentHits == 0)
	{
		mCombinedMaterials.materials.resize(mResidentMaterialCount);
		mCombinedMaterials.lightMetadata.resize(mResidentLightMetadataCount);
		mCombinedMaterials.textures.resize(mResidentTextureCount);
		outStats.residentHits = 1;
		outStats.residentRowsReused = mResidentMaterialCount;
	}

	if (!overlayMaterials.materials.empty() || !overlayMaterials.textures.empty())
	{
		nri_scene::AppendMaterialBridge(overlayMaterials, mCombinedMaterials);
		outStats.overlayRowsAppended = (uint32_t)overlayMaterials.materials.size();
	}
	return mCombinedMaterials;
}

const nri_scene::MaterialBridgeData& NRISceneMaterialFrameCache::ResolveTextureSlots(
	const NRISceneTextureSlotTable& slotTable)
{
	PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveCalls);
	const bool rebuildResident =
		!mResolvedResidentValid ||
		mResolvedTextureSlotRevision != slotTable.MappingRevision();
	if (rebuildResident)
	{
		if (mResolvedStaticPrefixDirty)
			PerfAuditAdd(PerfAuditMetric::MaterialFrameStaticPrefixFallbacks);
		PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRebuilds);
		if (!mResolvedResidentValid) PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveInvalidResident);
		if (mResolvedTextureSlotRevision != slotTable.MappingRevision())
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveSlotRevisionChanges);
		PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsCopied, mCombinedMaterials.materials.size());
		NRIAuditFrameBridgeCopy(mCombinedMaterials);
		mResolvedMaterials = mCombinedMaterials;
		mResolvedMaterials.materials.resize(mResidentMaterialCount);
		mResolvedMaterials.lightMetadata.resize(mResidentLightMetadataCount);
		PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsRemapped, mResolvedMaterials.materials.size());
		NRIResolveSceneMaterialBridgeTextureSlots(slotTable, mResolvedMaterials, 0u);
		mResolvedResidentValid = true;
		mResolvedTextureSlotRevision = slotTable.MappingRevision();
		mResolvedStaticPrefixDirty = false;
	}
	else
	{
		PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveHits);
		mResolvedMaterials.materials.resize(mResidentMaterialCount);
		mResolvedMaterials.lightMetadata.resize(mResidentLightMetadataCount);
		mResolvedMaterials.textures = mCombinedMaterials.textures;
		mResolvedMaterials.paletteLookup = mCombinedMaterials.paletteLookup;
		mResolvedMaterials.paletteWidth = mCombinedMaterials.paletteWidth;
		mResolvedMaterials.paletteHeight = mCombinedMaterials.paletteHeight;
		if (mResolvedStaticPrefixDirty)
		{
			std::copy_n(mCombinedMaterials.materials.begin(), mStaticMaterialCount, mResolvedMaterials.materials.begin());
			std::copy_n(mCombinedMaterials.lightMetadata.begin(), mStaticLightMetadataCount, mResolvedMaterials.lightMetadata.begin());
			NRIResolveSceneMaterialBridgeTextureSlotRange(slotTable, mResolvedMaterials, 0u, mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveStaticPatches);
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveStaticRowsCopied, mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolvePersistentRowsPreserved, mResidentMaterialCount - mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsCopied, mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsRemapped, mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeRowsCopied, mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeBytesCopied,
				(uint64_t)mStaticMaterialCount * sizeof(nri_scene::MaterialData) +
				(uint64_t)mStaticLightMetadataCount * sizeof(nri_scene::MaterialLightingMetadata));
			mResolvedStaticPrefixDirty = false;
		}
	}

	const uint32_t firstOverlayRow = (uint32_t)mResolvedMaterials.materials.size();
	PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsCopied, mCombinedMaterials.materials.size() - mResidentMaterialCount);
	mResolvedMaterials.materials.insert(
		mResolvedMaterials.materials.end(),
		mCombinedMaterials.materials.begin() + mResidentMaterialCount,
		mCombinedMaterials.materials.end());
	mResolvedMaterials.lightMetadata.insert(
		mResolvedMaterials.lightMetadata.end(),
		mCombinedMaterials.lightMetadata.begin() + mResidentLightMetadataCount,
		mCombinedMaterials.lightMetadata.end());
	PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsRemapped, mResolvedMaterials.materials.size() - firstOverlayRow);
	NRIResolveSceneMaterialBridgeTextureSlots(slotTable, mResolvedMaterials, firstOverlayRow);
	return mResolvedMaterials;
}

void NRISceneMaterialFrameCache::Reset()
{
	mCombinedMaterials = {};
	mResolvedMaterials = {};
	mStaticMaterialIdentity = 0;
	mStaticMaterialGeneration = 0;
	mPersistentMaterialGeneration = 0;
	mResidentMaterialCount = 0;
	mResidentLightMetadataCount = 0;
	mResidentTextureCount = 0;
	mStaticMaterialCount = 0;
	mStaticLightMetadataCount = 0;
	mStaticTextureCount = 0;
	mPersistentMaterialCount = 0;
	mHasPersistentMaterials = false;
	mResidentValid = false;
	mResolvedResidentValid = false;
	mResolvedStaticPrefixDirty = false;
	mResolvedTextureSlotRevision = 0;
}
