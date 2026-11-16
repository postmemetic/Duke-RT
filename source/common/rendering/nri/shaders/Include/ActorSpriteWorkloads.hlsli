#ifndef RAZE_NRI_ACTOR_SPRITE_WORKLOADS_HLSLI
#define RAZE_NRI_ACTOR_SPRITE_WORKLOADS_HLSLI

#include "SceneShadowContracts.hlsli"

bool IsIndirectActorSprite(uint primitiveFlags)
{
	const uint required = PRIMITIVE_FLAG_ACTOR_SPRITE_FALLBACK | PRIMITIVE_FLAG_REFLECTION_ONLY;
	return (primitiveFlags & required) == required;
}

bool RejectIndirectActorSpriteWorkload(uint primitiveFlags, uint traceMask)
{
	// These carriers share a dynamic occurrence. Match the equivalent actor
	// voxel's Reflection|GI mask without changing unrelated reflection sprites.
	return IsIndirectActorSprite(primitiveFlags) &&
		(traceMask & (NRI_TLAS_MASK_REFLECTION | NRI_TLAS_MASK_GI)) == 0u;
}

bool RejectActorSpriteWorkload(uint primitiveFlags, uint traceMask)
{
	if (RejectIndirectActorSpriteWorkload(primitiveFlags, traceMask)) return true;
	const uint required = PRIMITIVE_FLAG_ACTOR_SPRITE_FALLBACK | PRIMITIVE_FLAG_ACTOR_SPRITE_PRIMARY_HIDDEN;
	const uint secondaryWorkloads = NRI_TLAS_MASK_SHADOW | NRI_TLAS_MASK_REFLECTION | NRI_TLAS_MASK_GI |
		NRI_TLAS_MASK_EMISSIVE | NRI_TLAS_MASK_DEBUG;
	// A workload union is admitted if any of its non-primary rays can see the
	// body. This flag never removes shadow, reflection, GI or debug visibility.
	return (primitiveFlags & required) == required && (traceMask & NRI_TLAS_MASK_MAIN) != 0u &&
		(traceMask & secondaryWorkloads) == 0u;
}

bool RejectScenePrimitiveWorkload(uint primitiveFlags, uint traceMask, bool allowReflectionOnlySurfaces)
{
	if (RejectActorSpriteWorkload(primitiveFlags, traceMask)) return true;
	if (IsIndirectActorSprite(primitiveFlags)) return false;
	return (primitiveFlags & PRIMITIVE_FLAG_REFLECTION_ONLY) != 0u && !allowReflectionOnlySurfaces;
}

#endif
