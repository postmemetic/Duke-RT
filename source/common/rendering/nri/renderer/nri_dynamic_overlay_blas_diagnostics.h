#pragma once

#include <cstdint>

struct NRIDynamicOverlayBlasPolicyStats
{
	int32_t requestedPolicy = 0;
	uint32_t effectivePolicy = 0;
	uint32_t buildFlags = 0;
	bool requestedBuild = false;
	bool requestedRoute = false;
	bool filterPartition = false;
	bool effectiveBuild = false;
	bool effectiveRoute = false;
	uint32_t cacheLimit = 0;
	uint32_t cachedAssets = 0;
	uint32_t touchedAssets = 0;
	uint32_t cacheHitAgeMaxFrames = 0;
	uint64_t cacheHitAgeSumFrames = 0;
	uint64_t cachedAsBytes = 0;
	uint64_t cachedGeometryBytes = 0;
	uint64_t touchedAsBytes = 0;
	uint64_t builtAsBytes = 0;
	uint64_t buildScratchMaxBytes = 0;
	uint64_t sharedScratchBytes = 0;
	double totalCpuMs = 0.0;
	double coldCpuMs = 0.0;
};

void LogNRIDynamicOverlayBlasPolicyStats(uint64_t frameNumber, const NRIDynamicOverlayBlasPolicyStats& stats);
