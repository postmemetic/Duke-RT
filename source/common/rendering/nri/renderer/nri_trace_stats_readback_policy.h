#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <vector>

static constexpr uint32_t NRI_TRACE_SHADER_READBACK_SLOT_COUNT = 3;
static constexpr uint32_t NRI_TRACE_SHADER_INVALID_READBACK_SLOT = UINT32_MAX;

struct NRITraceShaderReadbackSlotObservation
{
	bool pending = false;
	bool fenceComplete = false;
	bool fenceAbandoned = false;
	uint64_t copySerial = 0;
};

struct NRITraceShaderReadbackDecision
{
	uint32_t newestReadySlot = NRI_TRACE_SHADER_INVALID_READBACK_SLOT;
	uint32_t readyCount = 0;
	uint32_t abandonedCount = 0;
};

inline NRITraceShaderReadbackDecision SelectNRITraceShaderReadback(
	const NRITraceShaderReadbackSlotObservation* slots,
	uint32_t slotCount)
{
	NRITraceShaderReadbackDecision decision = {};
	uint64_t newestSerial = 0;
	for (uint32_t slotIndex = 0; slotIndex < slotCount; ++slotIndex)
	{
		const NRITraceShaderReadbackSlotObservation& slot = slots[slotIndex];
		if (!slot.pending)
		{
			continue;
		}
		if (slot.fenceAbandoned)
		{
			decision.abandonedCount++;
			continue;
		}
		if (!slot.fenceComplete)
		{
			continue;
		}

		decision.readyCount++;
		if (decision.newestReadySlot == NRI_TRACE_SHADER_INVALID_READBACK_SLOT || slot.copySerial > newestSerial)
		{
			decision.newestReadySlot = slotIndex;
			newestSerial = slot.copySerial;
		}
	}
	return decision;
}

struct NRITraceShaderInstanceAttribution
{
	uint32_t primitiveOffset = 0;
	uint32_t primitiveCount = 0;
	uint32_t dataSource = 0;
	uint32_t metadata0 = 0;
	uint32_t metadata1 = 0;
	uint32_t metadata2 = 0;
	uint32_t explicitPrimitiveCount = 0;
};

static_assert(sizeof(NRITraceShaderInstanceAttribution) == 7u * sizeof(uint32_t),
	"Trace shader attribution rows must stay compact.");

inline void ResolveNRITraceShaderAttributionPrimitiveCounts(
	std::vector<NRITraceShaderInstanceAttribution>& rows,
	const std::array<uint32_t, 3>& sourcePrimitiveCounts)
{
	std::vector<uint32_t> order(rows.size());
	std::iota(order.begin(), order.end(), 0u);
	std::sort(
		order.begin(),
		order.end(),
		[&rows](uint32_t a, uint32_t b)
		{
			if (rows[a].dataSource != rows[b].dataSource)
			{
				return rows[a].dataSource < rows[b].dataSource;
			}
			return rows[a].primitiveOffset < rows[b].primitiveOffset;
		});

	for (size_t groupBegin = 0; groupBegin < order.size();)
	{
		const NRITraceShaderInstanceAttribution& first = rows[order[groupBegin]];
		size_t groupEnd = groupBegin + 1;
		while (groupEnd < order.size() &&
			rows[order[groupEnd]].dataSource == first.dataSource &&
			rows[order[groupEnd]].primitiveOffset == first.primitiveOffset)
		{
			groupEnd++;
		}

		const uint32_t total = first.dataSource < sourcePrimitiveCounts.size() ?
			sourcePrimitiveCounts[first.dataSource] : 0u;
		uint32_t endOffset = total;
		if (groupEnd < order.size() && rows[order[groupEnd]].dataSource == first.dataSource)
		{
			endOffset = std::min(total, rows[order[groupEnd]].primitiveOffset);
		}
		const uint32_t primitiveCount = first.primitiveOffset < endOffset && first.primitiveOffset < total ?
			endOffset - first.primitiveOffset : 0u;
		for (size_t index = groupBegin; index < groupEnd; ++index)
		{
			NRITraceShaderInstanceAttribution& row = rows[order[index]];
			row.primitiveCount = row.explicitPrimitiveCount != 0u ?
				row.explicitPrimitiveCount : primitiveCount;
		}
		groupBegin = groupEnd;
	}
}
