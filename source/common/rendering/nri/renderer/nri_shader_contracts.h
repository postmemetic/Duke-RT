#pragma once

#include "nri_exposure.h"
#include "nri_tlas_masks.h"

#include <cstddef>
#include <cstdint>

constexpr uint32_t NRI_MAX_SCENE_TEXTURES = 512;
constexpr uint32_t NRI_BLUE_NOISE_SCRAMBLING_RANKING_SLOT = 2 + NRI_MAX_SCENE_TEXTURES;
constexpr uint32_t NRI_BLUE_NOISE_SOBOL_SLOT = NRI_BLUE_NOISE_SCRAMBLING_RANKING_SLOT + 1;
constexpr uint32_t NRI_SCENE_DESCRIPTOR_NUM = NRI_BLUE_NOISE_SOBOL_SLOT + 1;
constexpr uint32_t NRI_SCENE_DATA_DESCRIPTOR_NUM = 28;
constexpr uint32_t NRI_SCENE_DATA_SPATIAL_ABSENCE_RAW_SLOT = 26;
constexpr uint32_t NRI_SCENE_DATA_SPATIAL_ABSENCE_TYPED_SLOT = 27;
constexpr uint32_t NRI_INPUT_DESCRIPTOR_NUM = 14;
constexpr uint32_t NRI_OUTPUT_DESCRIPTOR_NUM = 15;
constexpr uint32_t NRI_TRACE_SHADER_STATS_DESCRIPTOR_NUM = 1;
constexpr uint32_t NRI_SAMPLER_DESCRIPTOR_NUM = 4;

// The cache variant extends the ordinary five-set trace layout with one
// storage-buffer set. The ordinary TraceOpaque layout remains unchanged.
constexpr uint32_t NRI_INDIRECT_RADIANCE_CACHE_SET_INDEX = 5;
constexpr uint32_t NRI_INDIRECT_RADIANCE_CACHE_REGISTER_SPACE = 6;
constexpr uint32_t NRI_INDIRECT_RADIANCE_CACHE_DESCRIPTOR_NUM = 3;
constexpr uint32_t NRI_INDIRECT_RADIANCE_CACHE_RECORD_STRIDE = 48;
constexpr uint32_t NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_COUNT = 16;

enum NRIIndirectRadianceCacheTelemetryIndex : uint32_t
{
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_LOOKUPS = 0,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_FORCED_MISSES = 1,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_COLLISIONS = 2,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_STALE_GENERATION = 3,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_UNSUPPORTED_ROUTE = 4,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_EXACT_FALLBACK = 5,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_OCCUPANCY = 6,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_UPDATES = 7,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_CLEAR_COUNT = 8,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_LOOKUP_TIME = 9,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_UPDATE_TIME = 10,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_RESOLVE_TIME = 11,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_CLEAR_TIME = 12,
	NRI_INDIRECT_RADIANCE_CACHE_TELEMETRY_ACCEPTED_HITS = 13,
};

// Mirrors shaders/Include/IndirectRadianceCache.hlsli. StableKey must be
// derived from static semantic identity and quantized world-space evidence;
// Compatibility must never contain a transient TLAS instance index.
struct NRIIndirectRadianceCacheRecord
{
	uint32_t StableKey[2] = {};
	uint32_t Compatibility[2] = {};
	uint32_t QuantizedWorldPosition[2] = {};
	uint32_t PackedNormalAndRoute = 0;
	uint32_t MaterialFlags = 0;
	float IncidentRadiance[3] = {};
	uint32_t Metadata = 0;
};

static_assert(sizeof(NRIIndirectRadianceCacheRecord) == NRI_INDIRECT_RADIANCE_CACHE_RECORD_STRIDE,
	"Indirect-radiance cache records must match the HLSL contract.");

constexpr uint32_t NRI_FLAG_RESET_HISTORY = 0x1u;
constexpr uint32_t NRI_FLAG_USE_UPSCALED = 0x2u;
constexpr uint32_t NRI_FLAG_BOOTSTRAP_VIEW = 0x4u;
constexpr uint32_t NRI_FLAG_PRESENT_RAW_TRACE = 0x8u;
constexpr uint32_t NRI_FLAG_RAW_PRESENT_ADD_SECONDARY = 0x10u;
constexpr uint32_t NRI_FLAG_SPLIT_SHADOW_DENOISER = 0x20u;
constexpr uint32_t NRI_FLAG_USE_JITTER = 0x40u;
constexpr uint32_t NRI_FLAG_DIRECTIONAL_LIGHT = 0x80u;
constexpr uint32_t NRI_FLAG_FAST_EMISSIVE_SHADOW = 0x100u;
constexpr uint32_t NRI_FLAG_GATE_PRIMARY_VISIBLE_CHUNKS = 0x200u;
constexpr uint32_t NRI_FLAG_DIRECTIONAL_LIGHT_SHADOW = 0x400u;
constexpr uint32_t NRI_FLAG_TRACE_SHADER_STATS = 0x800u;
constexpr uint32_t NRI_FLAG_PROBABILISTIC_INDIRECT = 0x1000u;
constexpr uint32_t NRI_FLAG_INDIRECT_RADIANCE_CACHE = 0x2000u;
constexpr uint32_t NRI_FLAG_INDIRECT_RADIANCE_CACHE_ACCEPT = 0x4000u;
constexpr uint32_t NRI_FLAG_SPATIAL_ABSENCE_GATE = 0x8000u;
// Bit 23 is reserved from the packed jitter phase (current modes require at
// most 72 phases). Keeping actor gating in TraceOpaque flags avoids reading a
// stale spatial payload when the independently controlled actor gate is off.
constexpr uint32_t NRI_FLAG_SPATIAL_ACTOR_OCCURRENCE_GATE = 0x800000u;
// ReservedTrace1 bits 8..13 carry the emissive sample count. Bits 14 and 15
// enable the diagnostic comparator and generalized candidate query.
constexpr uint32_t NRI_TRACE_AUX_FILTER_COMPARE = 0x4000u;
constexpr uint32_t NRI_TRACE_AUX_FILTER_QUERY = 0x8000u;

constexpr uint32_t NRI_PRESENT_FLAG_SPLIT_SHADOW_DENOISER = NRI_FLAG_SPLIT_SHADOW_DENOISER;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_DISPLAY_INFO_AVAILABLE = 0x1u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_DISPLAY_HDR_SUPPORTED = 0x2u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE = 0x4u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_OFFSCREEN_HDR_TARGET = 0x8u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_AUTO_EXPOSURE = 0x10u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_EXPOSURE_TEXTURE_VALID = 0x20u;
constexpr uint32_t NRI_PRESENT_OUTPUT_FLAG_INPUT_PRE_EXPOSED = 0x40u;

constexpr uint32_t NRI_TEMPORAL_FLAG_AUTO_EXPOSURE = 0x1000u;
constexpr uint32_t NRI_TEMPORAL_FLAG_EXPOSURE_TEXTURE_VALID = 0x2000u;
constexpr uint32_t NRI_TEMPORAL_FLAG_VOLUME_REACTIVE = 0x4000u;
constexpr uint32_t NRI_JITTER_PHASE_SHIFT = 16u;
constexpr uint32_t NRI_JITTER_PHASE_MASK = 0x7fu;
constexpr uint32_t NRI_VOXEL_NORMAL_BLEND_SHIFT = 24u;
constexpr uint32_t NRI_TAA_JITTER_PHASE_COUNT = 8u;

constexpr uint32_t NRIPackTemporalJitterPhaseCount(uint32_t jitterPhaseCount)
{
	return ((jitterPhaseCount > NRI_JITTER_PHASE_MASK ? NRI_JITTER_PHASE_MASK : jitterPhaseCount) &
		NRI_JITTER_PHASE_MASK) << NRI_JITTER_PHASE_SHIFT;
}

static_assert(NRIPackTemporalJitterPhaseCount(72u) == 0x480000u);
static_assert((NRIPackTemporalJitterPhaseCount(127u) & NRI_FLAG_SPATIAL_ACTOR_OCCURRENCE_GATE) == 0u);

constexpr uint32_t NRI_RUNTIME_LIGHT_TILE_SIZE = 64u;
constexpr uint32_t NRI_PORTAL_FLAG_RUNTIME_BOUND = 0x1u;
constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_NONE = 0u;
constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE = 1u;
constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER = 2u;
constexpr uint32_t NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND = 3u;

// Mirrors shaders/Include/TraceConstants.hlsli.
struct NRITraceSceneConstants
{
	float CameraPos[3] = {};
	uint32_t RenderWidth = 0;
	float CameraForward[3] = {};
	uint32_t RenderHeight = 0;
	float CameraRight[3] = {};
	float TanHalfFovX = 1.0f;
	float CameraUp[3] = {};
	float TanHalfFovY = 1.0f;
	float PrevCameraPos[3] = {};
	uint32_t DisplayWidth = 0;
	float PrevCameraForward[3] = {};
	uint32_t DisplayHeight = 0;
	float PrevCameraRight[3] = {};
	float PrevTanHalfFovX = 1.0f;
	float PrevCameraUp[3] = {};
	float PrevTanHalfFovY = 1.0f;
	float LightDirection[3] = { 0.3f, 0.85f, -0.4f };
	uint32_t SceneInstanceCount = 0;
	float SkyColor[3] = { 0.38f, 0.48f, 0.65f };
	uint32_t DebugMode = 0;
	float GroundColor[3] = { 0.08f, 0.08f, 0.08f };
	uint32_t StaticPrimitiveCount = 0;
	uint32_t FrameIndex = 0;
	uint32_t DynamicPrimitiveCount = 0;
	uint32_t Flags = 0;
	uint32_t StaticMaterialCount = 0;
	uint32_t BootstrapMode = 0;
	uint32_t DynamicMaterialCount = 0;
	uint32_t BounceCounts = 0;
	uint32_t PortalCount = 0;
	uint32_t RuntimeLightCount = 0;
	uint32_t PortalDepth = 0;
	uint32_t ReservedTrace0 = 0;
	uint32_t ReservedTrace1 = 0;
};

// Mirrors shaders/Include/TemporalConstants.hlsli.
struct NRITemporalConstants
{
	uint32_t RenderWidth = 0;
	uint32_t RenderHeight = 0;
	uint32_t FrameIndex = 0;
	uint32_t Flags = 0;
	float Exposure = 1.0f;
};

// Mirrors shaders/Include/PresentConstants.hlsli.
struct NRIPresentConstants
{
	uint32_t InputWidth = 0;
	uint32_t InputHeight = 0;
	uint32_t DisplayWidth = 0;
	uint32_t DisplayHeight = 0;
	uint32_t PackedSceneOrigin = 0;
	uint32_t FrameIndex = 0;
	uint32_t DebugMode = 0;
	uint32_t Flags = 0;
	uint32_t DenoiserMode = 0;
	uint32_t OutputMode = 0;
	uint32_t TonemapMode = 0;
	uint32_t OutputFlags = 0;
	float Exposure = 1.0f;
	float Contrast = 1.0f;
	float Saturation = 1.0f;
	float Shoulder = 1.0f;
	float Toe = 1.0f;
	float PaperWhiteNits = 200.0f;
	float DisplayMaxLuminance = 80.0f;
	float DisplaySdrLuminance = 80.0f;
	uint32_t NightVisionPackedModeTint = 0;
	float NightVisionStrength = 0.0f;
	float NightVisionExposure = 1.0f;
	uint32_t NightVisionPackedControls = 0;
};

static_assert(sizeof(NRITraceSceneConstants) <= 224, "NRITraceSceneConstants must stay within the validated shared root-constant budget.");
static_assert(sizeof(NRITemporalConstants) <= 32, "NRITemporalConstants must stay compact.");
static_assert(sizeof(NRIPresentConstants) <= 96, "NRIPresentConstants must stay compact.");

// Mirrors shaders/Include/ExposureConstants.hlsli.
constexpr uint32_t NRI_EXPOSURE_SET_INPUTS = 0;
constexpr uint32_t NRI_EXPOSURE_SET_OUTPUTS = 1;
constexpr uint32_t NRI_EXPOSURE_SET_ROOT = 2;
constexpr uint32_t NRI_EXPOSURE_INPUT_BASE_REGISTER = 0;
constexpr uint32_t NRI_EXPOSURE_INPUT_DESCRIPTOR_NUM = 2;
constexpr uint32_t NRI_EXPOSURE_OUTPUT_TEXTURE_DESCRIPTOR_NUM = 1;
constexpr uint32_t NRI_EXPOSURE_OUTPUT_BUFFER_DESCRIPTOR_NUM = 2;
constexpr uint32_t NRI_EXPOSURE_OUTPUT_TEXTURE_BASE_REGISTER = 0;
constexpr uint32_t NRI_EXPOSURE_OUTPUT_BUFFER_BASE_REGISTER = 1;
constexpr uint32_t NRI_EXPOSURE_ROOT_REGISTER = 0;
constexpr uint32_t NRI_EXPOSURE_FLAG_FREEZE = 0x1u;
constexpr uint32_t NRI_EXPOSURE_FLAG_RESET = 0x2u;
constexpr uint32_t NRI_EXPOSURE_METERING_FULL = 0u;
constexpr uint32_t NRI_EXPOSURE_METERING_CENTER_WEIGHTED = 1u;
constexpr uint32_t NRI_EXPOSURE_METERING_BRIGHT_TAIL_SUPPRESSED = 2u;
constexpr uint32_t NRI_EXPOSURE_DEBUG_MAGIC = 0x45585033u;
constexpr uint32_t NRI_EXPOSURE_MAX_HISTOGRAM_BINS = 256u;
constexpr uint32_t NRI_EXPOSURE_DEBUG_WORD_COUNT = 16u;
constexpr float NRI_EXPOSURE_LOG_LUMINANCE_MIN = -12.0f;
constexpr float NRI_EXPOSURE_LOG_LUMINANCE_MAX = 12.0f;

struct NRIExposureConstants
{
	uint32_t RenderWidth = 0;
	uint32_t RenderHeight = 0;
	uint32_t FrameIndex = 0;
	uint32_t Flags = 0;
	uint32_t HistogramBinCount = 0;
	uint32_t SampleStep = 1;
	float DeltaTimeSeconds = 1.0f / 60.0f;
	uint32_t MeteringMode = 0;
	float LogLuminanceMin = NRI_EXPOSURE_LOG_LUMINANCE_MIN;
	float LogLuminanceMax = NRI_EXPOSURE_LOG_LUMINANCE_MAX;
	float InvLogLuminanceRange = 1.0f / 24.0f;
	float TargetLuminance = 0.18f;
	float MinExposure = 0.125f;
	float MaxExposure = 8.0f;
	float ExposureBias = 1.0f;
	float LowPercentile = 1.0f;
	float HighPercentile = 99.0f;
	float FallbackManualExposure = 1.0f;
	float AdaptUpSpeed = 3.0f;
	float AdaptDownSpeed = 1.0f;
};

static_assert(NRI_AUTO_EXPOSURE_MAX_HISTOGRAM_BINS == NRI_EXPOSURE_MAX_HISTOGRAM_BINS, "Exposure histogram capacity must match the shader contract.");
static_assert(NRI_AUTO_EXPOSURE_DEBUG_WORD_COUNT == NRI_EXPOSURE_DEBUG_WORD_COUNT, "Exposure debug readback size must match the shader contract.");
static_assert((uint32_t)NRIAutoExposureMeteringMode::FullFrame == NRI_EXPOSURE_METERING_FULL, "Exposure metering enum must match HLSL.");
static_assert((uint32_t)NRIAutoExposureMeteringMode::CenterWeighted == NRI_EXPOSURE_METERING_CENTER_WEIGHTED, "Exposure metering enum must match HLSL.");
static_assert((uint32_t)NRIAutoExposureMeteringMode::BrightTailSuppressed == NRI_EXPOSURE_METERING_BRIGHT_TAIL_SUPPRESSED, "Exposure metering enum must match HLSL.");
static_assert(sizeof(NRIExposureConstants) == 80, "NRIExposureConstants must match ExposureConstants.hlsli.");
static_assert(alignof(NRIExposureConstants) == 4, "NRIExposureConstants must remain scalar-aligned for HLSL root constants.");
static_assert(offsetof(NRIExposureConstants, RenderWidth) == 0, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, RenderHeight) == 4, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, FrameIndex) == 8, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, Flags) == 12, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, HistogramBinCount) == 16, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, SampleStep) == 20, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, DeltaTimeSeconds) == 24, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, MeteringMode) == 28, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, LogLuminanceMin) == 32, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, LogLuminanceMax) == 36, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, InvLogLuminanceRange) == 40, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, TargetLuminance) == 44, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, MinExposure) == 48, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, MaxExposure) == 52, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, ExposureBias) == 56, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, LowPercentile) == 60, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, HighPercentile) == 64, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, FallbackManualExposure) == 68, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, AdaptUpSpeed) == 72, "NRIExposureConstants layout mismatch.");
static_assert(offsetof(NRIExposureConstants, AdaptDownSpeed) == 76, "NRIExposureConstants layout mismatch.");

// Mirrors shaders/Include/BloomConstants.hlsli.
constexpr uint32_t NRI_BLOOM_SET_INPUTS = 0;
constexpr uint32_t NRI_BLOOM_SET_OUTPUTS = 1;
constexpr uint32_t NRI_BLOOM_SET_ROOT = 2;
constexpr uint32_t NRI_BLOOM_INPUT_DESCRIPTOR_NUM = 2;
constexpr uint32_t NRI_BLOOM_OUTPUT_DESCRIPTOR_NUM = 1;
constexpr uint32_t NRI_BLOOM_ROOT_REGISTER = 0;
constexpr uint32_t NRI_BLOOM_FLAG_THRESHOLD = 0x1u;
constexpr uint32_t NRI_BLOOM_FLAG_ENERGY_CONSTRAINED = 0x2u;
constexpr uint32_t NRI_BLOOM_FLAG_DEBUG = 0x4u;

struct NRIBloomConstants
{
	uint32_t InputWidth = 0;
	uint32_t InputHeight = 0;
	uint32_t OutputWidth = 0;
	uint32_t OutputHeight = 0;
	float Intensity = 0.0f;
	float Sigma = 0.0f;
	float Cutoff = 0.0f;
	float Fuzziness = 0.0f;
	uint32_t FrameIndex = 0;
	uint32_t LevelIndex = 0;
	uint32_t LevelCount = 0;
	uint32_t Flags = 0;
	float InputTexelSizeX = 0.0f;
	float InputTexelSizeY = 0.0f;
	float Reserved0 = 0.0f;
	float Reserved1 = 0.0f;
};

static_assert(sizeof(NRIBloomConstants) == 64, "NRIBloomConstants must match BloomConstants.hlsli.");
static_assert(alignof(NRIBloomConstants) == 4, "NRIBloomConstants must remain scalar-aligned for HLSL root constants.");
static_assert(offsetof(NRIBloomConstants, InputWidth) == 0, "NRIBloomConstants layout mismatch.");
static_assert(offsetof(NRIBloomConstants, Intensity) == 16, "NRIBloomConstants layout mismatch.");
static_assert(offsetof(NRIBloomConstants, FrameIndex) == 32, "NRIBloomConstants layout mismatch.");
static_assert(offsetof(NRIBloomConstants, InputTexelSizeX) == 48, "NRIBloomConstants layout mismatch.");

// Mirrors shaders/Include/VoxelComputeConstants.hlsli.
constexpr uint32_t NRI_VOXEL_COMPUTE_SET_INPUTS = 0;
constexpr uint32_t NRI_VOXEL_COMPUTE_SET_OUTPUTS = 1;
constexpr uint32_t NRI_VOXEL_COMPUTE_SET_ROOT = 2;
constexpr uint32_t NRI_VOXEL_COMPUTE_INPUT_DESCRIPTOR_NUM = 2;
constexpr uint32_t NRI_VOXEL_COMPUTE_FACE_DESCRIPTOR_NUM = 2;
constexpr uint32_t NRI_VOXEL_COMPUTE_RESULT_DESCRIPTOR_NUM = 1;
constexpr uint32_t NRI_VOXEL_COMPUTE_EMIT_OUTPUT_DESCRIPTOR_NUM = 3;
constexpr uint32_t NRI_VOXEL_COMPUTE_SCRATCH_DESCRIPTOR_NUM = 1;
constexpr uint32_t NRI_VOXEL_COMPUTE_ROOT_REGISTER = 0;
constexpr uint32_t NRI_VOXEL_COMPUTE_STATUS_COUNT_OK = 1;
constexpr uint32_t NRI_VOXEL_COMPUTE_STATUS_COUNT_MISMATCH = 2;
constexpr uint32_t NRI_VOXEL_COMPUTE_STATUS_EMIT_OK = 3;
constexpr uint32_t NRI_VOXEL_COMPUTE_STATUS_EMIT_MISMATCH = 4;
constexpr uint32_t NRI_VOXEL_COMPUTE_STATUS_SCAN_OK = 5;
constexpr uint32_t NRI_VOXEL_COMPUTE_STATUS_SCAN_MISMATCH = 6;
constexpr uint32_t NRI_VOXEL_COMPUTE_MISMATCH_OUTPUT_OVERFLOW = 1u << 5;
constexpr uint32_t NRI_VOXEL_COMPUTE_MISMATCH_SOURCE_RANGE = 1u << 6;
constexpr uint32_t NRI_VOXEL_COMPUTE_MISMATCH_COLOR_RUN_RANGE = 1u << 7;
constexpr uint32_t NRI_VOXEL_COMPUTE_MISMATCH_SCRATCH_RANGE = 1u << 8;
constexpr uint32_t NRI_VOXEL_COMPUTE_MISMATCH_ARITHMETIC_OVERFLOW = 1u << 9;
constexpr uint32_t NRI_VOXEL_COMPUTE_MISMATCH_SLAB_EMIT_COUNT = 1u << 10;
constexpr uint32_t NRI_VOXEL_COMPUTE_MISMATCH_ALGORITHM = 1u << 11;

struct NRIVoxelComputeConstants
{
	uint32_t JobCount = 0;
	uint32_t SlabRecordCount = 0;
	uint32_t FaceRecordCount = 0;
	uint32_t ColorRunRecordCount = 0;
	uint32_t ScratchRecordCount = 0;
	uint32_t MaxSlabsPerJob = 0;
	uint32_t AlgorithmVersion = 0;
	uint32_t Reserved0 = 0;
	uint32_t VertexRecordCount = 0;
	uint32_t IndexRecordCount = 0;
	uint32_t PrimitiveRecordCount = 0;
	uint32_t Reserved1 = 0;
};

struct NRIVoxelComputeJob
{
	uint32_t SlabOffset = 0;
	uint32_t SlabCount = 0;
	uint32_t FaceOffset = 0;
	uint32_t ExpectedFaces = 0;
	uint32_t ExpectedIndices = 0;
	uint32_t ExpectedVerticesNoDedupe = 0;
	uint32_t ExpectedVoxels = 0;
	uint32_t JobId = 0;
	uint32_t VertexOffset = 0;
	uint32_t IndexOffset = 0;
	uint32_t PrimitiveOffset = 0;
	float PivotX = 0.0f;
	float PivotY = 0.0f;
	float PivotZ = 0.0f;
	uint32_t MaterialBase = 0;
	uint32_t VertexCapacity = 0;
	uint32_t IndexCapacity = 0;
	uint32_t PrimitiveCapacity = 0;
	uint32_t ScratchOffset = 0;
	uint32_t ScratchCount = 0;
};

struct NRIVoxelComputeSlabRecord
{
	uint32_t X = 0;
	uint32_t Y = 0;
	uint32_t ZTop = 0;
	uint32_t CullMask = 0;
	uint32_t ZLength = 0;
	uint32_t ColorRunCount = 0;
	uint32_t ColorRunOffset = 0;
	uint32_t Reserved0 = 0;
};

struct NRIVoxelComputeColorRunRecord
{
	uint32_t ZOffset = 0;
	uint32_t ZLength = 0;
	uint32_t Color = 0;
	uint32_t Reserved0 = 0;
};

struct NRIVoxelComputeResult
{
	uint32_t FaceCount = 0;
	uint32_t IndexCount = 0;
	uint32_t VertexCountNoDedupe = 0;
	uint32_t VoxelCount = 0;
	uint32_t SlabCount = 0;
	uint32_t PrimitiveCount = 0;
	uint32_t MismatchMask = 0;
	uint32_t JobId = 0;
	uint32_t Status = 0;
	uint32_t VertexHash = 0;
	uint32_t IndexHash = 0;
	uint32_t PrimitiveHash = 0;
};

struct NRIVoxelComputeSlabScratch
{
	uint32_t FaceCount = 0;
	uint32_t FaceOffset = 0;
	uint32_t VoxelCount = 0;
	uint32_t StatusMask = 0;
};

struct NRIVoxelComputeFaceRecord
{
	int32_t X[4] = {};
	int32_t Y[4] = {};
	int32_t Z[4] = {};
	uint32_t Color = 0;
	uint32_t MaterialIndex = 0;
};

struct NRIVoxelComputeSceneVertex
{
	float Position[3] = {};
	float PrevPosition[3] = {};
	float Uv[2] = {};
};

struct NRIVoxelComputePrimitiveData
{
	uint32_t Indices[3] = {};
	uint32_t MaterialIndex = 0;
	float Uv0[2] = {};
	float Uv1[2] = {};
	float Uv2[2] = {};
	float Normal[3] = {};
	uint32_t Flags = 0;
	uint32_t PortalIndex = UINT32_MAX;
	uint32_t Reserved0 = UINT32_MAX;
	uint32_t SmoothNormals[2] = {};
};

static_assert(sizeof(NRIVoxelComputeConstants) == 48, "NRIVoxelComputeConstants must match VoxelComputeConstants.hlsli.");
static_assert(sizeof(NRIVoxelComputeJob) == 80, "NRIVoxelComputeJob must match VoxelComputeConstants.hlsli.");
static_assert(sizeof(NRIVoxelComputeSlabRecord) == 32, "NRIVoxelComputeSlabRecord must match VoxelComputeConstants.hlsli.");
static_assert(sizeof(NRIVoxelComputeColorRunRecord) == 16, "NRIVoxelComputeColorRunRecord must match VoxelComputeConstants.hlsli.");
static_assert(sizeof(NRIVoxelComputeResult) == 48, "NRIVoxelComputeResult must match VoxelComputeConstants.hlsli.");
static_assert(sizeof(NRIVoxelComputeSlabScratch) == 16, "NRIVoxelComputeSlabScratch must match VoxelComputeConstants.hlsli.");
static_assert(sizeof(NRIVoxelComputeFaceRecord) == 56, "NRIVoxelComputeFaceRecord must match VoxelComputeConstants.hlsli.");
static_assert(sizeof(NRIVoxelComputeSceneVertex) == 32, "NRIVoxelComputeSceneVertex must match VoxelComputeConstants.hlsli.");
static_assert(sizeof(NRIVoxelComputePrimitiveData) == 72, "NRIVoxelComputePrimitiveData must match VoxelComputeConstants.hlsli.");
