#include "nri_frame_resources.h"
#include "nri_cvars.h"

#include "nri_renderer.h"
#include "../system/nri_renderdevice.h"

#include "nri_upscaler.h"
#include "../framegen/nri_framegen.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <cmath>

namespace
{
	uint32_t BloomPyramidExtent(uint32_t extent, uint32_t level)
	{
		return std::max(1u, extent >> (level + 1u));
	}
}


bool NRIFrameResources::CreateFrameTexture(NRIRenderer& renderer, uint32_t slot, uint32_t width, uint32_t height, nri::Format format)
{
	if (slot >= (uint32_t)NRIRenderer::FrameTextureSlot::Count)
	{
		return false;
	}

	return renderer.mFrameBuffer->CreateOwnedTexture(
		renderer.GetFrameTexture((NRIRenderer::FrameTextureSlot)slot),
		width,
		height,
		format,
		NRIResourceFlags(nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureUsageBits::SHADER_RESOURCE_STORAGE));
}

nri::Format NRIFrameResources::ResolveFinalSceneFormat(const NRIRenderer& renderer)
{
	if (renderer.mFrameBuffer == nullptr)
	{
		return nri::Format::BGRA8_UNORM;
	}

	const NRIFrameGenerationPresentContract& presentContract = renderer.mFrameBuffer->mFrameGeneration.GetPresentContract();
	if (presentContract.resolvedTextureFormat != nri::Format::UNKNOWN)
	{
		return presentContract.resolvedTextureFormat;
	}

	if (renderer.mFrameBuffer->mResolvedSwapChainTextureFormat != nri::Format::UNKNOWN)
	{
		return renderer.mFrameBuffer->mResolvedSwapChainTextureFormat;
	}

	return nri::Format::BGRA8_UNORM;
}

void NRIFrameResources::DestroyFrameTextures(NRIRenderer& renderer)
{
	renderer.DestroyAutoExposureResources();
	for (auto& texture : renderer.mFrameTextures)
	{
		renderer.mFrameBuffer->DestroyTextureResource(texture);
	}
	renderer.mRenderWidth = 0;
	renderer.mRenderHeight = 0;
	renderer.mOutputWidth = 0;
	renderer.mOutputHeight = 0;
	renderer.mTargetWidth = 0;
	renderer.mTargetHeight = 0;
	renderer.mSceneLeft = 0;
	renderer.mSceneTop = 0;
	renderer.mFinalSceneFormat = nri::Format::UNKNOWN;
}

bool NRIFrameResources::EnsureFrameResources(NRIRenderer& renderer, uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight)
{
	Clocker clock(NriPTFrameResources);

	if (outputWidth == 0 || outputHeight == 0 || targetWidth == 0 || targetHeight == 0)
	{
		return false;
	}

	const int32_t sceneLeft = renderer.mFrameBuffer->mSceneViewport.left;
	// Preserve the oversized hardware viewport and crop it during present instead of shrinking it to the visible target.
	const int32_t sceneBottom = renderer.mFrameBuffer->mSceneViewport.top;
	const int32_t sceneTop = (int32_t)targetHeight - sceneBottom - (int32_t)outputHeight;

	const nri::UpscalerMode requestedUpscalerMode = renderer.GetSelectedUpscalerMode();
	const NRIMainUpscalerKind requestedMainUpscalerKind = renderer.GetSelectedMainUpscalerKind();
	bool fsrPreparationFailed = false;
	if (requestedMainUpscalerKind == NRIMainUpscalerKind::FSR)
	{
		const nri::UpscalerMode fsrMode = NRIResolveUpscalerModeForMain(requestedMainUpscalerKind, requestedUpscalerMode);
		fsrPreparationFailed =
			!renderer.IsMainUpscalerSupported(requestedMainUpscalerKind) ||
			!renderer.mUpscaler.EnsureMainUpscaler(
				*renderer.mFrameBuffer,
				requestedMainUpscalerKind,
				fsrMode,
				outputWidth,
				outputHeight,
				false,
				false);
	}
	const NRIMainUpscalerKind mainUpscalerKind = renderer.ResolveMainUpscalerKind(false);
	const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(mainUpscalerKind, requestedUpscalerMode);
	const float requestedRenderScale = std::max(0.33f, std::min((float)nri_renderscale, 1.0f));
	const float renderScale = fsrPreparationFailed
		? 1.0f
		: NRIResolveRenderScaleForMain(mainUpscalerKind, requestedUpscalerMode, requestedRenderScale);
	const NRIFrameGenerationPresentContract& presentContract = renderer.mFrameBuffer->mFrameGeneration.GetPresentContract();

	const uint32_t renderWidth = std::max(1u, (uint32_t)std::lround((double)outputWidth * renderScale));
	const uint32_t renderHeight = std::max(1u, (uint32_t)std::lround((double)outputHeight * renderScale));
	const nri::Format finalFormat = ResolveFinalSceneFormat(renderer);
	const nri::Format activeTargetFormat =
		(renderer.mFrameBuffer->mActiveTarget != nullptr && renderer.mFrameBuffer->mActiveTarget->format != nri::Format::UNKNOWN)
		? renderer.mFrameBuffer->mActiveTarget->format
		: nri::Format::UNKNOWN;

	const bool upToDate =
		renderer.mRenderWidth == renderWidth &&
		renderer.mRenderHeight == renderHeight &&
		renderer.mOutputWidth == outputWidth &&
		renderer.mOutputHeight == outputHeight &&
		renderer.mTargetWidth == targetWidth &&
		renderer.mTargetHeight == targetHeight &&
		renderer.mSceneLeft == sceneLeft &&
		renderer.mSceneTop == sceneTop &&
		renderer.mFinalSceneFormat == finalFormat &&
		(!nri_ptsmoke || (renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::PostVolumeOutput).texture != nullptr &&
			renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPing).texture != nullptr &&
			renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::RrVolumeInput).texture != nullptr)) &&
		renderer.GetFrameTexture(NRIRenderer::FrameTextureSlot::Final).texture != nullptr;

	if (upToDate)
	{
		return true;
	}

	// Frame-resource rebuilds on resize/upscaler mode changes can retire textures that the current
	// command allocator still references. Drain GPU work before destroying frame-sized resources.
	const bool dimensionsChanged =
		renderer.mRenderWidth != renderWidth ||
		renderer.mRenderHeight != renderHeight ||
		renderer.mOutputWidth != outputWidth ||
		renderer.mOutputHeight != outputHeight ||
		renderer.mTargetWidth != targetWidth ||
		renderer.mTargetHeight != targetHeight;
	renderer.WaitForCommandsTracked();
	renderer.mNrd.Shutdown();
	DestroyFrameTextures(renderer);
	renderer.mRenderWidth = renderWidth;
	renderer.mRenderHeight = renderHeight;
	renderer.mOutputWidth = outputWidth;
	renderer.mOutputHeight = outputHeight;
	renderer.mTargetWidth = targetWidth;
	renderer.mTargetHeight = targetHeight;
	renderer.mSceneLeft = sceneLeft;
	renderer.mSceneTop = sceneTop;
	renderer.mFinalSceneFormat = finalFormat;
	renderer.RequestHistoryReset(dimensionsChanged ? "resize" : "frame-resources");
	if (nri_ptscenestats)
	{
		Printf("NRI PT frame resources: main=%s policy=%s requested_mode=%s resolved_mode=%s requested_render_scale=%.3f resolved_render_scale=%.3f render=%ux%u output=%ux%u final=%s contract=%s active=%s jitter=%s phases=%u\n",
			NRIGetMainUpscalerName(mainUpscalerKind),
			NRIGetRenderResolutionPolicyName(mainUpscalerKind),
			NRIGetUpscalerModeName(requestedUpscalerMode),
			NRIGetUpscalerModeName(resolvedUpscalerMode),
			requestedRenderScale,
			renderScale,
			renderWidth,
			renderHeight,
			outputWidth,
			outputHeight,
			NRIFrameGenerationContext::GetNriFormatName(finalFormat),
			NRIFrameGenerationContext::GetNriFormatName(presentContract.resolvedTextureFormat),
			NRIFrameGenerationContext::GetNriFormatName(activeTargetFormat),
			NRIGetTemporalJitterModeName(mainUpscalerKind, renderer.mGuiCaptureActive),
			NRIGetTemporalJitterPhaseCount(mainUpscalerKind, resolvedUpscalerMode, renderer.mGuiCaptureActive));
	}

	const nri::Format colorFormat = nri::Format::RGBA16_SFLOAT;
	const nri::Format normalRoughnessFormat = nri::Format::R10_G10_B10_A2_UNORM;
	const nri::Format upscalerDepthFormat = nri::Format::R32_SFLOAT;
	const nri::Format rrGuideAlbedoFormat = nri::Format::R10_G10_B10_A2_UNORM;
	const nri::Format rrGuideSpecHitDistanceFormat = nri::Format::R16_SFLOAT;
	const nri::Format rrGuideNormalRoughnessFormat = nri::Format::RGBA16_SFLOAT;
	const bool smokeOutputReady = !nri_ptsmoke ||
		(CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::PostVolumeOutput, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::SmokeVolumeCurrent, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::SmokeVolumeCurrentMeta, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPing, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::SmokeVolumeHistoryPong, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPing, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::SmokeVolumeMetaPong, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::RrVolumeInput, renderWidth, renderHeight, colorFormat));

	return
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::ViewZ, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::Motion, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::NormalRoughness, renderWidth, renderHeight, normalRoughnessFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::BaseColorMetalness, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::UnfilteredDiffuse, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::UnfilteredSpecular, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::UnfilteredPenumbra, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::DenoisedDiffuse, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::DenoisedSpecular, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::DenoisedShadow, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::Composed, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::TraceTransparentOutput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::DirectLighting, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::DirectEmission, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::TaaHistoryPing, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::TaaHistoryPong, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::Validation, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::SrInput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::RrInput, renderWidth, renderHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::UpscalerDepth, renderWidth, renderHeight, upscalerDepthFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo, renderWidth, renderHeight, rrGuideAlbedoFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo, renderWidth, renderHeight, rrGuideAlbedoFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance, renderWidth, renderHeight, rrGuideSpecHitDistanceFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::RrGuideNormalRoughness, renderWidth, renderHeight, rrGuideNormalRoughnessFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::VendorOutput, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::PostSharpenOutput, outputWidth, outputHeight, colorFormat) &&
		smokeOutputReady &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::PostBloomOutput, outputWidth, outputHeight, colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::BloomPyramid0, BloomPyramidExtent(outputWidth, 0), BloomPyramidExtent(outputHeight, 0), colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::BloomPyramid1, BloomPyramidExtent(outputWidth, 1), BloomPyramidExtent(outputHeight, 1), colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::BloomPyramid2, BloomPyramidExtent(outputWidth, 2), BloomPyramidExtent(outputHeight, 2), colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::BloomPyramid3, BloomPyramidExtent(outputWidth, 3), BloomPyramidExtent(outputHeight, 3), colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::BloomPyramid4, BloomPyramidExtent(outputWidth, 4), BloomPyramidExtent(outputHeight, 4), colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::BloomPyramid5, BloomPyramidExtent(outputWidth, 5), BloomPyramidExtent(outputHeight, 5), colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::BloomPyramid6, BloomPyramidExtent(outputWidth, 6), BloomPyramidExtent(outputHeight, 6), colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::BloomPyramid7, BloomPyramidExtent(outputWidth, 7), BloomPyramidExtent(outputHeight, 7), colorFormat) &&
		CreateFrameTexture(renderer, (uint32_t)NRIRenderer::FrameTextureSlot::Final, targetWidth, targetHeight, finalFormat);
}
