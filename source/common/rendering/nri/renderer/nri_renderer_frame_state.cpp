#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_static_scene_geometry.h"
#include "../system/nri_renderdevice.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "perf_capture.h"
#include "gamestruct.h"
#include "mapinfo.h"
#include "printf.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
	static bool ShouldCollectFrameStatePerfTiming()
	{
		return (int)perf_looptraceframes > 0 || ShouldEmitRendererTemporalTraceLogs() || (bool)nri_ptslowdowntrace || PerfCompactCaptureTimingActive();
	}

	static double FrameStateDurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldCollectFrameStatePerfTiming() ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedPtPerfTimer()
		{
			if (mTarget != nullptr)
			{
				*mTarget += FrameStateDurationMs(mStart, std::chrono::steady_clock::now());
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	class MapMoverPrintfSink final : public NRIMapMoverPerfSink
	{
	public:
		void EmitLine(const char* line) override
		{
			Printf("%s", line);
		}
	};

	static void MarkChunkVisible(std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const size_t wordIndex = (size_t)(chunkIndex >> 5u);
		if (wordIndex >= visibleChunkWords.size())
		{
			return;
		}

		visibleChunkWords[wordIndex] |= 1u << (chunkIndex & 31u);
	}

	static bool IsChunkMarkedVisible(const std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const size_t wordIndex = (size_t)(chunkIndex >> 5u);
		if (wordIndex >= visibleChunkWords.size())
		{
			return false;
		}

		return (visibleChunkWords[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
	}

	static uint32_t GetFlatPlaneVisibilityIndex(int32_t sectorIndex, bool ceiling)
	{
		return (uint32_t)sectorIndex * 2u + (ceiling ? 1u : 0u);
	}

	static void MarkFlatPlaneVisible(std::vector<uint32_t>& visibleFlatPlaneWords, int32_t sectorIndex, bool ceiling)
	{
		if (sectorIndex < 0)
		{
			return;
		}

		const uint32_t flatPlaneIndex = GetFlatPlaneVisibilityIndex(sectorIndex, ceiling);
		const size_t wordIndex = (size_t)(flatPlaneIndex >> 5u);
		if (wordIndex >= visibleFlatPlaneWords.size())
		{
			return;
		}

		visibleFlatPlaneWords[wordIndex] |= 1u << (flatPlaneIndex & 31u);
	}

	static void MarkVisibleChunkForSector(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex, std::vector<uint32_t>& visibleChunkWords)
	{
		const int32_t chunkIndex = nri_static_scene_geometry::FindMapChunkIndexForSector(mapWorld, sectorIndex);
		if (chunkIndex >= 0)
		{
			MarkChunkVisible(visibleChunkWords, (uint32_t)chunkIndex);
		}
	}

	static void AccumulateVisibleChunksFromViewRoots(const HWDrawInfo& di, const nri_scene::PTMapWorld& mapWorld, std::vector<uint32_t>& visibleChunkWords)
	{
		if (di.Viewpoint.SectNums != nullptr)
		{
			for (int i = 0; i < di.Viewpoint.SectCount; ++i)
			{
				MarkVisibleChunkForSector(mapWorld, di.Viewpoint.SectNums[i], visibleChunkWords);
			}
		}
		else
		{
			MarkVisibleChunkForSector(mapWorld, di.Viewpoint.SectCount, visibleChunkWords);
		}
	}

	static void AccumulateVisibleChunksFromDrawLists(const HWDrawInfo& di, const nri_scene::PTMapWorld& mapWorld, std::vector<uint32_t>& visibleChunkWords)
	{
		for (int drawListType = 0; drawListType < GLDL_TYPES; ++drawListType)
		{
			const HWDrawList& drawList = di.drawlists[drawListType];

			for (const HWWall* wall : drawList.walls)
			{
				if (wall != nullptr && wall->seg != nullptr)
				{
					MarkVisibleChunkForSector(mapWorld, wall->seg->sector, visibleChunkWords);
				}
			}

			for (const HWFlat* flat : drawList.flats)
			{
				if (flat != nullptr && flat->sec != nullptr)
				{
					MarkVisibleChunkForSector(mapWorld, sector.IndexOf(flat->sec), visibleChunkWords);
				}
			}
		}
	}

	static void AccumulateVisibleFlatPlanesFromDrawLists(const HWDrawInfo& di, std::vector<uint32_t>& visibleFlatPlaneWords)
	{
		for (int drawListType = 0; drawListType < GLDL_TYPES; ++drawListType)
		{
			const HWDrawList& drawList = di.drawlists[drawListType];
			for (const HWFlat* flat : drawList.flats)
			{
				if (flat == nullptr || flat->sec == nullptr || flat->Sprite != nullptr)
				{
					continue;
				}

				MarkFlatPlaneVisible(
					visibleFlatPlaneWords,
					sector.IndexOf(flat->sec),
					flat->plane != 0);
			}
		}
	}

	static float GetHaltonSample(uint32_t index, uint32_t base)
	{
		float inverseBase = 1.0f / (float)base;
		float fraction = inverseBase;
		float result = 0.0f;

		while (index > 0)
		{
			result += fraction * (float)(index % base);
			index /= base;
			fraction *= inverseBase;
		}

		return result;
	}

	static void ComputeTemporalJitter(uint32_t frameIndex, uint32_t jitterPhaseCount, float outJitter[2])
	{
		jitterPhaseCount = std::max(jitterPhaseCount, 1u);
		const uint32_t sampleIndex = (frameIndex % jitterPhaseCount) + 1u;
		outJitter[0] = GetHaltonSample(sampleIndex, 2u) - 0.5f;
		outJitter[1] = GetHaltonSample(sampleIndex, 3u) - 0.5f;
	}

	static void Normalize3(float v[3])
	{
		const float length = std::max(0.0001f, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}

	static void TransformPoint(const VSMatrix& matrix, float x, float y, float z, float out[4])
	{
		float point[4] = { x, y, z, 1.0f };
		VSMatrix copy = matrix;
		copy.multMatrixPoint(point, out);
	}

	static void Copy3(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 3);
	}

	static void Copy2(const float* src, float* dst)
	{
		std::memcpy(dst, src, sizeof(float) * 2);
	}
}

void NRIRenderer::UpdatePerFrameState(HWDrawInfo& di, bool logicalMainView)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.updateStateMs);
	Clocker clock(NriPTUpdateState);
	mMapMovers.CaptureFrame(gi, di.Viewpoint.TicFrac, mMapWorld.buildSerial);
	mMapMoverShadow.CaptureChangedGroups(
		mMapMovers,
		mMapWorld,
		mFrameIndex,
		(int)nri_ptmapmovershadow);
	if ((int)nri_ptmapmovertrace > 0)
	{
		MapMoverPrintfSink sink;
		mMapMovers.EmitPerfTrace(mFrameIndex, sink, (int)nri_ptmapmovertrace >= 2);
	}

	if (mHasPreviousCameraState)
	{
		Copy3(mCurrentCameraPos, mPreviousCameraPos);
		Copy3(mCurrentCameraForward, mPreviousCameraForward);
		Copy3(mCurrentCameraRight, mPreviousCameraRight);
		Copy3(mCurrentCameraUp, mPreviousCameraUp);
		mPreviousTanHalfFovX = mCurrentTanHalfFovX;
		mPreviousTanHalfFovY = mCurrentTanHalfFovY;
		Copy2(mCurrentJitter, mPreviousJitter);
		std::memcpy(mPreviousViewToClip, mCurrentViewToClip, sizeof(mPreviousViewToClip));
		std::memcpy(mPreviousWorldToView, mCurrentWorldToView, sizeof(mPreviousWorldToView));
	}

	VSMatrix inverseView;
	if (!di.VPUniforms.mViewMatrix.inverseMatrix(inverseView))
	{
		std::memset(mCurrentCameraPos, 0, sizeof(mCurrentCameraPos));
		std::memset(mCurrentCameraForward, 0, sizeof(mCurrentCameraForward));
		std::memset(mCurrentCameraRight, 0, sizeof(mCurrentCameraRight));
		std::memset(mCurrentCameraUp, 0, sizeof(mCurrentCameraUp));
		mCurrentCameraForward[2] = -1.0f;
		mCurrentCameraRight[0] = 1.0f;
		mCurrentCameraUp[1] = 1.0f;
	}
	else
	{
		float origin[4] = {};
		float rightPoint[4] = {};
		float upPoint[4] = {};
		float forwardPoint[4] = {};
		TransformPoint(inverseView, 0.0f, 0.0f, 0.0f, origin);
		TransformPoint(inverseView, 1.0f, 0.0f, 0.0f, rightPoint);
		TransformPoint(inverseView, 0.0f, 1.0f, 0.0f, upPoint);
		TransformPoint(inverseView, 0.0f, 0.0f, -1.0f, forwardPoint);

		const float cameraPos[3] = {
			origin[0],
			origin[1],
			origin[2]
		};
		const float rightDelta[3] = {
			rightPoint[0] - origin[0],
			rightPoint[1] - origin[1],
			rightPoint[2] - origin[2]
		};
		const float upDelta[3] = {
			upPoint[0] - origin[0],
			upPoint[1] - origin[1],
			upPoint[2] - origin[2]
		};
		const float forwardDelta[3] = {
			forwardPoint[0] - origin[0],
			forwardPoint[1] - origin[1],
			forwardPoint[2] - origin[2]
		};

		Copy3(cameraPos, mCurrentCameraPos);
		Copy3(rightDelta, mCurrentCameraRight);
		Copy3(upDelta, mCurrentCameraUp);
		Copy3(forwardDelta, mCurrentCameraForward);

		Normalize3(mCurrentCameraRight);
		Normalize3(mCurrentCameraUp);
		Normalize3(mCurrentCameraForward);
	}

	const float* projection = di.VPUniforms.mProjectionMatrix.get();
	const float projectionScaleX = projection != nullptr ? std::fabs(projection[0]) : 0.0f;
	const float projectionScaleY = projection != nullptr ? std::fabs(projection[5]) : 0.0f;
	if (projectionScaleX > 0.0001f && projectionScaleY > 0.0001f)
	{
		// Match the hardware backend frustum exactly instead of rebuilding Y-FOV from the PT render dimensions.
		mCurrentTanHalfFovX = 1.0f / projectionScaleX;
		mCurrentTanHalfFovY = 1.0f / projectionScaleY;
	}
	else
	{
		const float tanHalfFovX = tanf((float)di.Viewpoint.FieldOfView.Radians() * 0.5f);
		mCurrentTanHalfFovX = tanHalfFovX;
		mCurrentTanHalfFovY = tanHalfFovX * ((float)mRenderHeight / std::max(1.0f, (float)mRenderWidth));
	}
	const NRIMainUpscalerKind resolvedMainUpscaler = ResolveMainUpscalerKind(false);
	if (!nri_ptbootstrap && !mGuiCaptureActive && NRIShouldUseTemporalJitter(resolvedMainUpscaler))
	{
		const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMainUpscaler, GetSelectedUpscalerMode());
		const uint32_t jitterPhaseCount = NRIGetTemporalJitterPhaseCount(resolvedMainUpscaler, resolvedUpscalerMode, mGuiCaptureActive);
		ComputeTemporalJitter(mFrameIndex, jitterPhaseCount, mCurrentJitter);
	}
	else
	{
		mCurrentJitter[0] = 0.0f;
		mCurrentJitter[1] = 0.0f;
	}
	FillMatrix(mCurrentViewToClip, di.VPUniforms.mProjectionMatrix);
	FillMatrix(mCurrentWorldToView, di.VPUniforms.mViewMatrix);
	const bool spatialAbsenceRequested = (bool)nri_pt360absencegate || (bool)nri_pt360actorabsencegate ||
		(bool)nri_pt360absenceprobe || (int)nri_pt360absencetrace > 0;
	const bool spatialAbsenceTraceRequested = (int)nri_pt360absencetrace > 0;
	if (spatialAbsenceRequested && logicalMainView && mMapWorld.valid && mMapWorld.buildSerial != 0)
	{
		di.Capture360Census(mMapWorld.buildSerial, 0, true);
		const HW360CensusSnapshot& census = di.Get360CensusSnapshot();
		NRISpatialAbsenceCensusInput input;
		input.complete = census.HasNegativeAuthority();
		input.rootStable = census.HasNegativeAuthority() && census.roots.Size() != 0;
		input.generationMatches = census.mapGeneration == mMapWorld.buildSerial &&
			mMapWorld.level == currentLevel;
		input.captureSerial = census.captureSerial;
		input.observationHash = census.observationHash;
		input.worldGeneration = mMapWorld.buildSerial;
		input.frameIndex = mFrameIndex;
		input.authoritativeRootSector = census.authoritativeRoot;
		input.rootSectorIndices.reserve(census.roots.Size());
		for (int rootSector : census.roots)
		{
			if (rootSector >= 0) input.rootSectorIndices.push_back((uint32_t)rootSector);
		}
		std::memcpy(input.center, mCurrentCameraPos, sizeof(input.center));
		input.guardRadius = (float)nri_pt360absenceradius;
		input.probeEnabled = (bool)nri_pt360absenceprobe && (int)nri_pt360absenceprobechunk >= 0;
		input.probeOrigin[0] = (float)nri_pt360absenceprobeoriginx;
		input.probeOrigin[1] = (float)nri_pt360absenceprobeoriginy;
		input.probeOrigin[2] = (float)nri_pt360absenceprobeoriginz;
		input.probeRadius = (float)nri_pt360absenceproberadius;
		input.probeExpectedChunk = input.probeEnabled ? (uint32_t)(int)nri_pt360absenceprobechunk : UINT32_MAX;
		input.probeTargetPixel = (uint32_t)(int)nri_pt360absenceprobetargetx |
			((uint32_t)(int)nri_pt360absenceprobetargety << 16u);
		input.probeReferencePixel = (uint32_t)(int)nri_pt360absenceprobereferencex |
			((uint32_t)(int)nri_pt360absenceprobereferencey << 16u);
		input.reachedSectorIndices.reserve(census.reachedSectorCount);
		for (unsigned sectorIndex = 0; sectorIndex < census.reachedSectors.Size(); ++sectorIndex)
		{
			if (census.ContainsSector(sectorIndex))
			{
				input.reachedSectorIndices.push_back(sectorIndex);
			}
		}
		input.reachedWallIndices.reserve(census.reachedWallCount);
		for (unsigned wallIndex = 0; wallIndex < census.reachedWalls.Size(); ++wallIndex)
		{
			if (census.reachedWalls[wallIndex])
			{
				input.reachedWallIndices.push_back(wallIndex);
			}
		}
		if (gi != nullptr)
		{
			input.authoredClosureSectorIndices.reserve(sector.Size());
			for (unsigned sectorIndex = 0; sectorIndex < sector.Size(); ++sectorIndex)
			{
				if (gi->IsPortalClosureSector((int)sectorIndex))
					input.authoredClosureSectorIndices.push_back(sectorIndex);
			}
		}
		for (uint32_t chunkIndex = 0; chunkIndex < mMapWorld.chunks.size(); ++chunkIndex)
		{
			const RuntimeMapMutationCache::ChunkReplacement* replacement = mRuntimeMutation.FindReplacement(chunkIndex);
			// Dirty/dragged discovery flags can pulse while the same chunk remains
			// the active static TLAS authority. Fail open only when a separately
			// published replacement/exclusion changes occurrence ownership;
			// otherwise those bookkeeping flags would flicker the gate.
			if (replacement != nullptr && (replacement->active || replacement->excludeStaticChunk))
			{
				input.uncertainChunkIndices.push_back(chunkIndex);
			}
		}
		const NRISpatialAbsenceSnapshot& absence = mSpatialAbsenceGate.Build(mMapWorld, input);
		if (spatialAbsenceTraceRequested)
		{
			float centerDelta[3] = {};
			if (mHasPreviousCameraState)
			{
				for (uint32_t axis = 0; axis < 3u; ++axis)
				{
					centerDelta[axis] = absence.center[axis] - mPreviousCameraPos[axis];
				}
			}
			Printf("NRI PT 360 absence: frame=%u capture=%llu complete=%u authority=%u previous_authority=%u authority_transition=%u stable_captures=%u census_fail=0x%x census_observe=0x%x census_hash=0x%016llx previous_census_hash=0x%016llx census_elapsed_ms=%.3f census_scratch_growths=%u build_elapsed_ms=%.3f topology_cache_hit=%u topology_pairs=%u world=%llu gpu_hash=0x%016llx semantic_hash=0x%016llx selection_hash=0x%016llx root_count=%u authoritative_root=%d root_local_space=%d sectors=%u sections=%u walls=%u candidates=%u certified=%u protected_open=%u protected_near=%u protected_collapsed=%u protected_inset=%u protected_frontier=%u authorized_pairs=%u pending_pairs=%u source_witnesses=%u selected_witnesses=%u footprint_triangles=%u grid_cells=%u grid_references=%u fail_open=0x%x records=%u center=(%.3f,%.3f,%.3f) center_delta=(%.3f,%.3f,%.3f) radius=%.2f\n",
				mFrameIndex,
				(unsigned long long)census.captureSerial,
				census.complete ? 1u : 0u,
				absence.HasNegativeAuthority() ? 1u : 0u,
				absence.previousAuthority ? 1u : 0u,
				absence.authorityTransition ? 1u : 0u,
				absence.stableCaptureCount,
				census.failureFlags,
				census.observationFlags,
				(unsigned long long)absence.censusObservationHash,
				(unsigned long long)absence.previousCensusObservationHash,
				census.elapsedMilliseconds,
				census.traversal.scratchArrayGrowths,
				absence.buildElapsedMilliseconds,
				absence.topologyCacheHit ? 1u : 0u,
				absence.topologyPairCount,
				(unsigned long long)absence.worldGeneration,
				(unsigned long long)absence.payloadHash,
				(unsigned long long)absence.semanticHash,
				(unsigned long long)absence.selectionHash,
				(uint32_t)absence.rootSectorIndices.size(),
				absence.authoritativeRootSector,
				absence.rootLocalSpaceIndex,
				census.reachedSectorCount,
				census.reachedSectionCount,
				census.reachedWallCount,
				absence.candidateCount,
				absence.certifiedCount,
				absence.openBoundaryProtectedCount,
				absence.nearTopologyProtectedCount,
				absence.collapsedPortalProtectedCount,
				absence.insetBoundaryEnclosureProtectedCount,
				absence.reachedMapFrontierProtectedCount,
				absence.authorizedPairCount,
				absence.pendingPairCount,
				absence.sourceWitnessCount,
				absence.selectedWitnessCount,
				absence.footprintTriangleCount,
				absence.footprintGridCellCount,
				absence.footprintGridReferenceCount,
				absence.failOpenFlags,
				(uint32_t)absence.gpuRecords.size(),
				absence.center[0], absence.center[1], absence.center[2],
				centerDelta[0], centerDelta[1], centerDelta[2],
				absence.guardRadius);
			uint32_t rows = 0;
			auto printConflict = [&](const NRISpatialAbsenceConflictRecord& conflict)
			{
				Printf("NRI PT 360 absence conflict: frame=%u capture=%llu world=%llu local_space=%d decision=%s positive_chunk=%u positive_sector=%d negative_chunk=%u negative_sector=%d witnesses=%u protected_open=%u protected_near=%u protected_collapsed=%u protected_inset=%u protected_frontier=%u topology_via=%d collapsed_via=%d inset_child=%d frontier_sector=%d frontier_wall=%d frontier_closure=%d continuity_count=%u continuity_previous=%u continuity_context=%u continuity_authorized=%u distance=%.3f overlap=((%.3f,%.3f,%.3f),(%.3f,%.3f,%.3f))\n",
					mFrameIndex,
					(unsigned long long)absence.captureSerial,
					(unsigned long long)absence.worldGeneration,
					absence.rootLocalSpaceIndex,
					GetNRISpatialAbsenceConflictDecisionName(conflict.decision),
					conflict.positiveChunk,
					conflict.positiveSector,
					conflict.negativeChunk,
					conflict.negativeSector,
					conflict.exactWitnessCount,
					(conflict.protectionFlags & NRI_SPATIAL_ABSENCE_PROTECTION_OPEN_BOUNDARY) != 0 ? 1u : 0u,
					(conflict.protectionFlags & NRI_SPATIAL_ABSENCE_PROTECTION_NEAR_ORDINARY_TOPOLOGY) != 0 ? 1u : 0u,
					(conflict.protectionFlags & NRI_SPATIAL_ABSENCE_PROTECTION_COLLAPSED_PORTAL_ENVELOPE) != 0 ? 1u : 0u,
					(conflict.protectionFlags & NRI_SPATIAL_ABSENCE_PROTECTION_INSET_BOUNDARY_ENCLOSURE) != 0 ? 1u : 0u,
					(conflict.protectionFlags & NRI_SPATIAL_ABSENCE_PROTECTION_REACHED_MAP_FRONTIER) != 0 ? 1u : 0u,
					conflict.topologyIntermediateSector,
					conflict.collapsedPortalSector,
					conflict.insetBoundaryChildSector,
					conflict.frontierReachedSector,
					conflict.frontierReachedWall,
					conflict.frontierClosureSector,
					conflict.continuityCount,
					conflict.continuityPresentPrevious ? 1u : 0u,
					conflict.continuityContextContinuous ? 1u : 0u,
					conflict.continuityAuthorized ? 1u : 0u,
					conflict.distanceToGuardCenter,
					conflict.overlapMin[0], conflict.overlapMin[1], conflict.overlapMin[2],
					conflict.overlapMax[0], conflict.overlapMax[1], conflict.overlapMax[2]);
				rows++;
			};
			const int32_t targetedChunk = (bool)nri_pt360absenceprobe && (int)nri_pt360absenceprobechunk >= 0
				? (int)nri_pt360absenceprobechunk : -1;
			if (targetedChunk >= 0)
			{
				for (const NRISpatialAbsenceConflictRecord& conflict : absence.conflicts)
				{
					if (rows >= 16u) break;
					if ((conflict.positiveChunk == (uint32_t)targetedChunk ||
						conflict.negativeChunk == (uint32_t)targetedChunk) &&
						conflict.protectionFlags != NRI_SPATIAL_ABSENCE_PROTECTION_NONE)
					{
						printConflict(conflict);
					}
				}
			}
			for (const NRISpatialAbsenceConflictRecord& conflict : absence.conflicts)
			{
				if (rows >= 16u) break;
				if (conflict.decision == NRISpatialAbsenceConflictDecision::Certified)
				{
					printConflict(conflict);
				}
			}
			// When nothing certifies, report a bounded generic sample of the
			// classifier's reasons. Diagnostics must not encode map-specific IDs.
			if (rows == 0)
			{
				for (const NRISpatialAbsenceConflictRecord& conflict : absence.conflicts)
				{
					if (rows >= 16u) break;
					printConflict(conflict);
				}
			}
			uint32_t selectionRows = 0;
			for (const NRISpatialAbsenceSelectionRecord& selection : absence.selections)
			{
				if (selectionRows >= 8u) break;
				const RuntimeMapMutationCache::ChunkReplacement* negativeReplacement =
					mRuntimeMutation.FindReplacement(selection.negativeChunk);
				const RuntimeMapMutationCache::ChunkReplacement* positiveReplacement =
					mRuntimeMutation.FindReplacement(selection.firstPositiveChunk);
				Printf("NRI PT 360 absence selection: frame=%u negative_chunk=%u first_positive_chunk=%u source_owners=%u selected_owners=%u source_owner_hash=0x%016llx selected_owner_hash=0x%016llx source_witnesses=%u selected_witnesses=%u footprint_triangles=%u selected_hash=0x%016llx bounds=((%.3f,%.3f,%.3f),(%.3f,%.3f,%.3f)) runtime_active=%u exclude_static=%u first_positive_runtime_active=%u first_positive_exclude_static=%u\n",
					mFrameIndex,
					selection.negativeChunk,
					selection.firstPositiveChunk,
					selection.sourcePositiveOwnerCount,
					selection.selectedPositiveOwnerCount,
					(unsigned long long)selection.sourcePositiveOwnerHash,
					(unsigned long long)selection.selectedPositiveOwnerHash,
					selection.sourceWitnessCount,
					selection.selectedWitnessCount,
					selection.footprintTriangleCount,
					(unsigned long long)selection.selectionHash,
					selection.boundsMin[0], selection.boundsMin[1], selection.boundsMin[2],
					selection.boundsMax[0], selection.boundsMax[1], selection.boundsMax[2],
					negativeReplacement != nullptr && negativeReplacement->active ? 1u : 0u,
					negativeReplacement != nullptr && negativeReplacement->excludeStaticChunk ? 1u : 0u,
					positiveReplacement != nullptr && positiveReplacement->active ? 1u : 0u,
					positiveReplacement != nullptr && positiveReplacement->excludeStaticChunk ? 1u : 0u);
				selectionRows++;
			}
			nri_pt360absencetrace = std::max((int)nri_pt360absencetrace - 1, 0);
		}
	}
	else
	{
		// Unsupported child/offscreen contexts publish no root-context negative
		// authority, but do not interrupt stability between logical main frames.
		// Disabling the feature or missing a main-view world starts a fresh audit.
		mSpatialAbsenceGate.Reset(mFrameIndex, !spatialAbsenceRequested || logicalMainView);
	}
	if (mSpatialAbsenceFormat != 0u)
	{
		BuildNRISpatialAbsenceGpuSnapshot(
			mSpatialAbsenceGate.GetSnapshot(),
			mSpatialAbsenceGpuSnapshot);
		if (spatialAbsenceTraceRequested)
		{
			Printf("NRI PT 360 absence typed: frame=%u format=%u valid=%u authority=%u fail_open=0x%x blocks=%u bytes=%llu source_hash=0x%016llx payload_hash=0x%016llx build_elapsed_ms=%.3f\n",
				mFrameIndex,
				mSpatialAbsenceFormat,
				mSpatialAbsenceGpuSnapshot.valid ? 1u : 0u,
				mSpatialAbsenceGpuSnapshot.HasNegativeAuthority(mSpatialAbsenceGate.GetSnapshot()) ? 1u : 0u,
				mSpatialAbsenceGpuSnapshot.failOpenFlags,
				(uint32_t)mSpatialAbsenceGpuSnapshot.blocks.size(),
				(unsigned long long)mSpatialAbsenceGpuSnapshot.blocks.size() * sizeof(NRISpatialAbsenceGpuBlock),
				(unsigned long long)mSpatialAbsenceGpuSnapshot.sourcePayloadHash,
				(unsigned long long)mSpatialAbsenceGpuSnapshot.payloadHash,
				mSpatialAbsenceGpuSnapshot.buildElapsedMilliseconds);
		}
	}
	else
	{
		mSpatialAbsenceGpuSnapshot = {};
	}
	const BitArray& visibleSectors = di.GetVisibleSectors();
	const size_t visibleChunkWordCount = std::max<size_t>((mMapWorld.chunks.size() + 31u) / 32u, 1u);
	const size_t visibleFlatPlaneWordCount = std::max<size_t>(((size_t)sector.Size() * 2u + 31u) / 32u, 1u);
	mCurrentVisibleChunkWords.assign(visibleChunkWordCount, 0u);
	mCurrentVisibleFlatPlaneWords.assign(visibleFlatPlaneWordCount, 0u);
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (!visibleSectors.Check(sectorIndex))
		{
			continue;
		}

		MarkVisibleChunkForSector(mMapWorld, (int32_t)sectorIndex, mCurrentVisibleChunkWords);
	}
	// HWDrawInfo can accumulate geometry from multiple RenderScene passes
	// while its final visible-sector bitset only reflects the last traversal.
	// Union the root sectors and accumulated drawlists so the PT chunk gate
	// tracks the scene the HAL actually built this frame.
	AccumulateVisibleChunksFromViewRoots(di, mMapWorld, mCurrentVisibleChunkWords);
	AccumulateVisibleChunksFromDrawLists(di, mMapWorld, mCurrentVisibleChunkWords);
	// Chunk visibility is still too coarse for overlapping static floors and ceilings.
	// Track the exact floor/ceiling sectors backed by the accumulated flat drawlists
	// so the RT primary path can reject hidden coplanar static flat sections.
	AccumulateVisibleFlatPlanesFromDrawLists(di, mCurrentVisibleFlatPlaneWords);
	if (ShouldEmitRendererTemporalTraceLogs())
	{
		const uint32_t targetWidth = mFrameBuffer->mActiveTarget != nullptr ? mFrameBuffer->mActiveTarget->width : 0u;
		const uint32_t targetHeight = mFrameBuffer->mActiveTarget != nullptr ? mFrameBuffer->mActiveTarget->height : 0u;
		const int32_t sceneLeft = mFrameBuffer->mSceneViewport.left;
		const int32_t sceneBottom = mFrameBuffer->mSceneViewport.top;
		const int32_t sceneWidth = mFrameBuffer->mSceneViewport.width;
		const int32_t sceneHeight = mFrameBuffer->mSceneViewport.height;
		const int32_t sceneTop = (int32_t)targetHeight - sceneBottom - sceneHeight;
		const auto& uniformCameraPos = di.VPUniforms.mCameraPos;
		const FVector3 hwForward(di.Viewpoint.HWAngles);
		Printf("NRI PT camera: frame=%u hw_pitch=%.3f hw_yaw=%.3f hw_roll=%.3f scene_bl=(%d,%d %dx%d) scene_tl=(%u,%u %ux%u) target=%ux%u uniform_pos=(%.3f,%.3f,%.3f) inverse_pos=(%.3f,%.3f,%.3f) hw_forward=(%.3f,%.3f,%.3f) basis_fwd=(%.3f,%.3f,%.3f) basis_right=(%.3f,%.3f,%.3f) basis_up=(%.3f,%.3f,%.3f) tan=(%.6f,%.6f) proj=(%.6f,%.6f,%.6f,%.6f)\n",
			mFrameIndex,
			di.Viewpoint.HWAngles.Pitch.Degrees(),
			di.Viewpoint.HWAngles.Yaw.Degrees(),
			di.Viewpoint.HWAngles.Roll.Degrees(),
			mFrameBuffer->mSceneViewport.left,
			mFrameBuffer->mSceneViewport.top,
			mFrameBuffer->mSceneViewport.width,
			mFrameBuffer->mSceneViewport.height,
			sceneLeft,
			sceneTop,
			sceneWidth,
			sceneHeight,
			targetWidth,
			targetHeight,
			uniformCameraPos.X,
			uniformCameraPos.Y,
			uniformCameraPos.Z,
			mCurrentCameraPos[0],
			mCurrentCameraPos[1],
			mCurrentCameraPos[2],
			hwForward.X,
			hwForward.Y,
			hwForward.Z,
			mCurrentCameraForward[0],
			mCurrentCameraForward[1],
			mCurrentCameraForward[2],
			mCurrentCameraRight[0],
			mCurrentCameraRight[1],
			mCurrentCameraRight[2],
			mCurrentCameraUp[0],
			mCurrentCameraUp[1],
			mCurrentCameraUp[2],
			mCurrentTanHalfFovX,
			mCurrentTanHalfFovY,
			projection != nullptr ? projection[0] : 0.0f,
			projection != nullptr ? projection[5] : 0.0f,
			projection != nullptr ? projection[8] : 0.0f,
			projection != nullptr ? projection[9] : 0.0f);
	}

	if (mHasPreviousCameraState && !mResetHistory)
	{
		const float dx = mCurrentCameraPos[0] - mPreviousCameraPos[0];
		const float dy = mCurrentCameraPos[1] - mPreviousCameraPos[1];
		const float dz = mCurrentCameraPos[2] - mPreviousCameraPos[2];
		const float distanceSq = dx * dx + dy * dy + dz * dz;
		static constexpr float TeleportDistanceThreshold = 2048.0f;
		if (distanceSq > TeleportDistanceThreshold * TeleportDistanceThreshold)
		{
			RequestHistoryReset("camera-teleport", true, false);
		}
	}

	if (!mHasPreviousCameraState)
	{
		Copy3(mCurrentCameraPos, mPreviousCameraPos);
		Copy3(mCurrentCameraForward, mPreviousCameraForward);
		Copy3(mCurrentCameraRight, mPreviousCameraRight);
		Copy3(mCurrentCameraUp, mPreviousCameraUp);
		mPreviousTanHalfFovX = mCurrentTanHalfFovX;
		mPreviousTanHalfFovY = mCurrentTanHalfFovY;
		Copy2(mCurrentJitter, mPreviousJitter);
		std::memcpy(mPreviousViewToClip, mCurrentViewToClip, sizeof(mPreviousViewToClip));
		std::memcpy(mPreviousWorldToView, mCurrentWorldToView, sizeof(mPreviousWorldToView));
	}

	UpdateNightVisionState();
}
