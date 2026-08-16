#include "nri_renderer.h"
#include "nri_cvars.h"

#include "../framegen/nri_framegen.h"
#include "../system/nri_renderdevice.h"
#include "nri_renderstate.h"
#include "nri_renderer_settings.h"
#include "nri_sky_environment.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "perf_capture.h"
#include "printf.h"
#include "v_video.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>


namespace
{
	constexpr float NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS = 0.5f;
	constexpr float NRI_FRAME_GENERATION_CADENCE_BREAK_MS = 250.0f;

	float GetTemporalExposureForOutput(const NRIPTOutputPolicy& outputPolicy)
	{
		return std::max(outputPolicy.exposure, 0.125f);
	}

	float GetExposureDeltaStopsForOutput(float previousExposure, float currentExposure)
	{
		const float safePrevious = std::max(previousExposure, 0.125f);
		const float safeCurrent = std::max(currentExposure, 0.125f);
		return std::abs(std::log2(safeCurrent) - std::log2(safePrevious));
	}

	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	class ScopedFrameOutputPerfTimer
	{
	public:
		explicit ScopedFrameOutputPerfTimer(double& targetMs)
			: mTarget((PerfLoopTraceActive() || ShouldEmitRendererTemporalTraceLogs() || PerfCompactCaptureTimingActive()) ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedFrameOutputPerfTimer()
		{
			if (mTarget != nullptr)
			{
				*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};
}

bool NRIRenderer::HasFrameGenerationCadenceBreak() const
{
	return mHasPendingFrameGenerationRealFrameTime &&
		mPendingFrameGenerationRealFrameTimeMs > NRI_FRAME_GENERATION_CADENCE_BREAK_MS;
}

void NRIRenderer::CopyFinalToActiveTarget()
{
	ScopedFrameOutputPerfTimer perfTimer(mLastPerfShellTraceStats.copyFinalMs);
	Clocker clock(NriPTCopyFinal);

	UpdateFrameGenerationFrameDesc();
	NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	CopyTextureToActiveTarget(final);
}

void NRIRenderer::UpdateFrameGenerationHistoryPolicy(int debugMode, const NRIFrameGenerationPolicy& frameGenPolicy, bool preserveHistory)
{
	if (preserveHistory)
	{
		return;
	}

	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	if (!mHasOutputPolicyState)
	{
		mHasOutputPolicyState = true;
		mLastOutputRequestedMode = outputPolicy.requestedMode;
		mLastOutputResolvedMode = outputPolicy.resolvedMode;
	}
	else if (outputPolicy.requestedMode != mLastOutputRequestedMode || outputPolicy.resolvedMode != mLastOutputResolvedMode)
	{
		RequestHistoryReset("output-mode-change");
		mFrameBuffer->PrintPathTracingOutputModeChange(mFrameIndex, mLastOutputRequestedMode, mLastOutputResolvedMode);
		if (ShouldEmitRendererTemporalTraceLogs())
		{
			Printf("NRI PT temporal reset: reason=output-mode-change frame=%u requested_output=%s->%s resolved_output=%s->%s\n",
				mFrameIndex,
				GetNRIPTOutputModeName(mLastOutputRequestedMode),
				GetNRIPTOutputModeName(outputPolicy.requestedMode),
				GetNRIPTOutputModeName(mLastOutputResolvedMode),
				GetNRIPTOutputModeName(outputPolicy.resolvedMode));
		}
		mLastOutputRequestedMode = outputPolicy.requestedMode;
		mLastOutputResolvedMode = outputPolicy.resolvedMode;
	}

	const float temporalExposure = GetTemporalExposureForOutput(outputPolicy);
	if (!mHasTemporalExposureState)
	{
		mHasTemporalExposureState = true;
		mLastTemporalExposure = temporalExposure;
	}
	else
	{
		const float exposureDeltaStops = GetExposureDeltaStopsForOutput(mLastTemporalExposure, temporalExposure);
		if (exposureDeltaStops >= NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS)
		{
			RequestHistoryReset("exposure-change");
			if (ShouldEmitRendererTemporalTraceLogs())
			{
				Printf("NRI PT temporal reset: reason=exposure-change frame=%u exposure=%.3f->%.3f delta_stops=%.3f threshold=%.3f\n",
					mFrameIndex,
					mLastTemporalExposure,
					temporalExposure,
					exposureDeltaStops,
					NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS);
			}
		}

		mLastTemporalExposure = temporalExposure;
	}

	const NRIMainUpscalerKind resolvedMainUpscaler = ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPostSharpen = ResolvePostSharpenKind(false);
	const bool runAppTaa = NRIShouldRunAppTaa(resolvedMainUpscaler);
	const bool denoiseEnabled = !!nri_denoise;
	if (!nri_ptbootstrap &&
		(debugMode != mLastDebugMode ||
		 resolvedMainUpscaler != mLastTemporalHistoryMainUpscaler ||
		 resolvedPostSharpen != mLastTemporalPostSharpen ||
		 runAppTaa != mLastTemporalAppTaaEnabled ||
		 denoiseEnabled != mLastTemporalDenoiseEnabled))
	{
		ArmTemporalTraceBudget("mode-change");
		if (ShouldEmitRendererTemporalTraceLogs())
		{
			Printf("NRI PT temporal reset: reason=mode-change frame=%u debug=%d->%d main=%s->%s post=%s->%s app_taa=%s->%s denoise=%s->%s\n",
				mFrameIndex,
				mLastDebugMode,
				debugMode,
				NRIGetMainUpscalerName(mLastTemporalHistoryMainUpscaler),
				NRIGetMainUpscalerName(resolvedMainUpscaler),
				NRIGetPostSharpenName(mLastTemporalPostSharpen),
				NRIGetPostSharpenName(resolvedPostSharpen),
				mLastTemporalAppTaaEnabled ? "yes" : "no",
				runAppTaa ? "yes" : "no",
				mLastTemporalDenoiseEnabled ? "yes" : "no",
				denoiseEnabled ? "yes" : "no");
		}
		RequestHistoryReset("mode-change");
	}
	mLastDebugMode = debugMode;
	mLastTemporalHistoryMainUpscaler = resolvedMainUpscaler;
	mLastTemporalPostSharpen = resolvedPostSharpen;
	mLastTemporalAppTaaEnabled = runAppTaa;
	mLastTemporalDenoiseEnabled = denoiseEnabled;

	if (!mHasFrameGenerationConfigState)
	{
		mHasFrameGenerationConfigState = true;
		mLastFrameGenerationRequestedEnabled = frameGenPolicy.requestedEnabled;
		mLastFrameGenerationResolvedUiMode = frameGenPolicy.resolvedUiMode;
		return;
	}

	const char* frameGenResetReason = nullptr;
	if (frameGenPolicy.requestedEnabled != mLastFrameGenerationRequestedEnabled)
	{
		frameGenResetReason = "framegen-toggle";
	}
	else if (frameGenPolicy.resolvedUiMode != mLastFrameGenerationResolvedUiMode)
	{
		frameGenResetReason = "framegen-ui-mode-change";
	}

	if (frameGenResetReason != nullptr)
	{
		mFrameBuffer->mFrameGeneration.RequestHistoryReset(frameGenResetReason);
		if (ShouldEmitRendererTemporalTraceLogs())
		{
			Printf("NRI PT frame generation reset: reason=%s frame=%u requested=%s->%s ui=%s->%s\n",
				frameGenResetReason,
				mFrameIndex,
				mLastFrameGenerationRequestedEnabled ? "on" : "off",
				frameGenPolicy.requestedEnabled ? "on" : "off",
				NRIFrameGenerationContext::GetUiModeName(mLastFrameGenerationResolvedUiMode),
				NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.resolvedUiMode));
		}
	}

	mLastFrameGenerationRequestedEnabled = frameGenPolicy.requestedEnabled;
	mLastFrameGenerationResolvedUiMode = frameGenPolicy.resolvedUiMode;
}

void NRIRenderer::NoteSuccessfulRealFrame()
{
	mLastFrameGenerationRealFrameTimeMs = mPendingFrameGenerationRealFrameTimeMs;
	mHasFrameGenerationRealFrameTime = mHasPendingFrameGenerationRealFrameTime;
	mLastFrameGenerationTimestamp = mPendingFrameGenerationTimestamp;
	mHasFrameGenerationTimestamp = true;
	++mFrameGenerationFrameId;
}

void NRIRenderer::UpdateFrameGenerationFrameDesc()
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	NRIFrameGenerationFrameDesc desc = {};
	const bool frameGenerationCadenceBreak = HasFrameGenerationCadenceBreak();
	desc.frameId = mFrameGenerationFrameId + 1u;
	desc.renderWidth = mRenderWidth;
	desc.renderHeight = mRenderHeight;
	desc.outputWidth = mOutputWidth;
	desc.outputHeight = mOutputHeight;
	desc.renderRect = { 0u, 0u, mRenderWidth, mRenderHeight };
	desc.outputRect = { 0u, 0u, mOutputWidth, mOutputHeight };
	desc.hasPreviousCamera = mHasPreviousCameraState;
	desc.resetHistory = mResetHistory || frameGenerationCadenceBreak;
	desc.hasRealFrameTimeMs = mHasPendingFrameGenerationRealFrameTime;
	desc.realFrameTimeMs = mPendingFrameGenerationRealFrameTimeMs;
	const char* resetReason = mResetHistory && !mLastHistoryResetReason.empty() ?
		mLastHistoryResetReason.c_str() :
		(frameGenerationCadenceBreak ? "cadence-break" : "none");
	std::strncpy(desc.resetReason, resetReason, std::size(desc.resetReason) - 1u);
	desc.resetReason[std::size(desc.resetReason) - 1u] = '\0';
	desc.hudlessColorSource = NRIFrameGenerationColorSource::Final;
	desc.hudlessColor = &GetFrameTexture(FrameTextureSlot::Final);
	desc.uiTexture = nullptr;
	desc.motionVectors = &GetFrameTexture(FrameTextureSlot::Motion);
	desc.depth = &GetFrameTexture(FrameTextureSlot::UpscalerDepth);
	std::memcpy(desc.cameraJitter, mCurrentJitter, sizeof(desc.cameraJitter));
	std::memcpy(desc.previousCameraJitter, mPreviousJitter, sizeof(desc.previousCameraJitter));
	desc.motionVectorScale[0] = 1.0f;
	desc.motionVectorScale[1] = 1.0f;
	desc.motionVectorSpace = NRIFrameGenerationMotionVectorSpace::ScreenPixels;
	desc.motionVectorDirection = NRIFrameGenerationMotionVectorDirection::CurrentToPrevious;
	desc.depthType = NRIFrameGenerationDepthType::ClipDepth;
	desc.depthInverted = false;
	desc.depthInfinite = false;
	std::memcpy(desc.currentViewToClip, mCurrentViewToClip, sizeof(desc.currentViewToClip));
	std::memcpy(desc.previousViewToClip, mPreviousViewToClip, sizeof(desc.previousViewToClip));
	std::memcpy(desc.currentWorldToView, mCurrentWorldToView, sizeof(desc.currentWorldToView));
	std::memcpy(desc.previousWorldToView, mPreviousWorldToView, sizeof(desc.previousWorldToView));
	std::memcpy(desc.cameraPosition, mCurrentCameraPos, sizeof(desc.cameraPosition));
	std::memcpy(desc.cameraForward, mCurrentCameraForward, sizeof(desc.cameraForward));
	std::memcpy(desc.cameraRight, mCurrentCameraRight, sizeof(desc.cameraRight));
	std::memcpy(desc.cameraUp, mCurrentCameraUp, sizeof(desc.cameraUp));
	desc.cameraNear = screen->GetZNear();
	desc.cameraFar = screen->GetZFar();
	desc.cameraFovVerticalRadians = 2.0f * atanf(mCurrentTanHalfFovY);
	desc.viewSpaceToMetersFactor = 1.0f;
	mFrameBuffer->mFrameGeneration.SetFrameDesc(*mFrameBuffer, desc);
}

void NRIRenderer::CopyTexture(NRITextureResource& source, NRITextureResource& destination)
{
	mFrameBuffer->TransitionTexture(source, NRICopySourceState());
	mFrameBuffer->TransitionTexture(destination, NRICopyDestinationState());
	mFrameBuffer->mCore.CmdCopyTexture(*mFrameBuffer->mCommandBuffer, *destination.texture, nullptr, *source.texture, nullptr);
}

void NRIRenderer::CopyTextureToActiveTarget(NRITextureResource& source)
{
	mFrameBuffer->TransitionTexture(source, NRICopySourceState());
	mFrameBuffer->TransitionTexture(*mFrameBuffer->mActiveTarget, NRICopyDestinationState());
	mFrameBuffer->mCore.CmdCopyTexture(*mFrameBuffer->mCommandBuffer, *mFrameBuffer->mActiveTarget->texture, nullptr, *source.texture, nullptr);
	mFrameBuffer->mRenderState->NotifyExternalTargetWrite();
}
