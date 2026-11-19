#pragma once

struct ActorSpriteAnimationState;
struct TilesetBuildInfo;

BEGIN_DUKE_NS
class DDukeActor;
void ConfigureEnemyVoxelDirectionalStarts(const TilesetBuildInfo& info, bool worldTour);
int ResolveEnemyVoxelDirectionalStart(const DDukeActor& actor);
void CaptureEnemyVoxelFamily(const DDukeActor& actor, ActorSpriteAnimationState& animation);
void ResetEnemyVoxelFamilyContentCertificate();
void RecordEnemyVoxelFamilyConSource(const char* name, const char* bytes, unsigned length, bool root);
END_DUKE_NS
