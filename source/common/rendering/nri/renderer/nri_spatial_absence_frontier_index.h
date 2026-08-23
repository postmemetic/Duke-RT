#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct NRISpatialAbsenceFrontierRoute
{
	int32_t reachedSector = -1;
	int32_t reachedWall = -1;
	int32_t closureSector = -1;
	int32_t negativeWall = -1;

	bool IsValid() const { return reachedSector >= 0 && reachedWall >= 0; }
};

struct NRISpatialAbsenceFrontierIndexStats
{
	uint32_t reachedSectorCount = 0;
	uint32_t reachedAuthoredWallCount = 0;
	uint32_t reciprocalLinkTestCount = 0;
	uint32_t apertureTestCount = 0;
	uint32_t closureDescriptorBuildCount = 0;
	uint32_t closureDescriptorHitCount = 0;
	uint32_t directRouteCount = 0;
	uint32_t closureRouteCount = 0;
};

// Live topology index whose results are valid only for the gate Build() that owns it.
// Exact negative-chunk wall-band provenance remains a gate policy decision.
class NRISpatialAbsenceFrontierIndex
{
public:
	void Build(
		const std::vector<uint32_t>& reachedSectorIndices,
		const std::vector<uint32_t>& authoredClosureSectorIndices);

	const NRISpatialAbsenceFrontierRoute* FindDirect(int32_t negativeSector) const;
	const std::vector<NRISpatialAbsenceFrontierRoute>* FindClosures(int32_t negativeSector) const;
	const NRISpatialAbsenceFrontierIndexStats& GetStats() const { return stats; }
	bool IsBuilt() const { return built; }

private:
	std::vector<NRISpatialAbsenceFrontierRoute> directRoutes;
	std::vector<std::vector<NRISpatialAbsenceFrontierRoute>> closureRoutes;
	NRISpatialAbsenceFrontierIndexStats stats;
	bool built = false;
};

bool RunNRISpatialAbsenceFrontierIndexSelfTests(std::string* failureReason = nullptr);
