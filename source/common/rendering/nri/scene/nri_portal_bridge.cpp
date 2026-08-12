#include "nri_portal_bridge.h"
#include "../renderer/nri_cvars.h"

#include "nri_scene_stats.h"
#include "nri_scene_texture_utils.h"

#include "c_cvars.h"
#include "gamestruct.h"
#include "hw_portal.h"
#include "image.h"
#include "textures.h"
#include "v_video.h"

#include <algorithm>
#include <cstdint>
#define NOMINMAX
#include <windows.h>


namespace
{
	using namespace nri_scene;
	thread_local int gPortalCaptureDepth = 0;

	struct CaptureState
	{
		FRenderState* renderState = nullptr;
	};

	class ScopedPortalCaptureState
	{
	public:
		ScopedPortalCaptureState(DCoreActor* viewer, int type)
			: mViewer(viewer), mType(type)
		{
			if (gi != nullptr)
			{
				gi->EnterPortal(mViewer, mType);
				mEntered = true;
			}
		}

		~ScopedPortalCaptureState()
		{
			if (mEntered && gi != nullptr)
			{
				gi->LeavePortal(mViewer, mType);
			}
		}

	private:
		DCoreActor* mViewer = nullptr;
		int mType = -1;
		bool mEntered = false;
	};

	void TranslateSurface(SurfaceRef& surface, const float delta[3])
	{
		for (CapturedVertex& vertex : surface.vertices)
		{
			for (int i = 0; i < 3; ++i)
			{
				vertex.position[i] += delta[i];
				vertex.prevPosition[i] += delta[i];
			}
		}
	}

	void AppendTranslatedSurfaces(const std::vector<SurfaceRef>& surfaces, const float delta[3], std::vector<SurfaceRef>& outSurfaces)
	{
		for (const SurfaceRef& source : surfaces)
		{
			SurfaceRef translated = source;
			TranslateSurface(translated, delta);
			outSurfaces.push_back(std::move(translated));
		}
	}

	void AppendTranslatedScene(const SceneView& source, const float delta[3], SceneView& outView)
	{
		AppendTranslatedSurfaces(source.opaqueWalls, delta, outView.opaqueWalls);
		AppendTranslatedSurfaces(source.opaqueFlats, delta, outView.opaqueFlats);
		AppendTranslatedSurfaces(source.opaqueSprites, delta, outView.opaqueSprites);
		outView.stats.portalViews += 1;
		AccumulateSceneDebugStats(outView.stats, source.stats);
		if (source.sky.priority > outView.sky.priority)
		{
			outView.sky = source.sky;
			Copy3(source.skyColor, outView.skyColor);
		}
	}

	void MergeSkyFromPortals(HWDrawInfo& di, SceneView& outView)
	{
		for (HWPortal* portal : di.Portals)
		{
			auto* skyPortal = portal != nullptr && portal->IsSky() ? portal : nullptr;
			if (skyPortal == nullptr || skyPortal->lines.Size() == 0)
			{
				continue;
			}

			for (const HWWall& wall : skyPortal->lines)
			{
				if (wall.sky != nullptr)
				{
					FGameTexture* texture = nullptr;
					uint32_t fadeColor = 0;
					__try
					{
						texture = wall.sky->texture;
						fadeColor = wall.sky->fadecolor.d;
					}
					__except (EXCEPTION_EXECUTE_HANDLER)
					{
						texture = nullptr;
						fadeColor = 0;
					}
					UpdateSceneSky(outView, texture, fadeColor, PTSkySourceType::Portal);
					outView.stats.skySurfaces++;
					return;
				}
			}
		}
	}

	bool ShouldCapturePortal(HWPortal* portal)
	{
		if (portal == nullptr)
		{
			return false;
		}

		switch (portal->GetType())
		{
		case PORTAL_WALL_MIRROR:
		case PORTAL_WALL_VIEW:
		case PORTAL_WALL_TO_SPRITE:
		case PORTAL_SECTOR_FLOOR:
		case PORTAL_SECTOR_CEILING:
			return true;
		default:
			return false;
		}
	}

	bool ShouldCaptureWallMirrorActors(HWPortal* portal)
	{
		return portal != nullptr && portal->GetType() == PORTAL_WALL_MIRROR;
	}

	unsigned int CountCapturablePortals(HWDrawInfo& di)
	{
		unsigned int count = 0;
		for (HWPortal* portal : di.Portals)
		{
			if (ShouldCapturePortal(portal))
			{
				count++;
			}
		}

		return count;
	}

	void CapturePortalsRecursive(HWDrawInfo& di, SceneView& outView, CaptureState& state)
	{
		const int maxDepth = std::max(0, std::min((int)nri_ptportaldepth, 8));
		if (state.renderState == nullptr)
		{
			return;
		}

		if (gPortalCaptureDepth >= maxDepth)
		{
			outView.stats.portalCapturesSkipped += CountCapturablePortals(di);
			return;
		}

		gPortalCaptureDepth++;
		MergeSkyFromPortals(di, outView);

		for (HWPortal* portal : di.Portals)
		{
			if (!ShouldCapturePortal(portal))
			{
				continue;
			}

			auto* scenePortal = static_cast<HWScenePortalBase*>(portal);
			HWDrawInfo* child = HWDrawInfo::StartDrawInfo(&di, di.Viewpoint, &di.VPUniforms);
			if (child == nullptr)
			{
				continue;
			}

			const bool setup = scenePortal->SetupForSceneCapture(child, *state.renderState);
			if (!setup)
			{
				child->EndDrawInfo();
				continue;
			}

			const int portalType = portal->GetType();
			const bool portalFlag = portalType == PORTAL_SECTOR_CEILING;
			{
				const ScopedPortalCaptureState portalCaptureState(child->Viewpoint.CameraActor, portalType);
				child->CreateScene(portalFlag);
			}

			SceneView childView;
			if (CaptureScene(*child, childView))
			{
				const float translation[3] = {
					(float)(di.Viewpoint.Pos.X - child->Viewpoint.Pos.X),
					(float)(di.Viewpoint.Pos.Z - child->Viewpoint.Pos.Z),
					(float)(di.Viewpoint.Pos.Y - child->Viewpoint.Pos.Y)
				};

				AppendTranslatedScene(childView, translation, outView);
			}

			scenePortal->ShutdownAfterSceneCapture(child, *state.renderState);
			child->EndDrawInfo();
		}

		gPortalCaptureDepth--;
	}
}

namespace nri_scene
{
void CapturePortalViews(HWDrawInfo& di, SceneView& outView)
{
	CaptureState state = {};
	state.renderState = screen != nullptr ? screen->RenderState() : nullptr;
	CapturePortalsRecursive(di, outView, state);
}

uint32_t VisitWallMirrorSceneChildren(HWDrawInfo& di, WallMirrorSceneVisitor visitor, void* user)
{
	FRenderState* renderState = screen != nullptr ? screen->RenderState() : nullptr;
	if (renderState == nullptr || visitor == nullptr)
	{
		return 0;
	}

	uint32_t visited = 0;
	for (HWPortal* portal : di.Portals)
	{
		if (!ShouldCaptureWallMirrorActors(portal))
		{
			continue;
		}

		auto* scenePortal = static_cast<HWScenePortalBase*>(portal);
		HWDrawInfo* child = HWDrawInfo::StartDrawInfo(&di, di.Viewpoint, &di.VPUniforms);
		if (child == nullptr)
		{
			continue;
		}

		const bool setup = scenePortal->SetupForSceneCapture(child, *renderState);
		if (!setup)
		{
			child->EndDrawInfo();
			continue;
		}

		{
			const ScopedPortalCaptureState portalCaptureState(child->Viewpoint.CameraActor, PORTAL_WALL_MIRROR);
			child->CreateScene(false);
		}

		visitor(*child, user);
		visited++;

		scenePortal->ShutdownAfterSceneCapture(child, *renderState);
		child->ClearOwnedPortals();
		child->EndDrawInfo();
	}

	return visited;
}
}
