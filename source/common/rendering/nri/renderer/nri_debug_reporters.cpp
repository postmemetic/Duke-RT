#include "nri_debug_reporters.h"
#include "nri_cvars.h"

#include "nri_renderer.h"
#include "../framegen/nri_framegen.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_scene_stats.h"
#include "../system/nri_renderdevice.h"
#include "nri_actor_sprite_diagnostics.h"
#include "nri_diagnostic_names.h"
#include "nri_frame_graph.h"
#include "nri_renderer_settings.h"
#include "nri_shader_contracts.h"
#include "nri_surface_light_overlay.h"
#include "c_cvars.h"
#include "mapinfo.h"
#include "printf.h"

#include <unordered_map>
#include <unordered_set>


namespace
{
	static const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}

	static const char* GetNightVisionModeName(NRIPTNightVisionMode mode)
	{
		switch (mode)
		{
		case NRIPTNightVisionMode::Duke: return "duke";
		default: return "none";
		}
	}

	static const char* GetUpscalerFamilyName(NRIMainUpscalerKind kind, bool runAppTaa)
	{
		if (NRIIsStandardSuperResolutionMain(kind)) return "vendor-sr";
		if (NRIIsRayReconstructionMain(kind)) return "vendor-rr";
		return runAppTaa ? "native-taa" : "native";
	}

	static const char* GetDirectionalLightSourceName(const NRIDirectionalLightState& state)
	{
		if (!state.enabled)
		{
			return "off";
		}

		return state.fromOverlay ? "overlay" : "default";
	}

	static const char* GetNrdHitDistanceReconstructionModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "area_3x3";
		case 2: return "area_5x5";
		default: return "off";
		}
	}

	static const char* GetNrdDenoiserModeName(NRINrdDenoiserMode mode)
	{
		switch (mode)
		{
		case NRINrdDenoiserMode::Relax: return "RELAX_DIFFUSE_SPECULAR";
		default: return "REBLUR_DIFFUSE_SPECULAR";
		}
	}

	static const char* GetNrdInputSplitModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "raw_left_denoised_right";
		case 2: return "denoised_left_raw_right";
		default: return "off";
		}
	}

	static uint32_t GetEffectivePtDebugMode()
	{
		if (nri_ptdebug < 0 || nri_ptdebug > (int)nri_diag::PtDebugMotionValidity)
		{
			return 0u;
		}

		const uint32_t debugMode = (uint32_t)nri_ptdebug;
		return IsNRIFrameGraphSupportedDebugMode(debugMode) ? debugMode : 0u;
	}

	static uint32_t GetBootstrapMode()
	{
		return std::clamp((uint32_t)std::max(0, (int)nri_ptbootstrapmode), 0u, 32u);
	}

	static NRIPresentRouteInfo ResolvePresentRouteInfo(uint32_t debugMode, bool bootstrap)
	{
		NRIFrameRouteRequest request = {};
		request.debugMode = debugMode;
		request.bootstrap = bootstrap;
		request.bootstrapMode = bootstrap ? GetBootstrapMode() : 0u;
		return ResolveNRIFrameRoute(request);
	}

	static uint32_t CoherencyFloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static uint64_t HashMaterialBridgeSummary(const nri_scene::MaterialBridgeData& materials)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)materials.materials.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)materials.lightMetadata.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)materials.textures.size());
		for (size_t i = 0; i < materials.materials.size(); ++i)
		{
			const auto& material = materials.materials[i];
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.textureIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.paletteIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.flags);
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.lightingFlags);
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.emissiveMode);
			hash = nri_scene::HashCombine64(hash, (uint64_t)material.emissiveTextureIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)CoherencyFloatBits(material.alpha));
		}

		for (const auto& metadata : materials.lightMetadata)
		{
			hash = nri_scene::HashCombine64(hash, metadata.materialKey);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.textureId);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.actorIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.textureIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.paletteIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.emissiveMode);
			hash = nri_scene::HashCombine64(hash, (uint64_t)metadata.emissiveTextureIndex);
		}

		for (const auto& texture : materials.textures)
		{
			hash = nri_scene::HashCombine64(hash, texture.key);
			hash = nri_scene::HashCombine64(hash, (uint64_t)texture.width);
			hash = nri_scene::HashCombine64(hash, (uint64_t)texture.height);
			hash = nri_scene::HashCombine64(hash, texture.indexed ? 1ull : 0ull);
		}

		return hash;
	}

	static bool IsChunkMarkedVisible(const std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const size_t wordIndex = (size_t)(chunkIndex >> 5u);
		if (wordIndex >= visibleChunkWords.size())
		{
			return false;
		}

		return (visibleChunkWords[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
	}

	static uint32_t GetFlatPlaneVisibilityIndex(int32_t sectorIndex, bool ceiling)
	{
		return (uint32_t)sectorIndex * 2u + (ceiling ? 1u : 0u);
	}

	static bool IsFlatPlaneMarkedVisible(const std::vector<uint32_t>& visibleFlatPlaneWords, int32_t sectorIndex, bool ceiling)
	{
		if (sectorIndex < 0)
		{
			return false;
		}

		const uint32_t flatPlaneIndex = GetFlatPlaneVisibilityIndex(sectorIndex, ceiling);
		const size_t wordIndex = (size_t)(flatPlaneIndex >> 5u);
		if (wordIndex >= visibleFlatPlaneWords.size())
		{
			return false;
		}

		return (visibleFlatPlaneWords[wordIndex] & (1u << (flatPlaneIndex & 31u))) != 0u;
	}

}

void NRIRendererDiagnostics::ResetSelfTestRouteSnapshot()
{
	mSelfTestRoute = {};
}

void NRIRendererDiagnostics::SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passes, bool denoiserRun, bool upscalerRun, bool exposureRun)
{
	if (!nri_ptselftest)
	{
		return;
	}

	mSelfTestRoute.routeName = routeName != nullptr ? routeName : "unknown";
	mSelfTestRoute.presenterName = presenterName != nullptr ? presenterName : "unknown";
	mSelfTestRoute.ownerName = ownerName != nullptr ? ownerName : "unknown";
	mSelfTestRoute.passes = passes != nullptr ? passes : "unknown";
	mSelfTestRoute.denoiserRun = denoiserRun;
	mSelfTestRoute.upscalerRun = upscalerRun;
	mSelfTestRoute.exposureRun = exposureRun;
}

void NRIRendererDiagnostics::EmitSelfTestSummary(const NRISelfTestSummarySnapshot& snapshot) const
{
	if (!nri_ptselftest)
	{
		return;
	}

	Printf("NRI PT selftest: frame=%u engine_frame=%u map=%s level=%s backend=nri api=%s world_active=%u menu_active=%s gameplay_frame=%u portal=%u drawmode=%d route=%s debug=%d passes=%s presenter=%s owner=%s denoiser_run=%u upscaler_run=%u exposure_run=%u present_kind=%s render_width=%u render_height=%u output_width=%u output_height=%u swapchain_format=%u hdr=%u prims=%u mats=%u scene_instances=%u static_instances=%u dynamic_instances=%u voxel_instances=%u emissive_instances=%u static_scene_upload_this_frame=%u static_scene_as_build_this_frame=%u runtime_voxel_onboarding_admitted=%u runtime_voxel_texture_prewarm_deferred=%u vertices=%u indices=%u vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu material_bytes=%llu instance_bytes=%llu scene_sig=0x%llx material_sig=0x%llx instance_sig=0x%llx sky_sig=0x%llx sky_mode=%s sky_source=%s sky_key=0x%llx sky_brightness=%.3f sky_action=%s auto_exposure=%u exposure_texture=%u exposure=%.6f target_exposure=%.6f adapted_exposure=%.6f metered_log_lum=%.6f exposure_stats_valid=%u exposure_stats_frame=%llu final_valid=%u final_nonzero=unknown final_nonzero_ratio=unknown final_luma_mean=unknown final_luma_min=unknown final_luma_max=unknown final_nan=unknown final_inf=unknown scene_reason=ok route_reason=ok exposure_reason=%s present_reason=ok\n",
		snapshot.traceFrameIndex,
		snapshot.engineFrameIndex,
		snapshot.mapName,
		snapshot.levelName,
		snapshot.graphicsApiName,
		snapshot.worldActive ? 1u : 0u,
		snapshot.menuActive ? "yes" : "no",
		snapshot.gameplayFrame ? 1u : 0u,
		snapshot.portal ? 1u : 0u,
		snapshot.drawmode,
		snapshot.route.routeName,
		snapshot.debugMode,
		snapshot.route.passes,
		snapshot.route.presenterName,
		snapshot.route.ownerName,
		snapshot.route.denoiserRun ? 1u : 0u,
		snapshot.route.upscalerRun ? 1u : 0u,
		snapshot.route.exposureRun ? 1u : 0u,
		snapshot.presentKind,
		snapshot.renderWidth,
		snapshot.renderHeight,
		snapshot.outputWidth,
		snapshot.outputHeight,
		snapshot.swapchainFormat,
		snapshot.hdr ? 1u : 0u,
		snapshot.primitiveCount,
		snapshot.materialCount,
		snapshot.sceneInstanceCount,
		snapshot.staticInstanceCount,
		snapshot.dynamicInstanceCount,
		snapshot.persistentVoxelInstanceCount,
		snapshot.emissiveInstanceCount,
		snapshot.staticSceneUploadThisFrame,
		snapshot.staticSceneAsBuildThisFrame,
		snapshot.runtimeVoxelOnboardingAdmitted,
		snapshot.runtimeVoxelTexturePrewarmDeferred,
		snapshot.vertexCount,
		snapshot.indexCount,
		(unsigned long long)snapshot.vertexBytes,
		(unsigned long long)snapshot.indexBytes,
		(unsigned long long)snapshot.primitiveBytes,
		(unsigned long long)snapshot.materialBytes,
		(unsigned long long)snapshot.instanceBytes,
		(unsigned long long)snapshot.sceneSignature,
		(unsigned long long)snapshot.materialSignature,
		(unsigned long long)snapshot.instanceSignature,
		(unsigned long long)snapshot.skySignature,
		snapshot.skyMode,
		snapshot.skySource,
		(unsigned long long)snapshot.skyKey,
		snapshot.skyBrightness,
		snapshot.skyAction,
		snapshot.autoExposure ? 1u : 0u,
		snapshot.exposureTexture ? 1u : 0u,
		snapshot.exposure,
		snapshot.targetExposure,
		snapshot.adaptedExposure,
		snapshot.meteredLogLuminance,
		snapshot.exposureStatsValid ? 1u : 0u,
		(unsigned long long)snapshot.exposureStatsFrame,
		snapshot.finalValid ? 1u : 0u,
		snapshot.exposureReason);
}

void NRIRenderer::ResetSelfTestRouteSnapshot()
{
	mDiagnostics.ResetSelfTestRouteSnapshot();
}

void NRIRenderer::SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passes, bool denoiserRun, bool upscalerRun, bool exposureRun)
{
	mDiagnostics.SetSelfTestRouteSnapshot(routeName, presenterName, ownerName, passes, denoiserRun, upscalerRun, exposureRun);
}

void NRIRenderer::PrintStatus()
{
	NRISyncLegacyUpscalerConfig(false);
	const NRIMainUpscalerKind requestedMain = GetSelectedMainUpscalerKind();
	const NRIMainUpscalerKind resolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind requestedPost = GetSelectedPostSharpenKind();
	const NRIPostSharpenKind resolvedPost = GetResolvedPostSharpenKindForStatus();
	const nri::UpscalerMode requestedUpscalerMode = GetSelectedUpscalerMode();
	const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMain, requestedUpscalerMode);
	const bool runAppTaa = NRIShouldRunAppTaa(resolvedMain);
	const float requestedRenderScale = std::max(0.33f, std::min((float)nri_renderscale, 1.0f));
	const float resolvedRenderScale = NRIResolveRenderScaleForMain(resolvedMain, requestedUpscalerMode, requestedRenderScale);
	const uint32_t bootstrapMode = GetBootstrapMode();
	const NRITraceSettings traceSettings = BuildNRITraceSettingsFromCVars();
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
	const NRITextureResource& srInput = GetFrameTexture(FrameTextureSlot::SrInput);
	const NRITextureResource& rrInput = GetFrameTexture(FrameTextureSlot::RrInput);
	const NRITextureResource& upscalerDepth = GetFrameTexture(FrameTextureSlot::UpscalerDepth);
	const NRITextureResource& vendorOutput = GetFrameTexture(FrameTextureSlot::VendorOutput);
	const NRITextureResource& postSharpenOutput = GetFrameTexture(FrameTextureSlot::PostSharpenOutput);
	const NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	const auto& frameGenPolicy = mFrameBuffer->mFrameGeneration.GetPolicy();
	const auto& frameGenPresentContract = mFrameBuffer->mFrameGeneration.GetPresentContract();
	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIPresentRouteInfo presentRoute = ResolvePresentRouteInfo(GetEffectivePtDebugMode(), !!nri_ptbootstrap);
	const nri::Format expectedFinalFormat = NRIFrameResources::ResolveFinalSceneFormat(*this);
	const bool hasFrameGenDesc = mFrameBuffer->mFrameGeneration.HasFrameDesc();
	const auto& frameGenDesc = mFrameBuffer->mFrameGeneration.GetFrameDesc();
	const auto& frameGenAudit = mFrameBuffer->mFrameGeneration.GetInputAudit();
	const auto& frameGenProvider = mFrameBuffer->mFrameGeneration.GetProviderState();
	const NRIAutoExposureSettings autoExposureSettings = GetNRIAutoExposureSettings(
		outputPolicy.exposure,
		IsNRIPTHdrOutputActive(outputPolicy));
	mExposure.SetSettings(autoExposureSettings);
	ReadbackAutoExposureStats();
	const NRIAutoExposureStatus& autoExposureStatus = mExposure.GetStatus();
	const NRIMainUpscalerKind autoExposureResolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind autoExposureResolvedPost = GetResolvedPostSharpenKindForStatus();
	const FrameTextureSlot autoExposurePresentSlot = mUseUpscaledInFinal ? mUpscaledInputSlot : mHistoryOutputSlot;
	const ExposureRoute autoExposurePresentRoute = ResolveExposureRoute(
		autoExposurePresentSlot,
		outputPolicy,
		autoExposureResolvedMain,
		autoExposureResolvedPost);
	const NRITextureResource* autoExposureStateTexture = mExposure.GetExposureStateTexture(mFrameIndex & 1u);
	const bool autoExposureSceneHdrInput = autoExposurePresentRoute.inputDomain == ExposureDomain::SceneHDR;
	const bool autoExposureTextureValid =
		autoExposureStateTexture != nullptr &&
		autoExposureStateTexture->shaderView != nullptr;
	const bool autoExposurePresentEligible =
		autoExposureSettings.enabled &&
		autoExposureSceneHdrInput &&
		autoExposureTextureValid;
	const bool vendorExposurePath = NRIUsesNriUpscalerProvider(autoExposureResolvedMain);
	const bool vendorExposureEngine =
		vendorExposurePath &&
		autoExposureResolvedMain != NRIMainUpscalerKind::FSR &&
		autoExposureSettings.enabled &&
		autoExposureTextureValid;
	const char* vendorExposureMode =
		!vendorExposurePath ? "none" :
		vendorExposureEngine ? "engine" :
		"vendor-auto";

	Printf("NRI PT status: support=%s", mPathTracingSupported ? "available" : "raster-fallback");
	if (!mPathTracingSupported)
	{
		Printf(" (%s)", GetAvailabilityReason());
	}
	Printf("\n");
	Printf("NRI PT frame: index=%u fg_frame_id=%llu render=%ux%u output=%ux%u prev_camera=%s reset_history=%s\n",
		mFrameIndex,
		(unsigned long long)mFrameGenerationFrameId,
		mRenderWidth,
		mRenderHeight,
		mOutputWidth,
		mOutputHeight,
		mHasPreviousCameraState ? "yes" : "no",
		mResetHistory ? "yes" : "no");
	Printf("NRI PT output: requested_mode=%s resolved_mode=%s control_block=%s tonemap=%s exposure=%.3f contrast=%.3f saturation=%.3f shoulder=%.3f toe=%.3f paper_white=%.1f offscreen_hdr=%s hdr_swapchain=%s display_info=%s display_hdr=%s display_sdr_nits=%.1f display_max_nits=%.1f\n",
		GetNRIPTOutputModeName(outputPolicy.requestedMode),
		GetNRIPTOutputModeName(outputPolicy.resolvedMode),
		GetNRIPTOutputControlBlockName(outputPolicy),
		GetNRIPTTonemapModeName(outputPolicy.tonemapMode),
		outputPolicy.exposure,
		outputPolicy.contrast,
		outputPolicy.saturation,
		outputPolicy.shoulder,
		outputPolicy.toe,
		outputPolicy.paperWhiteNits,
		outputPolicy.offscreenHdrTarget ? "yes" : "no",
		outputPolicy.hdrSwapChainActive ? "yes" : "no",
		outputPolicy.displayInfoAvailable ? "yes" : "no",
		outputPolicy.displayHdrSupported ? "yes" : "no",
		outputPolicy.displaySdrLuminance,
		outputPolicy.displayMaxLuminance);
	Printf("NRI PT auto exposure: enabled=%s control_block=%s freeze=%s stats=%s resources=%s state_textures=%s meter_source=%s meter_mode=%s histogram_bins=%u sample_step=%u target=%.3f range=%.3f..%.3f bias=%.3f percentiles=%.2f..%.2f hist_log_range=%.1f..%.1f adapt=%.3f/%.3f fallback_manual=%.3f resource_render=%ux%u memory=%llu alloc_serial=%u reset_pending=%s reset_serial=%llu reset_request_frame=%llu reset_consumed_frame=%llu reset_reason=%s\n",
		YesNo(autoExposureSettings.enabled),
		autoExposureSettings.hdrControlsActive ? "hdr" : "sdr",
		YesNo(autoExposureSettings.freeze),
		YesNo(autoExposureSettings.stats),
		YesNo(autoExposureStatus.resourcesAllocated),
		autoExposureStatus.resourcesAllocated ? "allocated" : "not_allocated",
		GetFrameTextureSlotName(mAutoExposureInputSourceSlot),
		GetNRIAutoExposureMeteringModeName(autoExposureSettings.meteringMode),
		autoExposureSettings.histogramBinCount,
		autoExposureSettings.sampleStep,
		autoExposureSettings.targetLuminance,
		autoExposureSettings.minExposure,
		autoExposureSettings.maxExposure,
		autoExposureSettings.exposureBias,
		autoExposureSettings.lowPercentile,
		autoExposureSettings.highPercentile,
		NRI_EXPOSURE_LOG_LUMINANCE_MIN,
		NRI_EXPOSURE_LOG_LUMINANCE_MAX,
		autoExposureSettings.adaptUpSpeed,
		autoExposureSettings.adaptDownSpeed,
		autoExposureSettings.fallbackManualExposure,
		autoExposureStatus.renderWidth,
		autoExposureStatus.renderHeight,
		(unsigned long long)autoExposureStatus.memoryBytes,
		autoExposureStatus.allocationSerial,
		YesNo(autoExposureStatus.resetPending),
		(unsigned long long)autoExposureStatus.resetSerial,
		(unsigned long long)autoExposureStatus.resetRequestFrame,
		(unsigned long long)autoExposureStatus.resetConsumedFrame,
		autoExposureStatus.resetReason[0] != '\0' ? autoExposureStatus.resetReason : "none");
	Printf("NRI PT auto exposure stats: valid=%s readback=%s frame=%llu samples=%u bins=%u..%u log_lum=%.3f..%.3f metered_log_lum=%.3f target_exposure=%.3f adapted_exposure=%.3f target_ev=%.3f adapted_ev=%.3f\n",
		YesNo(autoExposureStatus.debugValid),
		YesNo(autoExposureStatus.debugReadbackAllocated),
		(unsigned long long)autoExposureStatus.debugFrameIndex,
		autoExposureStatus.sampleCount,
		autoExposureStatus.lowBin,
		autoExposureStatus.highBin,
		autoExposureStatus.lowLogLuminance,
		autoExposureStatus.highLogLuminance,
		autoExposureStatus.meteredLogLuminance,
		autoExposureStatus.targetExposure,
		autoExposureStatus.adaptedExposure,
		std::log2(std::max(autoExposureStatus.targetExposure, 1.0e-6f)),
		std::log2(std::max(autoExposureStatus.adaptedExposure, 1.0e-6f)));
	Printf("NRI PT auto exposure present: slot=%s domain=%s enabled=%s scene_hdr=%s texture_valid=%s apply=%s manual_fallback=%.3f\n",
		GetFrameTextureSlotName(autoExposurePresentSlot),
		GetExposureDomainName(autoExposurePresentRoute.inputDomain),
		YesNo(autoExposureSettings.enabled),
		YesNo(autoExposureSceneHdrInput),
		YesNo(autoExposureTextureValid),
		YesNo(autoExposurePresentEligible),
		autoExposurePresentRoute.presentExposure);
	Printf("NRI PT auto exposure vendor: main=%s mode=%s texture_valid=%s engine_enabled=%s recreate_on_policy_change=yes\n",
		NRIGetMainUpscalerName(autoExposureResolvedMain),
		vendorExposureMode,
		YesNo(autoExposureTextureValid),
		YesNo(autoExposureSettings.enabled));
	Printf("NRI PT nightvision: mode=%s view_eligible=%s active=%s presenter=%s strength=%.3f remaining_s=%.3f\n",
		GetNightVisionModeName(mNightVisionState.mode),
		YesNo(mNightVisionState.viewEligible),
		YesNo(mNightVisionState.enabled),
		nri_ptnightvision ? "on" : "off",
		mNightVisionState.strength01,
		mNightVisionState.remainingSeconds);
	Printf("NRI PT nightvision tuning: exposure=%.3f contrast=%.3f saturation=%.3f\n",
		(float)nri_ptnightvisionexposure,
		(float)nri_ptnightvisioncontrast,
		(float)nri_ptnightvisionsaturation);
	Printf("NRI PT nightvision tint: red=%.3f green=%.3f blue=%.3f\n",
		(float)nri_ptnightvisionred,
		(float)nri_ptnightvisiongreen,
		(float)nri_ptnightvisionblue);
	Printf("NRI PT material calibration: fullbright_boost=%.3f voxel_emission_boost=%.3f\n",
		(float)nri_ptfullbrightboost,
		(float)nri_voxelemissionboost);
	if (outputPolicy.hdrSwapChainActive)
	{
		const float safeDisplaySdr = std::max(outputPolicy.displaySdrLuminance, 1.0f);
		const float safeDisplayMax = std::max(outputPolicy.displayMaxLuminance, safeDisplaySdr);
		const float safePaperWhite = std::clamp(std::max(outputPolicy.paperWhiteNits, safeDisplaySdr), safeDisplaySdr, safeDisplayMax);
		const float hdrPaperWhiteScale = safePaperWhite / 80.0f;
		const float hdrHeadroom = std::max(safeDisplayMax / safePaperWhite, 1.0f);
		const float hdrMaxScale = hdrPaperWhiteScale * hdrHeadroom;
		Printf("NRI PT output hdr: paper_scale=%.3f headroom=%.3f max_scale=%.3f active_linear16=%s\n",
			hdrPaperWhiteScale,
			hdrHeadroom,
			hdrMaxScale,
			outputPolicy.resolvedMode == NRIPTOutputMode::HDRLinear16 ? "yes" : "no");
	}
	Printf("NRI PT routing: debug=%u route=%s presenter=%s owner=%s root_bytes=scene:%u temporal:%u present:%u\n",
		GetEffectivePtDebugMode(),
		presentRoute.routeName,
		presentRoute.presenterName,
		presentRoute.ownerName,
		(unsigned)sizeof(NRITraceSceneConstants),
		(unsigned)sizeof(NRITemporalConstants),
		(unsigned)sizeof(NRIPresentConstants));
	Printf("NRI PT features: bootstrap=%s denoise=%s validation=%s api_validation=%s dred=%s main_upscaler=%s->%s post_sharpen=%s->%s requested_mode=%s resolved_mode=%s requested_render_scale=%.3f resolved_render_scale=%.3f sharpness=%.3f\n",
		nri_ptbootstrap ? "on" : "off",
		nri_denoise ? "on" : "off",
		nri_validation ? "on" : "off",
		nri_apivalidation ? "on" : "off",
		nri_dred ? "on" : "off",
		NRIGetMainUpscalerName(requestedMain),
		NRIGetMainUpscalerName(resolvedMain),
		NRIGetPostSharpenName(requestedPost),
		NRIGetPostSharpenName(resolvedPost),
		NRIGetUpscalerModeName(requestedUpscalerMode),
		NRIGetUpscalerModeName(resolvedUpscalerMode),
		requestedRenderScale,
		resolvedRenderScale,
		(float)nri_sharpness);
	Printf("NRI PT framegen policy: request=%s provider=%s operational=%s owner=%s output=%s->%s contract=%s scope=%s api=%s shader_model=%u.%u window=%s dxgi=%s supported=%s low_latency=%s->%s(avail=%s iface=%s swapchain=%s pacing=%s) async=%s->%s(avail=%s) ui=%s->%s swapchain=%s native=device:%s queue:%s swapchain:%s waitable=%s runtime=%s frame_desc=%s reason=%s\n",
		frameGenPolicy.requestedEnabled ? "on" : "off",
		NRIFrameGenerationContext::GetProviderName(frameGenPolicy.provider),
		frameGenPolicy.operational ? "yes" : "no",
		frameGenPolicy.ffxProxyPacing ? "proxy" : (mFrameBuffer->mSwapChain != nullptr ? "native" : "none"),
		GetNRIPTOutputModeName(frameGenPolicy.requestedOutputMode),
		GetNRIPTOutputModeName(frameGenPolicy.resolvedOutputMode),
		NRIFrameGenerationContext::GetOutputContractName(frameGenPolicy.resolvedOutputContract),
		frameGenPolicy.outputContractScope,
		frameGenPolicy.selectedApiName,
		frameGenPolicy.shaderModel / 10u,
		frameGenPolicy.shaderModel % 10u,
		NRIFrameGenerationContext::GetWindowModeName(frameGenPolicy.windowPresentationMode),
		NRIFrameGenerationContext::GetDxgiFullscreenStateName(frameGenPolicy.dxgiFullscreenKnown, frameGenPolicy.dxgiFullscreen),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.windowModeSupported),
		frameGenPolicy.requestedLowLatency ? "on" : "off",
		frameGenPolicy.resolvedLowLatency ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencyInterfaceAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.lowLatencySwapChainEnabled),
		NRIFrameGenerationContext::GetPacingModeName(frameGenPolicy),
		frameGenPolicy.requestedAsync ? "on" : "off",
		frameGenPolicy.resolvedAsync ? "on" : "off",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.asyncWorkloadAvailable),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.requestedUiMode),
		NRIFrameGenerationContext::GetUiModeName(frameGenPolicy.resolvedUiMode),
		frameGenPolicy.swapChainReady ? "ready" : "cold",
		frameGenPolicy.nativeDeviceAvailable ? "ok" : "missing",
		frameGenPolicy.nativeGraphicsQueueAvailable ? "ok" : "missing",
		frameGenPolicy.nativeSwapChainAvailable ? "ok" : "missing",
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.waitableSwapChainAvailable),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPolicy.providerRuntimeSupported),
		hasFrameGenDesc ? "captured" : "empty",
		frameGenPolicy.resolvedReason);
	Printf("NRI PT framegen present contract: output=%s->%s proxy=%s hdr_swapchain=%s hdr_context=%s swapchain=%s texture=%s active=%s dxgi=%s active_dxgi=%s requested_color_space=%s observed_color_space=%s transfer=%s min_nits=%.3f max_nits=%.3f hdr_scale=%.3f reason=%s\n",
		GetNRIPTOutputModeName(frameGenPresentContract.requestedOutputMode),
		GetNRIPTOutputModeName(frameGenPresentContract.resolvedOutputMode),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPresentContract.proxyAllowed),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPresentContract.usesHdrSwapChain),
		NRIFrameGenerationContext::GetAvailabilityName(frameGenPresentContract.hdrContext),
		NRIFrameGenerationContext::GetSwapChainFormatName(frameGenPresentContract.createdSwapChainFormat),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.resolvedTextureFormat),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.activePresentTargetFormat),
		frameGenPresentContract.resolvedDxgiFormatValid ? NRIFrameGenerationContext::GetDxgiFormatName(frameGenPresentContract.resolvedDxgiFormat) : "unknown",
		frameGenPresentContract.activePresentTargetDxgiFormatValid ? NRIFrameGenerationContext::GetDxgiFormatName(frameGenPresentContract.activePresentTargetDxgiFormat) : "unknown",
		frameGenPresentContract.requestedDxgiColorSpaceValid ? NRIFrameGenerationContext::GetDxgiColorSpaceName(frameGenPresentContract.requestedDxgiColorSpace) : "unknown",
		frameGenPresentContract.observedDxgiColorSpaceValid ? NRIFrameGenerationContext::GetDxgiColorSpaceName(frameGenPresentContract.observedDxgiColorSpace) : "unknown",
		NRIFrameGenerationContext::GetPresentTransferFunctionName(frameGenPresentContract.transferFunction),
		frameGenPresentContract.minLuminanceNits,
		frameGenPresentContract.maxLuminanceNits,
		frameGenPresentContract.hdrPaperWhiteScale,
		frameGenPresentContract.resolvedReason);
	Printf("NRI PT final surface: expected=%s allocated=%s contract=%s active=%s size=%ux%u\n",
		NRIFrameGenerationContext::GetNriFormatName(expectedFinalFormat),
		NRIFrameGenerationContext::GetNriFormatName(final.format),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.resolvedTextureFormat),
		NRIFrameGenerationContext::GetNriFormatName(frameGenPresentContract.activePresentTargetFormat),
		final.width,
		final.height);
	Printf("NRI PT framegen provider: runtime=%s funcs=%s context=%s swapctx=%s bridge=%s debug=%s no_swapchain_notify=%s cfg=%s prepare=%s fg_dispatch=%s ui_reg=%s camera=%s lib=%s version=%s dims=render:%ux%u display:%ux%u counts=cfg:%llu prep:%llu fg:%llu frames=%llu/%llu query=%s/%s create=%s/%s config=%s/%s prepare=%s dispatch=%s vram=fg:%s:%llu/%llu sc:%s:%llu/%llu resets=%llu last_reset=%s present=%s/%s count=%llu reason=%s\n",
		frameGenProvider.runtimeLoaded ? "yes" : "no",
		frameGenProvider.runtimeFunctionsLoaded ? "yes" : "no",
		frameGenProvider.contextCreated ? "yes" : "no",
		frameGenProvider.swapChainContextCreated ? "yes" : "no",
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.debugConfigured ? "yes" : "no",
		frameGenProvider.noSwapChainNotify ? "yes" : "no",
		frameGenProvider.configuredThisFrame ? "yes" : "no",
		frameGenProvider.prepareDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.uiResourceRegisteredThisFrame ? "yes" : "no",
		frameGenProvider.prepareCameraInfoProvided ? "yes" : "no",
		frameGenProvider.runtimeLibrary,
		frameGenProvider.providerVersion,
		frameGenProvider.contextRenderWidth,
		frameGenProvider.contextRenderHeight,
		frameGenProvider.contextDisplayWidth,
		frameGenProvider.contextDisplayHeight,
		(unsigned long long)frameGenProvider.configureCount,
		(unsigned long long)frameGenProvider.prepareCount,
		(unsigned long long)frameGenProvider.dispatchCount,
		(unsigned long long)frameGenProvider.lastConfiguredFrameId,
		(unsigned long long)frameGenProvider.lastPreparedFrameId,
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainQueryResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainCreateResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastSwapChainConfigureResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastPrepareResult),
		NRIFrameGenerationContext::GetProviderReturnCodeName(frameGenProvider.lastDispatchResult),
		frameGenProvider.memoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.totalUsageBytes,
		(unsigned long long)frameGenProvider.aliasableUsageBytes,
		frameGenProvider.swapChainMemoryUsageValid ? "yes" : "no",
		(unsigned long long)frameGenProvider.swapChainTotalUsageBytes,
		(unsigned long long)frameGenProvider.swapChainAliasableUsageBytes,
		(unsigned long long)frameGenProvider.resetCount,
		frameGenProvider.lastResetReason,
		frameGenProvider.lastPresentMode,
		NRIFrameGenerationContext::GetPresentResultName(frameGenProvider.lastPresentResult),
		(unsigned long long)frameGenProvider.presentCount,
		frameGenProvider.lastStatusReason);
	Printf("NRI PT framegen present: current=%s bridge_active=%s generated=%s fallback_pending=%s last=%s result=%s\n",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "generated" :
			(frameGenProvider.presentUsedBridgeThisFrame ? "passthrough" : "native"),
		frameGenProvider.presentBridgeReady ? "yes" : "no",
		frameGenProvider.frameGenerationDispatchedThisFrame ? "yes" : "no",
		frameGenProvider.nativeFallbackRequested ? "yes" : "no",
		frameGenProvider.lastPresentMode,
		NRIFrameGenerationContext::GetPresentResultName(frameGenProvider.lastPresentResult));
	if (hasFrameGenDesc)
	{
		Printf("NRI PT framegen inputs: frame_id=%llu hudless=%s:%ux%u ui=%ux%u motion=%ux%u depth=%ux%u render_rect=%d,%d+%ux%u output_rect=%d,%d+%ux%u reset=%s prev_camera=%s frame_time=%s frame_time_ms=%.3f\n",
			(unsigned long long)frameGenDesc.frameId,
			NRIFrameGenerationContext::GetColorSourceName(frameGenDesc.hudlessColorSource),
			frameGenDesc.hudlessColor != nullptr ? frameGenDesc.hudlessColor->width : 0u,
			frameGenDesc.hudlessColor != nullptr ? frameGenDesc.hudlessColor->height : 0u,
			frameGenDesc.uiTexture != nullptr ? frameGenDesc.uiTexture->width : 0u,
			frameGenDesc.uiTexture != nullptr ? frameGenDesc.uiTexture->height : 0u,
			frameGenDesc.motionVectors != nullptr ? frameGenDesc.motionVectors->width : 0u,
			frameGenDesc.motionVectors != nullptr ? frameGenDesc.motionVectors->height : 0u,
			frameGenDesc.depth != nullptr ? frameGenDesc.depth->width : 0u,
			frameGenDesc.depth != nullptr ? frameGenDesc.depth->height : 0u,
			frameGenDesc.renderRect.left,
			frameGenDesc.renderRect.top,
			frameGenDesc.renderRect.width,
			frameGenDesc.renderRect.height,
			frameGenDesc.outputRect.left,
			frameGenDesc.outputRect.top,
			frameGenDesc.outputRect.width,
			frameGenDesc.outputRect.height,
			frameGenDesc.resetReason[0] != '\0' ? frameGenDesc.resetReason : "none",
			frameGenDesc.hasPreviousCamera ? "yes" : "no",
			frameGenDesc.hasRealFrameTimeMs ? "captured" : "pending",
			frameGenDesc.realFrameTimeMs);
		Printf("NRI PT framegen contract: motion=%s/%s scale=%.3f,%.3f depth=%s inverted=%s infinite=%s jitter=current(%.3f,%.3f) prev(%.3f,%.3f) fsr3=motion:%s depth:%s prepare:%s adapter:%s reason=%s\n",
			NRIFrameGenerationContext::GetMotionVectorSpaceName(frameGenDesc.motionVectorSpace),
			NRIFrameGenerationContext::GetMotionVectorDirectionName(frameGenDesc.motionVectorDirection),
			frameGenDesc.motionVectorScale[0],
			frameGenDesc.motionVectorScale[1],
			NRIFrameGenerationContext::GetDepthTypeName(frameGenDesc.depthType),
			frameGenDesc.depthInverted ? "yes" : "no",
			frameGenDesc.depthInfinite ? "yes" : "no",
			frameGenDesc.cameraJitter[0],
			frameGenDesc.cameraJitter[1],
			frameGenDesc.previousCameraJitter[0],
			frameGenDesc.previousCameraJitter[1],
			frameGenAudit.fsr3MotionCompatible ? "yes" : "no",
			frameGenAudit.fsr3DepthCompatible ? "yes" : "no",
			frameGenAudit.fsr3PrepareInputsRequired ? "yes" : "no",
			NRIFrameGenerationContext::GetAdapterRequirementName(frameGenAudit.adapterRequirement),
			frameGenAudit.statusReason);
	}
	Printf("NRI PT resolution policy: policy=%s render=%ux%u output=%ux%u jitter=%s phases=%u\n",
		NRIGetRenderResolutionPolicyName(resolvedMain),
		mRenderWidth,
		mRenderHeight,
		mOutputWidth,
		mOutputHeight,
		NRIGetTemporalJitterModeName(resolvedMain, mGuiCaptureActive),
		NRIGetTemporalJitterPhaseCount(resolvedMain, resolvedUpscalerMode, mGuiCaptureActive));
	Printf("NRI PT output shell: family=%s sr_input=%ux%u rr_input=%ux%u guides=%ux%u vendor=%ux%u post_output=%ux%u post=%s active=%s last_reset_reason=%s\n",
		GetUpscalerFamilyName(resolvedMain, runAppTaa),
		srInput.width,
		srInput.height,
		rrInput.width,
		rrInput.height,
		upscalerDepth.width,
		upscalerDepth.height,
		vendorOutput.width,
		vendorOutput.height,
		postSharpenOutput.width,
		postSharpenOutput.height,
		NRIGetPostSharpenName(resolvedPost),
		resolvedPost == NRIPostSharpenKind::Off ? "pre-post" : "post-sharpen-output",
		mLastHistoryResetReason.c_str());
	Printf("NRI PT tracing: direct_scene_fallback=%s light_bounces=%u mirror_bounces=%u portal_depth=%u surface_probe=%d ceiling_nudge=%s ceiling_nudge_distance=%.4f\n",
		nri_ptdirectscene ? "on" : "off",
		traceSettings.lightBounceCount,
		traceSettings.mirrorBounceCount,
		traceSettings.portalDepth,
		(int)nri_ptsurfaceprobe,
		nri_ptceilingnudge ? "on" : "off",
		(float)nri_ptceilingnudgedistance);
	Printf("NRI PT lighting shell: directional=%s sector=%s\n",
		mDirectionalLightState.enabled ? "on" : "off",
		nri_ptsectorlighting ? "on" : "off");
	Printf("NRI PT directional light: source=%s shadow=%s rule=%u dir=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) angular=%.3f\n",
		GetDirectionalLightSourceName(mDirectionalLightState),
		mDirectionalLightState.enabled && mDirectionalLightState.shadow ? "on" : "off",
		mDirectionalLightState.ruleId,
		mDirectionalLightState.direction[0],
		mDirectionalLightState.direction[1],
		mDirectionalLightState.direction[2],
		mDirectionalLightState.color[0],
		mDirectionalLightState.color[1],
		mDirectionalLightState.color[2],
		mDirectionalLightState.angularSize);
	Printf("NRI PT transparent shell: trace_transparent=placeholder_noop\n");
	uint32_t emissiveBaseCount = 0;
	uint32_t emissiveConstantCount = 0;
	uint32_t emissiveGlowmapCount = 0;
	for (const auto& surface : mSceneLights.GetEmissiveSurfaces().activeSurfaces)
	{
		switch (surface.emissiveMode)
		{
		case nri_scene::MaterialEmissiveMode_UseBaseTexture: emissiveBaseCount++; break;
		case nri_scene::MaterialEmissiveMode_UseConstantColor: emissiveConstantCount++; break;
		case nri_scene::MaterialEmissiveMode_UseGlowmapTexture: emissiveGlowmapCount++; break;
		default: break;
		}
	}
	Printf("NRI PT NRD: integration=%s requested=%s validation_output=%s denoiser=%s motion=%s prev_position=%s extra_debugs=%s\n",
		mNrd.IsReady() ? "ready" : "cold",
		nri_denoise ? "on" : "off",
		nri_validation ? "expected" : "disabled",
		GetNrdDenoiserModeName(denoiserSettings.denoiserMode),
		"2.5D",
		"interpolated",
		"16=denoised_diff 17=denoised_spec 18=metalness 19=roughness 20=motion_z 21=live_raw_penumbra 22=live_raw_shadow 23=temporal_sigma_shadow 24=direct_lighting 25=direct_emission 26=analytic_direct 27=emissive_tags 28=emissive_direct 29=sector_ambient 30=emissive_uv 31=emissive_radiance 32=emissive_primitive 33=emissive_visibility 34=trace_transparent 35=sr_input 36=sr_depth 37=vendor_output 38=vendor_output_final 39=rr_input 40=rr_diffuse_albedo 41=rr_specular_albedo 42=rr_normal_roughness 43=rr_specular_hit_distance 44=post_sharpen_output 45=taa_pre_exposed_input 46=indirect_lobe_selection 47=motion_validity");
	const char* shadowSplitMode =
		!mUseSplitShadowDenoiser ? "off" :
		(GetEffectivePtDebugMode() >= 21 && GetEffectivePtDebugMode() <= 23) ? "sigma-debug" :
		"sigma-beauty";
	Printf("NRI PT NRD settings: max_frames=%u fast_frames=%u stabilization_frames=%u anti_firefly=%s hit_recon=%s input_split=%s shadow_split=%s\n",
		denoiserSettings.maxAccumulatedFrameNum,
		denoiserSettings.maxFastAccumulatedFrameNum,
		denoiserSettings.maxStabilizedFrameNum,
		denoiserSettings.enableAntiFirefly ? "on" : "off",
		GetNrdHitDistanceReconstructionModeName(denoiserSettings.hitDistanceReconstructionMode),
		GetNrdInputSplitModeName(denoiserSettings.inputSplitMode),
		shadowSplitMode);
	Printf("NRI PT SIGMA tuning: stabilization_frames=%u plane_distance_sensitivity=%.3f\n",
		denoiserSettings.sigmaMaxStabilizedFrameNum,
		denoiserSettings.sigmaPlaneDistanceSensitivity);
	if (denoiserSettings.denoiserMode == NRINrdDenoiserMode::Relax)
	{
		Printf("NRI PT NRD tuning: fast_history_sigma=%.2f prepass=%.2f/%.2f material_floor=1/2 blur_radius=n/a_relax\n",
			denoiserSettings.fastHistoryClampingSigmaScale,
			denoiserSettings.diffusePrepassBlurRadius,
			denoiserSettings.specularPrepassBlurRadius);
	}
	else
	{
		Printf("NRI PT NRD tuning: fast_history_sigma=%.2f blur_radius=%.2f..%.2f prepass=%.2f/%.2f material_floor=1/2\n",
			denoiserSettings.fastHistoryClampingSigmaScale,
			denoiserSettings.minBlurRadius,
			denoiserSettings.maxBlurRadius,
			denoiserSettings.diffusePrepassBlurRadius,
			denoiserSettings.specularPrepassBlurRadius);
	}
	Printf("NRI PT NRD guides: diffuse_signal=primary_demodulated_radiance specular_signal=primary_demodulated_radiance hit_distance=%s roughness=material_hint metalness=material_hint material_id=semantic_class\n",
		denoiserSettings.denoiserMode == NRINrdDenoiserMode::Relax ? "first_secondary_linear_hitdist" : "first_secondary_reblur_norm");
	Printf("NRI PT scene stats: %s\n", nri_ptscenestats ? "on" : "off");
	Printf("NRI PT mutation trace: chunk=%d sector=%d\n",
		(int)nri_ptmutationtracechunk,
		(int)nri_ptmutationtracesector);
	Printf("NRI PT runtime link trace: %s\n", nri_ptruntimelinktrace ? "on" : "off");
	Printf("NRI PT analytic lights: active=%u manual=%u muzzle_slots=%u muzzle_active=%u rules=%u topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u ordered_key_hash=0x%016llx soft=%u surviving_index_changed=%u surviving_soft_index_changed=%u limit=%u\n",
		(uint32_t)mSceneLights.GetAnalyticLights().activeLights.size(),
		(uint32_t)mSceneLights.GetAnalyticLights().manualLights.size(),
		mSceneLights.GetAnalyticLights().transientMuzzleSlotCount,
		mSceneLights.GetAnalyticLights().transientMuzzleActiveCount,
		(uint32_t)mSceneLights.GetAnalyticLights().spriteTileRules.size(),
		YesNo(mSceneLights.GetAnalyticLights().lastBuildTopologyChanged),
		YesNo(mSceneLights.GetAnalyticLights().lastBuildPropertiesChanged),
		(uint32_t)mSceneLights.GetAnalyticLights().addedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetAnalyticLights().removedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetAnalyticLights().reboundTopologyKeys.size(),
		(unsigned long long)mSceneLights.GetAnalyticLights().orderedStableKeyHash,
		mSceneLights.GetAnalyticLights().softLightCount,
		mSceneLights.GetAnalyticLights().survivingKeyIndexChangeCount,
		mSceneLights.GetAnalyticLights().survivingSoftLightIndexChangeCount,
		NRI_MAX_RUNTIME_POINT_LIGHTS);
	Printf("NRI PT analytic clusters: tile=%u grid=%ux%u used_indices=%u max_occupancy=%u debug_mode=%u\n",
		mBoundRuntimeLightTileSize,
		mBoundRuntimeLightTileCountX,
		mBoundRuntimeLightTileCountY,
		mBoundRuntimeLightTileIndexCount,
		mBoundRuntimeLightMaxTileOccupancy,
		nri_diag::PtDebugAnalyticDirect);
	Printf("NRI PT emissive surfaces: active=%u rules=%u auto=%u explicit=%u overrides=%u override_matches=%u material_response_rules=%u material_response_matches=%u total_power=%.3f topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u debug_mode=%u/%u thresholds=area>=%.3f power>=%.3f light=[%.3f,%.3f] reach=[%.3f,%.3f] glow_scale=%.3f glow_reach=%.3f glow_falloff=%.3f glow_blend=%.3f\n",
		(uint32_t)mSceneLights.GetEmissiveSurfaces().activeSurfaces.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().textureRules.size(),
		mSceneLights.GetEmissiveSurfaces().autoTaggedCount,
		mSceneLights.GetEmissiveSurfaces().explicitRuleMatchCount,
		mSceneLights.GetEmissiveSurfaces().overrideRuleCount,
		mSceneLights.GetEmissiveSurfaces().overrideMatchedSurfaceCount,
		mSceneLights.GetEmissiveSurfaces().materialResponseRuleCount,
		mSceneLights.GetEmissiveSurfaces().materialResponseMatchedSurfaceCount,
		mSceneLights.GetEmissiveSurfaces().totalPowerEstimate,
		YesNo(mSceneLights.GetEmissiveSurfaces().lastBuildTopologyChanged),
		YesNo(mSceneLights.GetEmissiveSurfaces().lastBuildPropertiesChanged),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().addedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().removedTopologyKeys.size(),
		(uint32_t)mSceneLights.GetEmissiveSurfaces().reboundTopologyKeys.size(),
		nri_diag::PtDebugEmissiveTags,
		nri_diag::PtDebugEmissiveDirect,
		(float)nri_ptemissiveminsurface,
		(float)nri_ptemissiveminpower,
		(float)nri_ptsectoremissionlightmin,
		(float)nri_ptsectoremissionlightmax,
		(float)nri_ptsectoremissionreachmin,
		(float)nri_ptsectoremissionreachmax,
		(float)nri_ptglowscale,
		(float)nri_ptglowreach,
		(float)nri_ptglowfalloff,
		(float)nri_ptglowblend);
	const auto& appendStats = mSceneLights.GetFrameAppendStats();
	Printf("NRI PT scene-light ingest: records=%u static=%u mutation=%u captured=%u dynamic=%u append_ms=static:%.3f mutation:%.3f captured:%.3f dynamic:%.3f rebuild_ms=analytic:%.3f emissive:%.3f sector:%.3f\n",
		appendStats.totalRecordCount,
		appendStats.staticRecordCount,
		appendStats.runtimeMutationRecordCount,
		appendStats.capturedRecordCount,
		appendStats.dynamicRecordCount,
		mLastPerfShellTraceStats.sceneLightStaticAppendMs,
		mLastPerfShellTraceStats.sceneLightRuntimeMutationAppendMs,
		mLastPerfShellTraceStats.sceneLightCapturedAppendMs,
		mLastPerfShellTraceStats.sceneLightDynamicAppendMs,
		mLastPerfShellTraceStats.sceneLightAnalyticMs,
		mLastPerfShellTraceStats.sceneLightEmissiveMs,
		mLastPerfShellTraceStats.sceneLightSectorMs);
	Printf("NRI PT emissive sources: base=%u glowmap=%u constant=%u\n",
		emissiveBaseCount,
		emissiveGlowmapCount,
		emissiveConstantCount);
	Printf("NRI PT emissive sampling: primitives=%u total_power=%.3f samples=%u dominant_tile=%u dominant_primitive=%u dominant_source=%s dominant_power=%.3f dominant_flags=0x%x debug_mode=%u\n",
		mBoundEmissivePrimitiveCount,
		mBoundEmissiveTotalPower,
		traceSettings.emissiveSampleCount,
		mBoundEmissiveDominantTile,
		mBoundEmissiveDominantPrimitive,
		nri_diag::GetSceneDataSourceName(mBoundEmissiveDominantDataSource),
		mBoundEmissiveDominantPower,
		mBoundEmissiveDominantFlags,
		nri_diag::PtDebugEmissiveSampleVisibility);
	Printf("NRI PT emissive query: tlas=%s fast_shadow=%s instances=%u static=%u dynamic=%u builds=%u\n",
		nri_ptemissivetlas ? "on" : "off",
		nri_ptemissivefastshadow ? "on" : "off",
		mEmissiveTlasInstanceCount,
		mEmissiveTlasStaticInstanceCount,
		mEmissiveTlasDynamicInstanceCount,
		mEmissiveTlasBuildCount);
	Printf("NRI PT sector lighting: enabled=%s active=%u raw_active=%u raw_nonneutral=%u response=boost:%u dim:%u neutral:%u eligible=%u fog=%u pulsing=%u debug_mode=%u multiplier=%.3f scales=ambient=%.3f hemi=%.3f fog=%.3f clamp=%.3f sector_response=%.3f/[%.3f,%.3f] intensity=[%.3f,%.3f] reach=[%.3f,%.3f] filter=pal=%d shade=[%d,%d] lotag=%d pulse=%d/%.3f\n",
		nri_ptsectorlighting ? "on" : "off",
		mSceneLights.GetSectorLighting().activeSectorCount,
		mSceneLights.GetSectorLighting().rawActiveSectorCount,
		mSceneLights.GetSectorLighting().rawNonNeutralSectorCount,
		mSceneLights.GetSectorLighting().responseBoostSectorCount,
		mSceneLights.GetSectorLighting().responseDimSectorCount,
		mSceneLights.GetSectorLighting().responseNeutralSectorCount,
		mSceneLights.GetSectorLighting().eligibleSectorCount,
		mSceneLights.GetSectorLighting().fogSectorCount,
		mSceneLights.GetSectorLighting().pulsingSectorCount,
		nri_diag::PtDebugSectorAmbient,
		NRIGetSectorLightMultiplier(),
		(float)nri_ptsectorambientscale,
		(float)nri_ptsectorhemiscale,
		(float)nri_ptsectorfogscale,
		(float)nri_ptsectorclamp,
		(float)nri_ptsectoremissionsignalstrength,
		(float)nri_ptsectoremissionresponsemin,
		(float)nri_ptsectoremissionresponsemax,
		(float)nri_ptsectoremissionlightmin,
		(float)nri_ptsectoremissionlightmax,
		(float)nri_ptsectoremissionreachmin,
		(float)nri_ptsectoremissionreachmax,
		(int)nri_ptsectorfilterpal,
		(int)nri_ptsectorfilterminshade,
		(int)nri_ptsectorfiltermaxshade,
		(int)nri_ptsectorfilterlotag,
		(int)nri_ptsectorpulseframes,
		(float)nri_ptsectorpulseamount);
	Printf("NRI PT sector buffer: sectors=%u active=%u pulsing=%u dominant_sector=%u dominant_contribution=%.3f\n",
		mBoundSectorLightSectorCount,
		mBoundSectorLightActiveCount,
		mBoundSectorLightPulsingCount,
		mBoundSectorLightDominantSector != UINT32_MAX ? mBoundSectorLightDominantSector : 0u,
		mBoundSectorLightDominantContribution);
	if (nri_ptbootstrap)
	{
		Printf("NRI PT bootstrap mode: %u\n", bootstrapMode);
	}

	if (mHasLoggedStats)
	{
		const auto& stats = mLastStats;
		Printf("NRI PT last scene: walls=%u flats=%u sprites=%u translucent=%u models=%u voxel_proxies=%u unsupported_models=%u voxel_cache=candidates:%u uncacheable:%u hits:%u misses:%u changes:%u split_stable:%u split_live:%u entries:%u surface_hits:%u stores:%u rebuilds:%u transform_rebakes:%u removes:%u not_captured:%u cached_prims:%u mirrors=%u skies=%u portal_views=%u portal_skips=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.modelDrawItems,
			stats.voxelProxyDrawItems,
			stats.unsupportedModelDrawItems,
			stats.voxelStableCandidates,
			stats.voxelStableUncacheable,
			stats.voxelStableSignatureHits,
			stats.voxelStableSignatureMisses,
			stats.voxelStableSignatureChanges,
			stats.voxelStableSplitStable,
			stats.voxelStableSplitLive,
			stats.voxelCacheEntries,
			stats.voxelCacheSurfaceHits,
			stats.voxelCacheSurfaceStores,
			stats.voxelCacheSurfaceRebuilds,
			stats.voxelCacheTransformRebakes,
			stats.voxelCacheSurfaceRemoves,
			stats.voxelCacheNotCaptured,
			stats.voxelCachePrimitives,
			stats.mirrorSurfaces,
			stats.skySurfaces,
			stats.portalViews,
			stats.portalCapturesSkipped,
			stats.triangleEstimate,
			stats.materialRefs);
	}
	else
	{
		Printf("NRI PT last scene: no translated PT scene has been captured yet.\n");
	}

	PrintMapWorldStatus();
	PrintPortalTraversalStatus();
	PrintStaticMapSceneStatus();
	PrintResidentMapChunkRegistryStatus();
	PrintDynamicSceneStatus();
	PrintTemporalStatus();
	mRuntimeMutation.PrintStatus();
	PrintRuntimeSpaceLinkStatus();
	PrintSceneBufferStatus();
	PrintSurfaceProbeStatus();
}

void NRIRenderer::TraceActorSpriteMaterialAssignments(const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& outMaterials, const char* traceLabel)
{
	NRIActorSpriteMaterialTraceSnapshot snapshot = {};
	snapshot.frameIndex = mFrameIndex;
	snapshot.label = traceLabel != nullptr ? traceLabel : "unlabeled";
	snapshot.materialCount = (uint32_t)outMaterials.materials.size();
	snapshot.textureCount = (uint32_t)outMaterials.textures.size();
	snapshot.bridgeHash = HashMaterialBridgeSummary(outMaterials);
	snapshot.queuedFrameIndex = mFrameBuffer != nullptr ? mFrameBuffer->mCurrentQueuedFrameIndex : 0u;
	snapshot.outstandingQueuedFrames = CountPotentialOutstandingQueuedFrames();

	const uint32_t spriteMaterialBase = (uint32_t)(sceneView.opaqueWalls.size() + sceneView.opaqueFlats.size());
	snapshot.actorHash = 1469598103934665603ull;
	std::unordered_set<int32_t> actorIndices;
	actorIndices.reserve(sceneView.opaqueSprites.size());
	const bool emitVerboseRows = nri_actor_sprite_diag::ShouldTraceVerbose((int)nri_ptactorspritetrace, (int)nri_pttraceframes);

	for (uint32_t spriteIndex = 0; spriteIndex < (uint32_t)sceneView.opaqueSprites.size(); ++spriteIndex)
	{
		const auto& surface = sceneView.opaqueSprites[spriteIndex];
		if (surface.provenance.actorIndex < 0)
		{
			continue;
		}

		const uint32_t materialIndex = spriteMaterialBase + spriteIndex;
		if (materialIndex >= outMaterials.materials.size() || materialIndex >= outMaterials.lightMetadata.size())
		{
			continue;
		}

		const auto& material = outMaterials.materials[materialIndex];
		const auto& metadata = outMaterials.lightMetadata[materialIndex];
		snapshot.actorSurfaceCount++;
		actorIndices.insert(surface.provenance.actorIndex);
		snapshot.actorHash = nri_scene::HashCombine64(snapshot.actorHash, (uint64_t)surface.provenance.actorIndex);
		snapshot.actorHash = nri_scene::HashCombine64(snapshot.actorHash, (uint64_t)(uint32_t)surface.provenance.sourceType);
		snapshot.actorHash = nri_scene::HashCombine64(snapshot.actorHash, (uint64_t)materialIndex);
		snapshot.actorHash = nri_scene::HashCombine64(snapshot.actorHash, (uint64_t)metadata.textureId);
		snapshot.actorHash = nri_scene::HashCombine64(snapshot.actorHash, (uint64_t)material.textureIndex);
		snapshot.actorHash = nri_scene::HashCombine64(snapshot.actorHash, (uint64_t)material.paletteIndex);
		snapshot.actorHash = nri_scene::HashCombine64(snapshot.actorHash, (uint64_t)material.emissiveMode);
		snapshot.actorHash = nri_scene::HashCombine64(snapshot.actorHash, (uint64_t)material.emissiveTextureIndex);
		snapshot.actorHash = nri_scene::HashCombine64(snapshot.actorHash, metadata.materialKey);

		if (emitVerboseRows && snapshot.rows.size() < 32u)
		{
			NRIActorSpriteMaterialTraceRow row = {};
			row.frameIndex = snapshot.frameIndex;
			row.label = snapshot.label;
			row.actorIndex = surface.provenance.actorIndex;
			row.sourceName = nri_diag::GetSurfaceSourceTypeName(surface.provenance.sourceType);
			row.materialIndex = materialIndex;
			row.textureId = metadata.textureId;
			row.textureIndex = material.textureIndex;
			row.emissiveMode = material.emissiveMode;
			row.emissiveTextureIndex = material.emissiveTextureIndex;
			row.paletteIndex = material.paletteIndex;
			row.materialFlags = material.flags;
			row.lightingFlags = material.lightingFlags;
			row.materialKey = metadata.materialKey;
			row.texture = metadata.texture;
			snapshot.rows.push_back(row);
		}
	}

	snapshot.actorCount = (uint32_t)actorIndices.size();
	snapshot.emitSummary = snapshot.actorSurfaceCount != 0 && nri_actor_sprite_diag::ShouldTraceCoherency((int)nri_ptactorspritetrace, (int)nri_pttraceframes);

	mDescriptorCoherencyDebugStats.actorMaterialBuilds++;
	mDescriptorCoherencyDebugStats.lastMaterialBuildLabel = snapshot.label;
	mDescriptorCoherencyDebugStats.lastMaterialCount = snapshot.materialCount;
	mDescriptorCoherencyDebugStats.lastTextureCount = snapshot.textureCount;
	mDescriptorCoherencyDebugStats.lastMaterialBridgeHash = snapshot.bridgeHash;
	mDescriptorCoherencyDebugStats.lastActorSpriteSurfaceCount = snapshot.actorSurfaceCount;
	mDescriptorCoherencyDebugStats.lastActorSpriteActorCount = snapshot.actorCount;
	mDescriptorCoherencyDebugStats.lastActorSpriteMaterialHash = snapshot.actorHash;

	PrintNRIActorSpriteMaterialTraceSnapshot(snapshot);
}

void NRIRenderer::TraceActorSpriteEvent(const PathTracingActorSpriteTraceEvent& event)
{
	if (!nri_actor_sprite_diag::ShouldTraceVerbose((int)nri_ptactorspritetrace, (int)nri_pttraceframes))
	{
		return;
	}

	if (event.hasVoxelKeys)
	{
		Printf("NRI PT actor-sprite %s: actor=%d stat=%d pic=%d base_tex=%d resolved_tex=%d pal=%d shade=%d cstat=0x%x cstat2=0x%x noanimate=%s fullbright=%s drawlist=%u tex_ptr=%p voxel_action=%s voxel_mesh_key=0x%llx voxel_mat_key=0x%llx voxel_inst_key=0x%llx voxel_surface_sig=0x%llx\n",
			nri_actor_sprite_diag::GetTraceStageName(event.stage),
			event.actorIndex,
			event.spriteStatnum,
			event.spritePicnum,
			event.baseTextureId,
			event.resolvedTextureId,
			event.palette,
			event.shade,
			event.cstat,
			event.cstat2,
			event.noAnimate ? "yes" : "no",
			event.fullbright ? "yes" : "no",
			event.drawListType,
			event.resolvedGameTexture,
			event.voxelAction != nullptr ? event.voxelAction : "unknown",
			(unsigned long long)event.voxelMeshKeyHash,
			(unsigned long long)event.voxelMaterialKeyHash,
			(unsigned long long)event.voxelInstanceKeyHash,
			(unsigned long long)event.voxelSurfaceSignature);
		return;
	}

	Printf("NRI PT actor-sprite %s: actor=%d stat=%d pic=%d base_tex=%d resolved_tex=%d pal=%d shade=%d cstat=0x%x cstat2=0x%x noanimate=%s fullbright=%s drawlist=%u tex_ptr=%p\n",
		nri_actor_sprite_diag::GetTraceStageName(event.stage),
		event.actorIndex,
		event.spriteStatnum,
		event.spritePicnum,
		event.baseTextureId,
		event.resolvedTextureId,
		event.palette,
		event.shade,
		event.cstat,
		event.cstat2,
		event.noAnimate ? "yes" : "no",
		event.fullbright ? "yes" : "no",
		event.drawListType,
		event.resolvedGameTexture);
}

NRISurfaceProbeStatusSnapshot NRIRenderer::BuildSurfaceProbeStatusSnapshot() const
{
	NRISurfaceProbeStatusSnapshot snapshot = {};
	if (!mSurfaceProbe.Last().valid)
	{
		return snapshot;
	}

	snapshot.recorded = true;
	if (!mSurfaceProbe.Last().hit)
	{
		return snapshot;
	}

	snapshot.hit = true;
	const NRISurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(mSurfaceProbe.Last());
	const uint32_t flags = mSurfaceProbe.Last().primitiveFlags;
	const uint32_t lightingFlags = mSurfaceProbe.Last().materialLightingFlags;
	const int32_t localSpaceIndex = mSurfaceProbe.Last().provenance.mapChunkIndex >= 0 ? nri_scene::FindMapWorldLocalSpaceIndex(mMapWorld, (uint32_t)mSurfaceProbe.Last().provenance.mapChunkIndex) : -1;
	const int32_t portalGraphIndex = nri_scene::FindMapWorldPortalIndex(mMapWorld, mSurfaceProbe.Last().provenance);
	bool chunkResidentStatic = false;
	bool chunkStaticTlasInstanced = false;
	bool chunkStaticProbeIncluded = false;
	bool chunkVisibleGate = false;
	bool flatPlaneVisibilityRelevant = false;
	bool flatPlaneVisible = false;
	NRIStaticSceneResidency::ChunkDiagnosticFacts staticChunkFacts = {};
	NRIRuntimeMutationSystem::ChunkDiagnosticFacts replacementFacts = {};
	if (mSurfaceProbe.Last().provenance.mapChunkIndex >= 0)
	{
		const uint32_t chunkIndex = (uint32_t)mSurfaceProbe.Last().provenance.mapChunkIndex;
		chunkVisibleGate = IsChunkMarkedVisible(mCurrentVisibleChunkWords, chunkIndex);
		staticChunkFacts = NRIStaticSceneResidency::BuildChunkDiagnosticFacts(mStaticMapScene, mStaticMapChunkAtlas, chunkIndex);
		chunkResidentStatic = staticChunkFacts.residentStatic;
		chunkStaticTlasInstanced = staticChunkFacts.staticTlasInstanced;
		chunkStaticProbeIncluded = staticChunkFacts.staticProbeIncluded;
		replacementFacts = mRuntimeMutation.BuildChunkDiagnosticFacts(chunkIndex);
	}
	if ((flags & nri_scene::MaterialFlag_Flat) != 0 &&
		(flags & (nri_scene::MaterialFlag_Sprite | nri_scene::MaterialFlag_Mirror | nri_scene::MaterialFlag_Sky | nri_scene::MaterialFlag_Portal)) == 0 &&
		mSurfaceProbe.Last().provenance.sectorIndex >= 0)
	{
		flatPlaneVisibilityRelevant = true;
		flatPlaneVisible = IsFlatPlaneMarkedVisible(mCurrentVisibleFlatPlaneWords, mSurfaceProbe.Last().provenance.sectorIndex, mSurfaceProbe.Last().normal[1] < 0.0f);
	}
	snapshot.sourceName = nri_diag::GetSurfaceSourceTypeName(mSurfaceProbe.Last().provenance.sourceType);
	snapshot.drawListName = nri_diag::GetDrawListTypeName(mSurfaceProbe.Last().provenance.drawListType);
	snapshot.ownerName = nri_diag::GetSurfaceProbeSceneOwnerName(mSurfaceProbe.Last().sceneOwner);
	snapshot.dataSourceName = nri_diag::GetSceneDataSourceName(mSurfaceProbe.Last().sceneDataSource);
	snapshot.chunkIndex = mSurfaceProbe.Last().provenance.mapChunkIndex;
	snapshot.gateVisible = YesNo(chunkVisibleGate);
	snapshot.flatDrawlistVisible = flatPlaneVisibilityRelevant ? YesNo(flatPlaneVisible) : "n/a";
	snapshot.staticResident = YesNo(chunkResidentStatic);
	snapshot.staticTlasInstanced = YesNo(chunkStaticTlasInstanced);
	snapshot.staticProbeIncluded = YesNo(chunkStaticProbeIncluded);
	snapshot.chunkReplaced = YesNo(replacementFacts.active);
	snapshot.chunkReasons = replacementFacts.reasonSummary;
	snapshot.sectionDirtyCount = replacementFacts.sectionDirtyCount;
	snapshot.sectorDirty = YesNo(replacementFacts.sectorDirty);
	snapshot.dragged = YesNo(replacementFacts.dragged);
	snapshot.blindSpot = YesNo(replacementFacts.blindSpot);
	snapshot.replacementSurfaceCount = replacementFacts.surfaceCount;
	snapshot.replacementTriangleCount = replacementFacts.triangleCount;
	snapshot.localSpaceIndex = localSpaceIndex;
	snapshot.portalGraphIndex = portalGraphIndex;
	snapshot.sectorIndex = mSurfaceProbe.Last().provenance.sectorIndex;
	snapshot.wallIndex = mSurfaceProbe.Last().provenance.wallIndex;
	snapshot.nextSectorIndex = mSurfaceProbe.Last().provenance.nextSectorIndex;
	snapshot.actorIndex = mSurfaceProbe.Last().provenance.actorIndex;
	snapshot.cstat = mSurfaceProbe.Last().provenance.cstat;
	snapshot.primitiveIndex = mSurfaceProbe.Last().primitiveIndex;
	snapshot.materialIndex = mSurfaceProbe.Last().materialIndex;
	snapshot.textureId = mSurfaceProbe.Last().textureId;
	snapshot.baseTextureId = mSurfaceProbe.Last().baseTextureId;
	snapshot.distance = mSurfaceProbe.Last().distance;
	snapshot.position[0] = mSurfaceProbe.Last().position[0];
	snapshot.position[1] = mSurfaceProbe.Last().position[1];
	snapshot.position[2] = mSurfaceProbe.Last().position[2];
	snapshot.primitiveFlags = flags;
	snapshot.indexed = YesNo((flags & nri_scene::MaterialFlag_Indexed) != 0);
	snapshot.fullbright = YesNo((flags & nri_scene::MaterialFlag_Fullbright) != 0);
	snapshot.flat = YesNo((flags & nri_scene::MaterialFlag_Flat) != 0);
	snapshot.sprite = YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0);
	snapshot.mirror = YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0);
	snapshot.sky = YesNo((flags & nri_scene::MaterialFlag_Sky) != 0);
	snapshot.portal = YesNo((flags & nri_scene::MaterialFlag_Portal) != 0);
	snapshot.facingBillboard = YesNo((flags & nri_scene::MaterialFlag_FacingBillboard) != 0);
	snapshot.pointSampled = YesNo((flags & nri_scene::MaterialFlag_PointSampled) != 0);
	snapshot.textureFullbright = YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureFullbright) != 0);
	snapshot.textureGlowing = YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureGlowing) != 0);
	snapshot.textureAutoGlow = YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureAutoGlowing) != 0);
	snapshot.hasGlowmap = YesNo((lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0);
	snapshot.hasNormalMap = YesNo(mSurfaceProbe.Last().normalTextureIndex != UINT32_MAX);
	snapshot.hasMetallicMap = YesNo(mSurfaceProbe.Last().metallicTextureIndex != UINT32_MAX);
	snapshot.hasRoughnessMap = YesNo(mSurfaceProbe.Last().roughnessTextureIndex != UINT32_MAX);
	snapshot.normalTextureIndex = mSurfaceProbe.Last().normalTextureIndex != UINT32_MAX ? mSurfaceProbe.Last().normalTextureIndex : 0u;
	snapshot.metallicTextureIndex = mSurfaceProbe.Last().metallicTextureIndex != UINT32_MAX ? mSurfaceProbe.Last().metallicTextureIndex : 0u;
	snapshot.roughnessTextureIndex = mSurfaceProbe.Last().roughnessTextureIndex != UINT32_MAX ? mSurfaceProbe.Last().roughnessTextureIndex : 0u;
	snapshot.metalnessHint = mSurfaceProbe.Last().metalnessHint;
	snapshot.roughnessHint = mSurfaceProbe.Last().roughnessHint;
	snapshot.materialClass = mSurfaceProbe.Last().materialClass;
	snapshot.emissiveModeName = nri_diag::GetMaterialEmissiveModeName(mSurfaceProbe.Last().emissiveMode);
	snapshot.emissiveTextureIndex = mSurfaceProbe.Last().emissiveTextureIndex != UINT32_MAX ? mSurfaceProbe.Last().emissiveTextureIndex : 0u;
	snapshot.lightSurface = YesNo(emissiveDiagnostics.sceneLightSurfaceMatch);
	snapshot.lightMaterialIndex = emissiveDiagnostics.sceneLightMaterialIndex != UINT32_MAX ? emissiveDiagnostics.sceneLightMaterialIndex : 0u;
	snapshot.emissiveSurface = YesNo(emissiveDiagnostics.activeEmissiveSurfaceMatch);
	snapshot.emissivePrimitiveMatchCount = emissiveDiagnostics.emissivePrimitiveMatchCount;
	snapshot.emissiveHit = YesNo(emissiveDiagnostics.exactEmissivePrimitiveMatch);
	snapshot.emissiveSourceFlags = emissiveDiagnostics.emissiveSourceFlags;
	snapshot.emissiveSourceRuleId = emissiveDiagnostics.emissiveSourceRuleId;
	snapshot.emissiveOverrideRuleId = emissiveDiagnostics.emissiveOverrideRuleId;
	snapshot.emissiveSectorIndex = emissiveDiagnostics.emissiveSectorIndex;
	snapshot.sectorResponseScale = emissiveDiagnostics.sectorResponseScale;
	snapshot.sectorReachScale = emissiveDiagnostics.sectorReachScale;
	snapshot.sectorResponseApplied = YesNo(emissiveDiagnostics.sectorResponseApplied);
	snapshot.emissivePrimitiveArea = emissiveDiagnostics.emissivePrimitiveArea;
	snapshot.emissivePowerEstimate = emissiveDiagnostics.emissivePowerEstimate;
	snapshot.emissiveSelectionWeight = emissiveDiagnostics.emissiveSelectionWeight;
	snapshot.emissiveSelectionPdf = emissiveDiagnostics.emissiveSelectionPdf;
	snapshot.emissiveIntensity = emissiveDiagnostics.emissiveIntensity;
	snapshot.materialResponse = YesNo(emissiveDiagnostics.materialResponseEnabled);
	snapshot.materialResponseScale = emissiveDiagnostics.materialResponseScale;
	snapshot.lightLevel = mSurfaceProbe.Last().lightLevel;
	snapshot.alpha = mSurfaceProbe.Last().alpha;
	for (int i = 0; i < 3; ++i)
	{
		snapshot.averageColor[i] = mSurfaceProbe.Last().averageColor[i];
		snapshot.emissiveColor[i] = mSurfaceProbe.Last().emissiveColor[i];
		snapshot.glowColor[i] = mSurfaceProbe.Last().glowColor[i];
	}
	return snapshot;
}

void PrintNRISurfaceProbeStatusSnapshot(const NRISurfaceProbeStatusSnapshot& snapshot)
{
	if (!snapshot.recorded)
	{
		Printf("NRI PT surface probe: no sampled center hit has been recorded yet.\n");
		return;
	}

	if (!snapshot.hit)
	{
		Printf("NRI PT surface probe: last sampled center ray missed translated PT geometry.\n");
		return;
	}

	Printf("NRI PT surface probe: source=%s drawlist=%s owner=%s data_source=%s chunk=%d gate_visible=%s flat_drawlist_visible=%s static_resident=%s static_tlas_instanced=%s static_probe_included=%s chunk_replaced=%s chunk_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u tile=%u material_tile=%u distance=%.2f pos=(%.2f, %.2f, %.2f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s facing_billboard=%s point_sampled=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s normalmap=%s metallic=%s roughness=%s normal_tex=%u metallic_tex=%u roughness_tex=%u metalness_hint=%.3f roughness_hint=%.3f material_class=%u emissive_mode=%s emissive_tex=%u light_surface=%s light_mat=%u emissive_surface=%s emissive_prims=%u emissive_hit=%s emissive_flags=0x%x emissive_rule=%u emissive_override=%u emissive_sector=%d sector_scale=%.3f sector_reach=%.3f sector_applied=%s emissive_area=%.2f emissive_power=%.3f emissive_sample_weight=%.3f emissive_pdf=%.6f emissive_intensity=%.3f material_response=%s material_scale=%.3f light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
		snapshot.sourceName,
		snapshot.drawListName,
		snapshot.ownerName,
		snapshot.dataSourceName,
		snapshot.chunkIndex,
		snapshot.gateVisible,
		snapshot.flatDrawlistVisible,
		snapshot.staticResident,
		snapshot.staticTlasInstanced,
		snapshot.staticProbeIncluded,
		snapshot.chunkReplaced,
		snapshot.chunkReasons.c_str(),
		snapshot.sectionDirtyCount,
		snapshot.sectorDirty,
		snapshot.dragged,
		snapshot.blindSpot,
		snapshot.replacementSurfaceCount,
		snapshot.replacementTriangleCount,
		snapshot.localSpaceIndex,
		snapshot.portalGraphIndex,
		snapshot.sectorIndex,
		snapshot.wallIndex,
		snapshot.nextSectorIndex,
		snapshot.actorIndex,
		snapshot.cstat,
		snapshot.primitiveIndex,
		snapshot.materialIndex,
		snapshot.textureId,
		snapshot.baseTextureId,
		snapshot.distance,
		snapshot.position[0],
		snapshot.position[1],
		snapshot.position[2],
		snapshot.primitiveFlags,
		snapshot.indexed,
		snapshot.fullbright,
		snapshot.flat,
		snapshot.sprite,
		snapshot.mirror,
		snapshot.sky,
		snapshot.portal,
		snapshot.facingBillboard,
		snapshot.pointSampled,
		snapshot.textureFullbright,
		snapshot.textureGlowing,
		snapshot.textureAutoGlow,
		snapshot.hasGlowmap,
		snapshot.hasNormalMap,
		snapshot.hasMetallicMap,
		snapshot.hasRoughnessMap,
		snapshot.normalTextureIndex,
		snapshot.metallicTextureIndex,
		snapshot.roughnessTextureIndex,
		snapshot.metalnessHint,
		snapshot.roughnessHint,
		snapshot.materialClass,
		snapshot.emissiveModeName,
		snapshot.emissiveTextureIndex,
		snapshot.lightSurface,
		snapshot.lightMaterialIndex,
		snapshot.emissiveSurface,
		snapshot.emissivePrimitiveMatchCount,
		snapshot.emissiveHit,
		snapshot.emissiveSourceFlags,
		snapshot.emissiveSourceRuleId,
		snapshot.emissiveOverrideRuleId,
		snapshot.emissiveSectorIndex,
		snapshot.sectorResponseScale,
		snapshot.sectorReachScale,
		snapshot.sectorResponseApplied,
		snapshot.emissivePrimitiveArea,
		snapshot.emissivePowerEstimate,
		snapshot.emissiveSelectionWeight,
		snapshot.emissiveSelectionPdf,
		snapshot.emissiveIntensity,
		snapshot.materialResponse,
		snapshot.materialResponseScale,
		snapshot.lightLevel,
		snapshot.alpha,
		snapshot.averageColor[0],
		snapshot.averageColor[1],
		snapshot.averageColor[2],
		snapshot.emissiveColor[0],
		snapshot.emissiveColor[1],
		snapshot.emissiveColor[2],
		snapshot.glowColor[0],
		snapshot.glowColor[1],
		snapshot.glowColor[2]);
}

void NRIRenderer::PrintSurfaceProbeStatus() const
{
	PrintNRISurfaceProbeStatusSnapshot(BuildSurfaceProbeStatusSnapshot());
}

void PrintNRIActorSpriteMaterialTraceSnapshot(const NRIActorSpriteMaterialTraceSnapshot& snapshot)
{
	for (const auto& row : snapshot.rows)
	{
		Printf("NRI PT actor-sprite material: frame=%u label=%s actor=%d source=%s material=%u tex_id=%u tex_index=%u emissive_mode=%u emissive_tex=%u palette=%u flags=0x%x light_flags=0x%x material_key=0x%llx tex_ptr=%p\n",
			row.frameIndex,
			row.label.c_str(),
			row.actorIndex,
			row.sourceName,
			row.materialIndex,
			row.textureId,
			row.textureIndex,
			row.emissiveMode,
			row.emissiveTextureIndex,
			row.paletteIndex,
			row.materialFlags,
			row.lightingFlags,
			(unsigned long long)row.materialKey,
			row.texture);
	}

	if (!snapshot.emitSummary)
	{
		return;
	}

	Printf("NRI PT actor-sprite materials: frame=%u label=%s materials=%u textures=%u actor_surfaces=%u actor_count=%u bridge_hash=0x%llx actor_hash=0x%llx qframe=%u outstanding_slots=%u\n",
		snapshot.frameIndex,
		snapshot.label.c_str(),
		snapshot.materialCount,
		snapshot.textureCount,
		snapshot.actorSurfaceCount,
		snapshot.actorCount,
		(unsigned long long)snapshot.bridgeHash,
		(unsigned long long)snapshot.actorHash,
		snapshot.queuedFrameIndex,
		snapshot.outstandingQueuedFrames);
}

void NRIRenderer::PrintSceneBufferStatus() const
{
	NRISceneBufferStatusSnapshot snapshot = {};
	const auto appendBuffer = [&snapshot](const NRIBufferResource& resource, const SceneBufferDebugStats& stats)
	{
		snapshot.buffers.push_back(BuildNRIBufferStatusSnapshot(resource, stats));
	};

	const NRIBufferResource& activeVertexBuffer = GetActiveVertexBuffer();
	const NRIBufferResource& activeIndexBuffer = GetActiveIndexBuffer();
	const NRIBufferResource& activePrimitiveBuffer = GetActivePrimitiveBuffer();
	const NRIBufferResource& activeMaterialBuffer = GetActiveMaterialBuffer();
	snapshot.totalUsedBytes = activeVertexBuffer.usedSize + activeIndexBuffer.usedSize + activePrimitiveBuffer.usedSize + activeMaterialBuffer.usedSize;
	snapshot.totalCapacityBytes = activeVertexBuffer.size + activeIndexBuffer.size + activePrimitiveBuffer.size + activeMaterialBuffer.size;
	snapshot.lastFrameUploadBytes =
		mVertexBufferStats.bytesUploadedLastFrame +
		mIndexBufferStats.bytesUploadedLastFrame +
		mPrimitiveBufferStats.bytesUploadedLastFrame +
		mMaterialBufferStats.bytesUploadedLastFrame;
	snapshot.lastFrameGrowEvents =
		mVertexBufferStats.growEventsLastFrame +
		mIndexBufferStats.growEventsLastFrame +
		mPrimitiveBufferStats.growEventsLastFrame +
		mMaterialBufferStats.growEventsLastFrame;
	snapshot.lastFrameOverwriteEvents =
		mVertexBufferStats.overwriteEventsLastFrame +
		mIndexBufferStats.overwriteEventsLastFrame +
		mPrimitiveBufferStats.overwriteEventsLastFrame +
		mMaterialBufferStats.overwriteEventsLastFrame;

	appendBuffer(activeVertexBuffer, mVertexBufferStats);
	appendBuffer(activeIndexBuffer, mIndexBufferStats);
	appendBuffer(activePrimitiveBuffer, mPrimitiveBufferStats);
	appendBuffer(activeMaterialBuffer, mMaterialBufferStats);
	appendBuffer(mPortalBuffer, mPortalBufferStats);
	appendBuffer(mRuntimeLightBuffer, mRuntimeLightBufferStats);
	appendBuffer(mRuntimeLightTileHeaderBuffer, mRuntimeLightTileHeaderBufferStats);
	appendBuffer(mRuntimeLightTileIndexBuffer, mRuntimeLightTileIndexBufferStats);
	appendBuffer(mEmissivePrimitiveHeaderBuffer, mEmissivePrimitiveHeaderBufferStats);
	appendBuffer(mEmissivePrimitiveBuffer, mEmissivePrimitiveBufferStats);
	appendBuffer(mEmissivePrimitiveCdfBuffer, mEmissivePrimitiveCdfBufferStats);
	appendBuffer(mEmissiveMaterialResponseBuffer, mEmissiveMaterialResponseBufferStats);
	appendBuffer(mSectorLightHeaderBuffer, mSectorLightHeaderBufferStats);
	appendBuffer(mSectorLightBuffer, mSectorLightBufferStats);
	PrintNRISceneBufferStatusSnapshot(snapshot);
}

void NRIRenderer::LogFallback(const char* reason)
{
	// Keep the normal log quiet, but let explicit debug sessions expose the
	// current failure instead of permanently hiding it behind an earlier,
	// unrelated preload fallback.
	if (mHasLoggedFallback && nri_ptdebug <= 0)
	{
		return;
	}

	Printf(TEXTCOLOR_ORANGE "NRI PT fallback: %s\n", reason != nullptr ? reason : "unknown reason");
	mHasLoggedFallback = true;
}

void NRIRenderer::LogBridgeStats(const nri_scene::SceneDebugStats& stats)
{
	if (!nri_ptscenestats)
	{
		mLastStats = stats;
		mHasLoggedStats = true;
		return;
	}

	if (!mHasLoggedStats || nri_scene::SceneDebugStatsDiffer(mLastStats, stats))
	{
		Printf("NRI PT scene: walls=%u flats=%u sprites=%u translucent=%u models=%u voxel_proxies=%u unsupported_models=%u voxel_cache=candidates:%u uncacheable:%u hits:%u misses:%u changes:%u split_stable:%u split_live:%u entries:%u surface_hits:%u stores:%u rebuilds:%u transform_rebakes:%u removes:%u not_captured:%u deferred:%u cached_prims:%u mirrors=%u skies=%u portal_views=%u portal_skips=%u approx_tris=%u materials=%u\n",
			stats.wallDrawItems,
			stats.flatDrawItems,
			stats.spriteDrawItems,
			stats.translucentDrawItems,
			stats.modelDrawItems,
			stats.voxelProxyDrawItems,
			stats.unsupportedModelDrawItems,
			stats.voxelStableCandidates,
			stats.voxelStableUncacheable,
			stats.voxelStableSignatureHits,
			stats.voxelStableSignatureMisses,
			stats.voxelStableSignatureChanges,
			stats.voxelStableSplitStable,
			stats.voxelStableSplitLive,
			stats.voxelCacheEntries,
			stats.voxelCacheSurfaceHits,
			stats.voxelCacheSurfaceStores,
			stats.voxelCacheSurfaceRebuilds,
			stats.voxelCacheTransformRebakes,
			stats.voxelCacheSurfaceRemoves,
			stats.voxelCacheNotCaptured,
			stats.voxelCacheDeferred,
			stats.voxelCachePrimitives,
			stats.mirrorSurfaces,
			stats.skySurfaces,
			stats.portalViews,
			stats.portalCapturesSkipped,
			stats.triangleEstimate,
			stats.materialRefs);
		mLastStats = stats;
		mHasLoggedStats = true;
	}
}

void NRIRenderer::PrintEmissiveSurfaceDump(float radius, uint32_t limit) const
{
	mSceneLights.PrintEmissiveSurfaceDump(mBoundEmissivePrimitiveRecords, mBoundEmissiveTotalPower, mCurrentCameraPos, radius, limit);
}

void NRIRenderer::PrintSceneLightDump(float radius, uint32_t limit) const
{
	mSceneLights.PrintSceneLightDump(mCurrentCameraPos, mMapWorld, mFrameIndex, radius, limit);
}

void NRIRenderer::PrintRuntimeSpaceLinkStatus() const
{
	Printf("NRI PT runtime links: active=%s geo_effect=%s query_attempted=%s query_rejected=%s candidate_sector=%d candidate_lotag=%d source_sector=%d reported_geo_count=%d view_roots=%u visible_sectors=%u providers=%u geo_providers=%u provider_groups=%u local_space_matches=%u visible_matches=%u links=%u translated_chunks=%u orphan_local_spaces=%u unresolved_runtime_portals=%u surfaces=%u tris=%u materials=%u\n",
		mRuntimeSpaceLinkLastFrame.active ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.geoEffectActive ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.queryAttempted ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.queryRejected ? "yes" : "no",
		mRuntimeSpaceLinkLastFrame.candidateSectorIndex,
		mRuntimeSpaceLinkLastFrame.candidateSectorLotag,
		mRuntimeSpaceLinkLastFrame.sourceSectorIndex,
		mRuntimeSpaceLinkLastFrame.reportedGeoCount,
		mRuntimeSpaceLinkLastFrame.viewRootSectorCount,
		mRuntimeSpaceLinkLastFrame.visibleSectorCount,
		mRuntimeSpaceLinkLastFrame.providerSectorCount,
		mRuntimeSpaceLinkLastFrame.geoProviderCount,
		mRuntimeSpaceLinkLastFrame.providerGroupCount,
		mRuntimeSpaceLinkLastFrame.localSpaceMatchedProviderCount,
		mRuntimeSpaceLinkLastFrame.visibleMatchedProviderCount,
		mRuntimeSpaceLinkLastFrame.linkCount,
		mRuntimeSpaceLinkLastFrame.translatedChunkCount,
		mRuntimeSpaceLinkLastFrame.orphanLocalSpaceCount,
		mRuntimeSpaceLinkLastFrame.unresolvedRuntimePortalCount,
		mRuntimeSpaceLinkLastFrame.surfaceCount,
		mRuntimeSpaceLinkLastFrame.triangleCount,
		mRuntimeSpaceLinkLastFrame.materialCount);
	Printf("NRI PT runtime link motion: prev_chunk_offsets=%u topology_changed=%s special_material_history=%s\n",
		(uint32_t)mRuntimeChunkTranslationHistory.size(),
		mRuntimeSpaceLinkLastFrame.topologyChanged ? "yes" : "no",
		"portal_mirror_raw_fallback");
}

void NRIRenderer::PrintMapWorldStatus() const
{
	if (!mMapWorld.valid)
	{
		Printf("NRI PT map world: no authoritative map world has been built yet.\n");
		return;
	}

	const auto& stats = mMapWorld.stats;
	Printf("NRI PT map world: level=%s build_serial=%llu chunks=%u local_spaces=%u sectors=%u sections=%u surfaces=%u walls=%u flats=%u portal_surfaces=%u portal_graph=%u portal_targets=%u wall_portals=%u sector_portals=%u mirror_portals=%u runtime_portals=%u skies=%u tris=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		stats.chunkCount,
		stats.localSpaceCount,
		stats.sectorCount,
		stats.sectionCount,
		stats.surfaceCount,
		stats.wallSurfaceCount,
		stats.flatSurfaceCount,
		stats.portalSurfaceCount,
		stats.portalCount,
		stats.portalTargetCount,
		stats.wallPortalCount,
		stats.sectorPortalCount,
		stats.mirrorPortalCount,
		stats.runtimePortalCount,
		stats.skySurfaceCount,
		stats.triangleCount);
}

NRIBufferStatusSnapshot BuildNRIBufferStatusSnapshot(const NRIBufferResource& resource, const SceneBufferDebugStats& stats)
{
	NRIBufferStatusSnapshot snapshot = {};
	snapshot.label = stats.label;
	snapshot.usedBytes = resource.usedSize;
	snapshot.capacityBytes = resource.size;
	snapshot.usedItems = resource.stride != 0 ? resource.usedSize / resource.stride : 0;
	snapshot.capacityItems = resource.stride != 0 ? resource.size / resource.stride : 0;
	snapshot.uploadCount = stats.uploadCount;
	snapshot.growthCount = stats.growthCount;
	snapshot.overwriteCount = stats.overwriteCount;
	snapshot.bytesUploadedLastFrame = stats.bytesUploadedLastFrame;
	snapshot.growEventsLastFrame = stats.growEventsLastFrame;
	snapshot.overwriteEventsLastFrame = stats.overwriteEventsLastFrame;
	snapshot.peakUsedBytes = stats.peakUsedBytes;
	return snapshot;
}

void PrintNRISceneBufferStatusSnapshot(const NRISceneBufferStatusSnapshot& snapshot)
{
	Printf("NRI PT scene buffers: used=%llu capacity=%llu last_frame_upload=%llu last_frame_grows=%u last_frame_overwrites=%u\n",
		(unsigned long long)snapshot.totalUsedBytes,
		(unsigned long long)snapshot.totalCapacityBytes,
		(unsigned long long)snapshot.lastFrameUploadBytes,
		snapshot.lastFrameGrowEvents,
		snapshot.lastFrameOverwriteEvents);

	for (const NRIBufferStatusSnapshot& buffer : snapshot.buffers)
	{
		Printf("NRI PT %s buffer: used=%llu/%llu bytes items=%llu/%llu uploads=%u grows=%u overwrites=%u last_frame_bytes=%llu last_frame_grows=%u last_frame_overwrites=%u peak_used=%llu\n",
			buffer.label,
			(unsigned long long)buffer.usedBytes,
			(unsigned long long)buffer.capacityBytes,
			(unsigned long long)buffer.usedItems,
			(unsigned long long)buffer.capacityItems,
			buffer.uploadCount,
			buffer.growthCount,
			buffer.overwriteCount,
			(unsigned long long)buffer.bytesUploadedLastFrame,
			buffer.growEventsLastFrame,
			buffer.overwriteEventsLastFrame,
			(unsigned long long)buffer.peakUsedBytes);
	}
}

void PrintNRITemporalStatusSnapshot(const NRITemporalStatusSnapshot& snapshot)
{
	Printf("NRI PT temporal: debug=%d requested_main=%s resolved_main=%s requested_post=%s resolved_post=%s taa=%s gui_capture=%s last_debug=%d last_main=%s last_post=%s reset=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] present=%s upscaled=%s use_upscaled=%s\n",
		snapshot.debugMode,
		snapshot.requestedMainUpscaler,
		snapshot.resolvedMainUpscaler,
		snapshot.requestedPostSharpen,
		snapshot.resolvedPostSharpen,
		snapshot.taa ? "on" : "off",
		snapshot.guiCapture ? "yes" : "no",
		snapshot.lastDebugMode,
		snapshot.lastMainUpscaler,
		snapshot.lastPostSharpen,
		snapshot.resetHistory ? "yes" : "no",
		snapshot.previousCamera ? "yes" : "no",
		snapshot.historyInput.slotName,
		snapshot.historyInput.width,
		snapshot.historyInput.height,
		snapshot.historyInput.access,
		snapshot.historyInput.layout,
		snapshot.historyInput.stages,
		snapshot.historyOutput.slotName,
		snapshot.historyOutput.width,
		snapshot.historyOutput.height,
		snapshot.historyOutput.access,
		snapshot.historyOutput.layout,
		snapshot.historyOutput.stages,
		snapshot.presentSlotName,
		snapshot.upscaledSlotName,
		snapshot.useUpscaled ? "yes" : "no");
	Printf("NRI PT beauty path: nrd_and_composition -> pre_exposed_hdr_temporal -> final_display_mapping inspect_scene=15 inspect_pre_exposed=45 inspect_post_taa=13 inspect_post_upscale=14\n");
	Printf("NRI PT temporal domain: history=%s present=%s temporal_exposure=%.3f present_exposure=%.3f exposure_stops=%.3f reset_threshold_stops=%.3f auto_exposure=%s exposure_texture=%s taa_apply=%s\n",
		snapshot.historyDomain,
		snapshot.presentDomain,
		snapshot.temporalExposure,
		snapshot.presentExposure,
		snapshot.exposureStops,
		snapshot.resetThresholdStops,
		snapshot.autoExposure ? "yes" : "no",
		snapshot.exposureTexture ? "yes" : "no",
		snapshot.taaApply ? "yes" : "no");
}

void PrintNRITemporalTraceSnapshot(const NRITemporalTraceSnapshot& snapshot)
{
	Printf("NRI PT temporal trace: stage=%s frame=%u debug=%d resolved_main=%s resolved_post=%s run_app_taa=%s gui_capture=%s primary_domain=%s secondary_domain=%s temporal_exposure=%.3f primary_present_exposure=%.3f secondary_present_exposure=%.3f reset=%s reset_reason=%s prev_camera=%s history_in=%s[%ux%u a=%u l=%u s=0x%x] history_out=%s[%ux%u a=%u l=%u s=0x%x] primary=%s[%ux%u a=%u l=%u s=0x%x] secondary=%s[%ux%u a=%u l=%u s=0x%x] use_upscaled=%s\n",
		snapshot.stage,
		snapshot.frameIndex,
		snapshot.debugMode,
		snapshot.resolvedMainUpscaler,
		snapshot.resolvedPostSharpen,
		snapshot.runAppTaa ? "yes" : "no",
		snapshot.guiCapture ? "yes" : "no",
		snapshot.primaryDomain,
		snapshot.secondaryDomain,
		snapshot.temporalExposure,
		snapshot.primaryPresentExposure,
		snapshot.secondaryPresentExposure,
		snapshot.resetHistory ? "yes" : "no",
		snapshot.resetReason,
		snapshot.previousCamera ? "yes" : "no",
		snapshot.historyInput.slotName,
		snapshot.historyInput.width,
		snapshot.historyInput.height,
		snapshot.historyInput.access,
		snapshot.historyInput.layout,
		snapshot.historyInput.stages,
		snapshot.historyOutput.slotName,
		snapshot.historyOutput.width,
		snapshot.historyOutput.height,
		snapshot.historyOutput.access,
		snapshot.historyOutput.layout,
		snapshot.historyOutput.stages,
		snapshot.primary.slotName,
		snapshot.primary.width,
		snapshot.primary.height,
		snapshot.primary.access,
		snapshot.primary.layout,
		snapshot.primary.stages,
		snapshot.secondary.slotName,
		snapshot.secondary.width,
		snapshot.secondary.height,
		snapshot.secondary.access,
		snapshot.secondary.layout,
		snapshot.secondary.stages,
		snapshot.useUpscaled ? "yes" : "no");
}

void PrintNRIPortalTraversalStatusSnapshot(const NRIPortalTraversalStatusSnapshot& snapshot)
{
	if (!snapshot.available)
	{
		Printf("NRI PT portal traversal: no authoritative portal graph is available.\n");
		return;
	}

	Printf("NRI PT portal traversal: depth=%u reflective=%u transfer=%u runtime_bound=%u hittable_surfaces=%u plane_portals_pending=%u\n",
		snapshot.depth,
		snapshot.reflective,
		snapshot.transfer,
		snapshot.runtimeBound,
		snapshot.hittableSurfaces,
		snapshot.pendingPlanePortals);
}

void PrintNRIResidentMapChunkRegistryStatusSnapshot(const NRIResidentMapChunkRegistryStatusSnapshot& snapshot)
{
	if (!snapshot.available)
	{
		Printf("NRI PT resident chunk registry: unavailable.\n");
		return;
	}

	Printf("NRI PT resident chunk registry: build_serial=%llu chunks=%u active=%u mapped=%u acceleration_resident=%u animated_candidates=%u animated_refresh_suppressed=%u\n",
		(unsigned long long)snapshot.buildSerial,
		snapshot.chunkCount,
		snapshot.activeChunkCount,
		snapshot.mappedChunkCount,
		snapshot.accelerationResidentChunkCount,
		snapshot.animatedCandidateChunkCount,
		snapshot.animatedRefreshSuppressedChunkCount);
	Printf("NRI PT map chunk bounds: chunks=%u valid=%u invalid=%u near_distance=%.1f visible=%u invisible_near=%u invisible_far=%u invisible_unknown=%u sample_chunk=%u center=(%.1f,%.1f,%.1f) radius=%.1f distance=%.1f tier=%s\n",
		snapshot.mapWorldChunkCount,
		snapshot.boundsValidCount,
		snapshot.boundsInvalidCount,
		(double)snapshot.nearDistance,
		snapshot.visibleCount,
		snapshot.invisibleNearCount,
		snapshot.invisibleFarCount,
		snapshot.invisibleUnknownCount,
		snapshot.sampleChunkIndex,
		(double)snapshot.sampleCenter[0],
		(double)snapshot.sampleCenter[1],
		(double)snapshot.sampleCenter[2],
		(double)snapshot.sampleRadius,
		(double)snapshot.sampleDistance,
		snapshot.sampleTier);
}
