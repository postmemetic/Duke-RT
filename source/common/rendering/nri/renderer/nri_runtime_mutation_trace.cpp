#include "nri_runtime_mutation_trace.h"
#include "nri_cvars.h"

#include "c_cvars.h"
#include "perf_capture.h"
#include "perf_audit.h"


namespace nri_runtime_mutation
{
	bool ShouldEmitTemporalTraceLogs()
	{
		return !!nri_pttemporaltrace && nri_pttraceframes > 0;
	}

	bool ShouldTracePtPerf()
	{
		return (int)perf_looptraceframes > 0 || ShouldEmitTemporalTraceLogs();
	}

	bool ShouldCollectRuntimeMutationPerfTiming()
	{
		return ShouldTracePtPerf() || (bool)nri_ptslowdowntrace || PerfCompactCaptureTimingActive() || PerfAuditActive();
	}

	static double RuntimeMutationDurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	ScopedPtPerfTimer::ScopedPtPerfTimer(double& targetMs)
		: mTarget(ShouldCollectRuntimeMutationPerfTiming() ? &targetMs : nullptr)
	{
		if (mTarget != nullptr)
		{
			mStart = std::chrono::steady_clock::now();
		}
	}

	ScopedPtPerfTimer::~ScopedPtPerfTimer()
	{
		if (mTarget != nullptr)
		{
			*mTarget += RuntimeMutationDurationMs(mStart, std::chrono::steady_clock::now());
		}
	}

	const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}

	uint32_t ScoreRuntimeSectorDirtyTruthEntry(const NRIRenderer::RuntimeSectorDirtyTruthTraceEntry& entry)
	{
		uint32_t score = 0;
		if (entry.forceTopology) score += 1u << 18;
		if (entry.baselineChanged) score += 1u << 17;
		if (entry.geometryChanged) score += 1u << 16;
		if (entry.materialChanged) score += 1u << 15;
		score += entry.liveTriangleCount * 8u;
		score += entry.liveSurfaceCount * 4u;
		return score;
	}

	uint32_t ScoreRuntimeAnimatedChurnTraceEntry(const NRIRenderer::RuntimeAnimatedChurnTraceEntry& entry)
	{
		uint32_t score = entry.materialRefreshes * 96u +
			entry.runtimeAttempts * 64u +
			entry.residentApplies * 48u +
			entry.syncSkips * 32u +
			entry.suppressionEmits * 16u;
		if (entry.suppressed)
		{
			score += 1u << 20;
		}
		return score;
	}

	uint32_t ScoreRuntimeMaterialOnlyMismatchTraceEntry(const NRIRenderer::RuntimeMaterialOnlyMismatchTraceEntry& entry)
	{
		const uint32_t materialDelta =
			entry.residentMaterialCount > entry.filteredMaterialCount ?
			entry.residentMaterialCount - entry.filteredMaterialCount :
			entry.filteredMaterialCount - entry.residentMaterialCount;
		uint32_t score = materialDelta * 128u;
		score += entry.residentMaterialCount * 16u;
		score += entry.filteredSurfaceCount * 8u;
		score += (entry.filteredWallCount + entry.filteredFlatCount) * 4u;
		if (entry.filteredWallCount != 0 && entry.filteredFlatCount != 0) score += 1u << 18;
		if (entry.residentWallCount != 0 && entry.residentFlatCount != 0) score += 1u << 17;
		return score;
	}

	uint32_t ScoreRuntimeStructuralRebuildTraceEntry(const NRIRenderer::RuntimeStructuralRebuildTraceEntry& entry)
	{
		uint32_t score = entry.triangleCount * 8u;
		score += entry.surfaceCount * 4u;
		score += entry.materialCount * 2u;
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_ForceTopology) != 0) score += 1u << 20;
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_StaticAnimatedFlip) != 0) score += 1u << 19;
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_ExcludeStaticFlip) != 0) score += 1u << 18;
		if (entry.mixedMaterialOnly) score += 1u << 17;
		if (entry.geometryOrDirty) score += 1u << 16;
		return score;
	}

	uint32_t ScoreRuntimeRecurringChunkTraceEntry(const NRIRenderer::RuntimeRecurringChunkTraceEntry& entry)
	{
		uint32_t score = entry.repeatedStateHitCount * 256u;
		score += entry.abaRecurrenceCount * 192u;
		score += entry.transitionCount * 64u;
		score += entry.uniqueStateCount * 32u;
		score += entry.visitCount * 8u;
		score += entry.lastTriangleCount * 4u;
		score += entry.lastMaterialCount * 2u;
		return score;
	}

	uint32_t ScoreRuntimeGeometryDirtyTraceEntry(const NRIRenderer::RuntimeGeometryDirtyTraceEntry& entry)
	{
		const uint32_t triangleDelta =
			entry.previousTriangleCount > entry.liveTriangleCount ?
			entry.previousTriangleCount - entry.liveTriangleCount :
			entry.liveTriangleCount - entry.previousTriangleCount;
		const uint32_t materialDelta =
			entry.previousMaterialCount > entry.liveMaterialCount ?
			entry.previousMaterialCount - entry.liveMaterialCount :
			entry.liveMaterialCount - entry.previousMaterialCount;
		uint32_t score = triangleDelta * 32u;
		score += materialDelta * 16u;
		score += entry.liveTriangleCount * 4u;
		score += entry.liveMaterialCount * 2u;
		if ((entry.familyMask & RuntimeGeometryDirtyFamily_GeometryDirtyMixed) != 0) score += 1u << 20;
		if ((entry.familyMask & RuntimeGeometryDirtyFamily_SectorWallGeometry) != 0) score += 1u << 19;
		if (entry.forceTopology) score += 1u << 18;
		if (entry.countChanged) score += 1u << 17;
		if (entry.wallsChanged && entry.flatsChanged) score += 1u << 16;
		return score;
	}

	uint32_t ScoreRuntimeInvisibleProofTraceEntry(const NRIRenderer::RuntimeInvisibleProofTraceEntry& entry)
	{
		uint32_t score = entry.previousTriangleCount * 8u;
		score += entry.previousMaterialCount * 4u;
		score += entry.previousSurfaceCount * 2u;
		if (entry.safeResidentNoopCandidate) score += 1u << 22;
		if (entry.exactSignatureMatch) score += 1u << 21;
		if (entry.animatedMaterialMatch) score += 1u << 20;
		if (entry.residentAuthoritative) score += 1u << 19;
		if (entry.residentAvailable) score += 1u << 18;
		if (entry.portalChunk) score += 1u << 17;
		if (entry.hardwareCanvas) score += 1u << 16;
		if (entry.hasAnimatedTextureCandidates || entry.animatedRefreshSuppressed || entry.staticAnimatedReplacement) score += 1u << 15;
		if (entry.visibleFloor || entry.visibleCeiling) score += 1u << 15;
		if (entry.excludeStaticChunk) score += 1u << 14;
		return score;
	}
}
