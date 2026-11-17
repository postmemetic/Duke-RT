#pragma once

struct ActorSpriteAnimationState;

BEGIN_DUKE_NS
class DDukeActor;
void CaptureEnemyVoxelFamily(const DDukeActor& actor, ActorSpriteAnimationState& animation);
void ResetEnemyVoxelFamilyContentCertificate();
void RecordEnemyVoxelFamilyConSource(const char* name, const char* bytes, unsigned length, bool root);
END_DUKE_NS
