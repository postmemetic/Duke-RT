#pragma once

#include "../scene/nri_map_world.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nri
{
	struct TopLevelInstance;
}

struct SceneInstanceData;

enum NRISpatialAbsenceGpuFlags : uint32_t
{
	NRI_SPATIAL_ABSENCE_GPU_VALID = 1u << 0,
	NRI_SPATIAL_ABSENCE_GPU_COMPLETE = 1u << 1,
	NRI_SPATIAL_ABSENCE_GPU_CERTIFIED = 1u << 2,
	NRI_SPATIAL_ABSENCE_GPU_FOOTPRINT = 1u << 3,
	NRI_SPATIAL_ABSENCE_GPU_PAIR = 1u << 4,
	NRI_SPATIAL_ABSENCE_GPU_GRID = 1u << 5,
	NRI_SPATIAL_ABSENCE_GPU_GRID_CELL = 1u << 6,
	NRI_SPATIAL_ABSENCE_GPU_GRID_REFERENCE = 1u << 7,
	NRI_SPATIAL_ABSENCE_GPU_PROBE = 1u << 8,
	NRI_SPATIAL_ABSENCE_GPU_GRID_CELL_INTERIOR = 1u << 9,
	// CPU structural validation covered every serialized record reachable from
	// the authorized negative selections. Shaders may use their exact
	// membership fast path only while this header-only seal is present.
	NRI_SPATIAL_ABSENCE_GPU_RAY_QUERY_VALIDATED = 1u << 10,
	// Positive player-root census membership. This is not negative overlap
	// authority; it is consumed only by the per-hit actor occurrence gate.
	NRI_SPATIAL_ABSENCE_GPU_REACHED = 1u << 11,
};

// Record zero is the snapshot header. The next chunkCount records form the
// chunk lookup table; authorized pair descriptors, complete chunk-footprint
// triangles, and conservative uniform-grid/CSR records follow it. Grid cells
// select complete candidate sets without replacing exact triangle containment.
// This keeps lookup independent of transient TLAS instance indices and avoids
// treating a broad-phase chunk AABB intersection as negative authority.
struct NRISpatialAbsenceGpuRecord
{
	uint32_t flags = 0;
	uint32_t data0 = 0;
	uint32_t data1 = 0;
	uint32_t data2 = 0;
	float payload[16] = {};
};

static_assert(sizeof(NRISpatialAbsenceGpuRecord) == 80,
	"Spatial-absence records must match the HLSL layout.");

enum NRISpatialAbsenceFailOpenFlags : uint32_t
{
	NRI_SPATIAL_ABSENCE_FAIL_NONE = 0,
	NRI_SPATIAL_ABSENCE_FAIL_INVALID_WORLD = 1u << 0,
	NRI_SPATIAL_ABSENCE_FAIL_INCOMPLETE_CENSUS = 1u << 1,
	NRI_SPATIAL_ABSENCE_FAIL_UNSTABLE_ROOT = 1u << 2,
	NRI_SPATIAL_ABSENCE_FAIL_GENERATION_MISMATCH = 1u << 3,
	NRI_SPATIAL_ABSENCE_FAIL_INVALID_GUARD = 1u << 4,
	NRI_SPATIAL_ABSENCE_FAIL_AMBIGUOUS_CONFLICT = 1u << 5,
	NRI_SPATIAL_ABSENCE_FAIL_GPU_INDEX_OVERFLOW = 1u << 6,
	NRI_SPATIAL_ABSENCE_FAIL_GPU_PAYLOAD_INVALID = 1u << 7,
};

enum class NRISpatialAbsenceConflictDecision : uint32_t
{
	Certified = 0,
	Disjoint,
	BoundaryContact,
	SameVisibility,
	UnknownSector,
	LinkedAdjacent,
	PortalRelated,
	DifferentLocalSpace,
	OutsideGuard,
	ExactOverlapMissing,
	RuntimeAuthorityUnknown,
	AmbiguousNegativeOwner,
	OpenBoundary,
	NearOrdinaryTopology,
	CollapsedPortalEnvelope,
	InsetBoundaryEnclosure,
	ReachedMapFrontier,
};

enum NRISpatialAbsenceProtectionFlags : uint32_t
{
	NRI_SPATIAL_ABSENCE_PROTECTION_NONE = 0,
	NRI_SPATIAL_ABSENCE_PROTECTION_OPEN_BOUNDARY = 1u << 0,
	NRI_SPATIAL_ABSENCE_PROTECTION_NEAR_ORDINARY_TOPOLOGY = 1u << 1,
	NRI_SPATIAL_ABSENCE_PROTECTION_COLLAPSED_PORTAL_ENVELOPE = 1u << 2,
	NRI_SPATIAL_ABSENCE_PROTECTION_INSET_BOUNDARY_ENCLOSURE = 1u << 3,
	NRI_SPATIAL_ABSENCE_PROTECTION_REACHED_MAP_FRONTIER = 1u << 4,
};

struct NRISpatialAbsenceCensusInput
{
	bool complete = false;
	bool rootStable = false;
	bool generationMatches = false;
	uint64_t captureSerial = 0;
	uint64_t observationHash = 0;
	uint64_t worldGeneration = 0;
	uint32_t frameIndex = 0;
	int32_t authoritativeRootSector = -1;
	float center[3] = {};
	float guardRadius = 0.0f;
	bool probeEnabled = false;
	float probeOrigin[3] = {};
	float probeRadius = 0.0f;
	uint32_t probeExpectedChunk = UINT32_MAX;
	uint32_t probeTargetPixel = 0;
	uint32_t probeReferencePixel = 0;
	// Workload counters and sub-policy timers are collected only for bounded
	// spatial-absence traces. They are diagnostic state, never policy input.
	bool collectWorkloadTelemetry = false;
	std::vector<uint32_t> rootSectorIndices;
	std::vector<uint32_t> reachedSectorIndices;
	std::vector<uint32_t> reachedWallIndices;
	std::vector<uint32_t> uncertainSectorIndices;
	// Runtime-dragged/replaced chunks must remain fail-open until their current
	// occurrence bounds and generation are available to the classifier.
	std::vector<uint32_t> uncertainChunkIndices;
	// Authored sectors whose closed state is allowed to protect an adjacent
	// sealing-band carrier. Runtime plane coincidence alone is not sufficient
	// closure authority.
	std::vector<uint32_t> authoredClosureSectorIndices;
};

struct NRISpatialAbsenceWorkloadTelemetry
{
	uint32_t structuralProtectionCandidateCount = 0;
	uint32_t uniqueNegativeChunkCount = 0;
	uint32_t negativeProtectionMemoHitCount = 0;
	uint32_t negativeProtectionMemoMissCount = 0;
	uint32_t frontierIndexBuildCount = 0;
	uint32_t reachedAuthoredWallVisitCount = 0;
	uint32_t reciprocalLinkTestCount = 0;
	uint32_t apertureTestCount = 0;
	uint32_t closureDescriptorBuildCount = 0;
	uint32_t closureDescriptorHitCount = 0;
	uint32_t negativeBandSurfaceVisitCount = 0;
	double frontierTraversalElapsedMilliseconds = 0.0;
	double frontierIndexElapsedMilliseconds = 0.0;
	double negativeProtectionElapsedMilliseconds = 0.0;
};

struct NRISpatialAbsenceConflictRecord
{
	NRISpatialAbsenceConflictDecision decision = NRISpatialAbsenceConflictDecision::Disjoint;
	uint32_t positiveChunk = UINT32_MAX;
	uint32_t negativeChunk = UINT32_MAX;
	int32_t positiveSector = -1;
	int32_t negativeSector = -1;
	float overlapMin[3] = {};
	float overlapMax[3] = {};
	float distanceToGuardCenter = 0.0f;
	uint32_t exactWitnessCount = 0;
	uint32_t protectionFlags = NRI_SPATIAL_ABSENCE_PROTECTION_NONE;
	int32_t topologyIntermediateSector = -1;
	int32_t collapsedPortalSector = -1;
	int32_t insetBoundaryChildSector = -1;
	int32_t frontierReachedSector = -1;
	int32_t frontierReachedWall = -1;
	int32_t frontierClosureSector = -1;
	uint32_t continuityCount = 0;
	bool continuityPresentPrevious = false;
	bool continuityContextContinuous = false;
	bool continuityAuthorized = false;
};

// Camera-independent summary of the exact coverage for one census-negative
// chunk. The factorized GPU representation publishes every source footprint
// triangle and positive owner; selected counts therefore equal source counts.
struct NRISpatialAbsenceSelectionRecord
{
	uint32_t negativeChunk = UINT32_MAX;
	uint32_t firstPositiveChunk = UINT32_MAX;
	uint32_t sourceWitnessCount = 0;
	uint32_t selectedWitnessCount = 0;
	uint32_t sourcePositiveOwnerCount = 0;
	uint32_t selectedPositiveOwnerCount = 0;
	uint32_t footprintTriangleCount = 0;
	uint64_t sourcePositiveOwnerHash = 0;
	uint64_t selectedPositiveOwnerHash = 0;
	uint64_t selectionHash = 0;
	float boundsMin[3] = {};
	float boundsMax[3] = {};
};

struct NRISpatialAbsenceSnapshot
{
	bool valid = false;
	uint32_t failOpenFlags = NRI_SPATIAL_ABSENCE_FAIL_NONE;
	uint64_t captureSerial = 0;
	uint64_t worldGeneration = 0;
	uint64_t payloadHash = 0;
	// These hashes deliberately exclude frame index, guard center, and raw
	// distance-to-center values. They only change when classifier semantics or
	// the retained GPU witness selection changes.
	uint64_t semanticHash = 0;
	uint64_t selectionHash = 0;
	uint64_t censusObservationHash = 0;
	uint64_t previousCensusObservationHash = 0;
	uint32_t frameIndex = 0;
	uint32_t candidateCount = 0;
	uint32_t certifiedCount = 0;
	uint32_t sourceWitnessCount = 0;
	uint32_t selectedWitnessCount = 0;
	uint32_t openBoundaryProtectedCount = 0;
	uint32_t nearTopologyProtectedCount = 0;
	uint32_t collapsedPortalProtectedCount = 0;
	uint32_t insetBoundaryEnclosureProtectedCount = 0;
	uint32_t reachedMapFrontierProtectedCount = 0;
	uint32_t authorizedPairCount = 0;
	uint32_t pendingPairCount = 0;
	uint32_t footprintTriangleCount = 0;
	uint32_t footprintGridCellCount = 0;
	uint32_t footprintGridReferenceCount = 0;
	uint32_t stableCaptureCount = 0;
	double buildElapsedMilliseconds = 0.0;
	NRISpatialAbsenceWorkloadTelemetry workload;
	bool topologyCacheHit = false;
	uint32_t topologyPairCount = 0;
	int32_t authoritativeRootSector = -1;
	int32_t rootLocalSpaceIndex = -1;
	std::vector<uint32_t> rootSectorIndices;
	std::vector<uint32_t> reachedSectorIndices;
	std::vector<uint32_t> reachedWallIndices;
	bool previousAuthority = false;
	bool authorityTransition = false;
	float center[3] = {};
	float guardRadius = 0.0f;
	std::vector<uint32_t> negativeChunkWords;
	std::vector<uint32_t> reachedChunkWords;
	std::vector<NRISpatialAbsenceGpuRecord> gpuRecords;
	std::vector<NRISpatialAbsenceConflictRecord> conflicts;
	std::vector<NRISpatialAbsenceSelectionRecord> selections;

	bool HasCensusAuthority() const
	{
		bool hasReachedChunk = false;
		for (uint32_t word : reachedChunkWords)
			hasReachedChunk = hasReachedChunk || word != 0u;
		return valid && !reachedSectorIndices.empty() && !reachedChunkWords.empty() &&
			hasReachedChunk && !gpuRecords.empty() &&
			(gpuRecords[0].flags & NRI_SPATIAL_ABSENCE_GPU_RAY_QUERY_VALIDATED) != 0 &&
			(failOpenFlags & (NRI_SPATIAL_ABSENCE_FAIL_INVALID_WORLD |
				NRI_SPATIAL_ABSENCE_FAIL_INCOMPLETE_CENSUS |
				NRI_SPATIAL_ABSENCE_FAIL_UNSTABLE_ROOT |
				NRI_SPATIAL_ABSENCE_FAIL_GENERATION_MISMATCH |
				NRI_SPATIAL_ABSENCE_FAIL_INVALID_GUARD |
				NRI_SPATIAL_ABSENCE_FAIL_AMBIGUOUS_CONFLICT |
				NRI_SPATIAL_ABSENCE_FAIL_GPU_INDEX_OVERFLOW |
				NRI_SPATIAL_ABSENCE_FAIL_GPU_PAYLOAD_INVALID)) == 0;
	}

	bool HasNegativeAuthority() const
	{
		return certifiedCount != 0 && HasCensusAuthority();
	}
};

// Marks only currently authorized census-negative static occurrences for
// candidate-stage inline ray filtering. Other TLAS instances remain opaque.
uint32_t ApplyNRISpatialAbsenceRayQueryCandidateFlags(
	bool enabled,
	const NRISpatialAbsenceSnapshot& snapshot,
	std::vector<nri::TopLevelInstance>& tlasInstances,
	const std::vector<SceneInstanceData>& sceneInstances);

struct NRISpatialAbsenceContinuityKey
{
	uint64_t worldGeneration = 0;
	uint32_t positiveChunk = UINT32_MAX;
	uint32_t negativeChunk = UINT32_MAX;
	uint32_t localSpaceIndex = UINT32_MAX;

	bool operator<(const NRISpatialAbsenceContinuityKey& other) const;
	bool operator==(const NRISpatialAbsenceContinuityKey& other) const;
};

struct NRISpatialAbsenceContinuityRecord
{
	NRISpatialAbsenceContinuityKey key;
	uint32_t consecutiveCaptureCount = 0;
	bool presentPrevious = false;
	bool authorized = false;
};

class NRISpatialAbsenceGate
{
public:
	NRISpatialAbsenceGate();
	~NRISpatialAbsenceGate();
	NRISpatialAbsenceGate(const NRISpatialAbsenceGate&) = delete;
	NRISpatialAbsenceGate& operator=(const NRISpatialAbsenceGate&) = delete;

	const NRISpatialAbsenceSnapshot& Build(
		const nri_scene::PTMapWorld& mapWorld,
		const NRISpatialAbsenceCensusInput& census);
	void Reset(uint32_t frameIndex = 0, bool resetStability = true);

	const NRISpatialAbsenceSnapshot& GetSnapshot() const { return mSnapshot; }

private:
	struct TopologyCache;
	std::unique_ptr<TopologyCache> mTopologyCache;
	NRISpatialAbsenceSnapshot mSnapshot;
	uint64_t mStableWorldGeneration = 0;
	uint64_t mStableObservationHash = 0;
	uint32_t mStableCaptureCount = 0;
	bool mHasPreviousCensusObservationHash = false;
	uint64_t mPreviousCensusObservationHash = 0;
	bool mHasPreviousAuthority = false;
	bool mPreviousAuthority = false;
	uint64_t mPreviousContinuityCaptureSerial = 0;
	uint64_t mContinuityWorldGeneration = 0;
	int32_t mContinuityRootLocalSpaceIndex = -1;
	std::vector<NRISpatialAbsenceContinuityRecord> mConflictContinuity;
};

// Synthetic, device-independent checks for the conservative classifier and
// fail-open contract. The caller owns how failures are surfaced.
bool RunNRISpatialAbsenceGateSelfTests(std::string* failureReason = nullptr);
const char* GetNRISpatialAbsenceConflictDecisionName(NRISpatialAbsenceConflictDecision decision);
