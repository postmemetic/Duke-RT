#include "NRI.hlsl"
#include "Include/TemporalConstants.hlsli"

NRI_ROOT_CONSTANTS(NRITemporalConstants, gTemporalConstants, 0, 2);

Texture2D<float4> gGeometryReactiveInput : register(t0, space0);
Texture2D<float4> gVolumeReactiveInput : register(t1, space0);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gCombinedReactiveOutput, u, 0, 1);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTemporalConstants.RenderWidth ||
		dispatchThreadId.y >= gTemporalConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float geometryReactive = saturate(gGeometryReactiveInput.Load(int3(pixelPos, 0)).x);
	const float volumeReactive = saturate(gVolumeReactiveInput.Load(int3(pixelPos, 0)).x);
	gCombinedReactiveOutput[pixelPos] = float4(max(geometryReactive, volumeReactive), 0.0, 0.0, 1.0);
}
