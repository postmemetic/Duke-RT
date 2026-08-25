#pragma once

#include "nri_frame_resources.h"
#include "nri_resources.h"
#include "nri_trace_stats_readback_policy.h"

#include <array>
#include <cstdint>
#include <vector>

enum NRISpatialAbsenceProbeOutcome : uint32_t
{
	NRI_SPATIAL_ABSENCE_PROBE_DISABLED = 0,
	NRI_SPATIAL_ABSENCE_PROBE_SNAPSHOT_INVALID,
	NRI_SPATIAL_ABSENCE_PROBE_FRAME_MISMATCH,
	NRI_SPATIAL_ABSENCE_PROBE_OUTSIDE_GUARD,
	NRI_SPATIAL_ABSENCE_PROBE_LOOKUP_INVALID,
	NRI_SPATIAL_ABSENCE_PROBE_OUTSIDE_UNION,
	NRI_SPATIAL_ABSENCE_PROBE_NEGATIVE_FOOTPRINT_MISS,
	NRI_SPATIAL_ABSENCE_PROBE_PAIR_BOUNDS_MISS,
	NRI_SPATIAL_ABSENCE_PROBE_POSITIVE_FOOTPRINT_MISS,
	NRI_SPATIAL_ABSENCE_PROBE_REJECT,
	NRI_SPATIAL_ABSENCE_PROBE_OUTCOME_COUNT,
};

static constexpr uint32_t NRI_TRACE_SHADER_RAY_KIND_COUNT = 6;
static constexpr uint32_t NRI_TRACE_SHADER_BASE_SCALAR_STAT_COUNT = 79;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CALLS = 0;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CANDIDATES = 1;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_OUTCOME_BASE = 2;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_CLAIM = 12;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_OUTCOME = 13;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_SOURCE = 14;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_INSTANCE = 15;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_PRIMITIVE = 16;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_CHUNK = 17;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_POSITION_X = 18;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_POSITION_Y = 19;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_POSITION_Z = 20;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_MATCHED_POSITIVE_CHUNK = 21;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FOOTPRINT_STAGE = 22;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FOOTPRINT_CELL_REFERENCES = 23;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FOOTPRINT_BEST_MARGIN = 24;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FOOTPRINT_BEST_TRIANGLE = 25;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_MATERIAL_FLAGS = 26;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FINAL_VALID = 27;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FINAL_SOURCE = 28;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FINAL_INSTANCE = 29;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FINAL_PRIMITIVE = 30;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FINAL_CHUNK = 31;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FINAL_POSITION_X = 32;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FINAL_POSITION_Y = 33;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_FINAL_POSITION_Z = 34;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_STRIDE = 35;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_RAY_BASE = NRI_TRACE_SHADER_BASE_SCALAR_STAT_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_PIXEL_STRIDE = 8;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_PIXEL_BASE =
	NRI_TRACE_SHADER_PROBE_RAY_BASE + NRI_TRACE_SHADER_RAY_KIND_COUNT * NRI_TRACE_SHADER_PROBE_RAY_STRIDE;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_TARGET_PIXEL_BASE = NRI_TRACE_SHADER_PROBE_PIXEL_BASE;
static constexpr uint32_t NRI_TRACE_SHADER_PROBE_REFERENCE_PIXEL_BASE =
	NRI_TRACE_SHADER_PROBE_PIXEL_BASE + NRI_TRACE_SHADER_PROBE_PIXEL_STRIDE;
static constexpr uint32_t NRI_TRACE_SHADER_SCALAR_STAT_COUNT =
	NRI_TRACE_SHADER_PROBE_PIXEL_BASE + 2 * NRI_TRACE_SHADER_PROBE_PIXEL_STRIDE;
static_assert(NRI_SPATIAL_ABSENCE_PROBE_OUTCOME_COUNT == 10);
static_assert(NRI_TRACE_SHADER_PROBE_RAY_CANDIDATE_CLAIM ==
	NRI_TRACE_SHADER_PROBE_RAY_OUTCOME_BASE + NRI_SPATIAL_ABSENCE_PROBE_OUTCOME_COUNT);
static_assert(NRI_TRACE_SHADER_SCALAR_STAT_COUNT == 305);
static constexpr uint32_t NRI_TRACE_SHADER_SPATIAL_COMPARE_BASE = NRI_TRACE_SHADER_SCALAR_STAT_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_SPATIAL_COMPARE_COUNT = 5;
static constexpr uint32_t NRI_TRACE_SHADER_SPATIAL_COMPARE_TOTAL = NRI_TRACE_SHADER_SPATIAL_COMPARE_BASE + 0;
static constexpr uint32_t NRI_TRACE_SHADER_SPATIAL_COMPARE_TYPED_UNAVAILABLE = NRI_TRACE_SHADER_SPATIAL_COMPARE_BASE + 1;
static constexpr uint32_t NRI_TRACE_SHADER_SPATIAL_COMPARE_OUTCOME_MISMATCH = NRI_TRACE_SHADER_SPATIAL_COMPARE_BASE + 2;
static constexpr uint32_t NRI_TRACE_SHADER_SPATIAL_COMPARE_POSITIVE_MISMATCH = NRI_TRACE_SHADER_SPATIAL_COMPARE_BASE + 3;
static constexpr uint32_t NRI_TRACE_SHADER_SPATIAL_COMPARE_PROBE_MISMATCH = NRI_TRACE_SHADER_SPATIAL_COMPARE_BASE + 4;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_QUERY_BASE =
	NRI_TRACE_SHADER_SPATIAL_COMPARE_BASE + NRI_TRACE_SHADER_SPATIAL_COMPARE_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_QUERY_COUNT = 16;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_QUERY_INITS = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 0;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_CANDIDATES = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 1;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_REFLECTION_IGNORES = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 2;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_CANDIDATE_COMMITS = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 3;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_UNEXPECTED_COMMITS = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 4;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_POSTCOMMIT_RESTARTS = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 5;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_COMPARE_TOTAL = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 6;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_COMPARE_EXCLUDED_PROBE = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 7;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_COMPARE_MATCH = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 8;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_COMPARE_HIT_MISS = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 9;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_COMPARE_IDENTITY = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 10;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_COMPARE_DISTANCE = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 11;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_COMPARE_SURFACE = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 12;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_COMPARE_PORTAL = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 13;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_COMPARE_TEMPORAL = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 14;
static constexpr uint32_t NRI_TRACE_SHADER_FILTER_COMPARE_SKIP_LIMIT = NRI_TRACE_SHADER_FILTER_QUERY_BASE + 15;
static constexpr uint32_t NRI_TRACE_SHADER_SURFACE_PROBE_BASE =
	NRI_TRACE_SHADER_FILTER_QUERY_BASE + NRI_TRACE_SHADER_FILTER_QUERY_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_SURFACE_PROBE_COUNT = 9;
static constexpr uint32_t NRI_TRACE_SHADER_SURFACE_PROBE_VALID = NRI_TRACE_SHADER_SURFACE_PROBE_BASE + 0;
static constexpr uint32_t NRI_TRACE_SHADER_SURFACE_PROBE_SOURCE = NRI_TRACE_SHADER_SURFACE_PROBE_BASE + 1;
static constexpr uint32_t NRI_TRACE_SHADER_SURFACE_PROBE_INSTANCE = NRI_TRACE_SHADER_SURFACE_PROBE_BASE + 2;
static constexpr uint32_t NRI_TRACE_SHADER_SURFACE_PROBE_PRIMITIVE = NRI_TRACE_SHADER_SURFACE_PROBE_BASE + 3;
static constexpr uint32_t NRI_TRACE_SHADER_SURFACE_PROBE_MATERIAL = NRI_TRACE_SHADER_SURFACE_PROBE_BASE + 4;
static constexpr uint32_t NRI_TRACE_SHADER_SURFACE_PROBE_TEXTURE = NRI_TRACE_SHADER_SURFACE_PROBE_BASE + 5;
static constexpr uint32_t NRI_TRACE_SHADER_SURFACE_PROBE_PALETTE = NRI_TRACE_SHADER_SURFACE_PROBE_BASE + 6;
static constexpr uint32_t NRI_TRACE_SHADER_SURFACE_PROBE_FLAGS = NRI_TRACE_SHADER_SURFACE_PROBE_BASE + 7;
static constexpr uint32_t NRI_TRACE_SHADER_SURFACE_PROBE_LIGHTING_FLAGS = NRI_TRACE_SHADER_SURFACE_PROBE_BASE + 8;
static constexpr uint32_t NRI_TRACE_SHADER_MOTION_AUDIT_BASE =
	NRI_TRACE_SHADER_SURFACE_PROBE_BASE + NRI_TRACE_SHADER_SURFACE_PROBE_COUNT;
static constexpr uint32_t NRI_TRACE_SHADER_MOTION_AUDIT_COUNT = 64;
static constexpr uint32_t NRI_TRACE_SHADER_MOTION_AUDIT_VALID = NRI_TRACE_SHADER_MOTION_AUDIT_BASE + 0;
static constexpr uint32_t NRI_TRACE_SHADER_MOTION_REASON_COUNTER_BASE = NRI_TRACE_SHADER_MOTION_AUDIT_BASE + 40;
static constexpr uint32_t NRI_TRACE_SHADER_MOTION_REASON_COUNTER_COUNT = 17;
static constexpr uint32_t NRI_TRACE_SHADER_INSTANCE_BUCKET_COUNT = 1024;
static constexpr uint32_t NRI_TRACE_SHADER_INSTANCE_COMMITTED_BASE =
	NRI_TRACE_SHADER_MOTION_AUDIT_BASE + NRI_TRACE_SHADER_MOTION_AUDIT_COUNT;
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
	uint32_t metadata2 = 0;
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

struct NRITraceShaderProbeAttribution
{
	bool valid = false;
	uint32_t instanceId = UINT32_MAX;
	uint32_t dataSource = UINT32_MAX;
	uint32_t metadata0 = 0;
	uint32_t metadata1 = 0;
	uint32_t metadata2 = 0;
};

struct NRITraceShaderSurfaceProbe
{
	bool valid = false;
	uint32_t dataSource = UINT32_MAX;
	uint32_t instanceId = UINT32_MAX;
	uint32_t primitiveIndex = UINT32_MAX;
	uint32_t materialIndex = UINT32_MAX;
	uint32_t textureIndex = UINT32_MAX;
	uint32_t paletteIndex = UINT32_MAX;
	uint32_t flags = 0;
	uint32_t lightingFlags = 0;
};

struct NRITraceShaderMotionAudit
{
	bool valid = false;
	uint32_t dataSource = UINT32_MAX;
	uint32_t instanceId = UINT32_MAX;
	uint32_t primitiveIndex = UINT32_MAX;
	float currentWorld[3] = {};
	float previousWorld[3] = {};
	float currentUv[2] = {};
	float previousUv[2] = {};
	float currentViewZ = 0.0f;
	float previousViewZ = 0.0f;
	float motion[4] = {};
	uint64_t surfaceId = 0;
	uint32_t generation = 0;
	uint32_t temporalFlags = 0;
	uint32_t validityReason = 0;
	uint32_t currentProjectionReason = 0;
	uint32_t previousProjectionReason = 0;
	uint32_t motionSource = 0;
	std::array<uint32_t, 4> sourceCounters = {};
	std::array<uint32_t, NRI_TRACE_SHADER_MOTION_REASON_COUNTER_COUNT> reasonCounters = {};
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
	NRITraceShaderProbeAttribution targetPixelAttribution;
	NRITraceShaderProbeAttribution referencePixelAttribution;
	std::array<NRITraceShaderProbeAttribution, NRI_TRACE_SHADER_RAY_KIND_COUNT> candidateAttribution = {};
	std::array<NRITraceShaderProbeAttribution, NRI_TRACE_SHADER_RAY_KIND_COUNT> finalAttribution = {};
	NRITraceShaderSurfaceProbe surfaceProbe;
	NRITraceShaderMotionAudit motionAudit;
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
