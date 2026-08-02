#include "nri_smoke_source_envelope.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

bool Near(double left, double right, double tolerance = 1e-5)
{
	return std::abs(left - right) <= tolerance;
}
}

int main()
{
	const NRISmokeSourceEnvelope disabled = {};
	Require(NRIEvaluateSmokeSourceEnvelope(disabled, 1u) == 1.0f &&
		NRIEvaluateSmokeSourceEnvelope(disabled, UINT64_MAX) == 1.0f,
		"the default envelope must be an exact identity");
	Require(NRIEvaluateSmokeSourceEnvelope({ 1.0f, 1u, 0.4f }, 17u) == 1.0f,
		"a one-cadence period must remain an identity instead of changing average mass");

	const NRISmokeSourceEnvelope strong = { 0.75f, 8u, 0.0f };
	Require(Near(NRIEvaluateSmokeSourceEnvelope(strong, 1u), 0.25) &&
		Near(NRIEvaluateSmokeSourceEnvelope(strong, 5u), 1.75),
		"phase zero must begin at the trough and reach the peak halfway through the cycle");
	Require(Near(NRISumSmokeSourceEnvelope(strong, 1u, 8u), 8.0),
		"one complete envelope period must preserve mean mass");
	Require(Near(NRISumSmokeSourceEnvelope(strong, 25u, 40u), 16.0),
		"multiple complete periods must preserve mean mass at later ordinals");

	const NRISmokeSourceEnvelope wrapped = { 0.5f, 4u, 1.25f };
	const NRISmokeSourceEnvelope canonical = { 0.5f, 4u, 0.25f };
	Require(Near(NRIEvaluateSmokeSourceEnvelope(wrapped, UINT64_MAX - 2u),
		NRIEvaluateSmokeSourceEnvelope(canonical, UINT64_MAX - 2u)),
		"phase wrapping and large ordinals must remain deterministic");
	Require(NRISumSmokeSourceEnvelope(strong, 9u, 8u) == 0.0,
		"an empty ordinal range must contribute no mass");

	std::cout << "Smoke source envelope tests passed.\n";
	return 0;
}
