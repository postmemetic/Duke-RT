#pragma once

#include "nri_scene_bridge.h"

#include <vector>

namespace nri_scene
{
struct SceneVertex
{
	float position[3] = {};
	float prevPosition[3] = {};
	float uv[2] = {};
};
static_assert(sizeof(SceneVertex) == 32, "SceneVertex must match Shared.hlsli.");

struct PrimitiveData
{
	uint32_t indices[3] = {};
	uint32_t materialIndex = 0;
	float uv0[2] = {};
	float uv1[2] = {};
	float uv2[2] = {};
	float normal[3] = {};
	uint32_t flags = 0;
	uint32_t portalIndex = UINT32_MAX;
	uint32_t reserved0 = UINT32_MAX;
	uint32_t smoothNormals[2] = {};
	uint32_t temporalSurfaceId[2] = {};
	uint32_t temporalGeneration = 0;
	uint32_t temporalFlags = 0;
};
static_assert(sizeof(PrimitiveData) == 88, "PrimitiveData must match SceneShadowContracts.hlsli.");

struct GeometryData
{
	std::vector<SceneVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<PrimitiveData> primitives;
	std::vector<SurfaceProvenance> primitiveProvenance;
};

struct GeometryBuildTraceStats
{
	double wallMs = 0.0;
	double flatMs = 0.0;
	double spriteMs = 0.0;
	uint32_t wallSurfaces = 0;
	uint32_t flatSurfaces = 0;
	uint32_t spriteSurfaces = 0;
	uint32_t indexedSurfaces = 0;
	uint32_t triangleFanSurfaces = 0;
	uint32_t spriteStripSurfaces = 0;
	uint32_t skippedSurfaces = 0;
	uint32_t sourceVertexCount = 0;
	uint32_t sourceIndexCount = 0;
	uint32_t outputVertexCount = 0;
	uint32_t outputIndexCount = 0;
	uint32_t outputPrimitiveCount = 0;
	uint32_t vertexCapacityGrowths = 0;
	uint32_t indexCapacityGrowths = 0;
	uint32_t primitiveCapacityGrowths = 0;
	uint32_t provenanceCapacityGrowths = 0;
};

void ClearGeometryRetainingCapacity(GeometryData& geometry);
void BuildGeometry(const SceneView& sceneView, GeometryData& outGeometry, GeometryBuildTraceStats* traceStats = nullptr, bool retainOutputCapacity = false);
}
