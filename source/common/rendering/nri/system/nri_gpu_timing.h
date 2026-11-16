#pragma once

#include "nri_local.h"
#include "perf_capture.h"
#include "perf_audit.h"

#include <array>
#include <cstdint>

class NRIRenderDevice;

enum class NRIGpuTimingScope : uint8_t
{
	Scene,
	Trace,
	TraceDispatch,
	Denoise,
	Composition,
	Upscale,
	Final,
	VoxelAdmission,
	VoxelUpload,
	VoxelArenaCopy,
	VoxelClassify,
	VoxelScan,
	VoxelEmit,
	VoxelFinalize,
	VoxelBlas,
	WorldTlas,
	SmokeSimulation,
	SmokeVolume,
	SmokeGridAllocate,
	SmokeGridInitialize,
	SmokeGridDeposit,
	SmokeGridHalo,
	SmokeGridSimulate,
	SmokeGridRebuild,
	SmokeDormantArchive,
	SmokeDormantPromote,
	SmokeDormantEvolve,
	SmokeWorldActive,
	SmokeWorldLink,
	SmokeWorldProposal,
	SmokeWorldSeed,
	SmokeWorldTemporal,
	SmokeWorldFilter,
	SmokeWorldScatter,
	SmokeCarrier,
	SmokeViewPrepare,
	SmokeMaterialize,
	SmokeAnalyticMaterialize,
	SmokeTransientBins,
	SmokeTransientLightBuild,
	SmokeTransientMaterialize,
	SmokeViewPoint,
	SmokeViewDirectional,
	SmokeViewDirectReuse,
	SmokeViewEmissive,
	SmokeAnalyticEmissiveBuild,
	SmokeAnalyticEmissiveApply,
	SmokeViewIndirect,
	SmokeIntegrate,
	SmokeReconstruction,
	DynamicOverlayBlas,
	StaticTangentBuild,
	ResidentBlas,
	Count
};

class NRIGpuTiming
{
public:
	static constexpr uint32_t SlotCount = 3;
	static constexpr uint32_t QueryCapacity = 194;
	static constexpr uint32_t ScopeCapacity = 96;

	void Prepare(nri::CoreInterface& core, nri::Device& device);
	void Destroy(nri::CoreInterface& core);
	void RetireSlot(nri::CoreInterface& core, uint32_t slotIndex);
	void BeginSegment(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer, uint32_t slotIndex, uint64_t rendererFrame, bool resumeScopes = false);
	void FinalizeSegment(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer, bool continueScopes = false);
	void CancelContinuation(const char* reason);
	void AbandonSlot(uint32_t slotIndex);
	uint64_t BeginScope(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer, NRIGpuTimingScope scope);
	void EndScope(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer, uint64_t scopeId);
	bool IsRecording() const { return mActiveSlot < SlotCount; }

private:
	struct Marker
	{
		// A logical scope survives an explicit command-buffer restart. Marker
		// indices do not: reusing one must never let an old RAII handle close it.
		uint64_t scopeId = 0;
		NRIGpuTimingScope scope = NRIGpuTimingScope::Scene;
		uint32_t beginQuery = 0;
		uint32_t endQuery = 0;
		bool open = false;
	};

	struct Slot
	{
		nri::QueryPool* queryPool = nullptr;
		nri::Buffer* readback = nullptr;
		PerfCompactCaptureToken token;
		PerfAuditToken auditToken;
		uint64_t auditRequest = 0;
		std::array<Marker, ScopeCapacity> markers = {};
		uint32_t querySize = 0;
		uint32_t queryCount = 0;
		uint32_t markerCount = 0;
		uint32_t segmentBeginQuery = 0;
		uint32_t segmentEndQuery = 0;
		uint32_t droppedScopes = 0;
		uint32_t droppedVoxelScopes = 0;
		uint32_t droppedSmokeScopes = 0;
		uint32_t droppedOverlayBlasScopes = 0;
		uint32_t droppedStaticTangentScopes = 0;
		uint32_t droppedResidentBlasScopes = 0;
		uint32_t resumedScopes = 0;
		uint32_t continuedScopes = 0;
		uint64_t rendererFrame = 0;
		bool pending = false;
	};
	struct Continuation
	{
		std::array<Marker, ScopeCapacity> markers = {};
		PerfCompactCaptureToken token;
		PerfAuditToken auditToken;
		uint64_t auditRequest = 0;
		uint64_t rendererFrame = 0;
		uint32_t slotIndex = SlotCount;
		uint32_t markerCount = 0;
	};
	static void DropScope(Slot& slot, NRIGpuTimingScope scope);

	std::array<Slot, SlotCount> mSlots = {};
	Continuation mContinuation;
	uint64_t mNextScopeId = 1;
	uint64_t mFrequencyHz = 0;
	uint32_t mActiveSlot = SlotCount;
	bool mPrepared = false;
	bool mUnsupported = false;
};

class NRIScopedGpuTiming
{
public:
	NRIScopedGpuTiming(NRIRenderDevice* device, NRIGpuTimingScope scope);
	~NRIScopedGpuTiming();
	NRIScopedGpuTiming(const NRIScopedGpuTiming&) = delete;
	NRIScopedGpuTiming& operator=(const NRIScopedGpuTiming&) = delete;

private:
	NRIRenderDevice* mDevice = nullptr;
	uint64_t mScopeId = 0;
};
