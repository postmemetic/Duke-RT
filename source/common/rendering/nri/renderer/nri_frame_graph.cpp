#include "nri_frame_graph.h"

#include "nri_bloom.h"
#include "nri_cvars.h"
#include "nri_pass_dispatch.h"
#include "../system/nri_renderdevice.h"
#include "nri_diagnostic_names.h"
#include "printf.h"

namespace
{
	constexpr uint32_t kSupportedDebugModes[] = {
		0u, 1u, 2u, 3u, 4u, 5u,
		9u, 10u, 11u, 12u,
		16u, 17u, 18u, 19u,
		21u, 22u, 24u, 25u,
		nri_diag::PtDebugAnalyticDirect, nri_diag::PtDebugEmissiveTags, nri_diag::PtDebugEmissiveDirect, nri_diag::PtDebugSectorAmbient,
		nri_diag::PtDebugEmissiveSampleVisibility, nri_diag::PtDebugUpscalerTraceTransparent, nri_diag::PtDebugTaaPreExposedInput,
		nri_diag::PtDebugIndirectLobeSelection
	};

	constexpr NRIPresentRouteInfo kBootstrapRawTraceRoute = {
		NRIPresentRouteKind::BootstrapFinal,
		"bootstrap_raw_trace",
		"Final",
		"bootstrap",
		"TraceOpaque,Final,CopyFinal",
		false,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kBootstrapFallbackRoute = {
		NRIPresentRouteKind::FallbackFinal,
		"bootstrap_fallback",
		"Final",
		"bootstrap",
		"TraceOpaque,Final,CopyFinal",
		false,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kFallbackFinalRoute = {
		NRIPresentRouteKind::FallbackFinal,
		"fallback_final",
		"Final",
		"fallback",
		"TraceOpaque,Final,CopyFinal",
		false,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kResolvedBeautyRoute = {
		NRIPresentRouteKind::ResolvedBeauty,
		"resolved_beauty",
		"FinalPresent",
		"beauty",
		"TraceOpaque,Composition,TraceTransparent,Exposure,UpscaleChain,FinalPresent,CopyFinal",
		false,
		true,
		true,
		true,
		false
	};

	constexpr NRIPresentRouteInfo kComposedDebugRoute = {
		NRIPresentRouteKind::ComposedDebug,
		"taa_pre_exposed_probe",
		"FinalPresent",
		"debug-temporal",
		"TraceOpaque,Composition,TraceTransparent,Exposure,FinalPresent,CopyFinal",
		false,
		false,
		true,
		true,
		false
	};

	constexpr NRIPresentRouteInfo kUpscalerTraceTransparentProbeRoute = {
		NRIPresentRouteKind::UpscalerTraceTransparentProbe,
		"upscaler_trace_transparent",
		"RawPresent",
		"debug-upscaler",
		"TraceOpaque,Composition,TraceTransparent,Exposure,RawPresent,CopyFinal",
		false,
		false,
		true,
		true,
		false
	};

	constexpr NRIPresentRouteInfo kValidationRawRoute = {
		NRIPresentRouteKind::ValidationRaw,
		"validation_raw",
		"RawPresent",
		"debug-nrd",
		"TraceOpaque,Denoiser,RawPresent,CopyFinal",
		true,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kDenoisedRawRoute = {
		NRIPresentRouteKind::DenoisedRaw,
		"denoised_raw",
		"RawPresent",
		"debug-nrd",
		"TraceOpaque,Denoiser,RawPresent,CopyFinal",
		true,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kFinalDebugRoute = {
		NRIPresentRouteKind::FinalDebug,
		"final_debug",
		"Final",
		"debug-final",
		"TraceOpaque,Final,CopyFinal",
		false,
		false,
		false,
		false,
		false
	};

	constexpr NRIPresentRouteInfo kRawTraceDebugRoute = {
		NRIPresentRouteKind::RawTraceDebug,
		"raw_trace_debug",
		"RawPresent",
		"debug-trace",
		"TraceOpaque,RawPresent,CopyFinal",
		false,
		false,
		false,
		false,
		true
	};

	bool IsInRange(uint32_t value, uint32_t minValue, uint32_t maxValue)
	{
		return value >= minValue && value <= maxValue;
	}

	struct FrameGraphLogState
	{
		bool phaseBCompositionPath = false;
		bool phaseGResolvedPresentPath = false;
		bool phaseFDenoiserPath = false;
		bool phaseFDenoiserFallback = false;
		bool phaseFTraceTransparentPath = false;
		bool traceTransparentProbePath = false;
		bool rawTraceBypass = false;
		bool phaseHRrInputPath = false;
	};

	FrameGraphLogState& GetFrameGraphLogState()
	{
		static FrameGraphLogState state = {};
		return state;
	}
}

const char* GetNRIFramePassName(NRIFramePass pass)
{
	switch (pass)
	{
	case NRIFramePass::TraceOpaque: return "TraceOpaque";
	case NRIFramePass::Denoiser: return "Denoiser";
	case NRIFramePass::Composition: return "Composition";
	case NRIFramePass::TraceTransparent: return "TraceTransparent";
	case NRIFramePass::Exposure: return "Exposure";
	case NRIFramePass::UpscaleChain: return "UpscaleChain";
	case NRIFramePass::RawPresent: return "RawPresent";
	case NRIFramePass::FinalPresent: return "FinalPresent";
	case NRIFramePass::Final: return "Final";
	case NRIFramePass::CopyFinalToTarget: return "CopyFinal";
	default: return "Unknown";
	}
}

bool IsNRIFrameGraphSupportedDebugMode(uint32_t debugMode)
{
	for (uint32_t supportedMode : kSupportedDebugModes)
	{
		if (supportedMode == debugMode)
		{
			return true;
		}
	}
	return false;
}

bool IsNRIFrameGraphRawTraceDebugMode(uint32_t debugMode)
{
	return
		IsInRange(debugMode, 1u, 5u) ||
		IsInRange(debugMode, 10u, 12u) ||
		debugMode == 18u ||
		debugMode == 19u ||
		IsInRange(debugMode, 21u, 22u) ||
		IsInRange(debugMode, 24u, 25u) ||
		IsInRange(debugMode, nri_diag::PtDebugAnalyticDirect, nri_diag::PtDebugSectorAmbient) ||
		debugMode == nri_diag::PtDebugEmissiveSampleVisibility ||
		debugMode == nri_diag::PtDebugIndirectLobeSelection;
}

bool IsNRIFrameGraphFinalShaderDebugMode(uint32_t)
{
	return false;
}

NRIPresentRouteInfo ResolveNRIFrameRoute(const NRIFrameRouteRequest& request)
{
	if (request.bootstrap)
	{
		if (request.bootstrapMode == 11u || request.bootstrapMode == 12u)
		{
			return kBootstrapRawTraceRoute;
		}
		return kBootstrapFallbackRoute;
	}

	if (request.debugMode == 0u)
	{
		return kResolvedBeautyRoute;
	}
	if (request.debugMode == nri_diag::PtDebugTaaPreExposedInput)
	{
		return kComposedDebugRoute;
	}
	if (request.debugMode == nri_diag::PtDebugUpscalerTraceTransparent)
	{
		return kUpscalerTraceTransparentProbeRoute;
	}
	if (request.debugMode == 9u)
	{
		return kValidationRawRoute;
	}
	if (request.debugMode == 16u || request.debugMode == 17u)
	{
		return kDenoisedRawRoute;
	}
	if (IsNRIFrameGraphFinalShaderDebugMode(request.debugMode))
	{
		return kFinalDebugRoute;
	}
	if (IsNRIFrameGraphRawTraceDebugMode(request.debugMode))
	{
		return kRawTraceDebugRoute;
	}

	return kFallbackFinalRoute;
}

bool ExecuteNRIFrameGraph(
	NRIPassDispatchContext& context,
	HWDrawInfo& di,
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	const NRIFrameGraphExecutionRequest& request)
{
	using FrameTextureSlot = NRIRenderer::FrameTextureSlot;

	FrameGraphLogState& logState = GetFrameGraphLogState();
	const int ptDebugMode = request.ptDebugMode;
	const bool denoise = request.denoise;
	const NRIPresentRouteInfo& presentRoute = request.presentRoute;
	context.mSelfTest.ResetSelfTestRouteSnapshot();
	const bool bootstrapRawTracePresent = presentRoute.kind == NRIPresentRouteKind::BootstrapFinal;
	const bool useResolvedPresent = presentRoute.kind == NRIPresentRouteKind::ResolvedBeauty;
	const bool useComposedDebugPresent = presentRoute.kind == NRIPresentRouteKind::ComposedDebug;
	const bool useUpscalerTraceTransparentProbe = presentRoute.kind == NRIPresentRouteKind::UpscalerTraceTransparentProbe;
	const bool useCompositionPath = useResolvedPresent || useComposedDebugPresent || useUpscalerTraceTransparentProbe;
	const bool useValidationPresent = presentRoute.kind == NRIPresentRouteKind::ValidationRaw;
	const bool useDenoisedDebugPresent = presentRoute.kind == NRIPresentRouteKind::DenoisedRaw;
	const bool useShadowDebugPresent = presentRoute.kind == NRIPresentRouteKind::ShadowFinal;
	const bool useFinalDebugPresent = presentRoute.kind == NRIPresentRouteKind::FinalDebug || useShadowDebugPresent;
	const bool rawTraceDirectPresent = presentRoute.kind == NRIPresentRouteKind::RawTraceDebug;
	const bool useSplitShadowDebugProbe = rawTraceDirectPresent && ptDebugMode >= 21 && ptDebugMode <= 22;
	const NRIMainUpscalerKind resolvedMainKind = context.mUpscalerService.ResolveMainUpscalerKind(false);
	const bool compositionConsumesNrd = useCompositionPath && denoise && !NRIIsRayReconstructionMain(resolvedMainKind);
	context.mTraceIndirectDenoiserAvailable =
		useValidationPresent ||
		useDenoisedDebugPresent ||
		(useShadowDebugPresent && denoise) ||
		compositionConsumesNrd ||
		ptDebugMode == (int)nri_diag::PtDebugIndirectLobeSelection;
	context.mHistoryInputSlot = (context.mFrame.frameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
	context.mHistoryOutputSlot = (context.mFrame.frameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
	context.mUpscaledInputSlot = FrameTextureSlot::PostSharpenOutput;
	context.mUseUpscaledInFinal = false;
	context.mUseDenoisedCompositionInputs = false;
	const bool directionalLightShadowEnabled = context.mDirectionalLightState.enabled && context.mDirectionalLightState.shadow;
	context.mUseSplitShadowDenoiser = directionalLightShadowEnabled && (useShadowDebugPresent || useSplitShadowDebugProbe || compositionConsumesNrd);
	const NRIPTOutputPolicy outputPolicy = context.mResources.GetOutputPolicy();
	const NRIAutoExposureSettings autoExposureSettings = GetNRIAutoExposureSettings(
		outputPolicy.exposure,
		IsNRIPTHdrOutputActive(outputPolicy));
	const char* autoExposureSettingsResetReason = nullptr;
	if (!context.mHasAutoExposureSettingsState)
	{
		context.mHasAutoExposureSettingsState = true;
	}
	else
	{
		autoExposureSettingsResetReason = GetNRIAutoExposureResetReasonForSettingsChange(context.mLastAutoExposureSettings, autoExposureSettings);
	}
	context.mLastAutoExposureSettings = autoExposureSettings;
	if (!context.mExposureService.EnsureAutoExposureResources(autoExposureSettings))
	{
		return false;
	}
	if (autoExposureSettingsResetReason != nullptr)
	{
		context.mExposureService.RequestAutoExposureReset(autoExposureSettingsResetReason);
	}

	if (!NRIPassDispatcher::DispatchTraceOpaque(context, di, geometry, materials))
	{
		return false;
	}
	if (!context.mSmokeService.PrepareFrame(context.mFrame.mainViewEligible))
	{
		return false;
	}

	if (bootstrapRawTracePresent)
	{
		context.mSelfTest.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, false, false, false);
		if (!NRIPassDispatcher::DispatchFinal(context))
		{
			return false;
		}

		context.mResources.CopyFinalToActiveTarget();
		return true;
	}

	if (useValidationPresent)
	{
		context.mSelfTest.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, true, false, false);
		if (!NRIPassDispatcher::DispatchDenoiser(context))
		{
			return false;
		}

		if (!NRIPassDispatcher::DispatchRawPresent(context, FrameTextureSlot::Validation))
		{
			return false;
		}

		context.mResources.CopyFinalToActiveTarget();
		return true;
	}

	if (useDenoisedDebugPresent)
	{
		context.mSelfTest.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, true, false, false);
		if (!NRIPassDispatcher::DispatchDenoiser(context))
		{
			return false;
		}

		const FrameTextureSlot denoisedSlot = ptDebugMode == 16 ? FrameTextureSlot::DenoisedDiffuse : FrameTextureSlot::DenoisedSpecular;
		if (!NRIPassDispatcher::DispatchRawPresent(context, denoisedSlot))
		{
			return false;
		}

		context.mResources.CopyFinalToActiveTarget();
		return true;
	}

	if (useShadowDebugPresent)
	{
		context.mSelfTest.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, denoise ? "TraceOpaque,Denoiser,Final,CopyFinal" : "TraceOpaque,Final,CopyFinal", denoise, false, false);
		if (denoise && !NRIPassDispatcher::DispatchDenoiser(context))
		{
			return false;
		}

		context.mUseUpscaledInFinal = false;
		if (!NRIPassDispatcher::DispatchFinal(context))
		{
			return false;
		}

		context.mResources.CopyFinalToActiveTarget();
		return true;
	}

	auto dispatchCompositionPath = [&]() -> bool
	{
		const bool buildRrInput = NRIIsRayReconstructionMain(resolvedMainKind);
		const bool needStandardComposition =
			!buildRrInput || useComposedDebugPresent || useUpscalerTraceTransparentProbe;

		context.mUseDenoisedCompositionInputs = false;

		if (buildRrInput)
		{
			if (!logState.phaseHRrInputPath)
			{
				Printf("DLRR now builds a separate noisy RrInput before NRD and bypasses opaque denoising for the vendor RR branch.\n");
				logState.phaseHRrInputPath = true;
			}

			context.mUseSplitShadowDenoiser = false;
			if (!NRIPassDispatcher::DispatchComposition(context, FrameTextureSlot::RrInput))
			{
				return false;
			}
			if (nri_ptsmoke && (int)nri_ptsmokedlrrmode == 1)
			{
				NRISmokeRouteDesc smokeRoute = {};
				smokeRoute.inputSlot = FrameTextureSlot::RrInput;
				smokeRoute.outputSlot = FrameTextureSlot::RrVolumeInput;
				smokeRoute.depthSlot = FrameTextureSlot::ViewZ;
				smokeRoute.exposureDomain = NRIRenderer::ExposureDomain::SceneHDR;
				smokeRoute.placement = NRISmokeRoutePlacement::DlrrPreUpscaleMainInput;
				smokeRoute.width = context.mFrame.renderWidth;
				smokeRoute.height = context.mFrame.renderHeight;
				smokeRoute.supported = true;
				if (!context.mSmokeService.DispatchRoute(smokeRoute))
					return false;
			}
		}

		if (!needStandardComposition)
		{
			const FrameTextureSlot rrExposureInput = nri_ptsmoke && (int)nri_ptsmokedlrrmode == 1 ?
				FrameTextureSlot::RrVolumeInput : FrameTextureSlot::RrInput;
			if (!context.mExposureService.DispatchAutoExposure(rrExposureInput))
			{
				return false;
			}
			return true;
		}

		if (!buildRrInput && denoise)
		{
			if (!logState.phaseFDenoiserPath)
			{
				Printf("The Composition-backed PT paths now route through NRD before Composition when nri_denoise is enabled.\n");
				logState.phaseFDenoiserPath = true;
			}

			if (!NRIPassDispatcher::DispatchDenoiser(context))
			{
				if (context.mActiveIndirectSamplingMode != 0u)
				{
					return false;
				}
				context.mUseSplitShadowDenoiser = false;
				if (!logState.phaseFDenoiserFallback)
				{
					Printf(TEXTCOLOR_ORANGE "NRD dispatch failed in the composition path; falling back to raw trace inputs for this frame.\n");
					logState.phaseFDenoiserFallback = true;
				}
			}
			else
			{
				context.mUseDenoisedCompositionInputs = true;
				context.mUseSplitShadowDenoiser = directionalLightShadowEnabled;
			}
		}

		if (!NRIPassDispatcher::DispatchComposition(context, FrameTextureSlot::Composed))
		{
			return false;
		}

		if (!logState.phaseFTraceTransparentPath)
		{
			Printf("Composition-backed PT paths now pass through placeholder TraceTransparent before output-resolution dispatch.\n");
			logState.phaseFTraceTransparentPath = true;
		}

		if (!NRIPassDispatcher::DispatchTraceTransparent(context))
		{
			return false;
		}

		if (!context.mExposureService.DispatchAutoExposure(FrameTextureSlot::TraceTransparentOutput))
		{
			return false;
		}

		return true;
	};

	if (useResolvedPresent)
	{
		const bool bloomEnabled = !!nri_ptbloom;
		const char* resolvedPassListName = bloomEnabled
			? "TraceOpaque,Composition,TraceTransparent,Exposure,UpscaleChain,Bloom,FinalPresent,CopyFinal"
			: presentRoute.passListName;
		context.mSelfTest.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, resolvedPassListName, denoise, true, true);
		if (!logState.phaseGResolvedPresentPath)
		{
			Printf("ptdebug 0 now routes through Composition, placeholder TraceTransparent, DispatchUpscaleChain, and the minimal FinalPresent presenter.\n");
			logState.phaseGResolvedPresentPath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!NRIPassDispatcher::DispatchUpscaleChain(context))
		{
			return false;
		}

		const FrameTextureSlot resolvedPresentSlot = context.mUseUpscaledInFinal ? context.mUpscaledInputSlot : context.mHistoryOutputSlot;
		FrameTextureSlot finalPresentSlot = resolvedPresentSlot;
		if (NRIIsRayReconstructionMain(context.mUpscalerService.ResolveMainUpscalerKind(false)) && (int)nri_ptsmokedlrrmode == 2)
		{
			NRISmokeRouteDesc smokeRoute = {};
			smokeRoute.inputSlot = resolvedPresentSlot;
			smokeRoute.outputSlot = FrameTextureSlot::PostVolumeOutput;
			smokeRoute.depthSlot = FrameTextureSlot::UpscalerDepth;
			smokeRoute.exposureDomain = NRIRenderer::ExposureDomain::SceneHDR;
			smokeRoute.placement = NRISmokeRoutePlacement::DlrrPostUpscale;
			smokeRoute.width = context.mFrame.outputWidth;
			smokeRoute.height = context.mFrame.outputHeight;
			smokeRoute.supported = false;
			if (!context.mSmokeService.DispatchRoute(smokeRoute))
			{
				return false;
			}
		}
		if (bloomEnabled)
		{
			NRIBloomDispatchDesc bloomDesc = {};
			bloomDesc.mode = NRIBloomDispatchDesc::Mode::Filter;
			bloomDesc.inputSlot = resolvedPresentSlot;
			bloomDesc.outputSlot = FrameTextureSlot::PostBloomOutput;
			if (!DispatchBloom(context, bloomDesc))
			{
				return false;
			}
			finalPresentSlot = FrameTextureSlot::PostBloomOutput;
			if ((int)nri_ptbloomdebug > 0)
			{
				if (!NRIPassDispatcher::DispatchRawPresent(context, finalPresentSlot))
				{
					return false;
				}
				context.mResources.CopyFinalToActiveTarget();
				return true;
			}
		}

		const NRIMainUpscalerKind resolvedMain = context.mUpscalerService.ResolveMainUpscalerKind(false);
		const NRIPostSharpenKind resolvedPost = context.mUpscalerService.ResolvePostSharpenKind(false);
		context.mUpscalerService.TraceTemporalState("resolved-present", resolvedMain, resolvedPost, context.mUpscalerService.ShouldRunAppTaaForFrameGraph(resolvedMain), finalPresentSlot, context.mHistoryOutputSlot);
		if (!NRIPassDispatcher::DispatchFinalPresent(context, finalPresentSlot))
		{
			return false;
		}

		context.mResources.CopyFinalToActiveTarget();
		return true;
	}

	if (useComposedDebugPresent)
	{
		context.mSelfTest.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, denoise, false, true);
		if (!logState.phaseBCompositionPath)
		{
			Printf("NRI Phase B: ptdebug 45 now routes through Composition, placeholder TraceTransparent, and the minimal FinalPresent presenter.\n");
			logState.phaseBCompositionPath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!NRIPassDispatcher::DispatchFinalPresent(context, FrameTextureSlot::TraceTransparentOutput))
		{
			return false;
		}

		context.mResources.CopyFinalToActiveTarget();
		return true;
	}

	if (useUpscalerTraceTransparentProbe)
	{
		context.mSelfTest.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, denoise, false, true);
		if (!logState.traceTransparentProbePath)
		{
			Printf("NRI Phase I instrumentation: ptdebug 34 now exposes TraceTransparentOutput before the upscaler chain.\n");
			logState.traceTransparentProbePath = true;
		}

		if (!dispatchCompositionPath())
		{
			return false;
		}

		if (!NRIPassDispatcher::DispatchRawPresent(context, FrameTextureSlot::TraceTransparentOutput))
		{
			return false;
		}

		context.mResources.CopyFinalToActiveTarget();
		return true;
	}

	if (useFinalDebugPresent)
	{
		context.mSelfTest.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, false, false, false);
		context.mUseUpscaledInFinal = false;
		if (!NRIPassDispatcher::DispatchFinal(context))
		{
			return false;
		}

		context.mResources.CopyFinalToActiveTarget();
		return true;
	}

	if (rawTraceDirectPresent)
	{
		context.mSelfTest.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, false, false, false);
		if (!logState.rawTraceBypass)
		{
			Printf("NRI frame-graph bypass: presenting raw TraceOpaque output through the direct present path for non-composition debug views.\n");
			logState.rawTraceBypass = true;
		}

		FrameTextureSlot rawPresentSlot = FrameTextureSlot::UnfilteredDiffuse;
		FrameTextureSlot rawPresentSecondarySlot = FrameTextureSlot::Count;
		FrameTextureSlot rawPresentTertiarySlot = FrameTextureSlot::Count;
		if (ptDebugMode == 11 || ptDebugMode == 12)
		{
			rawPresentSlot = FrameTextureSlot::UnfilteredSpecular;
		}
		else if (ptDebugMode == 18)
		{
			rawPresentSlot = FrameTextureSlot::BaseColorMetalness;
		}
		else if (ptDebugMode == 19)
		{
			rawPresentSlot = FrameTextureSlot::NormalRoughness;
		}
		else if (ptDebugMode == 21 || ptDebugMode == 22)
		{
			rawPresentSlot = FrameTextureSlot::UnfilteredPenumbra;
		}
		else if (ptDebugMode == 24)
		{
			rawPresentSlot = FrameTextureSlot::DirectLighting;
		}
		else if (ptDebugMode == 25)
		{
			rawPresentSlot = FrameTextureSlot::DirectEmission;
		}

		if (ptDebugMode == 12)
		{
			rawPresentSecondarySlot = FrameTextureSlot::ViewZ;
			rawPresentTertiarySlot = FrameTextureSlot::NormalRoughness;
		}

		if (!NRIPassDispatcher::DispatchRawPresent(context, rawPresentSlot, rawPresentSecondarySlot, rawPresentTertiarySlot))
		{
			return false;
		}

		context.mResources.CopyFinalToActiveTarget();
		return true;
	}

	if (!logState.rawTraceBypass)
	{
		Printf("NRI frame-graph bypass: presenting raw TraceOpaque output until composition integration is stabilized.\n");
		logState.rawTraceBypass = true;
	}

	context.mUseUpscaledInFinal = false;
	context.mSelfTest.SetSelfTestRouteSnapshot(presentRoute.routeName, presentRoute.presenterName, presentRoute.ownerName, presentRoute.passListName, false, false, false);
	if (!NRIPassDispatcher::DispatchFinal(context))
	{
		return false;
	}

	context.mResources.CopyFinalToActiveTarget();
	return true;
}
