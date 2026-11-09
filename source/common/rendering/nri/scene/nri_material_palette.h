#pragma once

#include "nri_material_bridge.h"

namespace nri_scene
{
// Palette sources are stable during one render/preload operation. Resolve once
// on first use, then share immutable bytes with every material builder on this
// thread. Every new view/preload tick must have its own scope, even at the same
// simulation tick. Calls without a scope retain full source validation.
class ScopedMaterialPaletteSnapshot
{
public:
	ScopedMaterialPaletteSnapshot(bool enabled, bool validate);
	~ScopedMaterialPaletteSnapshot();
	ScopedMaterialPaletteSnapshot(const ScopedMaterialPaletteSnapshot&) = delete;
	ScopedMaterialPaletteSnapshot& operator=(const ScopedMaterialPaletteSnapshot&) = delete;

private:
	friend bool ResolveMaterialPalette(MaterialBridgeData& outMaterials);
	ScopedMaterialPaletteSnapshot* mPrevious = nullptr;
	ImmutableBytePayload mPayload;
	bool mEnabled = false;
	bool mValidate = false;
};

// Returns true only when a new immutable lookup payload was built. Snapshot and
// global payload-cache hits both return false, preserving bridge build stats.
bool ResolveMaterialPalette(MaterialBridgeData& outMaterials);
}
