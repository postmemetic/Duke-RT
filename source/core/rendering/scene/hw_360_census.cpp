#include "hw_360_census.h"

#include <algorithm>
#include <cstdint>

#include "build.h"
#include "c_dispatch.h"
#include "hw_sections.h"
#include "i_time.h"

CVAR(Int, hw_360censustrace, 0, 0)

namespace
{
	struct HW360CensusStatus
	{
		bool available = false;
		bool complete = false;
		uint32_t failureFlags = 0;
		uint32_t observationFlags = 0;
		uint64_t captureSerial = 0;
		uint64_t mapGeneration = 0;
		uint64_t mapStorageGeneration = 0;
		uint64_t observationHash = 0;
		DVector3 origin = {};
		int authoritativeRoot = -1;
		uint32_t rootCount = 0;
		int firstRoot = -1;
		uint32_t reachedSectorCount = 0;
		uint32_t reachedSectionCount = 0;
		uint32_t reachedWallCount = 0;
		BunchDrawerCensusStats traversal = {};
		double elapsedMilliseconds = 0.0;
	};

	HW360CensusStatus gLatestStatus;

	uint64_t HashValue(uint64_t hash, uint64_t value)
	{
		constexpr uint64_t Prime = 1099511628211ull;
		for (unsigned i = 0; i < 8; ++i)
		{
			hash ^= uint8_t(value >> (i * 8));
			hash *= Prime;
		}
		return hash;
	}

	uint64_t CurrentMapGeneration()
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashValue(hash, reinterpret_cast<uintptr_t>(sector.Data()));
		hash = HashValue(hash, sector.Size());
		hash = HashValue(hash, reinterpret_cast<uintptr_t>(wall.Data()));
		hash = HashValue(hash, wall.Size());
		hash = HashValue(hash, reinterpret_cast<uintptr_t>(sections.Data()));
		hash = HashValue(hash, sections.Size());
		hash = HashValue(hash, reinterpret_cast<uintptr_t>(sectionLines.Data()));
		hash = HashValue(hash, sectionLines.Size());
		hash = HashValue(hash, reinterpret_cast<uintptr_t>(sectionsPerSector.Data()));
		hash = HashValue(hash, sectionsPerSector.Size());
		return hash;
	}

	uint32_t CountBits(const BitArray& bits)
	{
		uint32_t count = 0;
		for (unsigned i = 0; i < bits.Size(); ++i)
		{
			count += bits[i];
		}
		return count;
	}

	uint64_t HashSnapshotObservations(const HW360CensusSnapshot& snapshot)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashValue(hash, snapshot.mapGeneration);
		hash = HashValue(hash, uint32_t(snapshot.authoritativeRoot));
		for (auto root : snapshot.roots) hash = HashValue(hash, uint32_t(root));
		for (unsigned i = 0; i < snapshot.reachedSectors.Size(); ++i)
		{
			if (snapshot.reachedSectors[i]) hash = HashValue(hash, i);
		}
		for (unsigned i = 0; i < snapshot.reachedSections.Size(); ++i)
		{
			if (snapshot.reachedSections[i]) hash = HashValue(hash, uint64_t(i) | (1ull << 62));
		}
		for (unsigned i = 0; i < snapshot.reachedWalls.Size(); ++i)
		{
			if (snapshot.reachedWalls[i]) hash = HashValue(hash, uint64_t(i) | (2ull << 62));
		}
		return hash;
	}

	void PublishStatus(const HW360CensusSnapshot& snapshot)
	{
		gLatestStatus.available = true;
		gLatestStatus.complete = snapshot.complete;
		gLatestStatus.failureFlags = snapshot.failureFlags;
		gLatestStatus.observationFlags = snapshot.observationFlags;
		gLatestStatus.captureSerial = snapshot.captureSerial;
		gLatestStatus.mapGeneration = snapshot.mapGeneration;
		gLatestStatus.mapStorageGeneration = snapshot.mapStorageGeneration;
		gLatestStatus.observationHash = snapshot.observationHash;
		gLatestStatus.origin = snapshot.origin;
		gLatestStatus.authoritativeRoot = snapshot.authoritativeRoot;
		gLatestStatus.rootCount = snapshot.roots.Size();
		gLatestStatus.firstRoot = snapshot.roots.Size() > 0 ? snapshot.roots[0] : -1;
		gLatestStatus.reachedSectorCount = snapshot.reachedSectorCount;
		gLatestStatus.reachedSectionCount = snapshot.reachedSectionCount;
		gLatestStatus.reachedWallCount = snapshot.reachedWallCount;
		gLatestStatus.traversal = snapshot.traversal;
		gLatestStatus.elapsedMilliseconds = snapshot.elapsedMilliseconds;
	}

	void PrintStatus(const char* event)
	{
		if (!gLatestStatus.available)
		{
			Printf("HW 360 census: event=%s available=0\n", event);
			return;
		}

		Printf("HW 360 census: event=%s available=1 serial=%llu complete=%u failures=0x%08x observations=0x%08x map_generation=%llu map_storage_generation=%llu root_count=%u first_root=%d authoritative_root=%d origin=[%.3f,%.3f,%.3f] hash=%llu sectors=%u sections=%u walls=%u section_visits=%u wall_tests=%u bunches_started=%u bunches_processed=%u scratch_growths=%u elapsed_ms=%.3f\n",
			event,
			(unsigned long long)gLatestStatus.captureSerial,
			gLatestStatus.complete ? 1u : 0u,
			gLatestStatus.failureFlags,
			gLatestStatus.observationFlags,
			(unsigned long long)gLatestStatus.mapGeneration,
			(unsigned long long)gLatestStatus.mapStorageGeneration,
			gLatestStatus.rootCount,
			gLatestStatus.firstRoot,
			gLatestStatus.authoritativeRoot,
			gLatestStatus.origin.X, gLatestStatus.origin.Y, gLatestStatus.origin.Z,
			(unsigned long long)gLatestStatus.observationHash,
			gLatestStatus.reachedSectorCount,
			gLatestStatus.reachedSectionCount,
			gLatestStatus.reachedWallCount,
			gLatestStatus.traversal.sectionVisits,
			gLatestStatus.traversal.wallTests,
			gLatestStatus.traversal.bunchesStarted,
			gLatestStatus.traversal.bunchesProcessed,
			gLatestStatus.traversal.scratchArrayGrowths,
			gLatestStatus.elapsedMilliseconds);
	}
}

CCMD(hw_360census_status)
{
	PrintStatus("status");
}

bool RunHW360CensusContractSelfTest()
{
	const int soleExplicitRoot = 290;
	if (ResolveHW360CensusAuthoritativeRoot(&soleExplicitRoot, 1, 293, true) != 290)
		return false;
	if (ResolveHW360CensusAuthoritativeRoot(&soleExplicitRoot, 1, 293, false) != 293)
		return false;
	const int portalRoots[2] = { 290, 293 };
	if (ResolveHW360CensusAuthoritativeRoot(portalRoots, 2, 293, true) != 293)
		return false;
	if (ResolveHW360CensusAuthoritativeRoot(portalRoots, 2, 291, true) != 291)
		return false;

	HW360CensusSnapshot empty;
	if (empty.HasNegativeAuthority()) return false;

	HW360CensusSnapshot first;
	first.complete = true;
	first.failureFlags = HW360CensusFailure_None;
	first.mapGeneration = 7;
	first.authoritativeRoot = 3;
	first.roots.Push(3);
	first.reachedSectors.Resize(8);
	first.reachedSectors.Zero();
	first.reachedSectors.Set(3);
	first.reachedSectors.Set(6);
	first.reachedSections.Resize(8);
	first.reachedSections.Zero();
	first.reachedSections.Set(2);
	first.reachedSections.Set(5);
	first.observationHash = HashSnapshotObservations(first);

	HW360CensusSnapshot equivalent = first;
	if (!first.HasNegativeAuthority() || first.observationHash == 0 ||
		HashSnapshotObservations(equivalent) != first.observationHash)
	{
		return false;
	}

	equivalent.mapGeneration++;
	if (HashSnapshotObservations(equivalent) == first.observationHash) return false;

	equivalent = first;
	equivalent.failureFlags |= HW360CensusFailure_MapGenerationChanged;
	equivalent.complete = false;
	return !equivalent.HasNegativeAuthority();
}

CCMD(hw_360census_selftest)
{
	Printf("HW 360 census self-test: result=%s\n", RunHW360CensusContractSelfTest() ? "pass" : "fail");
}

int ResolveHW360CensusAuthoritativeRoot(
	const int* roots, unsigned rootCount, int cameraActorRoot, bool explicitRoots)
{
	if (cameraActorRoot < 0 || roots == nullptr || rootCount == 0)
		return cameraActorRoot;
	if (std::find(roots, roots + rootCount, cameraActorRoot) != roots + rootCount)
		return cameraActorRoot;
	// Explicit SectNums are the roots actually consumed by CreateScene. During
	// interpolated sector crossings the camera actor can relink one render frame
	// before that list. A sole explicit root is still unambiguous presentation
	// authority; multi-root disagreements remain fail-open.
	if (explicitRoots && rootCount == 1)
		return roots[0];
	return cameraActorRoot;
}

void HW360Census::Capture(const DVector3& renderOrigin, const int* roots, unsigned rootCount, int authoritativeRoot,
	uint64_t mapGeneration, bool logicalMainView, angle_t canonicalSplitAngle)
{
	HW360CensusSnapshot next;
	next.captureSerial = ++mCaptureSerial;
	next.origin = renderOrigin;
	next.authoritativeRoot = authoritativeRoot;
	next.canonicalSplitAngle = canonicalSplitAngle;
	next.failureFlags = HW360CensusFailure_None;
	next.mapGeneration = mapGeneration;
	next.mapStorageGeneration = CurrentMapGeneration();

	if (!logicalMainView) next.failureFlags |= HW360CensusFailure_UnsupportedContext;
	if (mapGeneration == 0) next.failureFlags |= HW360CensusFailure_MissingGeneration;
	if (roots == nullptr || rootCount == 0) next.failureFlags |= HW360CensusFailure_MissingRoot;
	if (sector.Size() == 0 || wall.Size() == 0 || sections.Size() == 0 || sectionLines.Size() == 0 ||
		sectionsPerSector.Size() != sector.Size())
	{
		next.failureFlags |= HW360CensusFailure_MissingTopology;
	}

	if (roots != nullptr)
	{
		for (unsigned i = 0; i < rootCount; ++i)
		{
			if (roots[i] < 0 || unsigned(roots[i]) >= sector.Size())
			{
				next.failureFlags |= HW360CensusFailure_InvalidRoot;
				continue;
			}
			next.roots.Push(roots[i]);
		}
	}

	if (next.roots.Size() > 1)
	{
		std::sort(next.roots.begin(), next.roots.end());
		unsigned writeIndex = 1;
		for (unsigned readIndex = 1; readIndex < next.roots.Size(); ++readIndex)
		{
			if (next.roots[readIndex] != next.roots[writeIndex - 1])
			{
				next.roots[writeIndex++] = next.roots[readIndex];
			}
		}
		next.roots.Resize(writeIndex);
		if (next.roots.Size() > 1) next.observationFlags |= HW360CensusObservation_MultipleRoots;
	}

	if (next.roots.Size() == 0) next.failureFlags |= HW360CensusFailure_MissingRoot;
	if (authoritativeRoot < 0 || unsigned(authoritativeRoot) >= sector.Size() ||
		std::find(next.roots.begin(), next.roots.end(), authoritativeRoot) == next.roots.end())
	{
		next.failureFlags |= HW360CensusFailure_AuthoritativeRootMismatch;
	}

	const double startMilliseconds = I_msTimeF();
	if (next.failureFlags == HW360CensusFailure_None)
	{
		mClipper.SetVisibleRange(0, 0xffffffff);
		mDrawer.InitCensus(&mClipper, renderOrigin, canonicalSplitAngle);
		mDrawer.RenderScene(next.roots.Data(), next.roots.Size(), false);
		next.reachedSectors = mDrawer.GotSector();
		next.reachedSections = mDrawer.GotSection();
		next.reachedWalls = mDrawer.GotWall();
		next.traversal = mDrawer.CensusStats();
		if (next.traversal.encounteredDraggedSector)
		{
			next.observationFlags |= HW360CensusObservation_DraggedSector;
		}
	}
	next.elapsedMilliseconds = I_msTimeF() - startMilliseconds;

	if (CurrentMapGeneration() != next.mapStorageGeneration)
	{
		next.failureFlags |= HW360CensusFailure_MapGenerationChanged;
	}

	next.complete = next.failureFlags == HW360CensusFailure_None;
	if (next.complete)
	{
		next.reachedSectorCount = CountBits(next.reachedSectors);
		next.reachedSectionCount = CountBits(next.reachedSections);
		next.reachedWallCount = CountBits(next.reachedWalls);
		next.observationHash = HashSnapshotObservations(next);
	}
	else
	{
		// Incomplete captures publish diagnostics but no observations that a
		// consumer could accidentally complement into negative authority.
		next.reachedSectors = {};
		next.reachedSections = {};
		next.reachedWalls = {};
		next.observationHash = 0;
	}

	mSnapshot = std::move(next);
	PublishStatus(mSnapshot);
	if ((int)hw_360censustrace > 0) PrintStatus("capture");
}
