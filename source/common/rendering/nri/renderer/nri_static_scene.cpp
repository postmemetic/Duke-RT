#include "nri_static_scene.h"
#include "nri_cvars.h"

#include "nri_renderer.h"
#include "nri_diagnostic_names.h"
#include "nri_persistent_voxel_services.h"
#include "nri_render_geometry_helpers.h"
#include "nri_ray_scene_builder.h"
#include "nri_scene_upload.h"
#include "nri_shader_contracts.h"
#include "nri_sky_environment.h"
#include "nri_static_scene_geometry.h"
#include "nri_runtime_mutation_shared.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_math.h"
#include "../system/nri_renderdevice.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "perf_capture.h"
#include "mapinfo.h"
#include "texturemanager.h"
#include "texinfo.h"

#include <algorithm>
#include <chrono>
#include <utility>


namespace
{
	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	static const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}

	static nri::AccelerationStructureBits GetStaticMapChunkBlasBuildBits()
	{
		return
			nri::AccelerationStructureBits::PREFER_FAST_TRACE |
			nri::AccelerationStructureBits::ALLOW_UPDATE;
	}

	static uint64_t ResolveStaticSegmentGeometrySignature(const StaticMapSceneCache::ChunkCache& chunk)
	{
		if (chunk.exactGeometrySignature != 0)
		{
			return chunk.exactGeometrySignature;
		}
		if (chunk.geometryPayloadHash != 0)
		{
			return chunk.geometryPayloadHash;
		}
		return chunk.geometryTopologySignature;
	}

	static StaticGeometrySegmentKey BuildStaticSegmentKey(
		const StaticMapSceneCache::ChunkCache& chunk,
		const StaticMapChunkAtlas::ChunkEntry& atlasChunk)
	{
		StaticGeometrySegmentKey key = {};
		key.geometrySignature = ResolveStaticSegmentGeometrySignature(chunk);
		key.vertexOffset = atlasChunk.vertexOffset;
		key.vertexCount = atlasChunk.vertexCount;
		key.indexOffset = atlasChunk.indexOffset;
		key.indexCount = atlasChunk.indexCount;
		key.primitiveOffset = atlasChunk.primitiveOffset;
		key.primitiveCount = atlasChunk.primitiveCount;
		key.materialOffset = atlasChunk.materialOffset;
		key.materialCount = atlasChunk.materialCount;
		key.sourceChunkIndex = atlasChunk.chunkIndex;
		return key;
	}

	static bool StaticSegmentKeysEqual(const StaticGeometrySegmentKey& lhs, const StaticGeometrySegmentKey& rhs)
	{
		return
			lhs.geometrySignature == rhs.geometrySignature &&
			lhs.vertexOffset == rhs.vertexOffset &&
			lhs.vertexCount == rhs.vertexCount &&
			lhs.indexOffset == rhs.indexOffset &&
			lhs.indexCount == rhs.indexCount &&
			lhs.primitiveOffset == rhs.primitiveOffset &&
			lhs.primitiveCount == rhs.primitiveCount &&
			lhs.materialOffset == rhs.materialOffset &&
			lhs.materialCount == rhs.materialCount &&
			lhs.sourceChunkIndex == rhs.sourceChunkIndex;
	}

	static StaticMapSegmentBlasCache::Entry* FindStaticSegmentCacheEntry(
		StaticMapSegmentBlasCache& cache,
		const StaticGeometrySegmentKey& key)
	{
		for (StaticMapSegmentBlasCache::Entry& entry : cache.entries)
		{
			if (StaticSegmentKeysEqual(entry.key, key))
			{
				return &entry;
			}
		}
		return nullptr;
	}

	static const StaticMapSegmentBlasCache::Entry* FindStaticSegmentCacheEntry(
		const StaticMapSegmentBlasCache& cache,
		const StaticGeometrySegmentKey& key)
	{
		for (const StaticMapSegmentBlasCache::Entry& entry : cache.entries)
		{
			if (StaticSegmentKeysEqual(entry.key, key))
			{
				return &entry;
			}
		}
		return nullptr;
	}

	static const NRIAccelerationStructureResource& ResolveStaticSegmentOrChunkAccelerationStructure(
		const StaticMapSceneCache& staticScene,
		const StaticMapChunkAtlas* atlas,
		uint32_t chunkListIndex,
		StaticMapSegmentBlasCache::RouteStats* routeStats)
	{
		const StaticMapSceneCache::ChunkCache& chunk = staticScene.chunks[chunkListIndex];
		const bool routeEnabled = nri_ptstaticsegmentroute;
		const auto noteChunkFallback = [routeStats]()
		{
			if (routeStats != nullptr)
			{
				routeStats->routedChunkFallback++;
				routeStats->chunkBlasRefs++;
			}
		};

		if (!routeEnabled)
		{
			if (routeStats != nullptr)
			{
				routeStats->rejectDisabled++;
			}
			noteChunkFallback();
			return chunk.accelerationStructure;
		}

		const StaticMapSegmentBlasCache& cache = staticScene.segmentBlasCache;
		if (!cache.valid ||
			cache.buildSerial != staticScene.buildSerial ||
			!cache.blasBuildEnabled ||
			atlas == nullptr ||
			!atlas->valid ||
			atlas->buildSerial != staticScene.buildSerial ||
			chunkListIndex >= atlas->chunks.size())
		{
			if (routeStats != nullptr)
			{
				routeStats->rejectMissingCache++;
			}
			noteChunkFallback();
			return chunk.accelerationStructure;
		}

		const StaticMapChunkAtlas::ChunkEntry& atlasChunk = atlas->chunks[chunkListIndex];
		if (!atlasChunk.valid)
		{
			if (routeStats != nullptr)
			{
				routeStats->rejectMissingCache++;
			}
			noteChunkFallback();
			return chunk.accelerationStructure;
		}

		const StaticGeometrySegmentKey key = BuildStaticSegmentKey(chunk, atlasChunk);
		const StaticMapSegmentBlasCache::Entry* segmentEntry = FindStaticSegmentCacheEntry(cache, key);
		if (segmentEntry == nullptr ||
			segmentEntry->accelerationStructure.accelerationStructure == nullptr)
		{
			if (routeStats != nullptr)
			{
				routeStats->rejectMissingBlas++;
			}
			noteChunkFallback();
			return chunk.accelerationStructure;
		}

		if (routeStats != nullptr)
		{
			routeStats->routedSegment++;
			routeStats->segmentBlasRefs++;
		}
		return segmentEntry->accelerationStructure;
	}

	template<typename Services>
	static void DestroyStaticSegmentBlasCacheResources(
		StaticMapSegmentBlasCache& cache,
		const Services& services)
	{
		if (services.destroyAccelerationStructureResource != nullptr)
		{
			for (StaticMapSegmentBlasCache::Entry& entry : cache.entries)
			{
				services.destroyAccelerationStructureResource(services.user, entry.accelerationStructure);
				entry.scratchSize = 0;
			}
		}
		cache = {};
	}

	template<typename Services>
	static bool PopulateStaticSegmentBlasCache(
		StaticMapSceneCache& staticScene,
		const StaticMapChunkAtlas& atlas,
		const std::vector<NRIStaticMapBlasBuildInput>& blasBuildInputs,
		const Services& services,
		nri::Buffer* vertexBuffer,
		nri::Buffer* indexBuffer,
		bool buildResidentBlas,
		uint64_t& maxScratchSize)
	{
		const uint32_t invalidatedEntries =
			staticScene.segmentBlasCache.valid ? staticScene.segmentBlasCache.entryCount : 0;
		DestroyStaticSegmentBlasCacheResources(staticScene.segmentBlasCache, services);

		StaticMapSegmentBlasCache& cache = staticScene.segmentBlasCache;
		cache.valid = true;
		cache.blasBuildEnabled = buildResidentBlas;
		cache.buildSerial = staticScene.buildSerial;
		cache.invalidations = invalidatedEntries;
		cache.entries.reserve(blasBuildInputs.size());

		for (const NRIStaticMapBlasBuildInput& buildInput : blasBuildInputs)
		{
			if (buildInput.chunkListIndex >= staticScene.chunks.size() ||
				buildInput.chunkListIndex >= atlas.chunks.size())
			{
				continue;
			}

			const StaticMapSceneCache::ChunkCache& chunk = staticScene.chunks[buildInput.chunkListIndex];
			const StaticMapChunkAtlas::ChunkEntry& atlasChunk = atlas.chunks[buildInput.chunkListIndex];
			if (!chunk.active || !atlasChunk.valid || atlasChunk.indexCount == 0 || atlasChunk.primitiveCount == 0)
			{
				continue;
			}

			cache.candidateCount++;
			const StaticGeometrySegmentKey key = BuildStaticSegmentKey(chunk, atlasChunk);
			if (StaticMapSegmentBlasCache::Entry* existing = FindStaticSegmentCacheEntry(cache, key))
			{
				existing->refCount++;
				existing->sourceChunkRefs++;
				cache.cacheHits++;
				cache.duplicateRefs++;
				continue;
			}

			StaticMapSegmentBlasCache::Entry entry = {};
			entry.key = key;
			entry.firstChunkListIndex = buildInput.chunkListIndex;
			entry.refCount = 1;
			entry.sourceChunkRefs = 1;
			cache.cacheMisses++;

			if (buildResidentBlas)
			{
				NRIStaticMapBlasBuildInput segmentBuildInput = {};
				segmentBuildInput.chunkListIndex = buildInput.chunkListIndex;
				segmentBuildInput.vertexCount = atlas.vertexCount;
				segmentBuildInput.indexOffsetBytes = (uint64_t)key.indexOffset * sizeof(uint32_t);
				segmentBuildInput.indexCount = key.indexCount;
				nri::BottomLevelGeometryDesc geometryDesc =
					nri_static_scene::BuildStaticMapBlasGeometryDesc(segmentBuildInput, vertexBuffer, indexBuffer);

				nri::AccelerationStructureDesc blasDesc = {};
				blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
				blasDesc.flags = GetStaticMapChunkBlasBuildBits();
				blasDesc.geometryOrInstanceNum = 1;
				blasDesc.geometries = &geometryDesc;
				if (services.createBottomLevelAccelerationStructure == nullptr ||
					!services.createBottomLevelAccelerationStructure(services.user, blasDesc, entry.accelerationStructure))
				{
					DestroyStaticSegmentBlasCacheResources(cache, services);
					return false;
				}

				entry.scratchSize =
					services.getAccelerationStructureBuildScratchBufferSize != nullptr ?
					services.getAccelerationStructureBuildScratchBufferSize(services.user, entry.accelerationStructure) :
					0;
				maxScratchSize = std::max(maxScratchSize, entry.scratchSize);
				cache.residentBlasCount++;
				cache.residentMemoryBytes += entry.accelerationStructure.memorySize;
			}

			cache.entries.push_back(std::move(entry));
		}

		cache.entryCount = (uint32_t)cache.entries.size();
		return true;
	}

	template<typename Services>
	static void BuildStaticSegmentBlasCacheResources(
		StaticMapSegmentBlasCache& cache,
		const StaticMapChunkAtlas& atlas,
		const Services& services,
		nri::Buffer* vertexBuffer,
		nri::Buffer* indexBuffer,
		NRIBufferResource& scratchBuffer,
		bool needsInitialScratchBarrier,
		std::vector<NRIAccelerationStructureResource*>& barrierResources)
	{
		if (!cache.blasBuildEnabled || cache.entries.empty())
		{
			return;
		}

		bool needsScratchBarrier = needsInitialScratchBarrier;
		for (StaticMapSegmentBlasCache::Entry& entry : cache.entries)
		{
			if (entry.accelerationStructure.accelerationStructure == nullptr)
			{
				continue;
			}

			if (needsScratchBarrier && services.cmdScratchReuseBarrier != nullptr)
			{
				services.cmdScratchReuseBarrier(services.user, scratchBuffer);
			}

			NRIStaticMapBlasBuildInput buildInput = {};
			buildInput.chunkListIndex = entry.firstChunkListIndex;
			buildInput.vertexCount = atlas.vertexCount;
			buildInput.indexOffsetBytes = (uint64_t)entry.key.indexOffset * sizeof(uint32_t);
			buildInput.indexCount = entry.key.indexCount;
			nri::BottomLevelGeometryDesc geometryDesc =
				nri_static_scene::BuildStaticMapBlasGeometryDesc(buildInput, vertexBuffer, indexBuffer);

			nri::BuildBottomLevelAccelerationStructureDesc build = {};
			build.dst = entry.accelerationStructure.accelerationStructure;
			build.geometries = &geometryDesc;
			build.geometryNum = 1;
			build.scratchBuffer = scratchBuffer.buffer;
			build.scratchOffset = 0;
			if (services.cmdBuildBottomLevelAccelerationStructure != nullptr)
			{
				services.cmdBuildBottomLevelAccelerationStructure(services.user, build);
			}

			cache.buildsThisFrame++;
			barrierResources.push_back(&entry.accelerationStructure);
			needsScratchBarrier = true;
		}
	}

	static bool ShouldTracePtPerf()
	{
		return PerfLoopTraceActive() || ShouldEmitRendererTemporalTraceLogs();
	}

	static bool ShouldCollectStaticScenePerfTiming()
	{
		return ShouldTracePtPerf() || PerfCompactCaptureTimingActive();
	}

	class ScopedStaticScenePerfTimer
	{
	public:
		explicit ScopedStaticScenePerfTimer(double& targetMs)
			: mTarget(ShouldCollectStaticScenePerfTiming() ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedStaticScenePerfTimer()
		{
			if (mTarget != nullptr)
			{
				*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	static FTextureID ResolveAuthoredTextureIdForStaticMapSurface(const nri_scene::PTMapSurface& surface)
	{
		switch (surface.kind)
		{
		case nri_scene::PTMapSurfaceKind::Floor:
		{
			const int32_t sectorIndex = surface.surface.provenance.sectorIndex;
			return sectorIndex >= 0 && (unsigned)sectorIndex < sector.Size() ? sector[(unsigned)sectorIndex].floortexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::Ceiling:
		{
			const int32_t sectorIndex = surface.surface.provenance.sectorIndex;
			return sectorIndex >= 0 && (unsigned)sectorIndex < sector.Size() ? sector[(unsigned)sectorIndex].ceilingtexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::WallOneSided:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size())
			{
				return FNullTextureID();
			}

			const walltype& wal = wall[(unsigned)wallIndex];
			return ((wal.cstat & CSTAT_WALL_1WAY) != 0 && wal.nextwall != -1) ? wal.overtexture : wal.walltexture;
		}
		case nri_scene::PTMapSurfaceKind::WallUpper:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			return wallIndex >= 0 && (unsigned)wallIndex < wall.Size() ? wall[(unsigned)wallIndex].walltexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::WallMiddle:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			return wallIndex >= 0 && (unsigned)wallIndex < wall.Size() ? wall[(unsigned)wallIndex].overtexture : FNullTextureID();
		}
		case nri_scene::PTMapSurfaceKind::WallLower:
		{
			const int32_t wallIndex = surface.surface.provenance.wallIndex;
			if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size())
			{
				return FNullTextureID();
			}

			const walltype& wal = wall[(unsigned)wallIndex];
			if ((wal.cstat & CSTAT_WALL_BOTTOM_SWAP) != 0 && wal.nextwall >= 0 && (unsigned)wal.nextwall < wall.Size())
			{
				return wall[(unsigned)wal.nextwall].walltexture;
			}
			return wal.walltexture;
		}
		default:
			return FNullTextureID();
		}
	}

	static bool IsAnimatedStaticMapSurfaceCandidate(const nri_scene::PTMapSurface& surface)
	{
		const FTextureID textureId = ResolveAuthoredTextureIdForStaticMapSurface(surface);
		return textureId.isValid() && GetExtInfo(textureId).picanm.type() != 0;
	}

	static bool ChunkHasAnimatedStaticMapSurfaceCandidates(const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk)
	{
		const uint32_t endSurface = std::min<uint32_t>(chunk.firstSurface + chunk.surfaceCount, (uint32_t)mapWorld.surfaces.size());
		for (uint32_t surfaceIndex = chunk.firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
		{
			if (IsAnimatedStaticMapSurfaceCandidate(mapWorld.surfaces[surfaceIndex]))
			{
				return true;
			}
		}

		return false;
	}

	static bool RefreshAnimatedBindingsForStaticMapChunk(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk,
		nri_scene::SceneView& ioChunkView)
	{
		uint32_t wallSurfaceIndex = 0;
		uint32_t flatSurfaceIndex = 0;
		const uint32_t endSurface = std::min<uint32_t>(chunk.firstSurface + chunk.surfaceCount, (uint32_t)mapWorld.surfaces.size());
		for (uint32_t surfaceIndex = chunk.firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
		{
			const auto& mapSurface = mapWorld.surfaces[surfaceIndex];
			if ((mapSurface.surface.material.flags & nri_scene::MaterialFlag_Sky) != 0 && mapSurface.surface.material.texture != nullptr)
			{
				continue;
			}

			nri_scene::SurfaceRef* targetSurface = nullptr;
			switch (mapSurface.kind)
			{
			case nri_scene::PTMapSurfaceKind::Floor:
			case nri_scene::PTMapSurfaceKind::Ceiling:
				if (flatSurfaceIndex >= ioChunkView.opaqueFlats.size())
				{
					return false;
				}
				targetSurface = &ioChunkView.opaqueFlats[flatSurfaceIndex++];
				break;
			default:
				if (wallSurfaceIndex >= ioChunkView.opaqueWalls.size())
				{
					return false;
				}
				targetSurface = &ioChunkView.opaqueWalls[wallSurfaceIndex++];
				break;
			}

			if (!IsAnimatedStaticMapSurfaceCandidate(mapSurface))
			{
				continue;
			}

			const FTextureID textureId = ResolveAuthoredTextureIdForStaticMapSurface(mapSurface);
			FGameTexture* liveTexture = textureId.isValid() ? TexMan.GetGameTexture(textureId, true) : nullptr;
			targetSurface->material.texture = liveTexture;
		}

		return wallSurfaceIndex == ioChunkView.opaqueWalls.size() && flatSurfaceIndex == ioChunkView.opaqueFlats.size();
	}
}

void NRIRenderer::ResetResidentMapChunkRegistry()
{
	mStaticSceneResidency.Registry() = {};
	mStaticSceneDiagnostics.Invalidate();
}

void NRIRenderer::QueueStaticMapSceneLightingInvalidation()
{
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.lightingInvalidationRequests++;
	}
	mPendingStaticMapLightingInvalidation = true;
}

void NRIRenderer::InvalidateStaticMapSceneForMaterialLighting()
{
	NRIStaticSceneMaterialLightingRefreshInput input = {};
	input.staticScene = &mStaticMapScene;

	NRIStaticSceneMaterialLightingRefreshServices services = {};
	services.user = this;
	services.ensurePaletteTexture = [](void* user, const nri_scene::MaterialBridgeData& materials) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsurePaletteTexture(materials);
	};
	services.ensureSceneTextures = [](
		void* user,
		const nri_scene::SceneView& sceneView,
		const nri_scene::MaterialBridgeData& materials,
		std::vector<nri_scene::MaterialData>& gpuMaterials,
		bool preserveExistingSky,
		const char* reason) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsureSceneTextures(sceneView, materials, gpuMaterials, preserveExistingSky, reason);
	};
	services.uploadStaticMaterialAtlas = [](void* user) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->EnsureStructuredBuffer(
			renderer->mStaticMaterialBuffer,
			renderer->mMaterialBufferStats,
			renderer->mStaticMapScene.gpuMaterials.data(),
			renderer->mStaticMapScene.gpuMaterials.size() * sizeof(nri_scene::MaterialData),
			sizeof(nri_scene::MaterialData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess());
	};

	if (!nri_static_scene::RefreshStaticMapSceneMaterialLighting(input, services))
	{
		// Fall back to a full resident-scene rebuild on the next frame if the
		// targeted material refresh path fails for any reason.
		DestroyStaticMapSceneCache("material-lighting-refresh-failed");
		mStaticMapScene = {};
		mStaticAccelerationBuildSerial = 0;
		return;
	}
}

NRIStaticSceneGeometryUploadServices NRIRenderer::BuildStaticSceneGeometryUploadServices()
{
	NRIStaticSceneGeometryUploadServices services = {};
	services.user = this;
	services.ensureResidentStructuredBuffer = [](void* user, NRIBufferResource& resource, SceneBufferDebugStats& stats, const void* data, uint64_t size, uint32_t stride, nri::BufferUsageBits usage, nri::AccessStage after, const char* waitReason, int uploadKind) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsureResidentStructuredBuffer(resource, stats, data, size, stride, usage, after, waitReason, uploadKind);
	};
	services.refreshResidentStaticSceneDataSet = [](void* user) -> bool
	{
		return static_cast<NRIRenderer*>(user)->RefreshResidentStaticSceneDataSet();
	};
	services.noteResidentStaticAtlasGrow = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->NoteResidentStaticAtlasGrow();
	};
	return services;
}

void NRIRenderer::SyncResidentMapChunkRegistryFromStaticScene()
{
	std::vector<RuntimeMutationResidentReplacementInfo> replacements;
	if (mMapWorld.valid)
	{
		mRuntimeMutation.CollectResidentReplacementInfo((uint32_t)mMapWorld.chunks.size(), replacements);
	}

	NRIStaticSceneRegistrySyncInput input = {};
	input.mapWorld = &mMapWorld;
	input.staticScene = &mStaticMapScene;
	input.atlas = &mStaticMapChunkAtlas;
	input.replacements = &replacements;
	input.hashResidentMaterialPayload = nri_runtime_mutation::HashResidentMaterialPayload;
	mStaticSceneResidency.SyncResidentMapChunkRegistryFromStaticScene(input);
}

bool NRIRenderer::RefreshStaticMapAnimatedMaterials()
{
	const uint64_t staticBuildSerialBefore = mStaticMapScene.buildSerial;
	const uint32_t animatedSuppressedCountBefore = mStaticMapScene.animatedRefreshSuppressedChunkCount;
	const bool staticValidBefore = mStaticMapScene.valid;
	const nri_scene::SceneView* preservedSkyView =
		(mSkyEnvironment.PreservedStaticMapSky().valid && mSkyEnvironment.PreservedStaticMapSky().buildSerial == mMapWorld.buildSerial)
		? &mSkyEnvironment.PreservedStaticMapSky().sceneView
		: nullptr;

	NRIStaticSceneAnimatedMaterialRefreshInput input = {};
	input.mapWorld = &mMapWorld;
	input.staticScene = &mStaticMapScene;
	input.atlas = &mStaticMapChunkAtlas;
	input.registry = &mStaticSceneResidency.Registry();
	input.preservedSkyView = preservedSkyView;
	input.visibleChunkWords = &mCurrentVisibleChunkWords;
	input.runtimeAnimatedSuppressionEmitCount = &mLastPerfShellTraceStats.runtimeAnimatedSuppressionEmitCount;
	input.traceStats = nri_ptscenestats;
	input.traceMaterialBridgeFailures = nri_ptscenestats && ShouldTracePtPerf();

	NRIStaticSceneAnimatedMaterialRefreshServices services = {};
	services.user = this;
	services.refreshAnimatedBindingsForStaticMapChunk = [](void* user, const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk, nri_scene::SceneView& ioChunkView)
	{
		(void)user;
		return RefreshAnimatedBindingsForStaticMapChunk(mapWorld, chunk, ioChunkView);
	};
	services.buildMaterialsWithActorOverrides = [](void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label)
	{
		static_cast<NRIRenderer*>(user)->BuildMaterialsWithActorOverrides(sceneView, materials, label);
	};
	services.ensurePaletteTexture = [](void* user, const nri_scene::MaterialBridgeData& materials)
	{
		return static_cast<NRIRenderer*>(user)->EnsurePaletteTexture(materials);
	};
	services.ensureSceneTextures = [](void* user, const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& gpuMaterials, bool preserveExistingSky, const char* reason)
	{
		return static_cast<NRIRenderer*>(user)->EnsureSceneTextures(sceneView, materials, gpuMaterials, preserveExistingSky, reason);
	};
	services.uploadStaticMaterialAtlas = [](void* user)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return nri_static_scene_geometry_upload::UploadStaticMapChunkMaterialAtlas(
			renderer->BuildStaticSceneGeometryUploadServices(),
			renderer->mStaticMaterialBuffer,
			renderer->mMaterialBufferStats,
			renderer->mStaticMapChunkAtlas,
			renderer->mStaticMapScene,
			renderer->mStaticMapScene.gpuMaterials);
	};
	services.recoverStaticScene = [](void* user, const char* reason)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		renderer->DestroyStaticMapSceneCache(reason);
		renderer->mStaticMapScene = {};
		renderer->mStaticAccelerationBuildSerial = 0;
		renderer->mSkyEnvironment.PreservedStaticMapSky() = {};
		return renderer->EnsureStaticMapScene();
	};
	services.syncResidentRegistry = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->SyncResidentMapChunkRegistryFromStaticScene();
	};
	services.markUploadedStaticMapSceneLastFrame = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->mUploadedStaticMapSceneLastFrame = true;
	};

	const bool result = nri_static_scene::RefreshStaticMapAnimatedMaterials(input, services);
	if (mStaticMapScene.buildSerial != staticBuildSerialBefore ||
		mStaticMapScene.animatedRefreshSuppressedChunkCount != animatedSuppressedCountBefore ||
		mStaticMapScene.valid != staticValidBefore)
	{
		mStaticSceneDiagnostics.Invalidate();
	}
	return result;
}

bool NRIRenderer::EnsureStaticMapScene()
{
	ScopedStaticScenePerfTimer perfTimer(mLastPerfShellTraceStats.staticSceneMs);
	const uint64_t staticBuildSerialBefore = mStaticMapScene.buildSerial;
	const uint32_t sceneBuildCountBefore = mStaticMapScene.sceneBuildCount;
	const uint32_t accelerationBuildCountBefore = mStaticMapScene.accelerationBuildCount;
	const size_t staticChunkCountBefore = mStaticMapScene.chunks.size();
	const bool staticValidBefore = mStaticMapScene.valid;

	NRIStaticSceneCacheBuildServices staticSceneCacheBuildServices = {};
	staticSceneCacheBuildServices.user = this;
	staticSceneCacheBuildServices.resetMutationCacheForStaticSceneBuild = [](void* user, uint32_t chunkCount)
	{
		static_cast<NRIRenderer*>(user)->mRuntimeMutation.ResetCacheForStaticSceneBuild(chunkCount);
	};
	staticSceneCacheBuildServices.initializeStaticChunkReplacement = [](void* user, const nri_scene::PTMapChunk& chunk)
	{
		static_cast<NRIRenderer*>(user)->mRuntimeMutation.InitializeStaticChunkReplacement(chunk);
	};
	staticSceneCacheBuildServices.buildMaterialsWithActorOverrides = [](void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label)
	{
		static_cast<NRIRenderer*>(user)->BuildMaterialsWithActorOverrides(sceneView, materials, label);
	};
	staticSceneCacheBuildServices.applyCommittedMapMotion = [](void* user, nri_scene::SceneView& sceneView)
	{
		static_cast<NRIRenderer*>(user)->ApplyCommittedMapMotion(sceneView);
	};
	staticSceneCacheBuildServices.chunkHasAnimatedStaticMapSurfaceCandidates = [](void*, const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk)
	{
		return ChunkHasAnimatedStaticMapSurfaceCandidates(mapWorld, chunk);
	};
	staticSceneCacheBuildServices.geometryBuildStaticChunkMs = &mLastPerfShellTraceStats.geometryBuildStaticChunkMs;
	staticSceneCacheBuildServices.geometryBuildStaticChunkCalls = &mLastPerfShellTraceStats.geometryBuildStaticChunkCalls;
	staticSceneCacheBuildServices.geometryBuildStaticChunkPrimitives = &mLastPerfShellTraceStats.geometryBuildStaticChunkPrimitives;
	staticSceneCacheBuildServices.ceilingNudge = nri_ptceilingnudge;
	staticSceneCacheBuildServices.ceilingNudgeDistance = (float)nri_ptceilingnudgedistance;

	NRIStaticSceneEnsureInput input = {};
	input.mapWorld = &mMapWorld;
	input.staticScene = &mStaticMapScene;
	input.atlas = &mStaticMapChunkAtlas;
	input.preservedSkyState = &mSkyEnvironment.PreservedStaticMapSky();
	input.staticAccelerationBuildSerial = &mStaticAccelerationBuildSerial;
	input.uploadedStaticMapSceneLastFrame = &mUploadedStaticMapSceneLastFrame;
	input.builtStaticMapSceneASLastFrame = &mBuiltStaticMapSceneASLastFrame;
	input.frameIndex = mFrameIndex;
	input.traceSceneStats = nri_ptscenestats;
	input.tracePtPerf = ShouldTracePtPerf();
	input.traceSkyPerf = ShouldTraceSkyPerf();

	NRIStaticSceneEnsureServices services = {};
	services.user = this;
	services.cacheBuildServices = staticSceneCacheBuildServices;
	services.destroyStaticMapSceneCache = [](void* user, const char* reason)
	{
		static_cast<NRIRenderer*>(user)->DestroyStaticMapSceneCache(reason);
	};
	services.refreshStaticMapAnimatedMaterials = [](void* user)
	{
		return static_cast<NRIRenderer*>(user)->RefreshStaticMapAnimatedMaterials();
	};
	services.ensurePaletteTexture = [](void* user, const nri_scene::MaterialBridgeData& materials)
	{
		return static_cast<NRIRenderer*>(user)->EnsurePaletteTexture(materials);
	};
	services.ensureSceneTextures = [](void* user, const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& gpuMaterials, bool preserveExistingSky, const char* reason)
	{
		return static_cast<NRIRenderer*>(user)->EnsureSceneTextures(sceneView, materials, gpuMaterials, preserveExistingSky, reason);
	};
	services.uploadStaticMapChunkAtlas = [](void* user)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return nri_static_scene_geometry_upload::UploadStaticMapChunkAtlas(
			renderer->mMapWorld,
			renderer->BuildStaticSceneGeometryUploadServices(),
			renderer->mStaticVertexBuffer,
			renderer->mVertexBufferStats,
			renderer->mStaticIndexBuffer,
			renderer->mIndexBufferStats,
			renderer->mStaticPrimitiveBuffer,
			renderer->mPrimitiveBufferStats,
			renderer->mStaticMaterialBuffer,
			renderer->mMaterialBufferStats,
			renderer->mStaticMapChunkAtlas,
			renderer->mStaticMapScene,
			renderer->mStaticMapScene.gpuMaterials);
	};
	services.buildStaticMapAccelerationStructures = [](void* user)
	{
		return static_cast<NRIRenderer*>(user)->BuildStaticMapAccelerationStructures();
	};
	services.syncResidentRegistryFromStaticScene = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->SyncResidentMapChunkRegistryFromStaticScene();
	};
	services.noteResidentStaticSceneTextureBuild = [](void*)
	{
		gRendererSkyPerfTraceStats.residentStaticSceneTextureBuilds++;
	};
	services.finalizeStaticMapSceneBuildCommands = [](void* user)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		if (renderer->mFrameBuffer == nullptr || renderer->mFrameBuffer->mCommandBuffer == nullptr)
		{
			return true;
		}
		if (!renderer->mFrameBuffer->SubmitWaitAndRestartCommandList("static-map-scene-build"))
		{
			return false;
		}
		renderer->ReleaseWorldAccelerationBuildScratch("static-map-scene-build");
		return true;
	};

	const bool result = nri_static_scene::EnsureStaticMapScene(input, services);
	if (mStaticMapScene.buildSerial != staticBuildSerialBefore ||
		mStaticMapScene.sceneBuildCount != sceneBuildCountBefore ||
		mStaticMapScene.accelerationBuildCount != accelerationBuildCountBefore ||
		mStaticMapScene.chunks.size() != staticChunkCountBefore ||
		mStaticMapScene.valid != staticValidBefore)
	{
		mStaticSceneDiagnostics.Invalidate();
	}
	return result;
}

bool nri_static_scene::RebuildResidentStaticMaterialBridgeFromChunks(
	StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas,
	bool traceFailures)
{
	if (!atlas.valid || atlas.chunks.size() != staticScene.chunks.size())
	{
		return false;
	}

	nri_scene::MaterialBridgeData bridge = {};
	std::vector<uint32_t> chunkListIndices;
	chunkListIndices.reserve(staticScene.chunks.size());
	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& chunkCache = staticScene.chunks[chunkListIndex];
		const auto& atlasChunk = atlas.chunks[chunkListIndex];
		if (!chunkCache.active || !atlasChunk.valid || atlasChunk.materialCount == 0)
		{
			continue;
		}

		chunkListIndices.push_back(chunkListIndex);
	}

	std::sort(
		chunkListIndices.begin(),
		chunkListIndices.end(),
		[&atlas](uint32_t lhs, uint32_t rhs)
		{
			const auto& lhsChunk = atlas.chunks[lhs];
			const auto& rhsChunk = atlas.chunks[rhs];
			if (lhsChunk.materialOffset != rhsChunk.materialOffset)
			{
				return lhsChunk.materialOffset < rhsChunk.materialOffset;
			}

			return lhs < rhs;
		});

	for (uint32_t chunkListIndex : chunkListIndices)
	{
		const auto& chunkCache = staticScene.chunks[chunkListIndex];
		const auto& atlasChunk = atlas.chunks[chunkListIndex];

		if (bridge.materials.size() < atlasChunk.materialOffset)
		{
			bridge.materials.resize(atlasChunk.materialOffset);
			bridge.lightMetadata.resize(atlasChunk.materialOffset);
		}

		const uint32_t nextMaterialOffset = (uint32_t)bridge.materials.size();
		if (nextMaterialOffset != atlasChunk.materialOffset ||
			(uint32_t)chunkCache.materialBridge.materials.size() != atlasChunk.materialCount)
		{
			if (traceFailures)
			{
				Printf("NRI PT static scene trace: event=resident_material_bridge_failed chunk=%u atlas_offset=%u next_offset=%u atlas_count=%u bridge_count=%u\n",
					chunkCache.chunkIndex,
					atlasChunk.materialOffset,
					nextMaterialOffset,
					atlasChunk.materialCount,
					(uint32_t)chunkCache.materialBridge.materials.size());
			}
			return false;
		}

		nri_scene::AppendMaterialBridge(chunkCache.materialBridge, bridge);
	}

	if (bridge.materials.size() < atlas.materialCount)
	{
		bridge.materials.resize(atlas.materialCount);
		bridge.lightMetadata.resize(atlas.materialCount);
	}

	staticScene.materialBridge = std::move(bridge);
	++staticScene.materialGeneration;
	if (staticScene.materialGeneration == 0)
	{
		staticScene.materialGeneration = 1;
	}
	return true;
}

bool nri_static_scene::RefreshStaticMapAnimatedMaterials(
	const NRIStaticSceneAnimatedMaterialRefreshInput& input,
	const NRIStaticSceneAnimatedMaterialRefreshServices& services)
{
	if (input.mapWorld == nullptr || input.staticScene == nullptr || input.atlas == nullptr)
	{
		return true;
	}

	const nri_scene::PTMapWorld& mapWorld = *input.mapWorld;
	StaticMapSceneCache& staticScene = *input.staticScene;
	const StaticMapChunkAtlas& atlas = *input.atlas;
	if (!staticScene.valid ||
		!staticScene.texturesResident ||
		!staticScene.buffersResident ||
		!staticScene.accelerationResident ||
		staticScene.buildSerial != mapWorld.buildSerial)
	{
		return true;
	}

	bool refreshedAnyChunk = false;
	uint32_t refreshedChunkCount = 0;
	const auto recoverStaticScene = [&](const char* reason) -> bool
	{
		return services.recoverStaticScene != nullptr ?
			services.recoverStaticScene(services.user, reason) :
			false;
	};
	const auto suppressAnimatedChunkRefresh = [&](StaticMapSceneCache::ChunkCache& targetChunk, const char* reason)
	{
		if (targetChunk.animatedRefreshSuppressed)
		{
			return;
		}

		targetChunk.animatedRefreshSuppressed = true;
		staticScene.animatedRefreshSuppressedChunkCount++;
		if (input.registry != nullptr &&
			targetChunk.chunkIndex < input.registry->entries.size() &&
			input.registry->entries[targetChunk.chunkIndex].valid)
		{
			auto& entry = input.registry->entries[targetChunk.chunkIndex];
			entry.animatedRefreshSuppressed = true;
			entry.animatedSuppressionEmitCount++;
		}
		if (input.runtimeAnimatedSuppressionEmitCount != nullptr)
		{
			(*input.runtimeAnimatedSuppressionEmitCount)++;
		}
		if (input.traceStats)
		{
			Printf("NRI PT static scene anim: suppressing chunk=%u resident animated refresh (%s).\n",
				targetChunk.chunkIndex,
				reason != nullptr ? reason : "unknown");
		}
	};

	for (size_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		auto& chunkCache = staticScene.chunks[chunkListIndex];
		if (chunkListIndex >= staticScene.lightChunkViews.size() || chunkCache.chunkIndex >= mapWorld.chunks.size())
		{
			return recoverStaticScene("animated-refresh-layout-mismatch");
		}
		if (!chunkCache.active)
		{
			continue;
		}
		if (!chunkCache.hasAnimatedTextureCandidates ||
			chunkCache.animatedRefreshSuppressed ||
			input.visibleChunkWords == nullptr ||
			!nri_runtime_mutation::IsChunkMarkedVisible(*input.visibleChunkWords, chunkCache.chunkIndex))
		{
			continue;
		}

		nri_scene::SceneView liveChunkView = staticScene.lightChunkViews[chunkListIndex];
		if (services.refreshAnimatedBindingsForStaticMapChunk == nullptr ||
			!services.refreshAnimatedBindingsForStaticMapChunk(services.user, mapWorld, mapWorld.chunks[chunkCache.chunkIndex], liveChunkView))
		{
			suppressAnimatedChunkRefresh(chunkCache, "surface-mapping-mismatch");
			continue;
		}
		const uint64_t liveAnimatedMaterialSignature = nri_runtime_mutation::ComputeAnimatedMaterialSignature(liveChunkView);
		if (liveAnimatedMaterialSignature == chunkCache.animatedMaterialSignature)
		{
			continue;
		}

		const uint64_t liveAnimatedGeometrySignature = nri_runtime_mutation::ComputeAnimatedGeometrySignature(liveChunkView);
		if (liveAnimatedGeometrySignature != chunkCache.animatedGeometrySignature)
		{
			staticScene.animatedGeometryFallbackCount++;
			suppressAnimatedChunkRefresh(chunkCache, "display-metric-mismatch");
			continue;
		}

		nri_scene::MaterialBridgeData liveChunkMaterials;
		{
			Clocker clock(NriPTMaterialBuild);
			if (services.buildMaterialsWithActorOverrides != nullptr)
			{
				services.buildMaterialsWithActorOverrides(services.user, liveChunkView, liveChunkMaterials, "static_map_anim_chunk");
			}
		}
		if ((uint32_t)liveChunkMaterials.materials.size() != chunkCache.materialCount)
		{
			staticScene.animatedGeometryFallbackCount++;
			suppressAnimatedChunkRefresh(chunkCache, "material-slice-mismatch");
			continue;
		}

		staticScene.lightChunkViews[chunkListIndex] = std::move(liveChunkView);
		chunkCache.materialBridge = std::move(liveChunkMaterials);
		chunkCache.animatedMaterialSignature = liveAnimatedMaterialSignature;
		refreshedAnyChunk = true;
		refreshedChunkCount++;
	}

	if (!refreshedAnyChunk)
	{
		return true;
	}

	nri_scene::BuildMapSceneView(mapWorld, staticScene.sceneView, input.preservedSkyView);
	if (!RebuildResidentStaticMaterialBridgeFromChunks(
		staticScene,
		atlas,
		input.traceMaterialBridgeFailures))
	{
		staticScene.animatedGeometryFallbackCount++;
		return recoverStaticScene("animated-refresh-material-bridge-failed");
	}

	const bool uploaded =
		services.ensurePaletteTexture != nullptr &&
		services.ensurePaletteTexture(services.user, staticScene.materialBridge) &&
		services.ensureSceneTextures != nullptr &&
		services.ensureSceneTextures(services.user, staticScene.sceneView, staticScene.materialBridge, staticScene.gpuMaterials, false, "static_map_scene_anim") &&
		services.uploadStaticMaterialAtlas != nullptr &&
		services.uploadStaticMaterialAtlas(services.user);
	if (!uploaded)
	{
		return recoverStaticScene("animated-refresh-upload-failed");
	}

	staticScene.texturesResident = true;
	staticScene.buffersResident = true;
	staticScene.gpuUploadCount++;
	staticScene.animatedRefreshCount += refreshedChunkCount;
	staticScene.animatedRefreshUploadCount++;
	if (services.syncResidentRegistry != nullptr)
	{
		services.syncResidentRegistry(services.user);
	}
	if (services.markUploadedStaticMapSceneLastFrame != nullptr)
	{
		services.markUploadedStaticMapSceneLastFrame(services.user);
	}
	return true;
}

bool nri_static_scene::RefreshStaticMapSceneMaterialLighting(
	const NRIStaticSceneMaterialLightingRefreshInput& input,
	const NRIStaticSceneMaterialLightingRefreshServices& services)
{
	if (input.staticScene == nullptr)
	{
		return true;
	}

	StaticMapSceneCache& staticScene = *input.staticScene;
	if (!staticScene.valid)
	{
		return true;
	}

	if (services.ensurePaletteTexture == nullptr ||
		services.ensureSceneTextures == nullptr ||
		services.uploadStaticMaterialAtlas == nullptr ||
		!services.ensurePaletteTexture(services.user, staticScene.materialBridge) ||
		!services.ensureSceneTextures(services.user, staticScene.sceneView, staticScene.materialBridge, staticScene.gpuMaterials, false, "static_map_scene") ||
		!services.uploadStaticMaterialAtlas(services.user))
	{
		return false;
	}

	staticScene.texturesResident = true;
	staticScene.buffersResident = true;
	staticScene.gpuUploadCount++;
	return true;
}

namespace
{
	uint32_t ResolveStaticMapChunkSectorIndex(const nri_scene::PTMapWorld* mapWorld, uint32_t chunkIndex)
	{
		if (mapWorld == nullptr || chunkIndex >= mapWorld->chunks.size() || mapWorld->chunks[chunkIndex].sectorIndex < 0)
		{
			return UINT32_MAX;
		}

		return (uint32_t)mapWorld->chunks[chunkIndex].sectorIndex;
	}

	void AppendStaticMapInstance(
		uint32_t primitiveOffset,
		uint32_t chunkIndex,
		uint32_t sectorIndex,
		const NRIAccelerationStructureResource& accelerationStructure,
		const NRIStaticMapInstanceBuildServices& services,
		std::vector<nri::TopLevelInstance>& outTlasInstances,
		std::vector<SceneInstanceData>& outSceneInstances)
	{
		if (accelerationStructure.accelerationStructure == nullptr || services.getAccelerationStructureHandle == nullptr)
		{
			return;
		}

		nri::TopLevelInstance instance = {};
		nri_scene::SetTopLevelInstanceTransform(instance, nri_scene::MakeIdentityPTTransform3x4());
		instance.mask = NRI_TLAS_MASK_ALL_WORKLOADS;
		instance.shaderBindingTableLocalOffset = 0;
		instance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		instance.accelerationStructureHandle = services.getAccelerationStructureHandle(services.user, accelerationStructure);
		NRIRaySceneBuilder builder(outTlasInstances, outSceneInstances);
		SceneInstanceData sceneRecord = {};
		sceneRecord.primitiveBase = primitiveOffset;
		sceneRecord.dataSource = nri_diag::SceneDataSourceStatic;
		sceneRecord.materialBase = 0;
		sceneRecord.materialCount = UINT32_MAX;
		sceneRecord.visibilityChunk = UINT32_MAX;
		sceneRecord.metadata0 = chunkIndex;
		sceneRecord.metadata1 = sectorIndex;
		builder.AddLegacyInstance(instance, sceneRecord);
	}

	void BuildStaticMapInstancesFromCache(
		const nri_scene::PTMapWorld* mapWorld,
		const StaticMapSceneCache& staticScene,
		const StaticMapChunkAtlas* atlas,
		const NRIStaticMapInstanceBuildServices& services,
		StaticMapSegmentBlasCache::RouteStats* segmentRouteStats,
		std::vector<nri::TopLevelInstance>& outTlasInstances,
		std::vector<SceneInstanceData>& outSceneInstances)
	{
		const bool useAtlas =
			atlas != nullptr &&
			atlas->valid &&
			atlas->chunks.size() == staticScene.chunks.size();

		outTlasInstances.clear();
		outSceneInstances.clear();
		outTlasInstances.reserve(staticScene.chunks.size());
		outSceneInstances.reserve(staticScene.chunks.size());

		for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
		{
			const auto& chunk = staticScene.chunks[chunkListIndex];
			if (!chunk.active)
			{
				continue;
			}

			uint32_t primitiveOffset = chunk.primitiveOffset;
			uint32_t chunkIndex = chunk.chunkIndex;
			if (useAtlas)
			{
				const auto& atlasChunk = atlas->chunks[chunkListIndex];
				if (!atlasChunk.valid)
				{
					continue;
				}

				primitiveOffset = atlasChunk.primitiveOffset;
				chunkIndex = atlasChunk.chunkIndex;
			}

			AppendStaticMapInstance(
				primitiveOffset,
				chunkIndex,
				ResolveStaticMapChunkSectorIndex(mapWorld, chunkIndex),
				ResolveStaticSegmentOrChunkAccelerationStructure(staticScene, atlas, chunkListIndex, segmentRouteStats),
				services,
				outTlasInstances,
				outSceneInstances);
		}
	}
}

void nri_static_scene::BuildStaticMapInstances(
	const NRIStaticMapInstanceBuildInput& input,
	const NRIStaticMapInstanceBuildServices& services,
	std::vector<nri::TopLevelInstance>& outTlasInstances,
	std::vector<SceneInstanceData>& outSceneInstances)
{
	if (input.staticScene == nullptr)
	{
		outTlasInstances.clear();
		outSceneInstances.clear();
		return;
	}

	const StaticMapSceneCache& staticScene = *input.staticScene;
	if (input.registry != nullptr &&
		input.registry->valid &&
		input.registry->buildSerial == staticScene.buildSerial &&
		!input.registry->entries.empty())
	{
		outTlasInstances.clear();
		outSceneInstances.clear();
		outTlasInstances.reserve(input.registry->activeChunkCount);
		outSceneInstances.reserve(input.registry->activeChunkCount);

		for (const auto& entry : input.registry->entries)
		{
			if (!entry.valid ||
				!entry.active ||
				!entry.mappedInStaticScene ||
				entry.staticSceneChunkListIndex >= staticScene.chunks.size())
			{
				continue;
			}

			const auto& chunk = staticScene.chunks[entry.staticSceneChunkListIndex];
			AppendStaticMapInstance(
				entry.primitiveOffset,
				entry.chunkIndex,
				ResolveStaticMapChunkSectorIndex(input.mapWorld, entry.chunkIndex),
				ResolveStaticSegmentOrChunkAccelerationStructure(
					staticScene,
					input.atlas,
					entry.staticSceneChunkListIndex,
					input.segmentRouteStats),
				services,
				outTlasInstances,
				outSceneInstances);
		}
		return;
	}

	if (input.atlas != nullptr &&
		input.atlas->valid &&
		input.atlas->buildSerial == staticScene.buildSerial &&
		input.atlas->chunks.size() == staticScene.chunks.size())
	{
		BuildStaticMapInstancesFromCache(input.mapWorld, staticScene, input.atlas, services, input.segmentRouteStats, outTlasInstances, outSceneInstances);
		return;
	}

	BuildStaticMapInstancesFromCache(input.mapWorld, staticScene, nullptr, services, input.segmentRouteStats, outTlasInstances, outSceneInstances);
}

bool nri_static_scene::BuildStaticMapBlasBuildInputs(
	const StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas,
	std::vector<NRIStaticMapBlasBuildInput>& outBuildInputs)
{
	outBuildInputs.clear();
	if (staticScene.chunks.empty() ||
		!atlas.valid ||
		atlas.chunks.size() != staticScene.chunks.size())
	{
		return false;
	}

	outBuildInputs.reserve(staticScene.chunks.size());
	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& atlasChunk = atlas.chunks[chunkListIndex];
		NRIStaticMapBlasBuildInput buildInput = {};
		buildInput.chunkListIndex = chunkListIndex;
		buildInput.vertexCount = atlas.vertexCount;
		buildInput.indexOffsetBytes = (uint64_t)atlasChunk.indexOffset * sizeof(uint32_t);
		buildInput.indexCount = atlasChunk.indexCount;
		outBuildInputs.push_back(buildInput);
	}
	return true;
}

nri::BottomLevelGeometryDesc nri_static_scene::BuildStaticMapBlasGeometryDesc(
	const NRIStaticMapBlasBuildInput& buildInput,
	nri::Buffer* vertexBuffer,
	nri::Buffer* indexBuffer)
{
	nri::BottomLevelGeometryDesc geometryDesc = {};
	geometryDesc.flags = nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
	geometryDesc.type = nri::BottomLevelGeometryType::TRIANGLES;
	geometryDesc.triangles.vertexBuffer = vertexBuffer;
	geometryDesc.triangles.vertexOffset = 0;
	geometryDesc.triangles.vertexNum = buildInput.vertexCount;
	geometryDesc.triangles.vertexStride = sizeof(nri_scene::SceneVertex);
	geometryDesc.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
	geometryDesc.triangles.indexBuffer = indexBuffer;
	geometryDesc.triangles.indexOffset = buildInput.indexOffsetBytes;
	geometryDesc.triangles.indexNum = buildInput.indexCount;
	geometryDesc.triangles.indexType = nri::IndexType::UINT32;
	return geometryDesc;
}

bool nri_static_scene::BuildStaticMapAccelerationStructures(
	const nri_scene::PTMapWorld* mapWorld,
	StaticMapSceneCache& staticScene,
	StaticMapSceneResources& staticResources,
	const NRIStaticSceneAccelerationBuildServices& services,
	bool updateLiveState)
{
	std::vector<NRIStaticMapBlasBuildInput> blasBuildInputs;
	if (!BuildStaticMapBlasBuildInputs(staticScene, staticResources.chunkAtlas, blasBuildInputs))
	{
		return false;
	}

	const bool needsWait =
		staticResources.topLevelAS.accelerationStructure != nullptr ||
		staticResources.tlasInstanceBuffer.buffer != nullptr ||
		staticResources.scratchBuffer.buffer != nullptr ||
		staticResources.topLevelScratchBuffer.buffer != nullptr;
	if (needsWait && services.waitForCommandsTracked != nullptr)
	{
		services.waitForCommandsTracked(services.user);
	}

	if (services.destroyBufferResource != nullptr)
	{
		services.destroyBufferResource(services.user, staticResources.tlasInstanceBuffer);
		services.destroyBufferResource(services.user, staticResources.scratchBuffer);
		services.destroyBufferResource(services.user, staticResources.topLevelScratchBuffer);
	}
	if (services.destroyAccelerationStructureResource != nullptr)
	{
		services.destroyAccelerationStructureResource(services.user, staticResources.topLevelAS);
		DestroyStaticSegmentBlasCacheResources(staticScene.segmentBlasCache, services);
		for (auto& chunk : staticScene.chunks)
		{
			services.destroyAccelerationStructureResource(services.user, chunk.accelerationStructure);
			chunk.residentBlasScratchSizeCacheKey = nullptr;
			chunk.residentBlasBuildScratchSize = 0;
			chunk.residentBlasUpdateScratchSize = 0;
			chunk.residentBlasVertexBuffer = nullptr;
			chunk.residentBlasIndexBuffer = nullptr;
			chunk.residentBlasVertexNum = 0;
			chunk.residentBlasIndexOffset = 0;
			chunk.residentBlasIndexNum = 0;
		}
	}

	uint64_t maxScratchSize = 0;
	for (const NRIStaticMapBlasBuildInput& buildInput : blasBuildInputs)
	{
		auto& chunk = staticScene.chunks[buildInput.chunkListIndex];
		nri::BottomLevelGeometryDesc geometryDesc =
			BuildStaticMapBlasGeometryDesc(buildInput, staticResources.vertexBuffer.buffer, staticResources.indexBuffer.buffer);

		nri::AccelerationStructureDesc blasDesc = {};
		blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
		blasDesc.flags = GetStaticMapChunkBlasBuildBits();
		blasDesc.geometryOrInstanceNum = 1;
		blasDesc.geometries = &geometryDesc;
		if (services.createBottomLevelAccelerationStructure == nullptr ||
			!services.createBottomLevelAccelerationStructure(services.user, blasDesc, chunk.accelerationStructure))
		{
			return false;
		}

		const uint64_t scratchSize =
			services.getAccelerationStructureBuildScratchBufferSize != nullptr ?
			services.getAccelerationStructureBuildScratchBufferSize(services.user, chunk.accelerationStructure) :
			0;
		chunk.residentBlasScratchSizeCacheKey = chunk.accelerationStructure.accelerationStructure;
		chunk.residentBlasBuildScratchSize = scratchSize;
		chunk.residentBlasUpdateScratchSize = 0;
		chunk.residentBlasVertexBuffer = staticResources.vertexBuffer.buffer;
		chunk.residentBlasIndexBuffer = staticResources.indexBuffer.buffer;
		chunk.residentBlasVertexNum = buildInput.vertexCount;
		chunk.residentBlasIndexOffset = buildInput.indexOffsetBytes;
		chunk.residentBlasIndexNum = buildInput.indexCount;
		maxScratchSize = std::max(maxScratchSize, scratchSize);
	}

	if (!PopulateStaticSegmentBlasCache(
		staticScene,
		staticResources.chunkAtlas,
		blasBuildInputs,
		services,
		staticResources.vertexBuffer.buffer,
		staticResources.indexBuffer.buffer,
		nri_ptstaticsegmentblasbuild,
		maxScratchSize))
	{
		return false;
	}

	if (services.createScratchBuffer == nullptr ||
		!services.createScratchBuffer(services.user, staticResources.scratchBuffer, maxScratchSize))
	{
		return false;
	}

	std::vector<NRIAccelerationStructureResource*> blasBarrierResources;
	blasBarrierResources.reserve(staticScene.chunks.size());
	for (size_t buildInputIndex = 0; buildInputIndex < blasBuildInputs.size(); ++buildInputIndex)
	{
		const NRIStaticMapBlasBuildInput& buildInput = blasBuildInputs[buildInputIndex];
		auto& chunk = staticScene.chunks[buildInput.chunkListIndex];
		nri::BottomLevelGeometryDesc geometryDesc =
			BuildStaticMapBlasGeometryDesc(buildInput, staticResources.vertexBuffer.buffer, staticResources.indexBuffer.buffer);

		nri::BuildBottomLevelAccelerationStructureDesc build = {};
		build.dst = chunk.accelerationStructure.accelerationStructure;
		build.geometries = &geometryDesc;
		build.geometryNum = 1;
		build.scratchBuffer = staticResources.scratchBuffer.buffer;
		build.scratchOffset = 0;
		if (services.cmdBuildBottomLevelAccelerationStructure != nullptr)
		{
			services.cmdBuildBottomLevelAccelerationStructure(services.user, build);
		}

		if (buildInputIndex + 1 < blasBuildInputs.size() && services.cmdScratchReuseBarrier != nullptr)
		{
			services.cmdScratchReuseBarrier(services.user, staticResources.scratchBuffer);
		}

		blasBarrierResources.push_back(&chunk.accelerationStructure);
	}

	BuildStaticSegmentBlasCacheResources(
		staticScene.segmentBlasCache,
		staticResources.chunkAtlas,
		services,
		staticResources.vertexBuffer.buffer,
		staticResources.indexBuffer.buffer,
		staticResources.scratchBuffer,
		!blasBuildInputs.empty(),
		blasBarrierResources);

	if (!blasBarrierResources.empty() && services.cmdAccelerationReadBarriers != nullptr)
	{
		services.cmdAccelerationReadBarriers(services.user, blasBarrierResources);
	}

	NRIStaticMapInstanceBuildInput instanceInput = {};
	instanceInput.mapWorld = mapWorld;
	instanceInput.staticScene = &staticScene;
	instanceInput.atlas = &staticResources.chunkAtlas;
	StaticMapSegmentBlasCache::RouteStats segmentRouteStats = {};
	instanceInput.segmentRouteStats = &segmentRouteStats;
	NRIStaticMapInstanceBuildServices instanceServices = {};
	instanceServices.user = services.user;
	instanceServices.getAccelerationStructureHandle = services.getAccelerationStructureHandle;
	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(instanceInput, instanceServices, instances, sceneInstances);
	staticScene.segmentBlasCache.routeStats = segmentRouteStats;
	staticResources.sceneInstances = sceneInstances;
	staticResources.accelerationBuildSerial = staticScene.buildSerial;
	return
		services.buildTopLevelAccelerationStructure != nullptr &&
		services.buildTopLevelAccelerationStructure(services.user, instances, staticResources, updateLiveState);
}

bool nri_static_scene::BuildLiveStaticMapAccelerationStructures(
	const NRIStaticSceneLiveAccelerationBuildInput& input,
	const NRIStaticSceneLiveAccelerationBuildServices& services)
{
	if (input.staticScene == nullptr ||
		input.atlas == nullptr ||
		input.staticVertexBuffer == nullptr ||
		input.staticIndexBuffer == nullptr ||
		input.staticPrimitiveBuffer == nullptr ||
		input.staticMaterialBuffer == nullptr ||
		input.emissiveTlasInstanceBuffer == nullptr ||
		input.sceneInstanceBuffer == nullptr ||
		input.scratchBuffer == nullptr ||
		input.emissiveTopLevelScratchBuffer == nullptr ||
		input.emissiveTopLevelAS == nullptr)
	{
		return false;
	}

	StaticMapSceneCache& staticScene = *input.staticScene;
	StaticMapChunkAtlas& atlas = *input.atlas;
	std::vector<NRIStaticMapBlasBuildInput> blasBuildInputs;
	if (!BuildStaticMapBlasBuildInputs(staticScene, atlas, blasBuildInputs))
	{
		return false;
	}

	const bool hasDynamicBottomLevelAS =
		services.hasAnyDynamicBottomLevelAS != nullptr &&
		services.hasAnyDynamicBottomLevelAS(services.user);
	const bool needsWait =
		input.hasWorldTlasFrameSlotResources ||
		input.emissiveTopLevelAS->accelerationStructure != nullptr ||
		hasDynamicBottomLevelAS ||
		input.emissiveTlasInstanceBuffer->buffer != nullptr ||
		input.sceneInstanceBuffer->buffer != nullptr ||
		input.scratchBuffer->buffer != nullptr ||
		input.emissiveTopLevelScratchBuffer->buffer != nullptr;
	if (needsWait && services.waitForCommandsTracked != nullptr)
	{
		services.waitForCommandsTracked(services.user);
	}

	if (services.destroyBufferResource != nullptr)
	{
		services.destroyBufferResource(services.user, *input.emissiveTlasInstanceBuffer);
		services.destroyBufferResource(services.user, *input.sceneInstanceBuffer);
		services.destroyBufferResource(services.user, *input.scratchBuffer);
		services.destroyBufferResource(services.user, *input.emissiveTopLevelScratchBuffer);
	}
	if (services.destroyWorldTlasFrameSlots != nullptr)
	{
		services.destroyWorldTlasFrameSlots(services.user);
	}
	if (services.destroyDynamicBottomLevelAccelerationStructures != nullptr)
	{
		services.destroyDynamicBottomLevelAccelerationStructures(services.user);
	}
	if (services.resetPersistentVoxelsForStaticAccelerationRebuild != nullptr)
	{
		services.resetPersistentVoxelsForStaticAccelerationRebuild(services.user);
	}
	if (services.destroyAccelerationStructureResource != nullptr)
	{
		services.destroyAccelerationStructureResource(services.user, *input.emissiveTopLevelAS);
		DestroyStaticSegmentBlasCacheResources(staticScene.segmentBlasCache, services);
		for (auto& chunk : staticScene.chunks)
		{
			services.destroyAccelerationStructureResource(services.user, chunk.accelerationStructure);
			chunk.residentBlasScratchSizeCacheKey = nullptr;
			chunk.residentBlasBuildScratchSize = 0;
			chunk.residentBlasUpdateScratchSize = 0;
			chunk.residentBlasVertexBuffer = nullptr;
			chunk.residentBlasIndexBuffer = nullptr;
			chunk.residentBlasVertexNum = 0;
			chunk.residentBlasIndexOffset = 0;
			chunk.residentBlasIndexNum = 0;
		}
	}

	uint64_t maxScratchSize = 0;
	for (const NRIStaticMapBlasBuildInput& buildInput : blasBuildInputs)
	{
		auto& chunk = staticScene.chunks[buildInput.chunkListIndex];
		nri::BottomLevelGeometryDesc geometryDesc =
			BuildStaticMapBlasGeometryDesc(buildInput, input.staticVertexBuffer->buffer, input.staticIndexBuffer->buffer);

		nri::AccelerationStructureDesc blasDesc = {};
		blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
		blasDesc.flags = GetStaticMapChunkBlasBuildBits();
		blasDesc.geometryOrInstanceNum = 1;
		blasDesc.geometries = &geometryDesc;
		if (services.createBottomLevelAccelerationStructure == nullptr ||
			!services.createBottomLevelAccelerationStructure(services.user, blasDesc, chunk.accelerationStructure))
		{
			return false;
		}

		const uint64_t scratchSize =
			services.getAccelerationStructureBuildScratchBufferSize != nullptr ?
			services.getAccelerationStructureBuildScratchBufferSize(services.user, chunk.accelerationStructure) :
			0;
		chunk.residentBlasScratchSizeCacheKey = chunk.accelerationStructure.accelerationStructure;
		chunk.residentBlasBuildScratchSize = scratchSize;
		chunk.residentBlasUpdateScratchSize = 0;
		chunk.residentBlasVertexBuffer = input.staticVertexBuffer->buffer;
		chunk.residentBlasIndexBuffer = input.staticIndexBuffer->buffer;
		chunk.residentBlasVertexNum = buildInput.vertexCount;
		chunk.residentBlasIndexOffset = buildInput.indexOffsetBytes;
		chunk.residentBlasIndexNum = buildInput.indexCount;
		maxScratchSize = std::max(maxScratchSize, scratchSize);
	}

	if (!PopulateStaticSegmentBlasCache(
		staticScene,
		atlas,
		blasBuildInputs,
		services,
		input.staticVertexBuffer->buffer,
		input.staticIndexBuffer->buffer,
		nri_ptstaticsegmentblasbuild,
		maxScratchSize))
	{
		return false;
	}

	if (services.createScratchBuffer == nullptr ||
		!services.createScratchBuffer(services.user, *input.scratchBuffer, maxScratchSize))
	{
		return false;
	}

	std::vector<NRIAccelerationStructureResource*> blasBarrierResources;
	blasBarrierResources.reserve(staticScene.chunks.size());
	for (size_t buildInputIndex = 0; buildInputIndex < blasBuildInputs.size(); ++buildInputIndex)
	{
		const NRIStaticMapBlasBuildInput& buildInput = blasBuildInputs[buildInputIndex];
		auto& chunk = staticScene.chunks[buildInput.chunkListIndex];
		nri::BottomLevelGeometryDesc geometryDesc =
			BuildStaticMapBlasGeometryDesc(buildInput, input.staticVertexBuffer->buffer, input.staticIndexBuffer->buffer);

		nri::BuildBottomLevelAccelerationStructureDesc build = {};
		build.dst = chunk.accelerationStructure.accelerationStructure;
		build.geometries = &geometryDesc;
		build.geometryNum = 1;
		build.scratchBuffer = input.scratchBuffer->buffer;
		build.scratchOffset = 0;
		if (services.cmdBuildBottomLevelAccelerationStructure != nullptr)
		{
			services.cmdBuildBottomLevelAccelerationStructure(services.user, build);
		}

		if (buildInputIndex + 1 < blasBuildInputs.size() && services.cmdScratchReuseBarrier != nullptr)
		{
			services.cmdScratchReuseBarrier(services.user, *input.scratchBuffer);
		}

		blasBarrierResources.push_back(&chunk.accelerationStructure);
	}

	BuildStaticSegmentBlasCacheResources(
		staticScene.segmentBlasCache,
		atlas,
		services,
		input.staticVertexBuffer->buffer,
		input.staticIndexBuffer->buffer,
		*input.scratchBuffer,
		!blasBuildInputs.empty(),
		blasBarrierResources);

	if (!blasBarrierResources.empty() && services.cmdAccelerationReadBarriers != nullptr)
	{
		services.cmdAccelerationReadBarriers(services.user, blasBarrierResources);
	}

	NRIStaticMapInstanceBuildInput instanceInput = {};
	instanceInput.mapWorld = input.mapWorld;
	instanceInput.staticScene = &staticScene;
	instanceInput.atlas = &atlas;
	instanceInput.registry = input.registry;
	StaticMapSegmentBlasCache::RouteStats segmentRouteStats = {};
	instanceInput.segmentRouteStats = &segmentRouteStats;
	NRIStaticMapInstanceBuildServices instanceServices = {};
	instanceServices.user = services.user;
	instanceServices.getAccelerationStructureHandle = services.getAccelerationStructureHandle;
	std::vector<nri::TopLevelInstance> instances;
	std::vector<SceneInstanceData> sceneInstances;
	BuildStaticMapInstances(instanceInput, instanceServices, instances, sceneInstances);
	staticScene.segmentBlasCache.routeStats = segmentRouteStats;
	if (input.staticAccelerationBuildSerial != nullptr)
	{
		*input.staticAccelerationBuildSerial = staticScene.buildSerial;
	}
	return
		services.buildTopLevelAccelerationStructure != nullptr &&
		services.updateSceneDataSet != nullptr &&
		services.buildTopLevelAccelerationStructure(services.user, instances) &&
		services.updateSceneDataSet(services.user, sceneInstances);
}

void nri_static_scene::DestroyStaticMapSceneResources(
	StaticMapSceneCache& staticScene,
	StaticMapSceneResources& staticResources,
	const NRIStaticSceneResourceDestroyServices& services,
	bool waitForCommands,
	bool resetSceneFrameGeometry)
{
	const bool hasResidentResources =
		!staticScene.chunks.empty() ||
		staticResources.vertexBuffer.buffer != nullptr ||
		staticResources.indexBuffer.buffer != nullptr ||
		staticResources.primitiveBuffer.buffer != nullptr ||
		staticResources.materialBuffer.buffer != nullptr ||
		staticResources.tlasInstanceBuffer.buffer != nullptr ||
		staticResources.scratchBuffer.buffer != nullptr ||
		staticResources.topLevelScratchBuffer.buffer != nullptr ||
		staticResources.topLevelAS.accelerationStructure != nullptr;
	if (waitForCommands && hasResidentResources && services.waitForCommandsTracked != nullptr)
	{
		services.waitForCommandsTracked(services.user);
	}

	if (services.destroyAccelerationStructureResource != nullptr)
	{
		DestroyStaticSegmentBlasCacheResources(staticScene.segmentBlasCache, services);
		for (auto& chunk : staticScene.chunks)
		{
			services.destroyAccelerationStructureResource(services.user, chunk.accelerationStructure);
			chunk.residentBlasScratchSizeCacheKey = nullptr;
			chunk.residentBlasBuildScratchSize = 0;
			chunk.residentBlasUpdateScratchSize = 0;
		}

		services.destroyAccelerationStructureResource(services.user, staticResources.topLevelAS);
	}
	if (services.destroyBufferResource != nullptr)
	{
		services.destroyBufferResource(services.user, staticResources.vertexBuffer);
		services.destroyBufferResource(services.user, staticResources.indexBuffer);
		services.destroyBufferResource(services.user, staticResources.primitiveBuffer);
		services.destroyBufferResource(services.user, staticResources.materialBuffer);
		services.destroyBufferResource(services.user, staticResources.tlasInstanceBuffer);
		services.destroyBufferResource(services.user, staticResources.scratchBuffer);
		services.destroyBufferResource(services.user, staticResources.topLevelScratchBuffer);
	}

	if (resetSceneFrameGeometry && services.resetSceneFrameGeometry != nullptr)
	{
		services.resetSceneFrameGeometry(services.user);
	}
	staticScene = {};
	staticResources = {};
}

void nri_static_scene::DestroyLiveStaticMapSceneCache(
	const NRIStaticSceneLiveCacheDestroyInput& input,
	const NRIStaticSceneLiveCacheDestroyServices& services,
	bool waitForCommands)
{
	if (input.staticScene == nullptr ||
		input.atlas == nullptr ||
		input.staticVertexBuffer == nullptr ||
		input.staticIndexBuffer == nullptr ||
		input.staticPrimitiveBuffer == nullptr ||
		input.staticMaterialBuffer == nullptr ||
		input.boundStaticPrimitiveCount == nullptr ||
		input.boundDynamicPrimitiveCount == nullptr ||
		input.boundStaticMaterialCount == nullptr ||
		input.boundDynamicMaterialCount == nullptr ||
		input.boundPortalCount == nullptr)
	{
		return;
	}

	if (services.resetPersistentDynamicEmissiveCache != nullptr)
	{
		services.resetPersistentDynamicEmissiveCache(services.user);
	}

	StaticMapSceneCache& staticScene = *input.staticScene;
	const bool hasResidentStaticSceneResources =
		!staticScene.chunks.empty() ||
		input.staticVertexBuffer->buffer != nullptr ||
		input.staticIndexBuffer->buffer != nullptr ||
		input.staticPrimitiveBuffer->buffer != nullptr ||
		input.staticMaterialBuffer->buffer != nullptr;
	if (waitForCommands && hasResidentStaticSceneResources && services.waitForCommandsTracked != nullptr)
	{
		services.waitForCommandsTracked(services.user);
	}

	if (services.destroyAccelerationStructureResource != nullptr)
	{
		DestroyStaticSegmentBlasCacheResources(staticScene.segmentBlasCache, services);
		for (auto& chunk : staticScene.chunks)
		{
			services.destroyAccelerationStructureResource(services.user, chunk.accelerationStructure);
			chunk.residentBlasScratchSizeCacheKey = nullptr;
			chunk.residentBlasBuildScratchSize = 0;
			chunk.residentBlasUpdateScratchSize = 0;
		}
	}
	if (services.destroyBufferResource != nullptr)
	{
		services.destroyBufferResource(services.user, *input.staticVertexBuffer);
		services.destroyBufferResource(services.user, *input.staticIndexBuffer);
		services.destroyBufferResource(services.user, *input.staticPrimitiveBuffer);
		services.destroyBufferResource(services.user, *input.staticMaterialBuffer);
	}

	*input.boundStaticPrimitiveCount = 0;
	*input.boundDynamicPrimitiveCount = 0;
	*input.boundStaticMaterialCount = 0;
	*input.boundDynamicMaterialCount = 0;
	*input.boundPortalCount = 0;
	nri_static_scene_geometry::ResetStaticMapChunkAtlas(*input.atlas);
	if (services.resetSceneFrameGeometry != nullptr)
	{
		services.resetSceneFrameGeometry(services.user);
	}
	if (services.resetRuntimeMutationCacheAndFrameForStaticScene != nullptr)
	{
		services.resetRuntimeMutationCacheAndFrameForStaticScene(services.user);
	}
	if (services.resetResidentMapChunkRegistry != nullptr)
	{
		services.resetResidentMapChunkRegistry(services.user);
	}
}

bool nri_static_scene::EnsureStaticMapScene(
	const NRIStaticSceneEnsureInput& input,
	const NRIStaticSceneEnsureServices& services)
{
	if (input.mapWorld == nullptr ||
		input.staticScene == nullptr ||
		input.atlas == nullptr ||
		input.staticAccelerationBuildSerial == nullptr ||
		input.uploadedStaticMapSceneLastFrame == nullptr ||
		input.builtStaticMapSceneASLastFrame == nullptr)
	{
		return false;
	}

	const nri_scene::PTMapWorld& mapWorld = *input.mapWorld;
	StaticMapSceneCache& staticScene = *input.staticScene;
	StaticMapChunkAtlas& atlas = *input.atlas;
	if (!mapWorld.valid)
	{
		return false;
	}

	const auto clearPreservedSky = [&input]()
	{
		if (input.preservedSkyState != nullptr)
		{
			*input.preservedSkyState = {};
		}
	};

	const char* rebuildReason = nullptr;
	if (staticScene.buildSerial != mapWorld.buildSerial)
	{
		rebuildReason = "build-serial-mismatch";
		if (services.destroyStaticMapSceneCache != nullptr)
		{
			services.destroyStaticMapSceneCache(services.user, "ensure-static-scene-build-serial-mismatch");
		}
		staticScene = {};
		*input.staticAccelerationBuildSerial = 0;
		clearPreservedSky();
	}

	if (staticScene.valid &&
		staticScene.texturesResident &&
		staticScene.buffersResident &&
		staticScene.accelerationResident &&
		staticScene.buildSerial == mapWorld.buildSerial)
	{
		if (services.refreshStaticMapAnimatedMaterials == nullptr ||
			!services.refreshStaticMapAnimatedMaterials(services.user))
		{
			return false;
		}
		staticScene.reuseCount++;
		return true;
	}

	if (rebuildReason == nullptr)
	{
		rebuildReason =
			!staticScene.valid ? "scene-invalid" :
			!staticScene.texturesResident ? "textures-not-resident" :
			!staticScene.buffersResident ? "buffers-not-resident" :
			!staticScene.accelerationResident ? "acceleration-not-resident" :
			"resident-rebuild";
	}
	if (input.traceSceneStats)
	{
		Printf("NRI PT static scene trace: event=rebuild reason=%s frame=%u scene_valid=%s textures=%s buffers=%s acceleration=%s scene_build_serial=%llu map_build_serial=%llu chunks=%u\n",
			rebuildReason,
			input.frameIndex,
			YesNo(staticScene.valid),
			YesNo(staticScene.texturesResident),
			YesNo(staticScene.buffersResident),
			YesNo(staticScene.accelerationResident),
			(unsigned long long)staticScene.buildSerial,
			(unsigned long long)mapWorld.buildSerial,
			(uint32_t)staticScene.chunks.size());
	}

	const NRIPreservedStaticMapSkyState* preservedSkyForBuild =
		input.preservedSkyState != nullptr &&
		input.preservedSkyState->valid &&
		input.preservedSkyState->buildSerial == mapWorld.buildSerial ?
		input.preservedSkyState :
		nullptr;
	if (!BuildStaticMapSceneCache(
		mapWorld,
		preservedSkyForBuild,
		services.cacheBuildServices,
		staticScene))
	{
		return false;
	}

	if (input.traceSkyPerf && services.noteResidentStaticSceneTextureBuild != nullptr)
	{
		services.noteResidentStaticSceneTextureBuild(services.user);
	}

	if (staticScene.geometry.primitives.empty() ||
		services.ensurePaletteTexture == nullptr ||
		!services.ensurePaletteTexture(services.user, staticScene.materialBridge) ||
		services.ensureSceneTextures == nullptr ||
		!services.ensureSceneTextures(services.user, staticScene.sceneView, staticScene.materialBridge, staticScene.gpuMaterials, false, "static_map_scene") ||
		services.uploadStaticMapChunkAtlas == nullptr ||
		!services.uploadStaticMapChunkAtlas(services.user) ||
		services.buildStaticMapAccelerationStructures == nullptr ||
		!services.buildStaticMapAccelerationStructures(services.user))
	{
		return false;
	}
	if (!nri_static_scene_geometry::RebuildResidentStaticCpuAtlasMirror(mapWorld, staticScene, atlas) ||
		!RebuildResidentStaticMaterialBridgeFromChunks(
			staticScene,
			atlas,
			input.traceSceneStats && input.tracePtPerf))
	{
		return false;
	}

	staticScene.valid = true;
	staticScene.texturesResident = true;
	staticScene.buffersResident = true;
	staticScene.accelerationResident = true;
	staticScene.buildSerial = mapWorld.buildSerial;
	staticScene.tlasInstanceCount = (uint32_t)staticScene.chunks.size();
	staticScene.sceneBuildCount++;
	staticScene.gpuUploadCount++;
	staticScene.accelerationBuildCount++;
	if (services.syncResidentRegistryFromStaticScene != nullptr)
	{
		services.syncResidentRegistryFromStaticScene(services.user);
	}
	*input.uploadedStaticMapSceneLastFrame = true;
	*input.builtStaticMapSceneASLastFrame = true;
	clearPreservedSky();

	Printf("NRI PT static scene resident: level=%s build_serial=%llu chunks=%u tris=%u materials=%u uploads=%u as_builds=%u\n",
		mapWorld.level != nullptr ? mapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)staticScene.buildSerial,
		(uint32_t)staticScene.chunks.size(),
		(uint32_t)staticScene.geometry.primitives.size(),
		(uint32_t)staticScene.gpuMaterials.size(),
		staticScene.gpuUploadCount,
		staticScene.accelerationBuildCount);
	return
		services.finalizeStaticMapSceneBuildCommands == nullptr ||
		services.finalizeStaticMapSceneBuildCommands(services.user);
}

void nri_static_scene::InitializeStaticMapSceneCacheBuild(
	const nri_scene::PTMapWorld& mapWorld,
	const NRIPreservedStaticMapSkyState* preservedSkyState,
	const NRIStaticSceneCacheBuildServices& services,
	StaticMapSceneCache& outStaticScene)
{
	outStaticScene.valid = false;
	outStaticScene.texturesResident = false;
	outStaticScene.buffersResident = false;
	outStaticScene.accelerationResident = false;
	outStaticScene.buildSerial = mapWorld.buildSerial;
	outStaticScene.materialGeneration = 1;
	outStaticScene.sceneBuildCount = 0;
	outStaticScene.gpuUploadCount = 0;
	outStaticScene.accelerationBuildCount = 0;
	outStaticScene.animatedCandidateChunkCount = 0;
	outStaticScene.animatedRefreshCount = 0;
	outStaticScene.animatedRefreshUploadCount = 0;
	outStaticScene.animatedGeometryFallbackCount = 0;
	outStaticScene.animatedRefreshSuppressedChunkCount = 0;
	outStaticScene.reuseCount = 0;
	outStaticScene.sceneView = {};
	outStaticScene.lightChunkViews.clear();
	outStaticScene.geometry = {};
	outStaticScene.materialBridge = {};
	outStaticScene.gpuMaterials.clear();
	outStaticScene.chunks.clear();
	outStaticScene.tlasInstanceCount = 0;
	outStaticScene.lightChunkViews.reserve(mapWorld.chunks.size());
	outStaticScene.chunks.reserve(mapWorld.chunks.size());

	if (services.resetMutationCacheForStaticSceneBuild != nullptr)
	{
		services.resetMutationCacheForStaticSceneBuild(services.user, (uint32_t)mapWorld.chunks.size());
	}

	const nri_scene::SceneView* preservedSkyView = preservedSkyState != nullptr ? &preservedSkyState->sceneView : nullptr;
	nri_scene::BuildMapSceneView(mapWorld, outStaticScene.sceneView, preservedSkyView);
}

void nri_static_scene::AppendStaticMapSceneCacheChunk(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::PTMapChunk& chunk,
	const nri_scene::SceneView* preservedSkyView,
	const NRIStaticSceneCacheBuildServices& services,
	StaticMapSceneCache& outStaticScene)
{
	if (services.initializeStaticChunkReplacement != nullptr)
	{
		services.initializeStaticChunkReplacement(services.user, chunk);
	}

	nri_scene::SceneView chunkSceneView;
	nri_scene::GeometryData chunkGeometry;
	nri_scene::MaterialBridgeData chunkMaterials;
	nri_scene::BuildMapChunkSceneView(mapWorld, chunk, chunkSceneView, preservedSkyView);
	if (services.ceilingNudge)
	{
		NudgeMapCeilingSections(chunkSceneView, services.ceilingNudgeDistance);
	}
	if (services.applyCommittedMapMotion != nullptr)
	{
		services.applyCommittedMapMotion(services.user, chunkSceneView);
	}
	{
		Clocker clock(NriPTGeometryBuild);
		const auto start = std::chrono::steady_clock::now();
		nri_scene::BuildGeometry(chunkSceneView, chunkGeometry);
		AssignGeometryPortalIndices(mapWorld, chunkGeometry);
		if (services.geometryBuildStaticChunkMs != nullptr)
		{
			*services.geometryBuildStaticChunkMs += DurationMs(start, std::chrono::steady_clock::now());
		}
	}
	if (services.geometryBuildStaticChunkCalls != nullptr)
	{
		(*services.geometryBuildStaticChunkCalls)++;
	}
	if (services.geometryBuildStaticChunkPrimitives != nullptr)
	{
		*services.geometryBuildStaticChunkPrimitives += (uint32_t)chunkGeometry.primitives.size();
	}
	{
		Clocker clock(NriPTMaterialBuild);
		if (services.buildMaterialsWithActorOverrides != nullptr)
		{
			services.buildMaterialsWithActorOverrides(services.user, chunkSceneView, chunkMaterials, "static_map_chunk");
		}
		else
		{
			nri_scene::BuildMaterials(chunkSceneView, chunkMaterials);
		}
	}
	if (chunkGeometry.primitives.empty())
	{
		return;
	}

	StaticMapSceneCache::ChunkCache chunkCache = {};
	chunkCache.chunkIndex = chunk.chunkIndex;
	chunkCache.vertexOffset = (uint32_t)outStaticScene.geometry.vertices.size();
	chunkCache.vertexCount = (uint32_t)chunkGeometry.vertices.size();
	chunkCache.indexOffset = (uint32_t)outStaticScene.geometry.indices.size();
	chunkCache.indexCount = (uint32_t)chunkGeometry.indices.size();
	chunkCache.primitiveOffset = (uint32_t)outStaticScene.geometry.primitives.size();
	chunkCache.primitiveCount = (uint32_t)chunkGeometry.primitives.size();
	chunkCache.materialOffset = (uint32_t)outStaticScene.materialBridge.materials.size();
	chunkCache.materialCount = (uint32_t)chunkMaterials.materials.size();
	chunkCache.geometryTopologySignature = nri_static_scene_geometry::ComputeGeometryTopologySignature(chunkGeometry);
	chunkCache.primitiveLayoutSignature = nri_static_scene_geometry::ComputePrimitiveLayoutSignature(chunkGeometry);
	chunkCache.exactGeometrySignature = nri_runtime_mutation::ComputeExactGeometrySignature(chunkSceneView);
	chunkCache.animatedMaterialSignature = nri_runtime_mutation::ComputeAnimatedMaterialSignature(chunkSceneView);
	chunkCache.animatedGeometrySignature = nri_runtime_mutation::ComputeAnimatedGeometrySignature(chunkSceneView);
	chunkCache.hasAnimatedTextureCandidates =
		services.chunkHasAnimatedStaticMapSurfaceCandidates != nullptr &&
		services.chunkHasAnimatedStaticMapSurfaceCandidates(services.user, mapWorld, chunk);
	chunkCache.animatedRefreshSuppressed = false;

	AppendGeometry(chunkGeometry, chunkCache.materialOffset, outStaticScene.geometry);
	nri_scene::AppendMaterialBridge(chunkMaterials, outStaticScene.materialBridge);
	chunkCache.geometryPayloadHash = nri_static_scene_geometry::HashResidentGeometryPayload(
		mapWorld,
		outStaticScene.geometry,
		chunkCache.vertexOffset,
		chunkCache.vertexCount,
		chunkCache.indexOffset,
		chunkCache.indexCount,
		chunkCache.primitiveOffset,
		chunkCache.primitiveCount,
		chunkCache.materialOffset,
		chunkCache.materialCount);
	chunkCache.materialBridge = std::move(chunkMaterials);
	if (chunkCache.hasAnimatedTextureCandidates)
	{
		outStaticScene.animatedCandidateChunkCount++;
	}
	outStaticScene.lightChunkViews.push_back(std::move(chunkSceneView));
	outStaticScene.chunks.push_back(std::move(chunkCache));
}

bool nri_static_scene::BuildStaticMapSceneCache(
	const nri_scene::PTMapWorld& mapWorld,
	const NRIPreservedStaticMapSkyState* preservedSkyState,
	const NRIStaticSceneCacheBuildServices& services,
	StaticMapSceneCache& outStaticScene)
{
	if (!mapWorld.valid)
	{
		return false;
	}

	InitializeStaticMapSceneCacheBuild(mapWorld, preservedSkyState, services, outStaticScene);
	const nri_scene::SceneView* preservedSkyView = preservedSkyState != nullptr ? &preservedSkyState->sceneView : nullptr;
	for (const nri_scene::PTMapChunk& chunk : mapWorld.chunks)
	{
		AppendStaticMapSceneCacheChunk(mapWorld, chunk, preservedSkyView, services, outStaticScene);
	}

	return !outStaticScene.geometry.primitives.empty();
}

void nri_static_scene::PrintStaticMapSceneStatus(
	const StaticMapSceneCache& staticScene,
	bool usedStaticMapSceneLastFrame,
	bool uploadedStaticMapSceneLastFrame,
	bool builtStaticMapSceneASLastFrame)
{
	const char* source = usedStaticMapSceneLastFrame ? "authoritative-map-world" : "captured-scene";
	Printf("NRI PT static scene: source=%s resident=%s build_serial=%llu scene_builds=%u uploads=%u as_builds=%u animated_candidate_chunks=%u animated_refreshes=%u animated_refresh_uploads=%u animated_geometry_fallbacks=%u animated_refresh_suppressed=%u reuses=%u last_frame_upload=%s last_frame_as_build=%s chunks=%u tlas_instances=%u tris=%u materials=%u\n",
		source,
		(staticScene.valid && staticScene.texturesResident && staticScene.buffersResident && staticScene.accelerationResident) ? "yes" : "no",
		(unsigned long long)staticScene.buildSerial,
		staticScene.sceneBuildCount,
		staticScene.gpuUploadCount,
		staticScene.accelerationBuildCount,
		staticScene.animatedCandidateChunkCount,
		staticScene.animatedRefreshCount,
		staticScene.animatedRefreshUploadCount,
		staticScene.animatedGeometryFallbackCount,
		staticScene.animatedRefreshSuppressedChunkCount,
		staticScene.reuseCount,
		uploadedStaticMapSceneLastFrame ? "yes" : "no",
		builtStaticMapSceneASLastFrame ? "yes" : "no",
		(uint32_t)staticScene.chunks.size(),
		staticScene.tlasInstanceCount,
		(uint32_t)staticScene.geometry.primitives.size(),
		(uint32_t)staticScene.gpuMaterials.size());
}

bool NRIRenderer::EnsureResidentStaticMapChunkAtlasBufferCapacity(const StaticMapChunkAtlas& atlas)
{
	if (!atlas.valid || !mStaticMapScene.valid)
	{
		return false;
	}
	const NRIStaticSceneGeometryUploadServices uploadServices = BuildStaticSceneGeometryUploadServices();

	const uint32_t targetVertexCapacity = std::max(atlas.vertexCapacity, atlas.vertexCount);
	const uint32_t targetIndexCapacity = std::max(atlas.indexCapacity, atlas.indexCount);
	const uint32_t targetPrimitiveCapacity = std::max(atlas.primitiveCapacity, atlas.primitiveCount);
	const uint32_t targetMaterialCapacity = std::max(atlas.materialCapacity, atlas.materialCount);

	const uint32_t currentVertexCapacity =
		mStaticVertexBuffer.stride != 0 ?
		(uint32_t)(mStaticVertexBuffer.size / mStaticVertexBuffer.stride) :
		0u;
	const uint32_t currentIndexCapacity =
		mStaticIndexBuffer.stride != 0 ?
		(uint32_t)(mStaticIndexBuffer.size / mStaticIndexBuffer.stride) :
		0u;
	const uint32_t currentPrimitiveCapacity =
		mStaticPrimitiveBuffer.stride != 0 ?
		(uint32_t)(mStaticPrimitiveBuffer.size / mStaticPrimitiveBuffer.stride) :
		0u;
	const uint32_t currentMaterialCapacity =
		mStaticMaterialBuffer.stride != 0 ?
		(uint32_t)(mStaticMaterialBuffer.size / mStaticMaterialBuffer.stride) :
		0u;

	const bool growVertexBuffer = currentVertexCapacity < targetVertexCapacity;
	const bool growIndexBuffer = currentIndexCapacity < targetIndexCapacity;
	const bool growPrimitiveBuffer = currentPrimitiveCapacity < targetPrimitiveCapacity;
	const bool growMaterialBuffer = currentMaterialCapacity < targetMaterialCapacity;
	if (!growVertexBuffer && !growIndexBuffer && !growPrimitiveBuffer && !growMaterialBuffer)
	{
		mStaticMapChunkAtlas.vertexCapacity = currentVertexCapacity;
		mStaticMapChunkAtlas.indexCapacity = currentIndexCapacity;
		mStaticMapChunkAtlas.primitiveCapacity = currentPrimitiveCapacity;
		mStaticMapChunkAtlas.materialCapacity = currentMaterialCapacity;
		return true;
	}

	if (growVertexBuffer)
	{
		std::vector<nri_scene::SceneVertex> uploadVertices(targetVertexCapacity);
		const size_t copyCount = std::min<size_t>(mStaticMapScene.geometry.vertices.size(), atlas.vertexCount);
		if (copyCount != 0)
		{
			std::copy_n(mStaticMapScene.geometry.vertices.data(), copyCount, uploadVertices.data());
		}
		if (!uploadServices.EnsureResidentStructuredBuffer(
				mStaticVertexBuffer,
				mVertexBufferStats,
				uploadVertices.data(),
				(uint64_t)uploadVertices.size() * sizeof(nri_scene::SceneVertex),
				sizeof(nri_scene::SceneVertex),
				NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
				NRIResourceAccelerationStructureReadAccess(),
				"resident_chunk_write",
				ResidentUploadKind_Vertex))
		{
			return false;
		}
		mStaticVertexBuffer.usedSize = (uint64_t)atlas.vertexCount * sizeof(nri_scene::SceneVertex);
	}

	if (growIndexBuffer)
	{
		std::vector<uint32_t> uploadIndices(targetIndexCapacity);
		const size_t copyCount = std::min<size_t>(mStaticMapScene.geometry.indices.size(), atlas.indexCount);
		if (copyCount != 0)
		{
			std::copy_n(mStaticMapScene.geometry.indices.data(), copyCount, uploadIndices.data());
		}
		if (!uploadServices.EnsureResidentStructuredBuffer(
				mStaticIndexBuffer,
				mIndexBufferStats,
				uploadIndices.data(),
				(uint64_t)uploadIndices.size() * sizeof(uint32_t),
				sizeof(uint32_t),
				NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
				NRIResourceAccelerationStructureReadAccess(),
				"resident_chunk_write",
				ResidentUploadKind_Index))
		{
			return false;
		}
		mStaticIndexBuffer.usedSize = (uint64_t)atlas.indexCount * sizeof(uint32_t);
	}

	if (growPrimitiveBuffer)
	{
		std::vector<nri_scene::PrimitiveData> uploadPrimitives(targetPrimitiveCapacity);
		const size_t copyCount = std::min<size_t>(mStaticMapScene.geometry.primitives.size(), atlas.primitiveCount);
		if (copyCount != 0)
		{
			std::copy_n(mStaticMapScene.geometry.primitives.data(), copyCount, uploadPrimitives.data());
		}
		if (!uploadServices.EnsureResidentStructuredBuffer(
				mStaticPrimitiveBuffer,
				mPrimitiveBufferStats,
				uploadPrimitives.data(),
				(uint64_t)uploadPrimitives.size() * sizeof(nri_scene::PrimitiveData),
				sizeof(nri_scene::PrimitiveData),
				nri::BufferUsageBits::SHADER_RESOURCE,
				NRIResourceComputeShaderResourceAccess(),
				"resident_chunk_write",
				ResidentUploadKind_Primitive))
		{
			return false;
		}
		mStaticPrimitiveBuffer.usedSize = (uint64_t)atlas.primitiveCount * sizeof(nri_scene::PrimitiveData);
	}

	if (growMaterialBuffer)
	{
		std::vector<nri_scene::MaterialData> uploadMaterials(targetMaterialCapacity);
		const size_t copyCount = std::min<size_t>(mStaticMapScene.gpuMaterials.size(), atlas.materialCount);
		if (copyCount != 0)
		{
			std::copy_n(mStaticMapScene.gpuMaterials.data(), copyCount, uploadMaterials.data());
		}
		if (!uploadServices.EnsureResidentStructuredBuffer(
				mStaticMaterialBuffer,
				mMaterialBufferStats,
				uploadMaterials.data(),
				(uint64_t)uploadMaterials.size() * sizeof(nri_scene::MaterialData),
				sizeof(nri_scene::MaterialData),
				nri::BufferUsageBits::SHADER_RESOURCE,
				NRIResourceComputeShaderResourceAccess(),
				"resident_chunk_write",
				ResidentUploadKind_Material))
		{
			return false;
		}
		mStaticMaterialBuffer.usedSize = (uint64_t)atlas.materialCount * sizeof(nri_scene::MaterialData);
	}

	mStaticMapChunkAtlas.vertexCapacity =
		mStaticVertexBuffer.stride != 0 ?
		(uint32_t)(mStaticVertexBuffer.size / mStaticVertexBuffer.stride) :
		atlas.vertexCapacity;
	mStaticMapChunkAtlas.indexCapacity =
		mStaticIndexBuffer.stride != 0 ?
		(uint32_t)(mStaticIndexBuffer.size / mStaticIndexBuffer.stride) :
		atlas.indexCapacity;
	mStaticMapChunkAtlas.primitiveCapacity =
		mStaticPrimitiveBuffer.stride != 0 ?
		(uint32_t)(mStaticPrimitiveBuffer.size / mStaticPrimitiveBuffer.stride) :
		atlas.primitiveCapacity;
	mStaticMapChunkAtlas.materialCapacity =
		mStaticMaterialBuffer.stride != 0 ?
		(uint32_t)(mStaticMaterialBuffer.size / mStaticMaterialBuffer.stride) :
		atlas.materialCapacity;
	uploadServices.NoteResidentStaticAtlasGrow();

	return uploadServices.RefreshResidentStaticSceneDataSet();
}

void NRIRenderer::BuildStaticMapInstances(std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const
{
	NRIStaticMapInstanceBuildInput input = {};
	input.mapWorld = &mMapWorld;
	input.staticScene = &mStaticMapScene;
	input.atlas = &mStaticMapChunkAtlas;
	input.registry = &mStaticSceneResidency.Registry();

	NRIStaticMapInstanceBuildServices services = {};
	services.user = const_cast<NRIRenderer*>(this);
	services.getAccelerationStructureHandle = [](void* user, const NRIAccelerationStructureResource& accelerationStructure)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*accelerationStructure.accelerationStructure);
	};

	nri_static_scene::BuildStaticMapInstances(input, services, outTlasInstances, outSceneInstances);
}

void NRIRenderer::BuildStaticMapInstances(const StaticMapSceneCache& staticScene, std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const
{
	NRIStaticMapInstanceBuildInput input = {};
	input.mapWorld = &mMapWorld;
	input.staticScene = &staticScene;

	NRIStaticMapInstanceBuildServices services = {};
	services.user = const_cast<NRIRenderer*>(this);
	services.getAccelerationStructureHandle = [](void* user, const NRIAccelerationStructureResource& accelerationStructure)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*accelerationStructure.accelerationStructure);
	};

	nri_static_scene::BuildStaticMapInstances(input, services, outTlasInstances, outSceneInstances);
}

void NRIRenderer::BuildStaticMapInstances(const StaticMapSceneCache& staticScene, const StaticMapChunkAtlas& atlas, std::vector<nri::TopLevelInstance>& outTlasInstances, std::vector<SceneInstanceData>& outSceneInstances) const
{
	NRIStaticMapInstanceBuildInput input = {};
	input.mapWorld = &mMapWorld;
	input.staticScene = &staticScene;
	input.atlas = &atlas;

	NRIStaticMapInstanceBuildServices services = {};
	services.user = const_cast<NRIRenderer*>(this);
	services.getAccelerationStructureHandle = [](void* user, const NRIAccelerationStructureResource& accelerationStructure)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*accelerationStructure.accelerationStructure);
	};

	nri_static_scene::BuildStaticMapInstances(input, services, outTlasInstances, outSceneInstances);
}

bool NRIRenderer::BuildStaticMapAccelerationStructures()
{
	Clocker clock(NriPTAcceleration);

	NRIStaticSceneLiveAccelerationBuildInput input = {};
	input.mapWorld = &mMapWorld;
	input.staticScene = &mStaticMapScene;
	input.atlas = &mStaticMapChunkAtlas;
	input.registry = &mStaticSceneResidency.Registry();
	input.staticVertexBuffer = &mStaticVertexBuffer;
	input.staticIndexBuffer = &mStaticIndexBuffer;
	input.staticPrimitiveBuffer = &mStaticPrimitiveBuffer;
	input.staticMaterialBuffer = &mStaticMaterialBuffer;
	input.emissiveTlasInstanceBuffer = &mEmissiveTlasInstanceBuffer;
	input.sceneInstanceBuffer = &mSceneInstanceBuffer;
	input.scratchBuffer = &mScratchBuffer;
	input.emissiveTopLevelScratchBuffer = &mEmissiveTopLevelScratchBuffer;
	input.emissiveTopLevelAS = &mEmissiveTopLevelAS;
	input.hasWorldTlasFrameSlotResources = mWorldTlasFrameSlots.HasResources();
	input.staticAccelerationBuildSerial = &mStaticAccelerationBuildSerial;

	NRIStaticSceneLiveAccelerationBuildServices services = {};
	services.user = this;
	services.getAccelerationStructureHandle = [](void* user, const NRIAccelerationStructureResource& accelerationStructure)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*accelerationStructure.accelerationStructure);
	};
	services.hasAnyDynamicBottomLevelAS = [](void* user)
	{
		return static_cast<NRIRenderer*>(user)->HasAnyDynamicBottomLevelAS();
	};
	services.waitForCommandsTracked = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->WaitForCommandsTracked();
	};
	services.destroyBufferResource = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyBufferResource(resource);
	};
	services.destroyAccelerationStructureResource = [](void* user, NRIAccelerationStructureResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyAccelerationStructureResource(resource);
	};
	services.destroyDynamicBottomLevelAccelerationStructures = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->DestroyDynamicBottomLevelAccelerationStructures();
	};
	services.destroyWorldTlasFrameSlots = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->DestroyWorldTlasFrameSlots();
	};
	services.resetPersistentVoxelsForStaticAccelerationRebuild = [](void* user)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		renderer->mPersistentVoxels.Reset("static-acceleration-rebuild", false, (int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats, BuildNRIPersistentVoxelResetServices(*renderer));
		renderer->mVoxelRepresentationPolicy.Reset();
	};
	services.createBottomLevelAccelerationStructure = [](void* user, const nri::AccelerationStructureDesc& desc, NRIAccelerationStructureResource& outAccelerationStructure)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		if (renderer->mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*renderer->mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, desc, outAccelerationStructure.accelerationStructure) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*outAccelerationStructure.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		outAccelerationStructure.memorySize = memoryDesc.size;
		outAccelerationStructure.memoryLocation = nri::MemoryLocation::DEVICE;
		outAccelerationStructure.buildFlags = desc.flags;
		outAccelerationStructure.buildType = desc.type;
		outAccelerationStructure.buildTypeValid = true;
		outAccelerationStructure.uncompactedMemorySize = memoryDesc.size;
		outAccelerationStructure.compacted = false;
		return true;
	};
	services.getAccelerationStructureBuildScratchBufferSize = [](void* user, const NRIAccelerationStructureResource& accelerationStructure)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*accelerationStructure.accelerationStructure);
	};
	services.createScratchBuffer = [](void* user, NRIBufferResource& scratchBuffer, uint64_t scratchSize)
	{
		return static_cast<NRIRenderer*>(user)->CreateBufferWithoutView(scratchBuffer, scratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER);
	};
	services.cmdBuildBottomLevelAccelerationStructure = [](void* user, const nri::BuildBottomLevelAccelerationStructureDesc& build)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		renderer->mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*renderer->mFrameBuffer->mCommandBuffer, &build, 1);
		renderer->NoteWorldBlasContentChanged();
	};
	services.cmdScratchReuseBarrier = [](void* user, NRIBufferResource& scratchBuffer)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		nri::BufferBarrierDesc scratchBarrier = {};
		scratchBarrier.buffer = scratchBuffer.buffer;
		scratchBarrier.before = NRIResourceAccelerationStructureScratchAccess();
		scratchBarrier.after = NRIResourceAccelerationStructureScratchAccess();

		nri::BarrierDesc scratchBarrierDesc = {};
		scratchBarrierDesc.buffers = &scratchBarrier;
		scratchBarrierDesc.bufferNum = 1;
		renderer->mFrameBuffer->mCore.CmdBarrier(*renderer->mFrameBuffer->mCommandBuffer, scratchBarrierDesc);
	};
	services.cmdAccelerationReadBarriers = [](void* user, const std::vector<NRIAccelerationStructureResource*>& accelerationStructures)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		std::vector<nri::BufferBarrierDesc> blasBarriers;
		blasBarriers.reserve(accelerationStructures.size());
		for (const NRIAccelerationStructureResource* accelerationStructure : accelerationStructures)
		{
			if (accelerationStructure == nullptr || accelerationStructure->accelerationStructure == nullptr)
			{
				continue;
			}

			nri::BufferBarrierDesc barrier = {};
			barrier.buffer = renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*accelerationStructure->accelerationStructure);
			barrier.before = NRIResourceAccelerationStructureWriteAccess();
			barrier.after = NRIResourceAccelerationStructureReadAccess();
			blasBarriers.push_back(barrier);
		}
		if (!blasBarriers.empty())
		{
			nri::BarrierDesc blasBarrierDesc = {};
			blasBarrierDesc.buffers = blasBarriers.data();
			blasBarrierDesc.bufferNum = (uint32_t)blasBarriers.size();
			renderer->mFrameBuffer->mCore.CmdBarrier(*renderer->mFrameBuffer->mCommandBuffer, blasBarrierDesc);
		}
	};
	services.buildTopLevelAccelerationStructure = [](void* user, const std::vector<nri::TopLevelInstance>& instances)
	{
		return static_cast<NRIRenderer*>(user)->BuildTopLevelAccelerationStructure(instances, SceneDataBufferMask_Static);
	};
	services.updateSceneDataSet = [](void* user, const std::vector<SceneInstanceData>& sceneInstances)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return NRISceneUploadManager::UpdateSceneDataSet(*renderer,
			renderer->mStaticVertexBuffer,
			renderer->mStaticIndexBuffer,
			renderer->mStaticPrimitiveBuffer,
			renderer->mStaticMaterialBuffer,
			renderer->mStaticVertexBuffer,
			renderer->mStaticIndexBuffer,
			renderer->mStaticPrimitiveBuffer,
			renderer->mStaticMaterialBuffer,
			sceneInstances,
			(uint32_t)renderer->mStaticMapScene.geometry.primitives.size(),
			0u,
			(uint32_t)renderer->mStaticMapScene.gpuMaterials.size(),
			0u,
			"build_static_map_scene");
	};

	return nri_static_scene::BuildLiveStaticMapAccelerationStructures(input, services);
}

bool NRIRenderer::BuildStaticMapAccelerationStructures(
	StaticMapSceneCache& staticScene,
	StaticMapSceneResources& staticResources,
	bool updateLiveState)
{
	Clocker clock(NriPTAcceleration);

	NRIStaticSceneAccelerationBuildServices services = {};
	services.user = this;
	services.getAccelerationStructureHandle = [](void* user, const NRIAccelerationStructureResource& accelerationStructure)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureHandle(*accelerationStructure.accelerationStructure);
	};
	services.waitForCommandsTracked = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->WaitForCommandsTracked();
	};
	services.destroyBufferResource = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyBufferResource(resource);
	};
	services.destroyAccelerationStructureResource = [](void* user, NRIAccelerationStructureResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyAccelerationStructureResource(resource);
	};
	services.createBottomLevelAccelerationStructure = [](void* user, const nri::AccelerationStructureDesc& desc, NRIAccelerationStructureResource& outAccelerationStructure)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		if (renderer->mFrameBuffer->mRayTracing.CreateCommittedAccelerationStructure(*renderer->mFrameBuffer->mDevice, nri::MemoryLocation::DEVICE, 0.0f, desc, outAccelerationStructure.accelerationStructure) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureMemoryDesc(*outAccelerationStructure.accelerationStructure, nri::MemoryLocation::DEVICE, memoryDesc);
		outAccelerationStructure.memorySize = memoryDesc.size;
		outAccelerationStructure.memoryLocation = nri::MemoryLocation::DEVICE;
		outAccelerationStructure.buildFlags = desc.flags;
		outAccelerationStructure.buildType = desc.type;
		outAccelerationStructure.buildTypeValid = true;
		outAccelerationStructure.uncompactedMemorySize = memoryDesc.size;
		outAccelerationStructure.compacted = false;
		return true;
	};
	services.getAccelerationStructureBuildScratchBufferSize = [](void* user, const NRIAccelerationStructureResource& accelerationStructure)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureBuildScratchBufferSize(*accelerationStructure.accelerationStructure);
	};
	services.createScratchBuffer = [](void* user, NRIBufferResource& scratchBuffer, uint64_t scratchSize)
	{
		return static_cast<NRIRenderer*>(user)->CreateBufferWithoutView(scratchBuffer, scratchSize, 16, nri::BufferUsageBits::SCRATCH_BUFFER);
	};
	services.cmdBuildBottomLevelAccelerationStructure = [](void* user, const nri::BuildBottomLevelAccelerationStructureDesc& build)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		renderer->mFrameBuffer->mRayTracing.CmdBuildBottomLevelAccelerationStructures(*renderer->mFrameBuffer->mCommandBuffer, &build, 1);
	};
	services.cmdScratchReuseBarrier = [](void* user, NRIBufferResource& scratchBuffer)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		nri::BufferBarrierDesc scratchBarrier = {};
		scratchBarrier.buffer = scratchBuffer.buffer;
		scratchBarrier.before = NRIResourceAccelerationStructureScratchAccess();
		scratchBarrier.after = NRIResourceAccelerationStructureScratchAccess();

		nri::BarrierDesc scratchBarrierDesc = {};
		scratchBarrierDesc.buffers = &scratchBarrier;
		scratchBarrierDesc.bufferNum = 1;
		renderer->mFrameBuffer->mCore.CmdBarrier(*renderer->mFrameBuffer->mCommandBuffer, scratchBarrierDesc);
	};
	services.cmdAccelerationReadBarriers = [](void* user, const std::vector<NRIAccelerationStructureResource*>& accelerationStructures)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		std::vector<nri::BufferBarrierDesc> blasBarriers;
		blasBarriers.reserve(accelerationStructures.size());
		for (const NRIAccelerationStructureResource* accelerationStructure : accelerationStructures)
		{
			if (accelerationStructure == nullptr || accelerationStructure->accelerationStructure == nullptr)
			{
				continue;
			}

			nri::BufferBarrierDesc barrier = {};
			barrier.buffer = renderer->mFrameBuffer->mRayTracing.GetAccelerationStructureBuffer(*accelerationStructure->accelerationStructure);
			barrier.before = NRIResourceAccelerationStructureWriteAccess();
			barrier.after = NRIResourceAccelerationStructureReadAccess();
			blasBarriers.push_back(barrier);
		}
		if (!blasBarriers.empty())
		{
			nri::BarrierDesc blasBarrierDesc = {};
			blasBarrierDesc.buffers = blasBarriers.data();
			blasBarrierDesc.bufferNum = (uint32_t)blasBarriers.size();
			renderer->mFrameBuffer->mCore.CmdBarrier(*renderer->mFrameBuffer->mCommandBuffer, blasBarrierDesc);
		}
	};
	services.buildTopLevelAccelerationStructure = [](void* user, const std::vector<nri::TopLevelInstance>& instances, StaticMapSceneResources& resources, bool updateLiveState)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->BuildTopLevelAccelerationStructure(
			instances,
			SceneDataBufferMask_Static,
			resources.topLevelAS,
			resources.tlasInstanceBuffer,
			resources.topLevelScratchBuffer,
			&resources.vertexBuffer,
			&resources.indexBuffer,
			&resources.tlasInstanceCount,
			updateLiveState,
			false);
	};

	return nri_static_scene::BuildStaticMapAccelerationStructures(&mMapWorld, staticScene, staticResources, services, updateLiveState);
}

void NRIRenderer::DestroyStaticMapSceneResources(StaticMapSceneCache& staticScene, StaticMapSceneResources& staticResources, bool waitForCommands)
{
	NRIStaticSceneResourceDestroyServices services = {};
	services.user = this;
	services.waitForCommandsTracked = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->WaitForCommandsTracked();
	};
	services.destroyBufferResource = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyBufferResource(resource);
	};
	services.destroyAccelerationStructureResource = [](void* user, NRIAccelerationStructureResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyAccelerationStructureResource(resource);
	};
	services.resetSceneFrameGeometry = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->mSceneFrameGeometry.Reset();
	};

	nri_static_scene::DestroyStaticMapSceneResources(
		staticScene,
		staticResources,
		services,
		waitForCommands && mFrameBuffer != nullptr,
		&staticScene == &mStaticMapScene);
}

void NRIRenderer::DestroyStaticMapSceneCache(const char* reason)
{
	mStaticSceneDiagnostics.Invalidate();
	mSurfaceLightOverlayCache.Reset();
	if (nri_ptscenestats)
	{
		Printf("NRI PT static scene trace: event=destroy reason=%s frame=%u scene_valid=%s textures=%s buffers=%s acceleration=%s scene_build_serial=%llu map_build_serial=%llu chunks=%u\n",
			reason != nullptr ? reason : "unspecified",
			mFrameIndex,
			YesNo(mStaticMapScene.valid),
			YesNo(mStaticMapScene.texturesResident),
			YesNo(mStaticMapScene.buffersResident),
			YesNo(mStaticMapScene.accelerationResident),
			(unsigned long long)mStaticMapScene.buildSerial,
			(unsigned long long)mMapWorld.buildSerial,
			(uint32_t)mStaticMapScene.chunks.size());
	}

	NRIStaticSceneLiveCacheDestroyInput input = {};
	input.staticScene = &mStaticMapScene;
	input.atlas = &mStaticMapChunkAtlas;
	input.staticVertexBuffer = &mStaticVertexBuffer;
	input.staticIndexBuffer = &mStaticIndexBuffer;
	input.staticPrimitiveBuffer = &mStaticPrimitiveBuffer;
	input.staticMaterialBuffer = &mStaticMaterialBuffer;
	input.boundStaticPrimitiveCount = &mBoundStaticPrimitiveCount;
	input.boundDynamicPrimitiveCount = &mBoundDynamicPrimitiveCount;
	input.boundStaticMaterialCount = &mBoundStaticMaterialCount;
	input.boundDynamicMaterialCount = &mBoundDynamicMaterialCount;
	input.boundPortalCount = &mBoundPortalCount;

	NRIStaticSceneLiveCacheDestroyServices services = {};
	services.user = this;
	services.waitForCommandsTracked = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->WaitForCommandsTracked();
	};
	services.destroyBufferResource = [](void* user, NRIBufferResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyBufferResource(resource);
	};
	services.destroyAccelerationStructureResource = [](void* user, NRIAccelerationStructureResource& resource)
	{
		static_cast<NRIRenderer*>(user)->DestroyAccelerationStructureResource(resource);
	};
	services.resetPersistentDynamicEmissiveCache = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->ResetPersistentDynamicEmissiveCache();
	};
	services.resetSceneFrameGeometry = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->mSceneFrameGeometry.Reset();
	};
	services.resetRuntimeMutationCacheAndFrameForStaticScene = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->mRuntimeMutation.ResetCacheAndFrame();
	};
	services.resetResidentMapChunkRegistry = [](void* user)
	{
		static_cast<NRIRenderer*>(user)->ResetResidentMapChunkRegistry();
	};

	nri_static_scene::DestroyLiveStaticMapSceneCache(input, services, mFrameBuffer != nullptr);
}

void NRIRenderer::PrintStaticMapSceneStatus() const
{
	nri_static_scene::PrintStaticMapSceneStatus(
		mStaticMapScene,
		mUsedStaticMapSceneLastFrame,
		mUploadedStaticMapSceneLastFrame,
		mBuiltStaticMapSceneASLastFrame);
}

bool NRIRenderer::PreloadStaticMapResources()
{
	const auto start = std::chrono::steady_clock::now();
	const bool wasResident =
		mStaticMapScene.valid &&
		mStaticMapScene.texturesResident &&
		mStaticMapScene.buffersResident &&
		mStaticMapScene.accelerationResident &&
		mStaticMapScene.buildSerial == mMapWorld.buildSerial;
	const uint32_t previousSceneBuildCount = mStaticMapScene.sceneBuildCount;
	const uint32_t previousUploadCount = mStaticMapScene.gpuUploadCount;
	const uint32_t previousAccelerationBuildCount = mStaticMapScene.accelerationBuildCount;
	const auto countDelta = [](uint32_t current, uint32_t previous) -> uint32_t
	{
		return current >= previous ? current - previous : current;
	};

	if (!EnsureStaticMapScene())
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading static chunk: event=failed level=%s build_serial=%llu scene_valid=%u textures=%u buffers=%u acceleration=%u map_valid=%u ms=%.3f\n",
				mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
				(unsigned long long)mMapWorld.buildSerial,
				mStaticMapScene.valid ? 1u : 0u,
				mStaticMapScene.texturesResident ? 1u : 0u,
				mStaticMapScene.buffersResident ? 1u : 0u,
				mStaticMapScene.accelerationResident ? 1u : 0u,
				mMapWorld.valid ? 1u : 0u,
				DurationMs(start, std::chrono::steady_clock::now()));
		}
		return false;
	}

	uint32_t activeChunks = 0;
	uint32_t blasResidentChunks = 0;
	for (const auto& chunk : mStaticMapScene.chunks)
	{
		if (chunk.active)
		{
			activeChunks++;
		}
		if (chunk.active && chunk.accelerationStructure.accelerationStructure != nullptr)
		{
			blasResidentChunks++;
		}
	}

	const bool atlasResident =
		mStaticMapChunkAtlas.valid &&
		mStaticMapChunkAtlas.buildSerial == mStaticMapScene.buildSerial &&
		mStaticMapChunkAtlas.chunks.size() == mStaticMapScene.chunks.size();
	const bool registryResident =
		mStaticSceneResidency.Registry().valid &&
		mStaticSceneResidency.Registry().buildSerial == mStaticMapScene.buildSerial;

	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading static chunk: event=%s level=%s build_serial=%llu scene_builds=%u uploads=%u as_builds=%u new_scene_builds=%u new_uploads=%u new_as_builds=%u chunks=%u active=%u blas=%u atlas=%u atlas_vertices=%u atlas_indices=%u atlas_primitives=%u atlas_materials=%u registry=%u registry_chunks=%u registry_mapped=%u registry_blas=%u light_chunks=%u textures=%u materials=%u tris=%u tlas_instances=%u ms=%.3f\n",
			wasResident ? "reuse" : "warm",
			mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
			(unsigned long long)mStaticMapScene.buildSerial,
			mStaticMapScene.sceneBuildCount,
			mStaticMapScene.gpuUploadCount,
			mStaticMapScene.accelerationBuildCount,
			countDelta(mStaticMapScene.sceneBuildCount, previousSceneBuildCount),
			countDelta(mStaticMapScene.gpuUploadCount, previousUploadCount),
			countDelta(mStaticMapScene.accelerationBuildCount, previousAccelerationBuildCount),
			(uint32_t)mStaticMapScene.chunks.size(),
			activeChunks,
			blasResidentChunks,
			atlasResident ? 1u : 0u,
			mStaticMapChunkAtlas.vertexCount,
			mStaticMapChunkAtlas.indexCount,
			mStaticMapChunkAtlas.primitiveCount,
			mStaticMapChunkAtlas.materialCount,
			registryResident ? 1u : 0u,
			mStaticSceneResidency.Registry().chunkCount,
			mStaticSceneResidency.Registry().mappedChunkCount,
			mStaticSceneResidency.Registry().accelerationResidentChunkCount,
			(uint32_t)mStaticMapScene.lightChunkViews.size(),
			(uint32_t)mStaticMapScene.materialBridge.textures.size(),
			(uint32_t)mStaticMapScene.gpuMaterials.size(),
			(uint32_t)mStaticMapScene.geometry.primitives.size(),
			mStaticMapScene.tlasInstanceCount,
			DurationMs(start, std::chrono::steady_clock::now()));
	}

	if ((int)nri_ptloadingtrace >= 2)
	{
		for (uint32_t chunkListIndex = 0; chunkListIndex < (uint32_t)mStaticMapScene.chunks.size(); ++chunkListIndex)
		{
			const auto& chunk = mStaticMapScene.chunks[chunkListIndex];
			const bool atlasChunkResident =
				atlasResident &&
				chunkListIndex < mStaticMapChunkAtlas.chunks.size() &&
				mStaticMapChunkAtlas.chunks[chunkListIndex].valid;
			const auto atlasChunk =
				atlasChunkResident ?
				mStaticMapChunkAtlas.chunks[chunkListIndex] :
				StaticMapChunkAtlas::ChunkEntry{};
			Printf("NRI PT loading static chunk: event=chunk chunk_list=%u chunk=%u active=%u blas=%u atlas=%u vertex_offset=%u vertex_count=%u index_offset=%u index_count=%u primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u light_view=%u animated=%u suppressed=%u\n",
				chunkListIndex,
				chunk.chunkIndex,
				chunk.active ? 1u : 0u,
				chunk.accelerationStructure.accelerationStructure != nullptr ? 1u : 0u,
				atlasChunkResident ? 1u : 0u,
				atlasChunkResident ? atlasChunk.vertexOffset : chunk.vertexOffset,
				atlasChunkResident ? atlasChunk.vertexCount : chunk.vertexCount,
				atlasChunkResident ? atlasChunk.indexOffset : chunk.indexOffset,
				atlasChunkResident ? atlasChunk.indexCount : chunk.indexCount,
				atlasChunkResident ? atlasChunk.primitiveOffset : chunk.primitiveOffset,
				atlasChunkResident ? atlasChunk.primitiveCount : chunk.primitiveCount,
				atlasChunkResident ? atlasChunk.materialOffset : chunk.materialOffset,
				atlasChunkResident ? atlasChunk.materialCount : chunk.materialCount,
				chunkListIndex < mStaticMapScene.lightChunkViews.size() ? 1u : 0u,
				chunk.hasAnimatedTextureCandidates ? 1u : 0u,
				chunk.animatedRefreshSuppressed ? 1u : 0u);
		}
	}

	return true;
}
