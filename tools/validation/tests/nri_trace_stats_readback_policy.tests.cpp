#include "nri_trace_stats_readback_policy.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			std::fprintf(stderr, "FAILED: %s\n", message);
			std::exit(1);
		}
	}

	const NRITraceShaderInstanceAttribution& FindRow(
		const std::vector<NRITraceShaderInstanceAttribution>& rows,
		uint32_t metadata0)
	{
		for (const NRITraceShaderInstanceAttribution& row : rows)
		{
			if (row.metadata0 == metadata0)
			{
				return row;
			}
		}
		std::fprintf(stderr, "FAILED: attribution row %u was not found\n", metadata0);
		std::exit(1);
	}
}

int main()
{
	Require(NRI_TRACE_SHADER_READBACK_SLOT_COUNT == 3,
		"trace stats must match the triple-buffered frame shell");
	Require(sizeof(NRITraceShaderInstanceAttribution) == 28,
		"frame-correct attribution rows must remain compact");

	std::array<NRITraceShaderReadbackSlotObservation, 3> slots = {};
	slots[0] = { true, true, false, 10 };
	slots[1] = { true, false, false, 11 };
	slots[2] = { true, true, false, 12 };
	NRITraceShaderReadbackDecision decision = SelectNRITraceShaderReadback(slots.data(), (uint32_t)slots.size());
	Require(decision.newestReadySlot == 2 && decision.readyCount == 2 && decision.abandonedCount == 0,
		"the newest completed copy must win without treating in-flight work as ready");

	slots[2].fenceAbandoned = true;
	decision = SelectNRITraceShaderReadback(slots.data(), (uint32_t)slots.size());
	Require(decision.newestReadySlot == 0 && decision.readyCount == 1 && decision.abandonedCount == 1,
		"abandoned copies must be retired instead of mapped or blocking the ring");

	std::vector<NRITraceShaderInstanceAttribution> rows = {
		{ 10, 0, 0, 100, 0 },
		{ 20, 0, 2, 200, 0 },
		{ 5, 0, 1, 300, 0 },
		{ 0, 0, 0, 101, 0 },
		{ 20, 0, 2, 201, 0 },
		{ 20, 0, 2, 203, 0, 0, 4 },
		{ 30, 0, 2, 202, 0 },
		{ 40, 0, 0, 102, 0 },
		{ 1, 0, 9, 900, 0 },
	};
	ResolveNRITraceShaderAttributionPrimitiveCounts(rows, { 30, 9, 40 });
	Require(FindRow(rows, 101).primitiveCount == 10 && FindRow(rows, 100).primitiveCount == 20,
		"same-source ranges must be decoded from the captured primitive bases");
	Require(FindRow(rows, 200).primitiveCount == 10 && FindRow(rows, 201).primitiveCount == 10 &&
		FindRow(rows, 202).primitiveCount == 10,
		"reused voxel geometry bases must retain the same bounded primitive estimate");
	Require(FindRow(rows, 203).primitiveCount == 4,
		"certified proxy rows must preserve their explicit primitive count across a duplicate exact base");
	Require(FindRow(rows, 300).primitiveCount == 4,
		"the final source range must end at its captured primitive total");
	Require(FindRow(rows, 102).primitiveCount == 0 && FindRow(rows, 900).primitiveCount == 0,
		"out-of-range and unknown sources must fail closed to an empty range");

	std::puts("NRI trace-stat nonblocking readback policy tests passed.");
	return 0;
}
