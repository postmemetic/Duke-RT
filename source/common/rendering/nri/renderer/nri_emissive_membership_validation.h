#pragma once

#include "nri_emissive_sampling_geometry.h"
#include "engineerrors.h"
#include "printf.h"

// Called only after the diagnostic gate. Compare the actual retained ranges
// with the original complete builder; never repair the candidate before proof.
inline void ValidateNRIStaticEmissiveRanges(const nri_scene::GeometryData* geometry,
	const std::vector<NRIEmissiveMaterialPrimitiveRange>& actual)
{
	std::vector<NRIEmissiveMaterialPrimitiveRange> expected;
	NRIEmissiveGeometryCache::BuildRanges(geometry, expected);
	bool equal = actual.size() == expected.size();
	for (size_t i = 0; equal && i < expected.size(); ++i)
		equal = actual[i].first == expected[i].first && actual[i].count == expected[i].count;
	if (!equal) I_FatalError("NRI PT emissive static ranges reference mismatch");
	static uint64_t checks = 0, rows = 0;
	++checks;
	rows += expected.size();
	if ((checks & (checks - 1)) == 0)
		Printf("PERF pt emissive static ranges reference NRI: checks=%llu hits=%llu rows=%llu mismatches=0\n",
			(unsigned long long)checks, (unsigned long long)checks, (unsigned long long)rows);
}
