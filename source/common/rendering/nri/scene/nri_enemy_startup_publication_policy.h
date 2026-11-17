#pragma once

#include "actor_presentation_snapshot.h"

#include <algorithm>

namespace nri_scene
{
// Startup publication may consume only an actual capture of this owner in the
// current frame. This is source authority, never a claim of GPU readiness.
struct EnemyStartupPublicationSource
{
	uint64_t world = 0, lifetime = 0;
	uint64_t placementGeneration = 0, placementStateHash = 0;
	uint64_t capturedFrame = 0;
	int32_t actorIndex = -1, textureId = -1;
};

inline bool IsCurrentCertifiedEnemyStartupPublication(
	const EnemyStartupPublicationSource& source,
	uint64_t frame,
	const ActorPresentationState* authority)
{
	if (!authority || frame == 0 || source.capturedFrame != frame ||
		source.world == 0 ||
		source.placementGeneration == 0 || source.placementStateHash == 0 ||
		!authority->authorityCurrent || !authority->publicationEligible ||
		authority->owner.ownerWorldEpoch != source.world ||
		authority->owner.ownerLifetimeGeneration != int64_t(source.lifetime) ||
		authority->actorIndex != source.actorIndex || source.actorIndex < 0 ||
		authority->placementGeneration != source.placementGeneration ||
		authority->placementStateHash != source.placementStateHash)
		return false;
	const auto& animation = authority->spriteAnimation;
	const auto& family = animation.enemyVoxelFamily;
	return animation.available && animation.enemy && family && family->complete &&
		family->signature != 0 && !family->textureIds.empty() && family->textureIds.size() <= 64 &&
		std::find(family->textureIds.begin(), family->textureIds.end(), source.textureId) != family->textureIds.end();
}
}
