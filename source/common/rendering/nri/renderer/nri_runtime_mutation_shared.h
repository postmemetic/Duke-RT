#pragma once

#include "../scene/nri_map_builder.h"
#include "../scene/nri_map_world.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <vector>

namespace nri_runtime_mutation
{
uint64_t RuntimeMutationHashCombine64(uint64_t hash, uint64_t value);
uint32_t RuntimeMutationFloatBits(float value);
void NudgeCapturedSurface(nri_scene::SurfaceRef& surface, float depthNudge);
void NudgeBlindSpotReplacementFlats(nri_scene::SceneView& sceneView);
uint32_t CountSceneViewSurfaces(const nri_scene::SceneView& sceneView);
uint64_t HashMaterialBridgeSummary(const nri_scene::MaterialBridgeData& materials);
uint64_t HashResidentMaterialPayload(const nri_scene::MaterialBridgeData& materials);
void FilterMaterialOnlyReplacementSceneView(nri_scene::SceneView& sceneView, uint32_t reasonMask);
bool SceneViewHasSectorDrivenWallBands(const nri_scene::SceneView& sceneView);
bool TryBuildMergedSectorMaterialOnlyBridge(
	const nri_scene::SceneView& residentChunkView,
	const nri_scene::MaterialBridgeData& residentChunkMaterials,
	const nri_scene::SceneView& filteredLiveChunkView,
	const nri_scene::MaterialBridgeData& filteredLiveMaterials,
	nri_scene::MaterialBridgeData& outMergedMaterials);
bool TryBuildMergedSectorMaterialOnlySceneView(
	const nri_scene::SceneView& residentChunkView,
	const nri_scene::SceneView& filteredLiveChunkView,
	nri_scene::SceneView& outMergedSceneView);
bool IsChunkMarkedVisible(const std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex);
uint64_t ComputeRecurringChunkStateSignature(
	uint32_t reasonMask,
	uint32_t liveWallCount,
	uint32_t liveFlatCount,
	uint32_t liveTriangleCount,
	uint32_t liveMaterialCount);
uint64_t ComputeAnimatedMaterialSignature(const nri_scene::SceneView& sceneView);
bool ComputeAnimatedMaterialSignatureWithBindings(
	const nri_scene::SceneView& sceneView,
	const std::vector<FGameTexture*>& bindings,
	uint64_t& outSignature);
uint64_t ComputeAnimatedGeometrySignature(const nri_scene::SceneView& sceneView);
uint64_t ComputeExactGeometrySignature(const nri_scene::SceneView& sceneView);
bool SceneViewUsesHardwareCanvasTexture(const nri_scene::SceneView& sceneView);
bool ChunkHasUnresolvedAuthoredTextures(const nri_scene::PTMapChunk& chunk);
}
