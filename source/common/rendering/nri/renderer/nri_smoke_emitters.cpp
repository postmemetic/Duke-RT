#include "nri_smoke_emitters.h"

#include "nri_smoke_admission.h"
#include "nri_scene_lights.h"
#include "nri_smoke_source_envelope.h"

#include "../scene/nri_hash.h"

#include "coreactor.h"
#include "lightoverlay.h"
#include "lightoverlay_smoke_editor.h"
#include "printf.h"

#include <algorithm>
#include <cmath>

namespace
{
	void WorldToPathTracingPosition(const DVector3& worldPosition, float outPosition[3])
	{
		outPosition[0] = (float)worldPosition.X;
		outPosition[1] = (float)-worldPosition.Z;
		outPosition[2] = (float)-worldPosition.Y;
	}

	void WorldToPathTracingDirection(const DVector3& worldDirection, float outDirection[3])
	{
		outDirection[0] = (float)worldDirection.X;
		outDirection[1] = (float)-worldDirection.Z;
		outDirection[2] = (float)-worldDirection.Y;
	}

	void SetPointSourceShape(NRISmokeInjectionCommandGpu& command)
	{
		command.shape = static_cast<uint32_t>(NRISmokeInjectionShape::Sphere);
		std::fill(command.halfAxisU, command.halfAxisU + 3, 0.0f);
		std::fill(command.halfAxisV, command.halfAxisV + 3, 0.0f);
	}

	bool ActorMatchesClass(const DCoreActor* actor, const PClassActor* actorClass)
	{
		return actor != nullptr && actorClass != nullptr && actor->GetClass() != nullptr &&
			(actor->GetClass() == actorClass || actor->GetClass()->IsDescendantOf(actorClass));
	}

	DVector3 ActorForward(const DCoreActor* actor)
	{
		const DVector2 forward = actor->spr.Angles.Yaw.ToVector();
		return DVector3(forward.X, forward.Y, 0.0);
	}

	DVector3 ActorVelocity(const DCoreActor* actor)
	{
		const DVector2 forward = actor->spr.Angles.Yaw.ToVector();
		return DVector3(forward.X * actor->vel.X, forward.Y * actor->vel.X, actor->vel.Z);
	}

	int32_t SmokeGridFloorDiv8(int32_t value)
	{
		const int32_t quotient = value / 8;
		const int32_t remainder = value - quotient * 8;
		return quotient - (remainder < 0 ? 1 : 0);
	}

	uint32_t ConservativeMapEmitterBrickFootprint(const NRISmokeInjectionCommandGpu& command,
		const NRISmokeStyleGpu& style, float gridCellSize)
	{
		const float cellSize = std::max(gridCellSize, 0.0001f);
		const float radius = std::min(std::max(std::max(command.spawnRadius,
			style.radius * command.radiusScale), cellSize), cellSize * 16.0f);
		float extent[3];
		for (uint32_t axis = 0; axis < 3u; ++axis)
			extent[axis] = std::abs(command.halfAxisU[axis]) + std::abs(command.halfAxisV[axis]) + radius;
		uint64_t bricks = 1u;
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			const int32_t minimumCell = (int32_t)std::floor((command.position[axis] - extent[axis]) / cellSize);
			const int32_t maximumCell = (int32_t)std::floor((command.position[axis] + extent[axis]) / cellSize);
			const int32_t minimumBrick = SmokeGridFloorDiv8(minimumCell);
			const int32_t maximumBrick = SmokeGridFloorDiv8(maximumCell);
			bricks *= (uint64_t)std::max(maximumBrick - minimumBrick + 1, 1);
		}
		return (uint32_t)std::min<uint64_t>(bricks, UINT32_MAX);
	}

	uint32_t HashAnalyticCarrier(uint64_t eventSerial, uint32_t carrierIndex)
	{
		uint32_t value = (uint32_t)eventSerial ^ (uint32_t)(eventSerial >> 32u) ^
			(carrierIndex + 1u) * 0x9e3779b9u;
		value ^= value >> 16u;
		value *= 0x7feb352du;
		value ^= value >> 15u;
		value *= 0x846ca68bu;
		return value ^ (value >> 16u);
	}

	uint64_t BuildSmokeOffsetRandomSeed(const PathTracingWeaponLightEvent& event)
	{
		uint64_t hash = nri_scene::NRIHashFnv1a64OffsetBasis;
		constexpr char domain[] = "smoke-event-offset";
		nri_scene::Fnv1a64Append(hash, domain, sizeof(domain) - 1u);
		hash = nri_scene::HashCombine64(hash, event.serial);
		hash = nri_scene::HashCombine64(hash,
			(uint64_t)(uint32_t)(event.hasEmitterActorIndex ?
				event.emitterActorIndex + 1 : 0));
		for (const unsigned char* cursor =
			(const unsigned char*)event.eventId.GetChars(); cursor != nullptr && *cursor != 0u;
			++cursor)
		{
			const uint8_t lower = *cursor >= 'A' && *cursor <= 'Z' ?
				(uint8_t)(*cursor - 'A' + 'a') : *cursor;
			nri_scene::Fnv1a64Append(hash, &lower, sizeof(lower));
		}
		return hash;
	}

	float NextSmokeOffsetSignedRandom(uint64_t& state)
	{
		state += 0x9e3779b97f4a7c15ull;
		uint64_t value = state;
		value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
		value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
		value ^= value >> 31u;
		const float unit = (float)((value >> 40u) & 0xffffffu) *
			(1.0f / 16777215.0f);
		return unit * 2.0f - 1.0f;
	}

	void ShapeAnalyticCarrier(const NRISmokeInjectionCommandGpu& command, uint64_t eventSerial,
		uint32_t carrierIndex, uint32_t carrierCount, float initialRadius,
		float position[3], float velocity[3])
	{
		std::copy(command.position, command.position + 3, position);
		std::copy(command.velocity, command.velocity + 3, velocity);
		if (carrierCount <= 1u) return;

		const float speed = std::sqrt(velocity[0] * velocity[0] +
			velocity[1] * velocity[1] + velocity[2] * velocity[2]);
		if (speed <= 0.0001f) return;
		float forward[3] = { velocity[0] / speed, velocity[1] / speed, velocity[2] / speed };
		float tangent[3] = {};
		if (std::abs(forward[1]) < 0.9f)
		{
			tangent[0] = -forward[2];
			tangent[2] = forward[0];
		}
		else
		{
			tangent[0] = forward[1];
			tangent[1] = -forward[0];
		}
		const float tangentLength = std::sqrt(tangent[0] * tangent[0] +
			tangent[1] * tangent[1] + tangent[2] * tangent[2]);
		for (float& axis : tangent) axis /= std::max(tangentLength, 0.0001f);
		const float bitangent[3] = {
			forward[1] * tangent[2] - forward[2] * tangent[1],
			forward[2] * tangent[0] - forward[0] * tangent[2],
			forward[0] * tangent[1] - forward[1] * tangent[0]
		};
		const uint32_t hash = HashAnalyticCarrier(eventSerial, carrierIndex);
		const float phase = ((float)hash / 4294967296.0f) * 6.28318530718f;
		const float ringRadius = initialRadius * 0.45f * std::sqrt(
			((float)carrierIndex + 0.5f) / (float)carrierCount);
		const float radial[3] = {
			tangent[0] * std::cos(phase) + bitangent[0] * std::sin(phase),
			tangent[1] * std::cos(phase) + bitangent[1] * std::sin(phase),
			tangent[2] * std::cos(phase) + bitangent[2] * std::sin(phase)
		};
		for (uint32_t axis = 0u; axis < 3u; ++axis)
			position[axis] += radial[axis] * ringRadius;

		const float coneRadians = command.velocityCone * 0.01745329252f;
		const float carrierRadius = std::sqrt(((float)carrierIndex + 0.5f) /
			(float)carrierCount);
		const float angle = coneRadians * carrierRadius;
		for (uint32_t axis = 0u; axis < 3u; ++axis)
			velocity[axis] = speed * (forward[axis] * std::cos(angle) + radial[axis] * std::sin(angle));
	}
}

size_t NRISmokeEmitterSystem::IdentityHash::operator()(const Identity& value) const
{
	size_t hash = (size_t)value.rule * 16777619u;
	hash ^= (size_t)(uint32_t)value.actorIndex + 0x9e3779b9u + (hash << 6) + (hash >> 2);
	hash ^= (size_t)value.actor + 0x9e3779b9u + (hash << 6) + (hash >> 2);
	return hash;
}

void NRISmokeEmitterSystem::Reset()
{
	mActorStates.clear();
	mMapEmitterStates.clear();
	mContinuousSources.Reset();
	mNextContinuousSourceGeneration = 0u;
	mEditorPreviewState = {};
	mEditorPreviewMapName = "";
	mEditorPreviewRuleId = "";
	mActiveMapName = "";
	mGeneration = 0;
}

void NRISmokeEmitterSystem::Gather(uint32_t epoch, double gameplayTimeSeconds, const TArray<PathTracingWeaponLightEvent>& weaponEvents,
	const SceneLightSystem& sceneLights,
	std::vector<NRISmokeStyleGpu>& styles, std::vector<NRISmokeInjectionCommandGpu>& commands,
	std::vector<NRISmokePulseEnqueueInfo>& commandEnqueueInfo,
	std::vector<NRISmokeAnalyticTrailObservationBatch>& trailObservations,
	std::vector<NRISmokeAnalyticCarrierRequest>& analyticRequests,
	uint32_t& nextSerial, uint32_t traceMode, const NRISmokeInterestSnapshot& interest,
	float gridCellSize, uint32_t gridBrickCapacity)
{
	const ResolvedLightOverlaySet& resolved = GetResolvedLightOverlaySet();
	if (mGeneration != resolved.resolvedGeneration || mActiveMapName.CompareNoCase(resolved.activeMapName) != 0)
	{
		mGeneration = resolved.resolvedGeneration;
		mActiveMapName = resolved.activeMapName;
		mActorStates.clear();
		mMapEmitterStates.clear();
		mContinuousSources.Reset();
		mNextContinuousSourceGeneration = 0u;
		mEditorPreviewState = {};
		mEditorPreviewMapName = "";
		mEditorPreviewRuleId = "";
	}
	styles.clear();
	styles.resize(std::max<uint32_t>(1u, resolved.smokeStyles.Size()));
	for (const auto& source : resolved.smokeStyles)
	{
		if (source.styleIndex >= styles.size()) continue;
		NRISmokeStyleGpu& target = styles[source.styleIndex];
		std::copy(source.albedo, source.albedo + 3, target.albedo);
		target.extinction = source.extinction;
		target.anisotropy = source.anisotropy;
		target.radius = source.radius;
		target.expansionVelocity = source.expansionVelocity;
		target.lifetime = source.lifetime;
		target.density = source.density;
		target.densityHalfLife = source.densityHalfLife;
		target.riseVelocity = source.riseVelocity;
		target.velocityRandom = source.velocityRandom;
		target.velocityInherit = source.velocityInherit;
		target.buoyancy = source.buoyancy;
		target.drag = source.drag;
		target.turbulence = source.turbulence;
		target.turbulenceScale = source.turbulenceScale;
		target.temperature = source.temperature;
		target.momentumScale = source.momentumScale;
		target.coolingHalfLife = source.coolingHalfLife;
	}
	analyticRequests.clear();
	auto routeCommand = [&](const NRISmokeInjectionCommandGpu& command,
		LightOverlaySmokeRepresentation representation, LightOverlaySmokeQueuePolicy queuePolicy,
		bool hasMaximumLatency, float maximumLatencySeconds, double authoredGameplaySeconds,
		uint64_t sourceEventSerial, uint32_t analyticCarrierCount, bool transitory,
		uint64_t analyticBridgeSourceKey = 0u,
		uint64_t analyticBridgeSegmentRevision = 0u) -> bool
	{
		if (representation != LightOverlaySmokeRepresentation::Analytic)
		{
			commands.push_back(command);
			NRISmokePulseEnqueueInfo info = {};
			info.authoredGameplaySeconds = authoredGameplaySeconds;
			info.maximumLatencySeconds = hasMaximumLatency ?
				std::max(maximumLatencySeconds, 0.0f) : 0.0f;
			info.queuePolicy = queuePolicy == LightOverlaySmokeQueuePolicy::Latest ?
				NRISmokePulseQueuePolicy::Latest : NRISmokePulseQueuePolicy::Retry;
			info.transitory = transitory;
			info.analyticBridgeSourceKey = analyticBridgeSourceKey;
			info.analyticBridgeSegmentRevision = analyticBridgeSegmentRevision;
			commandEnqueueInfo.push_back(info);
			return true;
		}
		// Slice 8 intentionally has no analytic backlog. Latest/retry require a
		// different stable-source replacement contract and fail closed for now.
		if (queuePolicy != LightOverlaySmokeQueuePolicy::Drop || command.styleIndex >= styles.size())
			return false;
		const NRISmokeStyleGpu& style = styles[command.styleIndex];
		const float initialRadius = std::max(std::max(command.spawnRadius,
			style.radius * command.radiusScale), 0.001f);
		const float radiusCells = initialRadius / std::max(gridCellSize, 0.0001f);
		const float normalization = std::max(1.0f,
			(4.0f * 3.14159265359f / 15.0f) * radiusCells * radiusCells * radiusCells);
		const uint32_t carrierCount = std::min(std::max(analyticCarrierCount, 1u),
			std::min(command.count, 8u));
		for (uint32_t carrierIndex = 0u; carrierIndex < carrierCount; ++carrierIndex)
		{
			NRISmokeAnalyticCarrierRequest request = {};
			ShapeAnalyticCarrier(command, sourceEventSerial, carrierIndex, carrierCount,
				initialRadius, request.position, request.velocity);
			std::copy(command.halfAxisU, command.halfAxisU + 3, request.halfAxisU);
			std::copy(command.halfAxisV, command.halfAxisV + 3, request.halfAxisV);
			request.velocity[1] -= style.riseVelocity;
			request.initialRadius = initialRadius;
			request.initialDensity = std::max(command.densityScale, 0.0f) / normalization;
			request.shape = command.shape;
			request.rangeCount = command.count / carrierCount +
				(carrierIndex < command.count % carrierCount ? 1u : 0u);
			request.expansionVelocity = style.expansionVelocity;
			request.densityHalfLife = std::max(style.densityHalfLife, 0.001f);
			request.lifetimeSeconds = std::max(style.lifetime, 0.001f);
			request.styleIndex = command.styleIndex;
			request.sourceId = command.sourceId;
			request.epoch = command.epoch;
			request.authoredGameplaySeconds = authoredGameplaySeconds;
			request.maximumLatencySeconds = hasMaximumLatency ? std::max(maximumLatencySeconds, 0.0f) : 0.0f;
			request.sourceEventSerial = sourceEventSerial;
			request.batchIndex = carrierIndex;
			request.batchCount = carrierCount;
			analyticRequests.push_back(request);
		}
		return true;
	};

	mContinuousSources.BeginFrame(mContinuousSourceWorkQuantity);
	for (auto& entry : mActorStates) entry.second.observed = false;
	std::vector<uint32_t> observedPerRule;
	std::vector<uint32_t> emittedPerRule;
	std::vector<uint32_t> particlesPerRule;
	std::vector<uint32_t> deferredPerRule;
	std::vector<uint32_t> timeDeferredPerRule;
	std::vector<uint32_t> activatedPerRule;
	std::vector<uint32_t> appearanceReadyPerRule;
	std::vector<uint32_t> appearanceObservedPerRule;
	std::vector<uint32_t> activationLatchedPerRule;
	std::vector<uint32_t> cadenceActivePerRule;
	if (traceMode != 0)
	{
		observedPerRule.resize(resolved.smokeActorRules.Size());
		emittedPerRule.resize(resolved.smokeActorRules.Size());
		particlesPerRule.resize(resolved.smokeActorRules.Size());
		deferredPerRule.resize(resolved.smokeActorRules.Size());
		timeDeferredPerRule.resize(resolved.smokeActorRules.Size());
		activatedPerRule.resize(resolved.smokeActorRules.Size());
		appearanceReadyPerRule.resize(resolved.smokeActorRules.Size());
		appearanceObservedPerRule.resize(resolved.smokeActorRules.Size());
		activationLatchedPerRule.resize(resolved.smokeActorRules.Size());
		cadenceActivePerRule.resize(resolved.smokeActorRules.Size());
	}
	uint32_t verbosePrinted = 0;
	TSpriteIterator<DCoreActor> iterator;
	while (DCoreActor* actor = iterator.Next())
	{
		if (actor == nullptr || !actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0 || actor->GetClass() == nullptr) continue;
		for (uint32_t ruleIndex = 0; ruleIndex < resolved.smokeActorRules.Size(); ++ruleIndex)
		{
			const auto& rule = resolved.smokeActorRules[ruleIndex];
			if (!rule.actorClassResolved || !rule.styleResolved || !ActorMatchesClass(actor, rule.actorClass)) continue;
			DCoreActor* owner = actor->GetOwnerActor();
			if (!rule.ownerClassName.IsEmpty() && (!rule.ownerClassResolved || !ActorMatchesClass(owner, rule.ownerClass))) continue;
			if (!rule.excludeOwnerClassName.IsEmpty() &&
				(!rule.excludeOwnerClassResolved || ActorMatchesClass(owner, rule.excludeOwnerClass))) continue;
			Identity identity = { ruleIndex, (int32_t)actor->GetIndex(), actor };
			const DVector3 currentPosition = actor->spr.pos;
			auto stateIt = mActorStates.find(identity);
			if (stateIt == mActorStates.end())
			{
				ActorState state = {};
				state.previousPosition = currentPosition;
				state.previousTimeSeconds = gameplayTimeSeconds;
				state.continuousStableKey = (uint64_t)++mNextContinuousSourceGeneration;
				stateIt = mActorStates.emplace(identity, state).first;
			}
			ActorState& state = stateIt->second;
			state.observed = true;
			const uint32_t actorSourceId = NRIMakeSmokeSourceId("actor", resolved.activeMapName.GetChars(),
				rule.id.GetChars(), static_cast<uint32_t>(actor->GetIndex()));
			const NRISmokeActorSourceLifetime sourceLifetime = NRIClassifySmokeActorSource({
				rule.id.GetChars(), rule.trigger == LightOverlaySmokeTrigger::Interval,
				rule.representation == LightOverlaySmokeRepresentation::Grid,
				rule.spacing, rule.velocityScale });
			if (traceMode != 0) observedPerRule[ruleIndex]++;
			const bool appearanceReady = rule.activationPolicy == LightOverlayActorActivationPolicy::Immediate ||
				sceneLights.HasActorAppearanceEvidence(identity.actorIndex);
			state.appearanceObserved = state.appearanceObserved || appearanceReady;
			if (traceMode != 0)
			{
				appearanceReadyPerRule[ruleIndex] += appearanceReady ? 1u : 0u;
				appearanceObservedPerRule[ruleIndex] += state.appearanceObserved ? 1u : 0u;
			}
			auto publishActorAuthority = [&](uint32_t emittedNow)
			{
				if (traceMode < 2 || rule.id.CompareNoCase("duke_fire_sustained") != 0)
					return;
				const bool cadenceActive = state.emitted;
				if (state.authorityTracePublished && emittedNow == 0u &&
					state.authorityTraceAppearanceReady == appearanceReady &&
					state.authorityTraceActivationLatched == state.activationLatched &&
					state.authorityTraceCadenceActive == cadenceActive)
					return;
				Printf("NRI PT smoke emitter: event=actor-authority rule=%s class=%s actor=%d source_id=%08x world=(%.3f,%.3f,%.3f) appearance_ready=%u appearance_seen=%u activation_latched=%u cadence_active=%u emitted_now=%u\n",
					rule.id.GetChars(), actor->GetClass()->TypeName.GetChars(), actor->GetIndex(), actorSourceId,
					currentPosition.X, currentPosition.Y, currentPosition.Z,
					appearanceReady ? 1u : 0u, state.appearanceObserved ? 1u : 0u,
					state.activationLatched ? 1u : 0u, cadenceActive ? 1u : 0u, emittedNow);
				state.authorityTracePublished = true;
				state.authorityTraceAppearanceReady = appearanceReady;
				state.authorityTraceActivationLatched = state.activationLatched;
				state.authorityTraceCadenceActive = cadenceActive;
			};
			if (!state.activationLatched)
			{
				if (rule.activationPolicy == LightOverlayActorActivationPolicy::Immediate || appearanceReady)
				{
					state.activationLatched = true;
					state.activationTimeSeconds = gameplayTimeSeconds;
					state.startTimeElapsed = rule.startTime <= 0.0f;
					if (traceMode != 0) activatedPerRule[ruleIndex]++;
				}
				else
				{
					state.previousPosition = currentPosition;
					state.previousTimeSeconds = gameplayTimeSeconds;
					state.activationTimeSeconds = gameplayTimeSeconds;
					state.spacingRemainder = 0.0f;
					state.intervalRemainder = 0.0;
					state.startDistanceTraveled = 0.0;
					state.startTimeElapsed = false;
					if (traceMode != 0) deferredPerRule[ruleIndex]++;
					if (traceMode >= 2 && verbosePrinted < 32u)
					{
						Printf("NRI PT smoke emitter: event=activation-deferred rule=%s class=%s actor=%d identity=%p activation=surface appearance=no\n",
							rule.id.GetChars(), actor->GetClass()->TypeName.GetChars(), actor->GetIndex(), actor);
						verbosePrinted++;
					}
					publishActorAuthority(0u);
					continue;
				}
			}
			if (traceMode != 0) activationLatchedPerRule[ruleIndex]++;
			struct TimedEmission
			{
				DVector3 position;
				double gameplaySeconds = 0.0;
				uint64_t cadenceOrdinal = 0u;
			};
			std::vector<TimedEmission> emissions;
			uint32_t continuousCadenceSteps = 0u;
			const NRISmokeSourceEnvelope sourceEnvelope = {
				rule.pulseAmount, rule.pulsePeriodCadences, rule.pulsePhase };
			DVector3 cadenceStartPosition = state.previousPosition;
			double cadenceStartTimeSeconds = state.previousTimeSeconds;
			if (!state.emitted)
			{
				// Time and distance advance independently from activation. If both
				// are authored, only the tail after the later crossing enters cadence.
				const DVector3 startSegment = currentPosition - state.previousPosition;
				const double startSegmentLength = startSegment.Length();
				const double elapsedSeconds = std::max(0.0, gameplayTimeSeconds - state.previousTimeSeconds);
				double timeCrossingFraction = 0.0;
				double distanceCrossingFraction = 0.0;
				bool timeReady = state.startTimeElapsed || rule.startTime <= 0.0f;
				if (!timeReady)
				{
					const double previousElapsed = std::max(0.0, state.previousTimeSeconds - state.activationTimeSeconds);
					const double elapsedSinceActivation = std::max(0.0, gameplayTimeSeconds - state.activationTimeSeconds);
					if (elapsedSinceActivation >= (double)rule.startTime)
					{
						timeCrossingFraction = elapsedSeconds > 0.0 ?
							std::clamp(((double)rule.startTime - previousElapsed) / elapsedSeconds, 0.0, 1.0) : 1.0;
						timeReady = true;
						state.startTimeElapsed = true;
					}
					else
					{
						if (traceMode != 0) timeDeferredPerRule[ruleIndex]++;
						if (traceMode >= 2 && verbosePrinted < 32u)
						{
							Printf("NRI PT smoke emitter: event=starttime-deferred rule=%s class=%s actor=%d identity=%p elapsed=%.3f starttime=%.3f\n",
								rule.id.GetChars(), actor->GetClass()->TypeName.GetChars(), actor->GetIndex(), actor,
								elapsedSinceActivation, rule.startTime);
							verbosePrinted++;
						}
					}
				}

				bool distanceReady = rule.startDistance <= 0.0f || state.startDistanceTraveled >= (double)rule.startDistance;
				if (!distanceReady)
				{
					const double remainingDistance = std::max(0.0, (double)rule.startDistance - state.startDistanceTraveled);
					if (startSegmentLength >= remainingDistance && startSegmentLength > 0.0)
					{
						distanceCrossingFraction = std::clamp(remainingDistance / startSegmentLength, 0.0, 1.0);
						state.startDistanceTraveled = rule.startDistance;
						distanceReady = true;
					}
					else
					{
						state.startDistanceTraveled += startSegmentLength;
					}
				}

				if (timeReady && distanceReady)
				{
					const double crossingFraction = std::max(timeCrossingFraction, distanceCrossingFraction);
					cadenceStartPosition = state.previousPosition + startSegment * crossingFraction;
					cadenceStartTimeSeconds = state.previousTimeSeconds + elapsedSeconds * crossingFraction;
					emissions.push_back({ cadenceStartPosition, cadenceStartTimeSeconds,
						state.continuousCadenceOrdinal + 1u });
					continuousCadenceSteps = 1u;
					state.emitted = true;
				}
			}
			if (state.emitted && rule.trigger == LightOverlaySmokeTrigger::Interval)
			{
				const DVector3 segment = currentPosition - cadenceStartPosition;
				const double segmentLength = segment.Length();
				uint32_t candidateCount = 0;
				double firstCrossing = 0.0;
				double measure = 0.0;
				const bool spatialCadence = rule.spacing > 0.0f && segmentLength > 0.0;
				if (spatialCadence)
				{
					state.intervalRemainder = 0.0;
					measure = segmentLength;
					const double total = (double)state.spacingRemainder + measure;
					candidateCount = (uint32_t)std::floor(total / (double)rule.spacing);
					firstCrossing = (double)rule.spacing - (double)state.spacingRemainder;
					state.spacingRemainder = (float)std::fmod(total, (double)rule.spacing);
				}
				else
				{
					measure = std::max(0.0, gameplayTimeSeconds - cadenceStartTimeSeconds);
					const double total = state.intervalRemainder + measure;
					candidateCount = (uint32_t)std::floor(total / (double)rule.intervalSeconds);
					firstCrossing = (double)rule.intervalSeconds - state.intervalRemainder;
					state.intervalRemainder = std::fmod(total, (double)rule.intervalSeconds);
				}

				const uint32_t emitCount = std::min(candidateCount, rule.maxSegmentsPerFrame);
				const uint32_t cadenceStepsBeforeInterval = continuousCadenceSteps;
				const uint32_t skipped = candidateCount - emitCount;
				const double stride = spatialCadence ? (double)rule.spacing : (double)rule.intervalSeconds;
				for (uint32_t emissionIndex = 0; emissionIndex < emitCount; ++emissionIndex)
				{
					const double crossing = firstCrossing + (double)(skipped + emissionIndex) * stride;
					const double fraction = measure > 0.0 ? std::clamp(crossing / measure, 0.0, 1.0) : 1.0;
					const double emissionTime = cadenceStartTimeSeconds +
						std::max(0.0, gameplayTimeSeconds - cadenceStartTimeSeconds) * fraction;
					emissions.push_back({ cadenceStartPosition + segment * fraction, emissionTime,
						state.continuousCadenceOrdinal + (uint64_t)cadenceStepsBeforeInterval +
						(uint64_t)skipped + (uint64_t)emissionIndex + 1u });
				}
				continuousCadenceSteps = candidateCount > UINT32_MAX - continuousCadenceSteps ?
					UINT32_MAX : continuousCadenceSteps + candidateCount;
			}

			state.previousPosition = currentPosition;
			state.previousTimeSeconds = gameplayTimeSeconds;
			const DVector3 forward = ActorForward(actor);
			const DVector3 right(-forward.Y, forward.X, 0.0);
			const DVector3 up(0.0, 0.0, 1.0);
			const DVector3 offset = right * rule.offset[0] + forward * rule.offset[1] + up * rule.offset[2];
			const DVector3 velocity = ActorVelocity(actor) * rule.velocityScale;
			const bool useAnalyticBridge = !emissions.empty() &&
				rule.representation == LightOverlaySmokeRepresentation::Grid &&
				rule.queuePolicy == LightOverlaySmokeQueuePolicy::Latest &&
				sourceLifetime == NRISmokeActorSourceLifetime::Transitory;
			const uint64_t bridgeSourceKey = useAnalyticBridge ?
				(uint64_t(epoch) << 32u) | actorSourceId : 0u;
			const uint64_t bridgeRevision = useAnalyticBridge ? ++state.trailUpdateOrdinal : 0u;
			std::vector<NRISmokeAnalyticTrailPoint> bridgePoints;
			bridgePoints.reserve(emissions.size());
			for (const TimedEmission& emission : emissions)
			{
				const DVector3& emissionPosition = emission.position;
				const float pulseWeight = NRIEvaluateSmokeSourceEnvelope(
					sourceEnvelope, emission.cadenceOrdinal);
				NRISmokeInjectionCommandGpu command = {};
				WorldToPathTracingPosition(emissionPosition + offset, command.position);
				WorldToPathTracingDirection(velocity, command.velocity);
				command.spawnRadius = rule.spawnRadius;
				command.styleIndex = rule.styleIndex;
				command.count = rule.count;
				command.serial = nextSerial++;
				command.densityScale = rule.densityScale * pulseWeight;
				command.radiusScale = rule.radiusScale;
				command.velocityCone = rule.velocityCone;
				command.epoch = epoch;
				command.sourceId = actorSourceId;
				command.sourceMetadata = NRIPackSmokeSourceMetadata(NRISmokeInjectionSourceClass::InteractiveActor);
				SetPointSourceShape(command);
				if (useAnalyticBridge)
				{
					NRISmokeAnalyticTrailPoint point = {};
					std::copy(command.position, command.position + 3, point.position);
					point.rangeCount = command.count;
					bridgePoints.push_back(point);
				}
				const bool routed = routeCommand(command, rule.representation, rule.queuePolicy,
					rule.hasMaxLatencySeconds, rule.maxLatencySeconds, emission.gameplaySeconds,
					(uint64_t)(uint32_t)actor->GetIndex() << 32u | command.serial,
					rule.analyticCarrierCount,
					sourceLifetime == NRISmokeActorSourceLifetime::Transitory,
					bridgeSourceKey, bridgeRevision);
				if (traceMode != 0)
				{
					emittedPerRule[ruleIndex] += routed ? 1u : 0u;
					particlesPerRule[ruleIndex] += routed ? command.count : 0u;
				}
				const bool publishAuthoritySource = traceMode >= 2 && !state.sourceTracePublished &&
					rule.id.CompareNoCase("duke_fire_sustained") == 0;
				if (traceMode >= 2 && (verbosePrinted < 32u || publishAuthoritySource))
				{
					Printf("NRI PT smoke emitter: event=%s rule=%s class=%s actor=%d identity=%p serial=%u source_id=%08x source_class=%u cadence_ordinal=%llu pulse_weight=%.4f density_scale=%.4f world=(%.3f,%.3f,%.3f) render=(%.3f,%.3f,%.3f) velocity=(%.3f,%.3f,%.3f) style=%u particles=%u\n",
						rule.trigger == LightOverlaySmokeTrigger::Interval ? "interval" : "spawn",
						rule.id.GetChars(), actor->GetClass()->TypeName.GetChars(), actor->GetIndex(), actor, command.serial,
						command.sourceId, static_cast<uint32_t>(NRIGetSmokeSourceClass(command.sourceMetadata)),
						(unsigned long long)emission.cadenceOrdinal, pulseWeight, command.densityScale,
						emissionPosition.X + offset.X, emissionPosition.Y + offset.Y, emissionPosition.Z + offset.Z,
						command.position[0], command.position[1], command.position[2],
						command.velocity[0], command.velocity[1], command.velocity[2], command.styleIndex, command.count);
					verbosePrinted++;
					state.sourceTracePublished = true;
				}
			}
			if (useAnalyticBridge && !bridgePoints.empty() && rule.styleIndex < styles.size())
			{
				const NRISmokeStyleGpu& style = styles[rule.styleIndex];
				NRISmokeAnalyticTrailObservationBatch batch = {};
				batch.observation.stableSourceKey = bridgeSourceKey;
				batch.observation.updateOrdinal = bridgeRevision;
				batch.observation.epoch = epoch;
				batch.observation.sourceId = actorSourceId;
				batch.observation.styleIndex = rule.styleIndex;
				batch.observation.authoredGameplaySeconds = emissions.back().gameplaySeconds;
				WorldToPathTracingDirection(velocity, batch.observation.velocity);
				batch.observation.velocity[1] -= style.riseVelocity;
				batch.observation.radius = std::max(std::max(rule.spawnRadius,
					style.radius * rule.radiusScale), 0.001f);
				double bridgeWeight = 0.0;
				for (const TimedEmission& emission : emissions)
					bridgeWeight += NRIEvaluateSmokeSourceEnvelope(sourceEnvelope, emission.cadenceOrdinal);
				batch.observation.densityScale = rule.densityScale *
					(float)(bridgeWeight / (double)emissions.size());
				batch.observation.expansionVelocity = style.expansionVelocity;
				batch.observation.densityHalfLife = std::min(
					std::max(style.densityHalfLife, 0.001f), 0.18f);
				batch.observation.presentationLifetimeSeconds = 0.35f;
				batch.observation.maximumLatencySeconds = rule.hasMaxLatencySeconds ?
					std::max(rule.maxLatencySeconds, 0.0f) : 0.0f;
				batch.observation.maximumSegmentLength = std::max(rule.spacing *
					(float)rule.maxSegmentsPerFrame, batch.observation.radius * 2.0f);
				batch.points = std::move(bridgePoints);
				trailObservations.push_back(std::move(batch));
			}
			state.continuousCadenceOrdinal += continuousCadenceSteps;
			if (sourceLifetime == NRISmokeActorSourceLifetime::PersistentContinuous)
			{
				NRISmokeContinuousSourceObservation observation = {};
				observation.stableKey = (uint64_t(actorSourceId) << 32u) | state.continuousStableKey;
				observation.cadenceOrdinal = state.continuousCadenceOrdinal;
				observation.gameplayTimeSeconds = gameplayTimeSeconds;
				observation.epoch = epoch;
				observation.sourceId = actorSourceId;
				observation.ruleIndex = ruleIndex;
				observation.actorIndex = identity.actorIndex;
				observation.styleIndex = rule.styleIndex;
				observation.particlesPerCadence = rule.count;
				observation.cadenceSteps = continuousCadenceSteps;
				WorldToPathTracingPosition(currentPosition + offset, observation.position);
				WorldToPathTracingDirection(velocity, observation.velocity);
				observation.spawnRadius = rule.spawnRadius;
				observation.densityScale = rule.densityScale;
				observation.radiusScale = rule.radiusScale;
				observation.pulseEnvelope = sourceEnvelope;
				observation.established = state.activationLatched && state.emitted;
				mContinuousSources.Observe(observation);
			}
			publishActorAuthority((uint32_t)emissions.size());
			if (traceMode != 0) cadenceActivePerRule[ruleIndex] += state.emitted ? 1u : 0u;
		}
	}
	if (traceMode != 0)
	{
		for (uint32_t ruleIndex = 0; ruleIndex < resolved.smokeActorRules.Size(); ++ruleIndex)
		{
			if (emittedPerRule[ruleIndex] == 0u && deferredPerRule[ruleIndex] == 0u &&
				timeDeferredPerRule[ruleIndex] == 0u && activatedPerRule[ruleIndex] == 0u) continue;
			const auto& rule = resolved.smokeActorRules[ruleIndex];
			Printf("NRI PT smoke emitter: event=frame-summary rule=%s class=%s observed=%u emitted=%u particles=%u activation=%s appearance_ready=%u appearance_seen=%u activation_latched=%u deferred=%u activated=%u starttime=%.3f time_deferred=%u cadence_active=%u\n",
				rule.id.GetChars(), rule.actorClassName.GetChars(), observedPerRule[ruleIndex], emittedPerRule[ruleIndex], particlesPerRule[ruleIndex],
				rule.activationPolicy == LightOverlayActorActivationPolicy::Immediate ? "immediate" : "surface",
				appearanceReadyPerRule[ruleIndex], appearanceObservedPerRule[ruleIndex], activationLatchedPerRule[ruleIndex],
				deferredPerRule[ruleIndex], activatedPerRule[ruleIndex], rule.startTime,
				timeDeferredPerRule[ruleIndex], cadenceActivePerRule[ruleIndex]);
		}
	}
	for (auto it = mActorStates.begin(); it != mActorStates.end(); )
		it = !it->second.observed ? mActorStates.erase(it) : std::next(it);
	const NRISmokeContinuousSourceSnapshot& continuousSnapshot = mContinuousSources.EndFrame();
	if (traceMode >= 2 && (continuousSnapshot.observed != 0u ||
		continuousSnapshot.cadenceStepsDiscarded != 0u))
	{
		Printf("NRI PT smoke emitter: event=continuous-source-summary observed=%u established=%u pending=%u selected=%u deferred=%u expired=%u cadence_accepted=%u cadence_coalesced=%u cadence_discarded=%u\n",
			continuousSnapshot.observed, continuousSnapshot.established, continuousSnapshot.pending,
			continuousSnapshot.selected, continuousSnapshot.deferred, continuousSnapshot.expired,
			continuousSnapshot.cadenceStepsAccepted, continuousSnapshot.cadenceStepsCoalesced,
			continuousSnapshot.cadenceStepsDiscarded);
	}

	struct MapEmissionStats
	{
		uint32_t active = 0u;
		uint32_t emitted = 0u;
		uint32_t particles = 0u;
		uint32_t skipped = 0u;
		uint32_t dormant = 0u;
		uint32_t coalesced = 0u;
		uint32_t debt = 0u;
		uint32_t footprintBricks = 0u;
		NRISmokeInterestTier tier = NRISmokeInterestTier::Hot;
	};
	auto emitMapRule = [&](const ResolvedLightOverlayMapSmokeEmitterRule& rule,
		MapEmitterState& state, const char* eventName,
		NRISmokeInjectionSourceClass sourceClass, NRISmokeInterestTier tier) -> MapEmissionStats
	{
		MapEmissionStats stats = {};
		stats.active = 1u;
		stats.tier = tier;
		LightOverlayMapSmokeEmitterRectangle rectangle = {};
		if (!BuildLightOverlayMapSmokeEmitterRectangle(rule, rectangle))
		{
			if (traceMode >= 2 && verbosePrinted < 32u)
			{
				Printf("NRI PT smoke emitter: event=%s-ignored map=%s rule=%s reason=invalid-basis\n",
					eventName, resolved.activeMapName.GetChars(), rule.id.GetChars());
				verbosePrinted++;
			}
			return stats;
		}
		const DVector3 center(rectangle.center[0], rectangle.center[1], rectangle.center[2]);
		const DVector3 normal(rectangle.normal[0], rectangle.normal[1], rectangle.normal[2]);
		const DVector3 axisU(rectangle.halfAxisU[0], rectangle.halfAxisU[1], rectangle.halfAxisU[2]);
		const DVector3 axisV(rectangle.halfAxisV[0], rectangle.halfAxisV[1], rectangle.halfAxisV[2]);
		const DVector3 velocity = normal * rule.velocityScale;
		if (rule.styleIndex < styles.size())
		{
			NRISmokeInjectionCommandGpu footprint = {};
			WorldToPathTracingPosition(center, footprint.position);
			WorldToPathTracingDirection(axisU, footprint.halfAxisU);
			WorldToPathTracingDirection(axisV, footprint.halfAxisV);
			footprint.spawnRadius = rule.spawnRadius;
			footprint.radiusScale = rule.radiusScale;
			stats.footprintBricks = ConservativeMapEmitterBrickFootprint(
				footprint, styles[rule.styleIndex], gridCellSize);
		}

		const double intervalSeconds = std::max(0.001, (double)rule.intervalSeconds);
		uint32_t candidateCount = 0u;
		uint64_t firstCadenceOrdinal = 0u;
		if (!state.initialized)
		{
			state.initialized = true;
			state.previousTimeSeconds = gameplayTimeSeconds;
			state.logicalElapsedSeconds = gameplayTimeSeconds;
			const double nonNegativeTime = std::max(gameplayTimeSeconds, 0.0);
			firstCadenceOrdinal = (uint64_t)std::floor(nonNegativeTime / intervalSeconds);
			state.nextCadenceOrdinal = firstCadenceOrdinal + 1u;
			state.intervalRemainder = std::fmod(nonNegativeTime, intervalSeconds);
			candidateCount = 1u;
		}
		else
		{
			const double elapsedSeconds = std::max(0.0, gameplayTimeSeconds - state.previousTimeSeconds);
			state.previousTimeSeconds = gameplayTimeSeconds;
			state.logicalElapsedSeconds += elapsedSeconds;
			const double total = state.intervalRemainder + elapsedSeconds;
			candidateCount = (uint32_t)std::min<double>(std::floor(total / intervalSeconds), UINT32_MAX);
			state.intervalRemainder = std::fmod(total, intervalSeconds);
			firstCadenceOrdinal = state.nextCadenceOrdinal;
			state.nextCadenceOrdinal += candidateCount;
		}
		constexpr uint32_t MaximumCoalescedDebt = 4096u;
		const bool dormant = tier == NRISmokeInterestTier::Dormant;
		const bool promoting = state.previousTier == NRISmokeInterestTier::Dormant && !dormant && state.coalescedDebt != 0u;
		state.previousTier = tier;
		if (dormant)
		{
			state.coalescedDebt = std::min<uint32_t>(MaximumCoalescedDebt,
				state.coalescedDebt + std::min(candidateCount, MaximumCoalescedDebt - state.coalescedDebt));
			stats.skipped = candidateCount;
			stats.dormant = candidateCount;
			stats.debt = state.coalescedDebt;
			return stats;
		}

		uint32_t emitCount = std::min(candidateCount, rule.maxSegmentsPerFrame);
		stats.skipped = candidateCount - emitCount;
		if (promoting)
		{
			stats.coalesced = state.coalescedDebt;
			state.coalescedDebt = 0u;
			// If no cadence crossing occurred this frame, one ordinary current-time
			// pulse starts prewarming. Missed history is never replayed wholesale.
			if (emitCount == 0u && rule.maxSegmentsPerFrame != 0u)
			{
				emitCount = 1u;
				candidateCount = 1u;
				firstCadenceOrdinal = state.nextCadenceOrdinal > 0u ? state.nextCadenceOrdinal - 1u : 0u;
			}
		}
		stats.debt = state.coalescedDebt;
		const uint32_t sourceId = NRIMakeSmokeSourceId(eventName,
			resolved.activeMapName.GetChars(), rule.id.GetChars());
		for (uint32_t emissionIndex = 0; emissionIndex < emitCount; ++emissionIndex)
		{
			const uint64_t cadenceOrdinal = firstCadenceOrdinal + (uint64_t)(candidateCount - emitCount + emissionIndex);
			NRISmokeInjectionCommandGpu command = {};
			WorldToPathTracingPosition(center, command.position);
			WorldToPathTracingDirection(velocity, command.velocity);
			WorldToPathTracingDirection(axisU, command.halfAxisU);
			WorldToPathTracingDirection(axisV, command.halfAxisV);
			command.spawnRadius = rule.spawnRadius;
			command.styleIndex = rule.styleIndex;
			command.count = rule.count;
			command.serial = NRIMakeSmokeSourceId("map-pulse", resolved.activeMapName.GetChars(),
				rule.id.GetChars(), (uint32_t)cadenceOrdinal);
			command.densityScale = rule.densityScale;
			command.radiusScale = rule.radiusScale;
			command.velocityCone = rule.velocityCone;
			command.epoch = epoch;
			command.shape = static_cast<uint32_t>(NRISmokeInjectionShape::Rectangle);
			command.sourceId = sourceId;
			command.sourceMetadata = NRIPackSmokeSourceMetadata(sourceClass);
			commands.push_back(command);
			commandEnqueueInfo.push_back({});
			state.emitted = true;
			stats.emitted++;
			stats.particles += command.count;
			if (traceMode >= 2 && verbosePrinted < 32u)
			{
				Printf("NRI PT smoke emitter: event=%s map=%s rule=%s command_serial=%u cadence_ordinal=%llu source_id=%08x source_class=%u style=%u particles=%u render=(%.3f,%.3f,%.3f) velocity=(%.3f,%.3f,%.3f) axis_u=(%.3f,%.3f,%.3f) axis_v=(%.3f,%.3f,%.3f) shape=rectangle\n",
					eventName, resolved.activeMapName.GetChars(), rule.id.GetChars(), command.serial,
					(unsigned long long)cadenceOrdinal,
					command.sourceId, static_cast<uint32_t>(NRIGetSmokeSourceClass(command.sourceMetadata)),
					command.styleIndex, command.count,
					command.position[0], command.position[1], command.position[2],
					command.velocity[0], command.velocity[1], command.velocity[2],
					command.halfAxisU[0], command.halfAxisU[1], command.halfAxisU[2],
					command.halfAxisV[0], command.halfAxisV[1], command.halfAxisV[2]);
				verbosePrinted++;
			}
		}
		return stats;
	};

	MapSmokeEmitterEditorRuntimePreview editorPreview = {};
	const bool hasEditorPreview = GetMapSmokeEmitterEditorRuntimePreview(editorPreview) && editorPreview.active &&
		resolved.currentMapAvailable && editorPreview.rule.mapName.CompareNoCase(resolved.activeMapName) == 0;
	for (uint32_t ruleIndex = 0; ruleIndex < resolved.mapSmokeEmitterRules.Size(); ++ruleIndex)
	{
		const auto& rule = resolved.mapSmokeEmitterRules[ruleIndex];
		if (!resolved.currentMapAvailable || !rule.styleResolved ||
			!rule.hasPosition || !rule.hasNormal || !rule.hasSize ||
			rule.mapName.CompareNoCase(resolved.activeMapName) != 0)
		{
			continue;
		}
		if (hasEditorPreview && editorPreview.suppressPersistedRule &&
			rule.mapName.CompareNoCase(editorPreview.rule.mapName) == 0 &&
			rule.id.CompareNoCase(editorPreview.rule.id) == 0)
		{
			continue;
		}
		const uint32_t sourceId = NRIMakeSmokeSourceId("map", resolved.activeMapName.GetChars(), rule.id.GetChars());
		const NRISmokeInterestTier tier = interest.Resolve(sourceId);
		const MapEmissionStats stats = emitMapRule(rule, mMapEmitterStates[sourceId], "map",
			NRISmokeInjectionSourceClass::AmbientMap, tier);
		if (traceMode != 0)
		{
			Printf("NRI PT smoke emitter: event=map-frame-summary map=%s rule=%s active=%u tier=%u emitted=%u particles=%u skipped=%u dormant=%u coalesced=%u debt=%u interval=%.3f footprint_bricks=%u grid_capacity=%u impossible=%u shape=rectangle\n",
				resolved.activeMapName.GetChars(), rule.id.GetChars(), stats.active, (uint32_t)stats.tier,
				stats.emitted, stats.particles, stats.skipped, stats.dormant, stats.coalesced,
				stats.debt, rule.intervalSeconds, stats.footprintBricks, gridBrickCapacity,
				stats.footprintBricks > gridBrickCapacity ? 1u : 0u);
		}
	}

	if (hasEditorPreview)
	{
		const ResolvedLightOverlaySmokeStyle* previewStyle = nullptr;
		for (const auto& style : resolved.smokeStyles)
		{
			if (style.id.CompareNoCase(editorPreview.rule.styleId) == 0)
			{
				previewStyle = &style;
				break;
			}
		}
		if (previewStyle != nullptr && editorPreview.rule.hasPosition &&
			editorPreview.rule.hasNormal && editorPreview.rule.hasSize)
		{
			if (mEditorPreviewMapName.CompareNoCase(editorPreview.rule.mapName) != 0 ||
				mEditorPreviewRuleId.CompareNoCase(editorPreview.rule.id) != 0)
			{
				mEditorPreviewState = {};
				mEditorPreviewMapName = editorPreview.rule.mapName;
				mEditorPreviewRuleId = editorPreview.rule.id;
			}
			ResolvedLightOverlayMapSmokeEmitterRule previewRule = {};
			static_cast<ParsedLightOverlayMapSmokeEmitterRule&>(previewRule) = editorPreview.rule;
			previewRule.styleIndex = previewStyle->styleIndex;
			previewRule.styleResolved = true;
			const MapEmissionStats stats = emitMapRule(previewRule, mEditorPreviewState, "map-preview",
				NRISmokeInjectionSourceClass::Diagnostic, NRISmokeInterestTier::Hot);
			if (traceMode != 0)
			{
				Printf("NRI PT smoke emitter: event=map-preview-frame-summary map=%s rule=%s active=%u emitted=%u particles=%u skipped=%u interval=%.3f revision=%u shape=rectangle\n",
					resolved.activeMapName.GetChars(), previewRule.id.GetChars(), stats.active, stats.emitted,
					stats.particles, stats.skipped, previewRule.intervalSeconds, editorPreview.revision);
			}
		}
	}
	else
	{
		mEditorPreviewState = {};
		mEditorPreviewMapName = "";
		mEditorPreviewRuleId = "";
	}

	uint32_t eventCommands = 0;
	uint32_t eventParticles = 0;
	for (const PathTracingWeaponLightEvent& event : weaponEvents)
	{
		bool matchedEventRule = false;
		for (const ResolvedLightOverlaySmokeEventRule& rule : resolved.smokeEventRules)
		{
			if (!rule.styleResolved || event.eventId.CompareNoCase(rule.id) != 0)
				continue;
			matchedEventRule = true;
			DVector3 worldPosition = event.worldPosition;
			if (event.hasBasis)
			{
				uint64_t offsetRandomState = BuildSmokeOffsetRandomSeed(event);
				const float resolvedOffset[3] = {
					rule.offset[0] + rule.offsetRandom[0] *
						NextSmokeOffsetSignedRandom(offsetRandomState),
					rule.offset[1] + rule.offsetRandom[1] *
						NextSmokeOffsetSignedRandom(offsetRandomState),
					rule.offset[2] + rule.offsetRandom[2] *
						NextSmokeOffsetSignedRandom(offsetRandomState)
				};
				worldPosition += event.basisRight * resolvedOffset[0] +
					event.basisForward * resolvedOffset[1] +
					event.basisUp * resolvedOffset[2];
			}
			if (event.hasSurfaceNormal && !event.surfaceNormal.isZero())
				worldPosition += event.surfaceNormal.Unit() * rule.normalOffset;

			DVector3 worldVelocity;
			bool directionAvailable = false;
			switch (rule.directionPolicy)
			{
			case LightOverlaySmokeDirectionPolicy::Normal:
				if (event.hasSurfaceNormal && !event.surfaceNormal.isZero())
				{
					worldVelocity = event.surfaceNormal.Unit();
					directionAvailable = true;
				}
				break;
			case LightOverlaySmokeDirectionPolicy::Incoming:
				if (event.hasIncomingDirection && !event.incomingDirection.isZero())
				{
					worldVelocity = event.incomingDirection.Unit();
					directionAvailable = true;
				}
				break;
			case LightOverlaySmokeDirectionPolicy::Aim:
			default:
				break;
			}
			// Aim is also the safe fallback for older producers and events that do
			// not carry the requested optional normal/incoming vector.
			if (!directionAvailable && event.hasBasis && !event.basisForward.isZero())
			{
				worldVelocity = event.basisForward.Unit();
				directionAvailable = true;
			}
			worldVelocity *= rule.velocityScale;

			NRISmokeInjectionCommandGpu command = {};
			WorldToPathTracingPosition(worldPosition, command.position);
			if (directionAvailable)
				WorldToPathTracingDirection(worldVelocity, command.velocity);
			command.spawnRadius = rule.spawnRadius;
			command.styleIndex = rule.styleIndex;
			command.count = rule.count;
			command.serial = nextSerial++;
			command.densityScale = rule.densityScale;
			command.radiusScale = rule.radiusScale;
			command.velocityCone = rule.velocityCone;
			command.epoch = epoch;
			command.sourceId = NRIMakeSmokeSourceId("event", resolved.activeMapName.GetChars(),
				rule.id.GetChars());
			command.sourceMetadata = NRIPackSmokeSourceMetadata(NRISmokeInjectionSourceClass::InteractiveEvent);
			SetPointSourceShape(command);
			const bool routed = routeCommand(command, rule.representation, rule.queuePolicy,
				rule.hasMaxLatencySeconds, rule.maxLatencySeconds, event.absoluteTimeSeconds,
				event.serial, rule.analyticCarrierCount, true, 0u, 0u);
			if (!routed)
			{
				if (traceMode != 0)
					Printf("NRI PT smoke emitter: event=weapon-ignored source_event=%s source_serial=%llu reason=unsupported-analytic-policy\n",
						event.eventId.GetChars(), (unsigned long long)event.serial);
				continue;
			}
			eventCommands++;
			eventParticles += command.count;

			if (traceMode != 0 && verbosePrinted < 32u)
			{
				Printf("NRI PT smoke emitter: event=weapon rule=%s source_event=%s source_serial=%llu command_serial=%u source_id=%08x source_class=%u style=%u particles=%u render=(%.3f,%.3f,%.3f) direction=(%.3f,%.3f,%.3f) cone=%.3f normal=%u incoming=%u\n",
					rule.id.GetChars(), event.eventId.GetChars(), (unsigned long long)event.serial, command.serial,
					command.sourceId, static_cast<uint32_t>(NRIGetSmokeSourceClass(command.sourceMetadata)),
					command.styleIndex, command.count, command.position[0], command.position[1], command.position[2],
					command.velocity[0], command.velocity[1], command.velocity[2], command.velocityCone,
					event.hasSurfaceNormal ? 1u : 0u, event.hasIncomingDirection ? 1u : 0u);
				verbosePrinted++;
			}
		}
		if (!matchedEventRule && traceMode != 0)
		{
			Printf("NRI PT smoke emitter: event=weapon-ignored source_event=%s source_serial=%llu reason=no-rule\n",
				event.eventId.GetChars(), (unsigned long long)event.serial);
		}
	}
	if (traceMode != 0 && eventCommands != 0u)
	{
		Printf("NRI PT smoke emitter: event=weapon-frame-summary source_events=%u commands=%u particles=%u\n",
			(uint32_t)weaponEvents.Size(), eventCommands, eventParticles);
	}
}
