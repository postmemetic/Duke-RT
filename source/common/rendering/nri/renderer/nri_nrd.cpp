#include "nri_nrd.h"

#include "printf.h"
#include "NRDIntegration.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	constexpr float NRI_NRD_DEFAULT_FRAME_TIME_MS = 1000.0f / 60.0f;
	constexpr float NRI_NRD_MAX_TEMPORAL_FRAME_TIME_MS = 1000.0f / 30.0f;

	const nrd::DenoiserDesc gDenoisers[] = {
		{ nrd::Identifier(1), nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR },
		{ nrd::Identifier(2), nrd::Denoiser::RELAX_DIFFUSE_SPECULAR },
		{ nrd::Identifier(3), nrd::Denoiser::SIGMA_SHADOW }
	};

	static bool UseRelax(NRINrdDenoiserMode mode)
	{
		return mode == NRINrdDenoiserMode::Relax;
	}

	static float ResolveNrdTemporalFrameTimeMs(float observedFrameTimeMs)
	{
		if (!std::isfinite(observedFrameTimeMs) || observedFrameTimeMs <= 0.0f)
		{
			return NRI_NRD_DEFAULT_FRAME_TIME_MS;
		}

		// CPU-side asset admission and shader-link stalls are not changes in scene motion.
		// Keep them from becoming a one-frame RELAX temporal-policy discontinuity.
		return std::min(observedFrameTimeMs, NRI_NRD_MAX_TEMPORAL_FRAME_TIME_MS);
	}

	static nrd::HitDistanceReconstructionMode ClampHitDistanceReconstructionMode(uint32_t value)
	{
		switch (value)
		{
		case 1: return nrd::HitDistanceReconstructionMode::AREA_3X3;
		case 2: return nrd::HitDistanceReconstructionMode::AREA_5X5;
		default: return nrd::HitDistanceReconstructionMode::OFF;
		}
	}

	static nrd::ReblurSettings BuildReblurSettings(const NRINrdDispatchDesc& desc)
	{
		nrd::ReblurSettings settings = {};
		settings.maxAccumulatedFrameNum = desc.maxAccumulatedFrameNum;
		settings.maxFastAccumulatedFrameNum = std::min(desc.maxFastAccumulatedFrameNum, settings.maxAccumulatedFrameNum);
		settings.maxStabilizedFrameNum = std::min(desc.maxStabilizedFrameNum, settings.maxAccumulatedFrameNum);
		settings.hitDistanceReconstructionMode = ClampHitDistanceReconstructionMode(desc.hitDistanceReconstructionMode);
		settings.enableAntiFirefly = desc.enableAntiFirefly;
		settings.fastHistoryClampingSigmaScale = desc.fastHistoryClampingSigmaScale;
		settings.diffusePrepassBlurRadius = desc.diffusePrepassBlurRadius;
		settings.specularPrepassBlurRadius = desc.specularPrepassBlurRadius;
		settings.minBlurRadius = desc.minBlurRadius;
		settings.maxBlurRadius = std::max(desc.maxBlurRadius, settings.minBlurRadius);

		// Keep the sample-style material floors, but expose the dominant spatial controls live
		// because current Raze gameplay still denoises a mixed direct+indirect signal.
		settings.minMaterialForDiffuse = 1.0f;
		settings.minMaterialForSpecular = 2.0f;
		settings.usePrepassOnlyForSpecularMotionEstimation =
			desc.indirectSamplingMode == 0u && settings.specularPrepassBlurRadius > 0.0f;

		return settings;
	}

	static nrd::RelaxSettings BuildRelaxSettings(const NRINrdDispatchDesc& desc)
	{
		nrd::RelaxSettings settings = {};
		settings.diffuseMaxAccumulatedFrameNum = desc.maxAccumulatedFrameNum;
		settings.specularMaxAccumulatedFrameNum = desc.maxAccumulatedFrameNum;
		settings.diffuseMaxFastAccumulatedFrameNum = std::min(desc.maxFastAccumulatedFrameNum, settings.diffuseMaxAccumulatedFrameNum);
		settings.specularMaxFastAccumulatedFrameNum = std::min(desc.maxFastAccumulatedFrameNum, settings.specularMaxAccumulatedFrameNum);
		settings.hitDistanceReconstructionMode = ClampHitDistanceReconstructionMode(desc.hitDistanceReconstructionMode);
		settings.enableAntiFirefly = desc.enableAntiFirefly;
		settings.fastHistoryClampingSigmaScale = desc.fastHistoryClampingSigmaScale;
		settings.diffusePrepassBlurRadius = desc.diffusePrepassBlurRadius;
		settings.specularPrepassBlurRadius = desc.specularPrepassBlurRadius;
		settings.minMaterialForDiffuse = 1.0f;
		settings.minMaterialForSpecular = 2.0f;
		return settings;
	}

	static nrd::SigmaSettings BuildSigmaSettings(const NRINrdDispatchDesc& desc)
	{
		nrd::SigmaSettings settings = {};
		settings.maxStabilizedFrameNum = std::min(desc.sigmaMaxStabilizedFrameNum, nrd::SIGMA_MAX_HISTORY_FRAME_NUM);
		settings.planeDistanceSensitivity = desc.sigmaPlaneDistanceSensitivity;
		settings.lightDirection[0] = desc.lightDirection[0];
		settings.lightDirection[1] = desc.lightDirection[1];
		settings.lightDirection[2] = desc.lightDirection[2];
		return settings;
	}

	static bool ReblurSettingsEqual(const nrd::ReblurSettings& a, const nrd::ReblurSettings& b)
	{
		return
			a.maxAccumulatedFrameNum == b.maxAccumulatedFrameNum &&
			a.maxFastAccumulatedFrameNum == b.maxFastAccumulatedFrameNum &&
			a.maxStabilizedFrameNum == b.maxStabilizedFrameNum &&
			a.hitDistanceReconstructionMode == b.hitDistanceReconstructionMode &&
			a.enableAntiFirefly == b.enableAntiFirefly &&
			a.fastHistoryClampingSigmaScale == b.fastHistoryClampingSigmaScale &&
			a.diffusePrepassBlurRadius == b.diffusePrepassBlurRadius &&
			a.specularPrepassBlurRadius == b.specularPrepassBlurRadius &&
			a.minBlurRadius == b.minBlurRadius &&
			a.maxBlurRadius == b.maxBlurRadius &&
			a.minMaterialForDiffuse == b.minMaterialForDiffuse &&
			a.minMaterialForSpecular == b.minMaterialForSpecular &&
			a.usePrepassOnlyForSpecularMotionEstimation == b.usePrepassOnlyForSpecularMotionEstimation;
	}

	static bool RelaxSettingsEqual(const nrd::RelaxSettings& a, const nrd::RelaxSettings& b)
	{
		return
			a.diffuseMaxAccumulatedFrameNum == b.diffuseMaxAccumulatedFrameNum &&
			a.specularMaxAccumulatedFrameNum == b.specularMaxAccumulatedFrameNum &&
			a.diffuseMaxFastAccumulatedFrameNum == b.diffuseMaxFastAccumulatedFrameNum &&
			a.specularMaxFastAccumulatedFrameNum == b.specularMaxFastAccumulatedFrameNum &&
			a.hitDistanceReconstructionMode == b.hitDistanceReconstructionMode &&
			a.enableAntiFirefly == b.enableAntiFirefly &&
			a.fastHistoryClampingSigmaScale == b.fastHistoryClampingSigmaScale &&
			a.diffusePrepassBlurRadius == b.diffusePrepassBlurRadius &&
			a.specularPrepassBlurRadius == b.specularPrepassBlurRadius &&
			a.minMaterialForDiffuse == b.minMaterialForDiffuse &&
			a.minMaterialForSpecular == b.minMaterialForSpecular;
	}

	static bool SigmaSettingsEqual(const nrd::SigmaSettings& a, const nrd::SigmaSettings& b)
	{
		return
			a.maxStabilizedFrameNum == b.maxStabilizedFrameNum &&
			a.planeDistanceSensitivity == b.planeDistanceSensitivity &&
			a.lightDirection[0] == b.lightDirection[0] &&
			a.lightDirection[1] == b.lightDirection[1] &&
			a.lightDirection[2] == b.lightDirection[2];
	}
}

nrd::Resource NRINrdContext::MakeResource(NRITextureResource& texture)
{
	nrd::Resource resource = {};
	resource.nri.texture = texture.texture;
	resource.state = texture.state;
	resource.userArg = &texture;
	return resource;
}

bool NRINrdContext::EnsureReady(nri::Device& device, uint32_t width, uint32_t height, uint8_t queuedFrameNum)
{
	if (mInitialized && mWidth == width && mHeight == height && mQueuedFrameNum == queuedFrameNum)
	{
		return true;
	}

	Shutdown();

	nrd::InstanceCreationDesc instanceCreationDesc = {};
	instanceCreationDesc.denoisers = gDenoisers;
	instanceCreationDesc.denoisersNum = (uint32_t)std::size(gDenoisers);

	nrd::IntegrationCreationDesc integrationDesc = {};
	static_assert(sizeof(integrationDesc.name) >= sizeof("RazeNRI"));
	memcpy(integrationDesc.name, "RazeNRI", sizeof("RazeNRI"));
	integrationDesc.resourceWidth = (uint16_t)width;
	integrationDesc.resourceHeight = (uint16_t)height;
	integrationDesc.queuedFrameNum = queuedFrameNum;
	integrationDesc.autoWaitForIdle = false;

	if (mIntegration.Recreate(integrationDesc, instanceCreationDesc, &device) != nrd::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "NRI NRD integration creation failed.\n");
		return false;
	}

	mReblurDenoiser = gDenoisers[0].identifier;
	mRelaxDenoiser = gDenoisers[1].identifier;
	mSigmaDenoiser = gDenoisers[2].identifier;
	mWidth = width;
	mHeight = height;
	mQueuedFrameNum = queuedFrameNum;
	mInitialized = true;
	mHasReblurSettings = false;
	mHasRelaxSettings = false;
	mHasSigmaSettings = false;
	mHasIndirectSamplingMode = false;
	mLastDenoiserMode = NRINrdDenoiserMode::Reblur;
	mLastIndirectSamplingMode = 0;
	return true;
}

void NRINrdContext::NewFrame()
{
	if (mInitialized)
	{
		mIntegration.NewFrame();
	}
}

bool NRINrdContext::Denoise(const NRINrdDispatchDesc& desc)
{
	if (!mInitialized || desc.commandBuffer == nullptr || desc.motion == nullptr || desc.viewZ == nullptr ||
		desc.normalRoughness == nullptr || desc.baseColorMetalness == nullptr ||
		desc.unfilteredDiffuse == nullptr || desc.unfilteredSpecular == nullptr ||
		desc.diffuse == nullptr || desc.specular == nullptr || desc.validation == nullptr)
	{
		return false;
	}
	if (desc.enableSigmaShadow && (desc.unfilteredPenumbra == nullptr || desc.shadow == nullptr))
	{
		return false;
	}

	const bool useRelax = UseRelax(desc.denoiserMode);
	const bool denoiserChanged = desc.denoiserMode != mLastDenoiserMode;
	const bool samplingPolicyChanged =
		!mHasIndirectSamplingMode ||
		desc.indirectSamplingMode != mLastIndirectSamplingMode;
	bool settingsChanged = false;
	nrd::Identifier activeDenoiser = useRelax ? mRelaxDenoiser : mReblurDenoiser;
	bool sigmaSettingsChanged = false;

	if (desc.enableSigmaShadow)
	{
		const nrd::SigmaSettings sigmaSettings = BuildSigmaSettings(desc);
		sigmaSettingsChanged =
			!mHasSigmaSettings ||
			!SigmaSettingsEqual(mSigmaSettings, sigmaSettings);

		// Match the sample more closely here: SIGMA settings are pushed every frame,
		// while history restart is still keyed off the explicit field comparison above.
		if (mIntegration.SetDenoiserSettings(mSigmaDenoiser, &sigmaSettings) != nrd::Result::SUCCESS)
		{
			return false;
		}

		mSigmaSettings = sigmaSettings;
		mHasSigmaSettings = true;
	}

	if (useRelax)
	{
		const nrd::RelaxSettings relaxSettings = BuildRelaxSettings(desc);
		settingsChanged =
			!mHasRelaxSettings ||
			!RelaxSettingsEqual(mRelaxSettings, relaxSettings);

		if (settingsChanged)
		{
			if (mIntegration.SetDenoiserSettings(activeDenoiser, &relaxSettings) != nrd::Result::SUCCESS)
			{
				return false;
			}

			mRelaxSettings = relaxSettings;
			mHasRelaxSettings = true;
		}
	}
	else
	{
		const nrd::ReblurSettings reblurSettings = BuildReblurSettings(desc);
		settingsChanged =
			!mHasReblurSettings ||
			!ReblurSettingsEqual(mReblurSettings, reblurSettings);

		if (settingsChanged)
		{
			if (mIntegration.SetDenoiserSettings(activeDenoiser, &reblurSettings) != nrd::Result::SUCCESS)
			{
				return false;
			}

			mReblurSettings = reblurSettings;
			mHasReblurSettings = true;
		}
	}

	nrd::CommonSettings commonSettings = {};
	std::memcpy(commonSettings.viewToClipMatrix, desc.viewToClipMatrix, sizeof(commonSettings.viewToClipMatrix));
	std::memcpy(commonSettings.viewToClipMatrixPrev, desc.viewToClipMatrixPrev, sizeof(commonSettings.viewToClipMatrixPrev));
	std::memcpy(commonSettings.worldToViewMatrix, desc.worldToViewMatrix, sizeof(commonSettings.worldToViewMatrix));
	std::memcpy(commonSettings.worldToViewMatrixPrev, desc.worldToViewMatrixPrev, sizeof(commonSettings.worldToViewMatrixPrev));
	// Raze feeds NRD the sample-shaped 2.5D screen-space contract:
	// - motion.xy written in pixel units by TraceOpaque
	// - motion.z written as viewZPrev - viewZ
	// NRD converts xy back to normalized screen space through motionVectorScale.
	commonSettings.motionVectorScale[0] = 1.0f / (float)desc.resourceWidth;
	commonSettings.motionVectorScale[1] = 1.0f / (float)desc.resourceHeight;
	commonSettings.motionVectorScale[2] = 1.0f;
	commonSettings.cameraJitter[0] = desc.cameraJitter[0];
	commonSettings.cameraJitter[1] = desc.cameraJitter[1];
	commonSettings.cameraJitterPrev[0] = desc.cameraJitterPrev[0];
	commonSettings.cameraJitterPrev[1] = desc.cameraJitterPrev[1];
	commonSettings.resourceSize[0] = (uint16_t)desc.resourceWidth;
	commonSettings.resourceSize[1] = (uint16_t)desc.resourceHeight;
	commonSettings.resourceSizePrev[0] = (uint16_t)desc.resourceWidth;
	commonSettings.resourceSizePrev[1] = (uint16_t)desc.resourceHeight;
	commonSettings.rectSize[0] = (uint16_t)desc.resourceWidth;
	commonSettings.rectSize[1] = (uint16_t)desc.resourceHeight;
	commonSettings.rectSizePrev[0] = (uint16_t)desc.resourceWidth;
	commonSettings.rectSizePrev[1] = (uint16_t)desc.resourceHeight;
	commonSettings.viewZScale = 1.0f;
	commonSettings.denoisingRange = 100000.0f;
	commonSettings.disocclusionThreshold = 0.01f;
	commonSettings.disocclusionThresholdAlternate = 0.05f;
	commonSettings.frameIndex = desc.frameIndex;
	const float temporalFrameTimeMs = ResolveNrdTemporalFrameTimeMs(desc.observedFrameTimeMs);
	commonSettings.timeDeltaBetweenFrames = temporalFrameTimeMs;
	commonSettings.accumulationMode = (desc.resetHistory || denoiserChanged || settingsChanged || sigmaSettingsChanged || samplingPolicyChanged) ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
	// Camera motion is already represented by the matrices above, so motion vectors stay in screen space here.
	commonSettings.isMotionVectorInWorldSpace = false;
	// REBLUR can use base-color/metalness to patch motion vectors during stabilization.
	// After enabling virtual miss motion this started pulling background/specular motion into
	// ordinary surface history in visible edge/emissive cases, so keep the optional path off
	// for REBLUR until we have a more deliberate motion-patching policy.
	commonSettings.isBaseColorMetalnessAvailable = useRelax;
	commonSettings.enableValidation = desc.enableValidation;
	if (desc.traceTemporalInput)
	{
		const float frameRateScale = useRelax ?
			std::clamp(16.66f / temporalFrameTimeMs, 0.25f, 4.0f) :
			std::max(33.333f / temporalFrameTimeMs, 1.0f);
		Printf("PERF pt nrd temporal NRI: frame=%u mode=%s indirect_mode=%u observed_frame_ms=%.3f effective_frame_ms=%.3f frame_rate_scale=%.3f renderer_queued_frames=%u integration_queued_frames=%u accumulation=%s reset_explicit=%u reset_mode=%u reset_settings=%u reset_sigma=%u reset_indirect=%u\n",
			desc.frameIndex,
			useRelax ? "relax" : "reblur",
			desc.indirectSamplingMode,
			desc.observedFrameTimeMs,
			temporalFrameTimeMs,
			frameRateScale,
			(unsigned)desc.queuedFrameNum,
			(unsigned)mQueuedFrameNum,
			commonSettings.accumulationMode == nrd::AccumulationMode::CONTINUE ? "continue" : "clear-and-restart",
			desc.resetHistory ? 1u : 0u,
			denoiserChanged ? 1u : 0u,
			settingsChanged ? 1u : 0u,
			sigmaSettingsChanged ? 1u : 0u,
			samplingPolicyChanged ? 1u : 0u);
	}

	if (mIntegration.SetCommonSettings(commonSettings) != nrd::Result::SUCCESS)
	{
		return false;
	}
	mLastDenoiserMode = desc.denoiserMode;
	mLastIndirectSamplingMode = desc.indirectSamplingMode;
	mHasIndirectSamplingMode = true;

	nrd::ResourceSnapshot resourceSnapshot = {};
	resourceSnapshot.restoreInitialState = false;
	resourceSnapshot.SetResource(nrd::ResourceType::IN_MV, MakeResource(*desc.motion));
	resourceSnapshot.SetResource(nrd::ResourceType::IN_VIEWZ, MakeResource(*desc.viewZ));
	resourceSnapshot.SetResource(nrd::ResourceType::IN_NORMAL_ROUGHNESS, MakeResource(*desc.normalRoughness));
	resourceSnapshot.SetResource(nrd::ResourceType::IN_BASECOLOR_METALNESS, MakeResource(*desc.baseColorMetalness));
	resourceSnapshot.SetResource(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, MakeResource(*desc.unfilteredDiffuse));
	resourceSnapshot.SetResource(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, MakeResource(*desc.unfilteredSpecular));
	resourceSnapshot.SetResource(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, MakeResource(*desc.diffuse));
	resourceSnapshot.SetResource(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, MakeResource(*desc.specular));
	resourceSnapshot.SetResource(nrd::ResourceType::OUT_VALIDATION, MakeResource(*desc.validation));
	if (desc.enableSigmaShadow)
	{
		resourceSnapshot.SetResource(nrd::ResourceType::IN_PENUMBRA, MakeResource(*desc.unfilteredPenumbra));
		resourceSnapshot.SetResource(nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY, MakeResource(*desc.shadow));
		const nrd::Identifier denoisers[] = { mSigmaDenoiser, activeDenoiser };
		mIntegration.Denoise(denoisers, 2, *desc.commandBuffer, resourceSnapshot);
	}
	else
	{
		mIntegration.Denoise(&activeDenoiser, 1, *desc.commandBuffer, resourceSnapshot);
	}

	for (size_t i = 0; i < resourceSnapshot.uniqueNum; ++i)
	{
		auto* texture = static_cast<NRITextureResource*>(resourceSnapshot.unique[i].userArg);
		if (texture != nullptr)
		{
			texture->state = resourceSnapshot.unique[i].state;
		}
	}

	return true;
}

void NRINrdContext::Shutdown()
{
	if (!mInitialized)
	{
		return;
	}

	mIntegration.Destroy();
	mInitialized = false;
	mWidth = 0;
	mHeight = 0;
	mQueuedFrameNum = 0;
	mReblurDenoiser = 0;
	mRelaxDenoiser = 0;
	mSigmaDenoiser = 0;
	mReblurSettings = {};
	mRelaxSettings = {};
	mSigmaSettings = {};
	mHasReblurSettings = false;
	mHasRelaxSettings = false;
	mHasSigmaSettings = false;
	mHasIndirectSamplingMode = false;
	mLastDenoiserMode = NRINrdDenoiserMode::Reblur;
	mLastIndirectSamplingMode = 0;
}
