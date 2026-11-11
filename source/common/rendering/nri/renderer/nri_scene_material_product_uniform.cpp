#include "nri_scene_material_product.h"
#include "nri_material_uniform_ranges.h"
#include "nri_cpu_detail_audit.h"
#include "c_cvars.h"

#include <algorithm>

CVAR(Bool, nri_ptmaterialsegmentreuse, false, 0)

NRIMaterialProductPatchResult NRISceneMaterialProduct::BuildTransformed(
	const std::vector<nri_scene::MaterialData>& base,
	const std::vector<nri_scene::MaterialRowRange>& uniformRanges,
	std::vector<nri_scene::MaterialData>& output,
	void* context, TransformRow transformRow, TransformReference transformReference,
	bool validate, bool retainOutput)
{
	const bool detail = PerfAuditCpuDetailActive();
	const bool segmentReuse = (bool)nri_ptmaterialsegmentreuse;
	const bool observeSegments = detail || validate;
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
	const bool compareOutput = retainOutput && output.size() == base.size();
	output.resize(base.size());
	bool precedingChanged = false;
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
		if (segmentReuse && compareOutput && count >= NRIMaterialSegmentMinimumRows)
		{
			if (observeSegments) ++result.segments.attempts;
			if (NRIEqualUniformMaterialOutput(row, output.data() + index, count,
				observeSegments ? &result.segments : nullptr))
			{
				if (observeSegments) { ++result.segments.hits; result.segments.reusedRows += count; }
				precedingChanged = false;
				index += count;
				continue;
			}
			if (observeSegments) result.segments.fallbackRows += count;
		}
		if (!compareOutput)
		{
			std::fill_n(output.begin() + index, count, row);
			result.copiedRows += count;
		}
		else for (size_t target = index; target < index + count; ++target)
		{
			result.outputComparedRows += detail;
			if (EqualRows(&row, &output[target], 1)) { precedingChanged = false; continue; }
			output[target] = row;
			++result.copiedRows;
			if (!precedingChanged) ++result.dirtyRanges;
			precedingChanged = true;
		}
		index += count;
	}
	result.changedRows = result.copiedRows;
	result.changed = result.copiedRows != 0;
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
			result.copiedRows += base.size();
			result.changedRows = base.size();
			result.changed = true;
		}
	}
	return result;
}
