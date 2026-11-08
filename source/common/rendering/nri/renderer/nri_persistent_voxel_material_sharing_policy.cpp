#include "nri_persistent_voxel_material_sharing_policy.h"

#include "coreactor.h"
#include "actor_lifecycle_journal.h"
#include "lightoverlay.h"
#include "perf_audit.h"

bool NRIVoxelMaterialSharingPolicy::BeginFrame(uint64_t buildSerial, uint32_t frameIndex)
{
	const uint32_t generation = GetResolvedLightOverlaySet().resolvedGeneration;
	const auto& lifecycle = GetActorLifecycleJournal();
	const bool lifecycleInvalidated = lifecycleChanges.Poll(lifecycle);
	const bool changed = world != buildSerial || rules != generation || lifecycleInvalidated;
	if (changed || frame != frameIndex || actorLifecycle != lifecycle.LatestSerial())
	{
		world = buildSerial; frame = frameIndex; rules = generation;
		actorWorld = lifecycle.WorldEpoch(); actorLifecycle = lifecycle.LatestSerial();
		actorsReady = false;
	}
	return changed;
}

void NRIVoxelMaterialSharingPolicy::BuildActorClassEligibility()
{
	actorsReady = true;
	actors.clear();
	const auto& resolved = GetResolvedLightOverlaySet();
	std::unordered_map<PClass*, bool> classes;
	auto classInvariant = [&](PClass* actorClass)
	{
		if (!actorClass || resolved.resolvedGeneration != rules) return false;
		auto canMatch = [&](const auto& rule)
		{
			PerfAuditAdd(PerfAuditMetric::VoxelMaterialSharingClassRuleChecks);
			// Unknown classes and future tile/owner changes cannot certify invariance.
			return !rule.actorClassResolved || !rule.actorClass || actorClass == rule.actorClass ||
				actorClass->IsDescendantOf(rule.actorClass);
		};
		// Even light-only actor rules stay excluded: their rule IDs enter occurrence
		// metadata. This intentionally accepts a smaller, easily proven subset.
		for (const auto& rule : resolved.actorRules) if (canMatch(rule)) return false;
		for (const auto& rule : resolved.actorOverrideRules) if (canMatch(rule)) return false;
		for (const auto& rule : resolved.smokeActorRules)
			if (rule.emitterForeground && canMatch(rule)) return false;
		return true;
	};
	TSpriteIterator<DCoreActor> it;
	while (auto* actor = it.Next())
	{
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialSharingClassActorsVisited);
		if (!actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0) continue;
		auto* actorClass = actor->GetClass();
		auto found = classes.find(actorClass);
		if (found == classes.end()) found = classes.emplace(actorClass, classInvariant(actorClass)).first;
		actors.emplace((int32_t)actor->GetIndex(), found->second);
	}
}

NRIVoxelMaterialSharingCertificate NRIVoxelMaterialSharingPolicy::Certify(const nri_scene::MaterialBridgeData& bridge)
{
	NRIVoxelMaterialSharingCertificate result = { world, rules, false, actorWorld };
	if (!world || !actorWorld || bridge.lightMetadata.empty() || bridge.materials.size() != bridge.lightMetadata.size()) return result;
	const int32_t actorIndex = bridge.lightMetadata.front().actorIndex;
	if (actorIndex < 0) return result; // Unknown/preload provenance remains private.
	for (const auto& metadata : bridge.lightMetadata)
	{
		if (metadata.actorIndex != actorIndex || metadata.actorOverlayRuleCount != 0 ||
			(metadata.sourceType != nri_scene::SurfaceSourceType::VoxelProxySprite &&
			 metadata.sourceType != nri_scene::SurfaceSourceType::FacingSprite)) return result;
	}
	if (!actorsReady) BuildActorClassEligibility();
	const auto actor = actors.find(actorIndex);
	result.invariant = actor != actors.end() && actor->second;
	return result;
}

void NRIVoxelMaterialSharingPolicy::Reset()
{
	world = 0; actorWorld = 0; actorLifecycle = 0; rules = 0; frame = UINT32_MAX; actorsReady = false; actors.clear();
	lifecycleChanges.Reset();
}
