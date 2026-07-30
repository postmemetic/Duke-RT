#pragma once

#include "../system/nri_local.h"
#include "nri_scene_texture_slot_table.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

class NRIRenderDevice;

namespace nri_scene
{
	struct MaterialData;
	struct MaterialBridgeData;
	struct TextureUpload;
}

// Runtime texture misses deliberately publish a fail-closed material row until
// the upload fence completes.  A later same-frame material refresh must preserve
// that row instead of rebuilding a live material against the placeholder
// descriptor.
uint32_t NRIPreservePendingTextureMaterialProxies(
	const std::vector<nri_scene::MaterialData>& publishedMaterials,
	std::vector<nri_scene::MaterialData>& refreshedMaterials,
	const std::vector<uint32_t>& deferredMaterialIndices);

struct NRISceneCachedTexture
{
	uint64_t key = 0;
	uint64_t uploadFenceValue = 0;
	uint64_t firstUseEventId = 0;
	uint64_t firstUseRequestFrame = 0;
	uint64_t estimatedUploadBytes = 0;
	uint32_t firstUseQueuedSlot = UINT32_MAX;
	NRITextureResource resource;
};

struct SceneTextureOverflowDebugStats
{
	uint32_t textureCountLastBuild = 0;
	uint32_t truncatedTextureCountLastBuild = 0;
	uint32_t baseTextureClampCountLastBuild = 0;
	uint32_t normalTextureClampCountLastBuild = 0;
	uint32_t metallicTextureClampCountLastBuild = 0;
	uint32_t roughnessTextureClampCountLastBuild = 0;
	uint32_t emissiveTextureClampCountLastBuild = 0;
	uint64_t totalOverflowBuilds = 0;
	bool warningLogged = false;
};

struct SceneTextureCacheDebugStats
{
	uint32_t cacheEntriesLastBuild = 0;
	uint32_t cacheEntriesHighWater = 0;
	uint32_t lookupMissesLastBuild = 0;
	uint32_t insertCountLastBuild = 0;
	uint32_t transitionCountLastFrame = 0;
	double lookupMsLastBuild = 0.0;
	double realizeMsLastBuild = 0.0;
	double descriptorMsLastBuild = 0.0;
	double transitionMsLastFrame = 0.0;
	uint32_t descriptorWritesLastBuild = 0;
	uint32_t descriptorSkipsLastBuild = 0;
	uint32_t descriptorRowsWrittenLastBuild = 0;
	uint32_t stableSlotModeLastBuild = 0;
	uint32_t stableDescriptorHitsLastBuild = 0;
	uint32_t stableDescriptorMissesLastBuild = 0;
};

enum class NRISceneTextureMissPolicy : uint8_t
{
	Synchronous,
	RuntimeAsyncDeferOverlay,
};

struct NRIMaterialTextureWarmupResult
{
	uint32_t textureRequests = 0;
	uint32_t textureHits = 0;
	uint32_t textureMisses = 0;
	uint32_t textureInserts = 0;
	uint64_t estimatedBytes = 0;
	double realizeMs = 0.0;
	bool pending = false;
	bool textureBudgetHit = false;
	bool byteBudgetHit = false;
	bool msBudgetHit = false;
};

enum class NRISceneTextureClosureState : uint8_t
{
	Pending,
	Ready,
	NotRequired,
	Deferred,
	Failed,
};

enum class NRISceneTextureClosureFailure : uint8_t
{
	None,
	DynamicTexture,
	PayloadUnavailable,
	ResourceCreation,
	DescriptorUnavailable,
	ResidencyLost,
};

struct SceneTextureResolveResult
{
	nri::Descriptor* descriptor = nullptr;
	bool cacheMiss = false;
	bool inserted = false;
	bool pending = false;
	bool activeCanvasSelfReference = false;
	NRISceneTextureClosureState closureState = NRISceneTextureClosureState::Ready;
	NRISceneTextureClosureFailure closureFailure = NRISceneTextureClosureFailure::None;
	double lookupMs = 0.0;
	double realizeMs = 0.0;
};

struct NRISceneTextureClosureResult
{
	uint64_t key = 0;
	uint64_t estimatedBytes = 0;
	uint32_t residencyIndex = UINT32_MAX;
	NRISceneTextureClosureState state = NRISceneTextureClosureState::Pending;
	NRISceneTextureClosureFailure failure = NRISceneTextureClosureFailure::None;
	double realizeMs = 0.0;
	// This is resource-view readiness, not publication into the current scene descriptor table.
	bool descriptorReady = false;
	bool realized = false;
	bool reused = false;

	bool IsReady() const
	{
		return state == NRISceneTextureClosureState::Ready ||
			state == NRISceneTextureClosureState::NotRequired;
	}
};

struct NRIMaterialTextureWarmupOptions
{
	uint32_t maxTextureInserts = 0;
	uint64_t maxUploadBytes = 0;
	double maxMilliseconds = 0.0;
};

struct NRIMaterialTextureWarmupCursor
{
	uint64_t sourceKey = 0;
	uint32_t nextTextureIndex = 0;
	bool ready = false;
};

class NRISceneTextureResidency
{
public:
	NRITextureResource& PaletteTexture() { return mPaletteTexture; }
	const NRITextureResource& PaletteTexture() const { return mPaletteTexture; }

	std::vector<NRISceneCachedTexture>& CachedTextures() { return mTextureCache; }
	const std::vector<NRISceneCachedTexture>& CachedTextures() const { return mTextureCache; }

	SceneTextureOverflowDebugStats& OverflowStats() { return mOverflowStats; }
	const SceneTextureOverflowDebugStats& OverflowStats() const { return mOverflowStats; }

	SceneTextureCacheDebugStats& CacheStats() { return mCacheStats; }
	const SceneTextureCacheDebugStats& CacheStats() const { return mCacheStats; }

	bool& LimitLogPrinted() { return mLimitLogPrinted; }
	bool LimitLogPrinted() const { return mLimitLogPrinted; }

	uint32_t CacheCount() const { return (uint32_t)mTextureCache.size(); }
	NRISceneTextureSlotTable& SlotTable() { return mSlotTable; }
	const NRISceneTextureSlotTable& SlotTable() const { return mSlotTable; }
	uint32_t FindCacheIndex(uint64_t key) const;
	uint32_t FindReadyCacheIndex(uint64_t key) const;
	uint32_t AddCachedTexture(NRISceneCachedTexture&& texture);

	bool EnsurePaletteTexture(NRIRenderDevice& device, const nri_scene::MaterialBridgeData& materials);
	bool EnsureCacheEntry(NRIRenderDevice& device, const nri_scene::TextureUpload& upload, double* outRealizeMs = nullptr);
	bool EnsurePreloadClosure(NRIRenderDevice& device, const nri_scene::TextureUpload& upload, NRISceneTextureClosureResult& outResult);
	bool EnsureRuntimeClosure(NRIRenderDevice& device, const nri_scene::TextureUpload& upload, NRISceneTextureClosureResult& outResult);
	bool QueryPreloadClosure(uint64_t key, NRISceneTextureClosureResult& outResult) const;
	bool QueryRuntimeClosure(NRIRenderDevice& device, uint64_t key, NRISceneTextureClosureResult& outResult);
	bool WarmMaterialTextures(NRIRenderDevice& device, const nri_scene::MaterialBridgeData& materials, NRIMaterialTextureWarmupResult& outResult);
	bool WarmMaterialTexturesBudgeted(NRIRenderDevice& device, const nri_scene::MaterialBridgeData& materials, const NRIMaterialTextureWarmupOptions& options, NRIMaterialTextureWarmupCursor& cursor, NRIMaterialTextureWarmupResult& outResult);
	bool ResolveTextureDescriptor(NRIRenderDevice& device, const nri_scene::TextureUpload& upload, bool tracePerf, NRISceneTextureMissPolicy missPolicy, SceneTextureResolveResult& outResult);
	nri::Descriptor* FindStableSlotDescriptor(uint64_t key, NRISceneTextureSlotHandle handle) const;
	void StoreStableSlotDescriptor(uint64_t key, NRISceneTextureSlotHandle handle, nri::Descriptor* descriptor);
	void PopulateStableSlotDescriptors(std::vector<nri::Descriptor*>& descriptors, uint32_t descriptorOffset) const;
	uint32_t TransitionInputsForCompute(NRIRenderDevice& device);

	void ClearLiveResources();
	void TrackLiveResource(NRITextureResource& resource);
	const std::vector<NRITextureResource*>& LiveResources() const { return mLiveResources; }

	void ClearCachedTextures(NRIRenderDevice* device);

private:
	struct StableSlotDescriptorCacheEntry
	{
		uint64_t key = 0;
		NRISceneTextureSlotHandle handle = {};
		nri::Descriptor* descriptor = nullptr;
	};
	enum class CachedTextureReadiness : uint8_t
	{
		Ready,
		Pending,
		Abandoned,
		Failed,
	};

	CachedTextureReadiness PollCachedTexture(
		NRIRenderDevice& device,
		uint32_t cacheIndex,
		NRISceneTextureClosureResult& outResult);
	void InvalidateCachedTexture(NRIRenderDevice& device, uint32_t cacheIndex);

	NRITextureResource mPaletteTexture;
	std::vector<NRISceneCachedTexture> mTextureCache;
	std::unordered_map<uint64_t, uint32_t> mTextureCacheKeyIndex;
	std::vector<NRITextureResource*> mLiveResources;
	SceneTextureOverflowDebugStats mOverflowStats = {};
	SceneTextureCacheDebugStats mCacheStats = {};
	NRISceneTextureSlotTable mSlotTable;
	std::vector<StableSlotDescriptorCacheEntry> mStableSlotDescriptors;
	bool mLimitLogPrinted = false;
};
