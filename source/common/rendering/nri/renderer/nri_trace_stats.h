#pragma once

#include "nri_frame_resources.h"
#include "nri_resources.h"
#include "nri_trace_stats_readback_policy.h"

#include <array>
#include <cstdint>
#include <vector>

static constexpr uint32_t NRI_TRACE_SHADER_SCALAR_STAT_COUNT = 72;
static constexpr uint32_t NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT = 1024;
static constexpr uint32_t NRI_TRACE_SHADER_RAY_KIND_COUNT = 6;
static constexpr uint32_t NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE = NRI_TRACE_SHADER_SCALAR_STAT_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_INSTANCE_ACCEPTED_BASE = NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE + NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE = NRI_TRACE_SHADER_INSTANCE_ACCEPTED_BASE + NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_STAT_COUNT =
	NRI_TRACE_SHADER_INSTANCE_KIND_COMMITTED_BASE + NRI_TRACE_SHADER_RAY_KIND_COUNT * NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_HOT_INSTANCE_COUNT = 8;

struct NRITraceShaderHotInstance
{
	uint32_t instanceId = 0;
	uint32_t dataSource = 0;
	uint32_t primitiveOffset = 0;
	uint32_t primitiveCount = 0;
	uint32_t metadata0 = 0;
	uint32_t metadata1 = 0;
	bool shadowProxy = false;
	uint32_t committed = 0;
	uint32_t accepted = 0;
	uint32_t primaryCommitted = 0;
	uint32_t ungatedCommitted = 0;
	uint32_t sunCommitted = 0;
	uint32_t pointCommitted = 0;
	uint32_t emissiveCommitted = 0;
	uint32_t fastEmissiveCommitted = 0;
};

struct NRITraceShaderStatsSnapshot
{
	struct ObserverStats
	{
		uint64_t copiesRequested = 0;
		uint64_t copiesRecorded = 0;
		uint64_t copiesDroppedBusy = 0;
		uint64_t copiesDroppedNoFence = 0;
		uint64_t readbacksPublished = 0;
		uint64_t readbacksSuperseded = 0;
		uint64_t readbacksAbandoned = 0;
		uint64_t readbackMapFailures = 0;
		uint64_t attributionRowsCopied = 0;
		uint64_t attributionBytesCopied = 0;
		uint32_t pendingReadbackCount = 0;
	};

	bool valid = false;
	uint64_t frameNumber = 0;
	std::array<uint32_t, NRI_TRACE_SHADER_STAT_COUNT> counters = {};
	uint32_t hotInstanceCount = 0;
	std::array<NRITraceShaderHotInstance, NRI_TRACE_SHADER_HOT_INSTANCE_COUNT> hotInstances = {};
	ObserverStats observer = {};
};

struct NRITraceShaderStatsFenceServices
{
	using GetRecordingCommandFenceValueFn = uint64_t (*)(void* user);
	using IsCommandFenceValueCompleteFn = bool (*)(void* user, uint64_t fenceValue);
	using IsCommandFenceValueAbandonedFn = bool (*)(void* user, uint64_t fenceValue);

	void* user = nullptr;
	GetRecordingCommandFenceValueFn getRecordingCommandFenceValue = nullptr;
	IsCommandFenceValueCompleteFn isCommandFenceValueComplete = nullptr;
	IsCommandFenceValueAbandonedFn isCommandFenceValueAbandoned = nullptr;

	uint64_t GetRecordingCommandFenceValue() const;
	bool IsCommandFenceValueComplete(uint64_t fenceValue) const;
	bool IsCommandFenceValueAbandoned(uint64_t fenceValue) const;
};

struct NRITraceShaderStatsCopyInput
{
	bool enabled = false;
	uint64_t frameNumber = 0;
	NRITraceShaderStatsFenceServices fences;
	const std::vector<SceneInstanceData>* boundSceneInstances = nullptr;
	uint32_t staticPrimitiveCount = 0;
	uint32_t dynamicPrimitiveCount = 0;
	uint32_t persistentVoxelPrimitiveCount = 0;
};

class NRITraceShaderStats
{
public:
	bool Ensure(const NRIResourceServices& services);
	void Destroy(const NRIResourceServices& services);
	void ResetBuffer(const NRIResourceServices& services, bool enabled);
	void CopyForReadback(const NRIResourceServices& services, const NRITraceShaderStatsCopyInput& input);
	void Readback(
		const NRIResourceServices& services,
		bool enabled,
		const NRITraceShaderStatsFenceServices& fences,
		NRITraceShaderStatsSnapshot& outStats);

	nri::Descriptor* Descriptor() const { return mStatsBuffer.shaderView; }

private:
	bool CreateBufferWithoutViewAtLocation(
		const NRIResourceServices& services,
		NRIBufferResource& resource,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::MemoryLocation memoryLocation);
	void ClearReadbackSlot(uint32_t slotIndex);
	void UpdateObserverSnapshot(NRITraceShaderStatsSnapshot& outStats) const;

	struct ReadbackSlot
	{
		NRIBufferResource buffer;
		std::vector<NRITraceShaderInstanceAttribution> attribution;
		uint64_t frameNumber = 0;
		uint64_t fenceValue = 0;
		uint64_t copySerial = 0;
		bool pending = false;
		bool initialized = false;
	};

	NRIBufferResource mStatsBuffer;
	NRIBufferResource mZeroBuffer;
	std::array<ReadbackSlot, NRI_TRACE_SHADER_READBACK_SLOT_COUNT> mReadbackSlots = {};
	NRITraceShaderStatsSnapshot::ObserverStats mObserver = {};
	uint64_t mNextCopySerial = 1;
	uint64_t mReadbackConsumerFence = 0;
	uint32_t mNextCopySlot = 0;
};
