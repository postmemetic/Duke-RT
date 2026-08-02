#include "nri_smoke_dormant_injection.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	constexpr float kLn2 = 0.69314718056f;

	bool Finite3(const float value[3])
	{
		return std::isfinite(value[0]) && std::isfinite(value[1]) &&
			std::isfinite(value[2]);
	}

	int32_t BrickCoordinate(float position, float brickSize)
	{
		const double value = std::floor((double)position / (double)brickSize);
		return (int32_t)std::clamp(value, (double)std::numeric_limits<int32_t>::min(),
			(double)std::numeric_limits<int32_t>::max());
	}
}

NRISmokeDormantInjectionBuildResult NRIBuildSmokeDormantInjections(
	const NRISmokeDormantInjectionBuildInput& input)
{
	NRISmokeDormantInjectionBuildResult result = {};
	if (input.requests == nullptr || input.styles == nullptr || input.authorities == nullptr ||
		input.epoch == 0u || !std::isfinite(input.cellSize) || input.cellSize <= 0.0f)
		return result;
	const float brickSize = input.cellSize * 8.0f;
	for (const NRISmokeContinuousSourceWorkRequest& request : *input.requests)
	{
		result.requests++;
		if (!request.requiresEstablishedAuthority || request.epoch != input.epoch ||
			request.sourceId == 0u || request.aggregateCadenceSteps == 0u ||
			request.firstCadenceOrdinal == 0u ||
			request.lastCadenceOrdinal < request.firstCadenceOrdinal ||
			request.lastCadenceOrdinal - request.firstCadenceOrdinal + 1u !=
				(uint64_t)request.aggregateCadenceSteps ||
			request.particlesPerCadence == 0u || request.styleIndex >= input.styles->size() ||
			!Finite3(request.position) || !Finite3(request.velocity) ||
			!std::isfinite(request.spawnRadius) || !std::isfinite(request.densityScale) ||
			!std::isfinite(request.radiusScale) ||
			!std::isfinite(request.pulseEnvelope.amount) ||
			!std::isfinite(request.pulseEnvelope.phase) ||
			request.pulseEnvelope.periodCadences == 0u)
		{
			result.invalidRequests++;
			continue;
		}
		const NRISmokeStyleGpu& style = (*input.styles)[request.styleIndex];
		const float radius = std::max({ request.spawnRadius,
			style.radius * std::max(request.radiusScale, 0.0f), input.cellSize });
		if (!std::isfinite(radius))
		{
			result.invalidRequests++;
			continue;
		}
		const NRISmokeSpatialCoordinate center = {
			BrickCoordinate(request.position[0], brickSize),
			BrickCoordinate(request.position[1], brickSize),
			BrickCoordinate(request.position[2], brickSize) };
		auto centerAuthority = input.authorities->find(center);
		if (centerAuthority == input.authorities->end() ||
			centerAuthority->second.authority != NRISmokeSpatialAuthority::Coarse)
		{
			result.missingAuthorities++;
			continue;
		}
		if (input.promotions != nullptr && input.promotions->find(center) != input.promotions->end())
		{
			result.promotionConflicts++;
			continue;
		}

		std::vector<const NRISmokeSpatialBrickObservation*> targets;
		const NRISmokeSpatialCoordinate minimum = {
			BrickCoordinate(request.position[0] - radius, brickSize),
			BrickCoordinate(request.position[1] - radius, brickSize),
			BrickCoordinate(request.position[2] - radius, brickSize) };
		const NRISmokeSpatialCoordinate maximum = {
			BrickCoordinate(request.position[0] + radius, brickSize),
			BrickCoordinate(request.position[1] + radius, brickSize),
			BrickCoordinate(request.position[2] + radius, brickSize) };
		for (const auto& authority : *input.authorities)
		{
			const NRISmokeSpatialCoordinate& coordinate = authority.first;
			if (authority.second.authority != NRISmokeSpatialAuthority::Coarse ||
				coordinate.x < minimum.x || coordinate.x > maximum.x ||
				coordinate.y < minimum.y || coordinate.y > maximum.y ||
				coordinate.z < minimum.z || coordinate.z > maximum.z ||
				(input.promotions != nullptr && input.promotions->find(coordinate) != input.promotions->end()))
				continue;
			targets.push_back(&authority.second);
		}
		if (targets.empty())
		{
			result.missingAuthorities++;
			continue;
		}
		if (targets.size() > (size_t)input.maximumInjections -
			std::min((size_t)input.maximumInjections, result.injections.size()))
		{
			result.capacityRejected++;
			continue;
		}

		const double weightedCadences = NRISumSmokeSourceEnvelope(request.pulseEnvelope,
			request.firstCadenceOrdinal, request.lastCadenceOrdinal);
		const float totalMass = std::max(style.density * request.densityScale, 0.0f) *
			(float)request.particlesPerCadence * (float)weightedCadences;
		const float targetMass = totalMass / (float)targets.size();
		const float extinction = targetMass * std::max(style.extinction, 0.0f);
		const float scattering[3] = {
			extinction * std::clamp(style.albedo[0], 0.0f, 1.0f),
			extinction * std::clamp(style.albedo[1], 0.0f, 1.0f),
			extinction * std::clamp(style.albedo[2], 0.0f, 1.0f) };
		const float anisotropyWeight = scattering[0] * 0.2126f +
			scattering[1] * 0.7152f + scattering[2] * 0.0722f;
		const float densityRate = kLn2 / std::max(style.densityHalfLife, 0.001f);
		const float coolingRate = kLn2 / std::max(style.coolingHalfLife, 0.001f);
		const float inheritedScale = style.velocityInherit * style.momentumScale;
		const float velocity[3] = { request.velocity[0] * inheritedScale,
			request.velocity[1] * inheritedScale + style.riseVelocity,
			request.velocity[2] * inheritedScale };
		for (const NRISmokeSpatialBrickObservation* target : targets)
		{
			NRISmokeDormantGridInjectionGpu injection = {};
			injection.coordinate[0] = target->coordinate.x;
			injection.coordinate[1] = target->coordinate.y;
			injection.coordinate[2] = target->coordinate.z;
			injection.generation = target->generation;
			injection.epoch = input.epoch;
			injection.cadenceSteps = request.aggregateCadenceSteps;
			injection.sourceId = request.sourceId;
			injection.flags = NRISmokeDormantGridInjectionFlag_EstablishedAuthority;
			std::copy(request.position, request.position + 3, injection.position);
			injection.radius = radius;
			injection.scalar[0] = targetMass;
			injection.scalar[1] = targetMass * std::max(style.temperature, 0.0f) *
				std::max(style.buoyancy, 0.0f);
			injection.scalar[2] = extinction;
			injection.scalar[3] = anisotropyWeight *
				std::clamp(style.anisotropy, -0.95f, 0.95f);
			injection.momentum[0] = velocity[0] * targetMass;
			injection.momentum[1] = velocity[1] * targetMass;
			injection.momentum[2] = velocity[2] * targetMass;
			injection.momentum[3] = targetMass * std::max(std::abs(style.turbulenceScale), 0.0001f);
			injection.optical[0] = scattering[0];
			injection.optical[1] = scattering[1];
			injection.optical[2] = scattering[2];
			injection.optical[3] = anisotropyWeight;
			injection.dynamics[0] = targetMass * densityRate;
			injection.dynamics[1] = targetMass * coolingRate;
			injection.dynamics[2] = targetMass * std::max(style.turbulence, 0.0f);
			injection.dynamics[3] = targetMass * std::max(style.drag, 0.0f);
			result.injections.push_back(injection);
		}
		result.routedSourceIds.insert(request.sourceId);
		result.routedSources++;
		result.cadenceSteps += request.aggregateCadenceSteps;
	}
	return result;
}
