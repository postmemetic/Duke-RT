#pragma once

#include "../scene/nri_map_world.h"

#include <cstdint>
#include <string>
#include <vector>

enum NRISpatialAbsenceGpuFlags : uint32_t
{
	NRI_SPATIAL_ABSENCE_GPU_VALID = 1u << 0,
	NRI_SPATIAL_ABSENCE_GPU_COMPLETE = 1u << 1,
	NRI_SPATIAL_ABSENCE_GPU_CERTIFIED = 1u << 2,
};

// Record zero is the snapshot header. The next chunkCount records form the
// chunk lookup table; exact XY triangle-overlap witnesses follow it. This
// keeps the shader lookup independent of transient TLAS instance indices and
// avoids treating a broad-phase chunk AABB intersection as negative authority.
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
	float center[3] = {};
	float guardRadius = 0.0f;
	std::vector<uint32_t> reachedSectorIndices;
	std::vector<uint32_t> uncertainSectorIndices;
	// Runtime-dragged/replaced chunks must remain fail-open until their current
	// occurrence bounds and generation are available to the classifier.
	std::vector<uint32_t> uncertainChunkIndices;
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
};

// Camera-independent summary of the exact witnesses retained for one
// census-negative chunk. Source counts describe classifier coverage before
// the bounded GPU selection; selected counts and hashes describe what the
// shader can actually test this frame.
struct NRISpatialAbsenceSelectionRecord
{
	uint32_t negativeChunk = UINT32_MAX;
	uint32_t firstPositiveChunk = UINT32_MAX;
	uint32_t sourceWitnessCount = 0;
	uint32_t selectedWitnessCount = 0;
	uint32_t sourcePositiveOwnerCount = 0;
	uint32_t selectedPositiveOwnerCount = 0;
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
	uint32_t stableCaptureCount = 0;
	bool previousAuthority = false;
	bool authorityTransition = false;
	float center[3] = {};
	float guardRadius = 0.0f;
	std::vector<uint32_t> negativeChunkWords;
	std::vector<NRISpatialAbsenceGpuRecord> gpuRecords;
	std::vector<NRISpatialAbsenceConflictRecord> conflicts;
	std::vector<NRISpatialAbsenceSelectionRecord> selections;

	bool HasNegativeAuthority() const
	{
		return valid && certifiedCount != 0 &&
			(failOpenFlags & (NRI_SPATIAL_ABSENCE_FAIL_INVALID_WORLD |
				NRI_SPATIAL_ABSENCE_FAIL_INCOMPLETE_CENSUS |
				NRI_SPATIAL_ABSENCE_FAIL_UNSTABLE_ROOT |
				NRI_SPATIAL_ABSENCE_FAIL_GENERATION_MISMATCH |
				NRI_SPATIAL_ABSENCE_FAIL_INVALID_GUARD |
				NRI_SPATIAL_ABSENCE_FAIL_AMBIGUOUS_CONFLICT)) == 0;
	}
};

class NRISpatialAbsenceGate
{
public:
	const NRISpatialAbsenceSnapshot& Build(
		const nri_scene::PTMapWorld& mapWorld,
		const NRISpatialAbsenceCensusInput& census);
	void Reset(uint32_t frameIndex = 0, bool resetStability = true);

	const NRISpatialAbsenceSnapshot& GetSnapshot() const { return mSnapshot; }

private:
	NRISpatialAbsenceSnapshot mSnapshot;
	uint64_t mStableWorldGeneration = 0;
	uint64_t mStableObservationHash = 0;
	uint32_t mStableCaptureCount = 0;
	bool mHasPreviousCensusObservationHash = false;
	uint64_t mPreviousCensusObservationHash = 0;
	bool mHasPreviousAuthority = false;
	bool mPreviousAuthority = false;
};

// Synthetic, device-independent checks for the conservative classifier and
// fail-open contract. The caller owns how failures are surfaced.
bool RunNRISpatialAbsenceGateSelfTests(std::string* failureReason = nullptr);
const char* GetNRISpatialAbsenceConflictDecisionName(NRISpatialAbsenceConflictDecision decision);
