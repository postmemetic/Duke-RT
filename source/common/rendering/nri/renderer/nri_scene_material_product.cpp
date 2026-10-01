#include "nri_scene_material_product.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

static_assert(std::is_trivially_copyable<nri_scene::MaterialData>::value, "Material row byte comparison requires a plain GPU payload.");
static_assert(sizeof(nri_scene::MaterialData) == 22 * sizeof(uint32_t), "Update exact material comparison when the shader row layout changes.");

bool NRISceneMaterialProduct::EqualRows(const nri_scene::MaterialData* a, const nri_scene::MaterialData* b, size_t count)
{
	return count == 0 || std::memcmp(a, b, count * sizeof(*a)) == 0;
}

NRIMaterialProductPatchResult NRISceneMaterialProduct::Refresh(
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
	bool validate)
{
	NRIMaterialProductPatchResult result;
	if (staticCount > base.size() || persistentCount > base.size() - staticCount)
		return result;
	const size_t dynamicOffset = staticCount + persistentCount;
	const size_t dynamicCount = base.size() - dynamicOffset;
	const bool stableLayout = combined.size() == base.size() &&
		persistent.size() == persistentCount && dynamic.size() == dynamicCount;
	const bool patch = enabled && !mQuarantined && stableLayout;
	if (!validate) mValidationCount = 0;
	if (validate && !mValidationWasEnabled) mValidationCount = 0;
	mValidationWasEnabled = validate;
	result.validationChecked = patch && validate && mValidationCount < 64;
	if (result.validationChecked) ++mValidationCount;
	result.fullRebuild = !patch;

	// Reference assembly precedes any edits so pending texture proxies retain
	// their complete published row, including flags, slot indices and emission.
	if (!patch || result.validationChecked)
	{
		mReference = base;
		transformReference(context, mReference);
		const size_t preservedCount = std::min(combined.size(), mReference.size());
		for (const uint32_t index : deferredIndices)
			if (index < preservedCount) mReference[index] = combined[index];
	}

	if (patch)
	{
		mDeferredMarks.resize(base.size(), 0);
		if (++mMarkGeneration == 0)
		{
			std::fill(mDeferredMarks.begin(), mDeferredMarks.end(), 0);
			mMarkGeneration = 1;
		}
		for (const uint32_t index : deferredIndices)
		{
			if (index < base.size())
			{
				mDeferredMarks[index] = mMarkGeneration;
				++result.preservedRows;
			}
		}

		bool precedingChanged = false;
		for (size_t index = 0; index < base.size(); ++index)
		{
			if (mDeferredMarks[index] == mMarkGeneration)
			{
				precedingChanged = false;
				continue;
			}
			auto candidate = base[index];
			transformRow(context, index, candidate);
			++result.evaluatedRows;
			if (EqualRows(&candidate, &combined[index], 1))
			{
				precedingChanged = false;
				continue;
			}
			combined[index] = candidate;
			++result.changedRows;
			++result.copiedRows;
			if (!precedingChanged) ++result.dirtyRanges;
			precedingChanged = true;
			if (index >= dynamicOffset)
			{
				dynamic[index - dynamicOffset] = candidate;
				result.dynamicChanged = true;
				++result.copiedRows;
			}
			else if (index >= staticCount)
			{
				persistent[index - staticCount] = candidate;
				result.persistentChanged = true;
				++result.copiedRows;
			}
		}
		result.changed = result.changedRows != 0;
		if (result.validationChecked)
		{
			// Compare the final combined payload AND both upload slices, including
			// IEEE float bits. Legacy float equality does not distinguish +/-0.
			result.validationMismatch =
				!EqualRows(combined.data(), mReference.data(), base.size()) ||
				(persistentCount != 0 && !EqualRows(persistent.data(), mReference.data() + staticCount, persistentCount)) ||
				(dynamicCount != 0 && !EqualRows(dynamic.data(), mReference.data() + dynamicOffset, dynamicCount));
			if (result.validationMismatch)
			{
				mQuarantined = true;
				result.fullRebuild = true;
			}
		}
	}

	if (result.fullRebuild)
	{
		result.changed = !stableLayout || result.validationMismatch ||
			!EqualRows(combined.data(), mReference.data(), base.size());
		if (!patch)
		{
			result.evaluatedRows = base.size();
			for (const uint32_t index : deferredIndices)
				if (index < std::min(combined.size(), base.size())) ++result.preservedRows;
		}
		if (result.changed)
		{
			combined = mReference;
			persistent.assign(combined.begin() + staticCount, combined.begin() + dynamicOffset);
			dynamic.assign(combined.begin() + dynamicOffset, combined.end());
			result.changedRows = base.size();
			result.copiedRows += base.size() + persistentCount + dynamicCount;
			result.persistentChanged = persistentCount != 0;
			result.dynamicChanged = true;
			result.dirtyRanges = base.empty() ? 0 : 1;
		}
	}
	result.valid = true;
	return result;
}
