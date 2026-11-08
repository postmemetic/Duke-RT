#pragma once
#include "nri_material_bridge.h"

namespace nri_scene
{
// Hash is only a lookup accelerator. Equality checks every GPU, metadata,
// ordered texture and palette field except the separately owned actor index.
uint64_t HashMaterialPayload(const MaterialBridgeData& bridge, uint64_t* bytesHashed = nullptr);
bool MaterialPayloadsEquivalent(const MaterialBridgeData& left, const MaterialBridgeData& right);
}
