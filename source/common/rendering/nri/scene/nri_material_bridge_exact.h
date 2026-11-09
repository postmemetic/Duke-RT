#pragma once
#include "nri_material_bridge.h"
namespace nri_scene
{
// Exact semantic fields, including IEEE float bits; ignores C++ object padding.
// Texture/palette shared backing is immutable; equal backing is exact authority.
bool MaterialBridgesExactlyEqual(const MaterialBridgeData& a, const MaterialBridgeData& b);
}
