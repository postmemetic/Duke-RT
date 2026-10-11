#pragma once

#include "../scene/nri_wall_decal_types.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"
#include "../scene/nri_wall_decals.h"

#include <vector>

namespace nri { struct Descriptor; }
class NRIRenderer;

// Live decals own material projectors, never acceleration structures. A fresh
// capture reconciles actor deletion, switches and moving receivers every frame.
class NRIWallDecals
{
public:
	void Reset();
	void Capture(HWDrawInfo& drawInfo);
	void AppendTextureKeys(std::vector<uint64_t>& keys) const;
	bool ResolveTextureDescriptors(NRIRenderer& renderer, std::vector<nri::Descriptor*>& descriptors,
		bool stableSlots, uint32_t legacyTextureCount);
	const std::vector<nri_scene::WallDecalHeaderGpuData>& Headers() const { return mHeaders; }
	const std::vector<nri_scene::WallDecalGpuData>& Records() const { return mRecords; }
	bool MaterialChanged() const { return mMaterialChanged; }
	bool NeedsTextureResolve() const { return mNeedsTextureResolve; }

private:
	std::vector<nri_scene::WallDecalHeaderGpuData> mHeaders;
	std::vector<nri_scene::WallDecalGpuData> mRecords;
	nri_scene::MaterialBridgeData mMaterials;
	nri_scene::SceneView mMaterialScene;
	std::vector<nri_scene::WallDecalCapture> mCaptureScratch;
	std::vector<uint32_t> mBaseTextureIndices;
	std::vector<uint32_t> mResolvedTextureIndices;
	std::vector<uint32_t> mRecordMaterialIndices;
	uint64_t mMaterialSignature = 0;
	bool mMaterialChanged = false;
	bool mNeedsTextureResolve = false;
};
