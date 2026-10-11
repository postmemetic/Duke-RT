#pragma once

#include <cstdint>

namespace nri_scene
{
struct SurfaceRef;
void BuildFluidFloorCarrierNormals(SurfaceRef& surface);
// Includes the complete connected corner fan, including neighbors beyond the
// chunk's immediate wall adjacency, so animated shading cannot remain stale.
uint64_t ComputeFluidFloorNormalSignature(int32_t sectorIndex);
}
