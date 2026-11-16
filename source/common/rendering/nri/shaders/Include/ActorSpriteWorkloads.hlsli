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

bool RejectReflectionOnlyWorkload(uint primitiveFlags, uint traceMask, bool allowReflectionOnlySurfaces)
{
	if (IsIndirectActorSprite(primitiveFlags))
		return RejectIndirectActorSpriteWorkload(primitiveFlags, traceMask);
	return (primitiveFlags & PRIMITIVE_FLAG_REFLECTION_ONLY) != 0u && !allowReflectionOnlySurfaces;
}

#endif
