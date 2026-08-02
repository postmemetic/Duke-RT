#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct NRISmokeSourceEnvelope
{
	float amount = 0.0f;
	uint32_t periodCadences = 1u;
	float phase = 0.0f;
};

inline float NRIEvaluateSmokeSourceEnvelope(const NRISmokeSourceEnvelope& envelope,
	uint64_t cadenceOrdinal)
{
	if (!(envelope.amount > 0.0f) || envelope.periodCadences <= 1u)
		return 1.0f;

	const double amount = std::clamp(std::isfinite(envelope.amount) ?
		(double)envelope.amount : 0.0, 0.0, 1.0);
	if (!(amount > 0.0)) return 1.0f;
	const double phase = std::isfinite(envelope.phase) ?
		(double)envelope.phase - std::floor((double)envelope.phase) : 0.0;
	const uint64_t zeroBasedOrdinal = cadenceOrdinal > 0u ? cadenceOrdinal - 1u : 0u;
	const double cycle = (double)(zeroBasedOrdinal % envelope.periodCadences) /
		(double)envelope.periodCadences + phase;
	constexpr double TwoPi = 6.28318530717958647692;
	return (float)std::max(1.0 - amount * std::cos(TwoPi * cycle), 0.0);
}

inline double NRISumSmokeSourceEnvelope(const NRISmokeSourceEnvelope& envelope,
	uint64_t firstCadenceOrdinal, uint64_t lastCadenceOrdinal)
{
	if (firstCadenceOrdinal == 0u || firstCadenceOrdinal > lastCadenceOrdinal)
		return 0.0;
	if (!(envelope.amount > 0.0f) || envelope.periodCadences <= 1u)
		return (double)(lastCadenceOrdinal - firstCadenceOrdinal + 1u);

	double sum = 0.0;
	for (uint64_t ordinal = firstCadenceOrdinal; ; ++ordinal)
	{
		sum += (double)NRIEvaluateSmokeSourceEnvelope(envelope, ordinal);
		if (ordinal == lastCadenceOrdinal) break;
	}
	return sum;
}
