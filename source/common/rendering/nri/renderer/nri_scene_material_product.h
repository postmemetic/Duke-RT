#pragma once

#include "../scene/nri_material_bridge.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct NRIMaterialProductPatchResult
{
	bool valid = false;
	bool changed = false;
	bool persistentChanged = false;
	bool dynamicChanged = false;
	bool fullRebuild = false;
	bool validationChecked = false;
	bool validationMismatch = false;
	size_t evaluatedRows = 0;
	size_t changedRows = 0;
	size_t copiedRows = 0;
	size_t dirtyRanges = 0;
	size_t preservedRows = 0;
};

// This owner retains scratch, never material authority. Every candidate row is
// reconstructed from this call's clean, slot-resolved base. No cross-frame
// policy/metadata hash or previously overridden material is used as an input.
class NRISceneMaterialProduct
{
public:
	using TransformRow = void (*)(void*, size_t, nri_scene::MaterialData&);
	using TransformReference = void (*)(void*, std::vector<nri_scene::MaterialData>&);

	NRIMaterialProductPatchResult Refresh(
		const std::vector<nri_scene::MaterialData>& base,
		size_t staticCount,
		size_t persistentCount,
		const std::vector<uint32_t>& deferredIndices,
		std::vector<nri_scene::MaterialData>& combined,
		std::vector<nri_scene::MaterialData>& persistent,
		std::vector<nri_scene::MaterialData>& dynamic,
		void* context,
		TransformRow transformRow,
		TransformReference transformReference,
		bool enabled,
		bool validate);

	bool Quarantined() const { return mQuarantined; }
	static bool EqualRows(const nri_scene::MaterialData* a, const nri_scene::MaterialData* b, size_t count);

private:
	std::vector<uint32_t> mDeferredMarks;
	std::vector<nri_scene::MaterialData> mReference;
	uint32_t mMarkGeneration = 0;
	uint32_t mValidationCount = 0;
	bool mValidationWasEnabled = false;
	bool mQuarantined = false;
};
