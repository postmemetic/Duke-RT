#include "Include/SmokeResources.hlsli"
#include "Include/SmokeTransientData.hlsli"
#include "Include/SmokeTransientLighting.hlsli"
#include "Include/SmokeEmissiveReservoir.hlsli"

#define NRI_SMOKE_TRANSIENT_MAX_SELECTED_POINTS 8u

void SmokeTransientEmptyLobes(out float3 lobes[6])
{
	[unroll]
	for (uint lobe = 0u; lobe < 6u; ++lobe)
		lobes[lobe] = 0.0;
}

float3 SmokeTransientEnvironmentLobe(uint lobe)
{
	const float3 sampled = gSmokeSkyTexture.SampleLevel(gSmokeLinearWrap,
		NRI_SMOKE_TRANSIENT_LIGHT_AXES[lobe], 0.0).rgb;
	const float3 environment = all(isfinite(sampled)) ? max(sampled, 0.0) : 0.0;
	// Optional six-axis cubemap quadrature, not a reconstruction of sector ambient.
	// Each direction represents 4*pi/6 steradians so isotropic phase closes to one.
	const float cubemapQuadratureWeight = 2.0943951023931953;
	return min(environment * max(gSmokeConstants.IndirectScale, 0.0) *
		cubemapQuadratureWeight, 32.0);
}

void SmokeTransientSelectPointLights(float3 receiverPosition, uint selectionBudget,
	out uint selectedIndices[NRI_SMOKE_TRANSIENT_MAX_SELECTED_POINTS],
	out float selectedScores[NRI_SMOKE_TRANSIENT_MAX_SELECTED_POINTS], out uint selectedCount)
{
	selectedCount = 0u;
	uint lightCapacity, lightStride;
	gSmokeRuntimePointLights.GetDimensions(lightCapacity, lightStride);
	const uint lightCount = min(gSmokeConstants.RuntimeLightCount, lightCapacity);
	const uint limit = min(selectionBudget, NRI_SMOKE_TRANSIENT_MAX_SELECTED_POINTS);
	[loop]
	for (uint lightIndex = 0u; lightIndex < lightCount && limit > 0u; ++lightIndex)
	{
		InterlockedAdd(gSmokeControl[0].TransientLightPointCandidatesTested, 1u);
		const RuntimePointLightData light = gSmokeRuntimePointLights[lightIndex];
		const float distanceToLight = length(light.position - receiverPosition);
		const float score = EvaluateAnalyticPointLightAttenuation(distanceToLight,
			light.radius, light.intensity) * dot(max(light.color, 0.0),
			float3(0.2126, 0.7152, 0.0722));
		if (!(score > 0.0) || !isfinite(score))
			continue;
		if (selectedCount < limit)
		{
			selectedIndices[selectedCount] = lightIndex;
			selectedScores[selectedCount] = score;
			selectedCount++;
			continue;
		}
		uint weakest = 0u;
		[unroll]
		for (uint index = 1u; index < NRI_SMOKE_TRANSIENT_MAX_SELECTED_POINTS; ++index)
		{
			if (index >= selectedCount) break;
			const RuntimePointLightData a = gSmokeRuntimePointLights[selectedIndices[index]];
			const RuntimePointLightData b = gSmokeRuntimePointLights[selectedIndices[weakest]];
			const bool lowerScore = selectedScores[index] < selectedScores[weakest];
			const bool equalScoreLowerKey = selectedScores[index] == selectedScores[weakest] &&
				(a.stableKeyHi < b.stableKeyHi ||
				(a.stableKeyHi == b.stableKeyHi && a.stableKeyLo < b.stableKeyLo));
			if (lowerScore || equalScoreLowerKey) weakest = index;
		}
		const RuntimePointLightData weakestLight = gSmokeRuntimePointLights[selectedIndices[weakest]];
		const bool replace = score > selectedScores[weakest] ||
			(score == selectedScores[weakest] &&
			(light.stableKeyHi > weakestLight.stableKeyHi ||
			(light.stableKeyHi == weakestLight.stableKeyHi && light.stableKeyLo > weakestLight.stableKeyLo)));
		if (replace)
		{
			selectedIndices[weakest] = lightIndex;
			selectedScores[weakest] = score;
		}
	}
	InterlockedAdd(gSmokeControl[0].TransientLightPointSelected, selectedCount);
}

void SmokeTransientEvaluateExternal(SmokeTransientGroup group, float3 receiverPosition,
	bool fullBuild, uint selectedIndices[NRI_SMOKE_TRANSIENT_MAX_SELECTED_POINTS],
	uint selectedCount, uint anchorIndex, out float3 lobes[6],
	out uint familyAttemptMask, out uint familySuccessMask, out uint emissiveKeyLo,
	out uint emissiveKeyHi, out bool unshadowed, out float directionalTransport)
{
	SmokeTransientEmptyLobes(lobes);
	familyAttemptMask = 0u;
	familySuccessMask = 0u;
	emissiveKeyLo = 0u;
	emissiveKeyHi = 0u;
	unshadowed = false;
	directionalTransport = 0.0;
	if ((gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_INDIRECT) != 0u &&
		gSmokeConstants.IndirectScale > 0.0)
	{
		familyAttemptMask |= NRI_SMOKE_TRANSIENT_LIGHT_ENVIRONMENT;
		[unroll]
		for (uint lobe = 0u; lobe < 6u; ++lobe)
			lobes[lobe] = SmokeTransientEnvironmentLobe(lobe);
		familySuccessMask |= NRI_SMOKE_TRANSIENT_LIGHT_ENVIRONMENT;
	}

	if (gSmokeConstants.LightMode > 0u &&
		(gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL) != 0u)
	{
		familyAttemptMask |= NRI_SMOKE_TRANSIENT_LIGHT_DIRECTIONAL;
		const float3 direction = SmokeDirectionalDirection();
		float visibility = 1.0;
		const bool shadowConfigured = gSmokeConstants.LightMode >= 2u &&
			(gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_DIRECTIONAL_SHADOW) != 0u;
		const bool wantsShadow = fullBuild && shadowConfigured;
		if (wantsShadow && SmokeShadowTracingReady())
		{
			InterlockedAdd(gSmokeControl[0].TransientLightVisibilityRays, 1u);
			InterlockedAdd(gSmokeControl[0].TransientLightDirectionalSamples, 1u);
			visibility = (SmokeFilteredVisibilityEffective()
				? SmokePointLightVisibleFiltered(receiverPosition, direction, 100000.0, false)
				: SmokePointLightVisible(receiverPosition, direction, 100000.0, false)) ? 1.0 : 0.0;
		}
		else if (shadowConfigured)
		{
			unshadowed = true;
		}
		float selfTransmittance = 1.0;
		if (fullBuild &&
			group.TransientClass != NRI_SMOKE_TRANSIENT_CLASS_FIRE &&
			(gSmokeConstants.LightSourceFlags & NRI_SMOKE_TRANSIENT_SELF_SHADOW) != 0u)
		{
			InterlockedAdd(gSmokeControl[0].TransientLightSelfTransmittanceTests, 1u);
			selfTransmittance = SmokeTransientSelfTransmittance(group,
				SmokeTransientGroupOpticalDepth(group, receiverPosition, direction, 100000.0));
		}
		directionalTransport = group.TransientClass == NRI_SMOKE_TRANSIENT_CLASS_FIRE
			? visibility : visibility * selfTransmittance;
		const float3 incident = SmokeDirectionalColor() * directionalTransport;
		// Fire caches geometry visibility alone. Its current local self-attenuation,
		// directional color and phase are evaluated during materialization, so exposed
		// shoulders do not inherit the dense interior anchors' smoke attenuation.
		if (group.TransientClass != NRI_SMOKE_TRANSIENT_CLASS_FIRE)
			SmokeTransientAccumulateIncident(incident, direction, lobes);
		if (any(incident > 0.0))
			familySuccessMask |= NRI_SMOKE_TRANSIENT_LIGHT_DIRECTIONAL;
	}

	if (gSmokeConstants.LightMode > 0u &&
		(gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_POINT) != 0u)
	{
		familyAttemptMask |= NRI_SMOKE_TRANSIENT_LIGHT_POINT;
		[loop]
		for (uint selected = 0u; selected < selectedCount; ++selected)
		{
			const RuntimePointLightData light = gSmokeRuntimePointLights[selectedIndices[selected]];
			const float3 toLight = light.position - receiverPosition;
			const float distanceSquared = dot(toLight, toLight);
			if (distanceSquared <= 1e-6) continue;
			const float distanceToLight = sqrt(distanceSquared);
			const float3 direction = toLight / distanceToLight;
			const float attenuation = EvaluateAnalyticPointLightAttenuation(distanceToLight,
				light.radius, light.intensity);
			if (attenuation <= 0.0) continue;
			float visibility = 1.0;
			const bool shadowConfigured = gSmokeConstants.LightMode >= 2u &&
				(light.flags & NRI_SMOKE_RUNTIME_LIGHT_FLAG_CASTS_SHADOW) != 0u;
			const bool wantsShadow = fullBuild && shadowConfigured;
			if (wantsShadow && SmokeShadowTracingReady())
			{
				InterlockedAdd(gSmokeControl[0].TransientLightVisibilityRays, 1u);
				visibility = (SmokeFilteredVisibilityEffective()
					? SmokePointLightVisibleFiltered(receiverPosition, direction, distanceToLight, false)
					: SmokePointLightVisible(receiverPosition, direction, distanceToLight, false)) ? 1.0 : 0.0;
			}
			else if (shadowConfigured)
			{
				unshadowed = true;
			}
			float selfTransmittance = 1.0;
			if (fullBuild &&
				(gSmokeConstants.LightSourceFlags & NRI_SMOKE_TRANSIENT_SELF_SHADOW) != 0u)
			{
				InterlockedAdd(gSmokeControl[0].TransientLightSelfTransmittanceTests, 1u);
				selfTransmittance = SmokeTransientSelfTransmittance(group,
					SmokeTransientGroupOpticalDepth(group, receiverPosition, direction, distanceToLight));
			}
			const float3 incident = max(light.color, 0.0) * attenuation * visibility * selfTransmittance;
			SmokeTransientAccumulateIncident(incident, direction, lobes);
			if (any(incident > 0.0))
				familySuccessMask |= NRI_SMOKE_TRANSIENT_LIGHT_POINT;
		}
	}

	if (gSmokeConstants.LightMode > 0u &&
		(gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_EMISSIVE) != 0u)
	{
		familyAttemptMask |= NRI_SMOKE_TRANSIENT_LIGHT_EMISSIVE;
		const uint sampleCount = fullBuild ? clamp(group.SamplesPerAnchor, 1u, 8u) : 1u;
		[loop]
		for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
		{
			InterlockedAdd(gSmokeControl[0].TransientLightEmissiveSamples, 1u);
			// Adjacent packets from one sustained fire share an emissive proposal
			// pattern instead of exposing pool-slot/generation noise as colored bands.
			// Other transient classes retain their established seed exactly.
			uint randomState = group.TransientClass == NRI_SMOKE_TRANSIENT_CLASS_FIRE
				? SmokeTransientHash(group.SourceId ^ SmokeTransientHash(group.Epoch) ^
					SmokeTransientHash(anchorIndex * 8u + sampleIndex) ^ 0x8f41b36du)
				: SmokeTransientHash(group.Slot ^ SmokeTransientHash(group.Generation) ^
					SmokeTransientHash(group.Epoch) ^
					SmokeTransientHash(anchorIndex * 8u + sampleIndex) ^ 0x8f41b36du);
			const uint candidateIndex = SmokeSampleEmissivePrimitive(randomState);
			if (candidateIndex == 0xffffffffu) continue;
			const EmissivePrimitiveData candidate = gSmokeEmissivePrimitives[candidateIndex];
			SmokeEmissiveReservoirRecord proposal = SmokeEmptyEmissiveReservoir();
			proposal.CandidateIndex = candidateIndex;
			proposal.SampleSeed = randomState;
			proposal.StableKeyLo = candidate.stableKeyLo;
			proposal.StableKeyHi = candidate.stableKeyHi;
			proposal.Generation = gSmokeConstants.CommandCount;
			float3 incident, direction;
			float distanceToLight;
			if (!SmokeEvaluateEmissiveIncident(proposal, receiverPosition, false,
				incident, direction, distanceToLight)) continue;
			float visibility = 1.0;
			if (fullBuild && gSmokeConstants.LightMode >= 2u && SmokeShadowTracingReady())
			{
				InterlockedAdd(gSmokeControl[0].TransientLightVisibilityRays, 1u);
				visibility = (SmokeFilteredVisibilityEffective()
					? SmokeEmissiveVisibleFiltered(receiverPosition, direction, distanceToLight, false)
					: SmokeEmissiveVisible(receiverPosition, direction, distanceToLight, false)) ? 1.0 : 0.0;
			}
			else if (gSmokeConstants.LightMode >= 2u)
			{
				unshadowed = true;
			}
			// Emissive surfaces can dominate a fire packet. Apply the same local
			// cloud attenuation as analytic/directional lighting; scene visibility
			// alone otherwise leaves a dense plume glowing uniformly from within.
			float selfTransmittance = 1.0;
			if (fullBuild &&
				(gSmokeConstants.LightSourceFlags & NRI_SMOKE_TRANSIENT_SELF_SHADOW) != 0u)
			{
				InterlockedAdd(gSmokeControl[0].TransientLightSelfTransmittanceTests, 1u);
				selfTransmittance = SmokeTransientSelfTransmittance(group,
					SmokeTransientGroupOpticalDepth(group, receiverPosition, direction, distanceToLight));
			}
			const float3 estimator = incident * visibility * selfTransmittance /
				max(candidate.selectionPdf * (float)sampleCount, 1e-6);
			SmokeTransientAccumulateIncident(estimator, direction, lobes);
			if (any(estimator > 0.0))
			{
				familySuccessMask |= NRI_SMOKE_TRANSIENT_LIGHT_EMISSIVE;
				emissiveKeyLo = candidate.stableKeyLo;
				emissiveKeyHi = candidate.stableKeyHi;
			}
		}
	}
}

bool SmokeTransientStoreAnchor(bool bankB, SmokeTransientGroup group,
	uint anchorIndex, float3 position, float3 lobes[6], float directionalTransport)
{
	SmokeTransientLightAnchor record = (SmokeTransientLightAnchor)0;
	[unroll]
	for (uint lobe = 0u; lobe < 6u; ++lobe)
		SmokeTransientLightStoreLobe(record, lobe, lobes[lobe]);
	record.Data2.yzw = asuint(position);
	if (group.TransientClass == NRI_SMOKE_TRANSIENT_CLASS_FIRE)
	{
		// These words held a diagnostic-only stored position; materialization has
		// always rebuilt current anchor positions from the current group bounds.
		record.Data2.y = asuint(saturate(isfinite(directionalTransport) ?
			directionalTransport : 0.0));
		record.Data2.z = asuint(max(isfinite(group.AgeSeconds) ? group.AgeSeconds : 0.0, 0.0));
		record.Data2.w = group.Reserved;
	}
	record.Data3 = uint4(group.Slot, group.Generation, group.Epoch,
		NRI_SMOKE_TRANSIENT_ANCHOR_WRITTEN | anchorIndex);
	const uint outputIndex = group.Slot * NRI_SMOKE_TRANSIENT_ANCHOR_COUNT + anchorIndex;
	uint capacity, stride;
	if (bankB)
	{
		gSmokeTransientLightAnchorsB.GetDimensions(capacity, stride);
		if (outputIndex >= capacity) return false;
		gSmokeTransientLightAnchorsB[outputIndex] = record;
	}
	else
	{
		gSmokeTransientLightAnchorsA.GetDimensions(capacity, stride);
		if (outputIndex >= capacity) return false;
		gSmokeTransientLightAnchorsA[outputIndex] = record;
	}
	InterlockedAdd(gSmokeControl[0].TransientLightAnchorsWritten, 1u);
	return true;
}

void SmokeTransientPublish(SmokeTransientGroup group, bool bankB, bool fullBuild,
	uint familyAttemptMask, uint familySuccessMask, bool unshadowed,
	uint pointKeyLo, uint pointKeyHi, uint emissiveKeyLo, uint emissiveKeyHi)
{
	SmokeTransientLightHeader header = (SmokeTransientLightHeader)0;
	header.GroupSlot = group.Slot;
	header.GroupGeneration = group.Generation;
	header.Epoch = group.Epoch;
	header.RequiredAnchorMask = group.RequiredAnchorMask;
	header.PublishedAnchorMask = group.RequiredAnchorMask;
	header.ShapeRevision = SmokeTransientShapeRevision(group);
	header.LightingBoundsRevision = SmokeTransientLightingBoundsRevision(group);
	header.BuildFrame = gSmokeConstants.FrameIndex;
	header.ObservedFrame = 0xffffffffu;
	header.SelectedPointKeyLo = pointKeyLo;
	header.SelectedPointKeyHi = pointKeyHi;
	header.SelectedEmissiveKeyLo = emissiveKeyLo;
	header.SelectedEmissiveKeyHi = emissiveKeyHi;
	header.FamilyAttemptMask = familyAttemptMask;
	header.FamilySuccessMask = familySuccessMask;
	header.PublishedState = 0u;
	gSmokeTransientLightHeaders[group.Slot] = header;
	DeviceMemoryBarrier();
	uint state = NRI_SMOKE_TRANSIENT_LIGHT_VALID | familySuccessMask |
		(bankB ? NRI_SMOKE_TRANSIENT_LIGHT_BANK_B : 0u) |
		(fullBuild ? NRI_SMOKE_TRANSIENT_LIGHT_FULL : NRI_SMOKE_TRANSIENT_LIGHT_FALLBACK) |
		(unshadowed ? NRI_SMOKE_TRANSIENT_LIGHT_UNSHADOWED : 0u);
	gSmokeTransientLightHeaders[group.Slot].PublishedState = state;
	if (fullBuild) InterlockedAdd(gSmokeControl[0].TransientLightPublishedFull, 1u);
	else InterlockedAdd(gSmokeControl[0].TransientLightPublishedFallback, 1u);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint groupCapacity, groupStride, headerCapacity, headerStride;
	gSmokeTransientGroups.GetDimensions(groupCapacity, groupStride);
	gSmokeTransientLightHeaders.GetDimensions(headerCapacity, headerStride);
	const uint groupIndex = dispatchThreadId.x;
	if (groupIndex >= min(NRI_SMOKE_TRANSIENT_GROUP_COUNT,
		min(groupCapacity, NRI_SMOKE_TRANSIENT_MAX_GROUPS))) return;
	const SmokeTransientGroup group = gSmokeTransientGroups[groupIndex];
	if ((group.Flags & NRI_SMOKE_TRANSIENT_GROUP_ACTIVE) == 0u ||
		group.Epoch != gSmokeConstants.SimulationEpoch || group.LobeCount == 0u ||
		group.Slot >= min(headerCapacity, NRI_SMOKE_TRANSIENT_MAX_GROUPS) ||
		group.RequiredAnchorMask == 0u || (group.RequiredAnchorMask & ~0xfu) != 0u ||
		!all(isfinite(group.BoundsMin)) || !all(isfinite(group.BoundsMax)) ||
		any(group.BoundsMax <= group.BoundsMin)) return;
	InterlockedAdd(gSmokeControl[0].TransientLightBuildGroups, 1u);

	const SmokeTransientLightHeader previous = gSmokeTransientLightHeaders[group.Slot];
	const bool previousValid = SmokeTransientHeaderIdentityMatches(previous, group);
	const bool wantsFull = (group.Flags & NRI_SMOKE_TRANSIENT_GROUP_FULL_BUILD) != 0u;
	if (previousValid)
	{
		const bool previousFull = (previous.PublishedState & NRI_SMOKE_TRANSIENT_LIGHT_FULL) != 0u;
		if (!wantsFull || (previousFull &&
			(group.Flags & NRI_SMOKE_TRANSIENT_GROUP_SLOW_REFRESH) == 0u)) return;
	}
	uint claim = 0xffffffffu;
	if (wantsFull)
		InterlockedAdd(gSmokeControl[0].TransientLightFullBuildClaims, 1u, claim);
	const bool buildFull = wantsFull && claim < NRI_SMOKE_TRANSIENT_FULL_BUILD_BUDGET;
	if (previousValid && !buildFull) return;

	uint selectedIndices[NRI_SMOKE_TRANSIENT_MAX_SELECTED_POINTS];
	float selectedScores[NRI_SMOKE_TRANSIENT_MAX_SELECTED_POINTS];
	uint selectedCount = 0u;
	if (gSmokeConstants.LightMode > 0u &&
		(gSmokeConstants.LightSourceFlags & NRI_SMOKE_LIGHT_SOURCE_POINT) != 0u &&
		NRI_SMOKE_TRANSIENT_POINT_BUDGET != 0u)
	{
		SmokeTransientSelectPointLights(group.Center, NRI_SMOKE_TRANSIENT_POINT_BUDGET,
			selectedIndices, selectedScores, selectedCount);
	}
	uint pointKeyLo = 0u, pointKeyHi = 0u;
	if (selectedCount > 0u)
	{
		uint strongestIndex = 0u;
		[unroll]
		for (uint selected = 1u; selected < NRI_SMOKE_TRANSIENT_MAX_SELECTED_POINTS; ++selected)
		{
			if (selected >= selectedCount) break;
			if (selectedScores[selected] > selectedScores[strongestIndex]) strongestIndex = selected;
		}
		const RuntimePointLightData strongest = gSmokeRuntimePointLights[selectedIndices[strongestIndex]];
		pointKeyLo = strongest.stableKeyLo;
		pointKeyHi = strongest.stableKeyHi;
	}

	const bool previousBankB = previousValid &&
		(previous.PublishedState & NRI_SMOKE_TRANSIENT_LIGHT_BANK_B) != 0u;
	const bool fallbackBankB = previousValid ? !previousBankB : false;
	if (!previousValid)
	{
		float3 fallbackLobes[6];
		uint fallbackAttempt, fallbackSuccess, emissiveKeyLo, emissiveKeyHi;
		bool fallbackUnshadowed;
		float fallbackDirectionalTransport;
		SmokeTransientEvaluateExternal(group, group.Center, false, selectedIndices,
			selectedCount, 0u, fallbackLobes, fallbackAttempt, fallbackSuccess,
			emissiveKeyLo, emissiveKeyHi, fallbackUnshadowed,
			fallbackDirectionalTransport);
		uint writtenMask = 0u;
		[unroll]
		for (uint anchorIndex = 0u; anchorIndex < NRI_SMOKE_TRANSIENT_ANCHOR_COUNT; ++anchorIndex)
		{
			if ((group.RequiredAnchorMask & (1u << anchorIndex)) == 0u) continue;
			const float3 anchorPosition = SmokeTransientAnchorPosition(anchorIndex,
				group.BoundsMin, group.BoundsMax);
			if (SmokeTransientStoreAnchor(fallbackBankB, group, anchorIndex,
				anchorPosition, fallbackLobes, fallbackDirectionalTransport))
				writtenMask |= 1u << anchorIndex;
		}
		if (writtenMask != group.RequiredAnchorMask) return;
		DeviceMemoryBarrier();
		SmokeTransientPublish(group, fallbackBankB, false, fallbackAttempt,
			fallbackSuccess, fallbackUnshadowed, pointKeyLo, pointKeyHi,
			emissiveKeyLo, emissiveKeyHi);
		InterlockedAdd(gSmokeControl[0].TransientLightFallbackBuilds, 1u);
	}
	if (!buildFull) return;

	const bool fullBankB = previousValid ? !previousBankB : !fallbackBankB;
	uint fullAttempt = 0u, fullSuccess = 0u;
	uint finalEmissiveKeyLo = 0u, finalEmissiveKeyHi = 0u;
	bool fullUnshadowed = false;
	uint fullWrittenMask = 0u;
	[unroll]
	for (uint anchorIndex = 0u; anchorIndex < NRI_SMOKE_TRANSIENT_ANCHOR_COUNT; ++anchorIndex)
	{
		if ((group.RequiredAnchorMask & (1u << anchorIndex)) == 0u) continue;
		const float3 anchorPosition = SmokeTransientAnchorPosition(anchorIndex,
			group.BoundsMin, group.BoundsMax);
		float3 anchorLobes[6];
		uint attemptMask, successMask, emissiveKeyLo, emissiveKeyHi;
		bool anchorUnshadowed;
		float anchorDirectionalTransport;
		SmokeTransientEvaluateExternal(group, anchorPosition, true, selectedIndices,
			selectedCount, anchorIndex, anchorLobes, attemptMask, successMask,
			emissiveKeyLo, emissiveKeyHi, anchorUnshadowed,
			anchorDirectionalTransport);
		fullAttempt |= attemptMask;
		fullSuccess |= successMask;
		fullUnshadowed = fullUnshadowed || anchorUnshadowed;
		if (emissiveKeyLo != 0u || emissiveKeyHi != 0u)
		{
			finalEmissiveKeyLo = emissiveKeyLo;
			finalEmissiveKeyHi = emissiveKeyHi;
		}
		if (SmokeTransientStoreAnchor(fullBankB, group, anchorIndex,
			anchorPosition, anchorLobes, anchorDirectionalTransport))
			fullWrittenMask |= 1u << anchorIndex;
	}
	if (fullWrittenMask != group.RequiredAnchorMask) return;
	DeviceMemoryBarrier();
	SmokeTransientPublish(group, fullBankB, true, fullAttempt, fullSuccess,
		fullUnshadowed, pointKeyLo, pointKeyHi, finalEmissiveKeyLo, finalEmissiveKeyHi);
	InterlockedAdd(gSmokeControl[0].TransientLightFullBuilds, 1u);
}
