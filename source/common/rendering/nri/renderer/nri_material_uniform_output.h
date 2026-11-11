#pragma once

#include "../scene/nri_material_bridge.h"

#include <algorithm>
#include <cstring>
#include <limits>

struct NRIMaterialSegmentComparisonStats
{
	size_t attempts = 0;
	size_t hits = 0;
	size_t reusedRows = 0;
	size_t compareCalls = 0;
	size_t compareExtentRows = 0;
	size_t fallbackRows = 0;
};

constexpr size_t NRIMaterialSegmentMinimumRows = 16;

// No retained certificate: inspect this call's output against its freshly
// transformed representative. Each successful block extends the proven equal
// prefix. This replaces many tiny comparisons with contiguous library calls;
// it does not claim to avoid checking the logical output bytes.
inline bool NRIEqualUniformMaterialOutput(const nri_scene::MaterialData& row,
	const nri_scene::MaterialData* output, size_t count,
	NRIMaterialSegmentComparisonStats* stats)
{
	if (count == 0) return true;
	if (output == nullptr || count > std::numeric_limits<size_t>::max() / sizeof(row)) return false;
	if (stats != nullptr) { ++stats->compareCalls; ++stats->compareExtentRows; }
	if (std::memcmp(&row, output, sizeof(row)) != 0) return false;
	for (size_t proven = 1; proven < count;)
	{
		const size_t next = std::min(proven, count - proven);
		if (stats != nullptr) { ++stats->compareCalls; stats->compareExtentRows += next; }
		if (std::memcmp(output, output + proven, next * sizeof(row)) != 0) return false;
		proven += next;
	}
	return true;
}
