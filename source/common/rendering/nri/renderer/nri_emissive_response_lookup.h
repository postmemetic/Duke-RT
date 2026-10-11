#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// Mirrors EmissiveMaterialResponseData and the header markers in
// shaders/Include/EmissiveResponseLookup.hlsli. The first row remains a header
// with dataSource=count; old shaders can still linearly scan sorted payloads.
struct NRIEmissiveMaterialResponseGpuData
{
	uint32_t dataSource = 0;
	uint32_t primitiveIndex = UINT32_MAX;
	float materialScale = 1.0f;
	uint32_t flags = 0;
};
static_assert(sizeof(NRIEmissiveMaterialResponseGpuData) == 16);

constexpr uint32_t NRI_EMISSIVE_RESPONSE_HEADER_LINEAR_REUSE = 0x45520101u;
constexpr uint32_t NRI_EMISSIVE_RESPONSE_HEADER_BINARY_REUSE = 0x45520102u;

inline uint32_t ResolveNRIEmissiveResponseLookupMode(int requestedMode)
{
	return (uint32_t)std::clamp(requestedMode, 0, 2);
}

inline uint32_t GetNRIEmissiveResponseHeaderMode(uint32_t flags)
{
	return flags == NRI_EMISSIVE_RESPONSE_HEADER_BINARY_REUSE ? 2u :
		(flags == NRI_EMISSIVE_RESPONSE_HEADER_LINEAR_REUSE ? 1u : 0u);
}

inline bool NRIEmissiveResponseKeyLess(
	const NRIEmissiveMaterialResponseGpuData& left,
	const NRIEmissiveMaterialResponseGpuData& right)
{
	return left.dataSource < right.dataSource ||
		(left.dataSource == right.dataSource && left.primitiveIndex < right.primitiveIndex);
}

inline void FinalizeNRIEmissiveMaterialResponses(
	std::vector<NRIEmissiveMaterialResponseGpuData>& responses, int requestedMode)
{
	if (responses.empty())
		responses.emplace_back();
	const uint32_t mode = ResolveNRIEmissiveResponseLookupMode(requestedMode);
	if (mode == 2u)
	{
		// The producer has already performed first-wins deduplication. Stability
		// additionally retains that rule if a future producer leaves equal keys.
		std::stable_sort(responses.begin() + 1, responses.end(), NRIEmissiveResponseKeyLess);
	}
	responses[0].dataSource = (uint32_t)responses.size() - 1u;
	responses[0].flags = mode == 2u ? NRI_EMISSIVE_RESPONSE_HEADER_BINARY_REUSE :
		(mode == 1u ? NRI_EMISSIVE_RESPONSE_HEADER_LINEAR_REUSE : 0u);
}
