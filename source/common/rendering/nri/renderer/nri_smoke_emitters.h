#pragma once

#include "nri_smoke_contracts.h"
#include "nri_smoke_analytic_carriers.h"
#include "nri_smoke_analytic_trail_bridge.h"
#include "nri_smoke_transient_clouds.h"
#include "nri_smoke_continuous_sources.h"
#include "nri_smoke_interest.h"
#include "nri_smoke_pulses.h"
#include "v_video.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

class SceneLightSystem;

struct NRISmokeEmitterRouteAttribution
{
	uint32_t sourceId = 0u;
	uint32_t authoredRepresentation = 0u;
	uint32_t effectiveRepresentation = 0u;
	uint32_t transientClass = 0u;
	uint32_t sourceQuantity = 0u;
	uint32_t gridCommands = 0u;
	uint32_t analyticCarriers = 0u;
	uint32_t transientGroups = 0u;
	uint32_t transientLobes = 0u;
};

struct NRISmokeEmitterRouteSnapshot
{
	uint64_t gatherId = 0u;
	uint32_t classMask = 0x3fu;
	uint32_t sourceClassMask = 0x3fu;
	bool actorEmittersEnabled = true;
	uint32_t suppressedActorRules = 0u;
	uint32_t suppressedEventRules = 0u;
	bool mapEmittersEnabled = true;
	uint32_t suppressedMapRules = 0u;
	uint32_t suppressedMapPreviews = 0u;
	uint32_t ambientMapCommands = 0u;
	uint32_t previewMapCommands = 0u;
	uint32_t gridCommands = 0u;
	uint32_t analyticCarriers = 0u;
	uint32_t transientGroups = 0u;
	uint32_t transientLobes = 0u;
	uint32_t fallbackGridCommands = 0u;
	uint32_t fallbackAnalyticCarriers = 0u;
	uint32_t trailBridgeObservations = 0u;
	std::vector<NRISmokeEmitterRouteAttribution> sources;
};

class NRISmokeEmitterSystem
{
public:
	void Gather(uint32_t epoch, double gameplayTimeSeconds, const TArray<PathTracingWeaponLightEvent>& weaponEvents,
		const SceneLightSystem& sceneLights,
		std::vector<NRISmokeStyleGpu>& styles, std::vector<NRISmokeInjectionCommandGpu>& commands,
		std::vector<NRISmokePulseEnqueueInfo>& commandEnqueueInfo,
		std::vector<NRISmokeAnalyticTrailObservationBatch>& trailObservations,
		std::vector<NRISmokeAnalyticCarrierRequest>& analyticRequests,
		std::vector<NRISmokeTransientLobeRequest>& transientRequests,
		uint32_t& nextSerial, uint32_t traceMode, const NRISmokeInterestSnapshot& interest,
		float gridCellSize, uint32_t gridBrickCapacity);
	void Reset();
	void SetTransientClassMask(uint32_t mask) { mTransientClassMask = mask & 0x3fu; }
	void SetSourceClassMask(uint32_t mask);
	void SetActorEmittersEnabled(bool enabled);
	void SetMapEmittersEnabled(bool enabled);
	uint32_t GetGeneration() const { return mGeneration; }
	void SetContinuousSourceWorkQuantity(uint32_t quantity) { mContinuousSourceWorkQuantity = quantity; }
	const NRISmokeContinuousSourceSnapshot& GetContinuousSourceSnapshot() const { return mContinuousSources.GetSnapshot(); }
	const NRISmokeEmitterRouteSnapshot& GetRouteSnapshot() const { return mRouteSnapshot; }

private:
	struct Identity
	{
		uint32_t rule = 0;
		int32_t actorIndex = -1;
		const void* actor = nullptr;
		bool operator==(const Identity& other) const { return rule == other.rule && actorIndex == other.actorIndex && actor == other.actor; }
	};
	struct IdentityHash
	{
		size_t operator()(const Identity& value) const;
	};
	struct ActorState
	{
		DVector3 previousPosition;
		double previousTimeSeconds = 0.0;
		double activationTimeSeconds = 0.0;
		float spacingRemainder = 0.0f;
		double intervalRemainder = 0.0;
		double startDistanceTraveled = 0.0;
		uint64_t continuousStableKey = 0;
		uint64_t continuousCadenceOrdinal = 0;
		uint64_t trailUpdateOrdinal = 0;
		bool activationLatched = false;
		bool appearanceObserved = false;
		bool sourceTracePublished = false;
		bool authorityTracePublished = false;
		bool authorityTraceAppearanceReady = false;
		bool authorityTraceActivationLatched = false;
		bool authorityTraceCadenceActive = false;
		bool startTimeElapsed = false;
		bool emitted = false;
		bool observed = false;
	};
	struct MapEmitterState
	{
		double previousTimeSeconds = 0.0;
		double logicalElapsedSeconds = 0.0;
		double intervalRemainder = 0.0;
		uint64_t nextCadenceOrdinal = 0;
		uint32_t coalescedDebt = 0;
		NRISmokeInterestTier previousTier = NRISmokeInterestTier::Dormant;
		bool initialized = false;
		bool emitted = false;
	};

	uint32_t mGeneration = 0;
	FString mActiveMapName;
	std::unordered_map<Identity, ActorState, IdentityHash> mActorStates;
	std::unordered_map<uint32_t, MapEmitterState> mMapEmitterStates;
	NRISmokeContinuousSourceOwner mContinuousSources;
	uint64_t mNextContinuousSourceGeneration = 0;
	uint32_t mContinuousSourceWorkQuantity = 8u;
	uint32_t mTransientClassMask = 0x3fu;
	uint32_t mSourceClassMask = 0x3fu;
	bool mActorEmittersEnabled = true;
	bool mMapEmittersEnabled = true;
	uint64_t mNextRouteGatherId = 0u;
	NRISmokeEmitterRouteSnapshot mRouteSnapshot = {};
	MapEmitterState mEditorPreviewState;
	FString mEditorPreviewMapName;
	FString mEditorPreviewRuleId;
};
