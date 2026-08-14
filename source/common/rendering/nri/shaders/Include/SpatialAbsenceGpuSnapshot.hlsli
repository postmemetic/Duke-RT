#ifndef RAZE_NRI_PT_SPATIAL_ABSENCE_GPU_SNAPSHOT_HLSLI
#define RAZE_NRI_PT_SPATIAL_ABSENCE_GPU_SNAPSHOT_HLSLI

// Version 1 deliberately preserves the current triangle-vertex predicate.
// Precomputed half spaces can be added as a separately versioned section once
// the typed publication path has demonstrated decision parity.
static const uint SPATIAL_ABSENCE_GPU_MAGIC = 0x5341474eu; // "NGAS"
static const uint SPATIAL_ABSENCE_GPU_VERSION = 1u;
static const uint SPATIAL_ABSENCE_GPU_HEADER_BLOCKS = 6u;
static const uint SPATIAL_ABSENCE_GPU_FOOTER_BLOCKS = 3u;
static const uint SPATIAL_ABSENCE_GPU_BLOCK_STRIDE = 16u;

static const uint SPATIAL_ABSENCE_GPU_SNAPSHOT_COMPLETE = 1u << 0u;
static const uint SPATIAL_ABSENCE_GPU_SNAPSHOT_SEALED = 1u << 1u;
static const uint SPATIAL_ABSENCE_GPU_REQUIRED_SNAPSHOT_FLAGS =
	SPATIAL_ABSENCE_GPU_SNAPSHOT_COMPLETE | SPATIAL_ABSENCE_GPU_SNAPSHOT_SEALED;

static const uint SPATIAL_ABSENCE_GPU_CHUNK_REACHED = 1u << 0u;
static const uint SPATIAL_ABSENCE_GPU_CHUNK_NEGATIVE = 1u << 1u;
static const uint SPATIAL_ABSENCE_GPU_CHUNK_FOOTPRINT = 1u << 2u;
static const uint SPATIAL_ABSENCE_GPU_CHUNK_KNOWN_FLAGS =
	SPATIAL_ABSENCE_GPU_CHUNK_REACHED | SPATIAL_ABSENCE_GPU_CHUNK_NEGATIVE |
	SPATIAL_ABSENCE_GPU_CHUNK_FOOTPRINT;
static const uint SPATIAL_ABSENCE_GPU_CELL_INTERIOR = 1u << 0u;

// t27 is the next scene-data slot after the raw compatibility payload at t26.
// The renderer descriptor layout intentionally remains an integration step.
StructuredBuffer<uint4> gSpatialAbsenceGpuSnapshot : register(t27, space2);

struct SpatialAbsenceGpuView
{
	uint blockCount;
	uint chunkCount;
	uint negativeCount;
	uint pairCount;
	uint footprintCount;
	uint cellCount;
	uint referenceCount;
	uint triangleCount;
	uint chunkBase;
	uint negativeBase;
	uint pairBase;
	uint footprintBase;
	uint cellBase;
	uint referenceBase;
	uint triangleBase;
	uint footerBase;
	uint worldGenerationLo;
	uint worldGenerationHi;
	uint captureSerialLo;
	uint captureSerialHi;
	uint sourceRawHashLo;
	uint sourceRawHashHi;
	float3 center;
	float radius;
};

struct SpatialAbsenceGpuFootprint
{
	uint triangleFirst;
	uint triangleCount;
	uint cellFirst;
	uint width;
	uint height;
	float2 boundsMin;
	float2 boundsMax;
};

bool SpatialAbsenceGpuTryAppend(uint count, uint blocksPerRecord, inout uint cursor)
{
	if (blocksPerRecord != 0u && count > (0xffffffffu - cursor) / blocksPerRecord)
		return false;
	cursor += count * blocksPerRecord;
	return true;
}

bool SpatialAbsenceGpuTryReferenceBlocks(uint referenceCount, out uint blockCount)
{
	blockCount = 0u;
	if (referenceCount > 0xfffffffcu)
		return false;
	blockCount = (referenceCount + 3u) / 4u;
	return true;
}

bool LoadSpatialAbsenceGpuView(out SpatialAbsenceGpuView view, out uint failureOutcome)
{
	view = (SpatialAbsenceGpuView)0;
	failureOutcome = SPATIAL_PROBE_OUTCOME_SNAPSHOT_INVALID;

	uint resourceBlocks = 0u;
	uint resourceStride = 0u;
	gSpatialAbsenceGpuSnapshot.GetDimensions(resourceBlocks, resourceStride);
	if (resourceStride != SPATIAL_ABSENCE_GPU_BLOCK_STRIDE ||
		resourceBlocks < SPATIAL_ABSENCE_GPU_HEADER_BLOCKS + SPATIAL_ABSENCE_GPU_FOOTER_BLOCKS)
		return false;

	const uint4 header0 = gSpatialAbsenceGpuSnapshot[0u];
	const uint4 header1 = gSpatialAbsenceGpuSnapshot[1u];
	const uint4 header2 = gSpatialAbsenceGpuSnapshot[2u];
	const uint4 header3 = gSpatialAbsenceGpuSnapshot[3u];
	const uint4 header4 = gSpatialAbsenceGpuSnapshot[4u];
	const uint4 header5 = gSpatialAbsenceGpuSnapshot[5u];
	if (header0.x != SPATIAL_ABSENCE_GPU_MAGIC ||
		header0.y != SPATIAL_ABSENCE_GPU_VERSION ||
		(header0.z & SPATIAL_ABSENCE_GPU_REQUIRED_SNAPSHOT_FLAGS) !=
			SPATIAL_ABSENCE_GPU_REQUIRED_SNAPSHOT_FLAGS ||
		header0.w > resourceBlocks ||
		header0.w < SPATIAL_ABSENCE_GPU_HEADER_BLOCKS + SPATIAL_ABSENCE_GPU_FOOTER_BLOCKS ||
		header4.w != SPATIAL_ABSENCE_GPU_HEADER_BLOCKS)
		return false;
	if (header1.x != gTraceConstants.FrameIndex)
	{
		failureOutcome = SPATIAL_PROBE_OUTCOME_FRAME_MISMATCH;
		return false;
	}

	uint cursor = SPATIAL_ABSENCE_GPU_HEADER_BLOCKS;
	view.chunkBase = cursor;
	if (!SpatialAbsenceGpuTryAppend(header1.y, 1u, cursor)) return false;
	view.negativeBase = cursor;
	if (!SpatialAbsenceGpuTryAppend(header1.z, 2u, cursor)) return false;
	view.pairBase = cursor;
	if (!SpatialAbsenceGpuTryAppend(header1.w, 2u, cursor)) return false;
	view.footprintBase = cursor;
	if (!SpatialAbsenceGpuTryAppend(header2.x, 2u, cursor)) return false;
	view.cellBase = cursor;
	if (!SpatialAbsenceGpuTryAppend(header2.y, 1u, cursor)) return false;
	view.referenceBase = cursor;
	uint referenceBlocks = 0u;
	if (!SpatialAbsenceGpuTryReferenceBlocks(header2.z, referenceBlocks) ||
		!SpatialAbsenceGpuTryAppend(referenceBlocks, 1u, cursor))
		return false;
	view.triangleBase = cursor;
	if (!SpatialAbsenceGpuTryAppend(header2.w, 2u, cursor)) return false;
	view.footerBase = cursor;
	if (!SpatialAbsenceGpuTryAppend(1u, SPATIAL_ABSENCE_GPU_FOOTER_BLOCKS, cursor) ||
		cursor != header0.w || header4.z != view.footerBase)
		return false;

	const uint4 footer0 = gSpatialAbsenceGpuSnapshot[view.footerBase + 0u];
	const uint4 footer1 = gSpatialAbsenceGpuSnapshot[view.footerBase + 1u];
	const uint4 footer2 = gSpatialAbsenceGpuSnapshot[view.footerBase + 2u];
	if (any(footer0 != header0) ||
		footer1.x != header1.x || footer1.y != header3.x ||
		footer1.z != header3.y || footer1.w != header3.z ||
		footer2.x != header3.w || footer2.y != header4.x ||
		footer2.z != header4.y || footer2.w != header4.z)
		return false;

	view.blockCount = header0.w;
	view.chunkCount = header1.y;
	view.negativeCount = header1.z;
	view.pairCount = header1.w;
	view.footprintCount = header2.x;
	view.cellCount = header2.y;
	view.referenceCount = header2.z;
	view.triangleCount = header2.w;
	view.worldGenerationLo = header3.x;
	view.worldGenerationHi = header3.y;
	view.captureSerialLo = header3.z;
	view.captureSerialHi = header3.w;
	view.sourceRawHashLo = header4.x;
	view.sourceRawHashHi = header4.y;
	view.center = asfloat(header5.xyz);
	view.radius = asfloat(header5.w);
	if (view.negativeCount > view.chunkCount ||
		(view.worldGenerationLo | view.worldGenerationHi) == 0u ||
		(view.captureSerialLo | view.captureSerialHi) == 0u ||
		(view.sourceRawHashLo | view.sourceRawHashHi) == 0u ||
		!all(isfinite(view.center)) || !isfinite(view.radius) || view.radius <= 0.0)
		return false;

	return true;
}

bool LoadSpatialAbsenceGpuChunk(
	SpatialAbsenceGpuView view,
	uint chunkIndex,
	out uint4 chunk)
{
	chunk = 0u;
	if (chunkIndex >= view.chunkCount)
		return false;
	chunk = gSpatialAbsenceGpuSnapshot[view.chunkBase + chunkIndex];
	return chunk.w == 0u && (chunk.x & ~SPATIAL_ABSENCE_GPU_CHUNK_KNOWN_FLAGS) == 0u;
}

bool SpatialAbsenceGpuChunkIsCertified(SpatialAbsenceGpuView view, uint chunkIndex)
{
	uint4 chunk;
	return LoadSpatialAbsenceGpuChunk(view, chunkIndex, chunk) &&
		(chunk.x & (SPATIAL_ABSENCE_GPU_CHUNK_NEGATIVE | SPATIAL_ABSENCE_GPU_CHUNK_FOOTPRINT)) ==
			(SPATIAL_ABSENCE_GPU_CHUNK_NEGATIVE | SPATIAL_ABSENCE_GPU_CHUNK_FOOTPRINT) &&
		chunk.y < view.negativeCount && chunk.z < view.footprintCount;
}

bool SpatialAbsenceGpuChunkWasReached(SpatialAbsenceGpuView view, uint chunkIndex)
{
	uint4 chunk;
	return LoadSpatialAbsenceGpuChunk(view, chunkIndex, chunk) &&
		(chunk.x & SPATIAL_ABSENCE_GPU_CHUNK_REACHED) != 0u;
}

bool LoadSpatialAbsenceGpuFootprint(
	SpatialAbsenceGpuView view,
	uint footprintIndex,
	out SpatialAbsenceGpuFootprint footprint)
{
	footprint = (SpatialAbsenceGpuFootprint)0;
	if (footprintIndex >= view.footprintCount)
		return false;
	const uint blockIndex = view.footprintBase + footprintIndex * 2u;
	const uint4 data = gSpatialAbsenceGpuSnapshot[blockIndex + 0u];
	const float4 bounds = asfloat(gSpatialAbsenceGpuSnapshot[blockIndex + 1u]);
	footprint.triangleFirst = data.x;
	footprint.triangleCount = data.y;
	footprint.cellFirst = data.z;
	footprint.width = data.w & 0xffffu;
	footprint.height = data.w >> 16u;
	footprint.boundsMin = bounds.xy;
	footprint.boundsMax = bounds.zw;
	if (footprint.width == 0u || footprint.height == 0u ||
		footprint.width > SPATIAL_ABSENCE_GRID_MAX_DIMENSION ||
		footprint.height > SPATIAL_ABSENCE_GRID_MAX_DIMENSION ||
		footprint.width > 0xffffffffu / footprint.height ||
		footprint.triangleFirst > view.triangleCount ||
		footprint.triangleCount > view.triangleCount - footprint.triangleFirst ||
		footprint.cellFirst > view.cellCount ||
		footprint.width * footprint.height > view.cellCount - footprint.cellFirst ||
		!all(isfinite(bounds)) || any(footprint.boundsMin >= footprint.boundsMax))
		return false;
	return true;
}

bool LoadSpatialAbsenceGpuTriangle(
	SpatialAbsenceGpuView view,
	uint triangleIndex,
	out float2 first,
	out float2 second,
	out float2 third)
{
	first = 0.0;
	second = 0.0;
	third = 0.0;
	if (triangleIndex >= view.triangleCount)
		return false;
	const uint blockIndex = view.triangleBase + triangleIndex * 2u;
	const float4 vertices01 = asfloat(gSpatialAbsenceGpuSnapshot[blockIndex + 0u]);
	const float4 vertex2 = asfloat(gSpatialAbsenceGpuSnapshot[blockIndex + 1u]);
	first = vertices01.xy;
	second = vertices01.zw;
	third = vertex2.xy;
	return all(isfinite(vertices01)) && all(isfinite(vertex2.xy)) &&
		vertex2.z == 0.0 && vertex2.w == 0.0;
}

void SpatialAbsenceGpuStatAdd(bool enabled, uint index, uint value)
{
	if (enabled)
		TraceShaderStatAdd(index, value);
}

bool PointInSpatialAbsenceGpuFootprint(
	SpatialAbsenceGpuView view,
	uint footprintIndex,
	float2 samplePoint,
	bool collectProbeDetails,
	bool recordStats,
	out bool recordsValid,
	out SpatialFootprintProbeDetails probeDetails)
{
	probeDetails = EmptySpatialFootprintProbeDetails();
	SpatialAbsenceGpuFootprint footprint;
	recordsValid = LoadSpatialAbsenceGpuFootprint(view, footprintIndex, footprint);
	if (!recordsValid || !all(isfinite(samplePoint)))
		return false;

	const float gridEpsilon = SPATIAL_ABSENCE_FOOTPRINT_PREDICATE_EPSILON;
	if (any(samplePoint < footprint.boundsMin - gridEpsilon) ||
		any(samplePoint > footprint.boundsMax + gridEpsilon))
	{
		probeDetails.stage = SPATIAL_FOOTPRINT_PROBE_GRID_BOUNDS;
		return false;
	}
	const float2 gridCoordinate = saturate(
		(samplePoint - footprint.boundsMin) / (footprint.boundsMax - footprint.boundsMin));
	const uint cellX = min((uint)floor(gridCoordinate.x * (float)footprint.width), footprint.width - 1u);
	const uint cellY = min((uint)floor(gridCoordinate.y * (float)footprint.height), footprint.height - 1u);
	const uint cellOffset = cellY * footprint.width + cellX;
	const uint4 cell = gSpatialAbsenceGpuSnapshot[view.cellBase + footprint.cellFirst + cellOffset];
	const bool interiorCell = (cell.w & SPATIAL_ABSENCE_GPU_CELL_INTERIOR) != 0u;
	if ((cell.w & ~SPATIAL_ABSENCE_GPU_CELL_INTERIOR) != 0u ||
		cell.x > view.referenceCount || cell.y > view.referenceCount - cell.x)
	{
		recordsValid = false;
		return false;
	}
	if ((interiorCell && (cell.y != 0u || cell.z >= footprint.triangleCount)) ||
		(!interiorCell && cell.z != 0xffffffffu))
	{
		recordsValid = false;
		return false;
	}
	probeDetails.cellReferenceCount = cell.y;

	bool inside = false;
	if (interiorCell)
	{
		float2 first, second, third;
		if (!LoadSpatialAbsenceGpuTriangle(
			view, footprint.triangleFirst + cell.z, first, second, third))
		{
			recordsValid = false;
			return false;
		}
		SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_WITNESS_TESTS, 1u);
		float certificateMargin = -3.402823466e+38;
		inside = collectProbeDetails ?
			PointInSpatialTriangle(samplePoint, first, second, third, certificateMargin) :
			PointInSpatialTriangleFast(samplePoint, first, second, third);
		if (collectProbeDetails)
		{
			probeDetails.bestMargin = certificateMargin;
			probeDetails.bestTriangle = cell.z;
		}
	}
	if (cell.y == 0u)
	{
		if (!inside)
			probeDetails.stage = SPATIAL_FOOTPRINT_PROBE_EMPTY_CELL;
		return inside;
	}

	[loop]
	for (uint referenceOffset = 0u; referenceOffset < cell.y && !inside; ++referenceOffset)
	{
		const uint logicalReference = cell.x + referenceOffset;
		const uint4 packedReferences =
			gSpatialAbsenceGpuSnapshot[view.referenceBase + logicalReference / 4u];
		const uint triangleOffset = packedReferences[logicalReference & 3u];
		if (triangleOffset >= footprint.triangleCount)
		{
			recordsValid = false;
			return false;
		}
		float2 first, second, third;
		if (!LoadSpatialAbsenceGpuTriangle(
			view, footprint.triangleFirst + triangleOffset, first, second, third))
		{
			recordsValid = false;
			return false;
		}
		SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_WITNESS_TESTS, 1u);
		float edgeMargin = -3.402823466e+38;
		inside = collectProbeDetails ?
			PointInSpatialTriangle(samplePoint, first, second, third, edgeMargin) :
			PointInSpatialTriangleFast(samplePoint, first, second, third);
		if (collectProbeDetails && edgeMargin > probeDetails.bestMargin)
		{
			probeDetails.bestMargin = edgeMargin;
			probeDetails.bestTriangle = triangleOffset;
		}
	}
	if (!inside)
		probeDetails.stage = SPATIAL_FOOTPRINT_PROBE_TRIANGLE_MISS;
	return inside;
}

uint EvaluateTypedSpatialAbsencePrevalidated(
	SpatialAbsenceGpuView view,
	uint chunkIndex,
	float3 worldPosition,
	uint statsKind,
	bool evaluationEnabled,
	bool exactNegativeSurfaceMembership,
	bool collectProbeDetails,
	bool recordStats,
	out uint matchedPositiveChunk,
	out SpatialFootprintProbeDetails footprintProbeDetails)
{
	matchedPositiveChunk = 0xffffffffu;
	footprintProbeDetails = EmptySpatialFootprintProbeDetails();
	if (!evaluationEnabled || chunkIndex == 0xffffffffu)
		return SPATIAL_PROBE_OUTCOME_DISABLED;

	if (chunkIndex >= view.chunkCount)
	{
		SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_SNAPSHOT_FAIL_OPEN, 1u);
		SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_SNAPSHOT_INVALID, 1u);
		return SPATIAL_PROBE_OUTCOME_SNAPSHOT_INVALID;
	}

	const float3 centerDelta = worldPosition - view.center;
	if (dot(centerDelta, centerDelta) > view.radius * view.radius)
	{
		SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_OUTSIDE_GUARD, 1u);
		return SPATIAL_PROBE_OUTCOME_OUTSIDE_GUARD;
	}

	uint4 chunk;
	if (!LoadSpatialAbsenceGpuChunk(view, chunkIndex, chunk) ||
		(chunk.x & (SPATIAL_ABSENCE_GPU_CHUNK_NEGATIVE | SPATIAL_ABSENCE_GPU_CHUNK_FOOTPRINT)) !=
			(SPATIAL_ABSENCE_GPU_CHUNK_NEGATIVE | SPATIAL_ABSENCE_GPU_CHUNK_FOOTPRINT) ||
		chunk.y >= view.negativeCount || chunk.z >= view.footprintCount)
	{
		SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_LOOKUP_MISS, 1u);
		return SPATIAL_PROBE_OUTCOME_LOOKUP_INVALID;
	}

	const uint negativeBlock = view.negativeBase + chunk.y * 2u;
	const uint4 negative0 = gSpatialAbsenceGpuSnapshot[negativeBlock + 0u];
	const float4 negative1 = asfloat(gSpatialAbsenceGpuSnapshot[negativeBlock + 1u]);
	const float3 witnessBoundsMin = float3(asfloat(negative0.z), asfloat(negative0.w), negative1.x);
	const float3 witnessBoundsMax = negative1.yzw;
	if (negative0.x > view.pairCount || negative0.y == 0u ||
		negative0.y > view.pairCount - negative0.x ||
		!all(isfinite(witnessBoundsMin)) || !all(isfinite(witnessBoundsMax)) ||
		any(witnessBoundsMin > witnessBoundsMax))
	{
		SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_LOOKUP_MISS, 1u);
		return SPATIAL_PROBE_OUTCOME_LOOKUP_INVALID;
	}

	const float3 witnessBoundsEpsilon = 1.0e-3;
	if (any(worldPosition < witnessBoundsMin - witnessBoundsEpsilon) ||
		any(worldPosition > witnessBoundsMax + witnessBoundsEpsilon))
	{
		SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_OUTSIDE_UNION, 1u);
		return SPATIAL_PROBE_OUTCOME_OUTSIDE_UNION;
	}

	bool negativeRecordsValid = true;
	bool insideNegative = exactNegativeSurfaceMembership;
	if (exactNegativeSurfaceMembership)
	{
		footprintProbeDetails.stage = SPATIAL_FOOTPRINT_PROBE_SURFACE_MEMBERSHIP;
	}
	else
	{
		insideNegative = PointInSpatialAbsenceGpuFootprint(
			view, chunk.z, worldPosition.xz, collectProbeDetails, recordStats,
			negativeRecordsValid, footprintProbeDetails);
	}
	if (!negativeRecordsValid)
	{
		SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_LOOKUP_MISS, 1u);
		return SPATIAL_PROBE_OUTCOME_LOOKUP_INVALID;
	}
	if (!insideNegative)
	{
		SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_EXACT_MISS, 1u);
		return SPATIAL_PROBE_OUTCOME_NEGATIVE_FOOTPRINT_MISS;
	}

	bool pairBoundsMatched = false;
	[loop]
	for (uint pairOffset = 0u; pairOffset < negative0.y; ++pairOffset)
	{
		const uint pairBlock = view.pairBase + (negative0.x + pairOffset) * 2u;
		const uint4 pair0 = gSpatialAbsenceGpuSnapshot[pairBlock + 0u];
		const float4 pair1 = asfloat(gSpatialAbsenceGpuSnapshot[pairBlock + 1u]);
		const float3 pairBoundsMin = float3(asfloat(pair0.z), asfloat(pair0.w), pair1.x);
		const float3 pairBoundsMax = pair1.yzw;
		uint4 positiveChunk;
		if (pair0.x >= view.chunkCount || pair0.y >= view.footprintCount ||
			!LoadSpatialAbsenceGpuChunk(view, pair0.x, positiveChunk) ||
			(positiveChunk.x & SPATIAL_ABSENCE_GPU_CHUNK_FOOTPRINT) == 0u ||
			positiveChunk.z != pair0.y || !all(isfinite(pairBoundsMin)) ||
			!all(isfinite(pairBoundsMax)) || any(pairBoundsMin > pairBoundsMax))
		{
			SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_LOOKUP_MISS, 1u);
			return SPATIAL_PROBE_OUTCOME_LOOKUP_INVALID;
		}

		const float2 pairBoundsEpsilon = 1.0e-3;
		if (any(worldPosition.xz < pairBoundsMin.xz - pairBoundsEpsilon) ||
			any(worldPosition.xz > pairBoundsMax.xz + pairBoundsEpsilon) ||
			worldPosition.y < pairBoundsMin.y || worldPosition.y > pairBoundsMax.y)
			continue;

		pairBoundsMatched = true;
		matchedPositiveChunk = pair0.x;
		bool positiveRecordsValid = false;
		SpatialFootprintProbeDetails positiveProbeDetails = EmptySpatialFootprintProbeDetails();
		const bool insidePositive = PointInSpatialAbsenceGpuFootprint(
			view, pair0.y, worldPosition.xz, collectProbeDetails, recordStats,
			positiveRecordsValid, positiveProbeDetails);
		if (!positiveRecordsValid)
		{
			footprintProbeDetails = positiveProbeDetails;
			SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_LOOKUP_MISS, 1u);
			return SPATIAL_PROBE_OUTCOME_LOOKUP_INVALID;
		}
		if (insidePositive)
		{
			SpatialAbsenceGpuStatAdd(
				recordStats,
				TRACE_STAT_SPATIAL_REJECT_PRIMARY + min(statsKind, TRACE_STATS_KIND_FAST_EMISSIVE),
				1u);
			return SPATIAL_PROBE_OUTCOME_REJECT;
		}
		footprintProbeDetails = positiveProbeDetails;
	}

	SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_EXACT_MISS, 1u);
	return pairBoundsMatched ?
		SPATIAL_PROBE_OUTCOME_POSITIVE_FOOTPRINT_MISS :
		SPATIAL_PROBE_OUTCOME_PAIR_BOUNDS_MISS;
}

uint EvaluateTypedSpatialAbsence(
	uint chunkIndex,
	float3 worldPosition,
	uint statsKind,
	bool evaluationEnabled,
	bool exactNegativeSurfaceMembership,
	bool collectProbeDetails,
	bool recordStats,
	out uint matchedPositiveChunk,
	out SpatialFootprintProbeDetails footprintProbeDetails)
{
	matchedPositiveChunk = 0xffffffffu;
	footprintProbeDetails = EmptySpatialFootprintProbeDetails();
	if (!evaluationEnabled || chunkIndex == 0xffffffffu)
		return SPATIAL_PROBE_OUTCOME_DISABLED;

	SpatialAbsenceGpuView view;
	uint failureOutcome = SPATIAL_PROBE_OUTCOME_SNAPSHOT_INVALID;
	if (!LoadSpatialAbsenceGpuView(view, failureOutcome))
	{
		SpatialAbsenceGpuStatAdd(recordStats, TRACE_STAT_SPATIAL_SNAPSHOT_FAIL_OPEN, 1u);
		SpatialAbsenceGpuStatAdd(
			recordStats,
			failureOutcome == SPATIAL_PROBE_OUTCOME_FRAME_MISMATCH ?
				TRACE_STAT_SPATIAL_FRAME_MISMATCH : TRACE_STAT_SPATIAL_SNAPSHOT_INVALID,
			1u);
		return failureOutcome;
	}

	return EvaluateTypedSpatialAbsencePrevalidated(
		view, chunkIndex, worldPosition, statsKind, evaluationEnabled,
		exactNegativeSurfaceMembership, collectProbeDetails, recordStats,
		matchedPositiveChunk, footprintProbeDetails);
}

void RecordSpatialAbsenceGpuComparison(
	uint rawOutcome,
	uint typedOutcome,
	uint rawPositiveChunk,
	uint typedPositiveChunk,
	SpatialFootprintProbeDetails rawProbe,
	SpatialFootprintProbeDetails typedProbe)
{
	TraceShaderStatAdd(TRACE_STAT_SPATIAL_COMPARE_TOTAL, 1u);
	if (typedOutcome == SPATIAL_PROBE_OUTCOME_SNAPSHOT_INVALID ||
		typedOutcome == SPATIAL_PROBE_OUTCOME_FRAME_MISMATCH)
		TraceShaderStatAdd(TRACE_STAT_SPATIAL_COMPARE_TYPED_UNAVAILABLE, 1u);
	if (typedOutcome != rawOutcome)
		TraceShaderStatAdd(TRACE_STAT_SPATIAL_COMPARE_OUTCOME_MISMATCH, 1u);
	if (typedPositiveChunk != rawPositiveChunk)
		TraceShaderStatAdd(TRACE_STAT_SPATIAL_COMPARE_POSITIVE_MISMATCH, 1u);
	if (typedProbe.stage != rawProbe.stage ||
		typedProbe.cellReferenceCount != rawProbe.cellReferenceCount ||
		typedProbe.bestTriangle != rawProbe.bestTriangle ||
		asuint(typedProbe.bestMargin) != asuint(rawProbe.bestMargin))
		TraceShaderStatAdd(TRACE_STAT_SPATIAL_COMPARE_PROBE_MISMATCH, 1u);
}

#endif
