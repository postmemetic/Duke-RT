#pragma once

#include "nri_sector_color_policy.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nri_scene
{
struct SectorEmissionHueCacheStats
{
	uint64_t requests = 0;
	uint64_t rawResolutions = 0;
	uint64_t hits = 0;
	uint64_t validationChecks = 0;
	uint64_t validationMismatches = 0;
	uint64_t operationQuarantines = 0;
	uint64_t nestedInvalidations = 0;
	uint64_t resolveNs = 0;
	uint64_t rawResolveNs = 0;
};

class SectorEmissionHueCache
{
public:
	void Reset();
	uint64_t CapacityBytes() const;
private:
	friend class ScopedSectorEmissionHueCache;
	struct Entry
	{
		SectorEmissionHue hue;
		uint64_t epoch = 0;
	};
	uint64_t NewEpoch();
	std::vector<Entry> mEntries;
	uint64_t mEpochSerial = 0;
};

using SectorEmissionHueResolver = SectorEmissionHue (*)(int sectorIndex, bool neighbors);

// One synchronous sampling update only. Sources must not be mutated by game,
// editor or script callbacks within this operation. Nested operations invalidate
// the enclosing operation on return; no result survives an operation boundary.
class ScopedSectorEmissionHueCache
{
public:
	ScopedSectorEmissionHueCache(SectorEmissionHueCache& cache, size_t sectorCount, bool enabled, bool validate);
	~ScopedSectorEmissionHueCache();
	ScopedSectorEmissionHueCache(const ScopedSectorEmissionHueCache&) = delete;
	ScopedSectorEmissionHueCache& operator=(const ScopedSectorEmissionHueCache&) = delete;
	const SectorEmissionHueCacheStats& Stats() const { return mStats; }
private:
	friend SectorEmissionHue ResolveCachedSectorEmissionHue(int, bool, SectorEmissionHueResolver);
	SectorEmissionHue Resolve(int sectorIndex, bool neighbors, SectorEmissionHueResolver raw);
	SectorEmissionHueCache& mCache;
	ScopedSectorEmissionHueCache* mPrevious = nullptr;
	SectorEmissionHueCacheStats mStats;
	uint64_t mEpoch = 0;
	uint64_t mStartNs = 0;
	bool mEnabled = false;
	bool mValidate = false;
	bool mQuarantined = false;
	bool mCollectTiming = false;
};

// Uncached payload/reference construction must resolve live engine state even
// while an outer sampling update has an active operation cache.
class ScopedSectorEmissionHueCacheBypass
{
public:
	explicit ScopedSectorEmissionHueCacheBypass(bool bypass);
	~ScopedSectorEmissionHueCacheBypass();
	ScopedSectorEmissionHueCacheBypass(const ScopedSectorEmissionHueCacheBypass&) = delete;
	ScopedSectorEmissionHueCacheBypass& operator=(const ScopedSectorEmissionHueCacheBypass&) = delete;
private:
	bool mBypass;
};

SectorEmissionHue ResolveCachedSectorEmissionHue(int sectorIndex, bool neighbors, SectorEmissionHueResolver raw);
}
