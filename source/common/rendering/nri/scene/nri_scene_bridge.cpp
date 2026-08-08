#include "nri_scene_bridge.h"
#include "../renderer/nri_cvars.h"

#include "nri_geometry_bridge.h"
#include "nri_portal_bridge.h"
#include "nri_surface_builder.h"
#include "nri_scene_texture_utils.h"
#include "nri_texture_signature.h"
#include "nri_voxel_geometry_hash.h"
#include "nri_voxel_actor_cache_maintenance.h"
#include "../renderer/nri_voxel_compute_meshing.h"

#include "actor_lifecycle_journal.h"
#include "actor_presentation_snapshot.h"
#include "c_cvars.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "gameupdate.h"
#include "filesystem.h"
#include "files.h"
#include "gamecontrol.h"
#include "hw_portal.h"
#include "hw_voxels.h"
#include "image.h"
#include "mapinfo.h"
#include "model_kvx.h"
#include "skyboxtexture.h"
#include "gametexture.h"
#include "texturemanager.h"
#include "texinfo.h"
#include "textures.h"
#include "v_video.h"
#include "perf_capture.h"
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <typeinfo>
#include <vector>
#include <windows.h>


namespace
{
	using namespace nri_scene;

	constexpr float kAttachedWallSpriteDepthNudge = 0.01f;
	constexpr uint32_t kTransientVoxelLiveSurfacePrimitiveLimit = 20000;

	SkyPerfStats gSkyPerfStats = {};
	DynamicCapturePerfStats gDynamicCapturePerfStats = {};
	VoxelMeshPrecacheStats gVoxelLoadingWarmupStats = {};
	uint32_t gVoxelMeshCacheFrame = 0;
	uint32_t gVoxelMeshCacheCaptureDepth = 0;
	uint32_t gVoxelMeshBuildsThisFrame = 0;

	double DurationMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	bool IsLocalPlayerActor(const DCoreActor* actor)
	{
		if (actor == nullptr ||
			myconnectindex < 0 ||
			myconnectindex >= MAXPLAYERS)
		{
			return false;
		}

		DCorePlayer* localPlayer = PlayerArray[myconnectindex];
		return localPlayer != nullptr && localPlayer->GetActor() == actor;
	}

	class ScopedDynamicCaptureTimer
	{
	public:
		explicit ScopedDynamicCaptureTimer(double& targetMs)
			: mTarget(&targetMs), mStart(std::chrono::steady_clock::now())
		{
		}

		~ScopedDynamicCaptureTimer()
		{
			*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	struct AverageColorCacheEntry
	{
		bool success = false;
		float color[3] = {};
	};

	struct CachedSkyInspection
	{
		bool valid = false;
		bool hasAverageColor = false;
		bool isCubemap = false;
		bool isThreeFace = false;
		bool flipTop = false;
		uint32_t faceMask = 0;
		float color[3] = {};
	};

	std::unordered_map<const FTexture*, AverageColorCacheEntry> gFrameLocalAverageTextureColorCache;
	std::unordered_map<uint64_t, AverageColorCacheEntry> gPersistentAverageTextureColorCache;
	std::unordered_map<const FGameTexture*, CachedSkyInspection> gSkyInspectionCache;
	struct VoxelMeshCacheEntry
	{
		FVoxelMeshData mesh;
		uint32_t deferredFrame = 0;
		bool built = false;
		bool valid = false;
	};

	struct VoxelMeshVariantSurfaceCacheEntry
	{
		SurfaceRef canonicalSurface;
		uint64_t meshVariantHash = 0;
		uint64_t geometryContentHash = 0;
		uint64_t renderPrimitiveHash = 0;
		int32_t sourcePicnum = -1;
		int32_t resolvedVoxelIndex = -1;
		bool built = false;
		bool valid = false;
	};

	std::unordered_map<const FVoxelModel*, VoxelMeshCacheEntry> gVoxelMeshCache;
	std::unordered_map<uint64_t, VoxelMeshVariantSurfaceCacheEntry> gVoxelMeshVariantSurfaceCache;
	uint64_t gVoxelActorCacheFrame = 0;
	uint32_t gVoxelActorCacheCaptureDepth = 0;
	uint64_t gVoxelActorCacheSerial = 1;
	bool gVoxelActorStartupTransientMode = false;
	uint64_t gVoxelActorLifecycleCursor = 0;
	bool gVoxelActorPendingRemoval = false;
	NRIVoxelActorMaintenanceGate gVoxelActorMaintenanceGate;
	std::vector<ActorLifecycleEvent> gVoxelActorLifecycleEvents;

	struct VoxelMeshVariantKey
	{
		const voxmodel_t* voxel = nullptr;
		const FVoxelModel* model = nullptr;
		FTextureID sourcePicnum = {};
		int resolvedVoxelIndex = -1;
		uint32_t geometryState = 0;
	};

	struct VoxelMaterialVariantKey
	{
		FGameTexture* voxelTexture = nullptr;
		FGameTexture* emissiveSourceTexture = nullptr;
		int palette = 0;
		int shade = 0;
		uint32_t alphaBits = 0;
		uint32_t materialFlags = 0;
	};

	struct VoxelActorCacheEntry
	{
		uint64_t signature = 0;
		uint64_t geometrySignature = 0;
		uint64_t surfaceSignature = 0;
		uint64_t bakedSurfaceSignature = 0;
		uint64_t materialSignature = 0;
		uint64_t transformBasisSignature = 0;
		uint64_t identityKey = 0;
		uint64_t ownerWorldEpoch = 0;
		uint64_t ownerLifetimeGeneration = 0;
		uint64_t placementGeneration = 0;
		uint64_t placementStateHash = 0;
		uint64_t presentationBasisStateHash = 0;
		uint64_t meshKeyHash = 0;
		uint64_t materialKeyHash = 0;
		uint64_t geometryContentHash = 0;
		uint64_t renderPrimitiveHash = 0;
		uint64_t meshVariantHash = 0;
		uint64_t materialVariantHash = 0;
		uint64_t instanceKeyHash = 0;
		VoxelMeshBakeSpace meshBakeSpace = VoxelMeshBakeSpace::Unknown;
		uint64_t desiredSignature = 0;
		uint64_t desiredMeshKeyHash = 0;
		uint64_t desiredMaterialKeyHash = 0;
		uint64_t desiredMeshVariantHash = 0;
		uint64_t desiredMaterialVariantHash = 0;
		uint64_t desiredSurfaceSignature = 0;
		uint32_t desiredPrimitiveCount = 0;
		uint64_t pendingFrame = 0;
		uint64_t surfaceFrame = 0;
		uint8_t pendingReason = 0;
		int32_t actorIndex = -1;
		int32_t physicalSectorIndex = -1;
		uintptr_t voxelPtr = 0;
		uintptr_t voxelModelPtr = 0;
		int32_t sourcePicnum = -1;
		int32_t resolvedVoxelIndex = -1;
		SurfaceRef surface;
		uint64_t lastSeenFrame = 0;
		uint32_t primitiveCount = 0;
		float currentTranslation[3] = {};
		float bakedTranslation[3] = {};
		float currentTransform[12] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		float lastActorScenePosition[3] = {};
		bool persistentReady = false;
		bool hasSurface = false;
		bool startupPending = false;
		bool sharedVariantSurface = false;
		bool hasLastActorScenePosition = false;
		bool indirectOnly = false;
		SurfaceRef lightSurface;
		SurfaceRef desiredMaterialSurface;
		bool hasDesiredMaterialSurface = false;
		bool pendingRemoval = false;
		bool authorityCurrent = false;
		bool publicationEligible = false;
	};

	struct VoxelActorCacheLookup;

	struct VoxelMeshBuildContext
	{
		const HWSprite* sprite = nullptr;
		const VoxelActorCacheLookup* lookup = nullptr;
		uint64_t meshVariantHash = 0;
		uint64_t materialVariantHash = 0;
		int32_t sourcePicnum = -1;
		int32_t resolvedVoxelIndex = -1;
		bool cacheSurfaceUpdate = false;
		bool forceTransient = false;
		bool directPublish = false;
		bool rawArchive = false;
		bool hadRetainedSurface = false;
		const char* cpuMeshClassification = "unsupported";
		const char* cpuMeshReason = "unclassified";
	};

	struct DynamicVoxelCaptureRouting
	{
		const char* cpuMeshClassification = "unsupported";
		const char* reason = "unclassified";
		uint32_t primitiveCount = 0;
		bool directOnlyAdmission = false;
	};

	std::unordered_map<uint64_t, VoxelActorCacheEntry> gVoxelActorCache;

	enum class LoadingVoxelRequestPriority : uint8_t
	{
		Force = 0,
		High = 1,
		Normal = 2,
		Opportunistic = 3,
	};

	enum LoadingVoxelRequestSourceBits : uint32_t
	{
		LoadingVoxelRequestSource_None = 0,
		LoadingVoxelRequestSource_LiveActorCurrent = 1u << 0,
		LoadingVoxelRequestSource_LiveActorAnimated = 1u << 1,
		LoadingVoxelRequestSource_LiveActorPicrange = 1u << 2,
		LoadingVoxelRequestSource_MountedVoxelPreload = 1u << 3,
		LoadingVoxelRequestSource_MountedVoxelPreloadGlobal = 1u << 4,
		LoadingVoxelRequestSource_MountedVoxelPreloadGame = 1u << 5,
		LoadingVoxelRequestSource_MountedVoxelPreloadMap = 1u << 6,
	};

	struct LoadingVoxelTextureCandidate
	{
		FTextureID texid = {};
		uint32_t sourceBits = LoadingVoxelRequestSource_None;
		LoadingVoxelRequestPriority priority = LoadingVoxelRequestPriority::Normal;
		bool gpuForce = false;
		bool gpuPrefer = false;
	};

	struct LoadingVoxelMaterialContext
	{
		FTextureID texid = {};
		DCoreActor* actor = nullptr;
		int32_t actorIndex = -1;
		uint32_t sourceBits = LoadingVoxelRequestSource_None;
		LoadingVoxelRequestPriority priority = LoadingVoxelRequestPriority::Normal;
		bool gpuForce = false;
		bool gpuPrefer = false;
	};

	struct LoadingVoxelPreloadRequest
	{
		FTextureID texid = {};
		DCoreActor* actor = nullptr;
		int32_t actorIndex = -1;
		FVoxelModel* model = nullptr;
		int32_t resolvedVoxelIndex = -1;
		uint64_t meshVariantHash = 0;
		uint32_t sourceBits = LoadingVoxelRequestSource_None;
		LoadingVoxelRequestPriority priority = LoadingVoxelRequestPriority::Normal;
		bool gpuForce = false;
		bool gpuPrefer = false;
		uint32_t primitiveCount = 0;
		std::vector<LoadingVoxelMaterialContext> materialContexts;
	};

	struct LoadingVoxelPreloadRequestGraph
	{
		std::vector<LoadingVoxelPreloadRequest> requests;
		std::unordered_map<uint64_t, size_t> requestByMeshVariant;
		uint32_t discovered = 0;
		uint32_t skippedInvalid = 0;
		uint32_t skippedDuplicate = 0;
		uint32_t actorCandidates = 0;
		uint32_t manifestSources = 0;
		uint32_t manifestLines = 0;
		uint32_t manifestRequests = 0;
		uint32_t manifestSkippedInactive = 0;
		uint32_t manifestSkippedSyntax = 0;
		uint32_t manifestSkippedActor = 0;
		uint32_t manifestSkippedUnsupported = 0;
	};

	uint32_t CountSurfacePrimitives(const SurfaceRef& surface);

	void AddVoxelMeshPrecacheStats(VoxelMeshPrecacheStats& target, const VoxelMeshPrecacheStats& delta)
	{
		target.textureCandidates += delta.textureCandidates;
		target.actorCandidates += delta.actorCandidates;
		target.modelCandidates += delta.modelCandidates;
		target.meshVariantCandidates += delta.meshVariantCandidates;
		target.meshHits += delta.meshHits;
		target.meshBuilds += delta.meshBuilds;
		target.meshInvalid += delta.meshInvalid;
		target.meshSkipped += delta.meshSkipped;
		target.meshVariantHits += delta.meshVariantHits;
		target.meshVariantBuilds += delta.meshVariantBuilds;
		target.meshVariantInvalid += delta.meshVariantInvalid;
		target.vertices += delta.vertices;
		target.indices += delta.indices;
		target.primitives += delta.primitives;
		target.variantPrimitives += delta.variantPrimitives;
		target.buildMs += delta.buildMs;
	}

	void RecordVoxelMeshPrecacheStats(const VoxelMeshPrecacheStats& delta, VoxelMeshPrecacheStats* stats)
	{
		if (stats != nullptr)
		{
			AddVoxelMeshPrecacheStats(*stats, delta);
		}
		AddVoxelMeshPrecacheStats(gVoxelLoadingWarmupStats, delta);
	}

	FVoxelModel* ResolveVoxelTextureModel(FTextureID texid, int* outVoxelIndex = nullptr)
	{
		if (outVoxelIndex != nullptr)
		{
			*outVoxelIndex = -1;
		}
		if (!texid.isValid())
		{
			return nullptr;
		}

		const int voxelIndex = GetExtInfo(texid).tiletovox;
		if (outVoxelIndex != nullptr)
		{
			*outVoxelIndex = voxelIndex;
		}
		if (voxelIndex < 0 || voxelIndex >= MAXVOXELS || voxmodels[voxelIndex] == nullptr)
		{
			return nullptr;
		}
		return voxmodels[voxelIndex]->model;
	}

	VoxelMeshVariantKey BuildLoadingVoxelMeshVariantKey(FTextureID texid, FVoxelModel* model, int voxelIndex)
	{
		VoxelMeshVariantKey key = {};
		key.voxel = voxelIndex >= 0 && voxelIndex < MAXVOXELS ? voxmodels[voxelIndex] : nullptr;
		key.model = model;
		key.sourcePicnum = texid;
		key.resolvedVoxelIndex = voxelIndex;
		key.geometryState = 0;
		return key;
	}

	struct VoxelCaptureBudget
	{
		uint32_t remainingTriangles = 0;
		uint32_t remainingCacheUpdates = 0;
		bool unlimited = false;
		bool unlimitedCacheUpdates = false;
		bool spentTriangleBudget = false;
	};

	enum class VoxelActorStability : uint8_t
	{
		Uncacheable,
		New,
		Stable,
		TransformRebake,
		Changed,
	};

	enum class VoxelActorPendingReason : uint8_t
	{
		None,
		ActorBudget,
		MeshDeferred,
		TriangleBudget,
		SurfaceBuildFailed,
		ActorNotLive,
	};

	struct VoxelActorCacheLookup
	{
		VoxelActorStability stability = VoxelActorStability::Uncacheable;
		uint64_t identityKey = 0;
		uint64_t signature = 0;
		uint64_t geometrySignature = 0;
		uint64_t surfaceSignature = 0;
		uint64_t materialSignature = 0;
		uint64_t transformBasisSignature = 0;
		uint64_t meshKeyHash = 0;
		uint64_t materialKeyHash = 0;
		uint64_t geometryContentHash = 0;
		uint64_t renderPrimitiveHash = 0;
		uint64_t meshVariantHash = 0;
		uint64_t materialVariantHash = 0;
		uint64_t instanceKeyHash = 0;
		uint64_t ownerWorldEpoch = 0;
		uint64_t ownerLifetimeGeneration = 0;
		uint64_t placementGeneration = 0;
		uint64_t placementStateHash = 0;
		uint64_t presentationBasisStateHash = 0;
		VoxelMeshBakeSpace meshBakeSpace = VoxelMeshBakeSpace::Unknown;
		int32_t actorIndex = -1;
		int32_t physicalSectorIndex = -1;
		uintptr_t voxelPtr = 0;
		uintptr_t voxelModelPtr = 0;
		int32_t sourcePicnum = -1;
		int32_t resolvedVoxelIndex = -1;
		float currentTranslation[3] = {};
		float currentTransform[12] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		float actorScenePosition[3] = {};
		bool hasActorScenePosition = false;
		bool indirectOnly = false;
		bool authorityCurrent = false;
		bool publicationEligible = false;
		VoxelActorCacheEntry* entry = nullptr;
	};

	bool ShouldTraceSkyPerf()
	{
		return nri_pttraceframes > 0;
	}

	bool ShouldTraceMirrorVoxelCapture()
	{
		return nri_voxelstats || (int)nri_pttraceframes > 0 || (int)perf_looptraceframes > 0;
	}

	class ScopedSkyPerfTimer
	{
	public:
		explicit ScopedSkyPerfTimer(uint64_t& targetUs)
			: mTarget(ShouldTraceSkyPerf() ? &targetUs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedSkyPerfTimer()
		{
			if (mTarget != nullptr)
			{
				const auto elapsed = std::chrono::steady_clock::now() - mStart;
				*mTarget += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
			}
		}

		ScopedSkyPerfTimer(const ScopedSkyPerfTimer&) = delete;
		ScopedSkyPerfTimer& operator=(const ScopedSkyPerfTimer&) = delete;

	private:
		uint64_t* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	bool TryBuildPersistentAverageColorSignature(FGameTexture* texture, TextureSignature& outSignature)
	{
		outSignature = {};
		return TryBuildAverageColorTextureSignature(texture, outSignature) &&
			outSignature.valid &&
			outSignature.persistentEligible;
	}

	bool TryLoadPersistentAverageColor(const TextureSignature& signature, float* outColor, bool& outSuccess)
	{
		outSuccess = false;
		if (!signature.valid || !signature.persistentEligible)
		{
			return false;
		}

		const auto it = gPersistentAverageTextureColorCache.find(signature.key);
		if (it == gPersistentAverageTextureColorCache.end())
		{
			return false;
		}

		outSuccess = it->second.success;
		if (outSuccess)
		{
			Copy3(it->second.color, outColor);
		}
		return true;
	}

	void StorePersistentAverageColor(const TextureSignature& signature, bool success, const float* color)
	{
		if (!signature.valid || !signature.persistentEligible)
		{
			return;
		}

		AverageColorCacheEntry& entry = gPersistentAverageTextureColorCache[signature.key];
		entry.success = success;
		if (success && color != nullptr)
		{
			Copy3(color, entry.color);
		}
	}

	bool TryLoadFrameLocalAverageColor(FTexture* baseTexture, float* outColor, bool& outSuccess)
	{
		outSuccess = false;
		if (baseTexture == nullptr)
		{
			return false;
		}

		const auto it = gFrameLocalAverageTextureColorCache.find(baseTexture);
		if (it == gFrameLocalAverageTextureColorCache.end())
		{
			return false;
		}

		outSuccess = it->second.success;
		if (outSuccess)
		{
			Copy3(it->second.color, outColor);
		}
		return true;
	}

	void StoreFrameLocalAverageColor(FTexture* baseTexture, bool success, const float* color)
	{
		if (baseTexture == nullptr)
		{
			return;
		}

		AverageColorCacheEntry& entry = gFrameLocalAverageTextureColorCache[baseTexture];
		entry.success = success;
		if (success && color != nullptr)
		{
			Copy3(color, entry.color);
		}
	}

	bool TryLoadCachedSkyInspection(FGameTexture* texture, CachedSkyInspection& outInspection)
	{
		if (!IsUsableGameTexturePointer(texture))
		{
			return false;
		}

		const auto it = gSkyInspectionCache.find(texture);
		if (it == gSkyInspectionCache.end())
		{
			return false;
		}

		outInspection = it->second;
		return true;
	}

	void StoreCachedSkyInspection(FGameTexture* texture, const CachedSkyInspection& inspection)
	{
		if (!IsUsableGameTexturePointer(texture))
		{
			return;
		}

		gSkyInspectionCache[texture] = inspection;
	}

	struct SkyCandidate
	{
		bool valid = false;
		bool hasAverageColor = false;
		bool hasFallbackColor = false;
		bool isCubemap = false;
		bool isThreeFace = false;
		bool flipTop = false;
		uint32_t faceMask = 0;
		uint32_t priority = 0;
		float color[3] = {};
	};

	FTexture* TryGetSkyTraceBaseTexture(FGameTexture* texture)
	{
		FTexture* baseTexture = nullptr;
		__try
		{
			baseTexture = IsUsableGameTexturePointer(texture) ? texture->GetTexture() : nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			baseTexture = nullptr;
		}
		return baseTexture;
	}

	int GetOwnerActorIndex(const HWWall& wall)
	{
		return wall.Sprite != nullptr && wall.Sprite->ownerActor != nullptr ? wall.Sprite->ownerActor->GetIndex() : -1;
	}

	int GetOwnerActorIndex(const HWFlat& flat)
	{
		return flat.Sprite != nullptr && flat.Sprite->ownerActor != nullptr ? flat.Sprite->ownerActor->GetIndex() : -1;
	}

	int GetOwnerActorIndex(const HWSprite& sprite)
	{
		return sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr ? sprite.Sprite->ownerActor->GetIndex() : -1;
	}

	CapturedVertex MakeCapturedVertex(const FFlatVertex& source)
	{
		return BuildCapturedVertex(source);
	}

	CapturedVertex MakeCapturedVertex(float x, float y, float z, float u, float v)
	{
		return BuildCapturedVertex(x, y, z, u, v);
	}

	uint32_t MakeSkyPriority(PTSkyMode mode, PTSkySourceType sourceType)
	{
		uint32_t priority = mode == PTSkyMode::Cubemap ? 100u : (mode == PTSkyMode::SolidColor ? 10u : 0u);
		switch (sourceType)
		{
		case PTSkySourceType::Portal:
			return priority + 3u;
		case PTSkySourceType::Flat:
			return priority + 2u;
		case PTSkySourceType::Wall:
			return priority + 1u;
		default:
			return priority;
		}
	}

	const char* GetSkySourceTraceName(PTSkySourceType sourceType)
	{
		switch (sourceType)
		{
		case PTSkySourceType::Wall:
			return "wall";
		case PTSkySourceType::Flat:
			return "flat";
		case PTSkySourceType::Portal:
			return "portal";
		default:
			return "none";
		}
	}

	bool TryComputeAverageColorFromBaseTexture(FTexture* baseTexture, float* outColor)
	{
		if (ShouldTraceSkyPerf())
		{
			gSkyPerfStats.averageColorBaseCalls++;
		}
		ScopedSkyPerfTimer timer(gSkyPerfStats.averageColorTimeUs);
		if (baseTexture == nullptr || baseTexture->GetImage() == nullptr)
		{
			return false;
		}

		FTextureBuffer texBuffer = baseTexture->CreateTexBuffer(0, CTF_ProcessData);
		if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
		{
			return false;
		}

		double sum[3] = {};
		const size_t pixelCount = (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight;
		if (ShouldTraceSkyPerf())
		{
			gSkyPerfStats.averageColorPixels += (uint64_t)pixelCount;
		}
		for (size_t i = 0; i < pixelCount; ++i)
		{
			const uint8_t* pixel = texBuffer.mBuffer + i * 4u;
			sum[0] += pixel[2];
			sum[1] += pixel[1];
			sum[2] += pixel[0];
		}

		const double scale = pixelCount > 0 ? 1.0 / (255.0 * (double)pixelCount) : 0.0;
		outColor[0] = (float)(sum[0] * scale);
		outColor[1] = (float)(sum[1] * scale);
		outColor[2] = (float)(sum[2] * scale);
		return true;
	}

	bool TryGetAverageTextureColorRecursive(FGameTexture* texture, float* outColor, int depth);

	void ApplyCachedSkyInspectionToCandidate(const CachedSkyInspection& inspection, uint32_t fallbackColor, PTSkySourceType sourceType, SkyCandidate& outCandidate)
	{
		outCandidate = {};
		outCandidate.valid = inspection.valid;
		outCandidate.isCubemap = inspection.isCubemap;
		outCandidate.isThreeFace = inspection.isThreeFace;
		outCandidate.flipTop = inspection.flipTop;
		outCandidate.faceMask = inspection.faceMask;
		outCandidate.priority = MakeSkyPriority(inspection.isCubemap ? PTSkyMode::Cubemap : PTSkyMode::SolidColor, sourceType);
		if (inspection.hasAverageColor)
		{
			outCandidate.hasAverageColor = true;
			Copy3(inspection.color, outCandidate.color);
		}
		else if (fallbackColor != 0)
		{
			const PalEntry fallback = PalEntry(fallbackColor);
			outCandidate.color[0] = fallback.r / 255.0f;
			outCandidate.color[1] = fallback.g / 255.0f;
			outCandidate.color[2] = fallback.b / 255.0f;
			outCandidate.hasFallbackColor = true;
		}
	}

	bool TryInspectSkyTextureInner(FGameTexture* texture, CachedSkyInspection& outInspection)
	{
		__try
		{
			if (!IsUsableGameTexturePointer(texture))
			{
				return false;
			}

			outInspection = {};
			outInspection.valid = true;
			if (TryGetAverageTextureColor(texture, outInspection.color))
			{
				outInspection.hasAverageColor = true;
			}

			FTexture* baseTexture = nullptr;
			__try
			{
				baseTexture = texture->GetTexture();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				baseTexture = nullptr;
			}

			auto* skybox = dynamic_cast<FSkyBox*>(baseTexture);
			if (skybox == nullptr)
			{
				if (ShouldTraceSkyPerf())
				{
					gSkyPerfStats.inspectSolidCandidates++;
				}
				return true;
			}

			outInspection.flipTop = skybox->GetSkyFlip();
			outInspection.isThreeFace = skybox->Is3Face();
			for (int i = 0; i < 6; ++i)
			{
				if (ShouldTraceSkyPerf())
				{
					gSkyPerfStats.inspectFaceWalks++;
				}
				FGameTexture* face = nullptr;
				__try
				{
					face = skybox->GetSkyFace(i);
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					face = nullptr;
				}

				if (IsUsableGameTexturePointer(face))
				{
					outInspection.faceMask |= 1u << i;
				}
			}

			if (!outInspection.isThreeFace && outInspection.faceMask == 0x3fu)
			{
				outInspection.isCubemap = true;
				if (ShouldTraceSkyPerf())
				{
					gSkyPerfStats.inspectCubemapCandidates++;
				}
			}
			else if (ShouldTraceSkyPerf())
			{
				gSkyPerfStats.inspectSolidCandidates++;
			}

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool TryInspectSkyTexture(FGameTexture* texture, uint32_t fallbackColor, PTSkySourceType sourceType, SkyCandidate& outCandidate)
	{
		CachedSkyInspection inspection = {};
		if (!TryLoadCachedSkyInspection(texture, inspection))
		{
			if (ShouldTraceSkyPerf())
			{
				gSkyPerfStats.inspectCalls++;
			}
			ScopedSkyPerfTimer timer(gSkyPerfStats.inspectTimeUs);
			if (!TryInspectSkyTextureInner(texture, inspection))
			{
				outCandidate = {};
				return false;
			}
			StoreCachedSkyInspection(texture, inspection);
			if (ShouldTraceSkyPerf())
			{
				FTexture* baseTexture = TryGetSkyTraceBaseTexture(texture);
				auto* skybox = dynamic_cast<FSkyBox*>(baseTexture);
				Printf("NRI PT sky inspect: source=%s texture=%p name=%s texid=%d display=%dx%d base=%p base_type=%s base_size=%dx%d skybox=%s three_face=%s face_mask=0x%x flip_top=%s cubemap=%s avg=%s color=(%.3f, %.3f, %.3f)\n",
					GetSkySourceTraceName(sourceType),
					texture,
					texture != nullptr ? texture->GetName().GetChars() : "(null)",
					texture != nullptr ? texture->GetID().GetIndex() : -1,
					texture != nullptr ? (int)texture->GetDisplayWidth() : 0,
					texture != nullptr ? (int)texture->GetDisplayHeight() : 0,
					baseTexture,
					baseTexture != nullptr ? typeid(*baseTexture).name() : "(null)",
					baseTexture != nullptr ? baseTexture->GetWidth() : 0,
					baseTexture != nullptr ? baseTexture->GetHeight() : 0,
					skybox != nullptr ? "yes" : "no",
					inspection.isThreeFace ? "yes" : "no",
					inspection.faceMask,
					inspection.flipTop ? "yes" : "no",
					inspection.isCubemap ? "yes" : "no",
					inspection.hasAverageColor ? "yes" : "no",
					inspection.color[0],
					inspection.color[1],
					inspection.color[2]);
			}
		}

		ApplyCachedSkyInspectionToCandidate(inspection, fallbackColor, sourceType, outCandidate);
		return outCandidate.valid;
	}

	void ApplySkyCandidate(SceneView& outView, FGameTexture* texture, const SkyCandidate& candidate, PTSkySourceType sourceType)
	{
		if (!candidate.valid || candidate.priority < outView.sky.priority)
		{
			return;
		}

		if (candidate.hasAverageColor || candidate.hasFallbackColor)
		{
			Copy3(candidate.color, outView.skyColor);
		}

		if (candidate.priority == outView.sky.priority && outView.sky.texture != nullptr)
		{
			return;
		}

		outView.sky.mode = candidate.isCubemap ? PTSkyMode::Cubemap : PTSkyMode::SolidColor;
		outView.sky.sourceType = sourceType;
		outView.sky.texture = texture;
		outView.sky.faceMask = candidate.faceMask;
		outView.sky.priority = candidate.priority;
		outView.sky.flipTop = candidate.flipTop;
		outView.sky.isThreeFace = candidate.isThreeFace;
	}

	bool TryGetAverageTextureColorRecursive(FGameTexture* texture, float* outColor, int depth)
	{
		if (ShouldTraceSkyPerf())
		{
			gSkyPerfStats.averageColorRecursiveCalls++;
		}
		if (!IsUsableGameTexturePointer(texture) || depth > 4)
		{
			return false;
		}

		FTexture* baseTexture = nullptr;
		__try
		{
			baseTexture = texture->GetTexture();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		if (baseTexture == nullptr)
		{
			return false;
		}

		TextureSignature persistentSignature = {};
		const bool hasPersistentSignature = TryBuildPersistentAverageColorSignature(texture, persistentSignature);

		bool cachedSuccess = false;
		if (hasPersistentSignature && TryLoadPersistentAverageColor(persistentSignature, outColor, cachedSuccess))
		{
			return cachedSuccess;
		}

		if (TryLoadFrameLocalAverageColor(baseTexture, outColor, cachedSuccess))
		{
			return cachedSuccess;
		}

		float computedColor[3] = {};
		if (TryComputeAverageColorFromBaseTexture(baseTexture, computedColor))
		{
			StoreFrameLocalAverageColor(baseTexture, true, computedColor);
			if (hasPersistentSignature)
			{
				StorePersistentAverageColor(persistentSignature, true, computedColor);
			}
			Copy3(computedColor, outColor);
			return true;
		}

		auto* skybox = dynamic_cast<FSkyBox*>(baseTexture);
		if (skybox == nullptr)
		{
			StoreFrameLocalAverageColor(baseTexture, false, nullptr);
			if (hasPersistentSignature)
			{
				StorePersistentAverageColor(persistentSignature, false, nullptr);
			}
			return false;
		}

		float accumulated[3] = {};
		int sampledFaces = 0;
		for (int i = 0; i < 6; ++i)
		{
			if (ShouldTraceSkyPerf())
			{
				gSkyPerfStats.recursiveSkyboxFaceSamples++;
			}
			float faceColor[3] = {};
			FGameTexture* skyFace = nullptr;
			__try
			{
				skyFace = skybox->GetSkyFace(i);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				skyFace = nullptr;
			}
			if (TryGetAverageTextureColorRecursive(skyFace, faceColor, depth + 1))
			{
				accumulated[0] += faceColor[0];
				accumulated[1] += faceColor[1];
				accumulated[2] += faceColor[2];
				sampledFaces++;
			}
		}

		if (sampledFaces > 0)
		{
			const float invCount = 1.0f / sampledFaces;
			computedColor[0] = accumulated[0] * invCount;
			computedColor[1] = accumulated[1] * invCount;
			computedColor[2] = accumulated[2] * invCount;
			StoreFrameLocalAverageColor(baseTexture, true, computedColor);
			Copy3(computedColor, outColor);
			return true;
		}

		FGameTexture* previous = nullptr;
		__try
		{
			previous = skybox->previous;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			previous = nullptr;
		}
		const bool success = TryGetAverageTextureColorRecursive(previous, computedColor, depth + 1);
		StoreFrameLocalAverageColor(baseTexture, success, success ? computedColor : nullptr);
		if (success)
		{
			Copy3(computedColor, outColor);
		}
		return success;
	}

	unsigned int CountDrawListItems(HWDrawInfo& di, DrawListType type)
	{
		return di.drawlists[type].Size();
	}

	unsigned int CountFanTriangles(const SurfaceRef& surface)
	{
		if (!surface.indices.empty())
		{
			return (unsigned int)(surface.indices.size() / 3u);
		}
		return surface.vertices.size() >= 3 ? (unsigned int)surface.vertices.size() - 2u : 0u;
	}

	unsigned int CountTriangleListTriangles(const SurfaceRef& surface)
	{
		if (!surface.indices.empty())
		{
			return (unsigned int)(surface.indices.size() / 3u);
		}
		return (unsigned int)(surface.vertices.size() / 3u);
	}

	void ApplyActorPreviousTransform(SurfaceRef& surface, DCoreActor* actor)
	{
		if (actor == nullptr)
		{
			return;
		}

		const DVector3 worldDelta = actor->spr.pos - actor->opos;
		const float renderDelta[3] = {
			(float)worldDelta.X,
			(float)-worldDelta.Z,
			(float)-worldDelta.Y
		};

		for (CapturedVertex& vertex : surface.vertices)
		{
			vertex.prevPosition[0] = vertex.position[0] - renderDelta[0];
			vertex.prevPosition[1] = vertex.position[1] - renderDelta[1];
			vertex.prevPosition[2] = vertex.position[2] - renderDelta[2];
		}
	}

	bool TryComputeSurfaceNormal(const SurfaceRef& surface, float* outNormal);

	bool TryFindNearbyWallSpriteBackingWall(const HWWall& wall, walltype*& outWall, DVector2& outNearestPoint)
	{
		outWall = nullptr;
		outNearestPoint = {};
		if (wall.Sprite == nullptr || wall.Sprite->sectp == nullptr)
		{
			return false;
		}

		if (wall.walldist != nullptr)
		{
			outWall = wall.walldist;
			outNearestPoint = NearestPointOnWall(wall.Sprite->pos.X, wall.Sprite->pos.Y, outWall, false);
			return true;
		}

		double maxOrthDist = 3.0 * maptoworld;
		const double maxDistSq = maxOrthDist * maxOrthDist;
		const DAngle maxAngDelta = DAngle360 / 1024;
		walltype* bestWall = nullptr;
		DVector2 bestNearestPoint = {};

		for (auto& candidate : wall.Sprite->sectp->walls)
		{
			const DVector2 delta = candidate.delta();
			const DAngle deltaAng = absangle(delta.Angle(), wall.Sprite->Angles.Yaw);
			if (deltaAng < DAngle90 - maxAngDelta || deltaAng > DAngle90 + maxAngDelta)
			{
				continue;
			}

			DVector2 nearestPoint = NearestPointOnWall(wall.Sprite->pos.X, wall.Sprite->pos.Y, &candidate, false);
			if (!((wall.Sprite->Angles.Yaw.Buildang()) & 510))
			{
				double newDist = DBL_MAX;
				if (delta.X == 0.0)
				{
					newDist = fabs(wall.Sprite->pos.X - candidate.pos.X);
				}
				else if (delta.Y == 0.0)
				{
					newDist = fabs(wall.Sprite->pos.Y - candidate.pos.Y);
				}

				if (newDist < maxOrthDist)
				{
					maxOrthDist = newDist;
					bestWall = &candidate;
					bestNearestPoint = nearestPoint;
				}
			}
			else
			{
				const double wallDistSq = SquareDistToWall(wall.Sprite->pos.X, wall.Sprite->pos.Y, &candidate, &nearestPoint);
				if (wallDistSq <= maxDistSq)
				{
					outWall = &candidate;
					outNearestPoint = nearestPoint;
					return true;
				}
			}
		}

		if (bestWall == nullptr)
		{
			return false;
		}

		outWall = bestWall;
		outNearestPoint = bestNearestPoint;
		return true;
	}

	void NudgeAttachedWallSpriteSurface(const HWWall& wall, SurfaceRef& surface)
	{
		if (wall.Sprite == nullptr || surface.vertices.empty())
		{
			return;
		}

		float offset[3] = {};
		walltype* backingWall = nullptr;
		DVector2 nearestPoint = {};
		if (TryFindNearbyWallSpriteBackingWall(wall, backingWall, nearestPoint))
		{
			DVector2 nudgeDirection = {};
			const DVector2 spriteCenter = wall.Sprite->pos.XY();
			const DVector2 wallToSprite = spriteCenter - nearestPoint;
			if (wallToSprite.LengthSquared() > 1.0e-8)
			{
				nudgeDirection = wallToSprite.Unit();
			}
			else
			{
				// Exact on-wall placements need a stable wall-side fallback. Match the
				// same sector-owned wall normal convention used by pushmove().
				const DVector2 wallNormal = backingWall->delta().Rotated90CCW();
				if (wallNormal.LengthSquared() <= 1.0e-8)
				{
					return;
				}

				nudgeDirection = wallNormal.Unit();
			}

			offset[0] = (float)(nudgeDirection.X * kAttachedWallSpriteDepthNudge);
			offset[2] = (float)(-nudgeDirection.Y * kAttachedWallSpriteDepthNudge);
		}
		else
		{
			float normal[3] = {};
			if (!TryComputeSurfaceNormal(surface, normal))
			{
				return;
			}

			offset[0] = normal[0] * kAttachedWallSpriteDepthNudge;
			offset[1] = normal[1] * kAttachedWallSpriteDepthNudge;
			offset[2] = normal[2] * kAttachedWallSpriteDepthNudge;
		}

		// The raster path uses depth bias / draw-order handling for wall-flush
		// sprite content. PT needs a small geometric equivalent or nearby walls
		// can win the closest-hit test.

		for (CapturedVertex& vertex : surface.vertices)
		{
			vertex.position[0] += offset[0];
			vertex.position[1] += offset[1];
			vertex.position[2] += offset[2];
			vertex.prevPosition[0] += offset[0];
			vertex.prevPosition[1] += offset[1];
			vertex.prevPosition[2] += offset[2];
		}
	}

	bool TryComputeSurfaceNormal(const SurfaceRef& surface, float* outNormal)
	{
		if (surface.vertices.size() < 3)
		{
			return false;
		}

		const CapturedVertex& a = surface.vertices[0];
		const CapturedVertex& b = surface.vertices[1];
		const CapturedVertex& c = surface.vertices[2];
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float lengthSq = nx * nx + ny * ny + nz * nz;
		if (lengthSq <= 1.0e-8f)
		{
			return false;
		}

		const float invLength = 1.0f / sqrtf(lengthSq);
		outNormal[0] = nx * invLength;
		outNormal[1] = ny * invLength;
		outNormal[2] = nz * invLength;
		return true;
	}

	void NudgeSpriteFlatSurface(const HWFlat& flat, SurfaceRef& surface)
	{
		if (flat.Sprite == nullptr || surface.vertices.empty())
		{
			return;
		}

		float normal[3] = {};
		if (!TryComputeSurfaceNormal(surface, normal))
		{
			return;
		}

		for (CapturedVertex& vertex : surface.vertices)
		{
			vertex.position[0] += normal[0] * kAttachedWallSpriteDepthNudge;
			vertex.position[1] += normal[1] * kAttachedWallSpriteDepthNudge;
			vertex.position[2] += normal[2] * kAttachedWallSpriteDepthNudge;
			vertex.prevPosition[0] += normal[0] * kAttachedWallSpriteDepthNudge;
			vertex.prevPosition[1] += normal[1] * kAttachedWallSpriteDepthNudge;
			vertex.prevPosition[2] += normal[2] * kAttachedWallSpriteDepthNudge;
		}
	}

	bool IsEffectivelyOpaque(const FRenderStyle& style, float alpha)
	{
		return alpha >= 0.999f &&
			style.BlendOp == STYLEOP_Add &&
			style.SrcAlpha == STYLEALPHA_Src &&
			style.DestAlpha == STYLEALPHA_InvSrc &&
			style.Flags == 0;
	}

	bool IsOpaqueSurface(const HWWall& wall)
	{
		return wall.texture != nullptr &&
			wall.vertcount >= 3 &&
			IsEffectivelyOpaque(wall.RenderStyle, wall.alpha);
	}

	bool IsSkyWall(const HWWall& wall)
	{
		return (wall.flags & HWWall::HWF_SKYHACK) != 0;
	}

	bool IsPortalSourceWall(const HWWall& wall)
	{
		if (wall.seg == nullptr)
		{
			return false;
		}

		switch (wall.seg->portalflags)
		{
		case PORTAL_WALL_VIEW:
		case PORTAL_WALL_TO_SPRITE:
			return true;
		default:
			return false;
		}
	}

	SurfaceProvenance MakeWallProvenance(const walltype* seg, SurfaceSourceType sourceType, uint32_t drawListType, int actorIndex, uint32_t materialFlags)
	{
		SurfaceProvenance provenance = {};
		provenance.sourceType = sourceType;
		provenance.actorIndex = actorIndex;
		provenance.drawListType = drawListType;
		provenance.materialFlags = materialFlags;
		if (seg != nullptr)
		{
			provenance.sectorIndex = seg->sector;
			provenance.wallIndex = wall.IndexOf(seg);
			provenance.nextSectorIndex = seg->nextsector;
			provenance.cstat = (uint32_t)seg->cstat;
		}
		return provenance;
	}

	SurfaceProvenance MakeFlatProvenance(const HWFlat& flat, uint32_t drawListType, uint32_t materialFlags)
	{
		SurfaceProvenance provenance = {};
		provenance.sourceType = flat.plane == plane_ceiling ? SurfaceSourceType::CeilingFlat : SurfaceSourceType::FloorFlat;
		provenance.actorIndex = GetOwnerActorIndex(flat);
		provenance.drawListType = drawListType;
		provenance.materialFlags = materialFlags;
		if (flat.sec != nullptr)
		{
			provenance.sectorIndex = sector.IndexOf(flat.sec);
			provenance.cstat = flat.plane == plane_ceiling ? (uint32_t)flat.sec->ceilingstat : (uint32_t)flat.sec->floorstat;
		}
		return provenance;
	}

	SurfaceProvenance MakeSpriteProvenance(const HWSprite& sprite, SurfaceSourceType sourceType, uint32_t drawListType, uint32_t materialFlags)
	{
		SurfaceProvenance provenance = {};
		provenance.sourceType = sourceType;
		provenance.actorIndex = GetOwnerActorIndex(sprite);
		provenance.drawListType = drawListType;
		provenance.materialFlags = materialFlags;
		if (sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr && sprite.Sprite->ownerActor->spr.sectp != nullptr)
		{
			provenance.sectorIndex = sector.IndexOf(sprite.Sprite->ownerActor->spr.sectp);
		}
		return provenance;
	}

	bool IsOpaqueSurface(const HWFlat& flat)
	{
		return flat.texture != nullptr &&
			flat.vertcount >= 3 &&
			IsEffectivelyOpaque(flat.RenderStyle, flat.alpha) &&
			flat.Sprite == nullptr &&
			true;
	}

	bool IsOpaqueSpriteFlat(const HWFlat& flat)
	{
		return flat.texture != nullptr &&
			flat.vertcount >= 3 &&
			IsEffectivelyOpaque(flat.RenderStyle, flat.alpha) &&
			flat.Sprite != nullptr;
	}

	bool IsSkyFlat(const HWFlat& flat)
	{
		if (flat.sec == nullptr)
		{
			return false;
		}

		if (flat.plane == plane_ceiling)
		{
			return (flat.sec->ceilingstat & CSTAT_SECTOR_SKY) != 0;
		}

		return (flat.sec->floorstat & CSTAT_SECTOR_SKY) != 0;
	}

	bool IsPortalSourceFlat(const HWFlat& flat)
	{
		if (flat.stack || flat.sec == nullptr)
		{
			return true;
		}

		const int flags = flat.sec->portalflags;
		if (flat.plane == plane_ceiling)
		{
			return flags == PORTAL_SECTOR_CEILING || flags == PORTAL_SECTOR_CEILING_REFLECT;
		}

		return flags == PORTAL_SECTOR_FLOOR || flags == PORTAL_SECTOR_FLOOR_REFLECT;
	}

	bool DrawListUsesAlphaClip(uint32_t drawListType)
	{
		switch (drawListType)
		{
		case GLDL_MASKEDWALLS:
		case GLDL_MASKEDWALLSS:
		case GLDL_MASKEDWALLSD:
		case GLDL_MASKEDWALLSV:
		case GLDL_MASKEDWALLSH:
		case GLDL_MASKEDFLATS:
		case GLDL_MASKEDSLOPEFLATS:
		case GLDL_TRANSLUCENT:
		case GLDL_MODELS:
			return true;
		default:
			return false;
		}
	}

	void CaptureWalls(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outWalls, SceneDebugStats& stats, SceneView& outView)
	{
		for (auto* wall : list.walls)
		{
			if (wall == nullptr || !IsOpaqueSurface(*wall))
			{
				continue;
			}

			if (IsSkyWall(*wall))
			{
				stats.skySurfaces++;
				UpdateSceneSky(outView, wall->texture, 0, PTSkySourceType::Wall);
				continue;
			}

			if (IsPortalSourceWall(*wall))
			{
				continue;
			}

			wall->MakeVertices(&di, false);
			if (wall->vertcount < 3)
			{
				continue;
			}

			uint32_t extraFlags = wall->Sprite != nullptr ? MaterialFlag_Sprite : MaterialFlag_None;
			if (wall->Sprite != nullptr || DrawListUsesAlphaClip(drawListType))
			{
				extraFlags |= MaterialFlag_AlphaClip;
			}
			MaterialRef material = MakeMaterialRef(wall->texture, wall->palette, wall->shade, wall->alpha, extraFlags);
			const FFlatVertex* vertices = screen->mVertexData->GetBuffer((int)wall->vertindex);
			SurfaceRef surface = BuildSurfaceFromVertices(vertices, wall->vertcount, material, MakeWallProvenance(wall->seg, SurfaceSourceType::DrawListWall, drawListType, GetOwnerActorIndex(*wall), material.flags));

			if (wall->Sprite != nullptr && wall->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, wall->Sprite->ownerActor);
			}

			NudgeAttachedWallSpriteSurface(*wall, surface);

			outWalls.push_back(std::move(surface));
		}
	}

	void CaptureMirrorBorders(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outWalls, SceneDebugStats& stats)
	{
		for (auto* wall : list.walls)
		{
			if (wall == nullptr || wall->type != RENDERWALL_MIRRORSURFACE)
			{
				continue;
			}

			wall->MakeVertices(&di, false);
			if (wall->vertcount < 3)
			{
				continue;
			}

			MaterialRef material = MakeMaterialRef(wall->texture, wall->palette, wall->shade, wall->alpha, MaterialFlag_Mirror);
			const FFlatVertex* vertices = screen->mVertexData->GetBuffer((int)wall->vertindex);
			SurfaceRef surface = BuildSurfaceFromVertices(vertices, wall->vertcount, material, MakeWallProvenance(wall->seg, SurfaceSourceType::MirrorWall, drawListType, GetOwnerActorIndex(*wall), material.flags));

			outWalls.push_back(std::move(surface));
			stats.mirrorSurfaces++;
		}
	}

	void CaptureFlats(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outFlats, SceneDebugStats& stats, SceneView& outView)
	{
		for (auto* flat : list.flats)
		{
			if (flat == nullptr || !IsOpaqueSurface(*flat))
			{
				continue;
			}

			if (IsSkyFlat(*flat))
			{
				stats.skySurfaces++;
				UpdateSceneSky(outView, flat->texture, 0, PTSkySourceType::Flat);
				continue;
			}

			if (IsPortalSourceFlat(*flat))
			{
				continue;
			}

			flat->MakeVertices(&di);
			if (flat->vertcount < 3)
			{
				continue;
			}

			uint32_t extraFlags = MaterialFlag_Flat;
			if (DrawListUsesAlphaClip(drawListType))
			{
				extraFlags |= MaterialFlag_AlphaClip;
			}
			MaterialRef material = MakeMaterialRef(flat->texture, flat->palette, flat->shade, flat->alpha, extraFlags);
			const FFlatVertex* vertices = screen->mVertexData->GetBuffer(flat->vertindex);
			SurfaceRef surface = BuildSurfaceFromVertices(vertices, (uint32_t)flat->vertcount, material, MakeFlatProvenance(*flat, drawListType, material.flags));

			if (flat->Sprite != nullptr && flat->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, flat->Sprite->ownerActor);
			}

			outFlats.push_back(std::move(surface));
		}
	}

	void CaptureSpriteFlats(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outFlats)
	{
		for (auto* flat : list.flats)
		{
			if (flat == nullptr)
			{
				continue;
			}

			flat->MakeVertices(&di);
			if (!IsOpaqueSpriteFlat(*flat))
			{
				continue;
			}

			MaterialRef material = MakeMaterialRef(flat->texture, flat->palette, flat->shade, flat->alpha, MaterialFlag_Flat | MaterialFlag_Sprite | MaterialFlag_AlphaClip);
			const FFlatVertex* vertices = screen->mVertexData->GetBuffer(flat->vertindex);
			SurfaceRef surface = BuildSurfaceFromVertices(vertices, (uint32_t)flat->vertcount, material, MakeFlatProvenance(*flat, drawListType, material.flags));

			if (flat->Sprite != nullptr && flat->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, flat->Sprite->ownerActor);
			}

			NudgeSpriteFlatSurface(*flat, surface);

			outFlats.push_back(std::move(surface));
		}
	}

	bool IsOpaqueSprite(const HWSprite& sprite)
	{
		return sprite.texture != nullptr &&
			sprite.modelframe == 0 &&
			IsEffectivelyOpaque(sprite.RenderStyle, sprite.alpha);
	}

	bool IsOwnedByActor(const HWSprite& sprite, int32_t actorIndex)
	{
		return
			sprite.Sprite != nullptr &&
			sprite.Sprite->ownerActor != nullptr &&
			(int32_t)sprite.Sprite->ownerActor->GetIndex() == actorIndex;
	}

	bool IsCapturableActorShadowTempSprite(const HWSprite& sprite)
	{
		return
			sprite.Sprite != nullptr &&
			sprite.Sprite->statnum == 99 &&
			sprite.Sprite->pal == 4 &&
			sprite.Sprite->shade == 127 &&
			(sprite.Sprite->cstat & CSTAT_SPRITE_TRANSLUCENT) != 0;
	}

	bool IsCapturableActorFacingSprite(const HWSprite& sprite, int32_t actorIndex)
	{
		return
			IsOwnedByActor(sprite, actorIndex) &&
			sprite.Sprite != nullptr &&
			(sprite.Sprite->statnum != 99 || IsCapturableActorShadowTempSprite(sprite)) &&
			sprite.texture != nullptr &&
			sprite.modelframe == 0 &&
			sprite.alpha > (1.0f / 255.0f);
	}

	void CaptureFacingSprites(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outSprites)
	{
		for (auto* sprite : list.sprites)
		{
			if (sprite == nullptr || !IsOpaqueSprite(*sprite))
			{
				continue;
			}

			if (sprite->vertexindex < 0)
			{
				sprite->CreateVertices(&di);
			}

			if (sprite->vertexindex < 0)
			{
				continue;
			}

			const FFlatVertex* vertices = screen->mVertexData->GetBuffer(sprite->vertexindex);
			if (vertices == nullptr)
			{
				continue;
			}

			uint32_t extraFlags = MaterialFlag_Sprite | MaterialFlag_AlphaClip;
			if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
			{
				extraFlags |= MaterialFlag_FacingBillboard;
			}
			MaterialRef material = MakeMaterialRef(sprite->texture, sprite->palette, sprite->shade, sprite->alpha, extraFlags);
			SurfaceRef surface = BuildQuadSurface(vertices, material, MakeSpriteProvenance(*sprite, SurfaceSourceType::FacingSprite, drawListType, material.flags));

			if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, sprite->Sprite->ownerActor);
				if ((int)nri_ptactorspritetrace > 0 && (int)nri_pttraceframes > 0 && screen != nullptr)
				{
					PathTracingActorSpriteTraceEvent event = {};
					event.stage = PathTracingActorSpriteTraceStage::CaptureScene;
					event.actorIndex = sprite->Sprite->ownerActor->GetIndex();
					event.spriteStatnum = sprite->Sprite->statnum;
					event.spritePicnum = sprite->Sprite->picnum;
					event.baseTextureId = sprite->Sprite->spritetexture().GetIndex();
					event.resolvedTextureId = sprite->texture != nullptr ? sprite->texture->GetID().GetIndex() : -1;
					event.palette = sprite->palette;
					event.shade = sprite->shade;
					event.cstat = sprite->Sprite->cstat;
					event.cstat2 = sprite->Sprite->cstat2;
					event.drawListType = drawListType;
					event.noAnimate = (sprite->Sprite->cstat2 & CSTAT2_SPRITE_NOANIMATE) != 0;
					event.fullbright = (sprite->Sprite->cstat2 & CSTAT2_SPRITE_FULLBRIGHT) != 0;
					event.resolvedGameTexture = sprite->texture;
					screen->EmitPathTracingActorSpriteTraceEvent(event);
				}
			}

			outSprites.push_back(std::move(surface));
		}
	}

	void CaptureActorFacingSprites(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, int32_t actorIndex, std::vector<SurfaceRef>& outSprites)
	{
		for (auto* sprite : list.sprites)
		{
			if (sprite == nullptr || !IsCapturableActorFacingSprite(*sprite, actorIndex))
			{
				continue;
			}

			if (sprite->vertexindex < 0)
			{
				sprite->CreateVertices(&di);
			}

			if (sprite->vertexindex < 0)
			{
				continue;
			}

			const FFlatVertex* vertices = screen->mVertexData->GetBuffer(sprite->vertexindex);
			if (vertices == nullptr)
			{
				continue;
			}

			uint32_t extraFlags = MaterialFlag_Sprite | MaterialFlag_AlphaClip;
			if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
			{
				extraFlags |= MaterialFlag_FacingBillboard;
			}
			MaterialRef material = MakeMaterialRef(sprite->texture, sprite->palette, sprite->shade, sprite->alpha, extraFlags);
			SurfaceRef surface = BuildQuadSurface(vertices, material, MakeSpriteProvenance(*sprite, SurfaceSourceType::FacingSprite, drawListType, material.flags));

			if (sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, sprite->Sprite->ownerActor);
				if ((int)nri_ptactorspritetrace > 0 && (int)nri_pttraceframes > 0 && screen != nullptr)
				{
					PathTracingActorSpriteTraceEvent event = {};
					event.stage = PathTracingActorSpriteTraceStage::CaptureActorScene;
					event.actorIndex = sprite->Sprite->ownerActor->GetIndex();
					event.spriteStatnum = sprite->Sprite->statnum;
					event.spritePicnum = sprite->Sprite->picnum;
					event.baseTextureId = sprite->Sprite->spritetexture().GetIndex();
					event.resolvedTextureId = sprite->texture != nullptr ? sprite->texture->GetID().GetIndex() : -1;
					event.palette = sprite->palette;
					event.shade = sprite->shade;
					event.cstat = sprite->Sprite->cstat;
					event.cstat2 = sprite->Sprite->cstat2;
					event.drawListType = drawListType;
					event.noAnimate = (sprite->Sprite->cstat2 & CSTAT2_SPRITE_NOANIMATE) != 0;
					event.fullbright = (sprite->Sprite->cstat2 & CSTAT2_SPRITE_FULLBRIGHT) != 0;
					event.resolvedGameTexture = sprite->texture;
					screen->EmitPathTracingActorSpriteTraceEvent(event);
				}
			}

			outSprites.push_back(std::move(surface));
		}
	}

	bool CaptureMirrorVoxelFallbackSprite(HWDrawInfo& di, HWSprite& sprite, uint32_t drawListType, std::vector<SurfaceRef>& outSprites)
	{
		if (!IsEffectivelyOpaque(sprite.RenderStyle, sprite.alpha))
		{
			return false;
		}

		FGameTexture* fallbackTexture = sprite.texture;
		if ((fallbackTexture == nullptr || !fallbackTexture->isValid()) && sprite.Sprite != nullptr)
		{
			fallbackTexture = TexMan.GetGameTexture(sprite.Sprite->spritetexture());
		}

		if (fallbackTexture == nullptr || !fallbackTexture->isValid())
		{
			return false;
		}

		if (sprite.Sprite == nullptr)
		{
			return false;
		}

		const tspritetype* spr = sprite.Sprite;
		int xsize = (int)fallbackTexture->GetDisplayWidth();
		int ysize = (int)fallbackTexture->GetDisplayHeight();
		int tilexoff = (int)fallbackTexture->GetDisplayLeftOffset();
		int tileyoff = (int)fallbackTexture->GetDisplayTopOffset();

		tilexoff += spr->xoffset;
		tileyoff += spr->yoffset;

		const float sx = (float)spr->scale.X * 0.8f;
		const float sy = (float)spr->scale.Y;
		const float width = xsize * sx;
		const float height = ysize * sy;
		float xoff = tilexoff * sx;
		float yoff = tileyoff * sy;

		if ((xsize & 1) != 0)
		{
			xoff -= sx * 0.5f;
		}

		if ((spr->cstat & CSTAT_SPRITE_YCENTER) != 0)
		{
			yoff -= height * 0.5f;
			if ((ysize & 1) != 0)
			{
				yoff -= sy * 0.5f;
			}
		}

		if ((spr->cstat & CSTAT_SPRITE_XFLIP) != 0)
		{
			xoff = -xoff;
		}

		float ul = (spr->cstat & CSTAT_SPRITE_XFLIP) != 0 ? 0.0f : 1.0f;
		float ur = (spr->cstat & CSTAT_SPRITE_XFLIP) != 0 ? 1.0f : 0.0f;
		float vt = (spr->cstat & CSTAT_SPRITE_YFLIP) != 0 ? 0.0f : 1.0f;
		float vb = (spr->cstat & CSTAT_SPRITE_YFLIP) != 0 ? 1.0f : 0.0f;
		if (fallbackTexture->isHardwareCanvas())
		{
			std::swap(vt, vb);
		}

		const auto& vp = di.Viewpoint;
		const float x = (float)spr->pos.X;
		const float y = (float)-spr->pos.Y;
		const float z = (float)-spr->pos.Z;
		const float viewvecX = (float)vp.ViewVector.X;
		const float viewvecY = (float)vp.ViewVector.Y;
		const float x1 = x - viewvecY * (xoff - (width * 0.5f));
		const float x2 = x - viewvecY * (xoff + (width * 0.5f));
		const float y1 = y + viewvecX * (xoff - (width * 0.5f));
		const float y2 = y + viewvecX * (xoff + (width * 0.5f));
		float z1 = z + yoff;
		float z2 = z + height + yoff;
		if (z1 < z2)
		{
			std::swap(z1, z2);
			std::swap(vt, vb);
		}

		SurfaceRef surface = {};
		uint32_t extraFlags = MaterialFlag_Sprite | MaterialFlag_AlphaClip;
		if (sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr)
		{
			extraFlags |= MaterialFlag_FacingBillboard;
		}
		surface.material = MakeMaterialRef(fallbackTexture, sprite.palette, sprite.shade, sprite.alpha, extraFlags);
		surface.provenance = MakeSpriteProvenance(sprite, SurfaceSourceType::FacingSprite, drawListType, surface.material.flags);
		surface.vertices.reserve(4);
		surface.vertices.push_back(MakeCapturedVertex(x1, y1, z1, ul, vt));
		surface.vertices.push_back(MakeCapturedVertex(x2, y2, z1, ur, vt));
		surface.vertices.push_back(MakeCapturedVertex(x1, y1, z2, ul, vb));
		surface.vertices.push_back(MakeCapturedVertex(x2, y2, z2, ur, vb));

		if (sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr)
		{
			ApplyActorPreviousTransform(surface, sprite.Sprite->ownerActor);
		}

		outSprites.push_back(std::move(surface));
		return true;
	}

	void TransformModelPoint(const VSMatrix& matrix, float x, float y, float z, CapturedVertex& outVertex, float u, float v)
	{
		float point[4] = { x, y, z, 1.0f };
		float transformed[4] = {};
		VSMatrix copy = matrix;
		copy.multMatrixPoint(point, transformed);

		outVertex.position[0] = transformed[0];
		outVertex.position[1] = transformed[1];
		outVertex.position[2] = transformed[2];
		outVertex.prevPosition[0] = transformed[0];
		outVertex.prevPosition[1] = transformed[1];
		outVertex.prevPosition[2] = transformed[2];
		outVertex.uv[0] = u;
		outVertex.uv[1] = v;
	}

	CapturedVertex MakeCapturedModelVertex(const VSMatrix& matrix, const FModelVertex& source)
	{
		CapturedVertex vertex = {};
		TransformModelPoint(matrix, source.x, source.y, source.z, vertex, source.u, source.v);
		return vertex;
	}

	CapturedVertex MakeCapturedLocalModelVertex(const FModelVertex& source)
	{
		CapturedVertex vertex = {};
		vertex.position[0] = source.x;
		vertex.position[1] = source.y;
		vertex.position[2] = source.z;
		vertex.prevPosition[0] = source.x;
		vertex.prevPosition[1] = source.y;
		vertex.prevPosition[2] = source.z;
		vertex.uv[0] = source.u;
		vertex.uv[1] = source.v;
		return vertex;
	}

	void AddVoxelProxyFace(const VSMatrix& matrix, const float* extents, const int* indices, SurfaceRef& outSurface)
	{
		static const float corners[8][3] = {
			{ 0.0f, 0.0f, 0.0f },
			{ 1.0f, 0.0f, 0.0f },
			{ 1.0f, 1.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f },
			{ 1.0f, 0.0f, 1.0f },
			{ 1.0f, 1.0f, 1.0f },
			{ 0.0f, 1.0f, 1.0f },
		};

		static const float uvs[4][2] = {
			{ 0.0f, 0.0f },
			{ 1.0f, 0.0f },
			{ 1.0f, 1.0f },
			{ 0.0f, 1.0f },
		};

		for (int i = 0; i < 4; ++i)
		{
			const float* local = corners[indices[i]];
			CapturedVertex vertex = {};
			TransformModelPoint(matrix, local[0] * extents[0], local[1] * extents[1], local[2] * extents[2], vertex, uvs[i][0], uvs[i][1]);
			outSurface.vertices.push_back(vertex);
		}
	}

	FGameTexture* GetVoxelReplacementEmissiveSourceTexture(const HWSprite& sprite)
	{
		if (sprite.Sprite == nullptr)
		{
			return nullptr;
		}

		FGameTexture* sourceTexture = TexMan.GetGameTexture(sprite.Sprite->spritetexture());
		return sourceTexture != nullptr && sourceTexture->isValid() ? sourceTexture : nullptr;
	}

	void CaptureVoxelProxySprite(const HWSprite& sprite, uint32_t drawListType, FGameTexture* voxelTexture, std::vector<SurfaceRef>& outSprites)
	{
		static const int faces[6][4] = {
			{ 0, 1, 2, 3 },
			{ 4, 5, 6, 7 },
			{ 0, 4, 7, 3 },
			{ 1, 5, 6, 2 },
			{ 3, 2, 6, 7 },
			{ 0, 1, 5, 4 },
		};

		const float extents[3] = {
			(float)sprite.voxel->siz.X,
			(float)sprite.voxel->siz.Z,
			(float)sprite.voxel->siz.Y
		};

		for (const auto& face : faces)
		{
			SurfaceRef surface = {};
			surface.material = MakeMaterialRef(voxelTexture, sprite.palette, sprite.shade, sprite.alpha, MaterialFlag_Sprite | MaterialFlag_AlphaClip);
			surface.material.emissiveSourceTexture = GetVoxelReplacementEmissiveSourceTexture(sprite);
			surface.material.flags |= MaterialFlag_PointSampled | MaterialFlag_Indexed;
			surface.provenance = MakeSpriteProvenance(sprite, SurfaceSourceType::VoxelProxySprite, drawListType, surface.material.flags);
			surface.vertices.reserve(4);
			AddVoxelProxyFace(sprite.rotmat, extents, face, surface);
			if (sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr)
			{
				ApplyActorPreviousTransform(surface, sprite.Sprite->ownerActor);
			}
			outSprites.push_back(std::move(surface));
		}
	}

	MaterialRef MakeVoxelPaletteMaterialRef(FGameTexture* voxelTexture, FGameTexture* emissiveSourceTexture, int palette, int shade, float alpha, uint32_t extraFlags)
	{
		MaterialRef material = MakeMaterialRef(voxelTexture, palette, shade, alpha, extraFlags | MaterialFlag_PointSampled);
		material.emissiveSourceTexture = emissiveSourceTexture;
		material.flags |= MaterialFlag_Indexed;
		return material;
	}

	uint64_t HashCombine64(uint64_t hash, uint64_t value)
	{
		hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
		return hash;
	}

	uint64_t QuantizeSignatureFloat(double value, double scale)
	{
		if (!std::isfinite(value))
		{
			return 0ull;
		}
		return (uint64_t)std::llround(value * scale);
	}

	uint32_t QuantizeSignatureFloat32(double value, double scale)
	{
		return (uint32_t)std::min<uint64_t>(QuantizeSignatureFloat(value, scale), UINT32_MAX);
	}

	int ResolveVoxelIndex(const HWSprite& sprite)
	{
		if (sprite.voxel == nullptr)
		{
			return -1;
		}

		if (sprite.Sprite != nullptr)
		{
			const int textureVoxelIndex = GetExtInfo(sprite.Sprite->spritetexture()).tiletovox;
			if (textureVoxelIndex >= 0 && textureVoxelIndex < MAXVOXELS && voxmodels[textureVoxelIndex] == sprite.voxel)
			{
				return textureVoxelIndex;
			}
		}

		for (int i = 0; i < MAXVOXELS; ++i)
		{
			if (voxmodels[i] == sprite.voxel)
			{
				return i;
			}
		}
		return -1;
	}

	VoxelMeshVariantKey BuildVoxelMeshVariantKey(const HWSprite& sprite)
	{
		VoxelMeshVariantKey key = {};
		key.voxel = sprite.voxel;
		key.model = sprite.voxel != nullptr ? sprite.voxel->model : nullptr;
		key.sourcePicnum = sprite.Sprite != nullptr ? sprite.Sprite->spritetexture() : FTextureID();
		key.resolvedVoxelIndex = ResolveVoxelIndex(sprite);
		key.geometryState = 0;
		return key;
	}

	VoxelMaterialVariantKey BuildVoxelMaterialVariantKey(FGameTexture* voxelTexture, const MaterialRef& material)
	{
		VoxelMaterialVariantKey key = {};
		key.voxelTexture = voxelTexture;
		key.emissiveSourceTexture = material.emissiveSourceTexture;
		key.palette = material.palette;
		key.shade = material.shade;
		key.alphaBits = QuantizeSignatureFloat32(material.alpha, 65535.0);
		key.materialFlags = material.flags;
		return key;
	}

	uint64_t BuildVoxelMeshVariantKeyHash(const VoxelMeshVariantKey& key)
	{
		if (key.model == nullptr)
		{
			return 0;
		}

		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)key.voxel);
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)key.model);
		hash = HashCombine64(hash, key.sourcePicnum.isValid() ? (uint64_t)(uint32_t)key.sourcePicnum.GetIndex() : 0ull);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)key.resolvedVoxelIndex);
		hash = HashCombine64(hash, (uint64_t)key.geometryState);
		return hash;
	}

	uint64_t BuildVoxelMaterialVariantKeyHash(const VoxelMaterialVariantKey& key)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, key.voxelTexture != nullptr ? (uint64_t)(uint32_t)key.voxelTexture->GetID().GetIndex() : 0ull);
		hash = HashCombine64(hash, key.emissiveSourceTexture != nullptr ? (uint64_t)(uint32_t)key.emissiveSourceTexture->GetID().GetIndex() : 0ull);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)key.palette);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)key.shade);
		hash = HashCombine64(hash, (uint64_t)key.alphaBits);
		hash = HashCombine64(hash, (uint64_t)key.materialFlags);
		return hash;
	}

	VoxelGeometryContentHashes BuildRawVoxelGeometryContentHashes(
		const FVoxelRawMeshStats& stats,
		int32_t sourcePicnum,
		int32_t resolvedVoxelIndex)
	{
		VoxelGeometryContentHashes hashes = {};
		if (stats.contentHash == 0)
		{
			return hashes;
		}
		hashes.geometryContentHash = HashCombine64(0x565847454f4d3033ull, stats.contentHash); // VXGEOM03
		uint64_t renderHash = HashCombine64(0x5658525052493034ull, stats.contentHash); // VXRPRI04
		renderHash = HashCombine64(renderHash, (uint64_t)(uint32_t)sourcePicnum);
		renderHash = HashCombine64(renderHash, (uint64_t)(uint32_t)resolvedVoxelIndex);
		hashes.renderPrimitiveHash = renderHash;
		return hashes;
	}

	uint64_t BuildVoxelActorIdentityKey(const ActorPresentationOwnerKey& owner)
	{
		if (!owner.IsValid())
		{
			return 0;
		}

		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, owner.ownerWorldEpoch);
		hash = HashCombine64(hash, (uint64_t)owner.ownerLifetimeGeneration);
		return hash != 0 ? hash : 1ull;
	}

	uint64_t BuildLegacyVoxelInstanceKeyHash(int32_t actorIndex, uintptr_t actorAddress)
	{
		if (actorIndex < 0 || actorAddress == 0)
		{
			return 0;
		}

		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)(uint32_t)actorIndex);
		hash = HashCombine64(hash, (uint64_t)actorAddress);
		return HashCombine64(hash, 0);
	}

	uint64_t BuildActorPresentationBasisStateHash(const ActorPresentationState& state)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)(uint32_t)state.picnum);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)state.textureId);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)state.displayTextureId);
		hash = HashCombine64(hash, QuantizeSignatureFloat(state.angles.yawDegrees, 4096.0));
		hash = HashCombine64(hash, QuantizeSignatureFloat(state.angles.pitchDegrees, 4096.0));
		hash = HashCombine64(hash, QuantizeSignatureFloat(state.angles.rollDegrees, 4096.0));
		hash = HashCombine64(hash, QuantizeSignatureFloat(state.scale.x, 4096.0));
		hash = HashCombine64(hash, QuantizeSignatureFloat(state.scale.y, 4096.0));
		hash = HashCombine64(hash, QuantizeSignatureFloat(state.modelRotation.yawDegrees, 4096.0));
		hash = HashCombine64(hash, QuantizeSignatureFloat(state.modelRotation.pitchDegrees, 4096.0));
		hash = HashCombine64(hash, QuantizeSignatureFloat(state.modelRotation.rollDegrees, 4096.0));
		hash = HashCombine64(hash, QuantizeSignatureFloat(state.modelPositionOffset.x, 4096.0));
		hash = HashCombine64(hash, QuantizeSignatureFloat(state.modelPositionOffset.y, 4096.0));
		hash = HashCombine64(hash, QuantizeSignatureFloat(state.modelPositionOffset.z, 4096.0));
		hash = HashCombine64(hash, (uint64_t)state.cstat);
		hash = HashCombine64(hash, (uint64_t)state.cstat2);
		hash = HashCombine64(hash, (uint64_t)state.renderFlags);
		return HashCombine64(hash, QuantizeSignatureFloat((double)state.alpha, 65535.0));
	}

	bool IsActorPresentationVoxelCacheOwner(const ActorPresentationState& state)
	{
		return state.authorityCurrent &&
			state.publicationEligible &&
			(state.renderFlags & SPREXT_NOTMD) == 0 &&
			(state.cstat2 & CSTAT2_SPRITE_NOMODEL) == 0;
	}

	bool TryBuildVoxelActorIdentity(const HWSprite& sprite, VoxelActorCacheLookup& lookup)
	{
		if (sprite.Sprite == nullptr || sprite.Sprite->ownerActor == nullptr || sprite.voxel == nullptr || sprite.voxel->model == nullptr)
		{
			return false;
		}

		const ActorPresentationState* authority =
			LookupActorPresentationByActor(sprite.Sprite->ownerActor);
		if (authority == nullptr || !authority->owner.IsValid() || authority->actorIndex < 0)
		{
			return false;
		}

		lookup.identityKey = BuildVoxelActorIdentityKey(authority->owner);
		if (lookup.identityKey == 0)
		{
			return false;
		}

		lookup.actorIndex = authority->actorIndex;
		lookup.ownerWorldEpoch = authority->owner.ownerWorldEpoch;
		lookup.ownerLifetimeGeneration = (uint64_t)authority->owner.ownerLifetimeGeneration;
		lookup.placementGeneration = authority->placementGeneration;
		lookup.placementStateHash = authority->placementStateHash;
		lookup.presentationBasisStateHash = BuildActorPresentationBasisStateHash(*authority);
		lookup.physicalSectorIndex = authority->physicalSectorIndex;
		lookup.authorityCurrent = authority->authorityCurrent;
		lookup.publicationEligible = IsActorPresentationVoxelCacheOwner(*authority);
		lookup.indirectOnly = IsLocalPlayerActor(sprite.Sprite->ownerActor);
		lookup.voxelPtr = (uintptr_t)sprite.voxel;
		lookup.voxelModelPtr = (uintptr_t)sprite.voxel->model;
		lookup.sourcePicnum = sprite.Sprite->spritetexture().GetIndex();
		lookup.instanceKeyHash = lookup.identityKey;
		return true;
	}

	uint64_t BuildVoxelActorMeshKeyHash(const HWSprite& sprite)
	{
		return BuildVoxelMeshVariantKeyHash(BuildVoxelMeshVariantKey(sprite));
	}

	uint64_t BuildVoxelActorSurfaceSignature(const HWSprite& sprite)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)sprite.voxel);
		hash = HashCombine64(hash, (uint64_t)(uintptr_t)sprite.voxel->model);

		if (sprite.Sprite != nullptr)
		{
			hash = HashCombine64(hash, (uint64_t)sprite.Sprite->cstat);
			hash = HashCombine64(hash, (uint64_t)sprite.Sprite->cstat2);
		}

		return hash;
	}

	uint64_t BuildVoxelActorTransformBasisSignature(const HWSprite& sprite)
	{
		uint64_t hash = 1469598103934665603ull;
		const FLOATTYPE* matrix = sprite.rotmat.get();
		for (int i = 0; i < 16; ++i)
		{
			if (i == 12 || i == 13 || i == 14)
			{
				continue;
			}
			hash = HashCombine64(hash, QuantizeSignatureFloat((double)matrix[i], 4096.0));
		}
		return hash;
	}

	void CopyVoxelActorTranslation(const HWSprite& sprite, float outTranslation[3])
	{
		const FLOATTYPE* matrix = sprite.rotmat.get();
		outTranslation[0] = (float)matrix[12];
		outTranslation[1] = (float)matrix[13];
		outTranslation[2] = (float)matrix[14];
	}

	void CopyLiveActorScenePosition(const DCoreActor& actor, float outPosition[3])
	{
		outPosition[0] = (float)actor.spr.pos.X;
		outPosition[1] = (float)-actor.spr.pos.Y;
		outPosition[2] = (float)-actor.spr.pos.Z;
	}

	bool CopyVoxelActorScenePosition(const HWSprite& sprite, float outPosition[3])
	{
		if (sprite.Sprite == nullptr || sprite.Sprite->ownerActor == nullptr)
		{
			return false;
		}
		CopyLiveActorScenePosition(*sprite.Sprite->ownerActor, outPosition);
		return true;
	}

	void CopyVoxelActorTransform(const HWSprite& sprite, float outTransform[12])
	{
		const FLOATTYPE* matrix = sprite.rotmat.get();
		outTransform[0] = (float)matrix[0];
		outTransform[1] = (float)matrix[4];
		outTransform[2] = (float)matrix[8];
		outTransform[3] = (float)matrix[12];
		outTransform[4] = (float)matrix[1];
		outTransform[5] = (float)matrix[5];
		outTransform[6] = (float)matrix[9];
		outTransform[7] = (float)matrix[13];
		outTransform[8] = (float)matrix[2];
		outTransform[9] = (float)matrix[6];
		outTransform[10] = (float)matrix[10];
		outTransform[11] = (float)matrix[14];
	}

	bool SameVoxelTransform(const float a[12], const float b[12])
	{
		constexpr float Epsilon = 0.0001f;
		for (size_t i = 0; i < 12; ++i)
		{
			if (std::abs(a[i] - b[i]) > Epsilon)
			{
				return false;
			}
		}
		return true;
	}

	void FillVoxelTranslationInstanceTransform(const float currentTranslation[3], const float bakedTranslation[3], float outTransform[12])
	{
		outTransform[0] = 1.0f;
		outTransform[1] = 0.0f;
		outTransform[2] = 0.0f;
		outTransform[3] = currentTranslation[0] - bakedTranslation[0];
		outTransform[4] = 0.0f;
		outTransform[5] = 1.0f;
		outTransform[6] = 0.0f;
		outTransform[7] = currentTranslation[1] - bakedTranslation[1];
		outTransform[8] = 0.0f;
		outTransform[9] = 0.0f;
		outTransform[10] = 1.0f;
		outTransform[11] = currentTranslation[2] - bakedTranslation[2];
	}

	uint64_t BuildVoxelActorMaterialSignature(FGameTexture* voxelTexture, const MaterialRef& material)
	{
		return BuildVoxelMaterialVariantKeyHash(BuildVoxelMaterialVariantKey(voxelTexture, material));
	}

	uint64_t BuildVoxelActorSignature(uint64_t geometrySignature, uint64_t materialSignature)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashCombine64(hash, geometrySignature);
		hash = HashCombine64(hash, materialSignature);
		return hash;
	}

	uint64_t BuildVoxelDirectMaterialPayloadKey(uint64_t baseKey, const SurfaceProvenance& provenance)
	{
		if (baseKey == 0 || provenance.actorIndex < 0)
		{
			return baseKey;
		}

		uint64_t hash = HashCombine64(baseKey, 0xD151EC7A11A17E5Bull);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)provenance.sourceType);
		hash = HashCombine64(hash, (uint64_t)(uint32_t)provenance.actorIndex);
		hash = HashCombine64(hash, (uint64_t)provenance.materialFlags);
		return hash;
	}

	const char* GetVoxelActorPendingReasonName(VoxelActorPendingReason reason)
	{
		switch (reason)
		{
		case VoxelActorPendingReason::ActorBudget: return "actor-budget";
		case VoxelActorPendingReason::MeshDeferred: return "mesh-deferred";
		case VoxelActorPendingReason::TriangleBudget: return "triangle-budget";
		case VoxelActorPendingReason::SurfaceBuildFailed: return "surface-build-failed";
		case VoxelActorPendingReason::ActorNotLive: return "actor-not-live";
		default: return "none";
		}
	}

	DynamicVoxelEscapeReason GetDynamicVoxelEscapeReasonForPending(VoxelActorPendingReason reason)
	{
		switch (reason)
		{
		case VoxelActorPendingReason::ActorBudget: return DynamicVoxelEscapeReason::ActorBudget;
		case VoxelActorPendingReason::MeshDeferred:
		case VoxelActorPendingReason::TriangleBudget: return DynamicVoxelEscapeReason::BuildBudget;
		case VoxelActorPendingReason::SurfaceBuildFailed: return DynamicVoxelEscapeReason::MissingSurface;
		case VoxelActorPendingReason::ActorNotLive: return DynamicVoxelEscapeReason::LifecycleTransient;
		default: return DynamicVoxelEscapeReason::VariantPending;
		}
	}

	bool IsDynamicVoxelEscapeEligibleForPersistent(DynamicVoxelEscapeReason reason)
	{
		switch (reason)
		{
		case DynamicVoxelEscapeReason::CameraOrWeaponSpecial:
		case DynamicVoxelEscapeReason::LifecycleTransient:
		case DynamicVoxelEscapeReason::ValidationQuarantine:
			return false;
		default:
			return true;
		}
	}

	bool IsDynamicVoxelEscapeForcedDynamic(DynamicVoxelEscapeReason reason)
	{
		return reason == DynamicVoxelEscapeReason::CameraOrWeaponSpecial ||
			reason == DynamicVoxelEscapeReason::LifecycleTransient;
	}

	bool IsExpectedDynamicVoxelEscape(DynamicVoxelEscapeReason reason)
	{
		switch (reason)
		{
		case DynamicVoxelEscapeReason::CameraOrWeaponSpecial:
		case DynamicVoxelEscapeReason::LifecycleTransient:
		case DynamicVoxelEscapeReason::ValidationQuarantine:
		case DynamicVoxelEscapeReason::MissingSurface:
			return true;
		default:
			return false;
		}
	}

	const char* GetVoxelMeshBakeSpaceName(VoxelMeshBakeSpace bakeSpace)
	{
		switch (bakeSpace)
		{
		case VoxelMeshBakeSpace::LocalSpace: return "local";
		case VoxelMeshBakeSpace::BakedTransform: return "baked";
		default: return "unknown";
		}
	}

	bool IsVoxelMeshTransformKeyed(VoxelMeshBakeSpace bakeSpace)
	{
		return bakeSpace != VoxelMeshBakeSpace::LocalSpace;
	}

	const char* GetVoxelActorStabilityName(VoxelActorStability stability)
	{
		switch (stability)
		{
		case VoxelActorStability::New: return "new";
		case VoxelActorStability::Stable: return "stable";
		case VoxelActorStability::TransformRebake: return "transform-rebake";
		case VoxelActorStability::Changed: return "changed";
		default: return "uncacheable";
		}
	}

	void EmitVoxelActorStateTrace(
		const HWSprite* sprite,
		const VoxelActorCacheLookup* lookup,
		const VoxelActorCacheEntry* entry,
		const char* action,
		VoxelActorPendingReason reason = VoxelActorPendingReason::None)
	{
		const bool traceAllVoxelStats = !!nri_voxelstats;
		if (!traceAllVoxelStats && !nri_ptvoxelactorstatetrace)
		{
			return;
		}
		if (!traceAllVoxelStats)
		{
			static int sTraceCount = 0;
			const int traceLimit = (int)nri_ptvoxelactorstatetracelimit;
			if (traceLimit >= 0 && sTraceCount >= traceLimit)
			{
				return;
			}
			sTraceCount++;
		}

		const int actorIndex =
			lookup != nullptr ? lookup->actorIndex :
			entry != nullptr ? entry->actorIndex :
			sprite != nullptr && sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr ? sprite->Sprite->ownerActor->GetIndex() :
			-1;
		const int statnum = sprite != nullptr && sprite->Sprite != nullptr ? sprite->Sprite->statnum : -1;
		const int picnum = sprite != nullptr && sprite->Sprite != nullptr ? sprite->Sprite->picnum : -1;
		const uint64_t meshKey =
			lookup != nullptr && lookup->meshKeyHash != 0 ? lookup->meshKeyHash :
			entry != nullptr ? entry->meshKeyHash :
			0;
		const uint64_t materialKey =
			lookup != nullptr && lookup->materialKeyHash != 0 ? lookup->materialKeyHash :
			entry != nullptr ? entry->materialKeyHash :
			0;
		const uint64_t instanceKey =
			lookup != nullptr && lookup->instanceKeyHash != 0 ? lookup->instanceKeyHash :
			entry != nullptr ? entry->instanceKeyHash :
			0;
		const uint64_t surfaceSignature =
			lookup != nullptr && lookup->surfaceSignature != 0 ? lookup->surfaceSignature :
			entry != nullptr ? entry->surfaceSignature :
			0;
		const uint64_t desiredMeshKey = entry != nullptr ? entry->desiredMeshKeyHash : 0;
		const uint64_t desiredMaterialKey = entry != nullptr ? entry->desiredMaterialKeyHash : 0;
		const uint64_t meshVariant =
			lookup != nullptr && lookup->meshVariantHash != 0 ? lookup->meshVariantHash :
			entry != nullptr ? entry->meshVariantHash :
			0;
		const uint64_t materialVariant =
			lookup != nullptr && lookup->materialVariantHash != 0 ? lookup->materialVariantHash :
			entry != nullptr ? entry->materialVariantHash :
			0;
		const uint64_t desiredMeshVariant = entry != nullptr ? entry->desiredMeshVariantHash : 0;
		const uint64_t desiredMaterialVariant = entry != nullptr ? entry->desiredMaterialVariantHash : 0;
		const uint64_t desiredSurfaceSignature = entry != nullptr ? entry->desiredSurfaceSignature : 0;
		const uint64_t transformBasisSignature =
			lookup != nullptr && lookup->transformBasisSignature != 0 ? lookup->transformBasisSignature :
			entry != nullptr ? entry->transformBasisSignature :
			0;
		const VoxelMeshBakeSpace meshBakeSpace =
			lookup != nullptr && lookup->meshBakeSpace != VoxelMeshBakeSpace::Unknown ? lookup->meshBakeSpace :
			entry != nullptr ? entry->meshBakeSpace :
			VoxelMeshBakeSpace::Unknown;
		const int32_t resolvedVoxelIndex =
			lookup != nullptr && lookup->resolvedVoxelIndex >= 0 ? lookup->resolvedVoxelIndex :
			entry != nullptr ? entry->resolvedVoxelIndex :
			-1;
		const bool hasSurface = entry != nullptr && entry->hasSurface;
		const bool persistentReady = entry != nullptr && entry->persistentReady;
		const VoxelActorPendingReason pendingReason =
			entry != nullptr ? (VoxelActorPendingReason)entry->pendingReason : VoxelActorPendingReason::None;
		const char* readyState =
			persistentReady ? "persistent" :
			hasSurface ? "surface-only" :
			pendingReason != VoxelActorPendingReason::None ? "pending" :
			"missing";
		const uint64_t pendingAge =
			entry != nullptr && entry->pendingFrame != 0 && gVoxelActorCacheFrame >= entry->pendingFrame ?
			gVoxelActorCacheFrame - entry->pendingFrame :
			0;
		const uint64_t surfaceAge =
			entry != nullptr && entry->surfaceFrame != 0 && gVoxelActorCacheFrame >= entry->surfaceFrame ?
			gVoxelActorCacheFrame - entry->surfaceFrame :
			0;
		const uint32_t primitiveCount = entry != nullptr ? entry->primitiveCount : 0u;

		Printf("PERF pt voxel actor state NRI: frame=%llu actor=%d stat=%d pic=%d action=%s reason=%s stability=%s mesh_key=0x%llx mat_key=0x%llx mesh_variant=0x%llx mat_variant=0x%llx inst_key=0x%llx voxel_index=%d basis_sig=0x%llx space=%s transform_keyed=%u surface_sig=0x%llx desired_mesh=0x%llx desired_mat=0x%llx desired_mesh_variant=0x%llx desired_mat_variant=0x%llx desired_surface=0x%llx persistent=%u has_surface=%u ready=%s prims=%u pending=%s pending_age=%llu surface_age=%llu last_seen=%llu\n",
			(unsigned long long)gVoxelActorCacheFrame,
			actorIndex,
			statnum,
			picnum,
			action != nullptr ? action : "unknown",
			GetVoxelActorPendingReasonName(reason),
			lookup != nullptr ? GetVoxelActorStabilityName(lookup->stability) : "n/a",
			(unsigned long long)meshKey,
			(unsigned long long)materialKey,
			(unsigned long long)meshVariant,
			(unsigned long long)materialVariant,
			(unsigned long long)instanceKey,
			resolvedVoxelIndex,
			(unsigned long long)transformBasisSignature,
			GetVoxelMeshBakeSpaceName(meshBakeSpace),
			IsVoxelMeshTransformKeyed(meshBakeSpace) ? 1u : 0u,
			(unsigned long long)surfaceSignature,
			(unsigned long long)desiredMeshKey,
			(unsigned long long)desiredMaterialKey,
			(unsigned long long)desiredMeshVariant,
			(unsigned long long)desiredMaterialVariant,
			(unsigned long long)desiredSurfaceSignature,
			persistentReady ? 1u : 0u,
			hasSurface ? 1u : 0u,
			readyState,
			primitiveCount,
			GetVoxelActorPendingReasonName(pendingReason),
			(unsigned long long)pendingAge,
			(unsigned long long)surfaceAge,
			(unsigned long long)(entry != nullptr ? entry->lastSeenFrame : 0));
	}

	void EmitVoxelActorKeyTrace(const HWSprite& sprite, const VoxelActorCacheLookup& lookup, const char* action, VoxelActorPendingReason reason = VoxelActorPendingReason::None)
	{
		EmitVoxelActorStateTrace(&sprite, &lookup, lookup.entry, action, reason);
		if ((int)nri_ptactorspritetrace <= 0 || (int)nri_pttraceframes <= 0 || screen == nullptr ||
			sprite.Sprite == nullptr || sprite.Sprite->ownerActor == nullptr)
		{
			return;
		}

		PathTracingActorSpriteTraceEvent event = {};
		event.stage = PathTracingActorSpriteTraceStage::CaptureScene;
		event.actorIndex = sprite.Sprite->ownerActor->GetIndex();
		event.spriteStatnum = sprite.Sprite->statnum;
		event.spritePicnum = sprite.Sprite->picnum;
		event.baseTextureId = sprite.Sprite->spritetexture().GetIndex();
		event.resolvedTextureId = sprite.voxel != nullptr && sprite.voxel->model != nullptr ? sprite.voxel->model->GetPaletteTexture().GetIndex() : -1;
		event.palette = sprite.palette;
		event.shade = sprite.shade;
		event.cstat = sprite.Sprite->cstat;
		event.cstat2 = sprite.Sprite->cstat2;
		event.drawListType = GLDL_MODELS;
		event.noAnimate = (sprite.Sprite->cstat2 & CSTAT2_SPRITE_NOANIMATE) != 0;
		event.fullbright = (sprite.Sprite->cstat2 & CSTAT2_SPRITE_FULLBRIGHT) != 0;
		event.resolvedGameTexture = nullptr;
		event.hasVoxelKeys = true;
		event.voxelMeshKeyHash = lookup.meshKeyHash;
		event.voxelMaterialKeyHash = lookup.materialKeyHash;
		event.voxelInstanceKeyHash = lookup.instanceKeyHash;
		event.voxelSurfaceSignature = lookup.surfaceSignature;
		event.voxelAction = action;
		screen->EmitPathTracingActorSpriteTraceEvent(event);
	}

	void TraceMirrorVoxelCacheDecision(
		const HWSprite& sprite,
		const VoxelActorCacheLookup* lookup,
		const VoxelActorCacheEntry* entry,
		const char* action)
	{
		if (!ShouldTraceMirrorVoxelCapture())
		{
			return;
		}

		const int actorIndex =
			lookup != nullptr ? lookup->actorIndex :
			sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr ? sprite.Sprite->ownerActor->GetIndex() :
			-1;
		const int statnum = sprite.Sprite != nullptr ? sprite.Sprite->statnum : -1;
		const int picnum = sprite.Sprite != nullptr ? sprite.Sprite->picnum : -1;
		const uint64_t identityKey = lookup != nullptr ? lookup->identityKey : 0ull;
		const uint64_t meshVariant = lookup != nullptr ? lookup->meshVariantHash : 0ull;
		const uint64_t materialVariant = lookup != nullptr ? lookup->materialVariantHash : 0ull;
		const uint64_t surfaceSignature = lookup != nullptr ? lookup->surfaceSignature : 0ull;
		const VoxelActorPendingReason pendingReason =
			entry != nullptr ? (VoxelActorPendingReason)entry->pendingReason : VoxelActorPendingReason::None;

		Printf("PERF pt mirror voxel cache NRI: frame=%llu actor=%d stat=%d pic=%d action=%s identity=0x%llx mesh_variant=0x%llx mat_variant=0x%llx surface_sig=0x%llx entry=%u has_surface=%u persistent=%u prims=%u pending=%s last_seen=%llu\n",
			(unsigned long long)gVoxelActorCacheFrame,
			actorIndex,
			statnum,
			picnum,
			action != nullptr ? action : "unknown",
			(unsigned long long)identityKey,
			(unsigned long long)meshVariant,
			(unsigned long long)materialVariant,
			(unsigned long long)surfaceSignature,
			entry != nullptr ? 1u : 0u,
			entry != nullptr && entry->hasSurface ? 1u : 0u,
			entry != nullptr && entry->persistentReady ? 1u : 0u,
			entry != nullptr ? entry->primitiveCount : 0u,
			GetVoxelActorPendingReasonName(pendingReason),
			(unsigned long long)(entry != nullptr ? entry->lastSeenFrame : 0ull));
	}

	void InitializeVoxelActorCacheEntryIdentity(VoxelActorCacheEntry& entry, const VoxelActorCacheLookup& lookup)
	{
		entry.identityKey = lookup.identityKey;
		entry.ownerWorldEpoch = lookup.ownerWorldEpoch;
		entry.ownerLifetimeGeneration = lookup.ownerLifetimeGeneration;
		entry.placementGeneration = lookup.placementGeneration;
		entry.placementStateHash = lookup.placementStateHash;
		entry.presentationBasisStateHash = lookup.presentationBasisStateHash;
		entry.actorIndex = lookup.actorIndex;
		entry.physicalSectorIndex = lookup.physicalSectorIndex;
		entry.voxelPtr = lookup.voxelPtr;
		entry.voxelModelPtr = lookup.voxelModelPtr;
		entry.sourcePicnum = lookup.sourcePicnum;
		entry.resolvedVoxelIndex = lookup.resolvedVoxelIndex;
		entry.instanceKeyHash = lookup.instanceKeyHash;
		entry.meshBakeSpace = lookup.meshBakeSpace;
		entry.indirectOnly = lookup.indirectOnly;
		entry.authorityCurrent = lookup.authorityCurrent;
		entry.publicationEligible = lookup.publicationEligible;
		entry.pendingRemoval = false;
	}

	void UpdateVoxelActorCacheEntryScenePosition(VoxelActorCacheEntry& entry, const VoxelActorCacheLookup& lookup)
	{
		if (!lookup.hasActorScenePosition)
		{
			return;
		}
		entry.lastActorScenePosition[0] = lookup.actorScenePosition[0];
		entry.lastActorScenePosition[1] = lookup.actorScenePosition[1];
		entry.lastActorScenePosition[2] = lookup.actorScenePosition[2];
		entry.hasLastActorScenePosition = true;
	}

	bool UpdateVoxelActorCacheEntryInstanceTransform(VoxelActorCacheEntry& entry, const VoxelActorCacheLookup& lookup)
	{
		const bool transformChanged = !SameVoxelTransform(entry.currentTransform, lookup.currentTransform);
		entry.currentTranslation[0] = lookup.currentTranslation[0];
		entry.currentTranslation[1] = lookup.currentTranslation[1];
		entry.currentTranslation[2] = lookup.currentTranslation[2];
		std::copy(std::begin(lookup.currentTransform), std::end(lookup.currentTransform), std::begin(entry.currentTransform));
		UpdateVoxelActorCacheEntryScenePosition(entry, lookup);
		return transformChanged;
	}

	void StoreVoxelActorDesiredMaterialSurface(
		VoxelActorCacheEntry& entry,
		const MaterialRef& material,
		const SurfaceProvenance& provenance,
		uint32_t primitiveCount)
	{
		entry.desiredMaterialSurface = {};
		entry.desiredMaterialSurface.material = material;
		entry.desiredMaterialSurface.provenance = provenance;
		entry.hasDesiredMaterialSurface = true;
		entry.desiredPrimitiveCount = primitiveCount;
	}

	bool HasLastValidResidentVoxelSurface(const VoxelActorCacheLookup& lookup)
	{
		return lookup.entry != nullptr && lookup.entry->hasSurface && lookup.entry->persistentReady;
	}

	bool IsVoxelMeshVariantSurfaceReady(uint64_t meshVariantHash);
	bool GetReadyVoxelMeshVariantContentHashes(uint64_t meshVariantHash, uint64_t& outGeometryContentHash, uint64_t& outRenderPrimitiveHash);

	bool IsVoxelActorSharedVariantReady(const VoxelActorCacheEntry& entry)
	{
		return entry.hasSurface &&
			entry.meshBakeSpace == VoxelMeshBakeSpace::LocalSpace &&
			IsVoxelMeshVariantSurfaceReady(entry.meshVariantHash);
	}

	DynamicVoxelCaptureRouting ClassifyDynamicVoxelCaptureRouting(
		DynamicVoxelCaptureMode captureMode,
		bool forceTransient,
		bool cacheSurfaceUpdate,
		bool directPublish,
		const VoxelActorCacheLookup& lookup,
		FVoxelModel* model)
	{
		DynamicVoxelCaptureRouting routing = {};
		if (forceTransient)
		{
			routing.cpuMeshClassification = "transient";
			routing.reason = "camera-special";
			return routing;
		}
		if (captureMode != DynamicVoxelCaptureMode::Authoritative)
		{
			routing.cpuMeshClassification = "transient";
			routing.reason = "transient-capture";
			return routing;
		}
		if (!cacheSurfaceUpdate)
		{
			routing.reason = "not-variant-update";
			return routing;
		}
		if (!directPublish)
		{
			routing.reason = "direct-publish-disabled";
			return routing;
		}
		if (!(bool)nri_ptvoxelcomputerawarchive)
		{
			routing.reason = "raw-archive-disabled";
			return routing;
		}
		if (lookup.identityKey == 0 || lookup.meshVariantHash == 0)
		{
			routing.reason = "missing-stable-key";
			return routing;
		}
		if (model == nullptr)
		{
			routing.cpuMeshClassification = "failure";
			routing.reason = "null-model";
			return routing;
		}
		FVoxelRawMeshStats rawStats = {};
		if (!QueryNRIVoxelComputeRawSourceArchiveStats(model, rawStats))
		{
			QueryNRIVoxelComputeRawSourceStats(model, rawStats);
			routing.cpuMeshClassification = "supported";
			routing.reason = "raw-source-pending";
			routing.directOnlyAdmission = true;
			return routing;
		}
		const uint32_t primitiveCount = rawStats.adjacencySurfaceFaceCount * 2u;
		if (primitiveCount == 0)
		{
			routing.cpuMeshClassification = "failure";
			routing.reason = "empty-raw-source";
			return routing;
		}
		const int configuredMaxPrimitives = (int)nri_ptvoxelcomputedirectmaxprimitives;
		const uint32_t maxDirectPublishPrimitives = configuredMaxPrimitives > 0 ? (uint32_t)configuredMaxPrimitives : 0u;
		if (maxDirectPublishPrimitives != 0 && primitiveCount > maxDirectPublishPrimitives)
		{
			routing.reason = "primitive-limit";
			return routing;
		}

		routing.cpuMeshClassification = "supported";
		routing.reason = "direct-only-admission";
		routing.primitiveCount = primitiveCount;
		routing.directOnlyAdmission = true;
		return routing;
	}

	bool CanPromoteVoxelActorCacheEntry(const VoxelActorCacheEntry& entry)
	{
		if (entry.persistentReady)
		{
			return true;
		}
		if (IsVoxelActorSharedVariantReady(entry))
		{
			return true;
		}
		const int configuredPromoteFrames = (int)nri_ptvoxelpersistentpromoteframes;
		const int promoteFrames = configuredPromoteFrames > 0 ? configuredPromoteFrames : 0;
		if (promoteFrames == 0 || entry.surfaceFrame == 0)
		{
			return true;
		}
		return gVoxelActorCacheFrame >= entry.surfaceFrame &&
			gVoxelActorCacheFrame - entry.surfaceFrame >= (uint64_t)promoteFrames;
	}

	void MarkVoxelActorVariantPending(
		const VoxelActorCacheLookup& lookup,
		VoxelActorPendingReason reason,
		const MaterialRef* material = nullptr,
		const SurfaceProvenance* provenance = nullptr,
		uint32_t primitiveCount = 0)
	{
		if (lookup.identityKey == 0)
		{
			return;
		}

		VoxelActorCacheEntry& entry = lookup.entry != nullptr ? *lookup.entry : gVoxelActorCache[lookup.identityKey];
		InitializeVoxelActorCacheEntryIdentity(entry, lookup);
		entry.desiredSignature = lookup.signature;
		entry.desiredMeshKeyHash = lookup.meshKeyHash;
		entry.desiredMaterialKeyHash = lookup.materialKeyHash;
		entry.desiredMeshVariantHash = lookup.meshVariantHash;
		entry.desiredMaterialVariantHash = lookup.materialVariantHash;
		entry.desiredSurfaceSignature = lookup.surfaceSignature;
		entry.pendingReason = (uint8_t)reason;
		entry.pendingFrame = gVoxelActorCacheFrame;
		entry.lastSeenFrame = gVoxelActorCacheFrame;
		const bool transformChanged = UpdateVoxelActorCacheEntryInstanceTransform(entry, lookup);
		if (material != nullptr && provenance != nullptr)
		{
			StoreVoxelActorDesiredMaterialSurface(entry, *material, *provenance, primitiveCount);
			entry.desiredMaterialKeyHash = BuildVoxelDirectMaterialPayloadKey(entry.desiredMaterialKeyHash, *provenance);
			entry.desiredMaterialVariantHash = BuildVoxelDirectMaterialPayloadKey(entry.desiredMaterialVariantHash, *provenance);
			entry.desiredSignature = BuildVoxelActorSignature(entry.desiredMeshVariantHash, entry.desiredMaterialVariantHash);
		}
		if (transformChanged || material != nullptr)
		{
			++gVoxelActorCacheSerial;
		}
		EmitVoxelActorStateTrace(nullptr, &lookup, &entry, "variant-build-queued", reason);
	}

	void TraceVoxelActorFallbackLastValid(const HWSprite& sprite, const VoxelActorCacheLookup& lookup, VoxelActorPendingReason reason)
	{
		EmitVoxelActorKeyTrace(sprite, lookup, "fallback-last-valid", reason);
	}

	void TraceVoxelActorFirstUseFallback(const HWSprite& sprite, const VoxelActorCacheLookup& lookup, VoxelActorPendingReason reason)
	{
		EmitVoxelActorKeyTrace(sprite, lookup, "fallback-empty", reason);
		if (lookup.stability != VoxelActorStability::New) return;
		PerfCompactFirstUseRecord record = {};
		record.actorLifecycleKey = lookup.identityKey;
		record.sourceKey = ((uint64_t)(uint32_t)lookup.sourcePicnum << 32) |
			(uint64_t)(uint32_t)lookup.resolvedVoxelIndex;
		record.meshKey = lookup.meshKeyHash;
		record.materialKey = lookup.materialKeyHash;
		record.validatedSignature = lookup.signature;
		record.rendererFrame = gVoxelActorCacheFrame;
		record.producerFrame = gVoxelActorCacheFrame;
		record.domain = PerfCompactFirstUseDomain::Actor;
		record.stage = PerfCompactFirstUseStage::Request;
		record.state = PerfCompactFirstUseState::Fallback;
		record.flags = PerfCompactFirstUseBegin | PerfCompactFirstUseEnd;
		PerfCompactCaptureNoteFirstUse(record);
	}

	VoxelActorCacheLookup TrackVoxelActorSignature(const HWSprite& sprite, FGameTexture* voxelTexture, const MaterialRef& material, SceneDebugStats& stats)
	{
		VoxelActorCacheLookup lookup = {};
		if (!TryBuildVoxelActorIdentity(sprite, lookup))
		{
			stats.voxelStableUncacheable++;
			stats.voxelStableSplitLive++;
			return lookup;
		}

		stats.voxelStableCandidates++;
		const uint64_t surfaceSignature = BuildVoxelActorSurfaceSignature(sprite);
		const uint64_t transformBasisSignature = BuildVoxelActorTransformBasisSignature(sprite);
		const VoxelMeshVariantKey meshVariantKey = BuildVoxelMeshVariantKey(sprite);
		const VoxelMaterialVariantKey materialVariantKey = BuildVoxelMaterialVariantKey(voxelTexture, material);
		const uint64_t meshVariantHash = BuildVoxelMeshVariantKeyHash(meshVariantKey);
		const uint64_t materialVariantHash = BuildVoxelMaterialVariantKeyHash(materialVariantKey);
		const uint64_t meshKeyHash = meshVariantHash;
		const uint64_t geometrySignature = meshVariantHash;
		const uint64_t materialSignature = materialVariantHash;
		const uint64_t signature = BuildVoxelActorSignature(geometrySignature, materialSignature);
		lookup.signature = signature;
		lookup.geometrySignature = geometrySignature;
		lookup.surfaceSignature = surfaceSignature;
		lookup.transformBasisSignature = transformBasisSignature;
		lookup.materialSignature = materialSignature;
		lookup.meshKeyHash = meshKeyHash;
		lookup.materialKeyHash = materialSignature;
		lookup.meshVariantHash = meshVariantHash;
		lookup.materialVariantHash = materialVariantHash;
		lookup.meshBakeSpace = VoxelMeshBakeSpace::LocalSpace;
		lookup.resolvedVoxelIndex = meshVariantKey.resolvedVoxelIndex;
		CopyVoxelActorTranslation(sprite, lookup.currentTranslation);
		CopyVoxelActorTransform(sprite, lookup.currentTransform);
		lookup.hasActorScenePosition = CopyVoxelActorScenePosition(sprite, lookup.actorScenePosition);
		auto found = gVoxelActorCache.find(lookup.identityKey);
		if (found == gVoxelActorCache.end())
		{
			stats.voxelStableSignatureMisses++;
			stats.voxelStableSplitLive++;
			lookup.stability = VoxelActorStability::New;
			EmitVoxelActorKeyTrace(sprite, lookup, "variant-miss");
			return lookup;
		}

		lookup.entry = &found->second;
		lookup.entry->lastSeenFrame = gVoxelActorCacheFrame;
		InitializeVoxelActorCacheEntryIdentity(*lookup.entry, lookup);
		lookup.entry->desiredSignature = signature;
		lookup.entry->desiredMeshKeyHash = meshKeyHash;
		lookup.entry->desiredMaterialKeyHash = materialSignature;
		lookup.entry->desiredMeshVariantHash = meshVariantHash;
		lookup.entry->desiredMaterialVariantHash = materialVariantHash;
		lookup.entry->desiredSurfaceSignature = surfaceSignature;
		const bool canUpdateByTranslationInstance =
			lookup.entry->hasSurface &&
			lookup.entry->persistentReady &&
			lookup.entry->geometrySignature == geometrySignature &&
			lookup.entry->materialSignature == materialSignature &&
			!SameVoxelTransform(lookup.entry->currentTransform, lookup.currentTransform);
		if (canUpdateByTranslationInstance)
		{
			lookup.entry->signature = signature;
			lookup.entry->surfaceSignature = surfaceSignature;
			lookup.entry->transformBasisSignature = transformBasisSignature;
			lookup.entry->meshBakeSpace = lookup.meshBakeSpace;
			UpdateVoxelActorCacheEntryInstanceTransform(*lookup.entry, lookup);
			lookup.entry->lastSeenFrame = gVoxelActorCacheFrame;
			lookup.entry->pendingReason = (uint8_t)VoxelActorPendingReason::None;
			lookup.entry->pendingFrame = 0;
			stats.voxelStableSignatureChanges++;
			stats.voxelStableSplitStable++;
			stats.voxelCacheSurfaceHits++;
			lookup.stability = VoxelActorStability::Stable;
			++gVoxelActorCacheSerial;
			EmitVoxelActorKeyTrace(sprite, lookup, "transform-instance");
			return lookup;
		}
		if (lookup.entry->signature == signature && lookup.entry->surfaceSignature == surfaceSignature && lookup.entry->hasSurface)
		{
			const bool confirmedStartupSurface = lookup.entry->startupPending && !gVoxelActorStartupTransientMode;
			if (confirmedStartupSurface)
			{
				lookup.entry->startupPending = false;
			}
			const bool promoted = !lookup.entry->persistentReady;
			if (!lookup.entry->persistentReady && !lookup.entry->startupPending && CanPromoteVoxelActorCacheEntry(*lookup.entry))
			{
				lookup.entry->persistentReady = true;
			}
			const bool transformChanged = UpdateVoxelActorCacheEntryInstanceTransform(*lookup.entry, lookup);
			if (confirmedStartupSurface || (promoted && lookup.entry->persistentReady) || transformChanged)
			{
				++gVoxelActorCacheSerial;
			}
			lookup.entry->pendingReason = (uint8_t)VoxelActorPendingReason::None;
			lookup.entry->pendingFrame = 0;
			stats.voxelStableSignatureHits++;
			stats.voxelStableSplitStable++;
			stats.voxelCacheSurfaceHits++;
			lookup.stability = VoxelActorStability::Stable;
			EmitVoxelActorKeyTrace(sprite, lookup, promoted ? (lookup.entry->persistentReady ? "promote" : "promote-deferred") : "hit");
			return lookup;
		}

		if (lookup.entry->geometrySignature == geometrySignature && lookup.entry->surfaceSignature == surfaceSignature && lookup.entry->hasSurface)
		{
			const bool confirmedStartupSurface = lookup.entry->startupPending && !gVoxelActorStartupTransientMode;
			if (confirmedStartupSurface)
			{
				lookup.entry->startupPending = false;
			}
			lookup.entry->signature = signature;
			lookup.entry->materialSignature = materialSignature;
			lookup.entry->materialKeyHash = lookup.materialKeyHash;
			lookup.entry->materialVariantHash = lookup.materialVariantHash;
			lookup.entry->meshBakeSpace = lookup.meshBakeSpace;
			if (!lookup.entry->sharedVariantSurface)
			{
				lookup.entry->surface.material = material;
			}
			lookup.entry->lightSurface.material = material;
			lookup.entry->lastSeenFrame = gVoxelActorCacheFrame;
			const bool promoted = !lookup.entry->persistentReady;
			if (!lookup.entry->persistentReady && !lookup.entry->startupPending && CanPromoteVoxelActorCacheEntry(*lookup.entry))
			{
				lookup.entry->persistentReady = true;
			}
			const bool transformChanged = UpdateVoxelActorCacheEntryInstanceTransform(*lookup.entry, lookup);
			lookup.entry->pendingReason = (uint8_t)VoxelActorPendingReason::None;
			lookup.entry->pendingFrame = 0;
			if (confirmedStartupSurface || !promoted || lookup.entry->persistentReady || transformChanged)
			{
				++gVoxelActorCacheSerial;
			}
			stats.voxelStableSignatureChanges++;
			stats.voxelStableSplitStable++;
			stats.voxelCacheSurfaceHits++;
			lookup.stability = VoxelActorStability::Stable;
			EmitVoxelActorKeyTrace(sprite, lookup, promoted ? (lookup.entry->persistentReady ? "promote" : "promote-deferred") : "material-only");
			return lookup;
		}

		stats.voxelStableSignatureChanges++;
		stats.voxelStableSplitLive++;
		if (lookup.entry->geometrySignature == geometrySignature)
		{
			lookup.stability = VoxelActorStability::TransformRebake;
			const bool materialChanged = lookup.entry->materialSignature != materialSignature;
			EmitVoxelActorKeyTrace(sprite, lookup, materialChanged ? "transform-material" : "transform-only");
		}
		else
		{
			lookup.stability = VoxelActorStability::Changed;
			EmitVoxelActorKeyTrace(sprite, lookup, IsVoxelMeshVariantSurfaceReady(lookup.meshVariantHash) ? "variant-hit" : "variant-miss");
			EmitVoxelActorKeyTrace(sprite, lookup, "variant-switch");
		}
		return lookup;
	}

	bool TryConsumeReadOnlyVoxelActorCacheSurface(const HWSprite& sprite, FGameTexture* voxelTexture, const MaterialRef& material, SceneDebugStats& stats)
	{
		VoxelActorCacheLookup lookup = {};
		if (!TryBuildVoxelActorIdentity(sprite, lookup))
		{
			stats.voxelStableUncacheable++;
			TraceMirrorVoxelCacheDecision(sprite, nullptr, nullptr, "identity-fail");
			return false;
		}

		stats.voxelStableCandidates++;
		const uint64_t surfaceSignature = BuildVoxelActorSurfaceSignature(sprite);
		const uint64_t transformBasisSignature = BuildVoxelActorTransformBasisSignature(sprite);
		const VoxelMeshVariantKey meshVariantKey = BuildVoxelMeshVariantKey(sprite);
		const VoxelMaterialVariantKey materialVariantKey = BuildVoxelMaterialVariantKey(voxelTexture, material);
		const uint64_t meshVariantHash = BuildVoxelMeshVariantKeyHash(meshVariantKey);
		const uint64_t materialVariantHash = BuildVoxelMaterialVariantKeyHash(materialVariantKey);
		const uint64_t meshKeyHash = meshVariantHash;
		const uint64_t geometrySignature = meshVariantHash;
		const uint64_t materialSignature = materialVariantHash;
		const uint64_t signature = BuildVoxelActorSignature(geometrySignature, materialSignature);
		lookup.signature = signature;
		lookup.geometrySignature = geometrySignature;
		lookup.surfaceSignature = surfaceSignature;
		lookup.transformBasisSignature = transformBasisSignature;
		lookup.materialSignature = materialSignature;
		lookup.meshKeyHash = meshKeyHash;
		lookup.materialKeyHash = materialSignature;
		lookup.meshVariantHash = meshVariantHash;
		lookup.materialVariantHash = materialVariantHash;
		lookup.meshBakeSpace = VoxelMeshBakeSpace::LocalSpace;
		lookup.resolvedVoxelIndex = meshVariantKey.resolvedVoxelIndex;
		CopyVoxelActorTransform(sprite, lookup.currentTransform);
		auto found = gVoxelActorCache.find(lookup.identityKey);
		if (found == gVoxelActorCache.end() || !found->second.hasSurface)
		{
			stats.voxelStableSignatureMisses++;
			TraceMirrorVoxelCacheDecision(sprite, &lookup, found != gVoxelActorCache.end() ? &found->second : nullptr, "miss");
			EmitVoxelActorKeyTrace(sprite, lookup, "readonly-miss");
			return false;
		}

		const VoxelActorCacheEntry& entry = found->second;
		if (entry.geometrySignature != geometrySignature)
		{
			stats.voxelStableSignatureChanges++;
			TraceMirrorVoxelCacheDecision(sprite, &lookup, &entry, "geometry-mismatch");
			EmitVoxelActorKeyTrace(sprite, lookup, "readonly-fallback-last-valid");
			stats.voxelStableSplitStable++;
			stats.voxelCacheSurfaceHits++;
			return true;
		}

		if (entry.signature != signature)
		{
			stats.voxelStableSignatureChanges++;
			TraceMirrorVoxelCacheDecision(sprite, &lookup, &entry, "material-mismatch");
			EmitVoxelActorKeyTrace(sprite, lookup, "readonly-material");
		}
		else if (entry.surfaceSignature != surfaceSignature)
		{
			stats.voxelStableSignatureChanges++;
			TraceMirrorVoxelCacheDecision(sprite, &lookup, &entry, "transform-mismatch");
			EmitVoxelActorKeyTrace(sprite, lookup, "readonly-transform");
		}
		else
		{
			stats.voxelStableSignatureHits++;
			TraceMirrorVoxelCacheDecision(sprite, &lookup, &entry, "hit");
			EmitVoxelActorKeyTrace(sprite, lookup, "readonly-hit");
		}

		stats.voxelStableSplitStable++;
		stats.voxelCacheSurfaceHits++;
		return true;
	}

	void NormalizeCachedSurfacePreviousPositions(SurfaceRef& surface)
	{
		for (CapturedVertex& vertex : surface.vertices)
		{
			vertex.prevPosition[0] = vertex.position[0];
			vertex.prevPosition[1] = vertex.position[1];
			vertex.prevPosition[2] = vertex.position[2];
		}
	}

	uint32_t CountSurfacePrimitives(const SurfaceRef& surface)
	{
		if (!surface.indices.empty())
		{
			return (uint32_t)(surface.indices.size() / 3u);
		}
		return surface.vertices.size() >= 3 ? (uint32_t)surface.vertices.size() - 2u : 0u;
	}

	void StoreVoxelActorCacheSurface(
		const VoxelActorCacheLookup& lookup,
		const SurfaceRef& meshSurface,
		const SurfaceRef& lightSurface,
		VoxelMeshBakeSpace meshBakeSpace,
		bool sharedVariantReady,
		SceneDebugStats& stats)
	{
		if (lookup.identityKey == 0)
		{
			return;
		}

		VoxelActorCacheEntry& entry = gVoxelActorCache[lookup.identityKey];
		const bool hadSurface = entry.hasSurface;
		const bool wasPersistentReady = entry.persistentReady;
		entry.signature = lookup.signature;
		entry.geometrySignature = lookup.geometrySignature;
		entry.surfaceSignature = lookup.surfaceSignature;
		entry.bakedSurfaceSignature = lookup.surfaceSignature;
		entry.materialSignature = lookup.materialSignature;
		entry.transformBasisSignature = lookup.transformBasisSignature;
		entry.meshKeyHash = lookup.meshKeyHash;
		entry.materialKeyHash = lookup.materialKeyHash;
		uint64_t geometryContentHash = lookup.geometryContentHash;
		uint64_t renderPrimitiveHash = lookup.renderPrimitiveHash;
		if ((geometryContentHash == 0 || renderPrimitiveHash == 0) &&
			sharedVariantReady &&
			lookup.meshVariantHash != 0)
		{
			uint64_t cachedGeometryContentHash = 0;
			uint64_t cachedRenderPrimitiveHash = 0;
			if (GetReadyVoxelMeshVariantContentHashes(lookup.meshVariantHash, cachedGeometryContentHash, cachedRenderPrimitiveHash))
			{
				if (geometryContentHash == 0)
				{
					geometryContentHash = cachedGeometryContentHash;
				}
				if (renderPrimitiveHash == 0)
				{
					renderPrimitiveHash = cachedRenderPrimitiveHash;
				}
			}
		}
		if (geometryContentHash == 0 || renderPrimitiveHash == 0)
		{
			const VoxelGeometryContentHashes hashes = BuildVoxelGeometryContentHashes(meshSurface);
			if (geometryContentHash == 0)
			{
				geometryContentHash = hashes.geometryContentHash;
			}
			if (renderPrimitiveHash == 0)
			{
				renderPrimitiveHash = hashes.renderPrimitiveHash;
			}
		}
		entry.geometryContentHash = geometryContentHash;
		entry.renderPrimitiveHash = renderPrimitiveHash;
		entry.meshVariantHash = lookup.meshVariantHash;
		entry.materialVariantHash = lookup.materialVariantHash;
		InitializeVoxelActorCacheEntryIdentity(entry, lookup);
		UpdateVoxelActorCacheEntryScenePosition(entry, lookup);
		entry.meshBakeSpace = meshBakeSpace;
		entry.desiredSignature = lookup.signature;
		entry.desiredMeshKeyHash = lookup.meshKeyHash;
		entry.desiredMaterialKeyHash = lookup.materialKeyHash;
		entry.desiredMeshVariantHash = lookup.meshVariantHash;
		entry.desiredMaterialVariantHash = lookup.materialVariantHash;
		entry.desiredSurfaceSignature = lookup.surfaceSignature;
		entry.pendingReason = (uint8_t)VoxelActorPendingReason::None;
		entry.pendingFrame = 0;
		entry.sharedVariantSurface =
			sharedVariantReady &&
			meshBakeSpace == VoxelMeshBakeSpace::LocalSpace &&
			lookup.meshVariantHash != 0;
		if (entry.sharedVariantSurface)
		{
			entry.surface = {};
		}
		else
		{
			entry.surface = meshSurface;
			entry.surface.material = lightSurface.material;
			entry.surface.provenance = lightSurface.provenance;
			NormalizeCachedSurfacePreviousPositions(entry.surface);
		}
		entry.lightSurface = {};
		entry.lightSurface.material = lightSurface.material;
		entry.lightSurface.provenance = lightSurface.provenance;
		entry.lastSeenFrame = gVoxelActorCacheFrame;
		entry.surfaceFrame = gVoxelActorCacheFrame;
		entry.primitiveCount = CountSurfacePrimitives(meshSurface);
		entry.currentTranslation[0] = lookup.currentTranslation[0];
		entry.currentTranslation[1] = lookup.currentTranslation[1];
		entry.currentTranslation[2] = lookup.currentTranslation[2];
		entry.bakedTranslation[0] = lookup.currentTranslation[0];
		entry.bakedTranslation[1] = lookup.currentTranslation[1];
		entry.bakedTranslation[2] = lookup.currentTranslation[2];
		std::copy(std::begin(lookup.currentTransform), std::end(lookup.currentTransform), std::begin(entry.currentTransform));
		// Transform rebakes and state variant switches are transitional updates of an
		// already valid actor. Keep already-resident actors renderable and let the
		// persistent actor path update the resource in place. First-use actors still
		// wait for the normal stable-frame promotion path unless the shared canonical
		// variant is ready, in which case actor promotion latency must not force a
		// large voxel through the dynamic overlay.
		entry.startupPending = gVoxelActorStartupTransientMode;
		entry.persistentReady =
			!entry.startupPending &&
			(sharedVariantReady ||
				(wasPersistentReady &&
					(lookup.stability == VoxelActorStability::TransformRebake ||
					 lookup.stability == VoxelActorStability::Changed)));
		entry.hasSurface = true;
		++gVoxelActorCacheSerial;
		if (sharedVariantReady && !wasPersistentReady)
		{
			EmitVoxelActorStateTrace(nullptr, &lookup, &entry, "shared-variant-promote", VoxelActorPendingReason::None);
		}

		if (lookup.stability == VoxelActorStability::New || !hadSurface)
		{
			stats.voxelCacheSurfaceStores++;
		}
		else if (lookup.stability == VoxelActorStability::TransformRebake)
		{
			stats.voxelCacheTransformRebakes++;
		}
		else if (lookup.stability == VoxelActorStability::Changed)
		{
			stats.voxelCacheSurfaceRebuilds++;
		}
	}

	bool IsLiveActorVoxelCacheOwner(DCoreActor* actor)
	{
		if (actor == nullptr ||
			!actor->exists() ||
			(actor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			return false;
		}
		if ((actor->sprext.renderflags & (SPREXT_NOTMD | SPREXT_TEMPINVISIBLE)) != 0 ||
			(actor->spr.cstat2 & CSTAT2_SPRITE_NOMODEL) != 0 ||
			(actor->spr.cstat & CSTAT_SPRITE_INVISIBLE) != 0)
		{
			return false;
		}
		return tilehasvoxel(actor->spr.spritetexture()) != 0;
	}

	bool IsLiveActorVoxelWarmupCandidate(DCoreActor* actor)
	{
		if (actor == nullptr ||
			!actor->exists() ||
			(actor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			return false;
		}
		if ((actor->sprext.renderflags & (SPREXT_NOTMD | SPREXT_TEMPINVISIBLE)) != 0 ||
			(actor->spr.cstat2 & CSTAT2_SPRITE_NOMODEL) != 0 ||
			(actor->spr.cstat & CSTAT_SPRITE_INVISIBLE) != 0 ||
			actor->spr.scale.X == 0.0 ||
			actor->spr.scale.Y == 0.0)
		{
			return false;
		}
		return true;
	}

	struct LoadingVoxelCpuBudget
	{
		uint32_t variantLimit = UINT32_MAX;
		uint32_t primitiveLimit = 0;
		int timeLimitMs = 0;
		int mode = 0;
	};

	struct LoadingVoxelGpuBudget
	{
		uint32_t minPrimitiveCount = 0;
		uint32_t variantLimit = UINT32_MAX;
		uint32_t primitiveLimit = 0;
		uint64_t byteLimit = 0;
		int mode = 0;
	};

	int NormalizeLoadingVoxelBudgetMode(int mode)
	{
		return (std::max)(0, (std::min)(4, mode));
	}

	LoadingVoxelCpuBudget GetLoadingVoxelCpuBudget()
	{
		LoadingVoxelCpuBudget budget = {};
		budget.mode = NormalizeLoadingVoxelBudgetMode((int)nri_ptloadingvoxelcpubudget);
		switch (budget.mode)
		{
		case 1:
			budget.variantLimit = 64;
			budget.primitiveLimit = 1000000;
			return budget;
		case 2:
			budget.variantLimit = 128;
			budget.primitiveLimit = 2000000;
			return budget;
		case 3:
			budget.variantLimit = 256;
			budget.primitiveLimit = 4000000;
			return budget;
		case 4:
			budget.variantLimit = UINT32_MAX;
			budget.primitiveLimit = 0;
			return budget;
		default:
			budget.variantLimit = (int)nri_ptloadingvoxelcpumaxvariants <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptloadingvoxelcpumaxvariants;
			budget.primitiveLimit = (int)nri_ptloadingvoxelcpumaxprims <= 0 ? 0u : (uint32_t)(int)nri_ptloadingvoxelcpumaxprims;
			budget.timeLimitMs = (int)nri_ptloadingvoxelcpumaxms;
			return budget;
		}
	}

	LoadingVoxelGpuBudget GetLoadingVoxelGpuBudget()
	{
		LoadingVoxelGpuBudget budget = {};
		budget.mode = NormalizeLoadingVoxelBudgetMode((int)nri_ptloadingvoxelgpubudget);
		switch (budget.mode)
		{
		case 1:
			budget.minPrimitiveCount = 10000;
			budget.variantLimit = 16;
			budget.primitiveLimit = 375000;
			budget.byteLimit = 32ull * 1024ull * 1024ull;
			return budget;
		case 2:
			budget.minPrimitiveCount = 10000;
			budget.variantLimit = 32;
			budget.primitiveLimit = 750000;
			budget.byteLimit = 64ull * 1024ull * 1024ull;
			return budget;
		case 3:
			budget.minPrimitiveCount = 5000;
			budget.variantLimit = 64;
			budget.primitiveLimit = 1500000;
			budget.byteLimit = 128ull * 1024ull * 1024ull;
			return budget;
		case 4:
			budget.minPrimitiveCount = 0;
			budget.variantLimit = UINT32_MAX;
			budget.primitiveLimit = 0;
			budget.byteLimit = 0;
			return budget;
		default:
			budget.minPrimitiveCount = (int)nri_ptloadingvoxelgpuminprims <= 0 ? 0u : (uint32_t)(int)nri_ptloadingvoxelgpuminprims;
			budget.variantLimit = (int)nri_ptloadingvoxelgpumaxvariants <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptloadingvoxelgpumaxvariants;
			budget.variantLimit = (std::min)(budget.variantLimit, (int)nri_ptloadingvoxelvariants <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptloadingvoxelvariants);
			budget.primitiveLimit = (int)nri_ptloadingvoxelgpumaxprims <= 0 ? 0u : (uint32_t)(int)nri_ptloadingvoxelgpumaxprims;
			budget.byteLimit = (int)nri_ptloadingvoxelgpumaxbytes <= 0 ? 0ull : (uint64_t)(int)nri_ptloadingvoxelgpumaxbytes;
			return budget;
		}
	}

	const char* LoadingVoxelPriorityName(LoadingVoxelRequestPriority priority)
	{
		switch (priority)
		{
		case LoadingVoxelRequestPriority::Force: return "force";
		case LoadingVoxelRequestPriority::High: return "high";
		case LoadingVoxelRequestPriority::Normal: return "normal";
		case LoadingVoxelRequestPriority::Opportunistic: return "opportunistic";
		default: return "unknown";
		}
	}

	std::string LoadingVoxelSourceBitsName(uint32_t sourceBits)
	{
		std::string result;
		auto append = [&](uint32_t bit, const char* name)
		{
			if ((sourceBits & bit) == 0)
			{
				return;
			}
			if (!result.empty())
			{
				result += "|";
			}
			result += name;
		};
		append(LoadingVoxelRequestSource_LiveActorCurrent, "live-actor-current");
		append(LoadingVoxelRequestSource_LiveActorAnimated, "live-actor-animated");
		append(LoadingVoxelRequestSource_LiveActorPicrange, "live-actor-picrange");
		append(LoadingVoxelRequestSource_MountedVoxelPreload, "mounted-voxel-preload");
		append(LoadingVoxelRequestSource_MountedVoxelPreloadMap, "mounted-map");
		append(LoadingVoxelRequestSource_MountedVoxelPreloadGame, "mounted-game");
		append(LoadingVoxelRequestSource_MountedVoxelPreloadGlobal, "mounted-global");
		return result.empty() ? "none" : result;
	}

	int LoadingVoxelSourceRank(uint32_t sourceBits)
	{
		if ((sourceBits & LoadingVoxelRequestSource_MountedVoxelPreloadMap) != 0)
		{
			return 0;
		}
		if ((sourceBits & LoadingVoxelRequestSource_MountedVoxelPreloadGame) != 0)
		{
			return 1;
		}
		if ((sourceBits & LoadingVoxelRequestSource_MountedVoxelPreloadGlobal) != 0 ||
			(sourceBits & LoadingVoxelRequestSource_MountedVoxelPreload) != 0)
		{
			return 2;
		}
		if ((sourceBits & LoadingVoxelRequestSource_LiveActorCurrent) != 0)
		{
			return 3;
		}
		if ((sourceBits & LoadingVoxelRequestSource_LiveActorAnimated) != 0)
		{
			return 4;
		}
		if ((sourceBits & LoadingVoxelRequestSource_LiveActorPicrange) != 0)
		{
			return 6;
		}
		return 5;
	}

	int LoadingVoxelGpuIntentRank(const LoadingVoxelPreloadRequest& request)
	{
		if (request.gpuForce)
		{
			return 0;
		}
		if (request.gpuPrefer)
		{
			return 1;
		}
		return 2;
	}

	int LoadingVoxelAdmissionRank(const LoadingVoxelPreloadRequest& request)
	{
		return (int)request.priority * 10000 + LoadingVoxelSourceRank(request.sourceBits) * 100 + LoadingVoxelGpuIntentRank(request);
	}

	bool IsLoadingVoxelRequestGpuCandidate(const LoadingVoxelPreloadRequest& request)
	{
		if (!nri_ptloadingvoxelgpu)
		{
			return false;
		}
		const bool explicitGpu =
			(request.sourceBits & LoadingVoxelRequestSource_MountedVoxelPreload) != 0 &&
			(request.gpuForce || request.gpuPrefer);
		if (explicitGpu)
		{
			return true;
		}
		if (nri_ptloadingvoxelgpuwhitelistonly)
		{
			return false;
		}
		const uint32_t stableSources =
			LoadingVoxelRequestSource_LiveActorCurrent |
			LoadingVoxelRequestSource_LiveActorAnimated;
		return (request.sourceBits & stableSources) != 0;
	}

	void SortLoadingVoxelPreloadRequests(LoadingVoxelPreloadRequestGraph& graph)
	{
		std::sort(graph.requests.begin(), graph.requests.end(), [](const LoadingVoxelPreloadRequest& a, const LoadingVoxelPreloadRequest& b)
		{
			if (a.priority != b.priority)
			{
				return (uint8_t)a.priority < (uint8_t)b.priority;
			}
			const int sourceRankA = LoadingVoxelSourceRank(a.sourceBits);
			const int sourceRankB = LoadingVoxelSourceRank(b.sourceBits);
			if (sourceRankA != sourceRankB)
			{
				return sourceRankA < sourceRankB;
			}
			if (a.actorIndex != b.actorIndex)
			{
				return a.actorIndex < b.actorIndex;
			}
			return a.texid.GetIndex() < b.texid.GetIndex();
		});
	}

	void AddUniqueLoadingActorTextureCandidate(
		FTextureID texid,
		uint32_t sourceBits,
		LoadingVoxelRequestPriority priority,
		std::vector<LoadingVoxelTextureCandidate>& candidates,
		std::unordered_map<int, size_t>& seenTextureIds)
	{
		if (!texid.isValid())
		{
			return;
		}

		const int textureId = texid.GetIndex();
		auto found = seenTextureIds.find(textureId);
		if (found != seenTextureIds.end())
		{
			LoadingVoxelTextureCandidate& candidate = candidates[found->second];
			candidate.sourceBits |= sourceBits;
			if ((uint8_t)priority < (uint8_t)candidate.priority)
			{
				candidate.priority = priority;
			}
			return;
		}

		LoadingVoxelTextureCandidate candidate = {};
		candidate.texid = texid;
		candidate.sourceBits = sourceBits;
		candidate.priority = priority;
		seenTextureIds.emplace(textureId, candidates.size());
		candidates.push_back(candidate);
	}

	void AddLoadingActorVoxelTextureRangeCandidates(
		int centerPicnum,
		int range,
		std::vector<LoadingVoxelTextureCandidate>& candidates,
		std::unordered_map<int, size_t>& seenTextureIds)
	{
		if (range <= 0 || centerPicnum < 0 || centerPicnum >= MAXTILES)
		{
			return;
		}

		const int firstPicnum = (std::max)(0, centerPicnum - range);
		const int lastPicnum = (std::min)(MAXTILES - 1, centerPicnum + range);
		for (int picnum = firstPicnum; picnum <= lastPicnum; ++picnum)
		{
			FTextureID texid = tileGetTextureID(picnum);
			if (!texid.isValid())
			{
				continue;
			}

			if (ResolveVoxelTextureModel(texid) != nullptr)
			{
				AddUniqueLoadingActorTextureCandidate(
					texid,
					LoadingVoxelRequestSource_LiveActorPicrange,
					LoadingVoxelRequestPriority::Opportunistic,
					candidates,
					seenTextureIds);
			}
		}
	}

	void BuildLoadingActorTextureCandidates(DCoreActor* actor, std::vector<LoadingVoxelTextureCandidate>& candidates)
	{
		candidates.clear();
		if (actor == nullptr)
		{
			return;
		}

		std::unordered_map<int, size_t> seenTextureIds;
		auto addBaseAndAnimated = [&](FTextureID texid)
		{
			AddUniqueLoadingActorTextureCandidate(
				texid,
				LoadingVoxelRequestSource_LiveActorCurrent,
				LoadingVoxelRequestPriority::High,
				candidates,
				seenTextureIds);
			if (texid.isValid() && (actor->spr.cstat2 & CSTAT2_SPRITE_NOANIMATE) == 0)
			{
				FTextureID animatedTexid = texid;
				tileUpdatePicnum(animatedTexid, actor->GetIndex() & 16383);
				AddUniqueLoadingActorTextureCandidate(
					animatedTexid,
					LoadingVoxelRequestSource_LiveActorAnimated,
					LoadingVoxelRequestPriority::Normal,
					candidates,
					seenTextureIds);
			}
		};

		addBaseAndAnimated(actor->spr.spritetexture());
		addBaseAndAnimated(actor->dispictex);
		AddLoadingActorVoxelTextureRangeCandidates(actor->spr.picnum, (int)nri_ptloadingvoxelpicrange, candidates, seenTextureIds);
	}

	bool AddLoadingVoxelPreloadRequest(
		LoadingVoxelPreloadRequestGraph& graph,
		DCoreActor* actor,
		const LoadingVoxelTextureCandidate& candidate)
	{
		graph.discovered++;
		int voxelIndex = -1;
		FVoxelModel* model = ResolveVoxelTextureModel(candidate.texid, &voxelIndex);
		if (model == nullptr)
		{
			graph.skippedInvalid++;
			return false;
		}

		const VoxelMeshVariantKey meshVariantKey = BuildLoadingVoxelMeshVariantKey(candidate.texid, model, voxelIndex);
		const uint64_t meshVariantHash = BuildVoxelMeshVariantKeyHash(meshVariantKey);
		if (meshVariantHash == 0)
		{
			graph.skippedInvalid++;
			return false;
		}

		LoadingVoxelMaterialContext materialContext = {};
		materialContext.texid = candidate.texid;
		materialContext.actor = actor;
		materialContext.actorIndex = actor != nullptr ? (int32_t)actor->GetIndex() : -1;
		materialContext.sourceBits = candidate.sourceBits;
		materialContext.priority = candidate.priority;
		materialContext.gpuForce = candidate.gpuForce;
		materialContext.gpuPrefer = candidate.gpuPrefer;

		auto found = graph.requestByMeshVariant.find(meshVariantHash);
		if (found != graph.requestByMeshVariant.end())
		{
			LoadingVoxelPreloadRequest& request = graph.requests[found->second];
			request.sourceBits |= candidate.sourceBits;
			request.gpuForce = request.gpuForce || candidate.gpuForce;
			request.gpuPrefer = request.gpuPrefer || candidate.gpuPrefer;
			request.materialContexts.push_back(materialContext);
			if ((uint8_t)candidate.priority < (uint8_t)request.priority)
			{
				request.priority = candidate.priority;
				request.actor = actor;
				request.actorIndex = actor != nullptr ? (int32_t)actor->GetIndex() : -1;
				request.texid = candidate.texid;
			}
			graph.skippedDuplicate++;
			return true;
		}

		LoadingVoxelPreloadRequest request = {};
		request.texid = candidate.texid;
		request.actor = actor;
		request.actorIndex = actor != nullptr ? (int32_t)actor->GetIndex() : -1;
		request.model = model;
		request.resolvedVoxelIndex = voxelIndex;
		request.meshVariantHash = meshVariantHash;
		request.sourceBits = candidate.sourceBits;
		request.priority = candidate.priority;
		request.gpuForce = candidate.gpuForce;
		request.gpuPrefer = candidate.gpuPrefer;
		request.materialContexts.push_back(materialContext);
		graph.requestByMeshVariant.emplace(meshVariantHash, graph.requests.size());
		graph.requests.push_back(request);
		return true;
	}

	std::string LowerAscii(std::string value)
	{
		for (char& ch : value)
		{
			ch = (char)std::tolower((unsigned char)ch);
		}
		return value;
	}

	bool TryParseIntToken(const std::string& token, int& outValue)
	{
		if (token.empty())
		{
			return false;
		}
		char* end = nullptr;
		const long value = std::strtol(token.c_str(), &end, 10);
		if (end == token.c_str() || *end != 0)
		{
			return false;
		}
		outValue = (int)value;
		return true;
	}

	bool ParseLoadingVoxelPriorityValue(const std::string& value, LoadingVoxelRequestPriority& outPriority)
	{
		const std::string lower = LowerAscii(value);
		if (lower == "force")
		{
			outPriority = LoadingVoxelRequestPriority::Force;
			return true;
		}
		if (lower == "high")
		{
			outPriority = LoadingVoxelRequestPriority::High;
			return true;
		}
		if (lower == "normal")
		{
			outPriority = LoadingVoxelRequestPriority::Normal;
			return true;
		}
		if (lower == "opportunistic")
		{
			outPriority = LoadingVoxelRequestPriority::Opportunistic;
			return true;
		}
		return false;
	}

	struct MountedVoxelPreloadOptions
	{
		LoadingVoxelRequestPriority priority = LoadingVoxelRequestPriority::High;
		uint32_t sourceBits = LoadingVoxelRequestSource_MountedVoxelPreload | LoadingVoxelRequestSource_MountedVoxelPreloadGlobal;
		bool gpuForce = false;
		bool gpuPrefer = false;
	};

	bool IsMountedVoxelPreloadOptionName(const std::string& token)
	{
		const std::string lower = LowerAscii(token);
		const size_t equals = lower.find('=');
		const std::string name = equals == std::string::npos ? lower : lower.substr(0, equals);
		return name == "priority" || name == "gpu" || name == "reason";
	}

	bool TryReadMountedVoxelPreloadOptionValue(const std::vector<std::string>& tokens, size_t& index, std::string& outName, std::string& outValue)
	{
		std::string token = tokens[index];
		const size_t equals = token.find('=');
		if (equals != std::string::npos)
		{
			outName = LowerAscii(token.substr(0, equals));
			outValue = token.substr(equals + 1);
			return true;
		}

		outName = LowerAscii(token);
		if (index + 1 >= tokens.size())
		{
			return false;
		}
		if (tokens[index + 1] == "=")
		{
			if (index + 2 >= tokens.size())
			{
				return false;
			}
			outValue = tokens[index + 2];
			index += 2;
			return true;
		}
		outValue = tokens[index + 1];
		index += 1;
		return true;
	}

	void ParseMountedVoxelPreloadOptions(const std::vector<std::string>& tokens, MountedVoxelPreloadOptions& outOptions)
	{
		for (size_t i = 0; i < tokens.size(); ++i)
		{
			if (!IsMountedVoxelPreloadOptionName(tokens[i]))
			{
				continue;
			}

			std::string name;
			std::string value;
			if (!TryReadMountedVoxelPreloadOptionValue(tokens, i, name, value))
			{
				continue;
			}

			if (name == "priority")
			{
				LoadingVoxelRequestPriority priority = outOptions.priority;
				if (ParseLoadingVoxelPriorityValue(value, priority))
				{
					outOptions.priority = priority;
				}
			}
			else if (name == "gpu")
			{
				const std::string lower = LowerAscii(value);
				outOptions.gpuForce = lower == "force";
				outOptions.gpuPrefer = lower == "prefer";
				if (lower == "none" || lower == "false" || lower == "0")
				{
					outOptions.gpuForce = false;
					outOptions.gpuPrefer = false;
				}
			}
		}
	}

	std::vector<std::string> TokenizeMountedVoxelPreloadLine(std::string line)
	{
		const size_t hash = line.find('#');
		if (hash != std::string::npos)
		{
			line.resize(hash);
		}
		const size_t slashComment = line.find("//");
		if (slashComment != std::string::npos)
		{
			line.resize(slashComment);
		}

		for (char& ch : line)
		{
			if (ch == ',' || ch == '{' || ch == '}' || ch == ';')
			{
				ch = ' ';
			}
		}

		std::vector<std::string> tokens;
		std::istringstream stream(line);
		std::string token;
		while (stream >> token)
		{
			tokens.push_back(token);
		}
		return tokens;
	}

	void AddMountedVoxelTexidRequest(
		LoadingVoxelPreloadRequestGraph& graph,
		DCoreActor* actor,
		FTextureID texid,
		const MountedVoxelPreloadOptions& options)
	{
		LoadingVoxelTextureCandidate candidate = {};
		candidate.texid = texid;
		candidate.sourceBits = options.sourceBits != LoadingVoxelRequestSource_None ? options.sourceBits : LoadingVoxelRequestSource_MountedVoxelPreload;
		candidate.priority = options.priority;
		candidate.gpuForce = options.gpuForce;
		candidate.gpuPrefer = options.gpuPrefer;
		if (AddLoadingVoxelPreloadRequest(graph, actor, candidate))
		{
			graph.manifestRequests++;
		}
	}

	void AddMountedVoxelPicnumRequest(
		LoadingVoxelPreloadRequestGraph& graph,
		DCoreActor* actor,
		int picnum,
		const MountedVoxelPreloadOptions& options)
	{
		if ((unsigned)picnum >= MAXTILES)
		{
			graph.manifestSkippedSyntax++;
			return;
		}
		AddMountedVoxelTexidRequest(graph, actor, tileGetTextureID(picnum), options);
	}

	void AddMountedVoxelPicnumRangeRequests(
		LoadingVoxelPreloadRequestGraph& graph,
		DCoreActor* actor,
		int firstPicnum,
		int lastPicnum,
		const MountedVoxelPreloadOptions& options)
	{
		if (firstPicnum > lastPicnum)
		{
			std::swap(firstPicnum, lastPicnum);
		}
		firstPicnum = (std::max)(0, firstPicnum);
		lastPicnum = (std::min)(MAXTILES - 1, lastPicnum);
		for (int picnum = firstPicnum; picnum <= lastPicnum; ++picnum)
		{
			FTextureID texid = tileGetTextureID(picnum);
			if (ResolveVoxelTextureModel(texid) != nullptr)
			{
				AddMountedVoxelTexidRequest(graph, actor, texid, options);
			}
		}
	}

	bool TryParseMountedVoxelPicrangeToken(const std::string& token, int& outFirst, int& outLast)
	{
		const size_t dash = token.find('-');
		if (dash == std::string::npos)
		{
			return false;
		}
		int first = 0;
		int last = 0;
		if (!TryParseIntToken(token.substr(0, dash), first) ||
			!TryParseIntToken(token.substr(dash + 1), last))
		{
			return false;
		}
		outFirst = first;
		outLast = last;
		return true;
	}

	void AddMountedVoxelPicnumListRequests(
		LoadingVoxelPreloadRequestGraph& graph,
		DCoreActor* actor,
		const std::string& value,
		const MountedVoxelPreloadOptions& options)
	{
		std::string normalized = value;
		for (char& ch : normalized)
		{
			if (ch == ',')
			{
				ch = ' ';
			}
		}

		std::istringstream stream(normalized);
		std::string token;
		while (stream >> token)
		{
			int picnum = -1;
			if (TryParseIntToken(token, picnum))
			{
				AddMountedVoxelPicnumRequest(graph, actor, picnum, options);
			}
			else
			{
				graph.manifestSkippedSyntax++;
			}
		}
	}

	bool IsMountedVoxelPreloadSectionActive(const std::string& section, const std::string& argument)
	{
		if (section == "global")
		{
			return true;
		}
		if (section == "game")
		{
			struct GameFilter
			{
				const char* name;
				int flag;
			};
			static const GameFilter filters[] =
			{
				{ "duke", GAMEFLAG_DUKE },
				{ "nam", GAMEFLAG_NAM | GAMEFLAG_NAPALM },
				{ "namonly", GAMEFLAG_NAM },
				{ "napalm", GAMEFLAG_NAPALM },
				{ "ww2gi", GAMEFLAG_WW2GI },
				{ "redneck", GAMEFLAG_RR },
				{ "redneckrides", GAMEFLAG_RRRA },
				{ "blood", GAMEFLAG_BLOOD },
				{ "shadowwarrior", GAMEFLAG_SW },
				{ "sw", GAMEFLAG_SW },
				{ "exhumed", GAMEFLAG_POWERSLAVE | GAMEFLAG_EXHUMED },
				{ "plutopak", GAMEFLAG_PLUTOPAK },
				{ "worldtour", GAMEFLAG_WORLDTOUR },
				{ "shareware", GAMEFLAG_SHAREWARE },
			};
			const std::string lower = LowerAscii(argument);
			for (const GameFilter& filter : filters)
			{
				if (lower == filter.name)
				{
					return (g_gameType & filter.flag) != 0;
				}
			}
			return false;
		}
		if (section == "map")
		{
			return currentLevel != nullptr && !argument.empty() && currentLevel->labelName.CompareNoCase(argument.c_str()) == 0;
		}
		return true;
	}

	DCoreActor* FindMountedVoxelPreloadActorDefault(const std::string& actorName)
	{
		if (actorName.empty())
		{
			return nullptr;
		}
		PClassActor* cls = PClass::FindActor(FName(actorName.c_str()));
		return cls != nullptr ? GetDefaultByType(cls) : nullptr;
	}

	void ParseMountedVoxelPreloadDirective(
		LoadingVoxelPreloadRequestGraph& graph,
		const std::vector<std::string>& tokens,
		bool active,
		uint32_t sourceBits,
		const char* sourceName,
		int lineNumber)
	{
		if (tokens.empty())
		{
			return;
		}
		if (!active)
		{
			graph.manifestSkippedInactive++;
			return;
		}

		const std::string directive = LowerAscii(tokens[0]);
		MountedVoxelPreloadOptions options = {};
		options.sourceBits = sourceBits != LoadingVoxelRequestSource_None ? sourceBits : options.sourceBits;
		ParseMountedVoxelPreloadOptions(tokens, options);

		auto logTrace = [&](const char* action, const char* reason)
		{
			if ((int)nri_ptloadingtrace >= 2)
			{
				Printf("NRI PT loading voxel preload: source=%s line=%d directive=%s action=%s reason=%s priority=%s gpu=%s\n",
					sourceName != nullptr ? sourceName : "(unknown)",
					lineNumber,
					directive.c_str(),
					action,
					reason != nullptr ? reason : "none",
					LoadingVoxelPriorityName(options.priority),
					options.gpuForce ? "force" : (options.gpuPrefer ? "prefer" : "none"));
			}
		};

		if (directive == "pic")
		{
			if (tokens.size() < 2)
			{
				graph.manifestSkippedSyntax++;
				logTrace("skip", "missing-picnum");
				return;
			}
			int picnum = -1;
			if (!TryParseIntToken(tokens[1], picnum))
			{
				graph.manifestSkippedSyntax++;
				logTrace("skip", "bad-picnum");
				return;
			}
			AddMountedVoxelPicnumRequest(graph, nullptr, picnum, options);
			logTrace("request", "pic");
			return;
		}

		if (directive == "texture")
		{
			if (tokens.size() < 2)
			{
				graph.manifestSkippedSyntax++;
				logTrace("skip", "missing-texture");
				return;
			}
			int textureId = -1;
			if (!TryParseIntToken(tokens[1], textureId))
			{
				graph.manifestSkippedSyntax++;
				logTrace("skip", "bad-texture");
				return;
			}
			AddMountedVoxelTexidRequest(graph, nullptr, FSetTextureID(textureId), options);
			logTrace("request", "texture");
			return;
		}

		if (directive == "picrange")
		{
			if (tokens.size() < 2)
			{
				graph.manifestSkippedSyntax++;
				logTrace("skip", "missing-picrange");
				return;
			}
			int firstPicnum = -1;
			int lastPicnum = -1;
			if (!TryParseMountedVoxelPicrangeToken(tokens[1], firstPicnum, lastPicnum))
			{
				if (tokens.size() < 3 ||
					!TryParseIntToken(tokens[1], firstPicnum) ||
					!TryParseIntToken(tokens[2], lastPicnum))
				{
					graph.manifestSkippedSyntax++;
					logTrace("skip", "bad-picrange");
					return;
				}
			}
			AddMountedVoxelPicnumRangeRequests(graph, nullptr, firstPicnum, lastPicnum, options);
			logTrace("request", "picrange");
			return;
		}

		if (directive == "voxel")
		{
			graph.manifestSkippedUnsupported++;
			logTrace("skip", "voxel-reverse-mapping-unsupported");
			return;
		}

		if (directive == "actor")
		{
			if (tokens.size() < 3)
			{
				graph.manifestSkippedSyntax++;
				logTrace("skip", "missing-actor-args");
				return;
			}
			DCoreActor* actorDefault = FindMountedVoxelPreloadActorDefault(tokens[1]);
			if (actorDefault == nullptr)
			{
				graph.manifestSkippedActor++;
				logTrace("skip", "actor-not-found");
				return;
			}

			const std::string spec = LowerAscii(tokens[2]);
			if (spec == "allpicnums")
			{
				AddMountedVoxelTexidRequest(graph, actorDefault, actorDefault->spr.spritetexture(), options);
				AddMountedVoxelTexidRequest(graph, actorDefault, actorDefault->dispictex, options);
				const int range = (int)nri_ptloadingvoxelpicrange;
				if (range > 0)
				{
					AddMountedVoxelPicnumRangeRequests(graph, actorDefault, actorDefault->spr.picnum - range, actorDefault->spr.picnum + range, options);
				}
				logTrace("request", "actor-allpicnums-default-range");
				return;
			}

			if (spec == "picnums" || spec.rfind("picnums=", 0) == 0)
			{
				if (spec.rfind("picnums=", 0) == 0)
				{
					AddMountedVoxelPicnumListRequests(graph, actorDefault, tokens[2].substr(8), options);
				}
				for (size_t i = 3; i < tokens.size(); ++i)
				{
					if (IsMountedVoxelPreloadOptionName(tokens[i]))
					{
						break;
					}
					AddMountedVoxelPicnumListRequests(graph, actorDefault, tokens[i], options);
				}
				logTrace("request", "actor-picnums");
				return;
			}

			if (spec == "picrange" || spec.rfind("picrange=", 0) == 0)
			{
				int firstPicnum = -1;
				int lastPicnum = -1;
				if (spec.rfind("picrange=", 0) == 0)
				{
					if (!TryParseMountedVoxelPicrangeToken(tokens[2].substr(9), firstPicnum, lastPicnum))
					{
						graph.manifestSkippedSyntax++;
						logTrace("skip", "bad-actor-picrange");
						return;
					}
				}
				else if (tokens.size() < 5 ||
					!TryParseIntToken(tokens[3], firstPicnum) ||
					!TryParseIntToken(tokens[4], lastPicnum))
				{
					graph.manifestSkippedSyntax++;
					logTrace("skip", "bad-actor-picrange");
					return;
				}
				AddMountedVoxelPicnumRangeRequests(graph, actorDefault, firstPicnum, lastPicnum, options);
				logTrace("request", "actor-picrange");
				return;
			}

			graph.manifestSkippedSyntax++;
			logTrace("skip", "unknown-actor-spec");
			return;
		}

		graph.manifestSkippedUnsupported++;
		logTrace("skip", "unknown-directive");
	}

	void ParseMountedVoxelPreloadLump(LoadingVoxelPreloadRequestGraph& graph, int lumpNum)
	{
		FileReader reader = fileSystem.OpenFileReader(lumpNum);
		if (!reader.isOpen())
		{
			graph.manifestSkippedSyntax++;
			return;
		}

		std::string text;
		text.resize((size_t)reader.GetLength());
		if (!text.empty())
		{
			reader.Read(text.data(), text.size());
		}

		const char* sourceName = fileSystem.GetFileFullName(lumpNum);
		bool active = true;
		uint32_t activeSourceBits = LoadingVoxelRequestSource_MountedVoxelPreload | LoadingVoxelRequestSource_MountedVoxelPreloadGlobal;
		int lineNumber = 0;
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line))
		{
			++lineNumber;
			std::vector<std::string> tokens = TokenizeMountedVoxelPreloadLine(line);
			if (tokens.empty())
			{
				continue;
			}

			graph.manifestLines++;
			const std::string first = LowerAscii(tokens[0]);
			if (first == "voxelpreload")
			{
				active = true;
				activeSourceBits = LoadingVoxelRequestSource_MountedVoxelPreload | LoadingVoxelRequestSource_MountedVoxelPreloadGlobal;
				continue;
			}
			if (first == "global" || first == "game" || first == "map")
			{
				const std::string argument = tokens.size() >= 2 ? tokens[1] : "";
				active = IsMountedVoxelPreloadSectionActive(first, argument);
				activeSourceBits = LoadingVoxelRequestSource_MountedVoxelPreload;
				if (first == "map")
				{
					activeSourceBits |= LoadingVoxelRequestSource_MountedVoxelPreloadMap;
				}
				else if (first == "game")
				{
					activeSourceBits |= LoadingVoxelRequestSource_MountedVoxelPreloadGame;
				}
				else
				{
					activeSourceBits |= LoadingVoxelRequestSource_MountedVoxelPreloadGlobal;
				}
				continue;
			}

			ParseMountedVoxelPreloadDirective(graph, tokens, active, activeSourceBits, sourceName, lineNumber);
		}
	}

	void AddMountedVoxelPreloadRequests(LoadingVoxelPreloadRequestGraph& graph)
	{
		if (!r_voxels || !nri_ptloadingvoxellist)
		{
			return;
		}

		static const char* voxelPreloadNames[] = { "VOXELPRELOAD", nullptr };
		int lastLump = 0;
		int lumpNum = -1;
		while ((lumpNum = fileSystem.FindLumpMulti(voxelPreloadNames, &lastLump)) != -1)
		{
			graph.manifestSources++;
			ParseMountedVoxelPreloadLump(graph, lumpNum);
		}

		if ((int)nri_ptloadingtrace >= 1 && graph.manifestSources != 0)
		{
			Printf("NRI PT loading voxel preload list: sources=%u lines=%u requests=%u skipped_inactive=%u skipped_syntax=%u skipped_actor=%u skipped_unsupported=%u\n",
				graph.manifestSources,
				graph.manifestLines,
				graph.manifestRequests,
				graph.manifestSkippedInactive,
				graph.manifestSkippedSyntax,
				graph.manifestSkippedActor,
				graph.manifestSkippedUnsupported);
		}
	}

	void BuildLiveActorVoxelPreloadRequestGraph(LoadingVoxelPreloadRequestGraph& graph)
	{
		graph = {};
		if (!r_voxels || (int)nri_ptloadingvoxelactors <= 0)
		{
			return;
		}

		std::vector<LoadingVoxelTextureCandidate> candidateTexids;
		AddMountedVoxelPreloadRequests(graph);

		TSpriteIterator<DCoreActor> it;
		while (DCoreActor* actor = it.Next())
		{
			if (!IsLiveActorVoxelWarmupCandidate(actor))
			{
				continue;
			}

			BuildLoadingActorTextureCandidates(actor, candidateTexids);
			bool countedActor = false;
			for (const LoadingVoxelTextureCandidate& candidate : candidateTexids)
			{
				if (AddLoadingVoxelPreloadRequest(graph, actor, candidate) && !countedActor)
				{
					graph.actorCandidates++;
					countedActor = true;
				}
			}
		}
		SortLoadingVoxelPreloadRequests(graph);
	}

	float GetLoadingActorAlpha(DCoreActor* actor)
	{
		if (actor == nullptr)
		{
			return 1.0f;
		}

		float alpha = 1.0f;
		if ((actor->spr.cstat & CSTAT_SPRITE_TRANSLUCENT) != 0)
		{
			alpha = GetAlphaFromBlend((actor->spr.cstat & CSTAT_SPRITE_TRANS_FLIP) ? DAMETH_TRANS2 : DAMETH_TRANS1, 0);
		}
		alpha *= 1.f - actor->sprext.alpha;
		return alpha;
	}

	void BuildActorPresentationIdentityMap(
		std::unordered_map<uint64_t, const ActorPresentationState*>& outActors)
	{
		outActors.clear();
		const ActorPresentationSnapshot& snapshot = GetActorPresentationSnapshot();
		if (!r_voxels || !snapshot.complete)
		{
			return;
		}

		for (const ActorPresentationState& actor : snapshot.actors)
		{
			// The captured HWSprite/cache entry is the representation evidence.
			// Duke action actors may keep a non-voxel base texture in the
			// snapshot while their temporary render sprite selects voxel frames.
			if (!IsActorPresentationVoxelCacheOwner(actor))
			{
				continue;
			}

			const uint64_t identityKey = BuildVoxelActorIdentityKey(actor.owner);
			if (identityKey != 0)
			{
				outActors[identityKey] = &actor;
			}
		}
	}

	void CopyActorPresentationCacheBasisPosition(
		const ActorPresentationState& actor,
		float outPosition[3])
	{
		outPosition[0] = (float)actor.position.x;
		outPosition[1] = (float)-actor.position.y;
		outPosition[2] = (float)-actor.position.z;
	}

	bool SyncRetainedVoxelActorAuthority(
		VoxelActorCacheEntry& entry,
		const ActorPresentationState& actor)
	{
		bool changed = false;
		float actorScenePosition[3] = {};
		CopyActorPresentationCacheBasisPosition(actor, actorScenePosition);
		if (!entry.hasLastActorScenePosition)
		{
			entry.lastActorScenePosition[0] = actorScenePosition[0];
			entry.lastActorScenePosition[1] = actorScenePosition[1];
			entry.lastActorScenePosition[2] = actorScenePosition[2];
			entry.hasLastActorScenePosition = true;
			changed = true;
		}
		else
		{
			const float delta[3] =
			{
				actorScenePosition[0] - entry.lastActorScenePosition[0],
				actorScenePosition[1] - entry.lastActorScenePosition[1],
				actorScenePosition[2] - entry.lastActorScenePosition[2],
			};
			constexpr float Epsilon = 0.0001f;
			if (std::abs(delta[0]) > Epsilon || std::abs(delta[1]) > Epsilon || std::abs(delta[2]) > Epsilon)
			{
				entry.currentTranslation[0] += delta[0];
				entry.currentTranslation[1] += delta[1];
				entry.currentTranslation[2] += delta[2];
				entry.currentTransform[3] += delta[0];
				entry.currentTransform[7] += delta[1];
				entry.currentTransform[11] += delta[2];
				entry.lastActorScenePosition[0] = actorScenePosition[0];
				entry.lastActorScenePosition[1] = actorScenePosition[1];
				entry.lastActorScenePosition[2] = actorScenePosition[2];
				changed = true;
			}
		}

		const uint64_t basisStateHash = BuildActorPresentationBasisStateHash(actor);
		const bool basisCurrent = basisStateHash == entry.presentationBasisStateHash;
		const bool authorityCurrent = actor.authorityCurrent;
		const bool publicationEligible =
			IsActorPresentationVoxelCacheOwner(actor) && basisCurrent;
		changed = changed ||
			entry.placementGeneration != actor.placementGeneration ||
			entry.placementStateHash != actor.placementStateHash ||
			entry.physicalSectorIndex != actor.physicalSectorIndex ||
			entry.authorityCurrent != authorityCurrent ||
			entry.publicationEligible != publicationEligible ||
			entry.pendingRemoval;
		entry.placementGeneration = actor.placementGeneration;
		entry.placementStateHash = actor.placementStateHash;
		entry.physicalSectorIndex = actor.physicalSectorIndex;
		entry.authorityCurrent = authorityCurrent;
		entry.publicationEligible = publicationEligible;
		entry.pendingRemoval = false;
		return changed;
	}

	bool MarkVoxelActorAuthorityMissing(VoxelActorCacheEntry& entry)
	{
		const bool changed = entry.authorityCurrent || entry.publicationEligible || !entry.pendingRemoval;
		entry.authorityCurrent = false;
		entry.publicationEligible = false;
		entry.pendingRemoval = true;
		return changed;
	}

	bool BeginVoxelActorCacheFrame()
	{
		const bool rootCapture = gVoxelActorCacheCaptureDepth++ == 0;
		if (rootCapture)
		{
			++gVoxelActorCacheFrame;
			if (gVoxelActorCacheFrame == 0)
			{
				gVoxelActorCacheFrame = 1;
			}
		}
		return rootCapture;
	}

	uint64_t EstimateSurfaceVertexBytes(const SurfaceRef& surface)
	{
		return (uint64_t)surface.vertices.size() * (uint64_t)sizeof(CapturedVertex);
	}

	uint64_t EstimateSurfaceIndexBytes(const SurfaceRef& surface)
	{
		return (uint64_t)surface.indices.size() * (uint64_t)sizeof(uint32_t);
	}

	uint64_t EstimateSurfacePrimitiveBytes(uint32_t primitiveCount)
	{
		return (uint64_t)primitiveCount * (uint64_t)sizeof(PrimitiveData);
	}

	uint64_t EstimateSurfaceMaterialBytes(const SurfaceRef& surface)
	{
		return surface.vertices.empty() ? 0ull : (uint64_t)sizeof(MaterialRef);
	}

	void InsertDynamicVoxelEscapeTopEntry(
		std::array<DynamicVoxelEscapeTraceEntry, DynamicVoxelEscapeTraceCount>& entries,
		unsigned int& count,
		const DynamicVoxelEscapeTraceEntry& entry)
	{
		if (!entry.valid)
		{
			return;
		}

		size_t insertIndex = DynamicVoxelEscapeTraceCount;
		for (size_t i = 0; i < DynamicVoxelEscapeTraceCount; ++i)
		{
			const DynamicVoxelEscapeTraceEntry& current = entries[i];
			if (!current.valid ||
				entry.totalBytes > current.totalBytes ||
				(entry.totalBytes == current.totalBytes && entry.primitiveCount > current.primitiveCount))
			{
				insertIndex = i;
				break;
			}
		}
		if (insertIndex >= DynamicVoxelEscapeTraceCount)
		{
			return;
		}

		for (size_t i = DynamicVoxelEscapeTraceCount - 1; i > insertIndex; --i)
		{
			entries[i] = entries[i - 1];
		}
		entries[insertIndex] = entry;
		count = (unsigned int)(std::min<size_t>)(DynamicVoxelEscapeTraceCount, count + 1u);
	}

	void RecordDynamicVoxelEscape(
		SceneDebugStats& stats,
		const HWSprite& sprite,
		const VoxelActorCacheLookup& lookup,
		const SurfaceRef& surface,
		DynamicVoxelEscapeReason reason)
	{
		const uint32_t primitiveCount = CountSurfacePrimitives(surface);
		const uint64_t vertexBytes = EstimateSurfaceVertexBytes(surface);
		const uint64_t indexBytes = EstimateSurfaceIndexBytes(surface);
		const uint64_t primitiveBytes = EstimateSurfacePrimitiveBytes(primitiveCount);
		const uint64_t materialBytes = EstimateSurfaceMaterialBytes(surface);
		const uint64_t totalBytes = vertexBytes + indexBytes + primitiveBytes + materialBytes;

		stats.dynamicVoxelEscapeActorCount++;
		stats.dynamicVoxelEscapePrimitiveCount += primitiveCount;
		stats.dynamicVoxelEscapeVertexBytes += vertexBytes;
		stats.dynamicVoxelEscapeIndexBytes += indexBytes;
		stats.dynamicVoxelEscapePrimitiveBytes += primitiveBytes;
		stats.dynamicVoxelEscapeMaterialBytes += materialBytes;
		stats.dynamicVoxelEscapeTotalBytes += totalBytes;
		if (IsDynamicVoxelEscapeEligibleForPersistent(reason))
		{
			stats.dynamicVoxelEscapeEligibleActorCount++;
		}
		if (IsDynamicVoxelEscapeForcedDynamic(reason))
		{
			stats.dynamicVoxelEscapeForcedActorCount++;
		}
		if (IsExpectedDynamicVoxelEscape(reason))
		{
			stats.dynamicVoxelExpectedEscapeActorCount++;
			stats.dynamicVoxelExpectedEscapePrimitiveCount += primitiveCount;
			stats.dynamicVoxelExpectedEscapeTotalBytes += totalBytes;
		}
		else
		{
			stats.dynamicVoxelUnexpectedEscapeActorCount++;
			stats.dynamicVoxelUnexpectedEscapePrimitiveCount += primitiveCount;
			stats.dynamicVoxelUnexpectedEscapeTotalBytes += totalBytes;
		}

		DynamicVoxelEscapeTraceEntry entry = {};
		entry.valid = true;
		entry.reason = reason;
		entry.actorIndex =
			lookup.actorIndex >= 0 ? lookup.actorIndex :
			sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr ? sprite.Sprite->ownerActor->GetIndex() :
			-1;
		entry.statnum = sprite.Sprite != nullptr ? sprite.Sprite->statnum : -1;
		entry.sourcePicnum = sprite.Sprite != nullptr ? sprite.Sprite->picnum : -1;
		entry.resolvedVoxelIndex = lookup.resolvedVoxelIndex;
		entry.meshVariantHash = lookup.meshVariantHash;
		entry.materialVariantHash = lookup.materialVariantHash;
		entry.primitiveCount = primitiveCount;
		entry.vertexBytes = vertexBytes;
		entry.indexBytes = indexBytes;
		entry.primitiveBytes = primitiveBytes;
		entry.materialBytes = materialBytes;
		entry.totalBytes = totalBytes;
		entry.persistentReady = lookup.entry != nullptr && lookup.entry->persistentReady;
		entry.hasCachedSurface = lookup.entry != nullptr && lookup.entry->hasSurface;
		InsertDynamicVoxelEscapeTopEntry(stats.dynamicVoxelEscapeTopEntries, stats.dynamicVoxelEscapeTopCount, entry);
		if (!IsExpectedDynamicVoxelEscape(reason))
		{
			InsertDynamicVoxelEscapeTopEntry(stats.dynamicVoxelUnexpectedEscapeTopEntries, stats.dynamicVoxelUnexpectedEscapeTopCount, entry);
		}
	}

	struct VoxelDuplicateVariantAggregate
	{
		uint64_t meshKeyHash = 0;
		uint64_t exampleBasisSignature = 0;
		int32_t sourcePicnum = -1;
		uint32_t actorCount = 0;
		uint32_t persistentActorCount = 0;
		uint32_t transformKeyedActorCount = 0;
		uint32_t localSpaceActorCount = 0;
		uint32_t bakedTransformActorCount = 0;
		uint32_t unknownSpaceActorCount = 0;
		uint32_t primitiveCountPerActor = 0;
		uint32_t totalDuplicatedPrimitives = 0;
		uint64_t duplicatedBytes = 0;
		std::unordered_set<uint64_t> basisSignatures;
	};

	bool ShouldCollectVoxelActorCacheDuplicationStats()
	{
		const GameUpdateSnapshot gameUpdate = GetGameUpdateSnapshot();
		NRIVoxelActorDuplicationAuditInput input = {};
		input.presentationGeneration = gameUpdate.presentationGeneration;
		input.slowdownInterval = (uint32_t)(std::max)(1, (int)nri_ptslowdowntraceinterval);
		input.slowdownTop = (int)nri_ptslowdowntop;
		input.voxelStatsEnabled = (bool)nri_voxelstats;
		input.slowdownTraceEnabled = (bool)nri_ptslowdowntrace;
		return ShouldCollectNRIVoxelActorDuplicationAudit(input);
	}

	void CollectVoxelActorCacheDuplicationStats(SceneDebugStats& stats)
	{
		const auto start = std::chrono::steady_clock::now();
		gDynamicCapturePerfStats.voxelDuplicationAuditCalls++;
		gDynamicCapturePerfStats.voxelDuplicationAuditEntriesScanned += (uint32_t)gVoxelActorCache.size();
		stats.voxelCachePrimitives = 0;
		stats.voxelCacheActorSurfaces = 0;
		stats.voxelCacheUniqueMeshKeys = 0;
		stats.voxelCacheUniqueMaterialKeys = 0;
		stats.voxelCacheLocalSpaceSurfaces = 0;
		stats.voxelCacheBakedTransformSurfaces = 0;
		stats.voxelCacheUnknownSpaceSurfaces = 0;
		stats.voxelCacheTransformKeyedSurfaces = 0;
		stats.voxelCacheUniqueTransformBases = 0;
		stats.voxelCacheInvariantWarnings = 0;
		stats.voxelCacheDuplicatedVertexBytes = 0;
		stats.voxelCacheDuplicatedIndexBytes = 0;
		stats.voxelCacheDuplicatedPrimitiveBytes = 0;
		stats.voxelCacheDuplicatedTotalBytes = 0;
		stats.voxelCacheDuplicateTopCount = 0;
		stats.voxelCacheDuplicateTopEntries = {};

		std::unordered_set<uint64_t> meshKeys;
		std::unordered_set<uint64_t> materialKeys;
		std::unordered_set<uint64_t> basisSignatures;
		std::unordered_map<uint64_t, VoxelDuplicateVariantAggregate> meshAggregates;
		meshKeys.reserve(gVoxelActorCache.size());
		materialKeys.reserve(gVoxelActorCache.size());
		basisSignatures.reserve(gVoxelActorCache.size());
		meshAggregates.reserve(gVoxelActorCache.size());

		for (const auto& pair : gVoxelActorCache)
		{
			const VoxelActorCacheEntry& entry = pair.second;
			if (!entry.hasSurface)
			{
				continue;
			}

			const uint64_t vertexBytes = EstimateSurfaceVertexBytes(entry.surface);
			const uint64_t indexBytes = EstimateSurfaceIndexBytes(entry.surface);
			const uint64_t primitiveBytes = EstimateSurfacePrimitiveBytes(entry.primitiveCount);
			const uint64_t totalBytes = vertexBytes + indexBytes + primitiveBytes;

			stats.voxelCacheActorSurfaces++;
			stats.voxelCachePrimitives += entry.primitiveCount;
			stats.voxelCacheDuplicatedVertexBytes += vertexBytes;
			stats.voxelCacheDuplicatedIndexBytes += indexBytes;
			stats.voxelCacheDuplicatedPrimitiveBytes += primitiveBytes;
			stats.voxelCacheDuplicatedTotalBytes += totalBytes;
			basisSignatures.insert(entry.transformBasisSignature);

			switch (entry.meshBakeSpace)
			{
			case VoxelMeshBakeSpace::LocalSpace:
				stats.voxelCacheLocalSpaceSurfaces++;
				break;
			case VoxelMeshBakeSpace::BakedTransform:
				stats.voxelCacheBakedTransformSurfaces++;
				break;
			default:
				stats.voxelCacheUnknownSpaceSurfaces++;
				break;
			}
			if (IsVoxelMeshTransformKeyed(entry.meshBakeSpace))
			{
				stats.voxelCacheTransformKeyedSurfaces++;
			}

			const uint64_t meshVariantHash = entry.meshVariantHash != 0 ? entry.meshVariantHash : entry.meshKeyHash;
			if (meshVariantHash != 0)
			{
				meshKeys.insert(meshVariantHash);
				VoxelDuplicateVariantAggregate& aggregate = meshAggregates[meshVariantHash];
				if (aggregate.actorCount == 0)
				{
					aggregate.meshKeyHash = meshVariantHash;
					aggregate.exampleBasisSignature = entry.transformBasisSignature;
					aggregate.sourcePicnum = entry.sourcePicnum;
					aggregate.primitiveCountPerActor = entry.primitiveCount;
				}
				else if (aggregate.sourcePicnum != entry.sourcePicnum)
				{
					aggregate.sourcePicnum = -1;
				}
				aggregate.actorCount++;
				aggregate.persistentActorCount += entry.persistentReady ? 1u : 0u;
				aggregate.basisSignatures.insert(entry.transformBasisSignature);
				if (IsVoxelMeshTransformKeyed(entry.meshBakeSpace))
				{
					aggregate.transformKeyedActorCount++;
				}
				switch (entry.meshBakeSpace)
				{
				case VoxelMeshBakeSpace::LocalSpace:
					aggregate.localSpaceActorCount++;
					break;
				case VoxelMeshBakeSpace::BakedTransform:
					aggregate.bakedTransformActorCount++;
					break;
				default:
					aggregate.unknownSpaceActorCount++;
					break;
				}
				aggregate.primitiveCountPerActor = (std::max)(aggregate.primitiveCountPerActor, entry.primitiveCount);
				aggregate.totalDuplicatedPrimitives += entry.primitiveCount;
				aggregate.duplicatedBytes += totalBytes;
			}
			const uint64_t materialVariantHash = entry.materialVariantHash != 0 ? entry.materialVariantHash : entry.materialKeyHash;
			if (materialVariantHash != 0)
			{
				materialKeys.insert(materialVariantHash);
			}
		}

		stats.voxelCacheUniqueMeshKeys = (unsigned int)meshKeys.size();
		stats.voxelCacheUniqueMaterialKeys = (unsigned int)materialKeys.size();
		stats.voxelCacheUniqueTransformBases = (unsigned int)basisSignatures.size();

		std::vector<VoxelDuplicateVariantAggregate> aggregates;
		gDynamicCapturePerfStats.voxelDuplicationAuditTemporaryContainersBuilt +=
			5u + (uint32_t)meshAggregates.size();
		aggregates.reserve(meshAggregates.size());
		for (const auto& pair : meshAggregates)
		{
			if (pair.second.bakedTransformActorCount > 0 && pair.second.basisSignatures.size() > 1)
			{
				stats.voxelCacheInvariantWarnings++;
				if (nri_voxelstats)
				{
					Printf(
						"PERF pt voxel invariant warning NRI: frame=%llu mesh_key=0x%llx source_pic=%d actors=%u space=baked basis_count=%u example_basis=0x%llx\n",
						(unsigned long long)gVoxelActorCacheFrame,
						(unsigned long long)pair.second.meshKeyHash,
						pair.second.sourcePicnum,
						pair.second.actorCount,
						(uint32_t)pair.second.basisSignatures.size(),
						(unsigned long long)pair.second.exampleBasisSignature);
				}
			}
			if (pair.second.actorCount > 1)
			{
				aggregates.push_back(pair.second);
			}
		}
		std::sort(aggregates.begin(), aggregates.end(), [](const auto& a, const auto& b)
		{
			if (a.totalDuplicatedPrimitives != b.totalDuplicatedPrimitives)
			{
				return a.totalDuplicatedPrimitives > b.totalDuplicatedPrimitives;
			}
			if (a.actorCount != b.actorCount)
			{
				return a.actorCount > b.actorCount;
			}
			return a.meshKeyHash < b.meshKeyHash;
		});

		stats.voxelCacheDuplicateTopCount =
			(unsigned int)(std::min)(aggregates.size(), (size_t)VoxelDuplicateVariantTraceCount);
		for (uint32_t i = 0; i < stats.voxelCacheDuplicateTopCount; ++i)
		{
			VoxelDuplicateVariantTraceEntry& top = stats.voxelCacheDuplicateTopEntries[i];
			const VoxelDuplicateVariantAggregate& aggregate = aggregates[i];
			top.valid = true;
			top.meshKeyHash = aggregate.meshKeyHash;
			top.exampleBasisSignature = aggregate.exampleBasisSignature;
			top.sourcePicnum = aggregate.sourcePicnum;
			top.actorCount = aggregate.actorCount;
			top.persistentActorCount = aggregate.persistentActorCount;
			top.uniqueBasisSignatureCount = (uint32_t)aggregate.basisSignatures.size();
			top.transformKeyedActorCount = aggregate.transformKeyedActorCount;
			if (aggregate.localSpaceActorCount > 0)
			{
				top.bakeSpace = VoxelMeshBakeSpace::LocalSpace;
			}
			else if (aggregate.bakedTransformActorCount > 0)
			{
				top.bakeSpace = VoxelMeshBakeSpace::BakedTransform;
			}
			else
			{
				top.bakeSpace = VoxelMeshBakeSpace::Unknown;
			}
			top.primitiveCountPerActor = aggregate.primitiveCountPerActor;
			top.totalDuplicatedPrimitives = aggregate.totalDuplicatedPrimitives;
			top.duplicatedBytes = aggregate.duplicatedBytes;
		}
		gDynamicCapturePerfStats.voxelDuplicationAuditMs +=
			DurationMs(start, std::chrono::steady_clock::now());
	}

	void PruneVoxelActorCacheLegacy(SceneDebugStats& stats)
	{
		const auto liveStart = std::chrono::steady_clock::now();
		std::unordered_map<uint64_t, const ActorPresentationState*> liveActors;
		BuildActorPresentationIdentityMap(liveActors);
		gDynamicCapturePerfStats.voxelMaintenanceLiveActorsEnumerated += (uint32_t)liveActors.size();
		gDynamicCapturePerfStats.voxelMaintenanceLiveEnumerationMs +=
			DurationMs(liveStart, std::chrono::steady_clock::now());

		const auto reconcileStart = std::chrono::steady_clock::now();
		for (auto it = gVoxelActorCache.begin(); it != gVoxelActorCache.end(); )
		{
			gDynamicCapturePerfStats.voxelMaintenanceCacheEntriesScanned++;
			auto liveActor = liveActors.find(it->first);
			if (liveActor == liveActors.end())
			{
				if (MarkVoxelActorAuthorityMissing(it->second))
				{
					++gVoxelActorCacheSerial;
				}
				if (it->second.lastSeenFrame == gVoxelActorCacheFrame)
				{
					EmitVoxelActorStateTrace(nullptr, nullptr, &it->second, "retained-actor-not-live-current-frame", VoxelActorPendingReason::ActorNotLive);
					++it;
					continue;
				}
				EmitVoxelActorStateTrace(nullptr, nullptr, &it->second, "remove", VoxelActorPendingReason::ActorNotLive);
				it = gVoxelActorCache.erase(it);
				stats.voxelCacheSurfaceRemoves++;
				gDynamicCapturePerfStats.voxelMaintenanceRemovals++;
				++gVoxelActorCacheSerial;
				continue;
			}
			if (it->second.lastSeenFrame != gVoxelActorCacheFrame)
			{
				if (SyncRetainedVoxelActorAuthority(it->second, *liveActor->second))
				{
					++gVoxelActorCacheSerial;
					gDynamicCapturePerfStats.voxelMaintenanceTransformSyncs++;
					EmitVoxelActorStateTrace(nullptr, nullptr, &it->second, "retained-transform-sync", VoxelActorPendingReason::None);
				}
				stats.voxelCacheNotCaptured++;
				EmitVoxelActorStateTrace(nullptr, nullptr, &it->second, "retained-not-captured", VoxelActorPendingReason::None);
			}
			else if (SyncRetainedVoxelActorAuthority(it->second, *liveActor->second))
			{
				++gVoxelActorCacheSerial;
			}
			++it;
		}
		gDynamicCapturePerfStats.voxelMaintenanceReconcileMs +=
			DurationMs(reconcileStart, std::chrono::steady_clock::now());
		gVoxelActorPendingRemoval = false;
	}

	void ReconcileVoxelActorCacheEntries(SceneDebugStats& stats)
	{
		const auto start = std::chrono::steady_clock::now();
		gVoxelActorPendingRemoval = false;
		for (auto it = gVoxelActorCache.begin(); it != gVoxelActorCache.end(); )
		{
			gDynamicCapturePerfStats.voxelMaintenanceCacheEntriesScanned++;
			VoxelActorCacheEntry& entry = it->second;
			// Removal events are authoritative and may outlive the actor object.
			const NRIVoxelActorPendingRemovalAction pendingRemovalAction =
				ResolveNRIVoxelActorPendingRemoval(
					entry.pendingRemoval,
					entry.lastSeenFrame,
					gVoxelActorCacheFrame);
			if (pendingRemovalAction == NRIVoxelActorPendingRemovalAction::RetainCurrentFrame)
			{
				gVoxelActorPendingRemoval = true;
				++it;
				continue;
			}
			if (pendingRemovalAction == NRIVoxelActorPendingRemovalAction::Erase)
			{
				EmitVoxelActorStateTrace(nullptr, nullptr, &entry, "remove-lifecycle", VoxelActorPendingReason::ActorNotLive);
				it = gVoxelActorCache.erase(it);
				stats.voxelCacheSurfaceRemoves++;
				gDynamicCapturePerfStats.voxelMaintenanceRemovals++;
				++gVoxelActorCacheSerial;
				continue;
			}
			const ActorPresentationOwnerKey owner =
			{
				entry.ownerWorldEpoch,
				(int64_t)entry.ownerLifetimeGeneration
			};
			const ActorPresentationState* actor = LookupActorPresentationByOwnerKey(owner);
			const bool live = actor != nullptr &&
				actor->actorIndex == entry.actorIndex &&
				BuildVoxelActorIdentityKey(actor->owner) == it->first &&
				IsActorPresentationVoxelCacheOwner(*actor);
			if (!live)
			{
				if (MarkVoxelActorAuthorityMissing(entry))
				{
					++gVoxelActorCacheSerial;
				}
				if (entry.lastSeenFrame == gVoxelActorCacheFrame)
				{
					entry.pendingRemoval = true;
					gVoxelActorPendingRemoval = true;
					EmitVoxelActorStateTrace(nullptr, nullptr, &entry, "retained-actor-not-live-current-frame", VoxelActorPendingReason::ActorNotLive);
					++it;
					continue;
				}
				EmitVoxelActorStateTrace(nullptr, nullptr, &entry, "remove-lifecycle", VoxelActorPendingReason::ActorNotLive);
				it = gVoxelActorCache.erase(it);
				stats.voxelCacheSurfaceRemoves++;
				gDynamicCapturePerfStats.voxelMaintenanceRemovals++;
				++gVoxelActorCacheSerial;
				continue;
			}

			if (entry.lastSeenFrame != gVoxelActorCacheFrame)
			{
				if (SyncRetainedVoxelActorAuthority(entry, *actor))
				{
					++gVoxelActorCacheSerial;
					gDynamicCapturePerfStats.voxelMaintenanceTransformSyncs++;
					EmitVoxelActorStateTrace(nullptr, nullptr, &entry, "retained-transform-sync", VoxelActorPendingReason::None);
				}
				stats.voxelCacheNotCaptured++;
			}
			else if (SyncRetainedVoxelActorAuthority(entry, *actor))
			{
				++gVoxelActorCacheSerial;
			}
			++it;
		}
		gDynamicCapturePerfStats.voxelMaintenanceReconcileMs +=
			DurationMs(start, std::chrono::steady_clock::now());
	}

	struct VoxelActorLifecycleApplyResult
	{
		bool forceReconcile = false;
		bool forceLegacyReconcile = false;
	};

	VoxelActorLifecycleApplyResult ApplyVoxelActorLifecycleEvents()
	{
		const auto start = std::chrono::steady_clock::now();
		const ActorLifecycleReadResult read =
			GetActorLifecycleJournal().ReadAfter(gVoxelActorLifecycleCursor, gVoxelActorLifecycleEvents);
		const bool resetSeen = std::any_of(
			gVoxelActorLifecycleEvents.begin(),
			gVoxelActorLifecycleEvents.end(),
			[](const ActorLifecycleEvent& event)
			{
				return event.type == ActorLifecycleEventType::Reset;
			});
		NRIVoxelActorLifecycleJournalInput journalInput = {};
		journalInput.lifecycleModeEnabled = true;
		journalInput.overflowed = read.overflowed;
		journalInput.resetSeen = resetSeen;
		const NRIVoxelActorLifecycleJournalDecision journalDecision =
			ResolveNRIVoxelActorLifecycleJournal(journalInput);
		if (journalDecision.advanceCursor)
		{
			gVoxelActorLifecycleCursor = read.latestSerial;
		}
		gDynamicCapturePerfStats.voxelLifecycleOverflows += read.overflowed ? 1u : 0u;
		VoxelActorLifecycleApplyResult result = {};
		result.forceLegacyReconcile = journalDecision.forceLegacyReconcile;
		if (!journalDecision.applyEvents)
		{
			// The retained tail is not a coherent history. In particular, applying
			// a tail Reset before reconciling current state can discard valid
			// post-reset entries. Skip it and fail closed to the exact live map.
			gDynamicCapturePerfStats.voxelLifecycleEventsDiscarded +=
				(uint32_t)gVoxelActorLifecycleEvents.size();
			gVoxelActorLifecycleEvents.clear();
			result.forceReconcile = true;
			gDynamicCapturePerfStats.voxelLifecycleMs +=
				DurationMs(start, std::chrono::steady_clock::now());
			return result;
		}

		gDynamicCapturePerfStats.voxelLifecycleEventsApplied += (uint32_t)gVoxelActorLifecycleEvents.size();

		for (const ActorLifecycleEvent& event : gVoxelActorLifecycleEvents)
		{
			switch (event.type)
			{
			case ActorLifecycleEventType::Inserted:
				gDynamicCapturePerfStats.voxelLifecycleInsertEvents++;
				break;
			case ActorLifecycleEventType::Removed:
			{
				gDynamicCapturePerfStats.voxelLifecycleRemoveEvents++;
				const uint64_t key = BuildVoxelActorIdentityKey(
					{ event.worldEpoch, event.actorIndex });
				auto found = gVoxelActorCache.find(key);
				if (found == gVoxelActorCache.end())
				{
					// Compatibility with cache rows authored by the former pointer-keyed
					// identity during migration. New rows never persist actor addresses.
					const uint64_t legacyKey = BuildLegacyVoxelInstanceKeyHash(
						event.actorIndex,
						event.actorAddress);
					found = gVoxelActorCache.find(legacyKey);
				}
				if (found != gVoxelActorCache.end())
				{
					MarkVoxelActorAuthorityMissing(found->second);
					gVoxelActorPendingRemoval = true;
					gDynamicCapturePerfStats.voxelLifecycleRemovalEntriesMarked++;
					++gVoxelActorCacheSerial;
				}
				result.forceReconcile = true;
				break;
			}
			case ActorLifecycleEventType::StatChanged:
				gDynamicCapturePerfStats.voxelLifecycleStatEvents++;
				result.forceReconcile = true;
				break;
			case ActorLifecycleEventType::Reset:
				gDynamicCapturePerfStats.voxelLifecycleResetEvents++;
				// Reset may be consumed after current-level actors have already been
				// captured (notably a same-map save reload). Reconcile against the
				// live map instead of clearing the mixed pre/post-reset cache here.
				gVoxelActorMaintenanceGate.Reset();
				result.forceReconcile = true;
				break;
			}
		}
		gDynamicCapturePerfStats.voxelLifecycleMs +=
			DurationMs(start, std::chrono::steady_clock::now());
		return result;
	}

	void PruneVoxelActorCache(SceneDebugStats& stats)
	{
		gDynamicCapturePerfStats.voxelMaintenanceCalls++;
		const bool lifecycleMode = (bool)nri_ptvoxelactorlifecycle;
		VoxelActorLifecycleApplyResult lifecycle = {};
		if (lifecycleMode)
		{
			lifecycle = ApplyVoxelActorLifecycleEvents();
		}
		else
		{
			// Legacy reconciliation already observes current state. Keep its
			// journal cursor current so a later mode change cannot replay stale
			// removals or resets into the delta path.
			NRIVoxelActorLifecycleJournalInput journalInput = {};
			journalInput.lifecycleModeEnabled = false;
			const NRIVoxelActorLifecycleJournalDecision journalDecision =
				ResolveNRIVoxelActorLifecycleJournal(journalInput);
			if (journalDecision.advanceCursor)
			{
				gVoxelActorLifecycleCursor = GetActorLifecycleJournal().LatestSerial();
			}
			gVoxelActorLifecycleEvents.clear();
		}
		const GameUpdateSnapshot gameUpdate = GetGameUpdateSnapshot();
		NRIVoxelActorMaintenanceInput input = {};
		input.simulationGeneration = gameUpdate.simulationGeneration;
		input.lifecycleModeEnabled = lifecycleMode;
		input.voxelsEnabled = (bool)r_voxels;
		input.forceReconcile = lifecycle.forceReconcile || gVoxelActorPendingRemoval;
		const NRIVoxelActorMaintenanceDecision decision = gVoxelActorMaintenanceGate.Evaluate(input);
		gDynamicCapturePerfStats.voxelMaintenanceReasonMask |= decision.reasonMask;

		if (decision.legacyEnumeration || lifecycle.forceLegacyReconcile)
		{
			gDynamicCapturePerfStats.voxelMaintenanceLegacyReconciles++;
			PruneVoxelActorCacheLegacy(stats);
		}
		else if (decision.reconcileCacheEntries)
		{
			gDynamicCapturePerfStats.voxelMaintenanceDeltaReconciles++;
			ReconcileVoxelActorCacheEntries(stats);
		}
		else
		{
			gDynamicCapturePerfStats.voxelMaintenanceSimulationSkips++;
		}

		stats.voxelCacheEntries = (unsigned int)gVoxelActorCache.size();
		if (ShouldCollectVoxelActorCacheDuplicationStats())
		{
			CollectVoxelActorCacheDuplicationStats(stats);
		}
	}

	void EndVoxelActorCacheFrame(SceneDebugStats& stats, bool rootCapture)
	{
		if (gVoxelActorCacheCaptureDepth > 0)
		{
			--gVoxelActorCacheCaptureDepth;
		}
		if (rootCapture)
		{
			PruneVoxelActorCache(stats);
		}
	}

	bool BeginVoxelMeshCacheFrame()
	{
		const bool rootCapture = gVoxelMeshCacheCaptureDepth++ == 0;
		if (rootCapture)
		{
			++gVoxelMeshCacheFrame;
			if (gVoxelMeshCacheFrame == 0)
			{
				gVoxelMeshCacheFrame = 1;
			}
			gVoxelMeshBuildsThisFrame = 0;
		}
		return rootCapture;
	}

	void EndVoxelMeshCacheFrame(bool rootCapture)
	{
		if (gVoxelMeshCacheCaptureDepth > 0)
		{
			--gVoxelMeshCacheCaptureDepth;
		}
		if (rootCapture)
		{
			gVoxelMeshBuildsThisFrame = 0;
		}
	}

	bool CanBuildVoxelMeshThisFrame()
	{
		const int buildBudget = (int)nri_ptvoxelmeshbuilds;
		return buildBudget <= 0 || gVoxelMeshBuildsThisFrame < (uint32_t)buildBudget;
	}

	void TraceDynamicVoxelMeshBuildEvent(const char* action, const char* reason, const VoxelMeshBuildContext* context, FVoxelModel* model, double buildMs, uint32_t vertices, uint32_t indices, bool valid)
	{
		if (!ShouldTraceNRIVoxelComputeMeshing() && (int)nri_ptvoxelcomputetrace <= 0)
		{
			return;
		}

		const HWSprite* sprite = context != nullptr ? context->sprite : nullptr;
		const VoxelActorCacheLookup* lookup = context != nullptr ? context->lookup : nullptr;
		const int32_t actorIndex =
			lookup != nullptr && lookup->actorIndex >= 0 ? lookup->actorIndex :
			sprite != nullptr && sprite->Sprite != nullptr && sprite->Sprite->ownerActor != nullptr ? (int32_t)sprite->Sprite->ownerActor->GetIndex() :
			-1;
		const int32_t statnum = sprite != nullptr && sprite->Sprite != nullptr ? sprite->Sprite->statnum : -1;
		const int32_t sourcePicnum =
			context != nullptr && context->sourcePicnum >= 0 ? context->sourcePicnum :
			lookup != nullptr ? lookup->sourcePicnum :
			-1;
		const int32_t resolvedVoxelIndex =
			context != nullptr ? context->resolvedVoxelIndex :
			lookup != nullptr ? lookup->resolvedVoxelIndex :
			-1;
		const uint64_t meshVariantHash =
			context != nullptr && context->meshVariantHash != 0 ? context->meshVariantHash :
			lookup != nullptr ? lookup->meshVariantHash :
			0;
		const uint64_t materialVariantHash =
			context != nullptr && context->materialVariantHash != 0 ? context->materialVariantHash :
			lookup != nullptr ? lookup->materialVariantHash :
			0;
		Printf("PERF pt voxel dynamic mesh miss NRI: action=%s reason=%s cpu_mesh_class=%s class_reason=%s actor=%d stat=%d model=%p pic=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx vertices=%u indices=%u prims=%u valid=%u build_ms=%.3f cache_update=%u retained_surface=%u forced_transient=%u direct_publish=%u raw_archive=%u\n",
			action != nullptr ? action : "unknown",
			reason != nullptr ? reason : "unknown",
			context != nullptr ? context->cpuMeshClassification : "unsupported",
			context != nullptr ? context->cpuMeshReason : "unclassified",
			actorIndex,
			statnum,
			(void*)model,
			sourcePicnum,
			resolvedVoxelIndex,
			(unsigned long long)meshVariantHash,
			(unsigned long long)materialVariantHash,
			vertices,
			indices,
			indices / 3u,
			valid ? 1u : 0u,
			buildMs,
			context != nullptr && context->cacheSurfaceUpdate ? 1u : 0u,
			context != nullptr && context->hadRetainedSurface ? 1u : 0u,
			context != nullptr && context->forceTransient ? 1u : 0u,
			context != nullptr && context->directPublish ? 1u : 0u,
			context != nullptr && context->rawArchive ? 1u : 0u);
	}

	const FVoxelMeshData* GetCachedVoxelMesh(FVoxelModel* model, bool& outDeferred, const VoxelMeshBuildContext* context = nullptr)
	{
		outDeferred = false;
		if (model == nullptr)
		{
			return nullptr;
		}

		auto found = gVoxelMeshCache.find(model);
		if (found == gVoxelMeshCache.end())
		{
			gDynamicCapturePerfStats.voxelMeshCacheMisses++;
			if (!CanBuildVoxelMeshThisFrame())
			{
				outDeferred = true;
				gDynamicCapturePerfStats.voxelMeshCacheDeferred++;
				gDynamicCapturePerfStats.modelBudgetTruncations++;
				TraceDynamicVoxelMeshBuildEvent("defer", "mesh-build-budget", context, model, 0.0, 0, 0, false);
				return nullptr;
			}

			VoxelMeshCacheEntry entry = {};
			const auto buildStart = std::chrono::steady_clock::now();
			{
				ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelMeshBuildMs);
				model->BuildCpuMesh(entry.mesh);
			}
			const double buildMs = DurationMs(buildStart, std::chrono::steady_clock::now());
			if (ShouldTraceNRIVoxelComputeMeshing() || ShouldRunNRIVoxelComputeMeshing())
			{
				FVoxelRawMeshStats rawStats = {};
				TArray<FVoxelRawSlabRecord> rawSlabs;
				TArray<FVoxelRawFaceRecord> rawFaces;
				TArray<FVoxelRawColorRunRecord> rawColorRuns;
				model->BuildRawMeshStats(
					rawStats,
					ShouldRunNRIVoxelComputeMeshing() ? &rawSlabs : nullptr,
					ShouldEmitNRIVoxelComputeMeshing() ? &rawFaces : nullptr,
					ShouldRunNRIVoxelComputeMeshing() ? &rawColorRuns : nullptr);
				if (ShouldTraceNRIVoxelComputeMeshing())
				{
					Printf(
						"PERF pt voxel raw census NRI: model=%p size=%ux%ux%u raw_bytes=%llu slabs=%u voxels=%u faces=%u face_records=%u top=%u bottom=%u side_spans=%u vertices_nodedupe=%u cpu_vertices=%u indices=%u cpu_indices=%u queued_compute=%u emit_compute=%u\n",
						(void*)model,
						rawStats.sizeX,
						rawStats.sizeY,
						rawStats.sizeZ,
						(unsigned long long)rawStats.rawByteCount,
						rawStats.slabCount,
						rawStats.voxelCount,
						rawStats.coalescedFaceCount,
						(uint32_t)rawFaces.Size(),
						rawStats.topFaceCount,
						rawStats.bottomFaceCount,
						rawStats.sideFaceSpanCount,
						rawStats.noDedupeVertexCount,
						(uint32_t)entry.mesh.vertices.Size(),
						rawStats.indexCount,
						(uint32_t)entry.mesh.indices.Size(),
						ShouldRunNRIVoxelComputeMeshing() ? 1u : 0u,
						ShouldEmitNRIVoxelComputeMeshing() ? 1u : 0u);
				}
				QueueNRIVoxelComputeCountJob(
					model,
					rawStats,
					ShouldRunNRIVoxelComputeMeshing() ? &rawSlabs : nullptr,
					ShouldEmitNRIVoxelComputeMeshing() ? &rawFaces : nullptr,
					ShouldRunNRIVoxelComputeMeshing() ? &rawColorRuns : nullptr,
					entry.mesh);
			}
			entry.built = true;
			entry.valid = entry.mesh.vertices.Size() > 0 && entry.mesh.indices.Size() >= 3;
			TraceDynamicVoxelMeshBuildEvent(
				"build",
				entry.valid ? "cache-miss" : "invalid-mesh",
				context,
				model,
				buildMs,
				(uint32_t)entry.mesh.vertices.Size(),
				(uint32_t)entry.mesh.indices.Size(),
				entry.valid);
			gVoxelMeshBuildsThisFrame++;
			gDynamicCapturePerfStats.voxelMeshCacheBuilds++;
			if (!entry.valid)
			{
				gDynamicCapturePerfStats.voxelMeshCacheInvalid++;
			}
			found = gVoxelMeshCache.emplace(model, std::move(entry)).first;
		}
		else
		{
			gDynamicCapturePerfStats.voxelMeshCacheHits++;
		}

		const VoxelMeshCacheEntry& entry = found->second;
		if (!entry.built || !entry.valid)
		{
			return nullptr;
		}
		return &entry.mesh;
	}

	bool TrySpendVoxelTriangleBudget(uint32_t triangleCount, VoxelCaptureBudget& budget)
	{
		if ((int)nri_ptvoxelmaxtriangles > 0 && triangleCount > (uint32_t)(int)nri_ptvoxelmaxtriangles)
		{
			return false;
		}

		if (budget.unlimited)
		{
			return true;
		}

		if (!budget.spentTriangleBudget && triangleCount > budget.remainingTriangles)
		{
			budget.remainingTriangles = 0;
			budget.spentTriangleBudget = true;
			return true;
		}

		if (triangleCount > budget.remainingTriangles)
		{
			return false;
		}

		budget.remainingTriangles -= triangleCount;
		budget.spentTriangleBudget = true;
		return true;
	}

	bool TrySpendVoxelCacheUpdateBudget(VoxelCaptureBudget& budget)
	{
		if (budget.unlimitedCacheUpdates)
		{
			return true;
		}
		if (budget.remainingCacheUpdates == 0)
		{
			return false;
		}

		--budget.remainingCacheUpdates;
		return true;
	}

	bool BuildCanonicalVoxelMeshSurface(const FVoxelMeshData& mesh, SurfaceRef& outSurface)
	{
		const unsigned int indexCount = mesh.indices.Size();
		outSurface = {};
		outSurface.vertices.reserve(mesh.vertices.Size());
		for (unsigned int i = 0; i < mesh.vertices.Size(); ++i)
		{
			outSurface.vertices.push_back(MakeCapturedLocalModelVertex(mesh.vertices[i]));
		}

		const unsigned int vertexCount = mesh.vertices.Size();
		outSurface.indices.reserve(indexCount);
		for (unsigned int i = 0; i + 2u < indexCount; i += 3u)
		{
			const unsigned int i0 = mesh.indices[i + 0u];
			const unsigned int i1 = mesh.indices[i + 1u];
			const unsigned int i2 = mesh.indices[i + 2u];
			if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
			{
				continue;
			}

			outSurface.indices.push_back(i0);
			outSurface.indices.push_back(i1);
			outSurface.indices.push_back(i2);
		}

		if (outSurface.indices.empty())
		{
			return false;
		}

		return true;
	}

	bool BuildCanonicalVoxelMeshSurface(const GeometryData& geometry, SurfaceRef& outSurface)
	{
		outSurface = {};
		outSurface.vertices.reserve(geometry.vertices.size());
		for (const SceneVertex& source : geometry.vertices)
		{
			CapturedVertex vertex = {};
			vertex.position[0] = source.position[0];
			vertex.position[1] = source.position[1];
			vertex.position[2] = source.position[2];
			vertex.prevPosition[0] = source.prevPosition[0];
			vertex.prevPosition[1] = source.prevPosition[1];
			vertex.prevPosition[2] = source.prevPosition[2];
			vertex.uv[0] = source.uv[0];
			vertex.uv[1] = source.uv[1];
			outSurface.vertices.push_back(vertex);
		}

		const uint32_t vertexCount = (uint32_t)geometry.vertices.size();
		outSurface.indices.reserve(geometry.indices.size());
		for (size_t i = 0; i + 2u < geometry.indices.size(); i += 3u)
		{
			const uint32_t i0 = geometry.indices[i + 0u];
			const uint32_t i1 = geometry.indices[i + 1u];
			const uint32_t i2 = geometry.indices[i + 2u];
			if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
			{
				continue;
			}
			outSurface.indices.push_back(i0);
			outSurface.indices.push_back(i1);
			outSurface.indices.push_back(i2);
		}

		return !outSurface.indices.empty();
	}

	bool IsVoxelMeshVariantSurfaceReady(uint64_t meshVariantHash)
	{
		auto found = gVoxelMeshVariantSurfaceCache.find(meshVariantHash);
		return found != gVoxelMeshVariantSurfaceCache.end() &&
			found->second.built &&
			found->second.valid;
	}

	const SurfaceRef* GetReadyVoxelMeshVariantSurface(uint64_t meshVariantHash)
	{
		auto found = gVoxelMeshVariantSurfaceCache.find(meshVariantHash);
		if (found == gVoxelMeshVariantSurfaceCache.end() ||
			!found->second.built ||
			!found->second.valid)
		{
			return nullptr;
		}
		return &found->second.canonicalSurface;
	}

	bool TryPromoteVoxelMeshVariantSurfaceFromCompute(
		const VoxelActorCacheLookup& lookup,
		FVoxelModel* model,
		bool& outPending)
	{
		outPending = false;
		if (!ShouldConsumeNRIVoxelComputeMeshing() ||
			ShouldDirectPublishNRIVoxelComputeMeshing() ||
			lookup.meshVariantHash == 0 ||
			model == nullptr ||
			IsVoxelMeshVariantSurfaceReady(lookup.meshVariantHash))
		{
			return false;
		}

		GeometryData generatedGeometry;
		uint32_t computeJobId = 0;
		if (TakeNRIVoxelComputeGeneratedGeometry(lookup.meshVariantHash, generatedGeometry, &computeJobId))
		{
			VoxelMeshVariantSurfaceCacheEntry entry = {};
			entry.meshVariantHash = lookup.meshVariantHash;
			entry.sourcePicnum = lookup.sourcePicnum;
			entry.resolvedVoxelIndex = lookup.resolvedVoxelIndex;
			entry.built = true;
			entry.valid = BuildCanonicalVoxelMeshSurface(generatedGeometry, entry.canonicalSurface);
			if (entry.valid)
			{
				const VoxelGeometryContentHashes hashes = BuildVoxelGeometryContentHashes(entry.canonicalSurface);
				entry.geometryContentHash = hashes.geometryContentHash;
				entry.renderPrimitiveHash = hashes.renderPrimitiveHash;
				gDynamicCapturePerfStats.voxelCanonicalSurfaceBuilds++;
				gVoxelMeshVariantSurfaceCache[lookup.meshVariantHash] = std::move(entry);
				if (ShouldTraceNRIVoxelComputeMeshing())
				{
					Printf("PERF pt voxel compute cutover NRI: action=surface-ready source=compute mesh_variant=0x%llx job=%u vertices=%u indices=%u primitives=%u\n",
						(unsigned long long)lookup.meshVariantHash,
						computeJobId,
						(uint32_t)generatedGeometry.vertices.size(),
						(uint32_t)generatedGeometry.indices.size(),
						(uint32_t)generatedGeometry.primitives.size());
				}
			}
			else
			{
				gDynamicCapturePerfStats.voxelCanonicalSurfaceInvalid++;
				if (ShouldTraceNRIVoxelComputeMeshing())
				{
					Printf("PERF pt voxel compute cutover NRI: action=fallback reason=invalid-surface mesh_variant=0x%llx job=%u vertices=%u indices=%u primitives=%u\n",
						(unsigned long long)lookup.meshVariantHash,
						computeJobId,
						(uint32_t)generatedGeometry.vertices.size(),
						(uint32_t)generatedGeometry.indices.size(),
						(uint32_t)generatedGeometry.primitives.size());
				}
			}
			return entry.valid;
		}

		const NRIVoxelComputeGeneratedGeometryStatus status =
			RequestNRIVoxelComputeGeneratedGeometry(lookup.meshVariantHash, model);
		if (status == NRIVoxelComputeGeneratedGeometryStatus::Queued)
		{
			outPending = true;
			if (ShouldTraceNRIVoxelComputeMeshing())
			{
				Printf("PERF pt voxel compute cutover NRI: action=defer reason=compute-pending mesh_variant=0x%llx tex=%d voxel=%d\n",
					(unsigned long long)lookup.meshVariantHash,
					lookup.sourcePicnum,
					lookup.resolvedVoxelIndex);
			}
			return false;
		}

		if (ShouldTraceNRIVoxelComputeMeshing())
		{
			Printf("PERF pt voxel compute cutover NRI: action=fallback reason=%s mesh_variant=0x%llx tex=%d voxel=%d\n",
				status == NRIVoxelComputeGeneratedGeometryStatus::Failed ? "compute-failed" : "compute-unavailable",
				(unsigned long long)lookup.meshVariantHash,
				lookup.sourcePicnum,
				lookup.resolvedVoxelIndex);
		}
		return false;
	}

	bool GetReadyVoxelMeshVariantContentHashes(uint64_t meshVariantHash, uint64_t& outGeometryContentHash, uint64_t& outRenderPrimitiveHash)
	{
		outGeometryContentHash = 0;
		outRenderPrimitiveHash = 0;
		auto found = gVoxelMeshVariantSurfaceCache.find(meshVariantHash);
		if (found == gVoxelMeshVariantSurfaceCache.end() ||
			!found->second.built ||
			!found->second.valid)
		{
			return false;
		}

		if (found->second.geometryContentHash == 0 || found->second.renderPrimitiveHash == 0)
		{
			const VoxelGeometryContentHashes hashes = BuildVoxelGeometryContentHashes(found->second.canonicalSurface);
			if (found->second.geometryContentHash == 0)
			{
				found->second.geometryContentHash = hashes.geometryContentHash;
			}
			if (found->second.renderPrimitiveHash == 0)
			{
				found->second.renderPrimitiveHash = hashes.renderPrimitiveHash;
			}
		}
		outGeometryContentHash = found->second.geometryContentHash;
		outRenderPrimitiveHash = found->second.renderPrimitiveHash;
		return outGeometryContentHash != 0 || outRenderPrimitiveHash != 0;
	}

	const SurfaceRef* GetCachedVoxelMeshVariantSurface(
		const VoxelActorCacheLookup& lookup,
		const FVoxelMeshData& mesh,
		bool recordPerf = true)
	{
		if (lookup.meshVariantHash == 0)
		{
			return nullptr;
		}

		auto found = gVoxelMeshVariantSurfaceCache.find(lookup.meshVariantHash);
		if (found == gVoxelMeshVariantSurfaceCache.end())
		{
			VoxelMeshVariantSurfaceCacheEntry entry = {};
			entry.meshVariantHash = lookup.meshVariantHash;
			entry.sourcePicnum = lookup.sourcePicnum;
			entry.resolvedVoxelIndex = lookup.resolvedVoxelIndex;
			entry.built = true;
			entry.valid = BuildCanonicalVoxelMeshSurface(mesh, entry.canonicalSurface);
			if (entry.valid)
			{
				const VoxelGeometryContentHashes hashes = BuildVoxelGeometryContentHashes(entry.canonicalSurface);
				entry.geometryContentHash = hashes.geometryContentHash;
				entry.renderPrimitiveHash = hashes.renderPrimitiveHash;
			}
			if (recordPerf)
			{
				gDynamicCapturePerfStats.voxelCanonicalSurfaceBuilds++;
			}
			if (!entry.valid)
			{
				if (recordPerf)
				{
					gDynamicCapturePerfStats.voxelCanonicalSurfaceInvalid++;
				}
			}
			found = gVoxelMeshVariantSurfaceCache.emplace(lookup.meshVariantHash, std::move(entry)).first;
		}
		else
		{
			if (recordPerf)
			{
				gDynamicCapturePerfStats.voxelCanonicalSurfaceHits++;
			}
		}

		const VoxelMeshVariantSurfaceCacheEntry& entry = found->second;
		if (!entry.built || !entry.valid)
		{
			return nullptr;
		}
		if (found->second.geometryContentHash == 0 || found->second.renderPrimitiveHash == 0)
		{
			const VoxelGeometryContentHashes hashes = BuildVoxelGeometryContentHashes(found->second.canonicalSurface);
			if (found->second.geometryContentHash == 0)
			{
				found->second.geometryContentHash = hashes.geometryContentHash;
			}
			if (found->second.renderPrimitiveHash == 0)
			{
				found->second.renderPrimitiveHash = hashes.renderPrimitiveHash;
			}
		}
		return &entry.canonicalSurface;
	}

	bool BuildVoxelMeshSurfaceFromCanonical(
		const HWSprite& sprite,
		uint32_t drawListType,
		const SurfaceRef& canonicalSurface,
		const MaterialRef& voxelMaterial,
		SurfaceRef& outSurface)
	{
		outSurface = {};
		outSurface.material = voxelMaterial;
		outSurface.provenance = MakeSpriteProvenance(sprite, SurfaceSourceType::VoxelProxySprite, drawListType, outSurface.material.flags);
		outSurface.vertices.reserve(canonicalSurface.vertices.size());
		for (const CapturedVertex& source : canonicalSurface.vertices)
		{
			CapturedVertex vertex = {};
			TransformModelPoint(sprite.rotmat, source.position[0], source.position[1], source.position[2], vertex, source.uv[0], source.uv[1]);
			outSurface.vertices.push_back(vertex);
		}
		outSurface.indices = canonicalSurface.indices;
		if (outSurface.indices.empty())
		{
			return false;
		}

		if (sprite.Sprite != nullptr && sprite.Sprite->ownerActor != nullptr)
		{
			ApplyActorPreviousTransform(outSurface, sprite.Sprite->ownerActor);
		}
		return true;
	}

	bool BuildVoxelMeshSurface(
		const HWSprite& sprite,
		uint32_t drawListType,
		const VoxelActorCacheLookup& lookup,
		const FVoxelMeshData& mesh,
		const MaterialRef& voxelMaterial,
		SurfaceRef& outSurface)
	{
		VoxelActorCacheLookup effectiveLookup = lookup;
		if (effectiveLookup.meshVariantHash == 0)
		{
			const VoxelMeshVariantKey meshVariantKey = BuildVoxelMeshVariantKey(sprite);
			effectiveLookup.meshVariantHash = BuildVoxelMeshVariantKeyHash(meshVariantKey);
			effectiveLookup.sourcePicnum = sprite.Sprite != nullptr ? sprite.Sprite->spritetexture().GetIndex() : -1;
			effectiveLookup.resolvedVoxelIndex = meshVariantKey.resolvedVoxelIndex;
		}

		if (const SurfaceRef* canonicalSurface = GetCachedVoxelMeshVariantSurface(effectiveLookup, mesh))
		{
			return BuildVoxelMeshSurfaceFromCanonical(sprite, drawListType, *canonicalSurface, voxelMaterial, outSurface);
		}

		SurfaceRef canonicalSurface = {};
		if (!BuildCanonicalVoxelMeshSurface(mesh, canonicalSurface))
		{
			return false;
		}
		return BuildVoxelMeshSurfaceFromCanonical(sprite, drawListType, canonicalSurface, voxelMaterial, outSurface);
	}

	bool ShouldUseTransientVoxelActorCapture(const HWSprite& sprite)
	{
		if (sprite.Sprite == nullptr)
		{
			return false;
		}

		// Duke security cameras (CAMERA1..CAMERA5, tiles 621..625) are actor-driven
		// camera props. They currently exercise a driver-hung path when promoted
		// into per-actor persistent BLASes, so keep them in the transient dynamic
		// overlay until the durable actor/AS representation models this class.
		return sprite.Sprite->picnum >= 621 && sprite.Sprite->picnum <= 625;
	}

	bool IsVoxelActorCacheAuthoringMode(DynamicVoxelCaptureMode captureMode)
	{
		return captureMode == DynamicVoxelCaptureMode::Authoritative;
	}

	bool CaptureVoxelMeshSprite(HWDrawInfo* di, HWSprite& sprite, uint32_t drawListType, VoxelCaptureBudget& budget, std::vector<SurfaceRef>& outSprites, SceneDebugStats& stats, DynamicVoxelCaptureMode captureMode)
	{
		if (sprite.modelframe >= 0 || sprite.voxel == nullptr || sprite.voxel->model == nullptr)
		{
			return false;
		}

		FGameTexture* voxelTexture = TexMan.GetGameTexture(sprite.voxel->model->GetPaletteTexture());
		if (voxelTexture == nullptr || !voxelTexture->isValid())
		{
			stats.voxelStableUncacheable++;
			return false;
		}

		FGameTexture* emissiveSourceTexture = GetVoxelReplacementEmissiveSourceTexture(sprite);
		const MaterialRef voxelMaterial = MakeVoxelPaletteMaterialRef(voxelTexture, emissiveSourceTexture, sprite.palette, sprite.shade, sprite.alpha, MaterialFlag_Sprite);
		const bool authoringIndirectOnlyActor =
			captureMode == DynamicVoxelCaptureMode::Authoritative &&
			sprite.Sprite != nullptr &&
			IsLocalPlayerActor(sprite.Sprite->ownerActor);
		const bool forceTransientVoxel = ShouldUseTransientVoxelActorCapture(sprite);
		if (forceTransientVoxel)
		{
			captureMode = DynamicVoxelCaptureMode::Transient;
		}
		if (captureMode == DynamicVoxelCaptureMode::ReadOnlyCache)
		{
			const bool consumedCache = TryConsumeReadOnlyVoxelActorCacheSurface(sprite, voxelTexture, voxelMaterial, stats);
			if (!consumedCache)
			{
				stats.voxelCacheNotCaptured++;
			}
			return consumedCache;
		}

		VoxelActorCacheLookup cacheLookup = {};
		if (IsVoxelActorCacheAuthoringMode(captureMode))
		{
			ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelClassifyMs);
			cacheLookup = TrackVoxelActorSignature(sprite, voxelTexture, voxelMaterial, stats);
		}
		if (cacheLookup.stability == VoxelActorStability::Stable &&
			cacheLookup.entry != nullptr &&
			cacheLookup.entry->hasSurface &&
			cacheLookup.entry->persistentReady)
		{
			return true;
		}

		const bool cacheSurfaceUpdate = IsVoxelActorCacheAuthoringMode(captureMode) && cacheLookup.stability != VoxelActorStability::Stable;
		const bool transformRebakeAlreadyResident =
			cacheLookup.stability == VoxelActorStability::TransformRebake &&
			cacheLookup.entry != nullptr &&
			cacheLookup.entry->persistentReady;
		const bool sharedVariantSurfaceReadyForUpdate =
			cacheSurfaceUpdate &&
			cacheLookup.meshVariantHash != 0 &&
			IsVoxelMeshVariantSurfaceReady(cacheLookup.meshVariantHash);
		const bool directPublish = ShouldDirectPublishNRIVoxelComputeMeshing();
		const DynamicVoxelCaptureRouting routing = ClassifyDynamicVoxelCaptureRouting(
			captureMode,
			forceTransientVoxel,
			cacheSurfaceUpdate,
			directPublish,
			cacheLookup,
			sprite.voxel->model);
		const bool cacheUpdateConsumesActorBudget =
			cacheSurfaceUpdate &&
			!transformRebakeAlreadyResident &&
			!sharedVariantSurfaceReadyForUpdate &&
			!routing.directOnlyAdmission;
		const SurfaceProvenance voxelMaterialProvenance =
			MakeSpriteProvenance(sprite, SurfaceSourceType::VoxelProxySprite, drawListType, voxelMaterial.flags);
		auto deferDesiredVariant = [&](VoxelActorPendingReason reason) -> bool
		{
			if (cacheSurfaceUpdate)
			{
				MarkVoxelActorVariantPending(
					cacheLookup,
					reason,
					&voxelMaterial,
					&voxelMaterialProvenance,
					routing.primitiveCount);
				stats.voxelCacheDeferred++;
				gDynamicCapturePerfStats.voxelCacheDeferred++;
				if (HasLastValidResidentVoxelSurface(cacheLookup))
				{
					TraceVoxelActorFallbackLastValid(sprite, cacheLookup, reason);
					return true;
				}
			}

			TraceVoxelActorFirstUseFallback(sprite, cacheLookup, reason);
			return true;
		};
		if (cacheUpdateConsumesActorBudget && !TrySpendVoxelCacheUpdateBudget(budget))
		{
			gDynamicCapturePerfStats.modelBudgetTruncations++;
			return deferDesiredVariant(VoxelActorPendingReason::ActorBudget);
		}

		bool computeSurfacePending = false;
		if (cacheSurfaceUpdate &&
			cacheLookup.meshVariantHash != 0 &&
			TryPromoteVoxelMeshVariantSurfaceFromCompute(cacheLookup, sprite.voxel->model, computeSurfacePending))
		{
			if (const SurfaceRef* canonicalSurface = GetReadyVoxelMeshVariantSurface(cacheLookup.meshVariantHash))
			{
				const unsigned int previousStores = stats.voxelCacheSurfaceStores;
				const unsigned int previousRebuilds = stats.voxelCacheSurfaceRebuilds;
				const unsigned int previousTransformRebakes = stats.voxelCacheTransformRebakes;
				const bool wasPersistentReady = cacheLookup.entry != nullptr && cacheLookup.entry->persistentReady;
				SurfaceRef lightSurface = {};
				lightSurface.material = voxelMaterial;
				lightSurface.provenance = MakeSpriteProvenance(sprite, SurfaceSourceType::VoxelProxySprite, drawListType, lightSurface.material.flags);
				{
					ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelStoreMs);
					StoreVoxelActorCacheSurface(cacheLookup, *canonicalSurface, lightSurface, VoxelMeshBakeSpace::LocalSpace, true, stats);
				}
				gDynamicCapturePerfStats.voxelCacheStores += stats.voxelCacheSurfaceStores - previousStores;
				gDynamicCapturePerfStats.voxelCacheRebuilds += stats.voxelCacheSurfaceRebuilds - previousRebuilds;
				gDynamicCapturePerfStats.voxelCacheRebuilds += stats.voxelCacheTransformRebakes - previousTransformRebakes;
				const auto storedEntry = gVoxelActorCache.find(cacheLookup.identityKey);
				if (!wasPersistentReady &&
					cacheLookup.identityKey != 0 &&
					storedEntry != gVoxelActorCache.end() &&
					storedEntry->second.persistentReady)
				{
					EmitVoxelActorKeyTrace(sprite, cacheLookup, "compute-variant-promote");
				}
				return true;
			}
		}
		if (computeSurfacePending)
		{
			return deferDesiredVariant(VoxelActorPendingReason::MeshDeferred);
		}
		if (routing.directOnlyAdmission)
		{
			return deferDesiredVariant(VoxelActorPendingReason::MeshDeferred);
		}

		VoxelMeshBuildContext meshBuildContext = {};
		meshBuildContext.sprite = &sprite;
		meshBuildContext.lookup = &cacheLookup;
		meshBuildContext.meshVariantHash = cacheLookup.meshVariantHash;
		meshBuildContext.materialVariantHash = cacheLookup.materialVariantHash;
		meshBuildContext.sourcePicnum = cacheLookup.sourcePicnum;
		meshBuildContext.resolvedVoxelIndex = cacheLookup.resolvedVoxelIndex;
		meshBuildContext.cacheSurfaceUpdate = cacheSurfaceUpdate;
		meshBuildContext.forceTransient = forceTransientVoxel;
		meshBuildContext.directPublish = directPublish;
		meshBuildContext.rawArchive = (bool)nri_ptvoxelcomputerawarchive;
		meshBuildContext.hadRetainedSurface = HasLastValidResidentVoxelSurface(cacheLookup);
		meshBuildContext.cpuMeshClassification = routing.cpuMeshClassification;
		meshBuildContext.cpuMeshReason = routing.reason;
		const FVoxelMeshData* mesh = nullptr;
		bool meshDeferred = false;
		{
			ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelMeshMs);
			mesh = GetCachedVoxelMesh(sprite.voxel->model, meshDeferred, &meshBuildContext);
		}
		if (meshDeferred)
		{
			if (cacheSurfaceUpdate)
			{
				return deferDesiredVariant(VoxelActorPendingReason::MeshDeferred);
			}

			if (!forceTransientVoxel && !authoringIndirectOnlyActor)
			{
				CaptureVoxelProxySprite(sprite, drawListType, voxelTexture, outSprites);
			}
			return true;
		}
		if (mesh == nullptr)
		{
			if (cacheSurfaceUpdate)
			{
				return deferDesiredVariant(VoxelActorPendingReason::MeshDeferred);
			}
			stats.voxelStableUncacheable++;
			return false;
		}

		if (sharedVariantSurfaceReadyForUpdate)
		{
			if (const SurfaceRef* canonicalSurface = GetCachedVoxelMeshVariantSurface(cacheLookup, *mesh, false))
			{
				const unsigned int previousStores = stats.voxelCacheSurfaceStores;
				const unsigned int previousRebuilds = stats.voxelCacheSurfaceRebuilds;
				const unsigned int previousTransformRebakes = stats.voxelCacheTransformRebakes;
				const bool wasPersistentReady = cacheLookup.entry != nullptr && cacheLookup.entry->persistentReady;
				SurfaceRef lightSurface = {};
				lightSurface.material = voxelMaterial;
				lightSurface.provenance = MakeSpriteProvenance(sprite, SurfaceSourceType::VoxelProxySprite, drawListType, lightSurface.material.flags);
				{
					ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelStoreMs);
					StoreVoxelActorCacheSurface(cacheLookup, *canonicalSurface, lightSurface, VoxelMeshBakeSpace::LocalSpace, true, stats);
				}
				gDynamicCapturePerfStats.voxelCacheStores += stats.voxelCacheSurfaceStores - previousStores;
				gDynamicCapturePerfStats.voxelCacheRebuilds += stats.voxelCacheSurfaceRebuilds - previousRebuilds;
				gDynamicCapturePerfStats.voxelCacheRebuilds += stats.voxelCacheTransformRebakes - previousTransformRebakes;
				const auto storedEntry = gVoxelActorCache.find(cacheLookup.identityKey);
				if (!wasPersistentReady &&
					cacheLookup.identityKey != 0 &&
					storedEntry != gVoxelActorCache.end() &&
					storedEntry->second.persistentReady)
				{
					EmitVoxelActorKeyTrace(sprite, cacheLookup, "shared-variant-promote");
				}
				return true;
			}
		}

		const unsigned int indexCount = mesh->indices.Size();
		const uint32_t triangleCount = indexCount / 3u;
		if (!TrySpendVoxelTriangleBudget(triangleCount, budget))
		{
			gDynamicCapturePerfStats.modelBudgetTruncations++;
			if (cacheSurfaceUpdate)
			{
				return deferDesiredVariant(VoxelActorPendingReason::TriangleBudget);
			}

			if (!forceTransientVoxel && !authoringIndirectOnlyActor)
			{
				CaptureVoxelProxySprite(sprite, drawListType, voxelTexture, outSprites);
			}
			return true;
		}

		SurfaceRef exactSurface = {};
		bool hasExactSurface = false;
		{
			ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelSurfaceMs);
			gDynamicCapturePerfStats.modelSurfaceBuilds++;
			hasExactSurface = BuildVoxelMeshSurface(sprite, drawListType, cacheLookup, *mesh, voxelMaterial, exactSurface);
		}
		if (!hasExactSurface)
		{
			if (cacheSurfaceUpdate)
			{
				return deferDesiredVariant(VoxelActorPendingReason::SurfaceBuildFailed);
			}
			return false;
		}

		if (cacheSurfaceUpdate)
		{
			const unsigned int previousStores = stats.voxelCacheSurfaceStores;
			const unsigned int previousRebuilds = stats.voxelCacheSurfaceRebuilds;
			const unsigned int previousTransformRebakes = stats.voxelCacheTransformRebakes;
			const bool wasPersistentReady = cacheLookup.entry != nullptr && cacheLookup.entry->persistentReady;
			const bool hadSurface = cacheLookup.entry != nullptr && cacheLookup.entry->hasSurface;
			const uint32_t exactPrimitiveCount = CountSurfacePrimitives(exactSurface);
			const SurfaceRef* canonicalSurface = GetCachedVoxelMeshVariantSurface(cacheLookup, *mesh, false);
			const SurfaceRef& storedMeshSurface = canonicalSurface != nullptr ? *canonicalSurface : exactSurface;
			const VoxelMeshBakeSpace storedBakeSpace =
				canonicalSurface != nullptr ? VoxelMeshBakeSpace::LocalSpace : VoxelMeshBakeSpace::BakedTransform;
			const bool sharedVariantReady =
				storedBakeSpace == VoxelMeshBakeSpace::LocalSpace &&
				IsVoxelMeshVariantSurfaceReady(cacheLookup.meshVariantHash);
			{
				ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelStoreMs);
				StoreVoxelActorCacheSurface(cacheLookup, storedMeshSurface, exactSurface, storedBakeSpace, sharedVariantReady, stats);
			}
			gDynamicCapturePerfStats.voxelCacheStores += stats.voxelCacheSurfaceStores - previousStores;
			gDynamicCapturePerfStats.voxelCacheRebuilds += stats.voxelCacheSurfaceRebuilds - previousRebuilds;
			gDynamicCapturePerfStats.voxelCacheRebuilds += stats.voxelCacheTransformRebakes - previousTransformRebakes;
			const auto storedEntry = gVoxelActorCache.find(cacheLookup.identityKey);
			const bool nowPersistentReady =
				cacheLookup.identityKey != 0 &&
				storedEntry != gVoxelActorCache.end() &&
				storedEntry->second.persistentReady;
			if (!wasPersistentReady &&
				!nowPersistentReady &&
				(hadSurface || exactPrimitiveCount <= kTransientVoxelLiveSurfacePrimitiveLimit))
			{
				const VoxelActorPendingReason pendingReason =
					cacheLookup.entry != nullptr ? (VoxelActorPendingReason)cacheLookup.entry->pendingReason : VoxelActorPendingReason::None;
				const DynamicVoxelEscapeReason escapeReason =
					cacheLookup.identityKey == 0 ? DynamicVoxelEscapeReason::NotCacheable :
					GetDynamicVoxelEscapeReasonForPending(pendingReason);
				RecordDynamicVoxelEscape(stats, sprite, cacheLookup, exactSurface, escapeReason);
				if (!authoringIndirectOnlyActor)
				{
					outSprites.push_back(std::move(exactSurface));
				}
			}
			return true;
		}

		const DynamicVoxelEscapeReason escapeReason =
			forceTransientVoxel ? DynamicVoxelEscapeReason::CameraOrWeaponSpecial :
			captureMode == DynamicVoxelCaptureMode::Transient ? DynamicVoxelEscapeReason::LifecycleTransient :
			cacheLookup.identityKey == 0 ? DynamicVoxelEscapeReason::NotCacheable :
			DynamicVoxelEscapeReason::Unknown;
		RecordDynamicVoxelEscape(stats, sprite, cacheLookup, exactSurface, escapeReason);
		if (!authoringIndirectOnlyActor)
		{
			outSprites.push_back(std::move(exactSurface));
		}
		return true;
	}

	VoxelCaptureBudget MakeVoxelCaptureBudget()
	{
		VoxelCaptureBudget budget = {};
		budget.unlimited = (int)nri_ptvoxeltrianglebudget <= 0;
		budget.remainingTriangles = budget.unlimited ? 0u : (uint32_t)(int)nri_ptvoxeltrianglebudget;
		budget.unlimitedCacheUpdates = (int)nri_ptvoxelcaptureactors <= 0;
		budget.remainingCacheUpdates = budget.unlimitedCacheUpdates ? 0u : (uint32_t)(int)nri_ptvoxelcaptureactors;
		return budget;
	}

	void CaptureModelSprites(HWDrawInfo& di, HWDrawList& list, uint32_t drawListType, std::vector<SurfaceRef>& outSprites, SceneDebugStats& stats, DynamicVoxelCaptureMode captureMode)
	{
		const bool rootMeshCapture = BeginVoxelMeshCacheFrame();
		std::vector<HWSprite*> sprites;
		if (sprites.capacity() < list.sprites.Size())
		{
			gDynamicCapturePerfStats.modelScratchGrows++;
		}
		sprites.reserve(list.sprites.Size());
		auto finish = [&]()
		{
			EndVoxelMeshCacheFrame(rootMeshCapture);
		};

		for (auto* sprite : list.sprites)
		{
			if (sprite != nullptr)
			{
				sprites.push_back(sprite);
			}
		}
		gDynamicCapturePerfStats.modelActorCandidates += (uint32_t)sprites.size();

		if (sprites.size() > 1)
		{
			const DVector3& viewPos = di.Viewpoint.Pos;
			{
				ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelSortMs);
				std::stable_sort(sprites.begin(), sprites.end(), [&viewPos](const HWSprite* a, const HWSprite* b)
				{
					const double adx = (double)a->x - viewPos.X;
					const double ady = (double)a->y + viewPos.Y;
					const double adz = (double)a->z + viewPos.Z;
					const double bdx = (double)b->x - viewPos.X;
					const double bdy = (double)b->y + viewPos.Y;
					const double bdz = (double)b->z + viewPos.Z;
					return adx * adx + ady * ady + adz * adz < bdx * bdx + bdy * bdy + bdz * bdz;
				});
			}
			gDynamicCapturePerfStats.modelActorSorted += (uint32_t)sprites.size();
		}
		else
		{
			gDynamicCapturePerfStats.modelActorSortSkipped += (uint32_t)sprites.size();
		}

		VoxelCaptureBudget budget = MakeVoxelCaptureBudget();
		for (auto* sprite : sprites)
		{
			if (sprite == nullptr)
			{
				continue;
			}

			stats.modelDrawItems++;

			if (sprite->modelframe > 0)
			{
				stats.unsupportedModelDrawItems++;
				continue;
			}

			if (sprite->modelframe >= 0 || sprite->voxel == nullptr || sprite->voxel->model == nullptr)
			{
				continue;
			}

			if (CaptureVoxelMeshSprite(&di, *sprite, drawListType, budget, outSprites, stats, captureMode))
			{
				stats.voxelProxyDrawItems++;
			}
		}
		finish();
	}

	bool CaptureActorModelSprites(
		HWDrawList& list,
		uint32_t drawListType,
		int32_t actorIndex,
		bool captureTransient,
		std::vector<SurfaceRef>& outSprites,
		SceneDebugStats& stats)
	{
		const bool rootMeshCapture = BeginVoxelMeshCacheFrame();
		bool currentVoxel = false;
		std::vector<SurfaceRef> authoritativeSurfaces;
		VoxelCaptureBudget budget = MakeVoxelCaptureBudget();
		for (auto* sprite : list.sprites)
		{
			if (sprite == nullptr || !IsOwnedByActor(*sprite, actorIndex))
			{
				continue;
			}

			stats.modelDrawItems++;

			if (sprite->modelframe > 0)
			{
				stats.unsupportedModelDrawItems++;
				continue;
			}

			if (sprite->modelframe >= 0 || sprite->voxel == nullptr || sprite->voxel->model == nullptr)
			{
				continue;
			}

			currentVoxel = true;
			authoritativeSurfaces.clear();
			(void)CaptureVoxelMeshSprite(nullptr, *sprite, drawListType, budget, authoritativeSurfaces, stats, DynamicVoxelCaptureMode::Authoritative);
			if (captureTransient && CaptureVoxelMeshSprite(nullptr, *sprite, drawListType, budget, outSprites, stats, DynamicVoxelCaptureMode::Transient))
			{
				stats.voxelProxyDrawItems++;
			}
		}
		EndVoxelMeshCacheFrame(rootMeshCapture);
		return currentVoxel;
	}

	void DiscardIndirectOnlyVoxelActorCacheEntry(int32_t actorIndex)
	{
		if (actorIndex < 0)
		{
			return;
		}

		for (auto it = gVoxelActorCache.begin(); it != gVoxelActorCache.end();)
		{
			if (it->second.actorIndex != actorIndex || !it->second.indirectOnly)
			{
				++it;
				continue;
			}
			EmitVoxelActorStateTrace(nullptr, nullptr, &it->second, "remove-indirect-representation-change", VoxelActorPendingReason::ActorNotLive);
			it = gVoxelActorCache.erase(it);
			++gVoxelActorCacheSerial;
		}
	}
}

namespace nri_scene
{
const char* GetDynamicVoxelEscapeReasonName(DynamicVoxelEscapeReason reason)
{
	switch (reason)
	{
	case DynamicVoxelEscapeReason::VariantPending: return "variant-pending";
	case DynamicVoxelEscapeReason::MaterialPending: return "material-pending";
	case DynamicVoxelEscapeReason::ActorBudget: return "actor-budget";
	case DynamicVoxelEscapeReason::BuildBudget: return "build-budget";
	case DynamicVoxelEscapeReason::UnsupportedTransform: return "unsupported-transform";
	case DynamicVoxelEscapeReason::NonLocalSpace: return "non-local-space";
	case DynamicVoxelEscapeReason::CameraOrWeaponSpecial: return "camera-or-weapon-special";
	case DynamicVoxelEscapeReason::LifecycleTransient: return "lifecycle-transient";
	case DynamicVoxelEscapeReason::NotCacheable: return "not-cacheable";
	case DynamicVoxelEscapeReason::ValidationQuarantine: return "validation-quarantine";
	case DynamicVoxelEscapeReason::FallbackDisabled: return "fallback-disabled";
	case DynamicVoxelEscapeReason::MissingSurface: return "missing-surface";
	default: return "unknown";
	}
}

void ResetAverageTextureColorCache()
{
	gFrameLocalAverageTextureColorCache.clear();
	gSkyInspectionCache.clear();
}

void ResetSkyPerfStats()
{
	gSkyPerfStats = {};
}

SkyPerfStats ConsumeSkyPerfStats()
{
	SkyPerfStats stats = gSkyPerfStats;
	gSkyPerfStats = {};
	return stats;
}

DynamicCapturePerfStats ConsumeDynamicCapturePerfStats()
{
	DynamicCapturePerfStats stats = gDynamicCapturePerfStats;
	gDynamicCapturePerfStats = {};
	return stats;
}

bool PrecacheVoxelModelCpuMesh(FVoxelModel* model, VoxelMeshPrecacheStats* stats)
{
	VoxelMeshPrecacheStats delta = {};
	delta.modelCandidates = 1;

	if (!r_voxels || model == nullptr)
	{
		delta.meshSkipped = 1;
		if ((int)nri_ptloadingtrace >= 2)
		{
			Printf("NRI PT loading voxel mesh: event=skip source=model model=%p reason=%s\n",
				model,
				r_voxels ? "null-model" : "voxels-disabled");
		}
		RecordVoxelMeshPrecacheStats(delta, stats);
		return true;
	}

	auto found = gVoxelMeshCache.find(model);
	if (found != gVoxelMeshCache.end())
	{
		if (found->second.built && found->second.valid)
		{
			delta.meshHits = 1;
			delta.vertices = found->second.mesh.vertices.Size();
			delta.indices = found->second.mesh.indices.Size();
			delta.primitives = delta.indices / 3;
			if ((int)nri_ptloadingtrace >= 2)
			{
				Printf("NRI PT loading voxel mesh: event=hit model=%p vertices=%u indices=%u tris=%u\n",
					model,
					delta.vertices,
					delta.indices,
					delta.primitives);
			}
			RecordVoxelMeshPrecacheStats(delta, stats);
			return true;
		}

		delta.meshInvalid = 1;
		RecordVoxelMeshPrecacheStats(delta, stats);
		return false;
	}

	VoxelMeshCacheEntry entry = {};
	const auto start = std::chrono::steady_clock::now();
	model->BuildCpuMesh(entry.mesh);
	const auto end = std::chrono::steady_clock::now();
	entry.built = true;
	entry.valid = entry.mesh.vertices.Size() > 0 && entry.mesh.indices.Size() >= 3;

	delta.meshBuilds = 1;
	delta.buildMs = DurationMs(start, end);
	delta.vertices = entry.mesh.vertices.Size();
	delta.indices = entry.mesh.indices.Size();
	delta.primitives = delta.indices / 3;
	if (!entry.valid)
	{
		delta.meshInvalid = 1;
	}

	if ((int)nri_ptloadingtrace >= 2)
	{
		Printf("NRI PT loading voxel mesh: event=build model=%p valid=%s vertices=%u indices=%u tris=%u ms=%.3f\n",
			model,
			entry.valid ? "true" : "false",
			delta.vertices,
			delta.indices,
			delta.primitives,
			delta.buildMs);
	}

	gVoxelMeshCache.emplace(model, std::move(entry));
	RecordVoxelMeshPrecacheStats(delta, stats);
	return delta.meshInvalid == 0;
}

bool PrecacheVoxelTextureCpuMesh(FTextureID texid, VoxelMeshPrecacheStats* stats)
{
	VoxelMeshPrecacheStats delta = {};
	delta.textureCandidates = 1;

	if (!r_voxels || !texid.isValid())
	{
		delta.meshSkipped = 1;
		if ((int)nri_ptloadingtrace >= 2)
		{
			Printf("NRI PT loading voxel mesh: event=skip source=texture tex=%d voxel=-1 reason=%s\n",
				texid.isValid() ? texid.GetIndex() : -1,
				r_voxels ? "invalid-texture" : "voxels-disabled");
		}
		RecordVoxelMeshPrecacheStats(delta, stats);
		return true;
	}

	int voxelIndex = -1;
	FVoxelModel* model = ResolveVoxelTextureModel(texid, &voxelIndex);
	if (model == nullptr)
	{
		delta.meshSkipped = 1;
		if ((int)nri_ptloadingtrace >= 2)
		{
			Printf("NRI PT loading voxel mesh: event=skip source=texture tex=%d voxel=%d reason=no-voxel-model\n",
				texid.isValid() ? texid.GetIndex() : -1,
				voxelIndex);
		}
		RecordVoxelMeshPrecacheStats(delta, stats);
		return true;
	}

	RecordVoxelMeshPrecacheStats(delta, stats);
	const bool meshReady = PrecacheVoxelModelCpuMesh(model, stats);
	if (!meshReady)
	{
		return false;
	}

	const VoxelMeshVariantKey meshVariantKey = BuildLoadingVoxelMeshVariantKey(texid, model, voxelIndex);
	const uint64_t meshVariantHash = BuildVoxelMeshVariantKeyHash(meshVariantKey);
	auto foundMesh = gVoxelMeshCache.find(model);
	if (meshVariantHash == 0 || foundMesh == gVoxelMeshCache.end() || !foundMesh->second.built || !foundMesh->second.valid)
	{
		return meshReady;
	}

	VoxelMeshPrecacheStats variantDelta = {};
	variantDelta.meshVariantCandidates = 1;
	VoxelActorCacheLookup lookup = {};
	lookup.meshVariantHash = meshVariantHash;
	lookup.sourcePicnum = texid.isValid() ? texid.GetIndex() : -1;
	lookup.resolvedVoxelIndex = voxelIndex;
	const bool hadVariantSurface = IsVoxelMeshVariantSurfaceReady(meshVariantHash);
	const SurfaceRef* surface = GetCachedVoxelMeshVariantSurface(lookup, foundMesh->second.mesh, false);
	if (hadVariantSurface)
	{
		variantDelta.meshVariantHits = 1;
	}
	else if (surface != nullptr)
	{
		variantDelta.meshVariantBuilds = 1;
	}
	else
	{
		variantDelta.meshVariantInvalid = 1;
	}
	variantDelta.variantPrimitives = surface != nullptr ? CountSurfacePrimitives(*surface) : 0u;
	if ((int)nri_ptloadingtrace >= 2)
	{
		Printf("NRI PT loading voxel variant: event=%s source=texture tex=%d voxel=%d mesh_variant=0x%llx transform_keyed=0 tris=%u\n",
			hadVariantSurface ? "hit" : (surface != nullptr ? "build" : "invalid"),
			texid.isValid() ? texid.GetIndex() : -1,
			voxelIndex,
			(unsigned long long)meshVariantHash,
			variantDelta.variantPrimitives);
	}
	RecordVoxelMeshPrecacheStats(variantDelta, stats);
	return meshReady;
}

void PreloadLiveActorVoxelRawSources()
{
	if (!r_voxels || !nri_ptvoxelcomputerawpreload || (int)nri_ptloadingvoxelactors <= 0)
	{
		if ((int)nri_ptloadingtrace >= 1 && nri_ptvoxelcomputerawpreload)
		{
			Printf("NRI PT loading voxel raw source preload: discovered=0 unique=0 selected=0 recorded=0 resident=0 skipped=0 failed=0 slabs=0 color_runs=0 raw_bytes=0 upload_bytes=0 ms=0.000 reason=%s\n",
				!r_voxels ? "voxels-disabled" : "loading-disabled");
		}
		return;
	}

	LoadingVoxelPreloadRequestGraph graph;
	BuildLiveActorVoxelPreloadRequestGraph(graph);
	const uint32_t sourceLimit =
		(int)nri_ptvoxelcomputerawpreloadmaxsources <= 0 ? UINT32_MAX : (uint32_t)(int)nri_ptvoxelcomputerawpreloadmaxsources;
	const uint64_t byteLimit =
		(int)nri_ptvoxelcomputerawpreloadmaxbytes <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxelcomputerawpreloadmaxbytes;
	uint32_t selected = 0;
	uint32_t duplicateSkipped = 0;
	uint32_t budgetSkipped = 0;
	uint64_t uploadBytes = 0;
	std::unordered_set<FVoxelModel*> seenModels;
	NRIVoxelComputeRawSourcePreloadStats stats = {};
	const auto start = std::chrono::steady_clock::now();
	for (const LoadingVoxelPreloadRequest& request : graph.requests)
	{
		if (request.model == nullptr)
		{
			continue;
		}
		if (!seenModels.insert(request.model).second)
		{
			duplicateSkipped++;
			continue;
		}
		if (selected >= sourceLimit)
		{
			budgetSkipped++;
			continue;
		}
		if (byteLimit != 0 && uploadBytes >= byteLimit)
		{
			budgetSkipped++;
			continue;
		}

		NRIVoxelComputeRawSourcePreloadStats delta = {};
		if (PreloadNRIVoxelComputeRawSource(request.model, &delta))
		{
			++selected;
			uploadBytes += delta.uploadBytes;
		}
		stats.requested += delta.requested;
		stats.recorded += delta.recorded;
		stats.alreadyResident += delta.alreadyResident;
		stats.skipped += delta.skipped;
		stats.failed += delta.failed;
		stats.slabRecords += delta.slabRecords;
		stats.colorRunRecords += delta.colorRunRecords;
		stats.rawBytes += delta.rawBytes;
		stats.uploadBytes += delta.uploadBytes;
		stats.buildMs += delta.buildMs;
	}

	if ((int)nri_ptloadingtrace >= 1 || ShouldTraceNRIVoxelComputeMeshing())
	{
		Printf("NRI PT loading voxel raw source preload: discovered=%u unique=%u selected=%u recorded=%u resident=%u skipped=%u duplicate_skipped=%u budget_skipped=%u failed=%u slabs=%u color_runs=%u raw_bytes=%llu upload_bytes=%llu ms=%.3f source_limit=%u byte_limit=%llu\n",
			graph.discovered,
			(uint32_t)seenModels.size(),
			selected,
			stats.recorded,
			stats.alreadyResident,
			stats.skipped,
			duplicateSkipped,
			budgetSkipped,
			stats.failed,
			stats.slabRecords,
			stats.colorRunRecords,
			(unsigned long long)stats.rawBytes,
			(unsigned long long)stats.uploadBytes,
			DurationMs(start, std::chrono::steady_clock::now()),
			sourceLimit,
			(unsigned long long)byteLimit);
	}
}

void PrecacheLiveActorVoxelMeshes(VoxelMeshPrecacheStats* stats)
{
	if (!r_voxels || !nri_ptloadingvoxelcpu || (int)nri_ptloadingvoxelactors <= 0)
	{
		return;
	}

	LoadingVoxelPreloadRequestGraph graph;
	BuildLiveActorVoxelPreloadRequestGraph(graph);
	if (graph.actorCandidates != 0)
	{
		VoxelMeshPrecacheStats actorDelta = {};
		actorDelta.actorCandidates = graph.actorCandidates;
		RecordVoxelMeshPrecacheStats(actorDelta, stats);
	}

	const LoadingVoxelCpuBudget cpuBudget = GetLoadingVoxelCpuBudget();
	const uint32_t variantLimit = cpuBudget.variantLimit;
	const uint32_t primitiveLimit = cpuBudget.primitiveLimit;
	const int timeLimitMs = cpuBudget.timeLimitMs;
	const auto start = std::chrono::steady_clock::now();
	uint32_t warmedVariants = 0;
	uint32_t primitiveTotal = 0;
	uint32_t skippedBudget = 0;
	uint32_t forceRequests = 0;
	uint32_t highRequests = 0;
	uint32_t normalRequests = 0;
	uint32_t opportunisticRequests = 0;
	uint32_t gpuEligibleRequests = 0;
	std::unordered_set<uint64_t> seenMeshVariants;
	for (const LoadingVoxelPreloadRequest& request : graph.requests)
	{
		if (IsLoadingVoxelRequestGpuCandidate(request))
		{
			gpuEligibleRequests++;
		}
		switch (request.priority)
		{
		case LoadingVoxelRequestPriority::Force: forceRequests++; break;
		case LoadingVoxelRequestPriority::High: highRequests++; break;
		case LoadingVoxelRequestPriority::Normal: normalRequests++; break;
		case LoadingVoxelRequestPriority::Opportunistic: opportunisticRequests++; break;
		default: break;
		}

		if ((uint8_t)request.priority > (uint8_t)LoadingVoxelRequestPriority::Force &&
			(warmedVariants >= variantLimit || (primitiveLimit != 0 && primitiveTotal >= primitiveLimit)))
		{
			skippedBudget++;
			if ((int)nri_ptloadingtrace >= 2)
			{
				const std::string sourceName = LoadingVoxelSourceBitsName(request.sourceBits);
				Printf("NRI PT loading voxel request: source=%s priority=%s actor=%d tex=%d voxel=%d mesh_variant=0x%llx tris=%u action=defer reason=cpu-budget variants=%u/%u prims=%u/%u\n",
					sourceName.c_str(),
					LoadingVoxelPriorityName(request.priority),
					request.actorIndex,
					request.texid.GetIndex(),
					request.resolvedVoxelIndex,
					(unsigned long long)request.meshVariantHash,
					request.primitiveCount,
					warmedVariants,
					variantLimit,
					primitiveTotal,
					primitiveLimit);
			}
			continue;
		}

		if ((uint8_t)request.priority > (uint8_t)LoadingVoxelRequestPriority::Force &&
			timeLimitMs > 0 &&
			DurationMs(start, std::chrono::steady_clock::now()) >= (double)timeLimitMs)
		{
			skippedBudget++;
			if ((int)nri_ptloadingtrace >= 2)
			{
				const std::string sourceName = LoadingVoxelSourceBitsName(request.sourceBits);
				Printf("NRI PT loading voxel request: source=%s priority=%s actor=%d tex=%d voxel=%d mesh_variant=0x%llx tris=%u action=defer reason=cpu-time ms=%.3f limit=%d\n",
					sourceName.c_str(),
					LoadingVoxelPriorityName(request.priority),
					request.actorIndex,
					request.texid.GetIndex(),
					request.resolvedVoxelIndex,
					(unsigned long long)request.meshVariantHash,
					request.primitiveCount,
					DurationMs(start, std::chrono::steady_clock::now()),
					timeLimitMs);
			}
			continue;
		}

		if (request.meshVariantHash != 0 && !seenMeshVariants.insert(request.meshVariantHash).second)
		{
			continue;
		}

		const bool hadVariantSurface = IsVoxelMeshVariantSurfaceReady(request.meshVariantHash);
		if ((int)nri_ptloadingtrace >= 2)
		{
			const std::string sourceName = LoadingVoxelSourceBitsName(request.sourceBits);
			Printf("NRI PT loading voxel request: source=%s priority=%s actor=%d tex=%d voxel=%d mesh_variant=0x%llx tris=%u action=%s reason=selected\n",
				sourceName.c_str(),
				LoadingVoxelPriorityName(request.priority),
				request.actorIndex,
				request.texid.GetIndex(),
				request.resolvedVoxelIndex,
				(unsigned long long)request.meshVariantHash,
				request.primitiveCount,
				hadVariantSurface ? "cpu-hit" : "cpu-build");
		}

		PrecacheVoxelTextureCpuMesh(request.texid, stats);

		uint32_t primitiveCount = 0;
		auto foundMesh = gVoxelMeshCache.find(request.model);
		if (foundMesh != gVoxelMeshCache.end() && foundMesh->second.built && foundMesh->second.valid)
		{
			VoxelActorCacheLookup lookup = {};
			lookup.meshVariantHash = request.meshVariantHash;
			lookup.sourcePicnum = request.texid.GetIndex();
			lookup.resolvedVoxelIndex = request.resolvedVoxelIndex;
			const SurfaceRef* surface = GetCachedVoxelMeshVariantSurface(lookup, foundMesh->second.mesh, false);
			primitiveCount = surface != nullptr ? CountSurfacePrimitives(*surface) : 0u;
		}
		primitiveTotal += primitiveCount;
		++warmedVariants;
	}

	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading voxel requests: discovered=%u unique=%u force=%u high=%u normal=%u opportunistic=%u cpu_selected=%u gpu_candidates=%u skipped_budget=%u skipped_invalid=%u skipped_duplicate=%u prims=%u ms=%.3f cpu_budget=%d max_variants=%u max_prims=%u max_ms=%d\n",
			graph.discovered,
			(uint32_t)graph.requests.size(),
			forceRequests,
			highRequests,
			normalRequests,
			opportunisticRequests,
			warmedVariants,
			gpuEligibleRequests,
			skippedBudget,
			graph.skippedInvalid,
			graph.skippedDuplicate,
			primitiveTotal,
			DurationMs(start, std::chrono::steady_clock::now()),
			cpuBudget.mode,
			variantLimit,
			primitiveLimit,
			timeLimitMs);
	}
}

void PrintAndResetLoadingWarmupStats(const char* phase)
{
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading warmup: phase=%s r_voxels=%u textures=%u actors=%u models=%u mesh_variants=%u mesh_hits=%u mesh_builds=%u mesh_invalid=%u mesh_skipped=%u variant_hits=%u variant_builds=%u variant_invalid=%u vertices=%u indices=%u tris=%u variant_tris=%u build_ms=%.3f\n",
			phase != nullptr ? phase : "unknown",
			r_voxels ? 1u : 0u,
			gVoxelLoadingWarmupStats.textureCandidates,
			gVoxelLoadingWarmupStats.actorCandidates,
			gVoxelLoadingWarmupStats.modelCandidates,
			gVoxelLoadingWarmupStats.meshVariantCandidates,
			gVoxelLoadingWarmupStats.meshHits,
			gVoxelLoadingWarmupStats.meshBuilds,
			gVoxelLoadingWarmupStats.meshInvalid,
			gVoxelLoadingWarmupStats.meshSkipped,
			gVoxelLoadingWarmupStats.meshVariantHits,
			gVoxelLoadingWarmupStats.meshVariantBuilds,
			gVoxelLoadingWarmupStats.meshVariantInvalid,
			gVoxelLoadingWarmupStats.vertices,
			gVoxelLoadingWarmupStats.indices,
			gVoxelLoadingWarmupStats.primitives,
			gVoxelLoadingWarmupStats.variantPrimitives,
			gVoxelLoadingWarmupStats.buildMs);
	}
	gVoxelLoadingWarmupStats = {};
}

void Copy3(const float* source, float* destination)
{
	destination[0] = source[0];
	destination[1] = source[1];
	destination[2] = source[2];
}

bool TryGetAverageTextureColor(FGameTexture* texture, float* outColor)
{
	__try
	{
		return TryGetAverageTextureColorRecursive(texture, outColor, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	return false;
	}
}

MaterialRef MakeMaterialRef(FGameTexture* texture, int palette, int shade, float alpha, uint32_t extraFlags)
{
	MaterialRef material = {};
	material.texture = texture;
	material.palette = palette;
	material.shade = shade;
	material.alpha = alpha;
	material.flags = extraFlags;

	if (texture != nullptr)
	{
		auto* baseTexture = texture->GetTexture();
		if (baseTexture != nullptr && baseTexture->GetImage() != nullptr && baseTexture->GetImage()->UseGamePalette())
		{
			material.flags |= MaterialFlag_Indexed;
		}

		if (texture->isFullbright())
		{
			material.flags |= MaterialFlag_Fullbright;
		}
	}

	return material;
}

void UpdateSceneSky(SceneView& outView, FGameTexture* texture, uint32_t fallbackColor, PTSkySourceType sourceType)
{
	if (ShouldTraceSkyPerf())
	{
		gSkyPerfStats.updateCalls++;
		switch (sourceType)
		{
		case PTSkySourceType::Wall:
			gSkyPerfStats.wallUpdateCalls++;
			break;
		case PTSkySourceType::Flat:
			gSkyPerfStats.flatUpdateCalls++;
			break;
		case PTSkySourceType::Portal:
			gSkyPerfStats.portalUpdateCalls++;
			break;
		default:
			break;
		}
	}
	ScopedSkyPerfTimer timer(gSkyPerfStats.updateTimeUs);
	SkyCandidate candidate = {};
	if (TryInspectSkyTexture(texture, fallbackColor, sourceType, candidate))
	{
		ApplySkyCandidate(outView, texture, candidate, sourceType);
	}
}

SceneDebugStats CollectDebugStats(HWDrawInfo& di)
{
	SceneDebugStats stats = {};

	stats.wallDrawItems =
		CountDrawListItems(di, GLDL_PLAINWALLS) +
		CountDrawListItems(di, GLDL_MASKEDWALLS) +
		CountDrawListItems(di, GLDL_MASKEDWALLSS) +
		CountDrawListItems(di, GLDL_MASKEDWALLSD) +
		CountDrawListItems(di, GLDL_MASKEDWALLSV) +
		CountDrawListItems(di, GLDL_MASKEDWALLSH) +
		CountDrawListItems(di, GLDL_TRANSLUCENTBORDER);

	stats.flatDrawItems =
		CountDrawListItems(di, GLDL_PLAINFLATS) +
		CountDrawListItems(di, GLDL_MASKEDFLATS) +
		CountDrawListItems(di, GLDL_MASKEDSLOPEFLATS);

	stats.spriteDrawItems = CountDrawListItems(di, GLDL_TRANSLUCENT) + CountDrawListItems(di, GLDL_MODELS);
	stats.modelDrawItems = CountDrawListItems(di, GLDL_MODELS);
	stats.translucentDrawItems = CountDrawListItems(di, GLDL_TRANSLUCENT);
	stats.totalDrawItems = stats.wallDrawItems + stats.flatDrawItems + stats.spriteDrawItems + stats.translucentDrawItems;
	stats.triangleEstimate = 0;
	stats.materialRefs = 0;
	return stats;
}

bool CaptureDynamicScene(HWDrawInfo& di, SceneView& outView, DynamicVoxelCaptureMode voxelCaptureMode)
{
	outView = {};
	outView.drawInfo = &di;
	gDynamicCapturePerfStats.calls++;
	const bool rootVoxelCacheFrame = [&]()
	{
		if (voxelCaptureMode != DynamicVoxelCaptureMode::Authoritative)
		{
			return false;
		}
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.voxelFrameMs);
		return BeginVoxelActorCacheFrame();
	}();
	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.countMs);
		outView.stats.wallDrawItems =
			CountDrawListItems(di, GLDL_MASKEDWALLSS) +
			CountDrawListItems(di, GLDL_MASKEDWALLSD) +
			CountDrawListItems(di, GLDL_MASKEDWALLSV) +
			CountDrawListItems(di, GLDL_MASKEDWALLSH);
		outView.stats.flatDrawItems =
			CountDrawListItems(di, GLDL_MASKEDFLATS) +
			CountDrawListItems(di, GLDL_MASKEDSLOPEFLATS);
		outView.stats.spriteDrawItems = CountDrawListItems(di, GLDL_TRANSLUCENT) + CountDrawListItems(di, GLDL_MODELS);
		outView.stats.translucentDrawItems = CountDrawListItems(di, GLDL_TRANSLUCENT);
		outView.stats.modelDrawItems = CountDrawListItems(di, GLDL_MODELS);
	}
	outView.stats.totalDrawItems = outView.stats.wallDrawItems + outView.stats.flatDrawItems + outView.stats.spriteDrawItems;

	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.wallsMs);
		CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSS], GLDL_MASKEDWALLSS, outView.opaqueWalls, outView.stats, outView);
		CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSD], GLDL_MASKEDWALLSD, outView.opaqueWalls, outView.stats, outView);
		CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSV], GLDL_MASKEDWALLSV, outView.opaqueWalls, outView.stats, outView);
		CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSH], GLDL_MASKEDWALLSH, outView.opaqueWalls, outView.stats, outView);
	}
	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.flatsMs);
		CaptureSpriteFlats(di, di.drawlists[GLDL_MASKEDFLATS], GLDL_MASKEDFLATS, outView.opaqueFlats);
		CaptureSpriteFlats(di, di.drawlists[GLDL_MASKEDSLOPEFLATS], GLDL_MASKEDSLOPEFLATS, outView.opaqueFlats);
	}
	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.facingSpritesMs);
		CaptureFacingSprites(di, di.drawlists[GLDL_TRANSLUCENT], GLDL_TRANSLUCENT, outView.opaqueSprites);
	}
	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.modelSpritesMs);
		CaptureModelSprites(di, di.drawlists[GLDL_MODELS], GLDL_MODELS, outView.opaqueSprites, outView.stats, voxelCaptureMode);
	}
	if (voxelCaptureMode == DynamicVoxelCaptureMode::Authoritative)
	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.voxelFrameMs);
		EndVoxelActorCacheFrame(outView.stats, rootVoxelCacheFrame);
	}

	{
		ScopedDynamicCaptureTimer timer(gDynamicCapturePerfStats.statsMs);
		for (const auto& wall : outView.opaqueWalls)
		{
			outView.stats.triangleEstimate += CountFanTriangles(wall);
			outView.stats.materialRefs++;
		}

		for (const auto& flat : outView.opaqueFlats)
		{
			outView.stats.triangleEstimate += CountTriangleListTriangles(flat);
			outView.stats.materialRefs++;
		}

		for (const auto& sprite : outView.opaqueSprites)
		{
			outView.stats.triangleEstimate += CountFanTriangles(sprite);
			outView.stats.materialRefs++;
		}
	}

	gDynamicCapturePerfStats.wallSurfaces += (uint32_t)outView.opaqueWalls.size();
	gDynamicCapturePerfStats.flatSurfaces += (uint32_t)outView.opaqueFlats.size();
	gDynamicCapturePerfStats.spriteSurfaces += (uint32_t)outView.opaqueSprites.size();
	gDynamicCapturePerfStats.voxelProxySurfaces += outView.stats.voxelProxyDrawItems;
	gDynamicCapturePerfStats.unsupportedModelSurfaces += outView.stats.unsupportedModelDrawItems;

	return !outView.opaqueWalls.empty() || !outView.opaqueFlats.empty() || !outView.opaqueSprites.empty();
}

ActorSpriteSceneCaptureResult CaptureActorSpriteScene(
	HWDrawInfo& di,
	int32_t actorIndex,
	bool residentVoxelReady,
	SceneView& outView)
{
	ActorSpriteSceneCaptureResult result = {};
	outView = {};
	outView.drawInfo = &di;
	std::vector<SurfaceRef> modelSprites;
	result.currentVoxel = CaptureActorModelSprites(
		di.drawlists[GLDL_MODELS],
		GLDL_MODELS,
		actorIndex,
		!residentVoxelReady,
		modelSprites,
		outView.stats);
	if (!result.currentVoxel)
	{
		DiscardIndirectOnlyVoxelActorCacheEntry(actorIndex);
	}
	if (!result.currentVoxel || !residentVoxelReady)
	{
		CaptureActorFacingSprites(di, di.drawlists[GLDL_TRANSLUCENT], GLDL_TRANSLUCENT, actorIndex, outView.opaqueSprites);
		outView.opaqueSprites.insert(
			outView.opaqueSprites.end(),
			std::make_move_iterator(modelSprites.begin()),
			std::make_move_iterator(modelSprites.end()));
	}

	outView.stats.spriteDrawItems = (uint32_t)outView.opaqueSprites.size();
	outView.stats.totalDrawItems = outView.stats.spriteDrawItems;
	for (const auto& sprite : outView.opaqueSprites)
	{
		outView.stats.triangleEstimate += CountFanTriangles(sprite);
		outView.stats.materialRefs++;
		if (sprite.provenance.sourceType != SurfaceSourceType::VoxelProxySprite)
		{
			outView.stats.translucentDrawItems++;
		}
	}

	result.capturedFallbackScene = !outView.opaqueSprites.empty();
	return result;
}

ActorSpriteSceneCaptureResult CaptureActorVoxelSprite(
	HWDrawInfo& di,
	HWSprite& sprite,
	bool residentVoxelReady,
	SceneView& outView)
{
	ActorSpriteSceneCaptureResult result = {};
	outView = {};
	outView.drawInfo = &di;
	if (sprite.modelframe >= 0 || sprite.voxel == nullptr || sprite.voxel->model == nullptr)
	{
		return result;
	}

	result.currentVoxel = true;
	const bool rootMeshCapture = BeginVoxelMeshCacheFrame();
	VoxelCaptureBudget budget = MakeVoxelCaptureBudget();
	std::vector<SurfaceRef> authoritativeSurfaces;
	(void)CaptureVoxelMeshSprite(
		nullptr,
		sprite,
		GLDL_MODELS,
		budget,
		authoritativeSurfaces,
		outView.stats,
		DynamicVoxelCaptureMode::Authoritative);
	if (!residentVoxelReady && CaptureVoxelMeshSprite(
		nullptr,
		sprite,
		GLDL_MODELS,
		budget,
		outView.opaqueSprites,
		outView.stats,
		DynamicVoxelCaptureMode::Transient))
	{
		outView.stats.voxelProxyDrawItems++;
	}
	EndVoxelMeshCacheFrame(rootMeshCapture);

	outView.stats.modelDrawItems = 1;
	outView.stats.spriteDrawItems = (uint32_t)outView.opaqueSprites.size();
	outView.stats.totalDrawItems = outView.stats.spriteDrawItems;
	for (const SurfaceRef& surface : outView.opaqueSprites)
	{
		outView.stats.triangleEstimate += CountFanTriangles(surface);
		outView.stats.materialRefs++;
	}
	result.capturedFallbackScene = !outView.opaqueSprites.empty();
	return result;
}

uint64_t GetPersistentVoxelCacheSerial()
{
	return gVoxelActorCacheSerial;
}

void ResetPersistentVoxelActorCache(const char* reason)
{
	const uint32_t entries = (uint32_t)gVoxelActorCache.size();
	if (!gVoxelActorCache.empty())
	{
		gVoxelActorCache.clear();
		++gVoxelActorCacheSerial;
	}
	gVoxelActorCacheCaptureDepth = 0;
	gVoxelActorLifecycleCursor = GetActorLifecycleJournal().LatestSerial();
	gVoxelActorLifecycleEvents.clear();
	gVoxelActorPendingRemoval = false;
	gVoxelActorMaintenanceGate.Reset();
	if ((int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats)
	{
		Printf("NRI PT voxel actor cache reset: reason=%s entries=%u serial=%llu frame=%llu\n",
			reason != nullptr && *reason != '\0' ? reason : "unspecified",
			entries,
			(unsigned long long)gVoxelActorCacheSerial,
			(unsigned long long)gVoxelActorCacheFrame);
	}
}

void SetPersistentVoxelActorStartupTransientMode(bool active, const char* reason)
{
	if (gVoxelActorStartupTransientMode == active)
	{
		return;
	}

	gVoxelActorStartupTransientMode = active;
	++gVoxelActorCacheSerial;
	uint32_t promotedEntries = 0;
	if (!active)
	{
		for (auto& pair : gVoxelActorCache)
		{
			VoxelActorCacheEntry& entry = pair.second;
			if (!entry.startupPending)
			{
				continue;
			}
			entry.startupPending = false;
			if (!entry.persistentReady && CanPromoteVoxelActorCacheEntry(entry))
			{
				entry.persistentReady = true;
			}
			++promotedEntries;
		}
		if (promotedEntries > 0)
		{
			++gVoxelActorCacheSerial;
		}
	}
	if ((int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats)
	{
		Printf("NRI PT voxel actor startup transient: active=%u reason=%s serial=%llu frame=%llu entries=%u promoted=%u\n",
			active ? 1u : 0u,
			reason != nullptr && *reason != '\0' ? reason : "unspecified",
			(unsigned long long)gVoxelActorCacheSerial,
			(unsigned long long)gVoxelActorCacheFrame,
			(uint32_t)gVoxelActorCache.size(),
			promotedEntries);
	}
}

bool BuildPersistentVoxelCacheSceneView(SceneView& outView)
{
	outView = {};
	std::vector<PersistentVoxelCacheEntryView> entries;
	if (!BuildPersistentVoxelCacheEntries(entries))
	{
		return false;
	}

	outView.opaqueSprites.reserve(entries.size());
	outView.stats.voxelCacheEntries = (unsigned int)entries.size();
	for (const auto& entry : entries)
	{
		if (entry.surface == nullptr)
		{
			continue;
		}
		outView.opaqueSprites.push_back(*entry.surface);
		outView.stats.triangleEstimate += entry.primitiveCount;
		outView.stats.voxelCachePrimitives += entry.primitiveCount;
		outView.stats.materialRefs++;
	}

	outView.stats.voxelCacheEntries = (unsigned int)outView.opaqueSprites.size();
	outView.stats.spriteDrawItems = (unsigned int)outView.opaqueSprites.size();
	outView.stats.modelDrawItems = outView.stats.spriteDrawItems;
	outView.stats.voxelProxyDrawItems = outView.stats.spriteDrawItems;
	outView.stats.totalDrawItems = outView.stats.spriteDrawItems;
	return true;
}

bool BuildPersistentVoxelCacheEntries(std::vector<PersistentVoxelCacheEntryView>& outEntries)
{
	outEntries.clear();
	if (gVoxelActorStartupTransientMode)
	{
		return false;
	}
	if (gVoxelActorCache.empty())
	{
		return false;
	}

	std::vector<std::pair<uint64_t, const VoxelActorCacheEntry*>> sortedEntries;
	sortedEntries.reserve(gVoxelActorCache.size());
	for (const auto& pair : gVoxelActorCache)
	{
		const bool publicationCurrent =
			pair.second.ownerWorldEpoch != 0 &&
			pair.second.placementGeneration != 0 &&
			pair.second.placementStateHash != 0 &&
			pair.second.physicalSectorIndex >= 0 &&
			pair.second.authorityCurrent &&
			pair.second.publicationEligible &&
			!pair.second.pendingRemoval;
		if (!publicationCurrent)
		{
			continue;
		}
		const bool currentReady = pair.second.hasSurface && pair.second.persistentReady && !pair.second.startupPending;
		const bool desiredDirectPending =
			ShouldDirectPublishNRIVoxelComputeMeshing() &&
			pair.second.pendingReason != (uint8_t)VoxelActorPendingReason::None &&
			pair.second.hasDesiredMaterialSurface &&
			pair.second.desiredMeshKeyHash != 0 &&
			pair.second.desiredMaterialKeyHash != 0 &&
			pair.second.desiredPrimitiveCount != 0 &&
			pair.second.voxelModelPtr != 0;
		if (currentReady || desiredDirectPending)
		{
			sortedEntries.emplace_back(pair.first, &pair.second);
		}
	}

	if (sortedEntries.empty())
	{
		return false;
	}

	std::sort(sortedEntries.begin(), sortedEntries.end(), [](const auto& a, const auto& b)
	{
		return a.first < b.first;
	});

	outEntries.reserve(sortedEntries.size());
	for (const auto& entry : sortedEntries)
	{
		PersistentVoxelCacheEntryView view = {};
		view.identityKey = entry.first;
		view.ownerWorldEpoch = entry.second->ownerWorldEpoch;
		view.ownerLifetimeGeneration = entry.second->ownerLifetimeGeneration;
		view.placementGeneration = entry.second->placementGeneration;
		view.placementStateHash = entry.second->placementStateHash;
		const bool desiredDirectPending =
			ShouldDirectPublishNRIVoxelComputeMeshing() &&
			entry.second->pendingReason != (uint8_t)VoxelActorPendingReason::None &&
			entry.second->hasDesiredMaterialSurface &&
			entry.second->desiredMeshKeyHash != 0 &&
			entry.second->desiredMaterialKeyHash != 0 &&
			entry.second->desiredPrimitiveCount != 0 &&
			entry.second->voxelModelPtr != 0;
		view.desiredPending = desiredDirectPending;
		view.directOnlyAdmission = desiredDirectPending;
		view.signature = desiredDirectPending ? entry.second->desiredSignature : entry.second->signature;
		view.geometrySignature = desiredDirectPending ? entry.second->desiredMeshVariantHash : entry.second->geometrySignature;
		view.surfaceSignature = desiredDirectPending ? entry.second->desiredSurfaceSignature : entry.second->surfaceSignature;
		view.bakedSurfaceSignature = desiredDirectPending ?
			entry.second->desiredSurfaceSignature :
			(entry.second->bakedSurfaceSignature != 0 ? entry.second->bakedSurfaceSignature : entry.second->surfaceSignature);
		view.materialSignature = desiredDirectPending ? entry.second->desiredMaterialVariantHash : entry.second->materialSignature;
		view.transformBasisSignature = entry.second->transformBasisSignature;
		view.meshKeyHash = desiredDirectPending ? entry.second->desiredMeshKeyHash : entry.second->meshKeyHash;
		view.materialKeyHash = desiredDirectPending ? entry.second->desiredMaterialKeyHash : entry.second->materialKeyHash;
		uint64_t geometryContentHash = desiredDirectPending ? 0ull : entry.second->geometryContentHash;
		uint64_t renderPrimitiveHash = desiredDirectPending ? 0ull : entry.second->renderPrimitiveHash;
		if (desiredDirectPending)
		{
			GetReadyVoxelMeshVariantContentHashes(
				entry.second->desiredMeshVariantHash,
				geometryContentHash,
				renderPrimitiveHash);
			FVoxelRawMeshStats rawStats = {};
			if (QueryNRIVoxelComputeRawSourceArchiveStats(
				reinterpret_cast<FVoxelModel*>(entry.second->voxelModelPtr), rawStats))
			{
				const VoxelGeometryContentHashes rawHashes = BuildRawVoxelGeometryContentHashes(
					rawStats,
					entry.second->sourcePicnum,
					entry.second->resolvedVoxelIndex);
				geometryContentHash = rawHashes.geometryContentHash;
				renderPrimitiveHash = rawHashes.renderPrimitiveHash;
			}
		}
		else if ((geometryContentHash == 0 || renderPrimitiveHash == 0) && entry.second->sharedVariantSurface)
		{
			uint64_t cachedGeometryContentHash = 0;
			uint64_t cachedRenderPrimitiveHash = 0;
			if (GetReadyVoxelMeshVariantContentHashes(entry.second->meshVariantHash, cachedGeometryContentHash, cachedRenderPrimitiveHash))
			{
				if (geometryContentHash == 0)
				{
					geometryContentHash = cachedGeometryContentHash;
				}
				if (renderPrimitiveHash == 0)
				{
					renderPrimitiveHash = cachedRenderPrimitiveHash;
				}
			}
		}
		view.geometryContentHash = geometryContentHash;
		view.renderPrimitiveHash = renderPrimitiveHash;
		view.meshVariantHash = desiredDirectPending ? entry.second->desiredMeshVariantHash : entry.second->meshVariantHash;
		view.materialVariantHash = desiredDirectPending ? entry.second->desiredMaterialVariantHash : entry.second->materialVariantHash;
		view.meshBakeSpace = entry.second->meshBakeSpace;
		view.actorIndex = entry.second->actorIndex;
		view.physicalSectorIndex = entry.second->physicalSectorIndex;
		view.sourcePicnum = entry.second->sourcePicnum;
		view.resolvedVoxelIndex = entry.second->resolvedVoxelIndex;
		view.primitiveCount = desiredDirectPending ? entry.second->desiredPrimitiveCount : entry.second->primitiveCount;
		view.lastSeenFrame = entry.second->lastSeenFrame;
		view.capturedThisFrame = entry.second->lastSeenFrame == gVoxelActorCacheFrame;
		view.indirectOnly = entry.second->indirectOnly;
		view.authorityCurrent = entry.second->authorityCurrent;
		view.publicationEligible = entry.second->publicationEligible;
		view.pendingRemoval = entry.second->pendingRemoval;
		view.retainedFrameAge = entry.second->lastSeenFrame != 0 && gVoxelActorCacheFrame >= entry.second->lastSeenFrame ?
			gVoxelActorCacheFrame - entry.second->lastSeenFrame :
			0;
		if (entry.second->meshBakeSpace == VoxelMeshBakeSpace::LocalSpace)
		{
			std::copy(std::begin(entry.second->currentTransform), std::end(entry.second->currentTransform), std::begin(view.instanceTransform));
		}
		else
		{
			FillVoxelTranslationInstanceTransform(entry.second->currentTranslation, entry.second->bakedTranslation, view.instanceTransform);
		}
		view.currentTranslation[0] = entry.second->currentTranslation[0];
		view.currentTranslation[1] = entry.second->currentTranslation[1];
		view.currentTranslation[2] = entry.second->currentTranslation[2];
		view.bakedTranslation[0] = entry.second->bakedTranslation[0];
		view.bakedTranslation[1] = entry.second->bakedTranslation[1];
		view.bakedTranslation[2] = entry.second->bakedTranslation[2];
		view.model = reinterpret_cast<FVoxelModel*>(entry.second->voxelModelPtr);
		view.sharedVariantSurface = entry.second->sharedVariantSurface;
		view.surface = desiredDirectPending ? nullptr : (entry.second->sharedVariantSurface ?
			GetReadyVoxelMeshVariantSurface(entry.second->meshVariantHash) :
			&entry.second->surface);
		view.materialSurface = desiredDirectPending ? entry.second->desiredMaterialSurface : SurfaceRef{};
		if (!desiredDirectPending && view.surface == nullptr)
		{
			continue;
		}
		view.lightSurface = desiredDirectPending ? nullptr : &entry.second->lightSurface;
		outEntries.push_back(std::move(view));
	}

	return true;
}

bool GetPersistentVoxelActorAuthority(
	uint64_t identityKey,
	int32_t actorIndex,
	PersistentVoxelActorAuthorityView& outAuthority)
{
	outAuthority = {};
	outAuthority.identityKey = identityKey;
	outAuthority.actorIndex = actorIndex;
	if (identityKey == 0 || actorIndex < 0)
	{
		return false;
	}

	const auto entryIt = gVoxelActorCache.find(identityKey);
	if (entryIt == gVoxelActorCache.end())
	{
		return false;
	}

	const VoxelActorCacheEntry& entry = entryIt->second;
	outAuthority.ownerWorldEpoch = entry.ownerWorldEpoch;
	outAuthority.ownerLifetimeGeneration = entry.ownerLifetimeGeneration;
	outAuthority.placementGeneration = entry.placementGeneration;
	outAuthority.placementStateHash = entry.placementStateHash;
	outAuthority.physicalSectorIndex = entry.physicalSectorIndex;
	outAuthority.lastSeenFrame = entry.lastSeenFrame;
	outAuthority.retainedFrameAge = entry.lastSeenFrame != 0 && gVoxelActorCacheFrame >= entry.lastSeenFrame ?
		gVoxelActorCacheFrame - entry.lastSeenFrame : 0;
	outAuthority.pendingRemoval = entry.pendingRemoval;
	outAuthority.capturedThisFrame = entry.lastSeenFrame == gVoxelActorCacheFrame;
	const ActorPresentationOwnerKey owner =
	{
		entry.ownerWorldEpoch,
		(int64_t)entry.ownerLifetimeGeneration
	};
	const ActorPresentationState* actor = LookupActorPresentationByOwnerKey(owner);
	const bool identityCurrent = actor != nullptr &&
		entry.actorIndex == actorIndex &&
		actor->actorIndex == actorIndex &&
		BuildVoxelActorIdentityKey(actor->owner) == identityKey;
	outAuthority.identityCurrent = identityCurrent;
	outAuthority.lifecycleGeneration = identityCurrent ? entry.ownerLifetimeGeneration : 0;
	outAuthority.authorityCurrent = identityCurrent && actor->authorityCurrent;
	outAuthority.publicationEligible = identityCurrent &&
		IsActorPresentationVoxelCacheOwner(*actor) &&
		BuildActorPresentationBasisStateHash(*actor) == entry.presentationBasisStateHash;
	outAuthority.live = outAuthority.authorityCurrent && outAuthority.publicationEligible;
	if (!outAuthority.live)
	{
		return true;
	}

	// The retained-cache delta basis above is legacy engine XYZ. Diagnostics
	// compare spatial evidence and GPU hits in PT world space (X, -Z, -Y), the
	// same convention used by WorldToPathTracingPosition and sprite transforms.
	outAuthority.actorScenePosition[0] = (float)actor->position.x;
	outAuthority.actorScenePosition[1] = (float)-actor->position.z;
	outAuthority.actorScenePosition[2] = (float)-actor->position.y;
	outAuthority.physicalSectorIndex = actor->physicalSectorIndex;
	if (entry.hasLastActorScenePosition)
	{
		std::copy(
			std::begin(entry.lastActorScenePosition),
			std::end(entry.lastActorScenePosition),
			std::begin(outAuthority.cachedActorScenePosition));
		constexpr float PositionEpsilon = 0.001f;
		float liveCacheBasisPosition[3] = {};
		CopyActorPresentationCacheBasisPosition(*actor, liveCacheBasisPosition);
		outAuthority.actorPositionSynchronized =
			std::abs(liveCacheBasisPosition[0] - outAuthority.cachedActorScenePosition[0]) <= PositionEpsilon &&
			std::abs(liveCacheBasisPosition[1] - outAuthority.cachedActorScenePosition[1]) <= PositionEpsilon &&
			std::abs(liveCacheBasisPosition[2] - outAuthority.cachedActorScenePosition[2]) <= PositionEpsilon;
	}

	if (entry.meshBakeSpace == VoxelMeshBakeSpace::LocalSpace)
	{
		std::copy(
			std::begin(entry.currentTransform),
			std::end(entry.currentTransform),
			std::begin(outAuthority.authoritativeInstanceTransform));
	}
	else
	{
		FillVoxelTranslationInstanceTransform(
			entry.currentTranslation,
			entry.bakedTranslation,
			outAuthority.authoritativeInstanceTransform);
	}
	return true;
}

PersistentVoxelActorCacheStats GetPersistentVoxelActorCacheStats()
{
	PersistentVoxelActorCacheStats stats = {};
	stats.entries = (uint32_t)gVoxelActorCache.size();
	stats.serial = gVoxelActorCacheSerial;
	stats.frame = gVoxelActorCacheFrame;
	for (const auto& pair : gVoxelActorCache)
	{
		const VoxelActorCacheEntry& entry = pair.second;
		if (!entry.hasSurface || !entry.persistentReady)
		{
			continue;
		}
		++stats.readyEntries;
		stats.primitiveCount += entry.primitiveCount;
		if (entry.lastSeenFrame == gVoxelActorCacheFrame)
		{
			++stats.capturedThisFrame;
		}
	}
	return stats;
}

bool BuildPrecachedVoxelVariantViews(std::vector<PrecachedVoxelVariantView>& outEntries)
{
	outEntries.clear();
	if (!r_voxels || !nri_ptloadingvoxelcpu || !nri_ptloadingvoxelgpu || (int)nri_ptloadingvoxelactors <= 0)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading voxel gpu requests: discovered=0 unique=0 gpu_selected=0 skipped_source=0 skipped_cpu=0 skipped_budget=0 skipped_material=0 skipped_small=0 forced=0 preferred=0 heuristic=0 prims=0 upload_bytes=0 reason=%s\n",
				!r_voxels ? "voxels-disabled" : (!nri_ptloadingvoxelcpu ? "cpu-disabled" : (!nri_ptloadingvoxelgpu ? "gpu-disabled" : "loading-disabled")));
		}
		return false;
	}

	const LoadingVoxelGpuBudget gpuBudget = GetLoadingVoxelGpuBudget();
	const uint32_t variantLimit = gpuBudget.variantLimit;
	const uint32_t primitiveLimit = gpuBudget.primitiveLimit;
	const uint32_t minPrimitiveCount = gpuBudget.minPrimitiveCount;
	const uint64_t byteLimit = gpuBudget.byteLimit;
	uint32_t primitiveTotal = 0;
	uint64_t uploadByteTotal = 0;
	uint32_t skippedNotCpuReady = 0;
	uint32_t skippedGpuSource = 0;
	uint32_t skippedBudget = 0;
	uint32_t skippedMaterial = 0;
	uint32_t skippedSmall = 0;
	uint32_t selectedForced = 0;
	uint32_t selectedPreferred = 0;
	uint32_t selectedHeuristic = 0;
	std::unordered_set<uint64_t> seenVariantPairs;
	std::unordered_set<uint64_t> selectedMeshVariants;
	std::unordered_map<uint64_t, uint32_t> selectedGeometryHashes;
	std::unordered_map<uint64_t, uint32_t> selectedRenderPrimitiveHashes;
	uint32_t selectedGeometryHashMissing = 0;
	uint32_t selectedRenderPrimitiveHashMissing = 0;
	uint32_t selectedDuplicateGeometryMeshes = 0;
	uint32_t selectedDuplicateRenderPrimitiveMeshes = 0;

	LoadingVoxelPreloadRequestGraph graph;
	BuildLiveActorVoxelPreloadRequestGraph(graph);
	for (const LoadingVoxelPreloadRequest& request : graph.requests)
	{
		if (!IsLoadingVoxelRequestGpuCandidate(request))
		{
			skippedGpuSource++;
			if ((int)nri_ptloadingtrace >= 2)
			{
				const std::string sourceName = LoadingVoxelSourceBitsName(request.sourceBits);
				Printf("NRI PT loading voxel variant: event=defer reason=gpu-source source=%s priority=%s actor=%d tex=%d voxel=%d mesh_variant=0x%llx\n",
					sourceName.c_str(),
					LoadingVoxelPriorityName(request.priority),
					request.actorIndex,
					request.texid.GetIndex(),
					request.resolvedVoxelIndex,
					(unsigned long long)request.meshVariantHash);
			}
			continue;
		}

		FGameTexture* voxelTexture = TexMan.GetGameTexture(request.model->GetPaletteTexture());
		if (voxelTexture == nullptr || !voxelTexture->isValid())
		{
			skippedMaterial++;
			continue;
		}

		auto foundMesh = gVoxelMeshCache.find(request.model);
		if (foundMesh == gVoxelMeshCache.end() || !foundMesh->second.built || !foundMesh->second.valid)
		{
			skippedNotCpuReady++;
			continue;
		}

		VoxelActorCacheLookup lookup = {};
		lookup.meshVariantHash = request.meshVariantHash;
		lookup.sourcePicnum = request.texid.GetIndex();
		lookup.resolvedVoxelIndex = request.resolvedVoxelIndex;
		const SurfaceRef* surface = GetCachedVoxelMeshVariantSurface(lookup, foundMesh->second.mesh, false);
		if (surface == nullptr)
		{
			skippedNotCpuReady++;
			continue;
		}

		const uint32_t primitiveCount = CountSurfacePrimitives(*surface);
		uint64_t geometryContentHash = 0;
		uint64_t renderPrimitiveHash = 0;
		if (!GetReadyVoxelMeshVariantContentHashes(request.meshVariantHash, geometryContentHash, renderPrimitiveHash))
		{
			const VoxelGeometryContentHashes hashes = BuildVoxelGeometryContentHashes(*surface);
			geometryContentHash = hashes.geometryContentHash;
			renderPrimitiveHash = hashes.renderPrimitiveHash;
		}
		const uint64_t estimatedMeshUploadBytes =
			(uint64_t)surface->vertices.size() * (uint64_t)sizeof(SceneVertex) +
			(uint64_t)surface->indices.size() * (uint64_t)sizeof(uint32_t) +
			(uint64_t)primitiveCount * (uint64_t)sizeof(PrimitiveData);
		const bool explicitGpu =
			(request.sourceBits & LoadingVoxelRequestSource_MountedVoxelPreload) != 0 &&
			(request.gpuForce || request.gpuPrefer);
		const bool forcedGpu = explicitGpu && request.gpuForce;
		const bool preferredGpu = explicitGpu && request.gpuPrefer && !request.gpuForce;
		if (!forcedGpu && minPrimitiveCount != 0 && primitiveCount < minPrimitiveCount)
		{
			skippedSmall++;
			if ((int)nri_ptloadingtrace >= 2)
			{
				const std::string sourceName = LoadingVoxelSourceBitsName(request.sourceBits);
				Printf("NRI PT loading voxel variant: event=defer reason=preload-small source=%s priority=%s actor=%d tex=%d voxel=%d mesh_variant=0x%llx tris=%u min_tris=%u\n",
					sourceName.c_str(),
					LoadingVoxelPriorityName(request.priority),
					request.actorIndex,
					request.texid.GetIndex(),
					request.resolvedVoxelIndex,
					(unsigned long long)request.meshVariantHash,
					primitiveCount,
					minPrimitiveCount);
			}
			continue;
		}

		std::vector<LoadingVoxelMaterialContext> contexts = request.materialContexts;
		if (contexts.empty())
		{
			LoadingVoxelMaterialContext fallbackContext = {};
			fallbackContext.texid = request.texid;
			fallbackContext.actor = request.actor;
			fallbackContext.actorIndex = request.actorIndex;
			fallbackContext.sourceBits = request.sourceBits;
			fallbackContext.priority = request.priority;
			fallbackContext.gpuForce = request.gpuForce;
			fallbackContext.gpuPrefer = request.gpuPrefer;
			contexts.push_back(fallbackContext);
		}

		for (const LoadingVoxelMaterialContext& context : contexts)
		{
			FGameTexture* emissiveSourceTexture = TexMan.GetGameTexture(context.texid);
			if (emissiveSourceTexture != nullptr && !emissiveSourceTexture->isValid())
			{
				emissiveSourceTexture = nullptr;
			}
			const int palette = context.actor != nullptr ? context.actor->spr.pal : 0;
			const int shade = context.actor != nullptr ? context.actor->spr.shade : 0;
			const float alpha = context.actor != nullptr ? GetLoadingActorAlpha(context.actor) : 1.0f;
			const MaterialRef material = MakeVoxelPaletteMaterialRef(
				voxelTexture,
				emissiveSourceTexture,
				palette,
				shade,
				alpha,
				MaterialFlag_Sprite);
			const uint64_t materialVariantHash = BuildVoxelMaterialVariantKeyHash(BuildVoxelMaterialVariantKey(voxelTexture, material));
			const uint64_t pairHash = HashCombine64(request.meshVariantHash, materialVariantHash);
			if (materialVariantHash == 0 || !seenVariantPairs.insert(pairHash).second)
			{
				continue;
			}

			const bool meshBudgetFirstUse = selectedMeshVariants.find(request.meshVariantHash) == selectedMeshVariants.end();
			// Additional actor/material variants for an already selected mesh should not
			// be charged as if they re-upload the shared voxel geometry.
			const uint64_t estimatedMaterialVariantBytes = 512ull;
			const uint64_t estimatedUploadBytes = meshBudgetFirstUse ? estimatedMeshUploadBytes : estimatedMaterialVariantBytes;
			const uint32_t estimatedPrimitiveBudget = meshBudgetFirstUse ? primitiveCount : 0u;
			const bool exceedsVariantBudget = outEntries.size() >= variantLimit;
			const bool exceedsPrimitiveBudget = primitiveLimit != 0 && primitiveTotal + estimatedPrimitiveBudget > primitiveLimit;
			const bool exceedsByteBudget = byteLimit != 0 && uploadByteTotal + estimatedUploadBytes > byteLimit;
			if (exceedsVariantBudget || exceedsPrimitiveBudget || exceedsByteBudget)
			{
				skippedBudget++;
				if ((int)nri_ptloadingtrace >= 2)
				{
					const std::string sourceName = LoadingVoxelSourceBitsName(request.sourceBits | context.sourceBits);
					Printf("NRI PT loading voxel variant: event=defer reason=preload-budget source=%s priority=%s actor=%d tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx tris=%u prims_used=%u prims_limit=%u variants_used=%u variants_limit=%u bytes=%llu bytes_used=%llu bytes_limit=%llu mesh_reused=%u\n",
						sourceName.c_str(),
						LoadingVoxelPriorityName(request.priority),
						context.actorIndex,
						context.texid.GetIndex(),
						request.resolvedVoxelIndex,
						(unsigned long long)request.meshVariantHash,
						(unsigned long long)materialVariantHash,
						primitiveCount,
						primitiveTotal,
						primitiveLimit,
						(uint32_t)outEntries.size(),
						variantLimit,
						(unsigned long long)estimatedUploadBytes,
						(unsigned long long)uploadByteTotal,
						(unsigned long long)byteLimit,
						meshBudgetFirstUse ? 0u : 1u);
				}
				continue;
			}

			PrecachedVoxelVariantView view = {};
			view.meshKeyHash = request.meshVariantHash;
			view.materialKeyHash = materialVariantHash;
			view.geometrySignature = request.meshVariantHash;
			view.geometryContentHash = geometryContentHash;
			view.renderPrimitiveHash = renderPrimitiveHash;
			view.meshVariantHash = request.meshVariantHash;
			view.materialVariantHash = materialVariantHash;
			view.sourceBits = request.sourceBits | context.sourceBits;
			view.priority = (int32_t)request.priority;
			view.admissionRank = LoadingVoxelAdmissionRank(request);
			view.sourcePicnum = context.texid.GetIndex();
			view.resolvedVoxelIndex = request.resolvedVoxelIndex;
			view.primitiveCount = primitiveCount;
			view.gpuForce = forcedGpu;
			view.gpuPrefer = preferredGpu;
			view.model = request.model;
			view.surface = surface;
			view.material = material;
			outEntries.push_back(std::move(view));
			if (meshBudgetFirstUse)
			{
				selectedMeshVariants.insert(request.meshVariantHash);
				if (geometryContentHash == 0)
				{
					selectedGeometryHashMissing++;
				}
				else if (++selectedGeometryHashes[geometryContentHash] > 1)
				{
					selectedDuplicateGeometryMeshes++;
				}
				if (renderPrimitiveHash == 0)
				{
					selectedRenderPrimitiveHashMissing++;
				}
				else if (++selectedRenderPrimitiveHashes[renderPrimitiveHash] > 1)
				{
					selectedDuplicateRenderPrimitiveMeshes++;
				}
				primitiveTotal += primitiveCount;
			}
			uploadByteTotal += estimatedUploadBytes;
			if (forcedGpu)
			{
				selectedForced++;
			}
			else if (preferredGpu)
			{
				selectedPreferred++;
			}
			else
			{
				selectedHeuristic++;
			}
		}
	}

	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading voxel gpu requests: discovered=%u unique=%u gpu_selected=%u skipped_source=%u skipped_cpu=%u skipped_budget=%u skipped_material=%u skipped_small=%u forced=%u preferred=%u heuristic=%u prims=%u upload_bytes=%llu whitelist_only=%u gpu_budget=%d min_prims=%u max_prims=%u max_bytes=%llu max_variants=%u\n",
			graph.discovered,
			(uint32_t)graph.requests.size(),
			(uint32_t)outEntries.size(),
			skippedGpuSource,
			skippedNotCpuReady,
			skippedBudget,
			skippedMaterial,
			skippedSmall,
			selectedForced,
			selectedPreferred,
			selectedHeuristic,
			primitiveTotal,
			(unsigned long long)uploadByteTotal,
			nri_ptloadingvoxelgpuwhitelistonly ? 1u : 0u,
			gpuBudget.mode,
			minPrimitiveCount,
			primitiveLimit,
			(unsigned long long)byteLimit,
			variantLimit);
		Printf("NRI PT voxel geometry dedupe: mesh_keys=%u geometry_hashes=%u render_primitive_hashes=%u geometry_hash_missing=%u render_primitive_hash_missing=%u duplicate_geometry_meshes=%u duplicate_render_primitive_meshes=%u hash_scope=picnum-independent\n",
			(uint32_t)selectedMeshVariants.size(),
			(uint32_t)selectedGeometryHashes.size(),
			(uint32_t)selectedRenderPrimitiveHashes.size(),
			selectedGeometryHashMissing,
			selectedRenderPrimitiveHashMissing,
			selectedDuplicateGeometryMeshes,
			selectedDuplicateRenderPrimitiveMeshes);
	}

	return !outEntries.empty();
}

bool BuildPrecachedVoxelRawManifestViews(std::vector<PrecachedVoxelRawManifestView>& outEntries, PrecachedVoxelRawManifestStats* outStats)
{
	outEntries.clear();
	if (outStats != nullptr)
	{
		*outStats = {};
	}
	if (!r_voxels || (int)nri_ptloadingvoxelactors <= 0)
	{
		return false;
	}

	LoadingVoxelPreloadRequestGraph graph;
	BuildLiveActorVoxelPreloadRequestGraph(graph);
	if (outStats != nullptr)
	{
		outStats->discovered = graph.discovered;
		outStats->uniqueRequests = (uint32_t)graph.requests.size();
		outStats->actorCandidates = graph.actorCandidates;
		outStats->skippedInvalid = graph.skippedInvalid;
		outStats->skippedDuplicate = graph.skippedDuplicate;
		outStats->manifestSources = graph.manifestSources;
		outStats->manifestLines = graph.manifestLines;
		outStats->manifestRequests = graph.manifestRequests;
		outStats->manifestSkippedInactive = graph.manifestSkippedInactive;
		outStats->manifestSkippedSyntax = graph.manifestSkippedSyntax;
		outStats->manifestSkippedActor = graph.manifestSkippedActor;
		outStats->manifestSkippedUnsupported = graph.manifestSkippedUnsupported;
	}

	std::unordered_set<uint64_t> seenVariantPairs;
	seenVariantPairs.reserve(graph.requests.size());
	for (const LoadingVoxelPreloadRequest& request : graph.requests)
	{
		if (request.model == nullptr || request.meshVariantHash == 0)
		{
			continue;
		}

		FVoxelRawMeshStats rawStats = {};
		const bool rawStatsReady = QueryNRIVoxelComputeRawSourceArchiveStats(request.model, rawStats);
		const uint32_t primitiveCount = rawStatsReady ? rawStats.adjacencySurfaceFaceCount * 2u : request.primitiveCount;
		const bool cpuSurfaceReady = IsVoxelMeshVariantSurfaceReady(request.meshVariantHash);
		const bool legacyGpuCandidate = IsLoadingVoxelRequestGpuCandidate(request);
		const bool explicitGpu =
			(request.sourceBits & LoadingVoxelRequestSource_MountedVoxelPreload) != 0 &&
			(request.gpuForce || request.gpuPrefer);
		const bool forcedGpu = explicitGpu && request.gpuForce;
		const bool preferredGpu = explicitGpu && request.gpuPrefer && !request.gpuForce;
		uint64_t geometryContentHash = 0;
		uint64_t renderPrimitiveHash = 0;
		GetReadyVoxelMeshVariantContentHashes(request.meshVariantHash, geometryContentHash, renderPrimitiveHash);
		if (rawStatsReady)
		{
			const VoxelGeometryContentHashes rawHashes = BuildRawVoxelGeometryContentHashes(
				rawStats,
				request.texid.GetIndex(),
				request.resolvedVoxelIndex);
			geometryContentHash = rawHashes.geometryContentHash;
			renderPrimitiveHash = rawHashes.renderPrimitiveHash;
		}

		FGameTexture* voxelTexture = TexMan.GetGameTexture(request.model->GetPaletteTexture());
		if (voxelTexture != nullptr && !voxelTexture->isValid())
		{
			voxelTexture = nullptr;
		}

		std::vector<LoadingVoxelMaterialContext> contexts = request.materialContexts;
		if (contexts.empty())
		{
			LoadingVoxelMaterialContext fallbackContext = {};
			fallbackContext.texid = request.texid;
			fallbackContext.actor = request.actor;
			fallbackContext.actorIndex = request.actorIndex;
			fallbackContext.sourceBits = request.sourceBits;
			fallbackContext.priority = request.priority;
			fallbackContext.gpuForce = request.gpuForce;
			fallbackContext.gpuPrefer = request.gpuPrefer;
			contexts.push_back(fallbackContext);
		}

		for (const LoadingVoxelMaterialContext& context : contexts)
		{
			FGameTexture* emissiveSourceTexture = TexMan.GetGameTexture(context.texid);
			if (emissiveSourceTexture != nullptr && !emissiveSourceTexture->isValid())
			{
				emissiveSourceTexture = nullptr;
			}
			const int palette = context.actor != nullptr ? context.actor->spr.pal : 0;
			const int shade = context.actor != nullptr ? context.actor->spr.shade : 0;
			const float alpha = context.actor != nullptr ? GetLoadingActorAlpha(context.actor) : 1.0f;
			const MaterialRef material = voxelTexture != nullptr ?
				MakeVoxelPaletteMaterialRef(voxelTexture, emissiveSourceTexture, palette, shade, alpha, MaterialFlag_Sprite) :
				MaterialRef{};
			const uint64_t materialVariantHash = voxelTexture != nullptr ?
				BuildVoxelMaterialVariantKeyHash(BuildVoxelMaterialVariantKey(voxelTexture, material)) :
				0ull;
			const uint64_t pairHash = HashCombine64(request.meshVariantHash, materialVariantHash);
			if (materialVariantHash != 0 && !seenVariantPairs.insert(pairHash).second)
			{
				continue;
			}

			PrecachedVoxelRawManifestView view = {};
			view.meshKeyHash = request.meshVariantHash;
			view.materialKeyHash = materialVariantHash;
			view.geometryContentHash = geometryContentHash;
			view.renderPrimitiveHash = renderPrimitiveHash;
			view.meshVariantHash = request.meshVariantHash;
			view.materialVariantHash = materialVariantHash;
			view.sourceBits = request.sourceBits | context.sourceBits;
			view.priority = (int32_t)request.priority;
			view.admissionRank = LoadingVoxelAdmissionRank(request);
			view.sourcePicnum = context.texid.GetIndex();
			view.resolvedVoxelIndex = request.resolvedVoxelIndex;
			view.primitiveCount = primitiveCount;
			view.rawSlabCount = rawStatsReady ? rawStats.slabCount : 0u;
			view.rawColorRunCount = 0u;
			view.rawBytes = rawStatsReady ? rawStats.rawByteCount : 0ull;
			view.gpuForce = forcedGpu;
			view.gpuPrefer = preferredGpu;
			view.legacyGpuCandidate = legacyGpuCandidate;
			view.rawSourceResident = rawStatsReady;
			view.rawStatsReady = rawStatsReady;
			view.materialContextReady = materialVariantHash != 0;
			view.cpuSurfaceReady = cpuSurfaceReady;
			view.model = request.model;
			view.material = material;
			outEntries.push_back(std::move(view));
		}
	}

	return !outEntries.empty();
}

bool CaptureScene(HWDrawInfo& di, SceneView& outView)
{
	outView = {};
	outView.drawInfo = &di;
	const bool rootVoxelCacheFrame = BeginVoxelActorCacheFrame();
	outView.stats = CollectDebugStats(di);

	CaptureWalls(di, di.drawlists[GLDL_PLAINWALLS], GLDL_PLAINWALLS, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLS], GLDL_MASKEDWALLS, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSS], GLDL_MASKEDWALLSS, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSD], GLDL_MASKEDWALLSD, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSV], GLDL_MASKEDWALLSV, outView.opaqueWalls, outView.stats, outView);
	CaptureWalls(di, di.drawlists[GLDL_MASKEDWALLSH], GLDL_MASKEDWALLSH, outView.opaqueWalls, outView.stats, outView);
	CaptureMirrorBorders(di, di.drawlists[GLDL_TRANSLUCENTBORDER], GLDL_TRANSLUCENTBORDER, outView.opaqueWalls, outView.stats);

	CaptureFlats(di, di.drawlists[GLDL_PLAINFLATS], GLDL_PLAINFLATS, outView.opaqueFlats, outView.stats, outView);
	CaptureFlats(di, di.drawlists[GLDL_MASKEDFLATS], GLDL_MASKEDFLATS, outView.opaqueFlats, outView.stats, outView);
	CaptureFlats(di, di.drawlists[GLDL_MASKEDSLOPEFLATS], GLDL_MASKEDSLOPEFLATS, outView.opaqueFlats, outView.stats, outView);
	CaptureSpriteFlats(di, di.drawlists[GLDL_MASKEDFLATS], GLDL_MASKEDFLATS, outView.opaqueFlats);
	CaptureSpriteFlats(di, di.drawlists[GLDL_MASKEDSLOPEFLATS], GLDL_MASKEDSLOPEFLATS, outView.opaqueFlats);

	CaptureFacingSprites(di, di.drawlists[GLDL_TRANSLUCENT], GLDL_TRANSLUCENT, outView.opaqueSprites);
	CaptureModelSprites(di, di.drawlists[GLDL_MODELS], GLDL_MODELS, outView.opaqueSprites, outView.stats, DynamicVoxelCaptureMode::Authoritative);
	CapturePortalViews(di, outView);
	EndVoxelActorCacheFrame(outView.stats, rootVoxelCacheFrame);

	for (const auto& wall : outView.opaqueWalls)
	{
		outView.stats.triangleEstimate += CountFanTriangles(wall);
		outView.stats.materialRefs++;
	}

	for (const auto& flat : outView.opaqueFlats)
	{
		outView.stats.triangleEstimate += CountTriangleListTriangles(flat);
		outView.stats.materialRefs++;
	}

	for (const auto& sprite : outView.opaqueSprites)
	{
		outView.stats.triangleEstimate += CountFanTriangles(sprite);
		outView.stats.materialRefs++;
	}

	return !outView.opaqueWalls.empty() || !outView.opaqueFlats.empty() || !outView.opaqueSprites.empty();
}
}
