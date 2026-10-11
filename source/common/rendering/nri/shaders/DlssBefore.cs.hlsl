#include "NRI.hlsl"
#include "NRD.hlsli"
#include "Include/TraceConstants.hlsli"

NRI_ROOT_CONSTANTS(NRITraceSceneConstants, gTraceConstants, 0, 5);

struct ReprojectionData
{
	float4 currentViewToClipMatrix[4];
	float4 previousViewToClipMatrix[4];
	float4 currentWorldToViewMatrix[4];
	float4 previousWorldToViewMatrix[4];
	float2 currentJitter;
	float2 previousJitter;
};

StructuredBuffer<ReprojectionData> gReprojectionDataBuffer : register(t18, space2);

Texture2D<float4> gViewZInput : register(t2, space3);
Texture2D<float4> gNormalRoughnessInput : register(t3, space3);
Texture2D<float4> gBaseColorInput : register(t4, space3);
Texture2D<float4> gSpecularInput : register(t6, space3);

NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gRrGuideNormalRoughnessOutput, u, 5, 4);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gRrGuideDiffuseAlbedoOutput, u, 9, 4);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float4>, gRrGuideSpecularAlbedoOutput, u, 10, 4);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float>, gRrGuideSpecHitDistanceOutput, u, 11, 4);
NRI_FORMAT("unknown") NRI_RESOURCE(RWTexture2D<float>, gUpscalerDepthOutput, u, 12, 4);

static const uint kUpscalerGuideModeSr = 1u;
static const uint kUpscalerGuideModeRr = 2u;
static const float4 kReblurHitDistanceParams = float4(3.0, 0.1, 20.0, -25.0);

bool UseSrGuides()
{
	return gTraceConstants.ReservedTrace0 == kUpscalerGuideModeSr;
}

bool UseRrGuides()
{
	return gTraceConstants.ReservedTrace0 == kUpscalerGuideModeRr;
}

bool UseRelaxDenoiser()
{
	return (gTraceConstants.ReservedTrace1 & 0x3fu) == 1u;
}

float4 MultiplyVsMatrixPoint(float4 v, float4 matrixColumns[4])
{
	return float4(
		dot(v, float4(matrixColumns[0].x, matrixColumns[1].x, matrixColumns[2].x, matrixColumns[3].x)),
		dot(v, float4(matrixColumns[0].y, matrixColumns[1].y, matrixColumns[2].y, matrixColumns[3].y)),
		dot(v, float4(matrixColumns[0].z, matrixColumns[1].z, matrixColumns[2].z, matrixColumns[3].z)),
		dot(v, float4(matrixColumns[0].w, matrixColumns[1].w, matrixColumns[2].w, matrixColumns[3].w)));
}

float3 ReconstructWorldPosition(uint2 pixelPos, float viewZ)
{
	const float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
	float2 uv = ((float2)pixelPos + 0.5) / resolution;
	float2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;

	const float3 relative =
		gTraceConstants.CameraForward * viewZ +
		gTraceConstants.CameraRight * (ndc.x * gTraceConstants.TanHalfFovX * viewZ) +
		gTraceConstants.CameraUp * (ndc.y * gTraceConstants.TanHalfFovY * viewZ);

	return gTraceConstants.CameraPos + relative;
}

float3 ReconstructViewPosition(uint2 pixelPos, float viewZ)
{
	const float2 resolution = float2(gTraceConstants.RenderWidth, gTraceConstants.RenderHeight);
	float2 uv = ((float2)pixelPos + 0.5) / resolution;
	float2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;

	return float3(
		ndc.x * gTraceConstants.TanHalfFovX * viewZ,
		ndc.y * gTraceConstants.TanHalfFovY * viewZ,
		-viewZ);
}

float ConvertViewZToClipDepth(uint2 pixelPos, float viewZ)
{
	if (abs(viewZ) >= NRD_INF * 0.5)
	{
		return 1.0;
	}

	const ReprojectionData reprojection = gReprojectionDataBuffer[0];
	const float3 viewPos = ReconstructViewPosition(pixelPos, viewZ);
	const float4 view = float4(viewPos, 1.0);
	const float4 clip = MultiplyVsMatrixPoint(view, reprojection.currentViewToClipMatrix);
	return clip.w > 1e-5 ? clip.z / clip.w : 1.0;
}

float3 GetDiffuseAlbedo(float3 baseColor, float metalness)
{
	return baseColor * (1.0 - metalness);
}

float3 GetSpecularF0(float3 baseColor, float metalness)
{
	return lerp(float3(0.04, 0.04, 0.04), baseColor, metalness);
}

float3 EvaluateEnvironmentSpecular(float3 specularF0, float roughness, float3 normal, float3 viewDir)
{
	const float noV = abs(dot(normal, viewDir));
	const float m = saturate(roughness * roughness);

	float4 X;
	X.x = 1.0;
	X.y = noV;
	X.z = noV * noV;
	X.w = noV * X.z;

	float4 Y;
	Y.x = 1.0;
	Y.y = m;
	Y.z = m * m;
	Y.w = m * Y.z;

	const float2x2 M1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
	const float3x3 M2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627);
	const float2x2 M3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
	const float3x3 M4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123);

	const float bias = dot(mul(M1, X.xy), Y.xy) / max(dot(mul(M2, X.xyw), Y.xyw), 1e-6);
	const float scale = dot(mul(M3, X.xy), Y.xy) / max(dot(mul(M4, X.xzw), Y.xyw), 1e-6);
	return saturate(specularF0 * scale + bias);
}

float GetSpecularHitDistance(float4 packedSpecular, float viewZ, float roughness)
{
	if (UseRelaxDenoiser())
	{
		return max(packedSpecular.w, 0.0);
	}

	return REBLUR_GetHitDist(saturate(packedSpecular.w), abs(viewZ), kReblurHitDistanceParams, roughness);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= gTraceConstants.RenderWidth || dispatchThreadId.y >= gTraceConstants.RenderHeight)
	{
		return;
	}

	const uint2 pixelPos = dispatchThreadId.xy;
	const float4 baseColor = saturate(gBaseColorInput[pixelPos]);
	const float rawViewZ = gViewZInput[pixelPos].x;
	const float viewZ = abs(rawViewZ);
	const bool isSky = viewZ >= NRD_INF * 0.5;
	const float4 packedSpecular = gSpecularInput[pixelPos];

	float materialID = 0.0;
	const float4 unpackedNormalRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(gNormalRoughnessInput[pixelPos], materialID);
	const float3 normal = unpackedNormalRoughness.xyz;
	const float roughness = saturate(unpackedNormalRoughness.w);
	const float metalness = saturate(baseColor.w);
	const float3 diffuseAlbedo = GetDiffuseAlbedo(baseColor.rgb, metalness);
	const float3 specularF0 = GetSpecularF0(baseColor.rgb, metalness);
	const float3 worldPos = isSky ? gTraceConstants.CameraPos : ReconstructWorldPosition(pixelPos, viewZ);
	const float3 viewDir = isSky ? -gTraceConstants.CameraForward : normalize(gTraceConstants.CameraPos - worldPos);
	const float3 envSpecular = isSky ? 0.0 : EvaluateEnvironmentSpecular(specularF0, roughness, normal, viewDir);
	const float specularHitDistance = isSky ? 0.0 : GetSpecularHitDistance(packedSpecular, viewZ, roughness);

	gRrGuideDiffuseAlbedoOutput[pixelPos] = float4(isSky ? 0.0 : saturate(diffuseAlbedo * (1.0 - envSpecular)), 1.0);
	gRrGuideSpecularAlbedoOutput[pixelPos] = float4(isSky ? 0.0 : saturate(envSpecular), 1.0);
	gRrGuideSpecHitDistanceOutput[pixelPos] = specularHitDistance;
	gUpscalerDepthOutput[pixelPos] = UseSrGuides() ? ConvertViewZToClipDepth(pixelPos, viewZ) : viewZ;
	gRrGuideNormalRoughnessOutput[pixelPos] = isSky ? 0.0 : float4(normal, roughness);
}
