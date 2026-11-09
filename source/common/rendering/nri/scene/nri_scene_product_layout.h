#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nri_scene
{
// Optional producer observations for canonical walls/flats/sprites composition.
// Ranges describe actual output, including indexed surfaces with unused vertices.
struct SceneProductRange { size_t first = 0; size_t count = 0; };
struct GeometryProductGroup
{
	SceneProductRange vertices, indices, primitives, materials;
};
struct GeometryProductLayout
{
	std::array<GeometryProductGroup, 3> groups = {};
	bool valid = false;
};
struct MaterialProductGroup
{
	SceneProductRange materials, textures;
};
struct MaterialProductLayout
{
	std::array<MaterialProductGroup, 3> groups = {};
	// BuildMaterials keys by source identity, not necessarily TextureUpload.key:
	// different sources can produce identical content keys and stay distinct.
	std::vector<uint64_t> textureLookupKeys;
	bool valid = false;
	void Clear() { groups = {}; textureLookupKeys.clear(); valid = false; }
};
}
