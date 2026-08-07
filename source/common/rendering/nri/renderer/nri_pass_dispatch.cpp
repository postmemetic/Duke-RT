#include "nri_pass_dispatch.h"
#include "nri_cvars.h"

#include "nri_descriptor_sets.h"
#include "nri_exposure.h"
#include "nri_frame_graph.h"
#include "nri_renderer_settings.h"
#include "nri_scene_upload.h"
#include "nri_shader_contracts.h"
#include "../system/nri_renderdevice.h"
#include "../system/nri_gpu_timing.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "perf_capture.h"
#include "printf.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>


namespace
{
	static double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	static bool ShouldTracePtPerf()
	{
		return (int)perf_looptraceframes > 0 || (int)nri_pttraceframes > 0;
	}

	static bool ShouldCollectPtPerfTiming()
	{
		return ShouldTracePtPerf() || (bool)nri_ptslowdowntrace || PerfCompactCaptureTimingActive();
	}

	static bool ShouldCollectTraceShaderStats()
	{
		return (bool)nri_pt360absenceprobe || (!!nri_ptshaderstats && ShouldTracePtPerf());
	}

	static NRITraceShaderStatsFenceServices BuildTraceShaderStatsFenceServices(NRIRenderDevice* device)
	{
		NRITraceShaderStatsFenceServices services = {};
		services.user = device;
		services.getRecordingCommandFenceValue = [](void* user) -> uint64_t
		{
			return user != nullptr ? static_cast<NRIRenderDevice*>(user)->GetRecordingCommandFenceValue() : 0;
		};
		services.isCommandFenceValueComplete = [](void* user, uint64_t fenceValue) -> bool
		{
			return user != nullptr && static_cast<NRIRenderDevice*>(user)->IsCommandFenceValueComplete(fenceValue);
		};
		services.isCommandFenceValueAbandoned = [](void* user, uint64_t fenceValue) -> bool
		{
			return user != nullptr && static_cast<NRIRenderDevice*>(user)->IsCommandFenceValueAbandoned(fenceValue);
		};
		return services;
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldCollectPtPerfTiming() ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedPtPerfTimer()
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

	static uint32_t GetEffectivePtDebugMode()
	{
		return (uint32_t)std::max(0, (int)nri_ptdebug);
	}

	static uint32_t GetBootstrapMode()
	{
		return (uint32_t)std::max(0, std::min((int)nri_ptbootstrapmode, 13));
	}

	static NRIPresentRouteInfo ResolvePresentRouteInfo(uint32_t debugMode, bool bootstrap)
	{
		NRIFrameRouteRequest request = {};
		request.debugMode = debugMode;
		request.bootstrap = bootstrap;
		request.bootstrapMode = bootstrap ? GetBootstrapMode() : 0u;
		return ResolveNRIFrameRoute(request);
	}

	static NRINrdDenoiserMode GetSelectedNrdDenoiserMode()
	{
		return (NRINrdDenoiserMode)std::clamp((int)nri_nrddenoiser, 0, 1);
	}

	static uint32_t GetDispatchSize(uint32_t value)
	{
		return (value + 7u) / 8u;
	}

	static uint64_t AppendTraceWorkloadHash(uint64_t hash, uint64_t value)
	{
		return (hash ^ value) * 1099511628211ull;
	}

	static float Clamp01(float value)
	{
		return std::max(0.0f, std::min(value, 1.0f));
	}

	static NRIRenderer::FrameTextureSlot GetDlrrMainInputSlot()
	{
		return nri_ptsmoke && (int)nri_ptsmokedlrrmode == 1 ?
			NRIRenderer::FrameTextureSlot::RrVolumeInput : NRIRenderer::FrameTextureSlot::RrInput;
	}

	static uint32_t PackVoxelNormalBlend8(float value)
	{
		return (uint32_t)std::lround(Clamp01(value) * 255.0f) << NRI_VOXEL_NORMAL_BLEND_SHIFT;
	}

	static float GetBaseAmbient()
	{
		return std::max(0.0f, (float)nri_ptbaseambient);
	}

	static float GetMetalAmbient()
	{
		return std::max(0.0f, (float)nri_ptmetalambient);
	}

	static uint32_t PackAmbientMultiplier12(float value)
	{
		return (uint32_t)std::min(4095.0f, std::max(0.0f, value) * 1024.0f + 0.5f);
	}

	static uint32_t PackPortalDepthAndAmbientMultipliers(uint32_t portalDepth, float baseAmbient, float metalAmbient)
	{
		return
			(portalDepth & 0xffu) |
			(PackAmbientMultiplier12(baseAmbient) << 8u) |
			(PackAmbientMultiplier12(metalAmbient) << 20u);
	}

	static float ClampDirectionalAngularSize(float angularSize)
	{
		return std::clamp(angularSize, 0.001f, 1.2f);
	}

	static uint32_t PackDirectionalLightColor24(const float color[3])
	{
		auto packChannel = [](float value) -> uint32_t
		{
			const float clamped = std::clamp(value, 0.0f, 8.0f);
			return (uint32_t)std::clamp((int)std::lround((double)(clamped * (255.0f / 8.0f))), 0, 255);
		};

		const uint32_t r = packChannel(color[0]);
		const uint32_t g = packChannel(color[1]);
		const uint32_t b = packChannel(color[2]);
		return r | (g << 8u) | (b << 16u);
	}

	static uint32_t PackDirectionalAngularSize16(float angularSize)
	{
		const float normalized = ClampDirectionalAngularSize(angularSize) / 1.2f;
		return (uint32_t)std::clamp((int)std::lround((double)(normalized * 65535.0f)), 0, 65535);
	}

	static uint32_t PackTraceBounceCounts(uint32_t lightBounceCount, uint32_t mirrorBounceCount, const float directionalColor[3])
	{
		return
			(lightBounceCount & 0xfu) |
			((mirrorBounceCount & 0xfu) << 4u) |
			(PackDirectionalLightColor24(directionalColor) << 8u);
	}

	static uint32_t PackTraceAux1(uint32_t denoiserMode, uint32_t emissiveSampleCount, float directionalAngularSize)
	{
		return
			(denoiserMode & 0xffu) |
			((emissiveSampleCount & 0xffu) << 8u) |
			(PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static uint32_t PackDenoiserAux1(uint32_t denoiserMode, float directionalAngularSize)
	{
		return (denoiserMode & 0xffu) | (PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static float GetTemporalExposure(const NRIPTOutputPolicy& outputPolicy)
	{
		return std::max(outputPolicy.exposure, 0.125f);
	}

	static uint32_t PackPresentSceneOrigin(int sceneLeft, int sceneTop)
	{
		return (uint16_t)(int16_t)sceneLeft | ((uint32_t)(uint16_t)(int16_t)sceneTop << 16);
	}

	static uint32_t PackNightVisionControls(float contrast, float saturation)
	{
		const uint32_t contrastBits = (uint32_t)std::lround(std::clamp(contrast, 0.0f, 2.0f) * (65535.0f / 2.0f));
		const uint32_t saturationBits = (uint32_t)std::lround(std::clamp(saturation, 0.0f, 2.0f) * (65535.0f / 2.0f));
		return contrastBits | (saturationBits << 16);
	}

	static uint32_t PackNightVisionModeAndTint(NRIPTNightVisionMode mode, float red, float green, float blue)
	{
		const uint32_t redBits = (uint32_t)std::lround(std::clamp(red, 0.0f, 2.0f) * (255.0f / 2.0f));
		const uint32_t greenBits = (uint32_t)std::lround(std::clamp(green, 0.0f, 2.0f) * (255.0f / 2.0f));
		const uint32_t blueBits = (uint32_t)std::lround(std::clamp(blue, 0.0f, 2.0f) * (255.0f / 2.0f));
		return (uint32_t)mode | (redBits << 8) | (greenBits << 16) | (blueBits << 24);
	}

	static void ApplyOutputPolicyToPresentConstants(const NRIPTOutputPolicy& policy, NRIPresentConstants& constants)
	{
		constants.OutputMode = (uint32_t)policy.resolvedMode;
		constants.TonemapMode = (uint32_t)policy.tonemapMode;
		constants.OutputFlags =
			(policy.displayInfoAvailable ? NRI_PRESENT_OUTPUT_FLAG_DISPLAY_INFO_AVAILABLE : 0u) |
			(policy.displayHdrSupported ? NRI_PRESENT_OUTPUT_FLAG_DISPLAY_HDR_SUPPORTED : 0u) |
			(policy.hdrSwapChainActive ? NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE : 0u) |
			(policy.offscreenHdrTarget ? NRI_PRESENT_OUTPUT_FLAG_OFFSCREEN_HDR_TARGET : 0u);
		constants.Exposure = policy.exposure;
		constants.Contrast = policy.contrast;
		constants.Saturation = policy.saturation;
		constants.Shoulder = policy.shoulder;
		constants.Toe = policy.toe;
		constants.PaperWhiteNits = policy.paperWhiteNits;
		constants.DisplayMaxLuminance = policy.displayMaxLuminance;
		constants.DisplaySdrLuminance = policy.displaySdrLuminance;
	}

	static void ApplyNightVisionStateToPresentConstants(const NRIPTNightVisionState& state, NRIPresentConstants& constants)
	{
		constants.NightVisionPackedModeTint = PackNightVisionModeAndTint(
			state.mode,
			(float)nri_ptnightvisionred,
			(float)nri_ptnightvisiongreen,
			(float)nri_ptnightvisionblue);
		constants.NightVisionStrength = nri_ptnightvision ? state.strength01 : 0.0f;
		constants.NightVisionExposure = (float)nri_ptnightvisionexposure;
		constants.NightVisionPackedControls = PackNightVisionControls(
			(float)nri_ptnightvisioncontrast,
			(float)nri_ptnightvisionsaturation);
	}

	static void Copy3(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 3);
	}

	static void Copy2(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 2);
	}

	static void Normalize3(float v[3])
	{
		const float length = std::max(0.0001f, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}

	static void ApplyDirectionalLightStateToConstants(const NRIDirectionalLightState& state, NRITraceSceneConstants& constants)
	{
		constants.LightDirection[0] = state.direction[0];
		constants.LightDirection[1] = state.direction[1];
		constants.LightDirection[2] = state.direction[2];
		Normalize3(constants.LightDirection);
	}
}

bool NRIPassDispatcher::DispatchBootstrapView(NRIPassDispatchContext& context)
{
	Clocker clock(NriPTBootstrapDispatch);

	if (!context.mResources.UpdateReprojectionBuffer())
	{
		return false;
	}

	const uint32_t bootstrapMode = GetBootstrapMode();
	NRITraceSceneConstants constants = {};
	Copy3(context.mFrame.currentCameraPos.data(), constants.CameraPos);
	Copy3(context.mFrame.currentCameraForward.data(), constants.CameraForward);
	Copy3(context.mFrame.currentCameraRight.data(), constants.CameraRight);
	Copy3(context.mFrame.currentCameraUp.data(), constants.CameraUp);
	Copy3(context.mFrame.previousCameraPos.data(), constants.PrevCameraPos);
	Copy3(context.mFrame.previousCameraForward.data(), constants.PrevCameraForward);
	Copy3(context.mFrame.previousCameraRight.data(), constants.PrevCameraRight);
	Copy3(context.mFrame.previousCameraUp.data(), constants.PrevCameraUp);
	constants.RenderWidth = context.mFrame.renderWidth;
	constants.RenderHeight = context.mFrame.renderHeight;
	constants.DisplayWidth = context.mFrame.outputWidth;
	constants.DisplayHeight = context.mFrame.outputHeight;
	constants.TanHalfFovX = context.mFrame.currentTanHalfFovX;
	constants.TanHalfFovY = context.mFrame.currentTanHalfFovY;
	constants.PrevTanHalfFovX = context.mFrame.previousTanHalfFovX;
	constants.PrevTanHalfFovY = context.mFrame.previousTanHalfFovY;
	constants.SceneInstanceCount = context.mSceneStats.sceneInstanceCount;
	constants.StaticPrimitiveCount = context.mSceneStats.staticPrimitiveCount;
	constants.DynamicPrimitiveCount = context.mSceneStats.dynamicPrimitiveCount;
	constants.FrameIndex = context.mFrame.frameIndex;
	constants.Flags =
		NRI_FLAG_BOOTSTRAP_VIEW |
		(context.mFrame.resetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(context.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(context.mDirectionalLightState.enabled && context.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u) |
		PackVoxelNormalBlend8(nri_ptvoxelnormalblend);
	constants.StaticMaterialCount = context.mSceneStats.staticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = context.mSceneStats.dynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, context.mDirectionalLightState.color);
	constants.ReservedTrace0 = (uint16_t)(int16_t)context.mFrame.sceneLeft | ((uint32_t)(uint16_t)(int16_t)context.mFrame.sceneTop << 16);
	Copy3(context.mFrame.skyColor.data(), constants.SkyColor);
	Copy3(context.mFrame.groundColor.data(), constants.GroundColor);
	ApplyDirectionalLightStateToConstants(context.mDirectionalLightState, constants);

	NRITextureResource& history = context.mTextures.Get(context.mHistoryOutputSlot);
	NRITextureResource& upscaled = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Composed);
	NRITextureResource& final = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Final);
	context.mResources.TransitionTexture(history, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(upscaled, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(final, NRIComputeStorageState());

	context.mFrameInputDescriptors.fill(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Composed).shaderView);
	context.mFrameInputDescriptors[0] = history.shaderView;
	context.mFrameInputDescriptors[1] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Motion).shaderView;
	context.mFrameInputDescriptors[2] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	context.mFrameInputDescriptors[3] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::NormalRoughness).shaderView;
	context.mFrameInputDescriptors[4] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::BaseColorMetalness).shaderView;
	context.mFrameInputDescriptors[5] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Composed).shaderView;
	context.mFrameInputDescriptors[6] = upscaled.shaderView;
	context.mFrameInputDescriptors[7] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Validation).shaderView;
	context.mFrameInputDescriptors[8] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse).shaderView;
	context.mFrameInputDescriptors[9] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	context.mFrameInputDescriptors[10] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	context.mDescriptors.UpdateFrameTextureSet(context.mUpscalerPrepassFrameTextureSet, context.mFrameInputDescriptors);

	context.mOutputDescriptors.fill(context.mTextures.Get(NRIRenderer::FrameTextureSlot::VendorOutput).storageView);
	context.mOutputDescriptors[2] = final.storageView;
	context.mDescriptors.UpdateOutputSet(context.mUpscalerPrepassOutputSet, context.mOutputDescriptors);

	context.mCommands.SetPipelineLayout(context.mPipelineLayout);
	context.mCommands.SetRootConstants(&constants, sizeof(constants));
	if (!context.mSceneBinding.BindSceneRootDescriptors())
	{
		return false;
	}
	context.mCommands.SetDescriptorSet(0, context.mSamplerSet);
	context.mCommands.SetDescriptorSet(1, context.mSceneBinding.GetCurrentSceneTextureSet());
	context.mCommands.SetDescriptorSet(2, context.mSceneBinding.GetCurrentSceneDataSet());
	context.mCommands.SetDescriptorSet(3, context.mFrameTextureSet);
	context.mCommands.SetDescriptorSet(4, context.mOutputSet);
	context.mCommands.SetPipeline(context.mPipelines.Get(NRIRenderer::PipelineSlot::Final));
	context.mCommands.Dispatch(GetDispatchSize(context.mFrame.targetWidth), GetDispatchSize(context.mFrame.targetHeight), 1);
	return true;
}



bool NRIPassDispatcher::DispatchFrameGraph(NRIPassDispatchContext& context, HWDrawInfo& di, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials, int)
{
	ScopedPtPerfTimer perfTimer(context.mLastPerfShellTraceStats.frameGraphMs);
	Clocker clock(NriPTFrameGraph);

	const int ptDebugMode = (int)GetEffectivePtDebugMode();
	NRIFrameGraphExecutionRequest request = {};
	request.ptDebugMode = ptDebugMode;
	request.denoise = !!nri_denoise;
	request.presentRoute = ResolvePresentRouteInfo((uint32_t)ptDebugMode, !!nri_ptbootstrap);
	return ExecuteNRIFrameGraph(context, di, geometry, materials, request);
}



bool NRIPassDispatcher::DispatchTraceOpaque(NRIPassDispatchContext& context, HWDrawInfo&, const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials)
{
	NRIScopedGpuTiming gpuTiming(context.mResources.frameBuffer, NRIGpuTimingScope::Trace);
	Clocker clock(NriPTTraceOpaque);
	ScopedPtPerfTimer traceOpaqueTimer(context.mLastPerfShellTraceStats.traceOpaqueMs);
	{
		ScopedPtPerfTimer perfTimer(context.mLastPerfShellTraceStats.traceOpaqueReadbackMs);
		context.mTraceShaderStats.Readback(
			context.mResources.BuildResourceServices(),
			ShouldCollectTraceShaderStats(),
			BuildTraceShaderStatsFenceServices(context.mResources.frameBuffer),
			context.mLastPerfTraceShaderStats);
		context.mExposureService.ReadbackAutoExposureStats();
		context.mIndirectRadianceCacheService.ReadbackTelemetry(!!nri_ptindirectradiancecache);
	}
	if (ShouldTracePtPerf())
	{
		const NRIIndirectRadianceCacheTelemetrySnapshot& cache = context.mIndirectRadianceCacheService.GetTelemetry();
		const bool cacheAcceptRequested = (bool)nri_ptindirectradiancecache && (bool)nri_ptindirectradiancecacheaccept;
		Printf("PERF pt indirect radiance cache NRI: frame=%u requested=%u accept_requested=%u mode=%s valid=%u telemetry_frame=%llu lookups=%llu accepted=%llu forced_miss=%llu collision=%llu stale=%llu unsupported=%llu exact_fallback=%llu occupancy=%llu updates=%llu clears=%llu table_bytes=%llu total_bytes=%llu invalidation=0x%x pending_readbacks=%u\n",
			context.mFrame.frameIndex,
			(bool)nri_ptindirectradiancecache ? 1u : 0u,
			cacheAcceptRequested ? 1u : 0u,
			cacheAcceptRequested ? "age-one" : "exact-miss",
			cache.valid ? 1u : 0u,
			(unsigned long long)cache.frameNumber,
			(unsigned long long)cache.lookupCount,
			(unsigned long long)cache.acceptedHitCount,
			(unsigned long long)cache.forcedMissCount,
			(unsigned long long)cache.collisionCount,
			(unsigned long long)cache.staleGenerationCount,
			(unsigned long long)cache.unsupportedRouteCount,
			(unsigned long long)cache.exactFallbackCount,
			(unsigned long long)cache.occupancy,
			(unsigned long long)cache.updateCount,
			(unsigned long long)cache.clearCount,
			(unsigned long long)cache.tableMemoryBytes,
			(unsigned long long)cache.totalMemoryBytes,
			cache.invalidationMask,
			cache.pendingReadbacks);
	}

	if (!context.mResources.UpdateReprojectionBuffer())
	{
		return false;
	}

	NRITraceSceneConstants constants = {};
	const NRITraceSettings traceSettings = BuildNRITraceSettingsFromCVars();
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const NRIMainUpscalerKind resolvedMainUpscaler = context.mUpscalerService.ResolveMainUpscalerKind(false);
	const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMainUpscaler, context.mUpscalerService.GetSelectedUpscalerMode());
	const uint32_t jitterPhaseCount = NRIGetTemporalJitterPhaseCount(resolvedMainUpscaler, resolvedUpscalerMode, context.mFrame.guiCaptureActive);
	const bool directSceneTrace = (!nri_ptbootstrap && nri_ptdirectscene) || bootstrapMode == 11u || bootstrapMode == 12u;
	context.mEffectiveIndirectSamplingMode =
		traceSettings.indirectSamplingMode != 0u &&
		context.mTraceIndirectDenoiserAvailable &&
		!directSceneTrace &&
		traceSettings.lightBounceCount > 0u ? 1u : 0u;
	context.mActiveIndirectSamplingMode =
		context.mEffectiveIndirectSamplingMode != 0u &&
		!context.mFrame.resetHistory ? 1u : 0u;
	const bool indirectRadianceCacheRequested =
		(bool)nri_ptindirectradiancecache &&
		bootstrapMode == 0u &&
		!directSceneTrace &&
		traceSettings.lightBounceCount > 1u;
	NRIIndirectRadianceCachePrepareResult indirectRadianceCache =
		context.mIndirectRadianceCacheService.Prepare(indirectRadianceCacheRequested);
	bool indirectRadianceCacheActive = indirectRadianceCache.active;
	if (indirectRadianceCacheActive && !context.mIndirectRadianceCacheService.RecordPendingClear())
	{
		indirectRadianceCacheActive = false;
	}
	const bool indirectRadianceCacheAccept =
		indirectRadianceCacheActive &&
		(bool)nri_ptindirectradiancecacheaccept &&
		indirectRadianceCache.invalidationMask == NRI_INDIRECT_RADIANCE_CACHE_INVALID_NONE &&
		!indirectRadianceCache.clearRequired &&
		!context.mFrame.resetHistory;
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars(context.mEffectiveIndirectSamplingMode);
	const bool useTemporalJitter =
		!nri_ptbootstrap &&
		!context.mFrame.guiCaptureActive &&
		NRIShouldUseTemporalJitter(resolvedMainUpscaler);
	Copy3(context.mFrame.currentCameraPos.data(), constants.CameraPos);
	Copy3(context.mFrame.currentCameraForward.data(), constants.CameraForward);
	Copy3(context.mFrame.currentCameraRight.data(), constants.CameraRight);
	Copy3(context.mFrame.currentCameraUp.data(), constants.CameraUp);
	Copy3(context.mFrame.previousCameraPos.data(), constants.PrevCameraPos);
	Copy3(context.mFrame.previousCameraForward.data(), constants.PrevCameraForward);
	Copy3(context.mFrame.previousCameraRight.data(), constants.PrevCameraRight);
	Copy3(context.mFrame.previousCameraUp.data(), constants.PrevCameraUp);
	constants.RenderWidth = context.mFrame.renderWidth;
	constants.RenderHeight = context.mFrame.renderHeight;
	constants.DisplayWidth = context.mFrame.outputWidth;
	constants.DisplayHeight = context.mFrame.outputHeight;
	constants.TanHalfFovX = context.mFrame.currentTanHalfFovX;
	constants.TanHalfFovY = context.mFrame.currentTanHalfFovY;
	constants.PrevTanHalfFovX = context.mFrame.previousTanHalfFovX;
	constants.PrevTanHalfFovY = context.mFrame.previousTanHalfFovY;
	constants.SceneInstanceCount = context.mSceneStats.sceneInstanceCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.StaticPrimitiveCount = context.mSceneStats.staticPrimitiveCount;
	constants.FrameIndex = context.mFrame.frameIndex;
	constants.DynamicPrimitiveCount = context.mSceneStats.dynamicPrimitiveCount;
	constants.Flags =
		(context.mFrame.resetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(directSceneTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(context.mUseSplitShadowDenoiser && !directSceneTrace ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(context.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(context.mDirectionalLightState.enabled && context.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u) |
		(nri_ptemissivefastshadow ? NRI_FLAG_FAST_EMISSIVE_SHADOW : 0u) |
		(nri_ptvisiblechunkgate ? NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS : 0u) |
		(ShouldCollectTraceShaderStats() ? NRI_FLAG_TRACE_SHADER_STATS : 0u) |
		(context.mActiveIndirectSamplingMode != 0u ? NRI_FLAG_PROBABILISTIC_INDIRECT : 0u) |
		(indirectRadianceCacheActive ? NRI_FLAG_INDIRECT_RADIANCE_CACHE : 0u) |
		(indirectRadianceCacheAccept ? NRI_FLAG_INDIRECT_RADIANCE_CACHE_ACCEPT : 0u) |
		(nri_pt360absencegate && context.mFrame.spatialAbsenceAuthority ? NRI_FLAG_SPATIAL_ABSENCE_GATE : 0u) |
		(useTemporalJitter ? NRI_FLAG_USE_JITTER : 0u) |
		NRIPackTemporalJitterPhaseCount(jitterPhaseCount) |
		PackVoxelNormalBlend8(nri_ptvoxelnormalblend);
	constants.StaticMaterialCount = context.mSceneStats.staticMaterialCount;
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = context.mSceneStats.dynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(
		traceSettings.lightBounceCount,
		traceSettings.mirrorBounceCount,
		context.mDirectionalLightState.color);
	constants.PortalCount = context.mSceneStats.portalCount;
	constants.RuntimeLightCount = context.mSceneStats.runtimeLightCount;
	constants.PortalDepth = PackPortalDepthAndAmbientMultipliers(
		traceSettings.portalDepth,
		GetBaseAmbient(),
		GetMetalAmbient());
	constants.ReservedTrace0 = (context.mSceneStats.runtimeLightTileCountX & 0xffffu) | ((context.mSceneStats.runtimeLightTileCountY & 0xffffu) << 16u);
	constants.ReservedTrace1 = PackTraceAux1(
		(uint32_t)denoiserSettings.denoiserMode,
		traceSettings.emissiveSampleCount,
		context.mDirectionalLightState.angularSize);
	Copy3(context.mFrame.skyColor.data(), constants.SkyColor);
	Copy3(context.mFrame.groundColor.data(), constants.GroundColor);
	ApplyDirectionalLightStateToConstants(context.mDirectionalLightState, constants);

	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::DirectLighting), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::DirectEmission), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Motion), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::SrInput), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Validation), NRIComputeStorageState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::VendorOutput), NRIComputeStorageState());

	const nri::Descriptor* defaultInput = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Composed).shaderView;
	context.mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	context.mDescriptors.UpdateFrameTextureSet();

	const nri::Descriptor* defaultOutput = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Validation).storageView;
	context.mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	context.mOutputDescriptors[0] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse).storageView;
	context.mOutputDescriptors[3] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Motion).storageView;
	context.mOutputDescriptors[4] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::ViewZ).storageView;
	context.mOutputDescriptors[5] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::NormalRoughness).storageView;
	context.mOutputDescriptors[6] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::BaseColorMetalness).storageView;
	context.mOutputDescriptors[9] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo).storageView;
	context.mOutputDescriptors[10] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).storageView;
	context.mOutputDescriptors[11] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance).storageView;
	context.mOutputDescriptors[12] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra).storageView;
	context.mOutputDescriptors[13] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::DirectLighting).storageView;
	context.mOutputDescriptors[14] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::DirectEmission).storageView;
	context.mDescriptors.UpdateOutputSet();

	const nri::PipelineLayout* tracePipelineLayout = indirectRadianceCacheActive ?
		context.mPipelines.GetIndirectRadianceCachePipelineLayout() : context.mPipelineLayout;
	if (tracePipelineLayout == nullptr)
	{
		return false;
	}
	context.mCommands.SetPipelineLayout(const_cast<nri::PipelineLayout*>(tracePipelineLayout));
	context.mCommands.SetRootConstants(&constants, sizeof(constants));
	if (!context.mSceneBinding.BindSceneRootDescriptors())
	{
		return false;
	}
	context.mCommands.SetDescriptorSet(0, context.mSamplerSet);
	context.mCommands.SetDescriptorSet(1, context.mSceneBinding.GetCurrentSceneTextureSet());
	context.mCommands.SetDescriptorSet(2, context.mSceneBinding.GetCurrentSceneDataSet());
	context.mCommands.SetDescriptorSet(3, context.mFrameTextureSet);
	context.mCommands.SetDescriptorSet(4, context.mOutputSet);
	if (indirectRadianceCacheActive)
	{
		context.mCommands.SetDescriptorSet(NRI_INDIRECT_RADIANCE_CACHE_SET_INDEX, indirectRadianceCache.descriptorSet);
	}
	const uint32_t dispatchX = GetDispatchSize(context.mFrame.renderWidth);
	const uint32_t dispatchY = GetDispatchSize(context.mFrame.renderHeight);
	const uint32_t dispatchZ = 1;
	context.mLastPerfShellTraceStats.traceOpaqueDispatchX = dispatchX;
	context.mLastPerfShellTraceStats.traceOpaqueDispatchY = dispatchY;
	context.mLastPerfShellTraceStats.traceOpaqueDispatchZ = dispatchZ;
	auto& tracePerf = context.mLastPerfShellTraceStats;
	// frameIndex is zero-based; the compact capture contract uses zero as the
	// unresolved identity sentinel, so publish it as a one-based frame serial.
	tracePerf.traceRendererFrame = (uint64_t)context.mFrame.frameIndex + 1ull;
	tracePerf.traceRenderWidth = constants.RenderWidth;
	tracePerf.traceRenderHeight = constants.RenderHeight;
	tracePerf.traceOutputWidth = constants.DisplayWidth;
	tracePerf.traceOutputHeight = constants.DisplayHeight;
	tracePerf.traceLightBounceCount = traceSettings.lightBounceCount;
	tracePerf.traceMirrorBounceCount = traceSettings.mirrorBounceCount;
	tracePerf.tracePortalDepth = traceSettings.portalDepth;
	tracePerf.traceEmissiveSampleCount = traceSettings.emissiveSampleCount;
	tracePerf.traceEmissiveRequestedSampleCount = traceSettings.emissiveRequestedSampleCount;
	tracePerf.traceEmissivePrimarySampleBudget = traceSettings.emissivePrimarySampleBudget;
	tracePerf.traceIndirectSamplingRequestedMode = traceSettings.indirectSamplingMode;
	tracePerf.traceIndirectSamplingEffectiveMode = context.mEffectiveIndirectSamplingMode;
	tracePerf.traceIndirectSamplingActiveMode = context.mActiveIndirectSamplingMode;
	tracePerf.traceHitDistanceReconstructionMode = denoiserSettings.hitDistanceReconstructionMode;
	tracePerf.traceRuntimeLightCount = context.mSceneStats.runtimeLightCount;
	tracePerf.traceRuntimeLightTileCountX = context.mSceneStats.runtimeLightTileCountX;
	tracePerf.traceRuntimeLightTileCountY = context.mSceneStats.runtimeLightTileCountY;
	tracePerf.traceRuntimeLightTileSize = context.mSceneStats.runtimeLightTileSize;
	tracePerf.traceRuntimeLightTileIndexCount = context.mSceneStats.runtimeLightTileIndexCount;
	tracePerf.traceRuntimeLightMaxTileOccupancy = context.mSceneStats.runtimeLightMaxTileOccupancy;
	tracePerf.traceEmissivePrimitiveCount = context.mSceneStats.emissivePrimitiveCount;
	tracePerf.traceEmissiveTotalPower = context.mSceneStats.emissiveTotalPower;
	tracePerf.traceFlags = constants.Flags;
	tracePerf.traceDebugMode = constants.DebugMode;
	tracePerf.traceBootstrapMode = constants.BootstrapMode;
	tracePerf.traceUpscalerKind = (uint32_t)resolvedMainUpscaler;
	tracePerf.traceUpscalerMode = (uint32_t)resolvedUpscalerMode;
	tracePerf.traceDenoiserMode = (uint32_t)denoiserSettings.denoiserMode;
	tracePerf.traceDirectScene = directSceneTrace ? 1u : 0u;
	tracePerf.traceDirectional = context.mDirectionalLightState.enabled ? 1u : 0u;
	tracePerf.traceDirectionalShadow = context.mDirectionalLightState.enabled && context.mDirectionalLightState.shadow ? 1u : 0u;
	tracePerf.traceSplitShadow = context.mUseSplitShadowDenoiser && !directSceneTrace ? 1u : 0u;
	tracePerf.traceFastEmissiveShadow = nri_ptemissivefastshadow ? 1u : 0u;
	tracePerf.traceVisibleChunkGate = nri_ptvisiblechunkgate ? 1u : 0u;
	uint64_t settingsKey = 1469598103934665603ull;
	const uint64_t settingsValues[] = {
		constants.RenderWidth, constants.RenderHeight, constants.DisplayWidth, constants.DisplayHeight,
		dispatchX, dispatchY, dispatchZ, constants.Flags & ~NRI_FLAG_RESET_HISTORY,
		constants.DebugMode, constants.BootstrapMode, constants.BounceCounts, constants.PortalDepth,
		constants.ReservedTrace1, traceSettings.emissiveRequestedSampleCount,
		traceSettings.emissivePrimarySampleBudget, traceSettings.indirectSamplingMode,
		context.mEffectiveIndirectSamplingMode, context.mActiveIndirectSamplingMode,
		denoiserSettings.hitDistanceReconstructionMode,
		(uint32_t)resolvedMainUpscaler, (uint32_t)resolvedUpscalerMode
	};
	for (uint64_t value : settingsValues) settingsKey = AppendTraceWorkloadHash(settingsKey, value);
	tracePerf.traceSettingsKey = settingsKey;
	uint64_t workloadKey = settingsKey;
	const uint64_t workloadValues[] = {
		constants.Flags, constants.ReservedTrace0,
		constants.SceneInstanceCount, constants.StaticPrimitiveCount, constants.DynamicPrimitiveCount,
		constants.StaticMaterialCount, constants.DynamicMaterialCount, constants.PortalCount,
		constants.RuntimeLightCount, context.mSceneStats.runtimeLightTileSize,
		context.mSceneStats.runtimeLightTileIndexCount, context.mSceneStats.runtimeLightMaxTileOccupancy,
		context.mSceneStats.emissivePrimitiveCount, (uint32_t)resolvedMainUpscaler, (uint32_t)resolvedUpscalerMode,
		tracePerf.traceVoxelOccurrenceControl
	};
	for (uint64_t value : workloadValues) workloadKey = AppendTraceWorkloadHash(workloadKey, value);
	uint32_t emissivePowerBits = 0;
	static_assert(sizeof(emissivePowerBits) == sizeof(context.mSceneStats.emissiveTotalPower));
	std::memcpy(&emissivePowerBits, &context.mSceneStats.emissiveTotalPower, sizeof(emissivePowerBits));
	tracePerf.traceWorkloadKey = AppendTraceWorkloadHash(workloadKey, emissivePowerBits);
	{
		ScopedPtPerfTimer perfTimer(context.mLastPerfShellTraceStats.traceOpaqueCommandMs);
		context.mTraceShaderStats.ResetBuffer(context.mResources.BuildResourceServices(), ShouldCollectTraceShaderStats());
		context.mCommands.core->CmdBeginAnnotation(*context.mCommands.commandBuffer, "Raze.TraceOpaque.Dispatch", nri::BGRA_UNUSED);
		context.mCommands.SetPipeline(context.mPipelines.Get(
			indirectRadianceCacheActive ? NRIRenderer::PipelineSlot::TraceOpaqueCache : NRIRenderer::PipelineSlot::TraceOpaque));
		{
			NRIScopedGpuTiming dispatchGpuTiming(context.mResources.frameBuffer, NRIGpuTimingScope::TraceDispatch);
			context.mCommands.Dispatch(dispatchX, dispatchY, dispatchZ);
		}
		context.mCommands.core->CmdEndAnnotation(*context.mCommands.commandBuffer);
	}
	{
		ScopedPtPerfTimer perfTimer(context.mLastPerfShellTraceStats.traceOpaqueStatsCopyMs);
		NRITraceShaderStatsCopyInput input = {};
		input.enabled = ShouldCollectTraceShaderStats();
		input.frameNumber = (uint64_t)context.mFrame.frameIndex;
		input.fences = BuildTraceShaderStatsFenceServices(context.mResources.frameBuffer);
		input.boundSceneInstances = &context.mBoundSceneInstances;
		input.staticPrimitiveCount = context.mSceneStats.staticPrimitiveCount;
		input.dynamicPrimitiveCount = context.mSceneStats.dynamicPrimitiveCount;
		input.persistentVoxelPrimitiveCount = context.mPersistentVoxels.BoundPrimitiveCount();
		context.mTraceShaderStats.CopyForReadback(context.mResources.BuildResourceServices(), input);
		if (indirectRadianceCacheActive)
		{
			context.mIndirectRadianceCacheService.CopyTelemetry((uint64_t)context.mFrame.frameIndex);
			context.mIndirectRadianceCacheService.AdvanceFrame();
		}
	}
	return true;
}



bool NRIPassDispatcher::DispatchDenoiser(NRIPassDispatchContext& context)
{
	NRIScopedGpuTiming gpuTiming(context.mResources.frameBuffer, NRIGpuTimingScope::Denoise);
	Clocker clock(NriPTDenoiser);
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars(context.mEffectiveIndirectSamplingMode);

	if (!context.mNrd.EnsureReady(*context.mResources.device, context.mFrame.renderWidth, context.mFrame.renderHeight, context.mFrame.queuedFrameNum))
	{
		return false;
	}

	context.mNrd.NewFrame();

	NRINrdDispatchDesc desc = {};
	desc.commandBuffer = context.mCommands.GetCommandBuffer();
	desc.motion = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::Motion);
	desc.viewZ = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::ViewZ);
	desc.normalRoughness = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::NormalRoughness);
	desc.baseColorMetalness = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::BaseColorMetalness);
	desc.unfilteredDiffuse = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse);
	desc.unfilteredSpecular = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredSpecular);
	desc.unfilteredPenumbra = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra);
	desc.diffuse = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::DenoisedDiffuse);
	desc.specular = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::DenoisedSpecular);
	desc.shadow = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::DenoisedShadow);
	desc.validation = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::Validation);
	desc.resourceWidth = context.mFrame.renderWidth;
	desc.resourceHeight = context.mFrame.renderHeight;
	desc.frameIndex = context.mFrame.frameIndex;
	desc.queuedFrameNum = context.mFrame.queuedFrameNum;
	desc.observedFrameTimeMs = context.mFrame.observedFrameTimeMs;
	Copy2(context.mFrame.currentJitter.data(), desc.cameraJitter);
	Copy2(context.mFrame.previousJitter.data(), desc.cameraJitterPrev);
	std::memcpy(desc.viewToClipMatrix, context.mFrame.currentViewToClip.data(), sizeof(desc.viewToClipMatrix));
	std::memcpy(desc.viewToClipMatrixPrev, context.mFrame.previousViewToClip.data(), sizeof(desc.viewToClipMatrixPrev));
	std::memcpy(desc.worldToViewMatrix, context.mFrame.currentWorldToView.data(), sizeof(desc.worldToViewMatrix));
	std::memcpy(desc.worldToViewMatrixPrev, context.mFrame.previousWorldToView.data(), sizeof(desc.worldToViewMatrixPrev));
	desc.lightDirection[0] = context.mDirectionalLightState.direction[0];
	desc.lightDirection[1] = context.mDirectionalLightState.direction[1];
	desc.lightDirection[2] = context.mDirectionalLightState.direction[2];
	Normalize3(desc.lightDirection);
	desc.denoiserMode = denoiserSettings.denoiserMode;
	desc.maxAccumulatedFrameNum = denoiserSettings.maxAccumulatedFrameNum;
	desc.maxFastAccumulatedFrameNum = denoiserSettings.maxFastAccumulatedFrameNum;
	desc.maxStabilizedFrameNum = denoiserSettings.maxStabilizedFrameNum;
	desc.hitDistanceReconstructionMode = denoiserSettings.hitDistanceReconstructionMode;
	desc.indirectSamplingMode = context.mEffectiveIndirectSamplingMode;
	desc.fastHistoryClampingSigmaScale = denoiserSettings.fastHistoryClampingSigmaScale;
	desc.diffusePrepassBlurRadius = denoiserSettings.diffusePrepassBlurRadius;
	desc.specularPrepassBlurRadius = denoiserSettings.specularPrepassBlurRadius;
	desc.minBlurRadius = denoiserSettings.minBlurRadius;
	desc.maxBlurRadius = denoiserSettings.maxBlurRadius;
	desc.sigmaMaxStabilizedFrameNum = denoiserSettings.sigmaMaxStabilizedFrameNum;
	desc.sigmaPlaneDistanceSensitivity = denoiserSettings.sigmaPlaneDistanceSensitivity;
	desc.resetHistory = context.mFrame.resetHistory;
	desc.enableAntiFirefly = denoiserSettings.enableAntiFirefly;
	desc.enableValidation = denoiserSettings.enableValidation;
	desc.enableSigmaShadow = context.mUseSplitShadowDenoiser;
	desc.traceTemporalInput = ShouldTracePtPerf();
	const bool denoised = context.mNrd.Denoise(desc);
	// NRD binds its private descriptor pool while recording dispatches. Restore
	// Raze's resource and sampler heaps before any subsequent pass binds our sets.
	context.mCommands.RestoreDescriptorPool();
	return denoised;
}



bool NRIPassDispatcher::DispatchComposition(NRIPassDispatchContext& context, NRIRenderer::FrameTextureSlot outputSlot)
{
	NRIScopedGpuTiming gpuTiming(context.mResources.frameBuffer, NRIGpuTimingScope::Composition);
	Clocker clock(NriPTComposition);

	NRITraceSceneConstants constants = {};
	const NRIDenoiserSettings denoiserSettings = BuildNRIDenoiserSettingsFromCVars();
	Copy3(context.mFrame.currentCameraPos.data(), constants.CameraPos);
	Copy3(context.mFrame.currentCameraForward.data(), constants.CameraForward);
	Copy3(context.mFrame.currentCameraRight.data(), constants.CameraRight);
	Copy3(context.mFrame.currentCameraUp.data(), constants.CameraUp);
	Copy3(context.mFrame.previousCameraPos.data(), constants.PrevCameraPos);
	Copy3(context.mFrame.previousCameraForward.data(), constants.PrevCameraForward);
	Copy3(context.mFrame.previousCameraRight.data(), constants.PrevCameraRight);
	Copy3(context.mFrame.previousCameraUp.data(), constants.PrevCameraUp);
	constants.RenderWidth = context.mFrame.renderWidth;
	constants.RenderHeight = context.mFrame.renderHeight;
	constants.DisplayWidth = context.mFrame.outputWidth;
	constants.DisplayHeight = context.mFrame.outputHeight;
	constants.TanHalfFovX = context.mFrame.currentTanHalfFovX;
	constants.TanHalfFovY = context.mFrame.currentTanHalfFovY;
	constants.PrevTanHalfFovX = context.mFrame.previousTanHalfFovX;
	constants.PrevTanHalfFovY = context.mFrame.previousTanHalfFovY;
	constants.FrameIndex = context.mFrame.frameIndex;
	constants.Flags =
		(context.mFrame.resetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(context.mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(context.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(context.mDirectionalLightState.enabled && context.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, context.mDirectionalLightState.color);
	constants.RuntimeLightCount = context.mSceneStats.runtimeLightCount;
	constants.ReservedTrace0 = denoiserSettings.inputSplitMode;
	constants.ReservedTrace1 = PackDenoiserAux1((uint32_t)denoiserSettings.denoiserMode, context.mDirectionalLightState.angularSize);
	Copy3(context.mFrame.skyColor.data(), constants.SkyColor);
	Copy3(context.mFrame.groundColor.data(), constants.GroundColor);
	ApplyDirectionalLightStateToConstants(context.mDirectionalLightState, constants);

	NRITextureResource& diffuse = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse);
	NRITextureResource& specular = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredSpecular);
	NRITextureResource& viewZ = context.mTextures.Get(NRIRenderer::FrameTextureSlot::ViewZ);
	NRITextureResource& normalRoughness = context.mTextures.Get(NRIRenderer::FrameTextureSlot::NormalRoughness);
	NRITextureResource& baseColorMetalness = context.mTextures.Get(NRIRenderer::FrameTextureSlot::BaseColorMetalness);
	NRITextureResource& rawShadow = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra);
	NRITextureResource& directLighting = context.mTextures.Get(NRIRenderer::FrameTextureSlot::DirectLighting);
	NRITextureResource& directEmission = context.mTextures.Get(NRIRenderer::FrameTextureSlot::DirectEmission);
	const NRIRenderer::FrameTextureSlot filteredDiffuseSlot = context.mUseDenoisedCompositionInputs ? NRIRenderer::FrameTextureSlot::DenoisedDiffuse : NRIRenderer::FrameTextureSlot::UnfilteredDiffuse;
	const NRIRenderer::FrameTextureSlot filteredSpecularSlot = context.mUseDenoisedCompositionInputs ? NRIRenderer::FrameTextureSlot::DenoisedSpecular : NRIRenderer::FrameTextureSlot::UnfilteredSpecular;
	const NRIRenderer::FrameTextureSlot filteredShadowSlot = context.mUseDenoisedCompositionInputs ? NRIRenderer::FrameTextureSlot::DenoisedShadow : NRIRenderer::FrameTextureSlot::UnfilteredPenumbra;
	NRITextureResource& filteredDiffuse = context.mTextures.Get(filteredDiffuseSlot);
	NRITextureResource& filteredSpecular = context.mTextures.Get(filteredSpecularSlot);
	NRITextureResource& filteredShadow = context.mTextures.Get(filteredShadowSlot);
	NRITextureResource& composed = context.mTextures.Get(outputSlot);

	context.mResources.TransitionTexture(diffuse, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(specular, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(viewZ, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(normalRoughness, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(baseColorMetalness, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(rawShadow, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(directLighting, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(directEmission, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(filteredDiffuse, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(filteredSpecular, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(filteredShadow, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(composed, NRIComputeStorageState());

	const nri::Descriptor* defaultInput = diffuse.shaderView;
	context.mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	context.mFrameInputDescriptors[2] = viewZ.shaderView;
	context.mFrameInputDescriptors[3] = normalRoughness.shaderView;
	context.mFrameInputDescriptors[4] = baseColorMetalness.shaderView;
	context.mFrameInputDescriptors[5] = diffuse.shaderView;
	context.mFrameInputDescriptors[6] = specular.shaderView;
	context.mFrameInputDescriptors[8] = filteredDiffuse.shaderView;
	context.mFrameInputDescriptors[9] = filteredSpecular.shaderView;
	context.mFrameInputDescriptors[10] = rawShadow.shaderView;
	context.mFrameInputDescriptors[11] = filteredShadow.shaderView;
	context.mFrameInputDescriptors[12] = directLighting.shaderView;
	context.mFrameInputDescriptors[13] = directEmission.shaderView;
	context.mDescriptors.UpdateFrameTextureSet(context.mCompositionFrameTextureSet, context.mFrameInputDescriptors);

	const nri::Descriptor* defaultOutput = composed.storageView;
	context.mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	context.mOutputDescriptors[1] = composed.storageView;
	context.mDescriptors.UpdateOutputSet(context.mCompositionOutputSet, context.mOutputDescriptors);

	context.mCommands.SetPipelineLayout(context.mPipelineLayout);
	context.mCommands.SetRootConstants(&constants, sizeof(constants));
	if (!context.mSceneBinding.BindSceneRootDescriptors())
	{
		return false;
	}
	context.mCommands.SetDescriptorSet(0, context.mSamplerSet);
	context.mCommands.SetDescriptorSet(1, context.mSceneBinding.GetCurrentSceneTextureSet());
	context.mCommands.SetDescriptorSet(2, context.mSceneBinding.GetCurrentSceneDataSet());
	context.mCommands.SetDescriptorSet(3, context.mCompositionFrameTextureSet);
	context.mCommands.SetDescriptorSet(4, context.mCompositionOutputSet);
	context.mCommands.SetPipeline(context.mPipelines.Get(NRIRenderer::PipelineSlot::Composition));
	context.mCommands.Dispatch(GetDispatchSize(context.mFrame.renderWidth), GetDispatchSize(context.mFrame.renderHeight), 1);
	return true;
}



bool NRIPassDispatcher::DispatchTraceTransparent(NRIPassDispatchContext& context)
{
	Clocker clock(NriPTComposition);
	NRISmokeRouteDesc route = {};
	route.inputSlot = NRIRenderer::FrameTextureSlot::Composed;
	route.outputSlot = NRIRenderer::FrameTextureSlot::TraceTransparentOutput;
	route.depthSlot = NRIRenderer::FrameTextureSlot::ViewZ;
	route.exposureDomain = NRIRenderer::ExposureDomain::SceneHDR;
	route.placement = NRISmokeRoutePlacement::StandardPreUpscale;
	route.width = context.mFrame.renderWidth;
	route.height = context.mFrame.renderHeight;
	route.supported = true;
	return context.mSmokeService.DispatchRoute(route);
}



bool NRIPassDispatcher::DispatchUpscalerPrepass(NRIPassDispatchContext& context, NRIMainUpscalerKind mainKind)
{
	if (mainKind == NRIMainUpscalerKind::Off)
	{
		return false;
	}

	const NRIRenderer::FrameTextureSlot vendorInputSlot =
		mainKind == NRIMainUpscalerKind::DLSR ? NRIRenderer::FrameTextureSlot::SrInput :
		GetDlrrMainInputSlot();
	NRITextureResource& vendorInput = context.mTextures.Get(vendorInputSlot);
	NRITextureResource& upscalerDepth = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UpscalerDepth);
	NRITextureResource& rrGuideDiffuseAlbedo = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo);
	NRITextureResource& rrGuideSpecularAlbedo = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo);
	NRITextureResource& rrGuideSpecularHitDistance = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance);
	NRITextureResource& rrGuideNormalRoughness = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideNormalRoughness);
	const bool useSrPrepass = mainKind == NRIMainUpscalerKind::DLSR;

	// SR consumes the post-transparent composed signal, while RR now arrives with an
	// explicitly prepared noisy RrInput from the frame-graph path above.
	if (useSrPrepass)
	{
		context.mResources.CopyTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::TraceTransparentOutput), vendorInput);
	}
	context.mResources.TransitionTexture(vendorInput, NRIComputeShaderResourceState());

	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(upscalerDepth, NRIComputeStorageState());
	if (!useSrPrepass)
	{
		context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeStorageState());
		context.mResources.TransitionTexture(rrGuideSpecularAlbedo, NRIComputeStorageState());
		context.mResources.TransitionTexture(rrGuideSpecularHitDistance, NRIComputeStorageState());
		context.mResources.TransitionTexture(rrGuideNormalRoughness, NRIComputeStorageState());
	}

	const nri::Descriptor* defaultInput = context.mTextures.Get(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	context.mFrameInputDescriptors.fill(const_cast<nri::Descriptor*>(defaultInput));
	context.mFrameInputDescriptors[2] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	if (!useSrPrepass)
	{
		context.mFrameInputDescriptors[3] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::NormalRoughness).shaderView;
		context.mFrameInputDescriptors[4] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::BaseColorMetalness).shaderView;
		context.mFrameInputDescriptors[6] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	if (!context.mDescriptors.UpdateFrameTextureSet(context.mUpscalerPrepassFrameTextureSet, context.mFrameInputDescriptors))
	{
		return false;
	}

	const nri::Descriptor* defaultOutput = upscalerDepth.storageView;
	context.mOutputDescriptors.fill(const_cast<nri::Descriptor*>(defaultOutput));
	context.mOutputDescriptors[12] = upscalerDepth.storageView;
	if (!useSrPrepass)
	{
		context.mOutputDescriptors[5] = rrGuideNormalRoughness.storageView;
		context.mOutputDescriptors[9] = rrGuideDiffuseAlbedo.storageView;
		context.mOutputDescriptors[10] = rrGuideSpecularAlbedo.storageView;
		context.mOutputDescriptors[11] = rrGuideSpecularHitDistance.storageView;
	}
	if (!context.mDescriptors.UpdateOutputSet(context.mUpscalerPrepassOutputSet, context.mOutputDescriptors))
	{
		return false;
	}

	NRITraceSceneConstants constants = {};
	constants.RenderWidth = context.mFrame.renderWidth;
	constants.RenderHeight = context.mFrame.renderHeight;
	constants.DisplayWidth = context.mFrame.outputWidth;
	constants.DisplayHeight = context.mFrame.outputHeight;
	constants.FrameIndex = context.mFrame.frameIndex;
	constants.ReservedTrace0 =
		mainKind == NRIMainUpscalerKind::DLSR ? 1u :
		mainKind == NRIMainUpscalerKind::DLRR ? 2u :
		0u;
	constants.ReservedTrace1 = (uint32_t)GetSelectedNrdDenoiserMode();
	constants.Flags = context.mFrame.resetHistory ? NRI_FLAG_RESET_HISTORY : 0u;
	context.mCommands.SetPipelineLayout(context.mPipelineLayout);
	context.mCommands.SetRootConstants(&constants, sizeof(constants));
	if (!context.mSceneBinding.BindSceneRootDescriptors())
	{
		return false;
	}
	context.mCommands.SetDescriptorSet(0, context.mSamplerSet);
	context.mCommands.SetDescriptorSet(1, context.mSceneBinding.GetCurrentSceneTextureSet());
	context.mCommands.SetDescriptorSet(2, context.mSceneBinding.GetCurrentSceneDataSet());
	context.mCommands.SetDescriptorSet(3, context.mUpscalerPrepassFrameTextureSet);
	context.mCommands.SetDescriptorSet(4, context.mUpscalerPrepassOutputSet);
	context.mCommands.SetPipeline(context.mPipelines.Get(useSrPrepass ? NRIRenderer::PipelineSlot::DlssSrBefore : NRIRenderer::PipelineSlot::DlssBefore));
	context.mCommands.Dispatch(GetDispatchSize(context.mFrame.renderWidth), GetDispatchSize(context.mFrame.renderHeight), 1);
	return true;
}



bool NRIPassDispatcher::DispatchRawPresent(NRIPassDispatchContext& context, NRIRenderer::FrameTextureSlot inputSlot, NRIRenderer::FrameTextureSlot secondarySlot, NRIRenderer::FrameTextureSlot tertiarySlot)
{
	Clocker clock(NriPTRawPresent);

	NRIPresentConstants constants = {};
	ApplyOutputPolicyToPresentConstants(context.mResources.GetOutputPolicy(), constants);
	constants.DisplayWidth = context.mFrame.outputWidth;
	constants.DisplayHeight = context.mFrame.outputHeight;
	constants.FrameIndex = context.mFrame.frameIndex;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.PackedSceneOrigin = PackPresentSceneOrigin(context.mFrame.sceneLeft, context.mFrame.sceneTop);
	constants.DenoiserMode = (uint32_t)GetSelectedNrdDenoiserMode();

	NRITextureResource& input = context.mTextures.Get(inputSlot);
	constants.InputWidth = input.width;
	constants.InputHeight = input.height;
	const bool addSecondary = secondarySlot != NRIRenderer::FrameTextureSlot::Count;
	NRITextureResource& secondary = context.mTextures.Get(addSecondary ? secondarySlot : inputSlot);
	const bool hasTertiary = tertiarySlot != NRIRenderer::FrameTextureSlot::Count;
	NRITextureResource& tertiary = context.mTextures.Get(hasTertiary ? tertiarySlot : inputSlot);
	NRITextureResource& final = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Final);
	if (addSecondary)
	{
		constants.Flags |= NRI_FLAG_RAW_PRESENT_ADD_SECONDARY;
	}
	if (context.mUseSplitShadowDenoiser)
	{
		constants.Flags |= NRI_PRESENT_FLAG_SPLIT_SHADOW_DENOISER;
	}

	context.mResources.TransitionTexture(input, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(secondary, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(tertiary, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		secondary.shaderView,
		tertiary.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = context.mRawPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	context.mCommands.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = context.mRawPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	context.mCommands.UpdateDescriptorRanges(&outputUpdate, 1);

	context.mCommands.SetPipelineLayout(context.mPresentPipelineLayout);
	context.mCommands.SetRootConstants(&constants, sizeof(constants));
	context.mCommands.SetDescriptorSet(0, context.mRawPresentFrameTextureSet);
	context.mCommands.SetDescriptorSet(1, context.mRawPresentOutputSet);
	context.mCommands.SetPipeline(context.mPipelines.Get(NRIRenderer::PipelineSlot::RawPresent));
	context.mCommands.Dispatch(GetDispatchSize(context.mFrame.targetWidth), GetDispatchSize(context.mFrame.targetHeight), 1);
	return true;
}



bool NRIPassDispatcher::DispatchFinalPresent(NRIPassDispatchContext& context, NRIRenderer::FrameTextureSlot inputSlot)
{
	Clocker clock(NriPTFinalPresent);

	const NRIPTOutputPolicy outputPolicy = context.mResources.GetOutputPolicy();
	const NRIMainUpscalerKind resolvedMain = context.mUpscalerService.ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPost = context.mUpscalerService.ResolvePostSharpenKind(false);
	const NRIRenderer::ExposureRoute exposureRoute = context.mExposureService.ResolveExposureRoute(inputSlot, outputPolicy, resolvedMain, resolvedPost);
	NRIPresentConstants constants = {};
	ApplyOutputPolicyToPresentConstants(outputPolicy, constants);
	ApplyNightVisionStateToPresentConstants(context.mNightVisionState, constants);
	constants.Exposure = exposureRoute.presentExposure;
	const bool finalPresentInputPreExposed = exposureRoute.inputDomain == NRIRenderer::ExposureDomain::PreExposedHDR;
	const bool finalPresentAutoExposureEligible =
		context.mExposure.GetSettings().enabled &&
		exposureRoute.inputDomain == NRIRenderer::ExposureDomain::SceneHDR;
	NRITextureResource* exposureStateTexture = nullptr;
	if (finalPresentAutoExposureEligible)
	{
		NRITextureResource& candidateExposureState = context.mExposure.GetMutableExposureStateTexture(context.mFrame.frameIndex & 1u);
		if (candidateExposureState.texture != nullptr)
		{
			exposureStateTexture = &candidateExposureState;
		}
	}
	const bool exposureStateTextureValid =
		exposureStateTexture != nullptr &&
		exposureStateTexture->shaderView != nullptr;
	constants.OutputFlags |=
		(finalPresentAutoExposureEligible ? NRI_PRESENT_OUTPUT_FLAG_AUTO_EXPOSURE : 0u) |
		(exposureStateTextureValid ? NRI_PRESENT_OUTPUT_FLAG_EXPOSURE_TEXTURE_VALID : 0u) |
		(finalPresentInputPreExposed ? NRI_PRESENT_OUTPUT_FLAG_INPUT_PRE_EXPOSED : 0u);
	constants.DisplayWidth = context.mFrame.outputWidth;
	constants.DisplayHeight = context.mFrame.outputHeight;
	constants.FrameIndex = context.mFrame.frameIndex;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.PackedSceneOrigin = PackPresentSceneOrigin(context.mFrame.sceneLeft, context.mFrame.sceneTop);

	NRITextureResource& input = context.mTextures.Get(inputSlot);
	NRITextureResource& final = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Final);
	constants.InputWidth = input.width;
	constants.InputHeight = input.height;

	context.mResources.TransitionTexture(input, NRIComputeShaderResourceState());
	if (exposureStateTextureValid)
	{
		context.mResources.TransitionTexture(*exposureStateTexture, NRIComputeShaderResourceState());
	}
	context.mResources.TransitionTexture(final, NRIComputeStorageState());

	const nri::Descriptor* inputs[3] = {
		input.shaderView,
		exposureStateTextureValid ? exposureStateTexture->shaderView : input.shaderView,
		input.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = context.mFinalPresentFrameTextureSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputs;
	inputUpdate.descriptorNum = (uint32_t)std::size(inputs);
	context.mCommands.UpdateDescriptorRanges(&inputUpdate, 1);

	const nri::Descriptor* outputs[1] = { final.storageView };
	nri::UpdateDescriptorRangeDesc outputUpdate = {};
	outputUpdate.descriptorSet = context.mFinalPresentOutputSet;
	outputUpdate.rangeIndex = 0;
	outputUpdate.descriptors = outputs;
	outputUpdate.descriptorNum = (uint32_t)std::size(outputs);
	context.mCommands.UpdateDescriptorRanges(&outputUpdate, 1);

	context.mCommands.SetPipelineLayout(context.mPresentPipelineLayout);
	context.mCommands.SetRootConstants(&constants, sizeof(constants));
	context.mCommands.SetDescriptorSet(0, context.mFinalPresentFrameTextureSet);
	context.mCommands.SetDescriptorSet(1, context.mFinalPresentOutputSet);
	context.mCommands.SetPipeline(context.mPipelines.Get(NRIRenderer::PipelineSlot::FinalPresent));
	context.mCommands.Dispatch(GetDispatchSize(context.mFrame.targetWidth), GetDispatchSize(context.mFrame.targetHeight), 1);
	return true;
}



bool NRIPassDispatcher::DispatchUpscaleChain(NRIPassDispatchContext& context)
{
	NRIScopedGpuTiming gpuTiming(context.mResources.frameBuffer, NRIGpuTimingScope::Upscale);
	Clocker clock(NriPTUpscale);

	const NRIMainUpscalerKind mainKind = context.mUpscalerService.ResolveMainUpscalerKind(true);
	const NRIPostSharpenKind postSharpenKind = context.mUpscalerService.ResolvePostSharpenKind(true);
	const bool runAppTaa = NRIShouldRunAppTaa(mainKind);
	const bool useAppTaaJitter = runAppTaa && !context.mFrame.guiCaptureActive;
	NRITextureResource& composed = context.mTextures.Get(NRIRenderer::FrameTextureSlot::TraceTransparentOutput);
	const NRIRenderer::FrameTextureSlot vendorSourceSlot =
		mainKind == NRIMainUpscalerKind::DLRR ? GetDlrrMainInputSlot() :
		NRIRenderer::FrameTextureSlot::TraceTransparentOutput;
	NRITextureResource& historyInput = context.mTextures.Get(context.mHistoryInputSlot);
	NRITextureResource& historyOutput = context.mTextures.Get(context.mHistoryOutputSlot);
	context.mUpscalerService.TraceTemporalState("upscale-entry", mainKind, postSharpenKind, runAppTaa, context.mHistoryOutputSlot, vendorSourceSlot);

	if (runAppTaa)
	{
		NRITemporalConstants constants = {};
		constants.RenderWidth = context.mFrame.renderWidth;
		constants.RenderHeight = context.mFrame.renderHeight;
		constants.FrameIndex = context.mFrame.frameIndex;
		constants.Flags =
			(context.mFrame.resetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
			(useAppTaaJitter ? NRI_FLAG_USE_JITTER : 0u) |
			NRIPackTemporalJitterPhaseCount(NRIGetTemporalJitterPhaseCount(
				mainKind,
				NRIResolveUpscalerModeForMain(mainKind, context.mUpscalerService.GetSelectedUpscalerMode()),
				context.mFrame.guiCaptureActive));
		constants.Exposure = GetTemporalExposure(context.mResources.GetOutputPolicy());
		NRITextureResource* exposureStateTexture = nullptr;
		if (context.mExposure.GetSettings().enabled)
		{
			NRITextureResource& candidateExposureState = context.mExposure.GetMutableExposureStateTexture(context.mFrame.frameIndex & 1u);
			if (candidateExposureState.texture != nullptr)
			{
				exposureStateTexture = &candidateExposureState;
			}
		}
		const bool exposureStateTextureValid =
			exposureStateTexture != nullptr &&
			exposureStateTexture->shaderView != nullptr;
		constants.Flags |=
			(context.mExposure.GetSettings().enabled ? NRI_TEMPORAL_FLAG_AUTO_EXPOSURE : 0u) |
			(exposureStateTextureValid ? NRI_TEMPORAL_FLAG_EXPOSURE_TEXTURE_VALID : 0u);
		const NRIRenderer::FrameTextureSlot volumeMetaSlot = context.mSmokeService.GetVolumeSlot(true);
		NRITextureResource* volumeMeta = volumeMetaSlot != NRIRenderer::FrameTextureSlot::Count ? &context.mTextures.Get(volumeMetaSlot) : nullptr;
		const bool volumeMetaValid = volumeMeta != nullptr && volumeMeta->shaderView != nullptr &&
			volumeMeta->width == context.mFrame.renderWidth && volumeMeta->height == context.mFrame.renderHeight;
		if (volumeMetaValid)
			constants.Flags |= NRI_TEMPORAL_FLAG_VOLUME_REACTIVE;

		context.mResources.TransitionTexture(composed, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(historyInput, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		if (exposureStateTextureValid)
		{
			context.mResources.TransitionTexture(*exposureStateTexture, NRIComputeShaderResourceState());
		}
		if (volumeMetaValid)
			context.mResources.TransitionTexture(*volumeMeta, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(historyOutput, NRIComputeStorageState());

		const nri::Descriptor* taaInputs[5] = {
			historyInput.shaderView,
			context.mTextures.Get(NRIRenderer::FrameTextureSlot::Motion).shaderView,
			composed.shaderView,
			exposureStateTextureValid ? exposureStateTexture->shaderView : composed.shaderView,
			volumeMetaValid ? volumeMeta->shaderView : composed.shaderView
		};
		nri::UpdateDescriptorRangeDesc taaInputUpdate = {};
		taaInputUpdate.descriptorSet = context.mTaaFrameTextureSet;
		taaInputUpdate.rangeIndex = 0;
		taaInputUpdate.descriptors = taaInputs;
		taaInputUpdate.descriptorNum = (uint32_t)std::size(taaInputs);
		context.mCommands.UpdateDescriptorRanges(&taaInputUpdate, 1);

		const nri::Descriptor* taaOutputs[1] = { historyOutput.storageView };
		nri::UpdateDescriptorRangeDesc taaOutputUpdate = {};
		taaOutputUpdate.descriptorSet = context.mTaaOutputSet;
		taaOutputUpdate.rangeIndex = 0;
		taaOutputUpdate.descriptors = taaOutputs;
		taaOutputUpdate.descriptorNum = (uint32_t)std::size(taaOutputs);
		context.mCommands.UpdateDescriptorRanges(&taaOutputUpdate, 1);

		context.mCommands.SetPipelineLayout(context.mTaaPipelineLayout);
		context.mCommands.SetRootConstants(&constants, sizeof(constants));
		context.mCommands.SetDescriptorSet(0, context.mTaaFrameTextureSet);
		context.mCommands.SetDescriptorSet(1, context.mTaaOutputSet);
		context.mCommands.SetPipeline(context.mPipelines.Get(NRIRenderer::PipelineSlot::Taa));
		context.mCommands.Dispatch(GetDispatchSize(context.mFrame.renderWidth), GetDispatchSize(context.mFrame.renderHeight), 1);
	}
	else if (mainKind == NRIMainUpscalerKind::Off)
	{
		context.mResources.CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLSR)
	{
		// Keep ptdebug 13 context.meaningful even when app-TAA is intentionally bypassed for vendor SR.
		context.mResources.CopyTexture(composed, historyOutput);
	}
	else if (mainKind == NRIMainUpscalerKind::DLRR)
	{
		// Keep ptdebug 13 context.meaningful for RR as well by exposing the explicit noisy RR input.
		context.mResources.CopyTexture(context.mTextures.Get(GetDlrrMainInputSlot()), historyOutput);
	}

	NRIRenderer::FrameTextureSlot resolvedInputSlot = context.mHistoryOutputSlot;

	if (mainKind != NRIMainUpscalerKind::Off)
	{
		const NRIRenderer::FrameTextureSlot vendorInputSlot =
			mainKind == NRIMainUpscalerKind::DLSR ? NRIRenderer::FrameTextureSlot::SrInput :
			GetDlrrMainInputSlot();
		NRITextureResource& vendorInput = context.mTextures.Get(vendorInputSlot);
		NRITextureResource& upscalerDepth = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UpscalerDepth);
		NRITextureResource& rrGuideDiffuseAlbedo = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo);
		NRITextureResource& rrGuideSpecularAlbedo = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo);
		NRITextureResource& rrGuideSpecularHitDistance = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance);
		NRITextureResource& rrGuideNormalRoughness = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideNormalRoughness);
		NRITextureResource& vendorOutput = context.mTextures.Get(NRIRenderer::FrameTextureSlot::VendorOutput);
		const NRIRenderer::FrameTextureSlot volumeMetaSlot = context.mSmokeService.GetVolumeSlot(true);
		NRITextureResource* volumeReactive = volumeMetaSlot != NRIRenderer::FrameTextureSlot::Count ? &context.mTextures.Get(volumeMetaSlot) : nullptr;
		if (volumeReactive != nullptr && (volumeReactive->shaderView == nullptr ||
			volumeReactive->width != context.mFrame.renderWidth || volumeReactive->height != context.mFrame.renderHeight))
			volumeReactive = nullptr;
		NRITextureResource* vendorExposure = nullptr;
		if (context.mExposure.GetSettings().enabled)
		{
			NRITextureResource& candidateExposureState = context.mExposure.GetMutableExposureStateTexture(context.mFrame.frameIndex & 1u);
			if (candidateExposureState.texture != nullptr && candidateExposureState.shaderView != nullptr)
			{
				vendorExposure = &candidateExposureState;
			}
		}

		if (!NRIPassDispatcher::DispatchUpscalerPrepass(context, mainKind))
		{
			return false;
		}

		context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(vendorInput, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(upscalerDepth, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(rrGuideDiffuseAlbedo, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(rrGuideSpecularAlbedo, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(rrGuideSpecularHitDistance, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(rrGuideNormalRoughness, NRIComputeShaderResourceState());
		if (vendorExposure != nullptr)
		{
			context.mResources.TransitionTexture(*vendorExposure, NRIComputeShaderResourceState());
		}
		if (volumeReactive != nullptr)
			context.mResources.TransitionTexture(*volumeReactive, NRIComputeShaderResourceState());
		context.mResources.TransitionTexture(vendorOutput, NRIComputeStorageState());

		const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(mainKind, context.mUpscalerService.GetSelectedUpscalerMode());
		if (!context.mUpscalerService.EnsureMainUpscaler(mainKind, resolvedUpscalerMode, context.mFrame.outputWidth, context.mFrame.outputHeight, vendorExposure != nullptr, volumeReactive != nullptr))
		{
			return false;
		}

		NRIUpscalerDispatchDesc upscalerDesc = {};
		upscalerDesc.commandBuffer = context.mCommands.GetCommandBuffer();
		upscalerDesc.input = &vendorInput;
		upscalerDesc.output = &vendorOutput;
		upscalerDesc.motion = &context.mTextures.Get(NRIRenderer::FrameTextureSlot::Motion);
		upscalerDesc.depth = &upscalerDepth;
		upscalerDesc.exposure = vendorExposure;
		upscalerDesc.normalRoughness = &rrGuideNormalRoughness;
		upscalerDesc.diffuseAlbedo = &rrGuideDiffuseAlbedo;
		upscalerDesc.specularAlbedo = &rrGuideSpecularAlbedo;
		upscalerDesc.specularHitDistance = &rrGuideSpecularHitDistance;
		upscalerDesc.reactive = volumeReactive;
		upscalerDesc.currentWidth = context.mFrame.renderWidth;
		upscalerDesc.currentHeight = context.mFrame.renderHeight;
		Copy2(context.mFrame.currentJitter.data(), upscalerDesc.cameraJitter);
		std::memcpy(upscalerDesc.viewToClipMatrix, context.mFrame.currentViewToClip.data(), sizeof(upscalerDesc.viewToClipMatrix));
		std::memcpy(upscalerDesc.worldToViewMatrix, context.mFrame.currentWorldToView.data(), sizeof(upscalerDesc.worldToViewMatrix));
		upscalerDesc.sharpness = Clamp01((float)nri_sharpness);
		upscalerDesc.resetHistory = context.mFrame.resetHistory;
		const bool mainUpscalerDispatched = context.mUpscalerService.DispatchMainUpscaler(mainKind, upscalerDesc);
		// NRI upscalers bind private descriptor pools while recording dispatches.
		// Restore Raze's pool before any subsequent pass binds our descriptor sets.
		context.mCommands.RestoreDescriptorPool();
		if (!mainUpscalerDispatched)
		{
			return false;
		}

		context.mUseUpscaledInFinal = true;
		context.mUpscaledInputSlot = NRIRenderer::FrameTextureSlot::VendorOutput;
		resolvedInputSlot = NRIRenderer::FrameTextureSlot::VendorOutput;
		context.mUpscalerService.TraceTemporalState("upscale-vendor", mainKind, postSharpenKind, runAppTaa, context.mUpscaledInputSlot, vendorSourceSlot);
	}
	else
	{
		context.mUseUpscaledInFinal = false;
		context.mUpscaledInputSlot = context.mHistoryOutputSlot;
		resolvedInputSlot = context.mHistoryOutputSlot;
		context.mUpscalerService.TraceTemporalState("upscale-native", mainKind, postSharpenKind, runAppTaa, resolvedInputSlot, context.mHistoryOutputSlot);
	}

	if (postSharpenKind == NRIPostSharpenKind::Off)
	{
		return true;
	}

	NRITextureResource& postInput = context.mTextures.Get(resolvedInputSlot);
	NRITextureResource& postOutput = context.mTextures.Get(NRIRenderer::FrameTextureSlot::PostSharpenOutput);
	context.mResources.TransitionTexture(postInput, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(postOutput, NRIComputeStorageState());
	if (!context.mUpscalerService.EnsurePostSharpen(postSharpenKind, context.mFrame.outputWidth, context.mFrame.outputHeight))
	{
		return false;
	}

	NRIUpscalerDispatchDesc postDesc = {};
	postDesc.commandBuffer = context.mCommands.GetCommandBuffer();
	postDesc.input = &postInput;
	postDesc.output = &postOutput;
	postDesc.currentWidth = postInput.width;
	postDesc.currentHeight = postInput.height;
	Copy2(context.mFrame.currentJitter.data(), postDesc.cameraJitter);
	postDesc.sharpness = Clamp01((float)nri_sharpness);
	postDesc.resetHistory = context.mFrame.resetHistory;
	const bool postSharpenDispatched = context.mUpscalerService.DispatchPostSharpen(postSharpenKind, postDesc);
	context.mCommands.RestoreDescriptorPool();
	if (!postSharpenDispatched)
	{
		return false;
	}

	context.mUseUpscaledInFinal = true;
	context.mUpscaledInputSlot = NRIRenderer::FrameTextureSlot::PostSharpenOutput;
	context.mUpscalerService.TraceTemporalState("upscale-post-sharpen", mainKind, postSharpenKind, runAppTaa, context.mUpscaledInputSlot, resolvedInputSlot);
	return true;
}



bool NRIPassDispatcher::DispatchFinal(NRIPassDispatchContext& context)
{
	NRIScopedGpuTiming gpuTiming(context.mResources.frameBuffer, NRIGpuTimingScope::Final);
	Clocker clock(NriPTFinal);

	NRITraceSceneConstants constants = {};
	const uint32_t bootstrapMode = nri_ptbootstrap ? GetBootstrapMode() : 0u;
	const bool presentRawTrace = (!nri_ptbootstrap && !context.mUseUpscaledInFinal) || bootstrapMode >= 13u;
	Copy3(context.mFrame.currentCameraPos.data(), constants.CameraPos);
	Copy3(context.mFrame.currentCameraForward.data(), constants.CameraForward);
	Copy3(context.mFrame.currentCameraRight.data(), constants.CameraRight);
	Copy3(context.mFrame.currentCameraUp.data(), constants.CameraUp);
	Copy3(context.mFrame.previousCameraPos.data(), constants.PrevCameraPos);
	Copy3(context.mFrame.previousCameraForward.data(), constants.PrevCameraForward);
	Copy3(context.mFrame.previousCameraRight.data(), constants.PrevCameraRight);
	Copy3(context.mFrame.previousCameraUp.data(), constants.PrevCameraUp);
	constants.RenderWidth = context.mFrame.renderWidth;
	constants.RenderHeight = context.mFrame.renderHeight;
	constants.DisplayWidth = context.mFrame.outputWidth;
	constants.DisplayHeight = context.mFrame.outputHeight;
	constants.TanHalfFovX = context.mFrame.currentTanHalfFovX;
	constants.TanHalfFovY = context.mFrame.currentTanHalfFovY;
	constants.PrevTanHalfFovX = context.mFrame.previousTanHalfFovX;
	constants.PrevTanHalfFovY = context.mFrame.previousTanHalfFovY;
	constants.SceneInstanceCount = context.mSceneStats.sceneInstanceCount;
	constants.StaticPrimitiveCount = context.mSceneStats.staticPrimitiveCount;
	constants.DynamicPrimitiveCount = context.mSceneStats.dynamicPrimitiveCount;
	constants.FrameIndex = context.mFrame.frameIndex;
	constants.Flags =
		(context.mFrame.resetHistory ? NRI_FLAG_RESET_HISTORY : 0u) |
		(context.mUseUpscaledInFinal ? NRI_FLAG_USE_UPSCALED : 0u) |
		(presentRawTrace ? NRI_FLAG_PRESENT_RAW_TRACE : 0u) |
		(context.mUseSplitShadowDenoiser ? NRI_FLAG_SPLIT_SHADOW_DENOISER : 0u) |
		(context.mDirectionalLightState.enabled ? NRI_FLAG_DIRECTIONAL_LIGHT : 0u) |
		(context.mDirectionalLightState.enabled && context.mDirectionalLightState.shadow ? NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW : 0u);
	constants.StaticMaterialCount = context.mSceneStats.staticMaterialCount;
	constants.DebugMode = GetEffectivePtDebugMode();
	constants.BootstrapMode = bootstrapMode;
	constants.DynamicMaterialCount = context.mSceneStats.dynamicMaterialCount;
	constants.BounceCounts = PackTraceBounceCounts(0u, 0u, context.mDirectionalLightState.color);
	constants.RuntimeLightCount = context.mSceneStats.runtimeLightCount;
	constants.ReservedTrace0 = (uint16_t)(int16_t)context.mFrame.sceneLeft | ((uint32_t)(uint16_t)(int16_t)context.mFrame.sceneTop << 16);
	constants.ReservedTrace1 = PackDenoiserAux1(0u, context.mDirectionalLightState.angularSize);
	Copy3(context.mFrame.skyColor.data(), constants.SkyColor);
	Copy3(context.mFrame.groundColor.data(), constants.GroundColor);
	ApplyDirectionalLightStateToConstants(context.mDirectionalLightState, constants);

	NRITextureResource& history = context.mTextures.Get(context.mHistoryOutputSlot);
	NRITextureResource& upscaled = context.mTextures.Get(context.mUpscaledInputSlot);
	NRITextureResource& final = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Final);
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Motion), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::ViewZ), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::NormalRoughness), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::BaseColorMetalness), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredSpecular), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::DenoisedShadow), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::DirectLighting), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::DirectEmission), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Composed), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Validation), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideSpecularHitDistance), NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(history, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(upscaled, NRIComputeShaderResourceState());
	context.mResources.TransitionTexture(final, NRIComputeStorageState());

	context.mFrameInputDescriptors.fill(context.mTextures.Get(NRIRenderer::FrameTextureSlot::Composed).shaderView);
	context.mFrameInputDescriptors[0] = history.shaderView;
	context.mFrameInputDescriptors[1] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Motion).shaderView;
	context.mFrameInputDescriptors[2] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::ViewZ).shaderView;
	context.mFrameInputDescriptors[3] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::NormalRoughness).shaderView;
	context.mFrameInputDescriptors[4] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::BaseColorMetalness).shaderView;
	context.mFrameInputDescriptors[5] = presentRawTrace ? (context.mUseUpscaledInFinal ? upscaled.shaderView : context.mTextures.Get(NRIRenderer::FrameTextureSlot::Composed).shaderView) : context.mTextures.Get(NRIRenderer::FrameTextureSlot::Composed).shaderView;
	context.mFrameInputDescriptors[6] = upscaled.shaderView;
	context.mFrameInputDescriptors[7] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::Validation).shaderView;
	context.mFrameInputDescriptors[8] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideDiffuseAlbedo).shaderView;
	context.mFrameInputDescriptors[9] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::RrGuideSpecularAlbedo).shaderView;
	context.mFrameInputDescriptors[10] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredPenumbra).shaderView;
	context.mFrameInputDescriptors[11] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::DenoisedShadow).shaderView;
	context.mFrameInputDescriptors[12] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::DirectLighting).shaderView;
	context.mFrameInputDescriptors[13] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::DirectEmission).shaderView;
	if (constants.DebugMode == 10)
	{
		context.mFrameInputDescriptors[5] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredDiffuse).shaderView;
	}
	else if (constants.DebugMode == 11)
	{
		context.mFrameInputDescriptors[5] = context.mTextures.Get(NRIRenderer::FrameTextureSlot::UnfilteredSpecular).shaderView;
	}
	context.mDescriptors.UpdateFrameTextureSet();

	context.mOutputDescriptors.fill(final.storageView);
	context.mOutputDescriptors[2] = final.storageView;
	context.mDescriptors.UpdateOutputSet();

	context.mCommands.SetPipelineLayout(context.mPipelineLayout);
	context.mCommands.SetRootConstants(&constants, sizeof(constants));
	if (!context.mSceneBinding.BindSceneRootDescriptors())
	{
		return false;
	}
	context.mCommands.SetDescriptorSet(0, context.mSamplerSet);
	context.mCommands.SetDescriptorSet(1, context.mSceneBinding.GetCurrentSceneTextureSet());
	context.mCommands.SetDescriptorSet(2, context.mSceneBinding.GetCurrentSceneDataSet());
	context.mCommands.SetDescriptorSet(3, context.mFrameTextureSet);
	context.mCommands.SetDescriptorSet(4, context.mOutputSet);
	context.mCommands.SetPipeline(context.mPipelines.Get(NRIRenderer::PipelineSlot::Final));
	context.mCommands.Dispatch(GetDispatchSize(context.mFrame.targetWidth), GetDispatchSize(context.mFrame.targetHeight), 1);
	return true;
}
