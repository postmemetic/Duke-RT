#pragma once

#include "nri_renderer.h"

#include <array>
#include <vector>

struct HWDrawInfo;

namespace nri_scene
{
	struct GeometryData;
	struct MaterialData;
}

enum class NRISmokeRoutePlacement : uint32_t
{
	StandardPreUpscale = 0,
	DlrrPreUpscaleMainInput,
	DlrrPostUpscale,
};

struct NRISmokeRouteDesc
{
	NRIRenderer::FrameTextureSlot inputSlot = NRIRenderer::FrameTextureSlot::Count;
	NRIRenderer::FrameTextureSlot outputSlot = NRIRenderer::FrameTextureSlot::Count;
	NRIRenderer::FrameTextureSlot depthSlot = NRIRenderer::FrameTextureSlot::Count;
	NRIRenderer::ExposureDomain exposureDomain = NRIRenderer::ExposureDomain::SceneHDR;
	NRISmokeRoutePlacement placement = NRISmokeRoutePlacement::StandardPreUpscale;
	uint32_t width = 0;
	uint32_t height = 0;
	bool supported = false;
};

class NRIPassDispatchContext
{
public:
	using FrameTextureSlot = NRIRenderer::FrameTextureSlot;
	using PipelineSlot = NRIRenderer::PipelineSlot;
	using ExposureRoute = NRIRenderer::ExposureRoute;
	using ExposureDomain = NRIRenderer::ExposureDomain;

	struct TextureService
	{
		using GetFrameTextureFn = NRITextureResource& (*)(void* user, FrameTextureSlot slot);

		void* user = nullptr;
		GetFrameTextureFn getFrameTexture = nullptr;

		NRITextureResource& Get(FrameTextureSlot slot) const;
	};

	struct PipelineService
	{
		using GetPipelineFn = nri::Pipeline* (*)(void* user, PipelineSlot slot);
		using EnsureIndirectRadianceCachePipelineFn = bool (*)(void* user);
		using GetIndirectRadianceCachePipelineLayoutFn = nri::PipelineLayout* (*)(void* user);

		void* user = nullptr;
		GetPipelineFn getPipeline = nullptr;
		EnsureIndirectRadianceCachePipelineFn ensureIndirectRadianceCachePipeline = nullptr;
		GetIndirectRadianceCachePipelineLayoutFn getIndirectRadianceCachePipelineLayout = nullptr;

		nri::Pipeline* Get(PipelineSlot slot) const;
		bool EnsureIndirectRadianceCachePipeline() const;
		nri::PipelineLayout* GetIndirectRadianceCachePipelineLayout() const;
	};

	struct DescriptorService
	{
		using UpdateFrameTextureSetFn = bool (*)(void* user);
		using UpdateFrameTextureSetWithDescriptorsFn = bool (*)(void* user, nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, NRI_INPUT_DESCRIPTOR_NUM>& descriptors);
		using UpdateOutputSetFn = bool (*)(void* user);
		using UpdateOutputSetWithDescriptorsFn = bool (*)(void* user, nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, NRI_OUTPUT_DESCRIPTOR_NUM>& descriptors);

		void* user = nullptr;
		UpdateFrameTextureSetFn updateFrameTextureSet = nullptr;
		UpdateFrameTextureSetWithDescriptorsFn updateFrameTextureSetWithDescriptors = nullptr;
		UpdateOutputSetFn updateOutputSet = nullptr;
		UpdateOutputSetWithDescriptorsFn updateOutputSetWithDescriptors = nullptr;

		bool UpdateFrameTextureSet() const;
		bool UpdateFrameTextureSet(nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, NRI_INPUT_DESCRIPTOR_NUM>& descriptors) const;
		bool UpdateOutputSet() const;
		bool UpdateOutputSet(nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, NRI_OUTPUT_DESCRIPTOR_NUM>& descriptors) const;
	};

	struct ResourceService
	{
		using BuildResourceServicesFn = NRIResourceServices (*)(void* user);
		using UpdateReprojectionBufferFn = bool (*)(void* user);
		using GetOutputPolicyFn = NRIPTOutputPolicy (*)(void* user);
		using CopyFinalToActiveTargetFn = void (*)(void* user);
		using CopyTextureFn = void (*)(void* user, NRITextureResource& source, NRITextureResource& destination);
		using TransitionTextureFn = void (*)(void* user, NRITextureResource& texture, nri::AccessLayoutStage after);

		void* user = nullptr;
		NRIRenderDevice* frameBuffer = nullptr;
		nri::CoreInterface* core = nullptr;
		nri::Device* device = nullptr;
		nri::CommandBuffer* commandBuffer = nullptr;
		BuildResourceServicesFn buildResourceServices = nullptr;
		UpdateReprojectionBufferFn updateReprojectionBuffer = nullptr;
		GetOutputPolicyFn getOutputPolicy = nullptr;
		CopyFinalToActiveTargetFn copyFinalToActiveTarget = nullptr;
		CopyTextureFn copyTexture = nullptr;
		TransitionTextureFn transitionTexture = nullptr;

		NRIResourceServices BuildResourceServices() const;
		bool UpdateReprojectionBuffer() const;
		NRIPTOutputPolicy GetOutputPolicy() const;
		void CopyFinalToActiveTarget() const;
		void CopyTexture(NRITextureResource& source, NRITextureResource& destination) const;
		void TransitionTexture(NRITextureResource& texture, nri::AccessLayoutStage after) const;
	};

	struct CommandService
	{
		nri::CoreInterface* core = nullptr;
		nri::CommandBuffer* commandBuffer = nullptr;
		nri::DescriptorPool* descriptorPool = nullptr;

		nri::CommandBuffer* GetCommandBuffer() const;
		void RestoreDescriptorPool() const;
		void SetPipelineLayout(nri::PipelineLayout* pipelineLayout) const;
		void SetRootConstants(const void* data, uint32_t size) const;
		void SetDescriptorSet(uint32_t setIndex, nri::DescriptorSet* descriptorSet) const;
		void SetPipeline(nri::Pipeline* pipeline) const;
		void Dispatch(uint32_t x, uint32_t y, uint32_t z) const;
		void UpdateDescriptorRanges(const nri::UpdateDescriptorRangeDesc* updates, uint32_t updateCount) const;
	};

	struct SceneBindingService
	{
		using GetCurrentSceneDescriptorSetFn = nri::DescriptorSet* (*)(void* user);
		using BindSceneRootDescriptorsFn = bool (*)(void* user);

		void* user = nullptr;
		GetCurrentSceneDescriptorSetFn getCurrentSceneTextureSet = nullptr;
		GetCurrentSceneDescriptorSetFn getCurrentSceneDataSet = nullptr;
		BindSceneRootDescriptorsFn bindSceneRootDescriptors = nullptr;

		nri::DescriptorSet* GetCurrentSceneTextureSet() const;
		nri::DescriptorSet* GetCurrentSceneDataSet() const;
		bool BindSceneRootDescriptors() const;
	};

	struct ExposureService
	{
		using ReadbackAutoExposureStatsFn = void (*)(void* user);
		using EnsureAutoExposureResourcesFn = bool (*)(void* user, const NRIAutoExposureSettings& settings);
		using RequestAutoExposureResetFn = void (*)(void* user, const char* reason);
		using DispatchAutoExposureFn = bool (*)(void* user, FrameTextureSlot sourceSlot);
		using ResolveExposureRouteFn = ExposureRoute (*)(void* user, FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind);

		void* user = nullptr;
		ReadbackAutoExposureStatsFn readbackAutoExposureStats = nullptr;
		EnsureAutoExposureResourcesFn ensureAutoExposureResources = nullptr;
		RequestAutoExposureResetFn requestAutoExposureReset = nullptr;
		DispatchAutoExposureFn dispatchAutoExposure = nullptr;
		ResolveExposureRouteFn resolveExposureRoute = nullptr;

		void ReadbackAutoExposureStats() const;
		bool EnsureAutoExposureResources(const NRIAutoExposureSettings& settings) const;
		void RequestAutoExposureReset(const char* reason) const;
		bool DispatchAutoExposure(FrameTextureSlot sourceSlot) const;
		ExposureRoute ResolveExposureRoute(FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const;
	};

	struct UpscalerService
	{
		using ResolveMainUpscalerKindFn = NRIMainUpscalerKind (*)(void* user, bool logFallback);
		using ResolvePostSharpenKindFn = NRIPostSharpenKind (*)(void* user, bool logFallback);
		using GetSelectedUpscalerModeFn = nri::UpscalerMode (*)(void* user);
		using ShouldRunAppTaaForFrameGraphFn = bool (*)(void* user, NRIMainUpscalerKind kind);
		using TraceTemporalStateFn = void (*)(void* user, const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot);
		using EnsureMainUpscalerFn = bool (*)(void* user, NRIMainUpscalerKind kind, nri::UpscalerMode mode, uint32_t outputWidth, uint32_t outputHeight, bool exposure, bool reactive);
		using DispatchMainUpscalerFn = bool (*)(void* user, NRIMainUpscalerKind kind, const NRIUpscalerDispatchDesc& desc);
		using EnsurePostSharpenFn = bool (*)(void* user, NRIPostSharpenKind kind, uint32_t outputWidth, uint32_t outputHeight);
		using DispatchPostSharpenFn = bool (*)(void* user, NRIPostSharpenKind kind, const NRIUpscalerDispatchDesc& desc);

		void* user = nullptr;
		ResolveMainUpscalerKindFn resolveMainUpscalerKind = nullptr;
		ResolvePostSharpenKindFn resolvePostSharpenKind = nullptr;
		GetSelectedUpscalerModeFn getSelectedUpscalerMode = nullptr;
		ShouldRunAppTaaForFrameGraphFn shouldRunAppTaaForFrameGraph = nullptr;
		TraceTemporalStateFn traceTemporalState = nullptr;
		EnsureMainUpscalerFn ensureMainUpscaler = nullptr;
		DispatchMainUpscalerFn dispatchMainUpscaler = nullptr;
		EnsurePostSharpenFn ensurePostSharpen = nullptr;
		DispatchPostSharpenFn dispatchPostSharpen = nullptr;

		NRIMainUpscalerKind ResolveMainUpscalerKind(bool logFallback) const;
		NRIPostSharpenKind ResolvePostSharpenKind(bool logFallback) const;
		nri::UpscalerMode GetSelectedUpscalerMode() const;
		bool ShouldRunAppTaaForFrameGraph(NRIMainUpscalerKind kind) const;
		void TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const;
		bool EnsureMainUpscaler(NRIMainUpscalerKind kind, nri::UpscalerMode mode, uint32_t outputWidth, uint32_t outputHeight, bool exposure, bool reactive) const;
		bool DispatchMainUpscaler(NRIMainUpscalerKind kind, const NRIUpscalerDispatchDesc& desc) const;
		bool EnsurePostSharpen(NRIPostSharpenKind kind, uint32_t outputWidth, uint32_t outputHeight) const;
		bool DispatchPostSharpen(NRIPostSharpenKind kind, const NRIUpscalerDispatchDesc& desc) const;
	};

	struct SelfTestService
	{
		using ResetSelfTestRouteSnapshotFn = void (*)(void* user);
		using SetSelfTestRouteSnapshotFn = void (*)(void* user, const char* routeName, const char* presenterName, const char* ownerName, const char* passListName, bool denoiserRun, bool upscalerRun, bool exposureRun);

		void* user = nullptr;
		ResetSelfTestRouteSnapshotFn resetSelfTestRouteSnapshot = nullptr;
		SetSelfTestRouteSnapshotFn setSelfTestRouteSnapshot = nullptr;

		void ResetSelfTestRouteSnapshot() const;
		void SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passListName, bool denoiserRun, bool upscalerRun, bool exposureRun) const;
	};

	struct SmokeService
	{
		using PrepareFrameFn = bool (*)(void* user, bool mainViewEligible, const TArray<PathTracingWeaponLightEvent>& weaponEvents);
		using DispatchRouteFn = bool (*)(void* user, const NRISmokeRouteDesc& route);
		using GetVolumeSlotFn = uint32_t (*)(void* user, bool metadata);

		void* user = nullptr;
		const TArray<PathTracingWeaponLightEvent>* weaponEvents = nullptr;
		PrepareFrameFn prepareFrame = nullptr;
		DispatchRouteFn dispatchRoute = nullptr;
		GetVolumeSlotFn getVolumeSlot = nullptr;

		bool PrepareFrame(bool mainViewEligible) const;
		bool DispatchRoute(const NRISmokeRouteDesc& route) const;
		FrameTextureSlot GetVolumeSlot(bool metadata) const;
	};

	struct IndirectRadianceCacheService
	{
		using PrepareFn = NRIIndirectRadianceCachePrepareResult (*)(void* user, bool enabled);
		using RecordPendingClearFn = bool (*)(void* user);
		using AdvanceFrameFn = void (*)(void* user);
		using CopyTelemetryFn = void (*)(void* user, uint64_t frameNumber);
		using ReadbackTelemetryFn = void (*)(void* user, bool enabled);
		using GetTelemetryFn = const NRIIndirectRadianceCacheTelemetrySnapshot& (*)(void* user);

		void* user = nullptr;
		PrepareFn prepare = nullptr;
		RecordPendingClearFn recordPendingClear = nullptr;
		AdvanceFrameFn advanceFrame = nullptr;
		CopyTelemetryFn copyTelemetry = nullptr;
		ReadbackTelemetryFn readbackTelemetry = nullptr;
		GetTelemetryFn getTelemetry = nullptr;

		NRIIndirectRadianceCachePrepareResult Prepare(bool enabled) const;
		bool RecordPendingClear() const;
		void AdvanceFrame() const;
		void CopyTelemetry(uint64_t frameNumber) const;
		void ReadbackTelemetry(bool enabled) const;
		const NRIIndirectRadianceCacheTelemetrySnapshot& GetTelemetry() const;
	};

	struct FrameSnapshot
	{
		uint32_t frameIndex = 0;
		uint64_t mainTemporalSerial = 0;
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		uint32_t outputWidth = 0;
		uint32_t outputHeight = 0;
		uint32_t targetWidth = 0;
		uint32_t targetHeight = 0;
		uint8_t queuedFrameNum = 1;
		float observedFrameTimeMs = 0.0f;
		int32_t sceneLeft = 0;
		int32_t sceneTop = 0;
		std::array<float, 3> currentCameraPos = {};
		std::array<float, 3> currentCameraForward = {};
		std::array<float, 3> currentCameraRight = {};
		std::array<float, 3> currentCameraUp = {};
		std::array<float, 3> previousCameraPos = {};
		std::array<float, 3> previousCameraForward = {};
		std::array<float, 3> previousCameraRight = {};
		std::array<float, 3> previousCameraUp = {};
		float currentTanHalfFovX = 0.0f;
		float currentTanHalfFovY = 0.0f;
		float previousTanHalfFovX = 0.0f;
		float previousTanHalfFovY = 0.0f;
		float cameraNear = 0.0f;
		float cameraFar = 0.0f;
		float viewSpaceToMetersFactor = 1.0f;
		std::array<float, 2> currentJitter = {};
		std::array<float, 2> previousJitter = {};
		std::array<float, 16> currentViewToClip = {};
		std::array<float, 16> previousViewToClip = {};
		std::array<float, 16> currentWorldToView = {};
		std::array<float, 16> previousWorldToView = {};
		std::array<float, 3> skyColor = {};
		std::array<float, 3> groundColor = {};
		bool guiCaptureActive = false;
		bool resetHistory = false;
		bool mainViewEligible = false;
		bool spatialAbsenceAuthority = false;
		bool spatialActorCensusAuthority = false;
	};

	struct SceneStatsSnapshot
	{
		uint32_t sceneInstanceCount = 0;
		uint32_t staticPrimitiveCount = 0;
		uint32_t dynamicPrimitiveCount = 0;
		uint32_t staticMaterialCount = 0;
		uint32_t dynamicMaterialCount = 0;
		uint32_t portalCount = 0;
		uint32_t runtimeLightCount = 0;
		uint32_t runtimeLightTileCountX = 0;
		uint32_t runtimeLightTileCountY = 0;
		uint32_t runtimeLightTileSize = 0;
		uint32_t runtimeLightTileIndexCount = 0;
		uint32_t runtimeLightMaxTileOccupancy = 0;
		uint32_t runtimeLightShadowBudget = 0;
		uint32_t runtimeLightShadowCandidateReferenceCount = 0;
		uint32_t runtimeLightShadowSelectedReferenceCount = 0;
		uint32_t runtimeLightShadowOverflowReferenceCount = 0;
		uint32_t runtimeLightMaxShadowCandidatesPerTile = 0;
		uint32_t runtimeLightMaxShadowSelectedPerTile = 0;
		uint64_t runtimeLightShadowSelectionHash = 0;
		uint32_t emissivePrimitiveCount = 0;
		float emissiveTotalPower = 0.0f;
	};

	struct Init
	{
		TextureService textures;
		PipelineService pipelines;
		DescriptorService descriptors;
		ResourceService resources;
		CommandService commands;
		SceneBindingService sceneBinding;
		ExposureService exposureService;
		UpscalerService upscalerService;
		SelfTestService selfTest;
		SmokeService smokeService;
		IndirectRadianceCacheService indirectRadianceCacheService;
		nri::PipelineLayout** pipelineLayout = nullptr;
		nri::PipelineLayout** taaPipelineLayout = nullptr;
		nri::PipelineLayout** presentPipelineLayout = nullptr;
		nri::PipelineLayout** bloomPipelineLayout = nullptr;
		nri::DescriptorSet** samplerSet = nullptr;
		nri::DescriptorSet** frameTextureSet = nullptr;
		nri::DescriptorSet** outputSet = nullptr;
		nri::DescriptorSet** compositionFrameTextureSet = nullptr;
		nri::DescriptorSet** compositionOutputSet = nullptr;
		nri::DescriptorSet** upscalerPrepassFrameTextureSet = nullptr;
		nri::DescriptorSet** upscalerPrepassOutputSet = nullptr;
		nri::DescriptorSet** taaFrameTextureSet = nullptr;
		nri::DescriptorSet** taaOutputSet = nullptr;
		nri::DescriptorSet** temporalReactiveInputSet = nullptr;
		nri::DescriptorSet** temporalReactiveOutputSet = nullptr;
		nri::DescriptorSet** rawPresentFrameTextureSet = nullptr;
		nri::DescriptorSet** rawPresentOutputSet = nullptr;
		nri::DescriptorSet** finalPresentFrameTextureSet = nullptr;
		nri::DescriptorSet** finalPresentOutputSet = nullptr;
		std::array<nri::DescriptorSet*, NRIRenderer::BloomDescriptorSetCount>* bloomInputSets = nullptr;
		std::array<nri::DescriptorSet*, NRIRenderer::BloomDescriptorSetCount>* bloomOutputSets = nullptr;
		std::array<nri::Descriptor*, NRI_INPUT_DESCRIPTOR_NUM>* frameInputDescriptors = nullptr;
		std::array<nri::Descriptor*, NRI_OUTPUT_DESCRIPTOR_NUM>* outputDescriptors = nullptr;
		NRIExposureController* exposure = nullptr;
		NRITraceShaderStats* traceShaderStats = nullptr;
		NRIMapMotionHistory* mapMotionHistory = nullptr;
		NRINrdContext* nrd = nullptr;
		NRIUpscalerContext* upscaler = nullptr;
		NRIPersistentVoxelResidency* persistentVoxels = nullptr;
		std::vector<SceneInstanceData>* boundSceneInstances = nullptr;
		NRIDirectionalLightState* directionalLightState = nullptr;
		NRIPTNightVisionState* nightVisionState = nullptr;
		NRIRenderer::PerfShellTraceStats* lastPerfShellTraceStats = nullptr;
		NRIRenderer::PerfTraceShaderStats* lastPerfTraceShaderStats = nullptr;
		NRIAutoExposureSettings* lastAutoExposureSettings = nullptr;
		FrameSnapshot frame;
		SceneStatsSnapshot sceneStats;
		bool* hasAutoExposureSettingsState = nullptr;
		bool* useUpscaledInFinal = nullptr;
		bool* useDenoisedCompositionInputs = nullptr;
		bool* useSplitShadowDenoiser = nullptr;
		FrameTextureSlot* historyInputSlot = nullptr;
		FrameTextureSlot* historyOutputSlot = nullptr;
		FrameTextureSlot* upscaledInputSlot = nullptr;
	};

	explicit NRIPassDispatchContext(const Init& init);

	TextureService mTextures;
	PipelineService mPipelines;
	DescriptorService mDescriptors;
	ResourceService mResources;
	CommandService mCommands;
	SceneBindingService mSceneBinding;
	ExposureService mExposureService;
	UpscalerService mUpscalerService;
	SelfTestService mSelfTest;
	SmokeService mSmokeService;
	IndirectRadianceCacheService mIndirectRadianceCacheService;
	nri::PipelineLayout*& mPipelineLayout;
	nri::PipelineLayout*& mTaaPipelineLayout;
	nri::PipelineLayout*& mPresentPipelineLayout;
	nri::PipelineLayout*& mBloomPipelineLayout;
	nri::DescriptorSet*& mSamplerSet;
	nri::DescriptorSet*& mFrameTextureSet;
	nri::DescriptorSet*& mOutputSet;
	nri::DescriptorSet*& mCompositionFrameTextureSet;
	nri::DescriptorSet*& mCompositionOutputSet;
	nri::DescriptorSet*& mUpscalerPrepassFrameTextureSet;
	nri::DescriptorSet*& mUpscalerPrepassOutputSet;
	nri::DescriptorSet*& mTaaFrameTextureSet;
	nri::DescriptorSet*& mTaaOutputSet;
	nri::DescriptorSet*& mTemporalReactiveInputSet;
	nri::DescriptorSet*& mTemporalReactiveOutputSet;
	nri::DescriptorSet*& mRawPresentFrameTextureSet;
	nri::DescriptorSet*& mRawPresentOutputSet;
	nri::DescriptorSet*& mFinalPresentFrameTextureSet;
	nri::DescriptorSet*& mFinalPresentOutputSet;
	std::array<nri::DescriptorSet*, NRIRenderer::BloomDescriptorSetCount>& mBloomInputSets;
	std::array<nri::DescriptorSet*, NRIRenderer::BloomDescriptorSetCount>& mBloomOutputSets;
	std::array<nri::Descriptor*, NRI_INPUT_DESCRIPTOR_NUM>& mFrameInputDescriptors;
	std::array<nri::Descriptor*, NRI_OUTPUT_DESCRIPTOR_NUM>& mOutputDescriptors;
	NRIExposureController& mExposure;
	NRITraceShaderStats& mTraceShaderStats;
	NRIMapMotionHistory& mMapMotionHistory;
	NRINrdContext& mNrd;
	NRIUpscalerContext& mUpscaler;
	NRIPersistentVoxelResidency& mPersistentVoxels;
	std::vector<SceneInstanceData>& mBoundSceneInstances;
	NRIDirectionalLightState& mDirectionalLightState;
	NRIPTNightVisionState& mNightVisionState;
	NRIRenderer::PerfShellTraceStats& mLastPerfShellTraceStats;
	NRIRenderer::PerfTraceShaderStats& mLastPerfTraceShaderStats;
	NRIAutoExposureSettings& mLastAutoExposureSettings;
	FrameSnapshot mFrame;
	SceneStatsSnapshot mSceneStats;
	bool& mHasAutoExposureSettingsState;
	bool& mUseUpscaledInFinal;
	bool& mUseDenoisedCompositionInputs;
	bool& mUseSplitShadowDenoiser;
	FrameTextureSlot& mHistoryInputSlot;
	FrameTextureSlot& mHistoryOutputSlot;
	FrameTextureSlot& mUpscaledInputSlot;
	bool mTraceIndirectDenoiserAvailable = false;
	uint32_t mEffectiveIndirectSamplingMode = 0;
	uint32_t mActiveIndirectSamplingMode = 0;
};

class NRIPassDispatcher
{
public:
	static bool DispatchBootstrapView(NRIPassDispatchContext& context);
	static bool DispatchFrameGraph(
		NRIPassDispatchContext& context,
		HWDrawInfo& di,
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials,
		int drawmode);
	static bool DispatchTraceOpaque(
		NRIPassDispatchContext& context,
		HWDrawInfo& di,
		const nri_scene::GeometryData& geometry,
		const std::vector<nri_scene::MaterialData>& materials);
	static bool DispatchDenoiser(NRIPassDispatchContext& context);
	static bool DispatchComposition(NRIPassDispatchContext& context, NRIRenderer::FrameTextureSlot outputSlot = NRIRenderer::FrameTextureSlot::Composed);
	static bool DispatchTraceTransparent(NRIPassDispatchContext& context);
	static bool DispatchUpscalerPrepass(NRIPassDispatchContext& context, NRIMainUpscalerKind mainKind);
	static bool DispatchRawPresent(
		NRIPassDispatchContext& context,
		NRIRenderer::FrameTextureSlot inputSlot,
		NRIRenderer::FrameTextureSlot secondarySlot = NRIRenderer::FrameTextureSlot::Count,
		NRIRenderer::FrameTextureSlot tertiarySlot = NRIRenderer::FrameTextureSlot::Count);
	static bool DispatchFinalPresent(NRIPassDispatchContext& context, NRIRenderer::FrameTextureSlot inputSlot);
	static bool DispatchUpscaleChain(NRIPassDispatchContext& context);
	static bool DispatchFinal(NRIPassDispatchContext& context);
};
