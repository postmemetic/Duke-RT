#ifndef RAZE_NRI_EMISSIVE_RESPONSE_LOOKUP_HLSLI
#define RAZE_NRI_EMISSIVE_RESPONSE_LOOKUP_HLSLI

#include "EmissiveLightContracts.hlsli"

// Header.flags markers are versioned; unrecognized payloads retain linear
// lookup. Header.dataSource remains the number of following 16-byte rows.
static const uint NRI_EMISSIVE_RESPONSE_HEADER_LINEAR_REUSE = 0x45520101u;
static const uint NRI_EMISSIVE_RESPONSE_HEADER_BINARY_REUSE = 0x45520102u;

uint GetEmissiveResponseLookupMode(uint flags)
{
	return flags == NRI_EMISSIVE_RESPONSE_HEADER_BINARY_REUSE ? 2u :
		(flags == NRI_EMISSIVE_RESPONSE_HEADER_LINEAR_REUSE ? 1u : 0u);
}

struct EmissiveResponseLookupResult
{
	float scale;
	uint found;
	uint examined;
};

EmissiveResponseLookupResult FindEmissiveResponseLinear(
	StructuredBuffer<EmissiveMaterialResponseData> responses,
	uint responseCount, uint dataSource, uint primitiveIndex)
{
	EmissiveResponseLookupResult result = { 1.0, 0u, 0u };
	[loop]
	for (uint i = 1u; i <= responseCount; ++i)
	{
		const EmissiveMaterialResponseData response = responses[i];
		result.examined++;
		if (response.dataSource == dataSource && response.primitiveIndex == primitiveIndex)
		{
			result.scale = max(response.materialScale, 0.0);
			result.found = 1u;
			return result;
		}
	}
	return result;
}

EmissiveResponseLookupResult FindEmissiveResponseBinary(
	StructuredBuffer<EmissiveMaterialResponseData> responses,
	uint responseCount, uint dataSource, uint primitiveIndex)
{
	EmissiveResponseLookupResult result = { 1.0, 0u, 0u };
	uint lower = 1u;
	uint upper = responseCount + 1u;
	[loop]
	while (lower < upper)
	{
		const uint middle = lower + (upper - lower) / 2u;
		const EmissiveMaterialResponseData response = responses[middle];
		result.examined++;
		const bool keyLess = response.dataSource < dataSource ||
			(response.dataSource == dataSource && response.primitiveIndex < primitiveIndex);
		if (keyLess)
		{
			lower = middle + 1u;
		}
		else
		{
			// Continue left after equality: stable sorting plus lower_bound keeps
			// the first-wins result even if a duplicate survives CPU generation.
			upper = middle;
			if (response.dataSource == dataSource && response.primitiveIndex == primitiveIndex)
			{
				result.scale = max(response.materialScale, 0.0);
				result.found = 1u;
			}
		}
	}
	return result;
}

#endif
