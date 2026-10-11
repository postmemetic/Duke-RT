#include "nri_scene_frame_mirrors.h"
#include "nri_cvars.h"
#include "nri_upload_hash.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"
#include "../scene/nri_scene_view_scratch.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "gamecontrol.h"
#include "gamestruct.h"
#include "hw_voxels.h"
#include "models/modeldata.h"
#include "printf.h"
#include "texinfo.h"
#include "buildtiles.h"

#include <chrono>
#include <cstring>

namespace
{
	static uint32_t CountSceneViewSurfaces(const nri_scene::SceneView& sceneView)
	{
		return (uint32_t)(sceneView.opaqueWalls.size() + sceneView.opaqueFlats.size() + sceneView.opaqueSprites.size());
	}

	static double LocalPlayerCaptureDurationMs(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	static bool localPlayerReflectionCaptureStatsDiffer(const NRILocalPlayerReflectionCaptureStats& a, const NRILocalPlayerReflectionCaptureStats& b)
	{
		return
			a.viewpointActorIndex != b.viewpointActorIndex ||
			a.localPlayerActorIndex != b.localPlayerActorIndex ||
			a.viewpointMatchesLocalPlayer != b.viewpointMatchesLocalPlayer ||
			a.primaryVisible != b.primaryVisible ||
			a.capturedScene != b.capturedScene ||
			a.rawFacingSprites != b.rawFacingSprites ||
			a.rawVoxelSprites != b.rawVoxelSprites ||
			a.capturedSurfaceCount != b.capturedSurfaceCount ||
			a.capturedMatchingActorSurfaces != b.capturedMatchingActorSurfaces ||
			a.capturedOtherActorSurfaces != b.capturedOtherActorSurfaces ||
			a.capturedActorlessSurfaces != b.capturedActorlessSurfaces ||
			a.filteredSurfaceCount != b.filteredSurfaceCount;
	}

	static uint32_t CountDrawListActorSprites(const HWDrawList& drawList, int32_t actorIndex, bool requireVoxel)
	{
		uint32_t count = 0;
		for (auto* sprite : drawList.sprites)
		{
			if (sprite == nullptr || sprite->Sprite == nullptr || sprite->Sprite->ownerActor == nullptr)
			{
				continue;
			}

			if ((int32_t)sprite->Sprite->ownerActor->GetIndex() != actorIndex)
			{
				continue;
			}

			const bool isVoxelSprite =
				sprite->modelframe < 0 &&
				sprite->voxel != nullptr &&
				sprite->voxel->model != nullptr;
			if (requireVoxel ? isVoxelSprite : !isVoxelSprite)
			{
				count++;
			}
		}
		return count;
	}

	static bool PrepareLocalPlayerVoxelSprite(HWDrawInfo& di, DCoreActor* localPlayerActor, HWSprite& outSprite)
	{
		for (unsigned int i = 0; i < di.tsprites.Size(); ++i)
		{
			tspritetype* sprite = di.tsprites.get(i);
			if (sprite == nullptr || sprite->ownerActor != localPlayerActor ||
				sprite->scale.X == 0.0 || sprite->scale.Y == 0.0)
			{
				continue;
			}

			FTextureID textureId = sprite->spritetexture();
			if (!textureId.isValid())
			{
				continue;
			}
			if ((sprite->cstat2 & CSTAT2_SPRITE_NOANIMATE) == 0)
			{
				tileUpdatePicnum(textureId, localPlayerActor->GetIndex() & 16383);
			}
			if ((sprite->cstat2 & CSTAT2_SPRITE_FULLBRIGHT) != 0)
			{
				sprite->shade = -127;
			}
			sprite->setspritetexture(textureId);

			if ((localPlayerActor->sprext.renderflags & SPREXT_NOTMD) != 0 ||
				(sprite->cstat2 & CSTAT2_SPRITE_NOMODEL) != 0)
			{
				return false;
			}
			const auto* modelFrame = modelManager.GetModel(textureId, sprite->pal);
			if (hw_models && modelFrame != nullptr && modelFrame->modelid >= 0 && modelFrame->framenum >= 0)
			{
				return false;
			}
			if (!r_voxels)
			{
				return false;
			}
			const int voxelIndex = GetExtInfo(textureId).tiletovox;
			if (voxelIndex < 0 || voxmodels[voxelIndex] == nullptr)
			{
				return false;
			}
			return outSprite.PrepareVoxel(
				&di,
				voxmodels[voxelIndex],
				sprite,
				sprite->sectp,
				voxrotate[voxelIndex]);
		}
		return false;
	}

	static void AccumulateSceneViewActorSurfaceStats(
		const nri_scene::SceneView& sceneView,
		int32_t actorIndex,
		uint32_t& outMatching,
		uint32_t& outOther,
		uint32_t& outActorless)
	{
		auto visit = [&](const auto& surfaces)
		{
			for (const auto& surface : surfaces)
			{
				if (surface.provenance.actorIndex < 0)
				{
					outActorless++;
				}
				else if (surface.provenance.actorIndex == actorIndex)
				{
					outMatching++;
				}
				else
				{
					outOther++;
				}
			}
		};

		visit(sceneView.opaqueWalls);
		visit(sceneView.opaqueFlats);
		visit(sceneView.opaqueSprites);
	}

	static void TraceLocalPlayerReflectionCaptureStats(const NRILocalPlayerReflectionCaptureStats& stats)
	{
		static bool hasPrevious = false;
		static NRILocalPlayerReflectionCaptureStats previous = {};
		if (!nri_ptscenestats)
		{
			hasPrevious = false;
			previous = {};
			return;
		}

		if (hasPrevious && !localPlayerReflectionCaptureStatsDiffer(previous, stats))
		{
			return;
		}

		Printf("NRI PT local player reflection capture: view_actor=%d local_actor=%d camera_match=%s primary_visible=%s raw_facing=%u raw_voxels=%u captured=%s surfaces=%u match=%u other=%u actorless=%u filtered=%u\n",
			stats.viewpointActorIndex,
			stats.localPlayerActorIndex,
			stats.viewpointMatchesLocalPlayer ? "yes" : "no",
			stats.primaryVisible ? "yes" : "no",
			stats.rawFacingSprites,
			stats.rawVoxelSprites,
			stats.capturedScene ? "yes" : "no",
			stats.capturedSurfaceCount,
			stats.capturedMatchingActorSurfaces,
			stats.capturedOtherActorSurfaces,
			stats.capturedActorlessSurfaces,
			stats.filteredSurfaceCount);
		hasPrevious = true;
		previous = stats;
	}

	class ScopedLocalPlayerReflectionVisibilityCaptureOverride
	{
public:
		explicit ScopedLocalPlayerReflectionVisibilityCaptureOverride(bool enabled)
		{
			if (!enabled || gi == nullptr) return;
			mEnabled = true;
			mPrevious = gi->GetMirrorPlayerVisibilityCaptureOverride();
			gi->SetMirrorPlayerVisibilityCaptureOverride(true);
		}

		~ScopedLocalPlayerReflectionVisibilityCaptureOverride()
		{
			if (mEnabled && gi != nullptr)
			{
				gi->SetMirrorPlayerVisibilityCaptureOverride(mPrevious);
			}
		}

		ScopedLocalPlayerReflectionVisibilityCaptureOverride(const ScopedLocalPlayerReflectionVisibilityCaptureOverride&) = delete;
		ScopedLocalPlayerReflectionVisibilityCaptureOverride& operator=(const ScopedLocalPlayerReflectionVisibilityCaptureOverride&) = delete;

	private:
		bool mEnabled = false;
		bool mPrevious = false;
	};

	static bool AppendLocalPlayerReflectionSurfaces(const nri_scene::SceneView& sourceView, nri_scene::SceneView& outView)
	{
		for (const nri_scene::SurfaceRef& sourceSurface : sourceView.opaqueSprites)
		{
			outView.opaqueSprites.push_back(sourceSurface);
		}

		return !outView.opaqueSprites.empty();
	}

	static bool CaptureLocalPlayerReflectionDynamicScene(
		HWDrawInfo& di,
		nri_scene::SceneView& outView,
		bool residentVoxelReady,
		bool localPlayerPrimaryVisible,
		NRIMirrorRebuildSceneViewStatsFn rebuildSceneViewStats,
		bool* outCurrentVoxel,
		NRILocalPlayerReflectionCaptureStats* outStats = nullptr)
	{
		nri_scene::ClearSceneViewRetainingCapacity(outView);
		if (outCurrentVoxel != nullptr)
		{
			*outCurrentVoxel = false;
		}
		NRILocalPlayerReflectionCaptureStats captureStats = {};
		captureStats.viewpointActorIndex = di.Viewpoint.CameraActor != nullptr ? (int32_t)di.Viewpoint.CameraActor->GetIndex() : -1;
		const auto publishStats = [&]()
		{
			if (outStats != nullptr)
			{
				*outStats = captureStats;
			}
			TraceLocalPlayerReflectionCaptureStats(captureStats);
		};
		if (gi == nullptr ||
			myconnectindex < 0 ||
			myconnectindex >= MAXPLAYERS)
		{
			publishStats();
			return false;
		}

		DCorePlayer* localPlayer = PlayerArray[myconnectindex];
		DCoreActor* localPlayerActor = localPlayer != nullptr ? localPlayer->GetActor() : nullptr;
		if (localPlayerActor == nullptr)
		{
			publishStats();
			return false;
		}

		const int32_t actorIndex = (int32_t)localPlayerActor->GetIndex();
		captureStats.localPlayerActorIndex = actorIndex;
		captureStats.viewpointMatchesLocalPlayer = di.Viewpoint.CameraActor == localPlayerActor;
		captureStats.primaryVisible = localPlayerPrimaryVisible;
		auto stageStart = std::chrono::steady_clock::now();
		HWDrawInfo* captureDi = HWDrawInfo::StartDrawInfo(&di, di.Viewpoint, &di.VPUniforms);
		captureDi->visibility = di.visibility;
		captureDi->rellight = di.rellight;
		// CreateScene normally owns this reset. This focused path deliberately skips
		// CreateScene, while HWDrawInfo pooling retains tsprite array contents.
		captureDi->tsprites.clear();
		captureStats.drawInfoSetupMs = LocalPlayerCaptureDurationMs(stageStart);

		const ScopedLocalPlayerReflectionVisibilityCaptureOverride reflectionCaptureOverride(true);
		renderAddTsprite(captureDi->tsprites, localPlayerActor);
		stageStart = std::chrono::steady_clock::now();
		gi->processSprites(
			captureDi->tsprites,
			DVector3(di.Viewpoint.Pos.X, -di.Viewpoint.Pos.Y, -di.Viewpoint.Pos.Z),
			DAngle::fromBam(di.Viewpoint.RotAngle),
			di.Viewpoint.TicFrac);
		captureStats.processSpritesMs = LocalPlayerCaptureDurationMs(stageStart);
		stageStart = std::chrono::steady_clock::now();
		HWSprite localPlayerVoxelSprite = {};
		const bool preparedVoxel = PrepareLocalPlayerVoxelSprite(*captureDi, localPlayerActor, localPlayerVoxelSprite);
		if (!preparedVoxel)
		{
			captureDi->DispatchSprites();
		}
		captureStats.dispatchSpritesMs = LocalPlayerCaptureDurationMs(stageStart);
		captureStats.rawFacingSprites = preparedVoxel ? 0u : CountDrawListActorSprites(captureDi->drawlists[GLDL_TRANSLUCENT], actorIndex, false);
		captureStats.rawVoxelSprites = preparedVoxel ? 1u : CountDrawListActorSprites(captureDi->drawlists[GLDL_MODELS], actorIndex, true);

		nri_scene::SceneView capturedView;
		stageStart = std::chrono::steady_clock::now();
		const nri_scene::ActorSpriteSceneCaptureResult sceneCapture = preparedVoxel ?
			nri_scene::CaptureActorVoxelSprite(*captureDi, localPlayerVoxelSprite, residentVoxelReady, capturedView) :
			nri_scene::CaptureActorSpriteScene(*captureDi, actorIndex, residentVoxelReady, capturedView);
		captureStats.cacheCaptureMs = LocalPlayerCaptureDurationMs(stageStart);
		if (outCurrentVoxel != nullptr)
		{
			*outCurrentVoxel = sceneCapture.currentVoxel;
		}
		stageStart = std::chrono::steady_clock::now();
		captureDi->EndDrawInfo();
		captureStats.drawInfoReleaseMs = LocalPlayerCaptureDurationMs(stageStart);
		if (!sceneCapture.capturedFallbackScene || !AppendLocalPlayerReflectionSurfaces(capturedView, outView))
		{
			publishStats();
			nri_scene::ClearSceneViewRetainingCapacity(outView);
			return false;
		}

		outView.drawInfo = &di;
		if (rebuildSceneViewStats != nullptr)
		{
			rebuildSceneViewStats(outView);
		}
		captureStats.capturedScene = true;
		captureStats.capturedSurfaceCount = CountSceneViewSurfaces(outView);
		AccumulateSceneViewActorSurfaceStats(
			outView,
			actorIndex,
			captureStats.capturedMatchingActorSurfaces,
			captureStats.capturedOtherActorSurfaces,
			captureStats.capturedActorlessSurfaces);

		// The ordinary dynamic scene owns primary-visible model/facing sprites in
		// external views. Only a voxel awaiting residency needs this direct fallback.
		outView.primitiveFlags = sceneCapture.currentVoxel && localPlayerPrimaryVisible ?
			nri_scene::PrimitiveFlag_None :
			nri_scene::PrimitiveFlag_ReflectionOnly;
		captureStats.filteredSurfaceCount = captureStats.capturedSurfaceCount;
		publishStats();
		return true;
	}

	static uint64_t HashLocalPlayerReflectionMaterialPayloadData(const nri_scene::MaterialBridgeData& materialBridge)
	{
		const uint64_t materialSize = (uint64_t)materialBridge.materials.size() * sizeof(nri_scene::MaterialData);
		return NRIHashUploadPayloadBytes(
			materialBridge.materials.empty() ? nullptr : materialBridge.materials.data(),
			materialSize);
	}

	static uint64_t BuildConservativeLocalPlayerReflectionPayloadStamp(
		uint64_t kind,
		uint64_t frameIndex,
		const nri_scene::GeometryData& geometry,
		const nri_scene::MaterialBridgeData& materials,
		uint64_t mapWorldBuildSerial)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, kind);
		hash = nri_scene::HashCombine64(hash, frameIndex);
		hash = nri_scene::HashCombine64(hash, mapWorldBuildSerial);
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.vertices.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.indices.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.primitives.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry.primitiveProvenance.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)materials.materials.size());
		return hash != 0 ? hash : 1;
	}

	static NRILocalPlayerReflectionUploadStamp BuildLocalPlayerReflectionUploadProducerStamp(
		const nri_scene::GeometryData& geometry,
		const nri_scene::MaterialBridgeData& materials,
		uint64_t frameIndex,
		uint64_t mapWorldBuildSerial)
	{
		NRILocalPlayerReflectionUploadStamp stamp = {};
		stamp.vertexPayloadStamp = BuildConservativeLocalPlayerReflectionPayloadStamp(1u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.indexPayloadStamp = BuildConservativeLocalPlayerReflectionPayloadStamp(2u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.primitivePayloadStamp = BuildConservativeLocalPlayerReflectionPayloadStamp(3u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.primitiveProvenanceStamp = BuildConservativeLocalPlayerReflectionPayloadStamp(4u, frameIndex, geometry, materials, mapWorldBuildSerial);
		stamp.materialPayloadStamp = HashLocalPlayerReflectionMaterialPayloadData(materials);
		return stamp;
	}
}

int32_t ResolveNRILocalPlayerActorIndex()
{
	if (gi == nullptr || myconnectindex < 0 || myconnectindex >= MAXPLAYERS)
	{
		return -1;
	}
	DCorePlayer* localPlayer = PlayerArray[myconnectindex];
	DCoreActor* localPlayerActor = localPlayer != nullptr ? localPlayer->GetActor() : nullptr;
	return localPlayerActor != nullptr ? (int32_t)localPlayerActor->GetIndex() : -1;
}

NRILocalPlayerReflectionCaptureResult CaptureNRILocalPlayerReflectionDynamicScene(const NRILocalPlayerReflectionCaptureRequest& request, nri_scene::SceneView& outView)
{
	NRILocalPlayerReflectionCaptureResult result = {};
	if (request.drawInfo == nullptr)
	{
		nri_scene::ClearSceneViewRetainingCapacity(outView);
		return result;
	}
	result.captured = CaptureLocalPlayerReflectionDynamicScene(
		*request.drawInfo,
		outView,
		request.residentVoxelReady,
		request.localPlayerPrimaryVisible,
		request.rebuildSceneViewStats,
		&result.currentVoxel,
		&result.stats);
	return result;
}

NRILocalPlayerReflectionUploadStamp BuildNRILocalPlayerReflectionUploadProducerStamp(
	const nri_scene::GeometryData& geometry,
	const nri_scene::MaterialBridgeData& materials,
	uint64_t frameIndex,
	uint64_t mapWorldBuildSerial)
{
	return BuildLocalPlayerReflectionUploadProducerStamp(geometry, materials, frameIndex, mapWorldBuildSerial);
}
