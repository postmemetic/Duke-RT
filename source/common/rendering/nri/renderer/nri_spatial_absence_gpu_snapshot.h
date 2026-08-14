#pragma once

#include "nri_spatial_absence_gate.h"

#include <cstdint>
#include <string>
#include <vector>

// CPU-owned mirror of one HLSL uint4. Every typed spatial-absence section uses
// this one stride so upload and shader address arithmetic cannot disagree.
struct NRISpatialAbsenceGpuBlock
{
	uint32_t words[4] = {};
};

static_assert(sizeof(NRISpatialAbsenceGpuBlock) == 16,
	"Typed spatial-absence blocks must match HLSL uint4.");

constexpr uint32_t NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_MAGIC = 0x5341474eu; // "NGAS"
constexpr uint32_t NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_VERSION = 1u;
constexpr uint32_t NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_HEADER_BLOCKS = 6u;
constexpr uint32_t NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FOOTER_BLOCKS = 3u;
constexpr uint32_t NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_BLOCKS = 1u;
constexpr uint32_t NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_NEGATIVE_BLOCKS = 2u;
constexpr uint32_t NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_PAIR_BLOCKS = 2u;
constexpr uint32_t NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FOOTPRINT_BLOCKS = 2u;
constexpr uint32_t NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CELL_BLOCKS = 1u;
constexpr uint32_t NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_TRIANGLE_BLOCKS = 2u;

enum NRISpatialAbsenceGpuSnapshotFlags : uint32_t
{
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_COMPLETE = 1u << 0,
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_SEALED = 1u << 1,
};

enum NRISpatialAbsenceGpuSnapshotChunkFlags : uint32_t
{
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_REACHED = 1u << 0,
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_NEGATIVE = 1u << 1,
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_FOOTPRINT = 1u << 2,
};

enum NRISpatialAbsenceGpuSnapshotCellFlags : uint32_t
{
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CELL_INTERIOR = 1u << 0,
};

enum NRISpatialAbsenceGpuSnapshotFailOpenFlags : uint32_t
{
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_NONE = 0,
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_UNAVAILABLE = 1u << 0,
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_UNSEALED = 1u << 1,
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_NOT_AUTHORITATIVE = 1u << 2,
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_HASH_MISMATCH = 1u << 3,
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_INVALID = 1u << 4,
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_INDEX_OVERFLOW = 1u << 5,
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_LAYOUT_INVALID = 1u << 6,
	NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SEAL_INVALID = 1u << 7,
};

// Offsets are derived from counts in fixed section order. They are published as
// a helper for upload/tests, but are intentionally not duplicated in the GPU
// header where a stale offset could create a second source of layout authority.
struct NRISpatialAbsenceGpuSnapshotLayout
{
	uint32_t chunkOffset = NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_HEADER_BLOCKS;
	uint32_t negativeOffset = 0;
	uint32_t pairOffset = 0;
	uint32_t footprintOffset = 0;
	uint32_t cellOffset = 0;
	uint32_t referenceOffset = 0;
	uint32_t triangleOffset = 0;
	uint32_t footerOffset = 0;
	uint32_t referenceBlockCount = 0;
	uint32_t totalBlockCount = 0;
};

struct NRISpatialAbsenceGpuSnapshot
{
	bool valid = false;
	uint32_t failOpenFlags = NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_NONE;
	uint32_t frameIndex = 0;
	uint64_t worldGeneration = 0;
	uint64_t captureSerial = 0;
	uint64_t sourcePayloadHash = 0;
	uint64_t payloadHash = 0;
	float center[3] = {};
	float guardRadius = 0.0f;
	uint32_t chunkCount = 0;
	uint32_t negativeCount = 0;
	uint32_t pairCount = 0;
	uint32_t footprintCount = 0;
	uint32_t cellCount = 0;
	uint32_t referenceCount = 0;
	uint32_t triangleCount = 0;
	double buildElapsedMilliseconds = 0.0;
	std::vector<NRISpatialAbsenceGpuBlock> blocks;

	bool HasCensusAuthority(const NRISpatialAbsenceSnapshot& source) const;
	bool HasNegativeAuthority(const NRISpatialAbsenceSnapshot& source) const;
};

bool BuildNRISpatialAbsenceGpuSnapshot(
	const NRISpatialAbsenceSnapshot& source,
	NRISpatialAbsenceGpuSnapshot& destination);

bool ValidateNRISpatialAbsenceGpuSnapshot(
	const NRISpatialAbsenceGpuSnapshot& snapshot);

bool GetNRISpatialAbsenceGpuSnapshotLayout(
	const NRISpatialAbsenceGpuSnapshot& snapshot,
	NRISpatialAbsenceGpuSnapshotLayout& layout);

// Device-independent contract checks for the typed conversion and fail-open
// publication rules. The caller owns how failures are surfaced.
bool RunNRISpatialAbsenceGpuSnapshotSelfTests(std::string* failureReason = nullptr);
