#include "nri_smoke_continuous_sources.h"

#include <cstdlib>
#include <iostream>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

NRISmokeContinuousSourceObservation Source(uint64_t key, uint32_t steps,
	uint64_t ordinal, bool established = true)
{
	NRISmokeContinuousSourceObservation result = {};
	result.stableKey = key;
	result.sourceId = static_cast<uint32_t>(key);
	result.ruleIndex = 4u;
	result.actorIndex = static_cast<int32_t>(key);
	result.styleIndex = 2u;
	result.particlesPerCadence = 9u;
	result.cadenceSteps = steps;
	result.cadenceOrdinal = ordinal;
	result.established = established;
	result.densityScale = 3.0f;
	result.pulseEnvelope = { 0.75f, 8u, 0.25f };
	return result;
}
}

int main()
{
	Require(NRIClassifySmokeActorSource({ "duke_fire_sustained", true, true, 0.0f, 0.0f }) ==
		NRISmokeActorSourceLifetime::PersistentContinuous,
		"the explicitly supported stationary fire interval must be persistent");
	Require(NRIClassifySmokeActorSource({ "DUKE_FIRE_SUSTAINED", true, true, 0.0f, 0.0f }) ==
		NRISmokeActorSourceLifetime::PersistentContinuous,
		"rule identity classification must follow LIGHTOVR case-insensitivity");
	Require(NRIClassifySmokeActorSource({ "duke_explosion_cloud", false, true, 0.0f, 0.0f }) ==
		NRISmokeActorSourceLifetime::Transitory,
		"spawn explosions must never become persistent continuous sources");
	Require(NRIClassifySmokeActorSource({ "duke_rpg_trail_continuous", true, true, 4.0f, 0.5f }) ==
		NRISmokeActorSourceLifetime::Transitory,
		"moving spatial RPG trails must never be retained or replayed");
	Require(NRIClassifySmokeActorSource({ "future_interval", true, true, 0.0f, 0.0f }) ==
		NRISmokeActorSourceLifetime::Transitory,
		"new interval rules must opt in explicitly instead of inheriting persistence");

	NRISmokeContinuousSourceOwner owner;
	owner.BeginFrame(1u);
	owner.Observe(Source(10u, 1u, 1u, false));
	const auto& unestablished = owner.EndFrame();
	Require(unestablished.work.empty() && unestablished.established == 0u,
		"cadence must not create delayed smoke before source establishment");

	owner.BeginFrame(1u);
	owner.Observe(Source(10u, 1u, 2u));
	const auto& established = owner.EndFrame();
	Require(established.work.size() == 1u && established.work[0].aggregateCadenceSteps == 1u &&
		established.work[0].requiresEstablishedAuthority &&
		established.work[0].pulseEnvelope.amount == 0.75f &&
		established.work[0].pulseEnvelope.periodCadences == 8u &&
		established.work[0].pulseEnvelope.phase == 0.25f,
		"established cadence must become one authority-gated aggregate request");

	owner.Reset();
	for (uint32_t frame = 0u; frame < 3u; ++frame)
	{
		owner.BeginFrame(1u);
		owner.Observe(Source(10u, 1u, frame + 1u));
		owner.Observe(Source(20u, 1u, frame + 1u));
		owner.Observe(Source(30u, 1u, frame + 1u));
		const auto& snapshot = owner.EndFrame();
		Require(snapshot.work.size() == 1u && snapshot.work[0].stableKey == 10u * (frame + 1u),
			"fixed-quantity round robin must advance deterministically across stable identities");
	}

	owner.Reset();
	owner.BeginFrame(0u);
	owner.Observe(Source(1u, 3u, 3u));
	Require(owner.EndFrame().deferred == 1u, "a zero work quantity must retain bounded cadence debt");
	owner.BeginFrame(1u);
	owner.Observe(Source(1u, 2u, 5u));
	const auto& coalesced = owner.EndFrame();
	Require(coalesced.work.size() == 1u && coalesced.work[0].aggregateCadenceSteps == 5u &&
		coalesced.work[0].firstCadenceOrdinal == 1u && coalesced.work[0].lastCadenceOrdinal == 5u,
		"deferred cadence must coalesce into one analytic request instead of replaying a burst");

	owner.Reset();
	owner.BeginFrame(0u);
	owner.Observe(Source(7u, NRISmokeContinuousSourceOwner::MaximumCadenceDebt + 6u,
		NRISmokeContinuousSourceOwner::MaximumCadenceDebt + 6u));
	const auto& capped = owner.EndFrame();
	Require(capped.cadenceStepsDiscarded == 6u,
		"continuous-source debt must stay strictly bounded under a long stall");
	owner.BeginFrame(1u);
	owner.Observe(Source(7u, 0u, NRISmokeContinuousSourceOwner::MaximumCadenceDebt + 6u));
	const auto& cappedWork = owner.EndFrame();
	Require(cappedWork.work.size() == 1u &&
		cappedWork.work[0].aggregateCadenceSteps == NRISmokeContinuousSourceOwner::MaximumCadenceDebt &&
		cappedWork.work[0].firstCadenceOrdinal == 7u,
		"overflow must keep the newest bounded logical cadence range");

	owner.Reset();
	owner.BeginFrame(0u);
	owner.Observe(Source(99u, 4u, 4u));
	owner.EndFrame();
	owner.BeginFrame(1u);
	const auto& expired = owner.EndFrame();
	Require(expired.expired == 1u && expired.cadenceStepsDiscarded == 4u && expired.work.empty(),
		"an expired actor must discard debt so smoke cannot appear after the source disappears");

	std::cout << "Smoke continuous-source policy tests passed.\n";
	return 0;
}
