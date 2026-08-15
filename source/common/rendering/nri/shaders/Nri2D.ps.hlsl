#include "Nri2DShared.hlsli"

Texture2D InputTexture : register(t0, space1);
SamplerState InputSampler : register(s0, space0);

struct PSInput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR;
};

float4 main(PSInput input) : SV_Target0
{
	float4 texel = float4(1.0, 1.0, 1.0, 1.0);
	if ((gNri2DConstants.Flags & NRI2D_FLAG_TEXTURED) != 0)
	{
		texel = InputTexture.Sample(InputSampler, input.TexCoord);
	}

	if ((gNri2DConstants.Flags & NRI2D_FLAG_ALPHA_FROM_RED) != 0)
	{
		float alpha = dot(texel.rgb, float3(0.3, 0.56, 0.14)) * texel.a;
		texel = float4(1.0, 1.0, 1.0, alpha);
	}

	if ((gNri2DConstants.Flags & NRI2D_FLAG_INVERT) != 0)
	{
		texel.rgb = 1.0 - texel.rgb;
	}

	float4 color = texel * input.Color * gNri2DConstants.VertexColor;
	color.rgb = color.rgb * gNri2DConstants.ObjectColor.rgb + gNri2DConstants.AddColor.rgb;
	color.a *= gNri2DConstants.ObjectColor.a;
	color *= gNri2DConstants.ScreenFade;
	color = saturate(color);

	if ((gNri2DConstants.Flags & NRI2D_FLAG_OUTPUT_HDR_LINEAR) != 0)
	{
		const float hdrScale = max(gNri2DConstants.OutputInfo.x, 0.0);
		const float gamma = max(gNri2DConstants.OutputInfo.y, 1.0);
		color.rgb = pow(color.rgb, gamma) * hdrScale;
	}

	return color;
}
