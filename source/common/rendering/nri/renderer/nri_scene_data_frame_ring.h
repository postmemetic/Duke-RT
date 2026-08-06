#pragma once

#include "nri_frame_resources.h"
#include "nri_scene_data_light_slot_cache.h"
#include "nri_scene_lights.h"

#include <algorithm>
#include <cstdint>

struct NRISceneDataFrameSlot
{
	NRIBufferResource reprojectionBuffer;
	NRIBufferResource visibleChunkBuffer;
	NRIBufferResource visibleFlatPlaneBuffer;
	NRIBufferResource spatialAbsenceBuffer;
	NRIBufferResource sceneInstanceBuffer;
	NRIBufferResource portalBuffer;
	NRIBufferResource runtimeLightBuffer;
	NRIBufferResource runtimeLightTileHeaderBuffer;
	NRIBufferResource runtimeLightTileIndexBuffer;
	NRIBufferResource emissivePrimitiveHeaderBuffer;
	NRIBufferResource emissivePrimitiveBuffer;
	NRIBufferResource emissivePrimitiveCdfBuffer;
	NRIBufferResource emissiveMaterialResponseBuffer;
	NRIBufferResource sectorLightHeaderBuffer;
	NRIBufferResource sectorLightBuffer;

	SceneBufferDebugStats reprojectionStats = { "SceneDataSlotReprojection" };
	SceneBufferDebugStats visibleChunkStats = { "SceneDataSlotVisibleChunk" };
	SceneBufferDebugStats visibleFlatPlaneStats = { "SceneDataSlotVisibleFlatPlane" };
	SceneBufferDebugStats spatialAbsenceStats = { "SceneDataSlotSpatialAbsence" };
	SceneBufferDebugStats sceneInstanceStats = { "SceneDataSlotSceneInstance" };
	SceneBufferDebugStats portalStats = { "SceneDataSlotPortal" };
	SceneBufferDebugStats runtimeLightStats = { "SceneDataSlotRuntimeLight" };
	SceneBufferDebugStats runtimeLightTileHeaderStats = { "SceneDataSlotRuntimeLightTileHeader" };
	SceneBufferDebugStats runtimeLightTileIndexStats = { "SceneDataSlotRuntimeLightTileIndex" };
	SceneBufferDebugStats emissivePrimitiveHeaderStats = { "SceneDataSlotEmissivePrimitiveHeader" };
	SceneBufferDebugStats emissivePrimitiveStats = { "SceneDataSlotEmissivePrimitive" };
	SceneBufferDebugStats emissivePrimitiveCdfStats = { "SceneDataSlotEmissivePrimitiveCdf" };
	SceneBufferDebugStats emissiveMaterialResponseStats = { "SceneDataSlotEmissiveMaterialResponse" };
	SceneBufferDebugStats sectorLightHeaderStats = { "SceneDataSlotSectorLightHeader" };
	SceneBufferDebugStats sectorLightStats = { "SceneDataSlotSectorLight" };

	uint64_t snapshotGeneration = 0;
	uint64_t sceneInstanceHash = 0;
	uint64_t tlasInstanceHash = 0;
	uint64_t portalHash = 0;
	uint32_t sceneInstanceCount = 0;
	uint32_t tlasInstanceCount = 0;
	uint32_t portalCount = 0;
	uint64_t emissiveSamplingPayloadHash = 0;
	bool emissiveSamplingPayloadValid = false;
	uint32_t emissivePrimitiveCount = 0;
	uint32_t emissiveDominantPrimitive = UINT32_MAX;
	uint32_t emissiveDominantTile = 0;
	uint32_t emissiveDominantFlags = 0;
	uint32_t emissiveDominantDataSource = 0;
	float emissiveTotalPower = 0.0f;
	float emissiveDominantPower = 0.0f;
	std::vector<NRIEmissivePrimitiveDebugRecord> emissivePrimitiveDebugRecords;
	NRISceneDataLightSlotReuseState lightReuse;

	uint64_t UsedBytes() const
	{
		return
			reprojectionBuffer.usedSize +
			visibleChunkBuffer.usedSize +
			visibleFlatPlaneBuffer.usedSize +
			spatialAbsenceBuffer.usedSize +
			sceneInstanceBuffer.usedSize +
			portalBuffer.usedSize +
			runtimeLightBuffer.usedSize +
			runtimeLightTileHeaderBuffer.usedSize +
			runtimeLightTileIndexBuffer.usedSize +
			emissivePrimitiveHeaderBuffer.usedSize +
			emissivePrimitiveBuffer.usedSize +
			emissivePrimitiveCdfBuffer.usedSize +
			emissiveMaterialResponseBuffer.usedSize +
			sectorLightHeaderBuffer.usedSize +
			sectorLightBuffer.usedSize;
	}

	uint64_t CapacityBytes() const
	{
		return
			reprojectionBuffer.size +
			visibleChunkBuffer.size +
			visibleFlatPlaneBuffer.size +
			spatialAbsenceBuffer.size +
			sceneInstanceBuffer.size +
			portalBuffer.size +
			runtimeLightBuffer.size +
			runtimeLightTileHeaderBuffer.size +
			runtimeLightTileIndexBuffer.size +
			emissivePrimitiveHeaderBuffer.size +
			emissivePrimitiveBuffer.size +
			emissivePrimitiveCdfBuffer.size +
			emissiveMaterialResponseBuffer.size +
			sectorLightHeaderBuffer.size +
			sectorLightBuffer.size;
	}

	uint32_t GrowEventsLastFrame() const
	{
		return
			reprojectionStats.growEventsLastFrame +
			visibleChunkStats.growEventsLastFrame +
			visibleFlatPlaneStats.growEventsLastFrame +
			spatialAbsenceStats.growEventsLastFrame +
			sceneInstanceStats.growEventsLastFrame +
			portalStats.growEventsLastFrame +
			runtimeLightStats.growEventsLastFrame +
			runtimeLightTileHeaderStats.growEventsLastFrame +
			runtimeLightTileIndexStats.growEventsLastFrame +
			emissivePrimitiveHeaderStats.growEventsLastFrame +
			emissivePrimitiveStats.growEventsLastFrame +
			emissivePrimitiveCdfStats.growEventsLastFrame +
			emissiveMaterialResponseStats.growEventsLastFrame +
			sectorLightHeaderStats.growEventsLastFrame +
			sectorLightStats.growEventsLastFrame;
	}
};
