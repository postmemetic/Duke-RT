#pragma once

#include "nri_sector_color_policy.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace nri_scene
{
// Copied semantic inputs. No engine pointer is retained in a published result.
struct SectorHueRuleInput
{
	int sector = -1, palette = -1, paletteSource = 0;
	bool mapScoped = false, enabled = true, hasColor = false, hasStrength = false, hasGain = false;
	float color[3] = {1, 1, 1}, strength = 1, gain = 1;
};
struct SectorHueDirectInput { uint8_t floorPalette = 0, ceilingPalette = 0; bool ceilingSky = false; };
struct SectorHuePlaneInput
{
	double ceilingZ = 0, floorZ = 0, origin[2] = {}, end[2] = {}, length = 0;
	int ceilingSlope = 0, floorSlope = 0;
	bool ceilingSloped = false, floorSloped = false;
};
struct SectorHueWallInput
{
	int nextSector = -1, point2 = -1;
	uint32_t blocked = 0;
	double start[2] = {}, end[2] = {};
};
// All callbacks run synchronously on the owner thread. Callers must not mutate
// map/rule state during the lease; no game/editor/script callback occurs here.
struct SectorHueInputSource
{
	const void* context = nullptr;
	size_t sectors = 0, walls = 0, rules = 0;
	bool enabled = false;
	uint64_t captureNs = 0;
	bool (*rule)(const void*, size_t, SectorHueRuleInput&) = nullptr;
	bool (*direct)(int, SectorHueDirectInput&) = nullptr;
	bool (*plane)(int, SectorHuePlaneInput&) = nullptr;
	bool (*wallRange)(int, size_t&, size_t&) = nullptr;
	bool (*wall)(size_t, SectorHueWallInput&) = nullptr;
	bool (*openPortal)(int, size_t) = nullptr;
};
SectorHueInputSource CaptureSectorHueInputSource();

struct SectorHueProductStats
{
	uint64_t requests = 0, hits = 0, directResults = 0, borrowedResults = 0, neutralResults = 0;
	uint64_t builds = 0, policyRows = 0, policyChanges = 0, directCaptures = 0, planeCaptures = 0;
	uint64_t wallCaptures = 0, portalEvaluations = 0, dependencies = 0;
	uint64_t fallbacks = 0, capacityFallbacks = 0, nestedFallbacks = 0;
	uint64_t validationChecks = 0, hitValidationChecks = 0, neutralValidationChecks = 0;
	uint64_t validationMismatches = 0, quarantines = 0, resolveNs = 0, prepareNs = 0, referenceNs = 0;
	uint64_t rawCalls = 0, bypassRawCalls = 0, avoidedRawResolutions = 0;
	uint64_t directInputChanges = 0, directChanges = 0, planeChanges = 0, adjacencyChanges = 0;
	uint64_t directInvalidations = 0, adjacencyInvalidations = 0;
	uint64_t allocations = 0, allocatedBytes = 0, checkNs = 0, buildNs = 0, bypassRawNs = 0;
};
class ScopedSectorHueProduct;
class SectorHueProduct
{
public:
	SectorHueProduct();
	~SectorHueProduct();
	void Reset();
	uint64_t CapacityBytes() const;
	SectorHueProduct(const SectorHueProduct&) = delete;
	SectorHueProduct& operator=(const SectorHueProduct&) = delete;
private:
	friend class ScopedSectorHueProduct;
	struct State;
	std::unique_ptr<State> mState;
	uint64_t mLifetime = 1;
};

// Modes: 0 original; 1 shadow (always returns raw); 2 consume certified result.
// One latest query result per key, bounded dependencies; overflow stays raw.
class ScopedSectorHueProduct
{
public:
	ScopedSectorHueProduct(SectorHueProduct& owner, int mode, bool validate);
	ScopedSectorHueProduct(SectorHueProduct& owner, const SectorHueInputSource& source, int mode, bool validate);
	~ScopedSectorHueProduct();
	ScopedSectorHueProduct(const ScopedSectorHueProduct&) = delete;
	ScopedSectorHueProduct& operator=(const ScopedSectorHueProduct&) = delete;
	const SectorHueProductStats& Stats() const { return mStats; }
private:
	friend SectorEmissionHue ResolveSectorHueProduct(int, bool, SectorEmissionHue (*)(int, bool));
	SectorEmissionHue Resolve(int sector, bool neighbors, SectorEmissionHue (*raw)(int, bool));
	SectorHueProduct& mOwner;
	SectorHueInputSource mSource;
	ScopedSectorHueProduct* mPrevious = nullptr;
	SectorHueProductStats mStats;
	uint64_t mLifetime = 0, mEpoch = 0, mStartNs = 0;
	int mMode = 0;
	bool mValidate = false, mInterrupted = false, mTiming = false;
};
SectorEmissionHue ResolveSectorHueProduct(int sector, bool neighbors, SectorEmissionHue (*raw)(int, bool));
}
