#pragma once

#include "nri_scene_surface_types.h"
#include "nri_wall_decal_types.h"

struct HWDrawInfo;

namespace nri_scene
{
struct WallDecalCapture
{
	WallDecalGpuData projection;
	SurfaceRef source;
};

// Enumerates live actors independently of primary-view visibility. Each entry
// is attached to one exact live wall; unattached transparency stays unsupported.
void CaptureWallDecals(HWDrawInfo& view, std::vector<WallDecalCapture>& decals);
}
