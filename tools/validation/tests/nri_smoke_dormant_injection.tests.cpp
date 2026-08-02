#include "nri_smoke_dormant_injection.h"

#include <cmath>
#include <cstdio>

namespace
{
	int failures = 0;
	void Check(bool condition, const char* message)
	{
		if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); failures++; }
	}
}

int main()
{
	std::vector<NRISmokeStyleGpu> styles(1);
	styles[0].density = 2.0f;
	styles[0].extinction = 0.5f;
	styles[0].albedo[0] = 1.0f;
	styles[0].albedo[1] = 0.5f;
	styles[0].albedo[2] = 0.25f;
	styles[0].riseVelocity = 4.0f;

	NRISmokeContinuousSourceWorkRequest request = {};
	request.firstCadenceOrdinal = 1u;
	request.lastCadenceOrdinal = 3u;
	request.epoch = 7u;
	request.sourceId = 9u;
	request.styleIndex = 0u;
	request.particlesPerCadence = 2u;
	request.aggregateCadenceSteps = 3u;
	request.position[0] = 16.0f;
	request.position[1] = 16.0f;
	request.position[2] = 16.0f;
	request.spawnRadius = 8.0f;
	request.densityScale = 1.0f;
	request.radiusScale = 1.0f;
	request.requiresEstablishedAuthority = true;
	std::vector<NRISmokeContinuousSourceWorkRequest> requests = { request };

	NRISmokeSpatialBrickObservation authority = {};
	authority.coordinate = { 0, 0, 0 };
	authority.generation = 5u;
	authority.authority = NRISmokeSpatialAuthority::Coarse;
	authority.occupied = true;
	std::map<NRISmokeSpatialCoordinate, NRISmokeSpatialBrickObservation> authorities;
	authorities[authority.coordinate] = authority;
	std::set<NRISmokeSpatialCoordinate> promotions;

	NRISmokeDormantInjectionBuildInput input = {};
	input.epoch = 7u;
	input.cellSize = 8.0f;
	input.requests = &requests;
	input.styles = &styles;
	input.authorities = &authorities;
	input.promotions = &promotions;
	auto result = NRIBuildSmokeDormantInjections(input);
	Check(result.routedSources == 1u && result.injections.size() == 1u,
		"established coarse source must route exactly once");
	Check(result.routedSourceIds.count(9u) == 1u,
		"routed source identity must be explicit");
	const auto& injection = result.injections[0];
	Check(injection.generation == 5u && injection.epoch == 7u,
		"archive generation and epoch must survive conversion");
	Check(std::abs(injection.scalar[0] - 12.0f) < 0.0001f,
		"cadence and particle count must become conserved mass");
	Check(std::abs(injection.scalar[2] - 6.0f) < 0.0001f,
		"extinction moment must match conserved mass");
	Check(std::abs(injection.momentum[1] - 48.0f) < 0.0001f,
		"rise velocity must become a mass-weighted momentum moment");

	requests[0].firstCadenceOrdinal = 4u;
	requests[0].lastCadenceOrdinal = 7u;
	requests[0].aggregateCadenceSteps = 4u;
	requests[0].pulseEnvelope = { 0.5f, 4u, 0.0f };
	result = NRIBuildSmokeDormantInjections(input);
	Check(result.injections.size() == 1u &&
		std::abs(result.injections[0].scalar[0] - 16.0f) < 0.0001f,
		"a complete coalesced pulse cycle must preserve unmodulated mass");
	requests[0].firstCadenceOrdinal = 1u;
	requests[0].lastCadenceOrdinal = 2u;
	requests[0].aggregateCadenceSteps = 2u;
	result = NRIBuildSmokeDormantInjections(input);
	Check(result.injections.size() == 1u &&
		std::abs(result.injections[0].scalar[0] - 6.0f) < 0.0001f,
		"a partial coalesced pulse cycle must sum its exact ordinal weights");
	requests[0].aggregateCadenceSteps = 3u;
	result = NRIBuildSmokeDormantInjections(input);
	Check(result.injections.empty() && result.invalidRequests == 1u,
		"mismatched cadence ranges must fail closed");
	requests[0] = request;

	promotions.insert(authority.coordinate);
	result = NRIBuildSmokeDormantInjections(input);
	Check(result.injections.empty() && result.promotionConflicts == 1u,
		"a source must remain on the fine route while its authority promotes");
	promotions.clear();
	authorities.clear();
	result = NRIBuildSmokeDormantInjections(input);
	Check(result.injections.empty() && result.missingAuthorities == 1u,
		"conversion must never create missing archive authority");

	if (failures != 0) return 1;
	std::puts("Smoke dormant injection conversion tests passed.");
	return 0;
}
