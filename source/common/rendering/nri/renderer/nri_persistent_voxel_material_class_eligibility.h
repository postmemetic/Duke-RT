#pragma once

#include <cstdint>

// The material matchers skip unresolved rule classes in this immutable resolved
// generation. A later resolution rebuild changes the generation and recertifies
// bindings before reuse. Unknown live actor classes cannot establish invariance.
template<class ActorClass, class RuleSet, class VisitRule>
bool NRIVoxelMaterialClassIsInvariant(ActorClass* actorClass, const RuleSet& resolved,
	uint32_t expectedGeneration, VisitRule&& visitRule)
{
	if (!actorClass || resolved.resolvedGeneration != expectedGeneration) return false;
	auto canMatch = [&](const auto& rule)
	{
		visitRule();
		return rule.actorClassResolved && rule.actorClass &&
			(actorClass == rule.actorClass || actorClass->IsDescendantOf(rule.actorClass));
	};
	// Keep tile/owner/style/activation predicates conservative: any class that
	// could match later in this generation remains private.
	for (const auto& rule : resolved.actorRules) if (canMatch(rule)) return false;
	for (const auto& rule : resolved.actorOverrideRules) if (canMatch(rule)) return false;
	for (const auto& rule : resolved.smokeActorRules)
		if (rule.emitterForeground && canMatch(rule)) return false;
	return true;
}
