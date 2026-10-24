#pragma once

#include "nri_scene_surface_types.h"

namespace nri_scene
{
// A luminance-normalized replacement hue. Zero means retain authored emission.
struct SectorEmissionHue
{
	float rgb[3] = {};
	int sectorIndex = -1;
	int palette = -1;
	int portalHops = 0;
	bool Active() const { return sectorIndex >= 0; }
};

SectorEmissionHue ResolveSectorEmissionHue(int sectorIndex, bool neighbors = true);
bool TransfersSectorPalette(int sectorIndex, int palette);
uint64_t SectorColorMaterialPolicySignature();
uint32_t ResolveSectorRenderPalette(uint32_t palette, SurfaceSourceType source, int sectorIndex, uint32_t flags);
int FindSectorColorAnchor(const float renderPosition[3]);
void ReplaceEmissionHue(float color[3], const float hue[3]);
void PrintSectorColorPolicy(int sectorIndex = -1);
}
