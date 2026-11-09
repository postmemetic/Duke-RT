#pragma once
#include "nri_material_bridge.h"
namespace nri_scene
{
void AppendMaterialBridgeReference(const MaterialBridgeData& source, MaterialBridgeData& destination);
void AppendMaterialBridgeReference(const MaterialBridgeData& source, MaterialBridgeData& destination,
 std::unordered_map<uint64_t, uint32_t>& textureLookup);
}
