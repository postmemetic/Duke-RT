#include "nri_scene_material_product.h"
#include "nri_material_uniform_ranges.h"

#include <algorithm>

NRIMaterialProductPatchResult NRISceneMaterialProduct::BuildTransformed(
	const std::vector<nri_scene::MaterialData>& base,
	const std::vector<nri_scene::MaterialRowRange>& uniformRanges,
	std::vector<nri_scene::MaterialData>& output,
	void* context, TransformRow transformRow, TransformReference transformReference,
	bool validate)
{
	NRIMaterialProductPatchResult result;
	result.valid = true;
	result.changed = true;
	result.fullRebuild = mQuarantined || !NRIValidMaterialUniformRanges(uniformRanges, base.size());
	if (result.fullRebuild)
	{
		output = base;
		transformReference(context, output);
		result.evaluatedRows = result.copiedRows = base.size();
		return result;
	}
	output.resize(base.size());
	size_t rangeIndex = 0;
	for (size_t index = 0; index < base.size();)
	{
		size_t count = 1;
		if (rangeIndex < uniformRanges.size() && uniformRanges[rangeIndex].first == index)
		{
			count = uniformRanges[rangeIndex++].count;
			result.uniformRows += count;
		}
		auto row = base[index];
		transformRow(context, index, row);
		++result.evaluatedRows;
		std::fill_n(output.begin() + index, count, row);
		result.copiedRows += count;
		index += count;
	}
	if (validate)
	{
		result.validationChecked = true;
		mReference = base;
		transformReference(context, mReference);
		if (!EqualRows(output.data(), mReference.data(), base.size()))
		{
			result.validationMismatch = true;
			result.fullRebuild = true;
			mQuarantined = true;
			output = mReference;
		}
	}
	return result;
}
