#pragma once

#include "nri_map_world.h"

#include <cstdint>
#include <vector>

namespace nri_scene
{
enum PTMapChunkMutationReasonBits : uint32_t
{
	PTMapChunkMutationReason_None = 0,
	PTMapChunkMutationReason_SectorGeometry = 1 << 0,
	PTMapChunkMutationReason_SectorMaterial = 1 << 1,
	PTMapChunkMutationReason_WallGeometry = 1 << 2,
	PTMapChunkMutationReason_WallMaterial = 1 << 3,
	PTMapChunkMutationReason_SectorDirty = 1 << 4,
	PTMapChunkMutationReason_SectionDirty = 1 << 5,
	PTMapChunkMutationReason_Dragged = 1 << 6,
};

struct PTMapWallMutationSnapshot
{
	DVector2 pos = {};
	int32_t point2 = -1;
	int32_t nextwall = -1;
	int32_t nextsector = -1;
	uint16_t cstat = 0;
	uint8_t portalflags = 0;
	int32_t walltexture = -1;
	int32_t overtexture = -1;
	float xpan = 0.0f;
	float ypan = 0.0f;
	int32_t xrepeat = 0;
	int32_t yrepeat = 0;
	int32_t pal = 0;
	int32_t shade = 0;
	double adjacentFloorz = 0.0;
	double adjacentCeilingz = 0.0;
	uint16_t adjacentFloorstat = 0;
	uint16_t adjacentCeilingstat = 0;
	int16_t adjacentFloorheinum = 0;
	int16_t adjacentCeilingheinum = 0;
	uint8_t adjacentPortalflags = 0;
	uint16_t nextWallCstat = 0;
	int32_t nextWallTexture = -1;
	int32_t nextOverTexture = -1;
	int32_t nextWallPal = 0;
	int32_t nextWallShade = 0;
};

struct PTMapChunkMutationBaseline
{
	int32_t sectorIndex = -1;
	uint64_t signature = 0;
	uint64_t fluidNormalSignature = 0;
	double floorz = 0.0;
	double ceilingz = 0.0;
	uint16_t floorstat = 0;
	uint16_t ceilingstat = 0;
	int16_t floorheinum = 0;
	int16_t ceilingheinum = 0;
	uint8_t portalflags = 0;
	int32_t floortexture = -1;
	int32_t ceilingtexture = -1;
	float floorxpan = 0.0f;
	float floorypan = 0.0f;
	float ceilingxpan = 0.0f;
	float ceilingypan = 0.0f;
	int32_t floorpal = 0;
	int32_t ceilingpal = 0;
	int32_t floorshade = 0;
	int32_t ceilingshade = 0;
	std::vector<int32_t> sectionIndices;
	std::vector<PTMapWallMutationSnapshot> walls;
};

struct PTMapChunkMutationAnalysis
{
	uint64_t signature = 0;
	uint32_t reasonMask = PTMapChunkMutationReason_None;
	uint32_t sectionDirtyCount = 0;
	bool sectorDirty = false;
	bool dragged = false;
	bool signatureChanged = false;
};

struct PTMapBuildOptions
{
};

void NotifyLevelGeometryReady();
uint64_t GetPendingLevelGeometryBuildSerial();
bool BuildMapWorld(PTMapWorld& outWorld, const PTMapBuildOptions& options = {});
bool BuildLiveMapChunkWorld(const PTMapChunk& chunk, PTMapWorld& outWorld, PTMapWorldStats* outStats = nullptr, const PTMapBuildOptions& options = {});
bool BuildLiveMapChunkSceneView(const PTMapChunk& chunk, SceneView& outView, PTMapWorldStats* outStats = nullptr, const PTMapBuildOptions& options = {});
uint64_t ComputeMapChunkGeometrySignature(const PTMapChunk& chunk);
bool CaptureMapChunkMutationBaseline(const PTMapChunk& chunk, PTMapChunkMutationBaseline& outBaseline);
bool AnalyzeMapChunkMutation(const PTMapChunk& chunk, const PTMapChunkMutationBaseline& baseline, PTMapChunkMutationAnalysis& outAnalysis);
}
