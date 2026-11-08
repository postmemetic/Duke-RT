#pragma once

#include "../scene/nri_material_bridge_ranges.h"

inline bool NRIValidMaterialUniformRanges(const std::vector<nri_scene::MaterialRowRange>& ranges, size_t rows)
{
	size_t end = 0;
	for (const auto range : ranges)
	{
		if (range.count == 0 || range.first < end || range.first > rows || range.count > rows - range.first)
			return false;
		end = (size_t)range.first + range.count;
	}
	return true;
}
