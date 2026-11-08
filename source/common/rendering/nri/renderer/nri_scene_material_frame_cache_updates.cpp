#include "nri_scene_material_frame_cache.h"
#include "../scene/nri_cpu_perf_audit.h"

#include <algorithm>

bool NRISceneMaterialFrameCache::TryPatchPersistentMaterials(
	const nri_scene::MaterialBridgeData& source,
	uint64_t generation,
	const nri_scene::MaterialBridgeChanges& changes,
	NRISceneMaterialFrameCacheStats& outStats)
{
	if (changes.fullRebuild || changes.beforeGeneration != mPersistentMaterialGeneration ||
		changes.generation != generation || changes.beforeRows != mPersistentMaterialCount ||
		changes.afterRows != source.materials.size() || source.lightMetadata.size() != source.materials.size() ||
		mStaticLightMetadataCount != mStaticMaterialCount || mResidentLightMetadataCount != mResidentMaterialCount ||
		changes.afterRows > UINT32_MAX - mStaticMaterialCount ||
		mCombinedMaterials.textures.size() < mResidentTextureCount)
		return false;
	uint32_t previousEnd = 0;
	for (const auto range : changes.rows)
	{
		if (range.first < previousEnd || range.first > changes.afterRows || range.count > changes.afterRows - range.first)
			return false;
		previousEnd = range.first + range.count;
	}
	// New rows must all be represented by the producer's change set.
	if (changes.afterRows > changes.beforeRows &&
		(changes.rows.empty() || changes.rows.back().first > changes.beforeRows || previousEnd != changes.afterRows))
		return false;
	std::unordered_map<uint64_t, uint32_t> textureLookup;
	for (uint32_t index = 0; index < mResidentTextureCount; ++index)
		textureLookup.emplace(mCombinedMaterials.textures[index].key, index);
	for (const auto& texture : source.textures)
		if (textureLookup.find(texture.key) == textureLookup.end()) return false;

	const uint32_t rowCount = mStaticMaterialCount + changes.afterRows;
	mCombinedMaterials.materials.resize(rowCount);
	mCombinedMaterials.lightMetadata.resize(rowCount);
	mCombinedMaterials.textures.resize(mResidentTextureCount);
	for (const auto& texture : source.textures)
	{
		const uint32_t index = textureLookup.find(texture.key)->second;
		// AppendMaterialBridge gives the existing static descriptor priority.
		if (index >= mStaticTextureCount) mCombinedMaterials.textures[index] = texture;
	}
	uint32_t copiedRows = 0;
	for (const auto range : changes.rows)
	{
		nri_scene::CopyMaterialBridgeRowRange(source, range, mCombinedMaterials,
			mStaticMaterialCount + range.first, textureLookup);
		mResolvedDirtyRows.push_back({mStaticMaterialCount + range.first, range.count});
		copiedRows += range.count;
	}
	// Build may run repeatedly before texture readiness permits Resolve. Keep
	// pending coverage bounded by the current row span, not publication count.
	// A later regrowth must mark its complete new extent dirty (checked above),
	// so clipping a shrunken tail here cannot lose required resolved updates.
	nri_scene::MergeMaterialRowRanges(mResolvedDirtyRows, rowCount);
	mPersistentMaterialGeneration = generation;
	mPersistentMaterialCount = changes.afterRows;
	mResidentMaterialCount = rowCount;
	mResidentLightMetadataCount = rowCount;
	outStats.residentHits = 1;
	outStats.residentRowsReused = rowCount - copiedRows - outStats.staticRowsCopied;
	PerfAuditAdd(PerfAuditMetric::MaterialFramePersistentPatches);
	PerfAuditAdd(PerfAuditMetric::MaterialFramePersistentRowsPatched, copiedRows);
	PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeRowsCopied, copiedRows);
	PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeBytesCopied,
		(uint64_t)copiedRows * (sizeof(nri_scene::MaterialData) + sizeof(nri_scene::MaterialLightingMetadata)));
	return true;
}
