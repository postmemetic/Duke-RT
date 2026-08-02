#pragma once

#include "nri_smoke_source_envelope.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

enum class NRISmokeActorSourceLifetime : uint8_t
{
	Transitory = 0,
	PersistentContinuous,
};

struct NRISmokeActorSourceTraits
{
	const char* ruleId = nullptr;
	bool intervalTrigger = false;
	bool gridRepresentation = false;
	float spacing = 0.0f;
	float velocityScale = 0.0f;
};

// Persistent classification is deliberately opt-in. An interval trigger alone is
// not sufficient: projectile trails are interval sources too, but must never be
// retained or replayed after the projectile has left.
inline NRISmokeActorSourceLifetime NRIClassifySmokeActorSource(const NRISmokeActorSourceTraits& traits)
{
	auto sameAsciiNoCase = [](const char* left, const char* right)
	{
		if (left == nullptr || right == nullptr) return false;
		while (*left != '\0' && *right != '\0')
		{
			const char a = (*left >= 'A' && *left <= 'Z') ? char(*left - 'A' + 'a') : *left;
			const char b = (*right >= 'A' && *right <= 'Z') ? char(*right - 'A' + 'a') : *right;
			if (a != b) return false;
			++left;
			++right;
		}
		return *left == *right;
	};

	const bool stationaryTemporalGrid = traits.intervalTrigger && traits.gridRepresentation &&
		traits.spacing <= 0.0f && std::abs(traits.velocityScale) <= 0.0001f;
	return stationaryTemporalGrid && sameAsciiNoCase(traits.ruleId, "duke_fire_sustained") ?
		NRISmokeActorSourceLifetime::PersistentContinuous : NRISmokeActorSourceLifetime::Transitory;
}

struct NRISmokeContinuousSourceObservation
{
	uint64_t stableKey = 0;
	uint64_t cadenceOrdinal = 0;
	double gameplayTimeSeconds = 0.0;
	uint32_t epoch = 0;
	uint32_t sourceId = 0;
	uint32_t ruleIndex = 0;
	int32_t actorIndex = -1;
	uint32_t styleIndex = 0;
	uint32_t particlesPerCadence = 0;
	uint32_t cadenceSteps = 0;
	float position[3] = {};
	float velocity[3] = {};
	float spawnRadius = 0.0f;
	float densityScale = 0.0f;
	float radiusScale = 0.0f;
	NRISmokeSourceEnvelope pulseEnvelope;
	bool established = false;
};

struct NRISmokeContinuousSourceWorkRequest
{
	uint64_t stableKey = 0;
	uint64_t firstCadenceOrdinal = 0;
	uint64_t lastCadenceOrdinal = 0;
	double gameplayTimeSeconds = 0.0;
	uint32_t epoch = 0;
	uint32_t sourceId = 0;
	uint32_t ruleIndex = 0;
	int32_t actorIndex = -1;
	uint32_t styleIndex = 0;
	uint32_t particlesPerCadence = 0;
	uint32_t aggregateCadenceSteps = 0;
	float position[3] = {};
	float velocity[3] = {};
	float spawnRadius = 0.0f;
	float densityScale = 0.0f;
	float radiusScale = 0.0f;
	NRISmokeSourceEnvelope pulseEnvelope;
	// Archive integration must only apply this aggregate to an already-owned
	// fine or coarse authority. It must not make deferred smoke appear later.
	bool requiresEstablishedAuthority = true;
};

struct NRISmokeContinuousSourceSnapshot
{
	uint32_t observed = 0;
	uint32_t established = 0;
	uint32_t pending = 0;
	uint32_t selected = 0;
	uint32_t deferred = 0;
	uint32_t expired = 0;
	uint32_t cadenceStepsAccepted = 0;
	uint32_t cadenceStepsCoalesced = 0;
	uint32_t cadenceStepsDiscarded = 0;
	std::vector<NRISmokeContinuousSourceWorkRequest> work;
};

class NRISmokeContinuousSourceOwner
{
public:
	static constexpr uint32_t MaximumCadenceDebt = 64u;

	void BeginFrame(uint32_t workQuantity)
	{
		mWorkQuantity = workQuantity;
		mSnapshot = {};
		for (auto& entry : mStates) entry.second.observed = false;
	}

	void Observe(const NRISmokeContinuousSourceObservation& observation)
	{
		if (observation.stableKey == 0u) return;
		State& state = mStates[observation.stableKey];
		state.observed = true;
		state.established = state.established || observation.established;
		state.latest = observation;
		mSnapshot.observed++;
		if (!state.established || observation.cadenceSteps == 0u) return;

		const uint32_t previousDebt = state.cadenceDebt;
		const uint64_t requestedDebt = uint64_t(previousDebt) + observation.cadenceSteps;
		state.cadenceDebt = static_cast<uint32_t>(std::min<uint64_t>(requestedDebt, MaximumCadenceDebt));
		const uint32_t accepted = state.cadenceDebt - previousDebt;
		const uint32_t discarded = observation.cadenceSteps - accepted;
		if (previousDebt == 0u)
			state.firstCadenceOrdinal = observation.cadenceOrdinal - observation.cadenceSteps + 1u;
		if (discarded != 0u)
			state.firstCadenceOrdinal = observation.cadenceOrdinal - state.cadenceDebt + 1u;
		state.lastCadenceOrdinal = observation.cadenceOrdinal;
		mSnapshot.cadenceStepsAccepted += accepted;
		mSnapshot.cadenceStepsDiscarded += discarded;
	}

	const NRISmokeContinuousSourceSnapshot& EndFrame()
	{
		for (auto it = mStates.begin(); it != mStates.end(); )
		{
			if (!it->second.observed)
			{
				mSnapshot.expired++;
				mSnapshot.cadenceStepsDiscarded += it->second.cadenceDebt;
				it = mStates.erase(it);
			}
			else
			{
				mSnapshot.established += it->second.established ? 1u : 0u;
				mSnapshot.pending += it->second.cadenceDebt != 0u ? 1u : 0u;
				++it;
			}
		}

		if (mWorkQuantity == 0u || mSnapshot.pending == 0u)
		{
			mSnapshot.deferred = mSnapshot.pending;
			return mSnapshot;
		}

		auto cursor = mStates.upper_bound(mLastSelectedStableKey);
		size_t visited = 0u;
		while (visited < mStates.size() && mSnapshot.selected < mWorkQuantity)
		{
			if (cursor == mStates.end()) cursor = mStates.begin();
			auto current = cursor++;
			visited++;
			State& state = current->second;
			if (!state.established || state.cadenceDebt == 0u) continue;

			NRISmokeContinuousSourceWorkRequest request = {};
			request.stableKey = current->first;
			request.firstCadenceOrdinal = state.firstCadenceOrdinal;
			request.lastCadenceOrdinal = state.lastCadenceOrdinal;
			request.gameplayTimeSeconds = state.latest.gameplayTimeSeconds;
			request.epoch = state.latest.epoch;
			request.sourceId = state.latest.sourceId;
			request.ruleIndex = state.latest.ruleIndex;
			request.actorIndex = state.latest.actorIndex;
			request.styleIndex = state.latest.styleIndex;
			request.particlesPerCadence = state.latest.particlesPerCadence;
			request.aggregateCadenceSteps = state.cadenceDebt;
			std::copy(state.latest.position, state.latest.position + 3, request.position);
			std::copy(state.latest.velocity, state.latest.velocity + 3, request.velocity);
			request.spawnRadius = state.latest.spawnRadius;
			request.densityScale = state.latest.densityScale;
			request.radiusScale = state.latest.radiusScale;
			request.pulseEnvelope = state.latest.pulseEnvelope;
			mSnapshot.work.push_back(request);
			mSnapshot.cadenceStepsCoalesced += state.cadenceDebt;
			state.cadenceDebt = 0u;
			state.firstCadenceOrdinal = 0u;
			mLastSelectedStableKey = current->first;
			mSnapshot.selected++;
		}
		mSnapshot.deferred = mSnapshot.pending - mSnapshot.selected;
		return mSnapshot;
	}

	void Reset()
	{
		mStates.clear();
		mSnapshot = {};
		mLastSelectedStableKey = 0u;
		mWorkQuantity = 0u;
	}

	const NRISmokeContinuousSourceSnapshot& GetSnapshot() const { return mSnapshot; }

private:
	struct State
	{
		NRISmokeContinuousSourceObservation latest;
		uint64_t firstCadenceOrdinal = 0;
		uint64_t lastCadenceOrdinal = 0;
		uint32_t cadenceDebt = 0;
		bool observed = false;
		bool established = false;
	};

	std::map<uint64_t, State> mStates;
	NRISmokeContinuousSourceSnapshot mSnapshot;
	uint64_t mLastSelectedStableKey = 0;
	uint32_t mWorkQuantity = 0;
};
