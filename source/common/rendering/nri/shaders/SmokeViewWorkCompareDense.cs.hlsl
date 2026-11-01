#include "Include/SmokeViewWorkResources.hlsli"
#include "Include/SmokeOpticalContent.hlsli"

uint SmokeViewOutputHash(uint value)
{
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	return value ^ (value >> 16u);
}

float SmokeViewCompareSliceDepth(uint boundary)
{
	const float normalized = (float)boundary / max((float)gViewConstants.FroxelDepth, 1.0);
	return gViewConstants.FroxelMaxDistance * pow(normalized, max(gViewConstants.DepthExponent, 0.001));
}

[numthreads(64, 1, 1)]
void main(uint dispatchThreadId : SV_DispatchThreadID)
{
	const uint froxelCount = SmokeViewColumnCount() * gViewConstants.FroxelDepth;
	if (dispatchThreadId >= froxelCount)
		return;
	const uint columnCount = SmokeViewColumnCount();
	const uint z = dispatchThreadId / columnCount;
	const uint columnIndex = dispatchThreadId - z * columnCount;
	const uint2 mask = gViewColumnMasks[columnIndex].Words;
	const bool selected = (mask[z >> 5u] & (1u << (z & 31u))) != 0u;
	const float4 medium = gViewDenseMedium[dispatchThreadId];
	const float3 source = gViewDenseSource[dispatchThreadId].rgb;
	const bool dense = SmokeHasOpticalContent(medium.w, any(medium.rgb > 0.0));
	if (dense)
	{
		InterlockedAdd(gViewWorkControl[0].DenseContributing, 1u);
		const uint hashLo = SmokeViewOutputHash(dispatchThreadId ^ asuint(medium.x) ^
			SmokeViewOutputHash(asuint(medium.y)) ^ SmokeViewOutputHash(asuint(medium.z)) ^ SmokeViewOutputHash(asuint(medium.w)));
		const uint hashHi = SmokeViewOutputHash((dispatchThreadId * 0x9e3779b9u) ^ asuint(source.x) ^
			SmokeViewOutputHash(asuint(source.y)) ^ SmokeViewOutputHash(asuint(source.z)));
		InterlockedXor(gViewWorkControl[0].OutputHashLo, hashLo);
		InterlockedXor(gViewWorkControl[0].OutputHashHi, hashHi);
	}
	if (dense == selected)
		return;
	if (!dense)
	{
		InterlockedAdd(gViewWorkControl[0].FalsePositives, 1u);
		return;
	}

	InterlockedAdd(gViewWorkControl[0].FalseNegatives, 1u);
	const uint x = columnIndex % gViewConstants.FroxelWidth;
	const uint y = columnIndex / gViewConstants.FroxelWidth;
	if (x == 0u || y == 0u || x + 1u == gViewConstants.FroxelWidth ||
		y + 1u == gViewConstants.FroxelHeight || z == 0u || z + 1u == gViewConstants.FroxelDepth)
		InterlockedAdd(gViewWorkControl[0].BoundaryFalseNegatives, 1u);

	const float2 uv = (float2(x, y) + 0.5) /
		float2(gViewConstants.FroxelWidth, gViewConstants.FroxelHeight);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	const float3 ray = normalize(gViewConstants.CameraForward +
		ndc.x * gViewConstants.TanHalfFovX * gViewConstants.CameraRight +
		ndc.y * gViewConstants.TanHalfFovY * gViewConstants.CameraUp);
	const float viewLength = SmokeViewCompareSliceDepth(z + 1u) - SmokeViewCompareSliceDepth(z);
	const float worldLength = viewLength / max(dot(ray, gViewConstants.CameraForward), 1e-4);
	const float tauError = max(medium.w * worldLength, 0.0);
	const float opacityError = 1.0 - exp(-tauError);
	const float radianceError = length(max(source, 0.0));
	if (!isfinite(tauError) || !isfinite(opacityError) || !isfinite(radianceError))
	{
		InterlockedAdd(gViewWorkControl[0].Overflow, 1u);
		return;
	}
	InterlockedMax(gViewWorkControl[0].TauErrorBits, asuint(tauError));
	InterlockedMax(gViewWorkControl[0].OpacityErrorBits, asuint(opacityError));
	InterlockedMax(gViewWorkControl[0].RadianceErrorBits, asuint(radianceError));
}
