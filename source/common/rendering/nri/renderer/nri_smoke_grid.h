#pragma once

#include "nri_renderer_settings.h"
#include "nri_resources.h"
#include "nri_smoke_grid_contracts.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class NRIRenderDevice;

// NRISmokeSystem is the renderer-private integration boundary. It assembles
// this small service surface while it has legitimate access to the render
// device, descriptor pool, and queued-frame state. Keeping those details out
// of NRISmokeGrid avoids adding another broad friend of NRIRenderer.
struct NRISmokeGridServices
{
	using LoadShaderBlobFn = bool (*)(void* user, const char* fileName, std::vector<uint8_t>& outBlob);
	using WaitForCommandsFn = void (*)(void* user, const char* reason);

	nri::CoreInterface* core = nullptr;
	nri::Device* device = nullptr;
	nri::CommandBuffer* commandBuffer = nullptr;
	NRIRenderDevice* gpuTimingDevice = nullptr;
	nri::DescriptorPool* descriptorPool = nullptr;
	nri::GraphicsAPI graphicsAPI = nri::GraphicsAPI::VK;
	uint32_t queuedFrameCount = 0;
	uint32_t queuedFrameIndex = 0;
	uint64_t rendererFrame = 0;
	void* user = nullptr;
	LoadShaderBlobFn loadShaderBlob = nullptr;
	WaitForCommandsFn waitForCommands = nullptr;

	bool IsDeviceValid() const
	{
		return core != nullptr && device != nullptr && descriptorPool != nullptr;
	}

	bool IsRecordingValid() const
	{
		return IsDeviceValid() && commandBuffer != nullptr;
	}

	bool LoadShaderBlob(const char* fileName, std::vector<uint8_t>& outBlob) const
	{
		return loadShaderBlob != nullptr && loadShaderBlob(user, fileName, outBlob);
	}

	void WaitForCommands(const char* reason) const
	{
		if (waitForCommands != nullptr)
			waitForCommands(user, reason);
	}
};

struct NRISmokeGridFrameDesc
{
	uint32_t frameIndex = 0;
	uint32_t simulationEpoch = 1;
	uint32_t commandCount = 0;
	uint32_t styleCount = 0;
	uint32_t simulationSubsteps = 0;
	bool hashHealthDiagnostic = false;
	bool spatialObservationReadback = false;
	float simulationStep = 1.0f / 60.0f;
	const nri::Descriptor* styleView = nullptr;
	const nri::Descriptor* commandView = nullptr;
};

struct NRISmokeGridSourceStatusSnapshot
{
	uint32_t sourceId = 0;
	uint32_t sourceClass = 0;
	uint32_t priority = 0;
	uint32_t commands = 0;
	uint32_t requestedBricks = 0;
	uint32_t existingHits = 0;
	uint32_t admittedNew = 0;
	uint32_t rejectedCapacity = 0;
	uint32_t rejectedProbe = 0;
	uint32_t rejectedInvalid = 0;
	uint32_t footprintCulled = 0;
	uint32_t depositionCells = 0;
	uint32_t requestedMassQ = 0;
	uint32_t depositedMassQ = 0;
	uint32_t rejectedMassQ = 0;
	uint32_t admittedKeyHash = 0;
};

struct NRISmokeGridStatusSnapshot
{
	bool requested = false;
	bool initialized = false;
	bool resourcesReady = false;
	bool gpuStatsValid = false;
	uint64_t gpuRendererFrame = UINT64_MAX;
	uint32_t representation = 0;
	uint32_t brickCapacity = 0;
	uint32_t hashCapacity = 0;
	uint32_t cellCapacity = 0;
	uint32_t activePing = 0;
	uint32_t fieldPing = 0;
	uint64_t residentBytes = 0;
	uint64_t controlReadbackBytes = 0;
	uint64_t sourceReadbackBytes = 0;
	NRISmokeGridControlGpu gpu = {};
	bool gpuFrameDeltaValid = false;
	uint32_t gpuFrameDeltaInterval = 0;
	NRISmokeGridControlGpu gpuFrameDelta = {};
	std::vector<NRISmokeGridSourceStatusSnapshot> sources;
	std::vector<NRISmokeGridBrickGpu> spatialBricks;
	uint64_t spatialGpuRendererFrame = UINT64_MAX;
	std::string failureReason = "not-requested";
	std::string resetReason = "initial";
};

class NRISmokeGrid
{
public:
	static constexpr uint32_t StorageDescriptorCount = 23u;
	static constexpr uint32_t EvaluationDescriptorCount = 11u;
	static constexpr uint32_t DormantTransactionDescriptorCount = 18u;
	static constexpr uint32_t SourceCapacity = 256u;

	// Pipeline and descriptor-set initialization is intentionally lazy. A grid
	// failure must remain local so the particle backend can stay authoritative.
	bool Initialize(const NRISmokeGridServices& services);
	bool PrepareFrame(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
		uint32_t frameIndex, uint32_t simulationEpoch);
	bool RecordFrame(const NRISmokeGridServices& services, const NRISmokeSettings& settings,
		const NRISmokeGridFrameDesc& frame);

	void Reset(uint32_t simulationEpoch, const char* reason);
	void Shutdown(const NRISmokeGridServices& services);
	void PrintStatus() const;

	// Fixed order consumed by the main smoke evaluation layout at u17..u27:
	// control, hash, bricks, scalar A/B, velocity A/B, optical A/B, dynamics A/B.
	bool GetEvaluationStorageDescriptors(
		std::array<const nri::Descriptor*, EvaluationDescriptorCount>& descriptors) const;
	// Focused writable service for the dormant authority transaction. Order:
	// control, hash, bricks, free, active A/B, field A/B pairs, deposits 0..3.
	bool GetDormantTransactionStorageDescriptors(
		std::array<const nri::Descriptor*, DormantTransactionDescriptorCount>& descriptors) const;
	bool GetDormantTransactionStorageBuffers(
		std::array<nri::Buffer*, DormantTransactionDescriptorCount>& buffers) const;
	const NRISmokeGridStatusSnapshot& GetStatusSnapshot() const { return mStatus; }
	uint32_t GetActivePing() const { return mActivePing; }
	uint32_t GetFieldPing() const { return mFieldPing; }
	const nri::Descriptor* GetPromptOutcomeDescriptor() const { return mPromptOutcomes.storageView; }
	std::vector<NRISmokePromptOutcomeGpu> ConsumePromptOutcomes();

private:
	struct FrameSlot
	{
		nri::DescriptorSet* inputSet = nullptr;
		NRIBufferResource controlReadback;
		NRIBufferResource sourceReadback;
		NRIBufferResource promptReadback;
		NRIBufferResource spatialBrickReadback;
		bool readbackPending = false;
		bool diagnosticReadbackPending = false;
		bool promptReadbackInitialized = false;
		bool diagnosticReadbackInitialized = false;
		bool spatialReadbackPending = false;
		bool spatialReadbackInitialized = false;
		uint64_t readbackRendererFrame = UINT64_MAX;
		uint32_t readbackEpoch = 0;
	};

	bool EnsureResources(const NRISmokeGridServices& services, const NRISmokeSettings& settings);
	bool CreateBuffer(const NRISmokeGridServices& services, NRIBufferResource& out, uint64_t size,
		uint32_t stride, nri::BufferUsageBits usage, nri::MemoryLocation location, bool storageView);
	void DestroyBuffer(const NRISmokeGridServices& services, NRIBufferResource& resource);
	void DestroyResources(const NRISmokeGridServices& services);
	void ConsumeReadback(const NRISmokeGridServices& services, uint32_t simulationEpoch);
	void SetFailure(const char* reason);

	void TransitionResourcesToStorage(const NRISmokeGridServices& services);
	void StorageBarrier(const NRISmokeGridServices& services);
	void TransitionDispatchToArgument(const NRISmokeGridServices& services);
	void TransitionDispatchToStorage(const NRISmokeGridServices& services);
	void Dispatch(const NRISmokeGridServices& services, NRISmokeGridConstants& constants,
		NRISmokeGridPass pass, uint32_t x, uint32_t y = 1u, uint32_t z = 1u);
	void DispatchIndirect(const NRISmokeGridServices& services, NRISmokeGridConstants& constants,
		NRISmokeGridPass pass, uint64_t byteOffset = 0u);
	bool RecordControlReadback(const NRISmokeGridServices& services,
		const NRISmokeSettings& settings, bool spatialObservationReadback);

	std::array<NRIBufferResource*, StorageDescriptorCount> StorageResources();
	std::array<const NRIBufferResource*, StorageDescriptorCount> StorageResources() const;

	NRISmokeGridStatusSnapshot mStatus = {};
	nri::PipelineLayout* mPipelineLayout = nullptr;
	std::array<nri::Pipeline*, 14> mPipelines = {};
	nri::DescriptorSet* mStorageSet = nullptr;
	std::vector<FrameSlot> mFrameSlots;

	NRIBufferResource mControl;
	NRIBufferResource mHash;
	NRIBufferResource mBricks;
	NRIBufferResource mFreeList;
	NRIBufferResource mActiveA;
	NRIBufferResource mActiveB;
	NRIBufferResource mDispatchArgs;
	NRIBufferResource mScalarA;
	NRIBufferResource mScalarB;
	NRIBufferResource mVelocityA;
	NRIBufferResource mVelocityB;
	NRIBufferResource mOpticalA;
	NRIBufferResource mOpticalB;
	NRIBufferResource mDynamicsA;
	NRIBufferResource mDynamicsB;
	NRIBufferResource mDeposit0;
	NRIBufferResource mDeposit1;
	NRIBufferResource mDeposit2;
	NRIBufferResource mDeposit3;
	NRIBufferResource mSourceStats;
	NRIBufferResource mPromptOutcomes;
	NRIBufferResource mPromptLedger;
	NRIBufferResource mVorticity;
	std::vector<NRISmokePromptOutcomeGpu> mPromptCommits;

	uint32_t mResourceBrickCapacity = 0;
	uint32_t mResourceHashCapacity = 0;
	uint32_t mResourceCellCapacity = 0;
	uint32_t mResourceEpoch = 0;
	uint32_t mActivePing = 0;
	uint32_t mFieldPing = 0;
	double mSimulationSeconds = 0.0;
	float mResourceCellSize = 0.0f;
	bool mInitialized = false;
	bool mResourcesInitialized = false;
	bool mDispatchIsArgument = false;
	bool mNeedsClear = true;
};
