#include "nri_scene_lights.h"
#include "nri_cvars.h"
#include "coreactor.h"
#include "actor_lifecycle_journal.h"
#include "perf_audit.h"

void SceneLightSystem::RefreshLiveActorOverlayRules(bool hasActorRules, bool forceRebuild)
{
	PerfAuditScope timer(PerfAuditMetric::ActorLightRuleMaintenanceNs);
	PerfAuditAdd(PerfAuditMetric::ActorLightRuleCalls);
	auto& state = mCompiledOverlayRules.actorState;
	state.inputs.clear();
	uint64_t visits = 0;
	if (hasActorRules)
	{
		TSpriteIterator<DCoreActor> it;
		while (auto actor = it.Next())
		{
			++visits;
			if (!actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0 || actor->GetClass() == nullptr)
				continue;
			NRIActorLightInput input;
			input.actorIndex = (int32_t)actor->GetIndex();
			input.actorClass = actor->GetClass();
			input.palette = actor->spr.pal;
			input.position[0] = (float)actor->spr.pos.X;
			input.position[1] = (float)-actor->spr.pos.Z;
			input.position[2] = (float)-actor->spr.pos.Y;
			const FTextureID texture = actor->dispictex.isValid() ? actor->dispictex : actor->spr.spritetexture();
			input.textureId = texture.isValid() ? (uint32_t)texture.GetIndex() : 0u;
			state.inputs.push_back(input);
		}
	}
	const auto stats = UpdateNRIActorLightRules(state, GetActorLifecycleJournal().WorldEpoch(),
		forceRebuild || !nri_ptactorlightmaintenance, mCompiledOverlayRules.actorTemplates,
		mCompiledOverlayRules.liveActorRules,
		[](const NRIActorLightInput& actor, const CompiledOverlayRules::ActorTemplate& actorTemplate)
		{
			const auto actorClass = static_cast<const PClass*>(actor.actorClass);
			return actorClass == actorTemplate.actorClass || actorClass->IsDescendantOf(actorTemplate.actorClass);
		},
		[](const NRIActorLightInput& actor, AnalyticLightRegistry::ActorOverlayRule& rule)
		{
			rule.actorClassName = static_cast<const PClass*>(actor.actorClass)->TypeName.GetChars();
			rule.actorIndex = actor.actorIndex;
			rule.actorPalette = actor.palette;
			rule.actorTextureId = actor.textureId;
			std::copy_n(actor.position, 3, rule.actorPosition);
		});
	PerfAuditAdd(PerfAuditMetric::ActorLightRuleActorsVisited, visits);
	PerfAuditAdd(stats.reused ? PerfAuditMetric::ActorLightRuleReuses : PerfAuditMetric::ActorLightRuleRebuilds);
	PerfAuditAdd(PerfAuditMetric::ActorLightRuleRowsRefreshed, stats.refreshedRows);
	PerfAuditAdd(PerfAuditMetric::ActorLightRuleRowsCopied, stats.copiedRows);
	PerfAuditSet(PerfAuditMetric::ActorLightRuleScratchBytes, state.CapacityBytes());
}
