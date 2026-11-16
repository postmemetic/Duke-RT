#pragma once

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "nri_scene_instance_visibility.h"
#include <cstdint>
#include <unordered_map>
#include <utility>

class NRIPersistentVoxelResidency;
struct NRIPersistentVoxelSettings;
struct NRIPersistentVoxelTlasServices;
struct NRIActorOccurrencePolicyContext;
class NRIRenderDevice;
class NRISceneTextureResidency;

struct NRIActorRepresentationStats
{
	uint32_t candidates = 0, voxel = 0, sprite = 0, transient = 0;
	uint32_t suppressed = 0, unavailable = 0, duplicates = 0;
	uint64_t vertexBytes = 0, indexBytes = 0, primitiveBytes = 0;
};

// A frame-local selection, made before material/geometry upload. Sprite
// descriptors are bounded by live engine owners; no per-animation GPU cache.
class NRIActorRepresentations
{
public:
	// Resolve cheap texture dependencies before residency can expire a held voxel.
	void PrepareTextures(uint32_t frameIndex, NRIPersistentVoxelResidency& voxels,
		NRIRenderDevice& device, NRISceneTextureResidency& textures, void* user,
		void (*buildMaterials)(void*, nri_scene::SceneView&, nri_scene::MaterialBridgeData&));
	bool Build(uint32_t frameIndex, NRIPersistentVoxelResidency& voxels,
		const NRIPersistentVoxelSettings& settings,
		const NRIPersistentVoxelTlasServices& services,
		const NRIActorOccurrencePolicyContext& occurrenceContext,
		const NRISceneInstanceVisibilityContext& visibilityContext,
		const nri_scene::SceneView* dynamicScene, int32_t separateReflectionActor);
	void FinishGeometry();
	void RecordAssembly(uint32_t frameIndex, const NRIPersistentVoxelResidency& voxels,
		const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<std::pair<uint32_t, uint32_t>>& publishedRanges, bool ready) const;
	nri_scene::SceneView& Scene() { return mScene; }
	nri_scene::GeometryData& Geometry() { return mGeometry; }
	nri_scene::MaterialBridgeData& Materials() { return mMaterials; }
	const NRIActorRepresentationStats& Stats() const { return mStats; }
	const std::vector<uint32_t>& PriorityMaterials() const { return mPriorityMaterials; }
	bool HasIndirectSprites(uint32_t frameIndex) const;
private:
	struct SelectedSprite { uint32_t ownerChunk = UINT32_MAX; bool indirectOnly = false; };
	struct Decision
	{
		uint64_t identity = 0, world = 0, lifetime = 0, desiredMesh = 0, desiredMaterial = 0;
		uint64_t selectedMesh = 0, selectedMaterial = 0;
		uint32_t waitFrames = 0;
		int32_t actorIndex = -1;
		const char* selection = "unavailable";
	};
	uint32_t mFrameIndex = UINT32_MAX;
	std::vector<Decision> mDecisions;
	std::vector<uint32_t> mPriorityMaterials;
	std::unordered_map<int32_t, SelectedSprite> mSelected;
	nri_scene::SceneView mScene;
	nri_scene::GeometryData mGeometry;
	nri_scene::MaterialBridgeData mMaterials;
	NRIActorRepresentationStats mStats;
};
