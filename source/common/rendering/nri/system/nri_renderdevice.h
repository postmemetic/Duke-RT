#pragma once

#include "base_sysfb.h"
#include "../nri_output.h"
#include "../framegen/nri_framegen.h"
#include "nri_frame_shell.h"
#include "nri_local.h"
#include "Extensions/NRIWrapperD3D12.h"

#include <chrono>
#include <memory>
#include <unordered_set>
#include <vector>

class NRIRenderState;
class NRIHardwareDataBuffer;
class NRIHardwareTexture;
class NRIHardwareVertexBuffer;
class NRIHardwareIndexBuffer;
class FCanvasTexture;
class FTexture;
class FGameTexture;
class NRIRenderer;
class NRIPassDispatchContext;
class NRIDescriptorSetManager;
class NRIFrameResources;
class NRIPipelineStateManager;
class NRIExposurePassAccess;
class NRIAccelerationStructureManager;
class NRIBlueNoiseResources;
class NRISceneTextureResidency;
class NRIUpscalerContext;
class NRIGpuTiming;
enum class NRIGpuTimingScope : uint8_t;

#ifdef _WIN32
struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain4;
#endif

struct NRIBackendCapabilities
{
	nri::GraphicsAPI liveApi = nri::GraphicsAPI::VK;
	uint32_t shaderModel = 0;
	bool d3d12 = false;
	bool vulkan = false;
	bool nativeD3D12DeviceAvailable = false;
	bool nativeD3D12GraphicsQueueAvailable = false;
	bool nativeD3D12SwapChainAvailable = false;
	bool lowLatencyFeatureAvailable = false;
	bool lowLatencyInterfaceAvailable = false;
	bool lowLatencyAvailable = false;
	bool lowLatencySwapChainEnabled = false;
	bool waitableSwapChainAvailable = false;
	bool dredRequested = false;
};

struct NRIAdapterMemoryTelemetry
{
	bool liveUsageAvailable = false;
	uint64_t localBudgetBytes = 0;
	uint64_t localUsageBytes = 0;
	uint64_t nonLocalBudgetBytes = 0;
	uint64_t nonLocalUsageBytes = 0;
};

class NRIRenderDevice : public SystemBaseFrameBuffer
{
	friend class NRISmokeSystem;
	typedef SystemBaseFrameBuffer Super;

public:
	NRIRenderDevice(void *hMonitor, bool fullscreen);
	~NRIRenderDevice();

	void Update() override;
	void ToggleFullscreen(bool yes) override;
	void InitializeState() override;
	bool CompileNextShader() override;
	int GetShaderCount() override;
	int Backend() override { return 4; }
	bool SupportsQueued2DTextureRenders() const override { return true; }
	const char* DeviceName() const override;
	void BeginFrame() override;
	FRenderState* RenderState() override;
	void Draw2D() override;
	void WaitForCommands(bool finish) override;
	void SetVSync(bool vsync) override;
	void RequestSwapChainRefresh(const char* reason, bool forceRecreate);
	void SetSaveBuffers(bool yes) override;
	bool PrepareSavePicScene(int width, int height) override;
	void FinishSavePicScene() override;
	void ImageTransitionScene(bool unknown) override;
	void SetSceneRenderTarget(bool useSSAO) override;
	void SetActiveRenderTarget() override;
	void PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D) override;
	bool RenderPathTracedScene(HWDrawInfo& di, int drawmode, bool portal) override;
	bool HasActiveSceneFrame() const override;
	bool HasCurrentCommandBuffer() const;
	bool HasActiveTarget() const;
	bool IsPreloadCommandContextActive() const { return mPreloadCommandContextActive; }
	uint64_t GetRecordingCommandFenceValue() const { return mCommandBufferOpen ? mRecordingCommandFenceValue : 0; }
	bool IsFrameFenceValueComplete(uint64_t fenceValue);
	bool IsCommandFenceValueComplete(uint64_t fenceValue);
	bool IsCommandFenceValueAbandoned(uint64_t fenceValue) const;
	nri::CoreInterface* GetCoreInterface() { return &mCore; }
	nri::Device* GetDevice() const { return mDevice; }
	nri::CommandBuffer* GetCurrentCommandBuffer() const { return mCommandBuffer; }
	nri::DescriptorPool* GetDescriptorPool() const { return mDescriptorPool; }
	bool StartPathTracingLevelPreload() override;
	bool TickPathTracingLevelPreload() override;
	bool IsPathTracingLevelPreloadPending() const override;
	bool IsPreloadSubmitBudgetHit() const;
	uint32_t GetPreloadSubmitCountThisTick() const;
	uint32_t GetPreloadSubmitLimitThisTick() const;
	bool HasTerminalDeviceLoss() const;
	void CancelPathTracingLevelPreload() override;
	void NotifyPathTracingLevelFirstFrameRelease() override;
	void NotifyPathTracingLevelPreloadFinalCheckRelease() override;
	void NotifyLevelUnloadBegin(const LevelTransitionInfo& info) override;
	void NotifyLevelUnloadComplete(const LevelTransitionInfo& info) override;
	void NotifyLevelLoadBegin(const LevelTransitionInfo& info) override;
	void EmitPathTracingWeaponLightEvent(const PathTracingWeaponLightEvent& event) override;
	void EmitPathTracingActorSpriteTraceEvent(const PathTracingActorSpriteTraceEvent& event) override;
	void PrintPathTracingSurfaceProbeStatus() const override;
	bool BuildPathTracingEmissiveLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const override;
	bool BuildPathTracingSurfaceLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const override;
	bool BuildPathTracingVoxelPolicyEditTarget(PathTracingVoxelPolicyEditTarget& outTarget) const override;
	bool ApplyPathTracingVoxelPolicyEdit(const PathTracingVoxelPolicyEditRequest& request,
		PathTracingVoxelPolicyEditResult& outResult) override;
	bool ProjectPathTracingEditorLine(const float renderStart[3], const float renderEnd[3],
		DVector2& outStart, DVector2& outEnd) const override;
	bool SetPathTracingEditorPointLight(const DVector3& worldPosition, const float color[3], float intensity, float radius) override;
	void ClearPathTracingEditorPointLight() override;
	void ConsumePathTracingWeaponLightEvents(TArray<PathTracingWeaponLightEvent>& outEvents);
	uint32_t GetPendingPathTracingWeaponLightEventCount() const;
	NRIPTOutputPolicy GetPathTracingOutputPolicy() const;
	void PrintPathTracingOutputModeChange(uint32_t frameIndex, NRIPTOutputMode previousRequestedMode, NRIPTOutputMode previousResolvedMode) const;

	IHardwareTexture* CreateHardwareTexture(int numchannels) override;
	IVertexBuffer* CreateVertexBuffer() override;
	IIndexBuffer* CreateIndexBuffer() override;
	IDataBuffer* CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) override;
	FTexture* WipeStartScreen() override;
	FTexture* WipeEndScreen() override;
	bool QueueScreenshot(FileWriter* file, const char* filename = nullptr) override;
	TArray<uint8_t> GetScreenshotBuffer(int& pitch, ESSType& color_type, float& gamma) override;
	bool FlipSavePic() const override { return false; }
	void PrintPathTracingCaps() const;
	void PrintPathTracingStatus() const;
	void PrintPathTracingBuffers() const;
	void ResetPathTracingHistory();
	void ResetPathTracingAutoExposure();
	void PrintPathTracingSmokeStatus() const;
	void ResetPathTracingSmoke();
	void QueueSyntheticPathTracingSmoke();
	void NotifyPathTracingCameraCut(const char* reason) override;
	void SetPathTracingGuiCaptureState(bool active) override;
	bool SpawnPathTracingPointLight(float red, float green, float blue, float intensity, float radius, float offset, uint32_t& outId);
	bool RemovePathTracingPointLight(uint32_t id);
	void ClearPathTracingPointLights();
	void PrintPathTracingPointLights() const;
	bool SpawnPathTracingDebugSphere(float diameter, float distance, float metalness, float roughness, uint32_t& outId);
	bool RemovePathTracingDebugSphere(uint32_t id);
	void ClearPathTracingDebugSpheres();
	void PrintPathTracingDebugSpheres() const;
	bool AddPathTracingSpriteTileLightHeuristic(uint32_t textureId, float red, float green, float blue, float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId);
	void ClearPathTracingLightHeuristics();
	void PrintPathTracingLightHeuristics() const;
	void PrintPathTracingSceneLightDump(float radius, uint32_t limit) const;
	void PrintPathTracingLightClusters() const;
	bool AddPathTracingTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId);
	void ClearPathTracingEmissiveHeuristics();
	void PrintPathTracingEmissiveHeuristics() const;
	void NotifyPathTracingGlowControlChange();
	void NotifyPathTracingMaterialLightingCalibrationChange();
	void NotifyPathTracingAnalyticLightSettingsChange();
	void NotifyPathTracingDebugSphereTessellationChange();
	void PrintPathTracingEmissiveSurfaces(float radius, uint32_t limit) const;
	void PrintPathTracingSectorLights(float radius, uint32_t limit) const;
	void PrintPathTracingMapChunkDump(int32_t chunkIndex) const;
	void PrintPathTracingMapChunkCompare(int32_t chunkIndex) const;
	bool ShouldSkipSceneBuildForPathTracedScene(int drawmode, bool portal) const override;
	bool IsFullscreenModeActive() const { return m_Fullscreen; }
#ifdef _WIN32
	ID3D12Device* GetNativeD3D12Device() const { return mNativeD3D12Device; }
	ID3D12CommandQueue* GetNativeD3D12GraphicsQueue() const { return mNativeD3D12GraphicsQueue; }
	IDXGISwapChain4* GetNativeD3D12SwapChain() const { return mNativeD3D12SwapChain; }
	bool IsFrameGenerationPresentPathActive() const { return !mFrameGenerationPresentImages.empty() && mFrameGeneration.ShouldUsePresentBridge(); }
#endif
	uint64_t GetAdapterLocalBudgetBytes() const { return mAdapterLocalBudgetBytes; }
	NRIAdapterMemoryTelemetry GetAdapterMemoryTelemetry() const;
	uint64_t GetAdapterNonLocalBudgetBytes() const { return mAdapterNonLocalBudgetBytes; }
	NRIBackendCapabilities BuildBackendCapabilities() const;
	uint32_t BeginGpuTimingScope(NRIGpuTimingScope scope);
	void EndGpuTimingScope(uint32_t markerIndex);
	bool UsesDiagnosticShaderVariant() const;

private:
	static constexpr uint32_t FrameSequenceHistorySize = NRIFrameShell::FrameSequenceHistorySize;
	static constexpr uint32_t QueuedFrameCount = NRIFrameShell::QueuedFrameCount;
	using QueuedFrame = NRIFrameShell::QueuedFrame;
	using FrameSequenceEntry = NRIFrameShell::FrameSequenceEntry;
	using FrameBoundaryDebugStats = NRIFrameShell::FrameBoundaryDebugStats;

	struct RetiredTextureResource
	{
		NRITextureResource resource = {};
		uint64_t fenceValue = 0;
	};

	struct Texture2DDebugStats
	{
		uint64_t frameNumber = 0;
		uint32_t ensureCalls = 0;
		uint32_t canvasEnsures = 0;
		uint32_t cacheHits = 0;
		uint32_t cacheMisses = 0;
		uint32_t uploadAttempts = 0;
		uint32_t uploadFailures = 0;
		uint32_t resourceCreates = 0;
		uint32_t resourceRecreates = 0;
		uint64_t uploadedBytes = 0;
		uint64_t totalEnsureCalls = 0;
		uint64_t totalCanvasEnsures = 0;
		uint64_t totalCacheHits = 0;
		uint64_t totalCacheMisses = 0;
		uint64_t totalUploadAttempts = 0;
		uint64_t totalUploadFailures = 0;
		uint64_t totalResourceCreates = 0;
		uint64_t totalResourceRecreates = 0;
		uint64_t totalUploadedBytes = 0;
		uint64_t residentBytes = 0;
		uint64_t peakResidentBytes = 0;
	};

	struct PendingScreenshotCapture
	{
		std::unique_ptr<FileWriter> file;
		FString fileName;
		nri::Buffer* readbackBuffer = nullptr;
		uint64_t serial = 0;
		uint64_t shellFrameIndex = 0;
		uint32_t rendererFrameIndex = ~0u;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t rowPitch = 0;
		uint32_t slicePitch = 0;
		nri::Format sourceFormat = nri::Format::UNKNOWN;
		bool readbackRecorded = false;
	};

	using PFN_nriEnumerateAdapters = nri::Result(NRI_CALL*)(nri::AdapterDesc*, uint32_t&);
	using PFN_nriCreateDevice = nri::Result(NRI_CALL*)(const nri::DeviceCreationDesc&, nri::Device*&);
	using PFN_nriDestroyDevice = void (NRI_CALL*)(nri::Device*);
	using PFN_nriGetInterface = nri::Result(NRI_CALL*)(const nri::Device&, const char*, size_t, void*);

	void LogStartup();
	bool LoadNRI();
	bool CreateDevice();
	bool CreateQueuedFrames();
	bool CreateSwapChain();
	bool ApplyPendingSwapChainRefresh();
	void DestroyQueuedFrames();
	void DestroySwapChain();
	bool CreateRenderResources();
	void DestroyRenderResources();
	bool BeginCommandList(const char* reason, bool waitForSlotReuse = false);
	void AbandonRecordingCommandFenceValue();
	bool TryGetFenceValue(nri::Fence& fence, const char* context, uint64_t& outCompletedFenceValue);
	bool WaitForFenceValue(nri::Fence& fence, uint64_t fenceValue, const char* context);
	bool SubmitWaitAndRestartCommandList(const char* reason);
	void MarkTerminalDeviceLoss(const char* context);
	[[noreturn]] void FatalTerminalDeviceLoss(const char* context);
	bool BeginPreloadCommandContext(const char* reason);
	bool EndPreloadCommandContext(const char* reason);
	bool EnsureSwapChainSize();
	void EndFrameAndPresent();
	void LogD3D12FailureDiagnostics(const char* context);
	void RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect&)> renderFunc) override;
	void RenderTextureView(FGameTexture* tex, std::function<void(IntRect&)> renderFunc) override;
	void SnapshotCurrentViewToCanvas(FCanvasTexture* tex) override;
	void CopyScreenToBuffer(int width, int height, uint8_t* buffer) override;
	void TransitionTexture(NRITextureResource& texture, nri::AccessLayoutStage after);
	void PrepareTargetForRendering(NRITextureResource& target, bool clear);
	void FinishTargetRendering(NRITextureResource& target, nri::AccessLayoutStage after);
	void RetireTextureResource(NRITextureResource& resource);
	void ReleaseRetiredTextureResources(bool finish);
	void DestroyTextureResource(NRITextureResource& resource);
	bool CreateTextureViews(NRITextureResource& resource);
	bool CreateOwnedTexture(NRITextureResource& resource, uint32_t width, uint32_t height, nri::Format format, nri::TextureUsageBits usage, nri::TextureType type = nri::TextureType::TEXTURE_2D, uint32_t layerNum = 1, nri::TextureView shaderViewType = nri::TextureView::TEXTURE);
	bool UploadTextureData(NRITextureResource& resource, const void* data, uint32_t width, uint32_t height, uint32_t rowPitch);
	bool UploadTextureDataAsync(NRITextureResource& resource, const void* data, uint32_t width, uint32_t height, uint32_t rowPitch, uint64_t& outFenceValue);
	bool UploadTextureSubresources(NRITextureResource& resource, const nri::TextureSubresourceUploadDesc* subresources, uint32_t subresourceNum, uint32_t width, uint32_t height);
	bool CopyCurrentTargetToTexture(NRITextureResource& destination);
	bool LoadShaderBlob(const char* fileName, std::vector<uint8_t>& outBlob);
	const void* GetVertexShaderBytecode(size_t& size) const;
	const void* GetPixelShaderBytecode(size_t& size) const;
	nri::GraphicsAPI GetSelectedAPI() const;
	nri::GraphicsAPI GetLiveAPI() const;
	NRISamplerMode GetSamplerMode(int clampMode) const;
	nri::DescriptorSet* GetSamplerSet(NRISamplerMode mode) const;
	nri::DescriptorSet* CreateTextureSet(nri::Descriptor* shaderView);
	bool RenderPathTracingSanityFrame();
	void PrintFrameBoundaryStatus() const;
	void PrintFrameSequenceStatus() const;
	void PrintSwapChainStatus() const;
	void PrintFrameShellStatus() const;
	void Print2DTextureStatus() const;
	void PrintVramTelemetryStatus() const;
	const char* DescribeTextureTarget(const NRITextureResource* target) const;
	void RecordFrameSequence(uint32_t releaseSemaphoreIndex, uint64_t submittedFenceValue, nri::Result presentResult);
	void Reset2DTextureFrameStats();
	void Note2DTextureEnsure(bool canvas);
	void Note2DTextureCacheHit();
	void Note2DTextureCacheMiss();
	void Note2DTextureUploadAttempt(uint64_t bytes, bool success);
	void Note2DTextureResourceCreate(bool recreated);
	void Note2DTextureResidentBytesChanged(uint64_t oldBytes, uint64_t newBytes);
	void NoteSwapChainAcquire(uint32_t imageIndex);
	void NoteSwapChainPresent(uint32_t imageIndex);
	void NoteSwapChainAbandon(uint32_t imageIndex);
	uint32_t GetQueuedFrameIndex(uint64_t frameIndex) const;
	void SelectQueuedFrame(uint32_t queuedFrameIndex);
	void ResetFrameTracking(bool presentedAcquiredImage = false);
	void RefreshNativeFrameGenerationHandles();
	void RefreshNativeFrameGenerationSwapChain();
	bool RefreshFrameGenerationPresentTargets();
	void DestroyFrameGenerationPresentTargets();
	bool ShouldRequestFrameGenerationLowLatencySwapChain() const;
	nri::SwapChainBits GetEffectiveRequestedSwapChainFlags() const;
	bool RefreshSwapChainDisplayDesc(bool logChanges);
	void ResolvePathTracingSwapChainOutput(nri::SwapChainFormat& outRequestedFormat, nri::SwapChainFormat& outResolvedFormat, const char*& outReason) const;
	void SyncPathTracingOutputModeCVarWithSwapChainState(const char* evaluatedResolveReason = nullptr);
	bool ShouldUseFrameGenerationUiTarget() const;
	bool ShouldHandoffFrameGenerationUiTexture() const;
	bool ShouldUseFrameGenerationUiLocalCompositeFallback() const;
	const char* GetFrameGenerationUiRouteName() const;
	uint32_t GetFrameGenerationSceneBlendPrefixCount() const;
	bool EnsureFrameGenerationUiTexture(uint32_t width, uint32_t height, nri::Format format);
	NRITextureResource* GetFrameGenerationUiTargetResource() const;
	bool EnsureViewSnapshotTexture(uint32_t width, uint32_t height, nri::Format format);
	NRITextureResource* GetViewSnapshotTargetResource() const;
	void ClearTargetColor(NRITextureResource& target, float red, float green, float blue, float alpha);
	void ClearActiveTargetIfPending();
	void BeginFrameGenerationUiTarget();
	void DrawFrameGenerationSceneBlendPrefix();
	void FinalizeFrameGenerationUiTarget();
	void CompositeFrameGenerationUiTexture();
	void DestroyFrameGenerationUiTexture();
	void DestroyViewSnapshotTexture();
	bool EnsureSaveTarget(uint32_t width, uint32_t height);
	bool SubmitAndWaitCurrentCommandBuffer();
	bool CopyTextureToTexture(NRITextureResource& destination, NRITextureResource& source);
	bool SnapshotTextureToCanvas(FCanvasTexture* tex, NRITextureResource& source);
	void RecordPendingScreenshotReadbacks();
	void FinishPendingScreenshotReadbacks(bool submitted, uint64_t submittedFenceValue);
	void ClearPendingScreenshotReadbacks();
	void ResetLevelTransitionShellState();
	uint32_t ClearPendingPathTracingWeaponLightEvents();
	void LogLevelTransitionSnapshot(const char* phase, const LevelTransitionInfo& info, bool preloadPending, uint32_t clearedWeaponLightEvents) const;
	void TraceVoxelPreloadLifecycle(const char* stage, const LevelTransitionInfo& info) const;
	void CaptureCompactPerfRendererStats(bool rendered);
	void CaptureCompactPerfFrameBoundary(bool presentOk);

	friend class NRIHardwareTexture;
	friend class NRIRenderState;
	friend class NRIRenderer;
	friend class NRIDescriptorSetManager;
	friend class NRIFrameResources;
	friend class NRIPipelineStateManager;
	friend class NRIExposurePassAccess;
	friend class NRIAccelerationStructureManager;
	friend class NRIBlueNoiseResources;
	friend class NRIPersistentVoxelServiceFactory;
	friend class NRISceneTextureResidency;
	friend class NRIUpscalerContext;
	friend class NRIFrameGenerationContext;

	std::unique_ptr<NRIRenderState> mRenderState;
	std::unique_ptr<NRIRenderer> mRenderer;
	std::unique_ptr<NRIGpuTiming> mGpuTiming;
	void* mNriModule = nullptr;
	PFN_nriEnumerateAdapters mEnumerateAdapters = nullptr;
	PFN_nriCreateDevice mCreateDeviceFn = nullptr;
	PFN_nriDestroyDevice mDestroyDeviceFn = nullptr;
	PFN_nriGetInterface mGetInterfaceFn = nullptr;

	nri::CoreInterface mCore = {};
	nri::HelperInterface mHelper = {};
	nri::RayTracingInterface mRayTracing = {};
	nri::StreamerInterface mStreamer = {};
	nri::SwapChainInterface mSwapChainInterface = {};
	nri::UpscalerInterface mUpscaler = {};
	nri::LowLatencyInterface mLowLatency = {};
	nri::WrapperD3D12Interface mWrapperD3D12 = {};
	nri::Device* mDevice = nullptr;
	nri::Queue* mGraphicsQueue = nullptr;
	nri::SwapChain* mSwapChain = nullptr;
	nri::Streamer* mStreamerInstance = nullptr;
	nri::Fence* mFrameFence = nullptr;
	nri::Fence* mCommandCompletionFence = nullptr;
	nri::CommandAllocator* mCommandAllocator = nullptr;
	nri::CommandBuffer* mCommandBuffer = nullptr;
	nri::DescriptorPool* mDescriptorPool = nullptr;
	nri::PipelineLayout* mPipelineLayout = nullptr;
	nri::Descriptor* mConstantBufferView = nullptr;
	nri::Descriptor* mSamplers[(size_t)NRISamplerMode::Count] = {};
	nri::DescriptorSet* mSamplerSets[(size_t)NRISamplerMode::Count] = {};
	NRIFrameGenerationContext mFrameGeneration;

	std::vector<NRISwapChainImage> mSwapChainImages;
	std::vector<NRITextureResource> mFrameGenerationPresentImages;
	std::vector<QueuedFrame> mQueuedFrames;
	std::vector<RetiredTextureResource> mRetiredTextureResources;
	NRITextureResource mSceneTarget;
	NRITextureResource mSaveTarget;
	NRITextureResource* mActiveTarget = nullptr;
	NRITextureResource* mCurrentPresentTarget = nullptr;
	FCanvasTexture* mActiveCanvasTexture = nullptr;
	FTexture* mActiveCanvasSourceTexture = nullptr;
	FCanvasTexture* mPendingViewSnapshotCanvas = nullptr;
	FGameTexture* mFrameGenerationUiTexture = nullptr;
	FGameTexture* mViewSnapshotTexture = nullptr;
	nri::DescriptorSet* mWhiteTextureSet = nullptr;
	NRIHardwareTexture* mWhiteTexture = nullptr;

	std::vector<uint8_t> mVertexShaderBlob;
	std::vector<uint8_t> mPixelShaderBlob;
	bool mDiagnosticShaderVariantEffective = false;
	bool mShaderVariantWarningEmitted = false;
	bool mShaderVariantSelectionEmitted = false;
	FString mDeviceName = "NRI";
	nri::GraphicsAPI mCreatedDeviceApi = nri::GraphicsAPI::VK;
	nri::SwapChainBits mSwapChainFlags = nri::SwapChainBits::NONE;
	nri::SwapChainFormat mRequestedSwapChainFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	nri::SwapChainFormat mCreatedSwapChainFormat = nri::SwapChainFormat::BT709_G22_8BIT;
	nri::Format mResolvedSwapChainTextureFormat = nri::Format::UNKNOWN;
	NRIWindowPresentationMode mCreatedWindowPresentationMode = NRIWindowPresentationMode::Windowed;
	bool mCreatedWindowPresentationModeValid = false;
	nri::DisplayDesc mSwapChainDisplayDesc = {};
	nri::Result mSwapChainDisplayDescResult = nri::Result::FAILURE;
	uint64_t mSwapChainDisplayGeneration = 0;
	uint64_t mCreatedSwapChainDisplayGeneration = 0;
	uint8_t mSwapChainQueuedFrameNum = 0;
	uint8_t mSwapChainTextureCount = 0;
	uint64_t mObservedSwapChainAcquireMask = 0;
	uint64_t mObservedSwapChainPresentMask = 0;
	std::vector<uint64_t> mSwapChainAcquireCounts;
	std::vector<uint64_t> mSwapChainPresentCounts;
	std::vector<uint64_t> mSwapChainAbandonCounts;
#ifdef _WIN32
	ID3D12Device* mNativeD3D12Device = nullptr;
	ID3D12CommandQueue* mNativeD3D12GraphicsQueue = nullptr;
	IDXGISwapChain4* mNativeD3D12SwapChain = nullptr;
	bool mFrameGenerationPresentAllowsTearing = false;
#endif
	uint64_t mFrameIndex = 0;
	uint64_t mSubmittedFenceValue = 0;
	uint64_t mRecordingCommandFenceValue = 0;
	uint64_t mNextCommandFenceValue = 1;
	std::unordered_set<uint64_t> mAbandonedCommandFenceValues;
	bool mFrameBegun = false;
	bool mSwapChainRefreshPending = false;
	bool mSwapChainRefreshForceRecreate = false;
	uint32_t mSwapChainRefreshRequestCount = 0;
	FString mSwapChainRefreshReason = "none";
	bool mWindowModeTransitionPending = false;
	NRIWindowPresentationMode mWindowModeTransitionFrom = NRIWindowPresentationMode::Windowed;
	uint64_t mWindowModeTransitionSerial = 0;
	uint8_t mWindowModeTransitionSettleBudget = 0;
	bool mUsingSaveTarget = false;
	bool mStandaloneSavePicFrame = false;
	bool mPreloadCommandContextActive = false;
	bool mPathTracingLevelPreloadPending = false;
	uint32_t mPreloadSubmitsThisTick = 0;
	uint32_t mPreloadMaxSubmitsThisTick = 0;
	bool mPreloadSubmitBudgetHit = false;
	bool mTerminalDeviceLoss = false;
	bool mLoggedTerminalDeviceLoss = false;
	bool mShuttingDown = false;
	FString mLastPreloadSubmitReason = "none";
	bool mHasAcquiredSwapChainImage = false;
	bool mHasPresentedSwapChainFrame = false;
	bool mHasSwapChainDisplayDesc = false;
	bool mFrameGenerationUiTargetActive = false;
	bool mPathTracingGuiCaptureActive = false;
	uint32_t mCurrentSwapChainImage = 0;
	uint32_t mCurrentQueuedFrameIndex = 0;
	uint32_t mAcquireSemaphoreIndex = 0;
	uint32_t mRequestedSaveTargetWidth = 0;
	uint32_t mRequestedSaveTargetHeight = 0;
	FString mSwapChainOutputResolveReason = "requested-sdr";
	bool mCommandBufferOpen = false;
	bool mInitialized = false;
	bool mLoggedStartup = false;
	bool mLoggedD3D12FailureDred = false;
	nri::Result mLastSubmitAndWaitResult = nri::Result::SUCCESS;
	FrameBoundaryDebugStats mLastFrameBoundaryStats;
	FrameSequenceEntry mFrameSequenceHistory[FrameSequenceHistorySize] = {};
	uint32_t mFrameSequenceWriteIndex = 0;
	Texture2DDebugStats mTexture2DDebugStats;
	std::vector<PendingScreenshotCapture> mPendingScreenshotCaptures;
	uint64_t mNextScreenshotCaptureSerial = 1;
	uint64_t mAdapterLocalBudgetBytes = 0;
	uint64_t mAdapterNonLocalBudgetBytes = 0;
	bool mTraceThisFrame = false;
	TArray<PathTracingWeaponLightEvent> mPendingPathTracingWeaponLightEvents;
	uint64_t mNextPathTracingWeaponLightEventSerial = 1;
	uint32_t mPathTracingWeaponLightEventsEnqueuedThisFrame = 0;
	uint32_t mPathTracingEditorPointLightId = 0;
	bool mPathTracingEditorPointLightActive = false;
	bool mLevelTransitionInProgress = false;
	LevelTransitionInfo mCurrentLevelTransition = {};
	std::chrono::steady_clock::time_point mLevelTransitionAcceptedTime = {};
	uint64_t mLevelTransitionTimelineSerial = 0;
};
