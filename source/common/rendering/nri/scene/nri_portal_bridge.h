#pragma once

#include "nri_scene_bridge.h"

namespace nri_scene
{
void CapturePortalViews(HWDrawInfo& di, SceneView& outView);
using WallMirrorSceneVisitor = void (*)(HWDrawInfo& child, void* user);
uint32_t VisitWallMirrorSceneChildren(HWDrawInfo& di, WallMirrorSceneVisitor visitor, void* user);
}
