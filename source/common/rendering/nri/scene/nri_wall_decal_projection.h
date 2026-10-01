#pragma once

#include "nri_wall_decal_types.h"

#include <algorithm>
#include <cmath>

namespace nri_scene
{
// Render-space quad: bottom-left, bottom-right, constant bottom/top Y. UVs
// are the clipped HW sprite UVs, so offsets, flips and ceiling clipping agree.
inline bool BuildWallDecalProjection(float x0, float z0, float x1, float z1,
	float bottom, float top, float leftU, float rightU, float bottomV, float topV,
	WallDecalGpuData& decal)
{
	const float dx = x1 - x0;
	const float dz = z1 - z0;
	const float lengthSquared = dx * dx + dz * dz;
	const float height = top - bottom;
	if (!std::isfinite(lengthSquared) || !std::isfinite(height) ||
		!std::isfinite(leftU) || !std::isfinite(rightU) ||
		!std::isfinite(bottomV) || !std::isfinite(topV) ||
		lengthSquared <= 1.e-8f || height <= 1.e-6f)
		return false;
	const float du = (rightU - leftU) / lengthSquared;
	decal.worldToU[0] = dx * du;
	decal.worldToU[1] = 0.0f;
	decal.worldToU[2] = dz * du;
	decal.worldToU[3] = leftU - x0 * decal.worldToU[0] - z0 * decal.worldToU[2];
	decal.worldToV[1] = (topV - bottomV) / height;
	decal.worldToV[0] = decal.worldToV[2] = 0.0f;
	decal.worldToV[3] = bottomV - bottom * decal.worldToV[1];
	decal.uvBounds[0] = std::min(leftU, rightU);
	decal.uvBounds[1] = std::min(bottomV, topV);
	decal.uvBounds[2] = std::max(leftU, rightU);
	decal.uvBounds[3] = std::max(bottomV, topV);
	return std::isfinite(decal.worldToU[0]) && std::isfinite(decal.worldToU[2]) &&
		std::isfinite(decal.worldToU[3]) && std::isfinite(decal.worldToV[1]) &&
		std::isfinite(decal.worldToV[3]);
}
}
