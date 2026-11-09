#pragma once

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_product_layout.h"

#include <unordered_map>

struct NRIEmissiveTailScratch
{
	nri_scene::SceneView view;
	nri_scene::GeometryData geometry;
	nri_scene::MaterialBridgeData materials;
	nri_scene::MaterialProductLayout materialLayout;
	std::unordered_map<uint64_t, uint32_t> textureLookup;
	std::vector<uint32_t> liveTextureRemap;
	std::vector<uint32_t> tailTextureRemap;
	void Clear();
};

// Splice actual producer ranges in canonical wall/flat/sprite order. No texture
// resolution or material policy is run here; only current owned products enter.
// False means unsupported input; callers must publish the independent full build.
bool ComposeNRIEmissiveTailProducts(
	const nri_scene::SceneView& liveView, const nri_scene::GeometryData& liveGeometry,
	const nri_scene::GeometryProductLayout& liveGeometryLayout,
	const nri_scene::MaterialBridgeData& liveMaterials, const nri_scene::MaterialProductLayout& liveMaterialLayout,
	const nri_scene::SceneView& tailView, const nri_scene::GeometryData& tailGeometry,
	const nri_scene::GeometryProductLayout& tailGeometryLayout,
	const nri_scene::MaterialBridgeData& tailMaterials, const nri_scene::MaterialProductLayout& tailMaterialLayout,
	NRIEmissiveTailScratch& scratch, nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials);

bool NRIEmissiveTailViewsEqual(const nri_scene::SceneView& lhs, const nri_scene::SceneView& rhs);
bool NRIEmissiveTailGeometryEqual(const nri_scene::GeometryData& lhs, const nri_scene::GeometryData& rhs);
bool NRIEmissiveTailMaterialsEqual(const nri_scene::MaterialBridgeData& lhs, const nri_scene::MaterialBridgeData& rhs);
