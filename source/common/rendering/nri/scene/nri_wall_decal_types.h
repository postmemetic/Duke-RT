#pragma once

#include <cstdint>

namespace nri_scene
{
// Matches shaders/Include/WallDecals.hlsli. Wall identity is map provenance,
// independent of material slots, chunks, BLASes and TLAS instances.
struct WallDecalHeaderGpuData
{
	uint32_t first = 0;
	uint32_t count = 0;
};

struct WallDecalGpuData
{
	float worldToU[4] = {};
	float worldToV[4] = {};
	float plane[4] = {};
	uint32_t textureIndex = UINT32_MAX;
	uint32_t paletteIndex = 0;
	uint32_t flags = 0;
	uint32_t actorIndex = UINT32_MAX;
	float alpha = 0.0f;
	float lightLevel = 1.0f;
	uint32_t wallIndex = UINT32_MAX;
	uint32_t sectorIndex = UINT32_MAX;
	float uvBounds[4] = {};
};
static_assert(sizeof(WallDecalHeaderGpuData) == 8, "Wall decal header shader contract");
static_assert(sizeof(WallDecalGpuData) == 96, "Wall decal record shader contract");
}
