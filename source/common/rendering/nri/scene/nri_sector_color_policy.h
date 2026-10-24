#pragma once

#include "nri_scene_surface_types.h"

namespace nri_scene
{
// A luminance-normalized target with independent replacement strength and output gain.
struct SectorEmissionHue
{
	float rgb[3] = {};
	float strength = 1.0f;
	float gain = 1.0f;
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
void BuildEmissionColorResponse(const SectorEmissionHue& hue, float response[4]);
void ApplyEmissionColorResponse(float color[3], const SectorEmissionHue& hue);
void PrintSectorColorPolicy(int sectorIndex = -1);
}
