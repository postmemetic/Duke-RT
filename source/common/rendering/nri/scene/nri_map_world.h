#pragma once

#include "nri_scene_bridge.h"
#include "nri_surface_identity.h"

#include <cstdint>
#include <vector>

struct MapRecord;

namespace nri_scene
{
enum class PTMapChunkKind : uint32_t
{
	Sector = 0,
};

enum class PTPortalKind : uint32_t
{
	WallMirror = 0,
	WallView,
	WallToSprite,
	SectorFloorStack,
	SectorCeilingStack,
	SectorFloorMirror,
	SectorCeilingMirror,
};

enum class PTMapSurfaceKind : uint32_t
{
	Floor = 0,
	Ceiling,
	WallOneSided,
	WallUpper,
	WallMiddle,
	WallLower,
	Portal,
};

struct PTMapSurfaceKey
{
	uint32_t primary = UINT32_MAX;
	uint32_t secondary = UINT32_MAX;
};

struct PTMapSurface
{
	SurfaceRef surface;
	PTMapSurfaceKind kind = PTMapSurfaceKind::Floor;
	PTMapSurfaceKey key = {};
	uint32_t chunkIndex = UINT32_MAX;
};

struct PTMapChunkBounds
{
	bool valid = false;
	float min[3] = {};
	float max[3] = {};
	float center[3] = {};
	float radius = 0.0f;
};

struct PTMapChunk
{
	PTMapChunkKind kind = PTMapChunkKind::Sector;
	uint32_t chunkIndex = UINT32_MAX;
	int32_t sectorIndex = -1;
	UpdatePartitionIdentity updatePartitionIdentity;
	RayTracingGeometryIdentity currentGeometryIdentity;
	RayTracingInstanceIdentity currentInstanceIdentity;
	uint32_t localSpaceIndex = UINT32_MAX;
	uint32_t firstSurface = 0;
	uint32_t surfaceCount = 0;
	uint32_t triangleCount = 0;
	PTMapChunkBounds bounds;
};

struct PTMapLocalSpace
{
	uint32_t localSpaceIndex = UINT32_MAX;
	int32_t anchorSectorIndex = -1;
	uint32_t chunkCount = 0;
};

struct PTMapPortalTarget
{
	int32_t sectorIndex = -1;
	int32_t wallIndex = -1;
	uint32_t chunkIndex = UINT32_MAX;
	uint32_t localSpaceIndex = UINT32_MAX;
};

struct PTMapPortal
{
	uint32_t portalIndex = UINT32_MAX;
	PTPortalKind kind = PTPortalKind::WallMirror;
	uint32_t sourceChunkIndex = UINT32_MAX;
	uint32_t sourceLocalSpaceIndex = UINT32_MAX;
	int32_t sourceSectorIndex = -1;
	int32_t sourceWallIndex = -1;
	int32_t sourcePlane = -1;
	uint32_t sourceSurfaceIndex = UINT32_MAX;
	int32_t portalNum = -1;
	uint32_t firstTarget = 0;
	uint32_t targetCount = 0;
	double delta[3] = {};
	bool runtimeBoundTarget = false;
};

struct PTMapWorldStats
{
	uint32_t sectorCount = 0;
	uint32_t sectionCount = 0;
	uint32_t chunkCount = 0;
	uint32_t localSpaceCount = 0;
	uint32_t surfaceCount = 0;
	uint32_t wallSurfaceCount = 0;
	uint32_t flatSurfaceCount = 0;
	uint32_t portalSurfaceCount = 0;
	uint32_t skySurfaceCount = 0;
	uint32_t triangleCount = 0;
	uint32_t portalCount = 0;
	uint32_t portalTargetCount = 0;
	uint32_t wallPortalCount = 0;
	uint32_t sectorPortalCount = 0;
	uint32_t mirrorPortalCount = 0;
	uint32_t runtimePortalCount = 0;
};

struct PTMapWorld
{
	MapRecord* level = nullptr;
	uint64_t buildSerial = 0;
	uint64_t topologyRevision = 0;
	bool valid = false;
	std::vector<PTMapChunk> chunks;
	std::vector<uint32_t> sectorChunkLookup;
	std::vector<PTMapSurface> surfaces;
	std::vector<PTMapLocalSpace> localSpaces;
	std::vector<PTMapPortal> portals;
	std::vector<PTMapPortalTarget> portalTargets;
	PTMapWorldStats stats;

	void Reset();
};

SceneDebugStats CollectMapWorldDebugStats(const PTMapWorld& mapWorld);
void BuildMapSceneView(const PTMapWorld& mapWorld, SceneView& outView, const SceneView* preservedSkyView = nullptr);
void BuildMapChunkSceneView(const PTMapWorld& mapWorld, const PTMapChunk& chunk, SceneView& outView, const SceneView* preservedSkyView = nullptr);
int32_t FindMapWorldLocalSpaceIndex(const PTMapWorld& mapWorld, uint32_t chunkIndex);
int32_t FindMapWorldPortalIndex(const PTMapWorld& mapWorld, const SurfaceProvenance& provenance);
}
