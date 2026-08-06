#pragma once

#include <cstdint>

#include "basics.h"
#include "tarray.h"
#include "vectors.h"
#include "hw_bunchdrawer.h"
#include "hw_clipper.h"

enum EHW360CensusFailure : uint32_t
{
	HW360CensusFailure_None = 0,
	HW360CensusFailure_UnsupportedContext = 1u << 0,
	HW360CensusFailure_MissingRoot = 1u << 1,
	HW360CensusFailure_InvalidRoot = 1u << 2,
	HW360CensusFailure_MissingTopology = 1u << 3,
	HW360CensusFailure_MapGenerationChanged = 1u << 4,
	HW360CensusFailure_MissingGeneration = 1u << 5,
	HW360CensusFailure_AuthoritativeRootMismatch = 1u << 6,
};

enum EHW360CensusObservation : uint32_t
{
	HW360CensusObservation_None = 0,
	HW360CensusObservation_MultipleRoots = 1u << 0,
	HW360CensusObservation_DraggedSector = 1u << 1,
};

struct HW360CensusSnapshot
{
	bool complete = false;
	uint32_t failureFlags = HW360CensusFailure_MissingRoot;
	uint32_t observationFlags = HW360CensusObservation_None;
	uint64_t captureSerial = 0;
	uint64_t mapGeneration = 0;
	uint64_t mapStorageGeneration = 0;
	uint64_t observationHash = 0;
	DVector3 origin = {};
	int authoritativeRoot = -1;
	angle_t canonicalSplitAngle = 0;
	TArray<int> roots;
	BitArray reachedSectors;
	BitArray reachedSections;
	BitArray reachedWalls;
	uint32_t reachedSectorCount = 0;
	uint32_t reachedSectionCount = 0;
	uint32_t reachedWallCount = 0;
	BunchDrawerCensusStats traversal = {};
	double elapsedMilliseconds = 0.0;

	bool HasNegativeAuthority() const { return complete && failureFlags == HW360CensusFailure_None; }
	bool ContainsSector(unsigned sectorIndex) const
	{
		return sectorIndex < reachedSectors.Size() && reachedSectors[sectorIndex];
	}
};

class HW360Census
{
	BunchDrawer mDrawer;
	Clipper mClipper;
	HW360CensusSnapshot mSnapshot;
	uint64_t mCaptureSerial = 0;

public:
	void Capture(const DVector3& renderOrigin, const int* roots, unsigned rootCount, int authoritativeRoot,
		uint64_t mapGeneration, bool logicalMainView, angle_t canonicalSplitAngle = 0);
	const HW360CensusSnapshot& Snapshot() const { return mSnapshot; }
};

bool RunHW360CensusContractSelfTest();
