#include "nri_pass_dispatch.h"

#include "nri_cvars.h"
#include "nri_descriptor_sets.h"
#include "nri_pipeline_state.h"
#include "nri_scene_upload.h"
#include "nri_smoke.h"
#include "../system/nri_renderdevice.h"
#include "v_video.h"

#include <algorithm>

NRIPassDispatchContext NRIRenderer::BuildPassDispatchContext(bool mainViewEligible)
{
	NRIPassDispatchContext::Init init = {};
	NRIRenderDevice* const frameBuffer = mFrameBuffer;

	auto buildTextureService = [&]()
	{
		NRIPassDispatchContext::TextureService service = {};
		service.user = this;
		service.getFrameTexture = [](void* user, FrameTextureSlot slot) -> NRITextureResource&
		{
			return static_cast<NRIRenderer*>(user)->GetFrameTexture(slot);
		};
		return service;
	};

	auto buildPipelineService = [&]()
	{
		NRIPassDispatchContext::PipelineService service = {};
		service.user = this;
		service.getPipeline = [](void* user, PipelineSlot slot) -> nri::Pipeline*
		{
			return static_cast<NRIRenderer*>(user)->GetPipeline(slot);
		};
		service.ensureIndirectRadianceCachePipeline = [](void* user) -> bool
		{
			return NRIPipelineStateManager::EnsureIndirectRadianceCachePipeline(*static_cast<NRIRenderer*>(user));
		};
		service.getIndirectRadianceCachePipelineLayout = [](void* user) -> nri::PipelineLayout*
		{
			return static_cast<NRIRenderer*>(user)->mIndirectRadianceCachePipelineLayout;
		};
		return service;
	};

	auto buildDescriptorService = [&]()
	{
		NRIPassDispatchContext::DescriptorService service = {};
		service.user = this;
		service.updateFrameTextureSet = [](void* user) -> bool
		{
			return NRIDescriptorSetManager::UpdateFrameTextureSet(*static_cast<NRIRenderer*>(user));
		};
		service.updateFrameTextureSetWithDescriptors = [](void* user, nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, NRI_INPUT_DESCRIPTOR_NUM>& descriptors) -> bool
		{
			return NRIDescriptorSetManager::UpdateFrameTextureSet(*static_cast<NRIRenderer*>(user), descriptorSet, descriptors);
		};
		service.updateOutputSet = [](void* user) -> bool
		{
			return NRIDescriptorSetManager::UpdateOutputSet(*static_cast<NRIRenderer*>(user));
		};
		service.updateOutputSetWithDescriptors = [](void* user, nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, NRI_OUTPUT_DESCRIPTOR_NUM>& descriptors) -> bool
		{
			return NRIDescriptorSetManager::UpdateOutputSet(*static_cast<NRIRenderer*>(user), descriptorSet, descriptors);
		};
		return service;
	};

	auto buildResourceService = [&]()
	{
		NRIPassDispatchContext::ResourceService service = {};
		service.user = this;
		service.frameBuffer = frameBuffer;
		service.core = frameBuffer != nullptr ? frameBuffer->GetCoreInterface() : nullptr;
		service.device = frameBuffer != nullptr ? frameBuffer->GetDevice() : nullptr;
		service.commandBuffer = frameBuffer != nullptr ? frameBuffer->GetCurrentCommandBuffer() : nullptr;
		service.buildResourceServices = [](void* user) -> NRIResourceServices
		{
			return static_cast<NRIRenderer*>(user)->BuildResourceServices();
		};
		service.updateReprojectionBuffer = [](void* user) -> bool
		{
			return NRISceneUploadManager::UpdateReprojectionBuffer(*static_cast<NRIRenderer*>(user), nullptr);
		};
		service.getOutputPolicy = [](void* user) -> NRIPTOutputPolicy
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mFrameBuffer->GetPathTracingOutputPolicy();
		};
		service.copyFinalToActiveTarget = [](void* user)
		{
			static_cast<NRIRenderer*>(user)->CopyFinalToActiveTarget();
		};
		service.copyTexture = [](void* user, NRITextureResource& source, NRITextureResource& destination)
		{
			static_cast<NRIRenderer*>(user)->CopyTexture(source, destination);
		};
		service.transitionTexture = [](void* user, NRITextureResource& texture, nri::AccessLayoutStage after)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			renderer->mFrameBuffer->TransitionTexture(texture, after);
		};
		return service;
	};

	auto buildCommandService = [&]()
	{
		NRIPassDispatchContext::CommandService service = {};
		service.core = frameBuffer != nullptr ? frameBuffer->GetCoreInterface() : nullptr;
		service.commandBuffer = frameBuffer != nullptr ? frameBuffer->GetCurrentCommandBuffer() : nullptr;
		service.descriptorPool = frameBuffer != nullptr ? frameBuffer->mDescriptorPool : nullptr;
		return service;
	};

	auto buildSceneBindingService = [&]()
	{
		NRIPassDispatchContext::SceneBindingService service = {};
		service.user = this;
		service.getCurrentSceneTextureSet = [](void* user) -> nri::DescriptorSet*
		{
			return static_cast<NRIRenderer*>(user)->GetCurrentSceneTextureSet();
		};
		service.getCurrentSceneDataSet = [](void* user) -> nri::DescriptorSet*
		{
			return static_cast<NRIRenderer*>(user)->GetCurrentSceneDataSet();
		};
		service.bindSceneRootDescriptors = [](void* user) -> bool
		{
			return static_cast<NRIRenderer*>(user)->BindSceneRootDescriptors();
		};
		return service;
	};

	auto buildExposureService = [&]()
	{
		NRIPassDispatchContext::ExposureService service = {};
		service.user = this;
		service.readbackAutoExposureStats = [](void* user)
		{
			static_cast<NRIRenderer*>(user)->ReadbackAutoExposureStats();
		};
		service.ensureAutoExposureResources = [](void* user, const NRIAutoExposureSettings& settings) -> bool
		{
			return static_cast<NRIRenderer*>(user)->EnsureAutoExposureResources(settings);
		};
		service.requestAutoExposureReset = [](void* user, const char* reason)
		{
			static_cast<NRIRenderer*>(user)->RequestAutoExposureReset(reason);
		};
		service.dispatchAutoExposure = [](void* user, FrameTextureSlot sourceSlot) -> bool
		{
			return static_cast<NRIRenderer*>(user)->DispatchAutoExposure(sourceSlot);
		};
		service.resolveExposureRoute = [](void* user, FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) -> ExposureRoute
		{
			return static_cast<NRIRenderer*>(user)->ResolveExposureRoute(inputSlot, outputPolicy, mainKind, postSharpenKind);
		};
		return service;
	};

	auto buildUpscalerService = [&]()
	{
		NRIPassDispatchContext::UpscalerService service = {};
		service.user = this;
		service.resolveMainUpscalerKind = [](void* user, bool logFallback) -> NRIMainUpscalerKind
		{
			return static_cast<NRIRenderer*>(user)->ResolveMainUpscalerKind(logFallback);
		};
		service.resolvePostSharpenKind = [](void* user, bool logFallback) -> NRIPostSharpenKind
		{
			return static_cast<NRIRenderer*>(user)->ResolvePostSharpenKind(logFallback);
		};
		service.getSelectedUpscalerMode = [](void* user) -> nri::UpscalerMode
		{
			return static_cast<NRIRenderer*>(user)->GetSelectedUpscalerMode();
		};
		service.shouldRunAppTaaForFrameGraph = [](void* user, NRIMainUpscalerKind kind) -> bool
		{
			return static_cast<NRIRenderer*>(user)->ShouldRunAppTaaForFrameGraph(kind);
		};
		service.traceTemporalState = [](void* user, const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot)
		{
			static_cast<NRIRenderer*>(user)->TraceTemporalState(stage, resolvedMainUpscaler, resolvedPostSharpen, runAppTaa, primarySlot, secondarySlot);
		};
		service.ensureMainUpscaler = [](void* user, NRIMainUpscalerKind kind, nri::UpscalerMode mode, uint32_t outputWidth, uint32_t outputHeight, bool exposure, bool reactive) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mUpscaler.EnsureMainUpscaler(*renderer->mFrameBuffer, kind, mode, outputWidth, outputHeight, exposure, reactive);
		};
		service.dispatchMainUpscaler = [](void* user, NRIMainUpscalerKind kind, const NRIUpscalerDispatchDesc& desc) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mUpscaler.DispatchMainUpscaler(*renderer->mFrameBuffer, kind, desc);
		};
		service.ensurePostSharpen = [](void* user, NRIPostSharpenKind kind, uint32_t outputWidth, uint32_t outputHeight) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mUpscaler.EnsurePostSharpen(*renderer->mFrameBuffer, kind, outputWidth, outputHeight);
		};
		service.dispatchPostSharpen = [](void* user, NRIPostSharpenKind kind, const NRIUpscalerDispatchDesc& desc) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mUpscaler.DispatchPostSharpen(*renderer->mFrameBuffer, kind, desc);
		};
		return service;
	};

	auto buildSelfTestService = [&]()
	{
		NRIPassDispatchContext::SelfTestService service = {};
		service.user = this;
		service.resetSelfTestRouteSnapshot = [](void* user)
		{
			static_cast<NRIRenderer*>(user)->ResetSelfTestRouteSnapshot();
		};
		service.setSelfTestRouteSnapshot = [](void* user, const char* routeName, const char* presenterName, const char* ownerName, const char* passListName, bool denoiserRun, bool upscalerRun, bool exposureRun)
		{
			static_cast<NRIRenderer*>(user)->SetSelfTestRouteSnapshot(routeName, presenterName, ownerName, passListName, denoiserRun, upscalerRun, exposureRun);
		};
		return service;
	};

	auto buildSmokeService = [&]()
	{
		NRIPassDispatchContext::SmokeService service = {};
		service.user = this;
		service.weaponEvents = &mWeaponEventBatch.Events();
		service.prepareFrame = [](void* user, bool eligible, const TArray<PathTracingWeaponLightEvent>& weaponEvents) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mSmoke == nullptr || renderer->mSmoke->PrepareFrame(*renderer, eligible, weaponEvents);
		};
		service.dispatchRoute = [](void* user, const NRISmokeRouteDesc& route) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mSmoke == nullptr || renderer->mSmoke->DispatchRoute(*renderer, route);
		};
		service.getVolumeSlot = [](void* user, bool metadata) -> uint32_t
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			if (renderer->mSmoke == nullptr)
				return (uint32_t)NRIRenderer::FrameTextureSlot::Count;
			const NRISmokeStatusSnapshot& status = renderer->mSmoke->GetStatusSnapshot();
			if (!status.enabled || !status.mainViewEligible || !status.routeSupported || status.dispatchedFrame != renderer->mFrameIndex)
				return (uint32_t)NRIRenderer::FrameTextureSlot::Count;
			return metadata ? status.volumeMetaSlot : status.volumeResolvedSlot;
		};
		return service;
	};

	auto buildIndirectRadianceCacheService = [&]()
	{
		NRIPassDispatchContext::IndirectRadianceCacheService service = {};
		service.user = this;
		service.prepare = [](void* user, bool enabled) -> NRIIndirectRadianceCachePrepareResult
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			if (!enabled)
			{
				return renderer->mIndirectRadianceCache.Prepare(
					BuildNRIIndirectRadianceCacheServices(*renderer), false, {});
			}
			if (!renderer->mTraceShaderStats.Ensure(renderer->BuildResourceServices()) ||
				!NRIPipelineStateManager::EnsureIndirectRadianceCachePipeline(*renderer))
			{
				return {};
			}

			auto combine = [](uint64_t hash, uint64_t value) -> uint64_t
			{
				return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u));
			};
			NRIIndirectRadianceCacheCompatibilityInput compatibility = {};
			compatibility.valid =
				renderer->mMapWorld.valid &&
				renderer->mStaticMapScene.valid &&
				renderer->mSceneInstancePayloadCacheValid &&
				renderer->mLastWorldTlasInstancePayloadHash != 0;
			compatibility.mapIdentity = renderer->mMapWorld.valid ? renderer->mMapWorld.buildSerial : 0;
			compatibility.staticSceneIdentity = combine(
				combine(
					combine(renderer->mStaticVertexBuffer.payloadHash, renderer->mStaticIndexBuffer.payloadHash),
					renderer->mStaticPrimitiveBuffer.payloadHash),
				renderer->mStaticMapScene.buildSerial);
			compatibility.portalRouteIdentity = combine(renderer->mPortalPayloadHash, renderer->mSkyEnvironment.ActiveKey());
			compatibility.materialIdentity = combine(
				renderer->mStaticMaterialBuffer.payloadHash,
				renderer->mMaterialBuffer.payloadHash);
			compatibility.mutationIdentity = combine(
				combine(
					combine(renderer->mVertexBuffer.payloadHash, renderer->mIndexBuffer.payloadHash),
					renderer->mPrimitiveBuffer.payloadHash),
				renderer->mSceneInstancePayloadHash);
			compatibility.voxelOccurrenceIdentity = combine(
				renderer->mLastWorldTlasInstancePayloadHash,
				renderer->mLastWorldTlasSceneInstancePayloadHash);
			compatibility.lightingIdentity = combine(
				combine(
					combine(renderer->mRuntimeLightPayloadHash, renderer->mEmissiveSamplingPayloadHash),
					renderer->mSectorLightingPayloadHash),
				renderer->mDirectionalLightState.stateHash);
			return renderer->mIndirectRadianceCache.Prepare(
				BuildNRIIndirectRadianceCacheServices(*renderer), true, compatibility);
		};
		service.recordPendingClear = [](void* user) -> bool
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			return renderer->mIndirectRadianceCache.RecordPendingClear(
				BuildNRIIndirectRadianceCacheServices(*renderer));
		};
		service.advanceFrame = [](void* user)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			renderer->mIndirectRadianceCache.AdvanceFrame(BuildNRIIndirectRadianceCacheServices(*renderer));
		};
		service.copyTelemetry = [](void* user, uint64_t frameNumber)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			renderer->mIndirectRadianceCache.CopyTelemetryForReadback(
				BuildNRIIndirectRadianceCacheServices(*renderer), frameNumber);
		};
		service.readbackTelemetry = [](void* user, bool enabled)
		{
			NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
			renderer->mIndirectRadianceCache.ReadbackTelemetry(
				BuildNRIIndirectRadianceCacheServices(*renderer),
				enabled,
				renderer->mLastIndirectRadianceCacheTelemetry);
		};
		service.getTelemetry = [](void* user) -> const NRIIndirectRadianceCacheTelemetrySnapshot&
		{
			return static_cast<NRIRenderer*>(user)->mLastIndirectRadianceCacheTelemetry;
		};
		return service;
	};

	init.textures = buildTextureService();
	init.pipelines = buildPipelineService();
	init.descriptors = buildDescriptorService();
	init.resources = buildResourceService();
	init.commands = buildCommandService();
	init.sceneBinding = buildSceneBindingService();
	init.exposureService = buildExposureService();
	init.upscalerService = buildUpscalerService();
	init.selfTest = buildSelfTestService();
	init.smokeService = buildSmokeService();
	init.indirectRadianceCacheService = buildIndirectRadianceCacheService();
	init.pipelineLayout = &mPipelineLayout;
	init.taaPipelineLayout = &mTaaPipelineLayout;
	init.presentPipelineLayout = &mPresentPipelineLayout;
	init.bloomPipelineLayout = &mBloomPipelineLayout;
	init.samplerSet = &mSamplerSet;
	init.frameTextureSet = &mFrameTextureSet;
	init.outputSet = &mOutputSet;
	init.compositionFrameTextureSet = &mCompositionFrameTextureSet;
	init.compositionOutputSet = &mCompositionOutputSet;
	init.upscalerPrepassFrameTextureSet = &mUpscalerPrepassFrameTextureSet;
	init.upscalerPrepassOutputSet = &mUpscalerPrepassOutputSet;
	init.taaFrameTextureSet = &mTaaFrameTextureSet;
	init.taaOutputSet = &mTaaOutputSet;
	init.temporalReactiveInputSet = &mTemporalReactiveInputSet;
	init.temporalReactiveOutputSet = &mTemporalReactiveOutputSet;
	init.rawPresentFrameTextureSet = &mRawPresentFrameTextureSet;
	init.rawPresentOutputSet = &mRawPresentOutputSet;
	init.finalPresentFrameTextureSet = &mFinalPresentFrameTextureSet;
	init.finalPresentOutputSet = &mFinalPresentOutputSet;
	init.bloomInputSets = &mBloomInputSets;
	init.bloomOutputSets = &mBloomOutputSets;
	init.frameInputDescriptors = &mFrameInputDescriptors;
	init.outputDescriptors = &mOutputDescriptors;
	init.exposure = &mExposure;
	init.traceShaderStats = &mTraceShaderStats;
	init.mapMotionHistory = &mMapMotionHistory;
	init.nrd = &mNrd;
	init.upscaler = &mUpscaler;
	init.persistentVoxels = &mPersistentVoxels;
	init.boundSceneInstances = &mBoundSceneInstances;
	init.directionalLightState = &mDirectionalLightState;
	init.nightVisionState = &mNightVisionState;
	init.lastPerfShellTraceStats = &mLastPerfShellTraceStats;
	init.lastPerfTraceShaderStats = &mLastPerfTraceShaderStats;
	init.lastAutoExposureSettings = &mLastAutoExposureSettings;
	init.frame.frameIndex = mFrameIndex;
	init.frame.mainTemporalSerial = mMainTemporalSerial;
	init.frame.renderWidth = mRenderWidth;
	init.frame.renderHeight = mRenderHeight;
	init.frame.outputWidth = mOutputWidth;
	init.frame.outputHeight = mOutputHeight;
	init.frame.targetWidth = mTargetWidth;
	init.frame.targetHeight = mTargetHeight;
	init.frame.queuedFrameNum = frameBuffer != nullptr ?
		(uint8_t)std::clamp<size_t>(frameBuffer->mQueuedFrames.size(), 1u, 255u) :
		1u;
	init.frame.observedFrameTimeMs = mHasPendingFrameGenerationRealFrameTime ? mPendingFrameGenerationRealFrameTimeMs : 0.0f;
	init.frame.sceneLeft = mSceneLeft;
	init.frame.sceneTop = mSceneTop;
	std::copy(mCurrentCameraPos, mCurrentCameraPos + 3, init.frame.currentCameraPos.begin());
	std::copy(mCurrentCameraForward, mCurrentCameraForward + 3, init.frame.currentCameraForward.begin());
	std::copy(mCurrentCameraRight, mCurrentCameraRight + 3, init.frame.currentCameraRight.begin());
	std::copy(mCurrentCameraUp, mCurrentCameraUp + 3, init.frame.currentCameraUp.begin());
	std::copy(mPreviousCameraPos, mPreviousCameraPos + 3, init.frame.previousCameraPos.begin());
	std::copy(mPreviousCameraForward, mPreviousCameraForward + 3, init.frame.previousCameraForward.begin());
	std::copy(mPreviousCameraRight, mPreviousCameraRight + 3, init.frame.previousCameraRight.begin());
	std::copy(mPreviousCameraUp, mPreviousCameraUp + 3, init.frame.previousCameraUp.begin());
	init.frame.currentTanHalfFovX = mCurrentTanHalfFovX;
	init.frame.currentTanHalfFovY = mCurrentTanHalfFovY;
	init.frame.previousTanHalfFovX = mPreviousTanHalfFovX;
	init.frame.previousTanHalfFovY = mPreviousTanHalfFovY;
	init.frame.cameraNear = screen->GetZNear();
	init.frame.cameraFar = screen->GetZFar();
	init.frame.viewSpaceToMetersFactor = 1.0f;
	std::copy(mCurrentJitter, mCurrentJitter + 2, init.frame.currentJitter.begin());
	std::copy(mPreviousJitter, mPreviousJitter + 2, init.frame.previousJitter.begin());
	std::copy(mCurrentViewToClip, mCurrentViewToClip + 16, init.frame.currentViewToClip.begin());
	std::copy(mPreviousViewToClip, mPreviousViewToClip + 16, init.frame.previousViewToClip.begin());
	std::copy(mCurrentWorldToView, mCurrentWorldToView + 16, init.frame.currentWorldToView.begin());
	std::copy(mPreviousWorldToView, mPreviousWorldToView + 16, init.frame.previousWorldToView.begin());
	std::copy(mSkyColor, mSkyColor + 3, init.frame.skyColor.begin());
	std::copy(mGroundColor, mGroundColor + 3, init.frame.groundColor.begin());
	init.frame.guiCaptureActive = mGuiCaptureActive;
	init.frame.resetHistory = mResetHistory || !!nri_ptmotionreseteveryframe;
	init.frame.mainViewEligible = mainViewEligible;
	const NRISpatialAbsenceSnapshot& rawSpatialAbsence = mSpatialAbsenceGate.GetSnapshot();
	const bool typedSpatialCensusAuthority =
		mSpatialAbsenceGpuSnapshot.HasCensusAuthority(rawSpatialAbsence);
	const bool routeSpatialCensusAuthority = mSpatialAbsenceFormat == 0u ?
		rawSpatialAbsence.HasCensusAuthority() : typedSpatialCensusAuthority;
	const bool routeSpatialNegativeAuthority = mSpatialAbsenceFormat == 0u ?
		rawSpatialAbsence.HasNegativeAuthority() :
		mSpatialAbsenceGpuSnapshot.HasNegativeAuthority(rawSpatialAbsence);
	init.frame.spatialAbsenceAuthority =
		routeSpatialNegativeAuthority && mSpatialAbsenceRayQueryCandidateInstanceCount > 0u;
	init.frame.spatialActorCensusAuthority = routeSpatialCensusAuthority;
	init.sceneStats.sceneInstanceCount = (uint32_t)mBoundSceneInstances.size();
	init.sceneStats.staticPrimitiveCount = mBoundStaticPrimitiveCount;
	init.sceneStats.dynamicPrimitiveCount = mBoundDynamicPrimitiveCount;
	init.sceneStats.staticMaterialCount = mBoundStaticMaterialCount;
	init.sceneStats.dynamicMaterialCount = mBoundDynamicMaterialCount;
	init.sceneStats.portalCount = mBoundPortalCount;
	init.sceneStats.runtimeLightCount = mBoundRuntimeLightCount;
	init.sceneStats.runtimeLightTileCountX = mBoundRuntimeLightTileCountX;
	init.sceneStats.runtimeLightTileCountY = mBoundRuntimeLightTileCountY;
	init.sceneStats.runtimeLightTileSize = mBoundRuntimeLightTileSize;
	init.sceneStats.runtimeLightTileIndexCount = mBoundRuntimeLightTileIndexCount;
	init.sceneStats.runtimeLightMaxTileOccupancy = mBoundRuntimeLightMaxTileOccupancy;
	init.sceneStats.runtimeLightShadowBudget = mBoundRuntimeLightShadowBudget;
	init.sceneStats.runtimeLightShadowCandidateReferenceCount = mBoundRuntimeLightShadowCandidateReferenceCount;
	init.sceneStats.runtimeLightShadowSelectedReferenceCount = mBoundRuntimeLightShadowSelectedReferenceCount;
	init.sceneStats.runtimeLightShadowOverflowReferenceCount = mBoundRuntimeLightShadowOverflowReferenceCount;
	init.sceneStats.runtimeLightMaxShadowCandidatesPerTile = mBoundRuntimeLightMaxShadowCandidatesPerTile;
	init.sceneStats.runtimeLightMaxShadowSelectedPerTile = mBoundRuntimeLightMaxShadowSelectedPerTile;
	init.sceneStats.runtimeLightShadowSelectionHash = mBoundRuntimeLightShadowSelectionHash;
	init.sceneStats.runtimeLightShadowRetainedReferenceCount = mBoundRuntimeLightShadowTransitions.retainedReferenceCount;
	init.sceneStats.runtimeLightShadowReplacedReferenceCount = mBoundRuntimeLightShadowTransitions.replacedReferenceCount;
	init.sceneStats.runtimeLightShadowExpiredReferenceCount = mBoundRuntimeLightShadowTransitions.expiredReferenceCount;
	init.sceneStats.runtimeLightShadowRetainedKeyHash = mBoundRuntimeLightShadowTransitions.retainedKeyHash;
	init.sceneStats.runtimeLightShadowReplacedKeyHash = mBoundRuntimeLightShadowTransitions.replacedKeyHash;
	init.sceneStats.runtimeLightShadowExpiredKeyHash = mBoundRuntimeLightShadowTransitions.expiredKeyHash;
	init.sceneStats.emissivePrimitiveCount = mBoundEmissivePrimitiveCount;
	init.sceneStats.emissiveTotalPower = mBoundEmissiveTotalPower;
	init.hasAutoExposureSettingsState = &mHasAutoExposureSettingsState;
	init.useUpscaledInFinal = &mUseUpscaledInFinal;
	init.useDenoisedCompositionInputs = &mUseDenoisedCompositionInputs;
	init.useSplitShadowDenoiser = &mUseSplitShadowDenoiser;
	init.historyInputSlot = &mHistoryInputSlot;
	init.historyOutputSlot = &mHistoryOutputSlot;
	init.upscaledInputSlot = &mUpscaledInputSlot;
	return NRIPassDispatchContext(init);
}
