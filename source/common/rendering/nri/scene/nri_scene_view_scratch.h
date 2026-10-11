#pragma once

#include "nri_scene_bridge.h"

namespace nri_scene
{
inline void ClearSceneViewRetainingCapacity(SceneView& view)
{
	// Default construction resets every scalar, borrowed pointer and sky field.
	// Swap only the owned vector storage back before publishing the empty view.
	SceneView empty;
	empty.opaqueWalls.swap(view.opaqueWalls);
	empty.opaqueFlats.swap(view.opaqueFlats);
	empty.opaqueSprites.swap(view.opaqueSprites);
	empty.opaqueWalls.clear();
	empty.opaqueFlats.clear();
	empty.opaqueSprites.clear();
	view = std::move(empty);
}
}
