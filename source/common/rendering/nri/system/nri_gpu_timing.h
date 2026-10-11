#pragma once

#include "nri_local.h"
#include "perf_capture.h"

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
	void BeginSegment(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer, uint32_t slotIndex, uint64_t rendererFrame);
	void FinalizeSegment(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer);
	void AbandonSlot(uint32_t slotIndex);
	uint32_t BeginScope(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer, NRIGpuTimingScope scope);
	void EndScope(nri::CoreInterface& core, nri::CommandBuffer& commandBuffer, uint32_t markerIndex);
	bool IsRecording() const { return mActiveSlot < SlotCount; }

private:
	struct Marker
	{
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
		uint64_t rendererFrame = 0;
		bool pending = false;
	};

	std::array<Slot, SlotCount> mSlots = {};
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
	uint32_t mMarker = UINT32_MAX;
};
