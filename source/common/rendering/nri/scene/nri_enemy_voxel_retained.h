#pragma once

#include <cstdint>

class HWSprite;
struct HWDrawInfo;
struct ActorPresentationState;

namespace nri_scene
{
// Feedback only: the renderer remains the sole representation decision owner.
void BeginEnemyVoxelSourceFrame();
void CaptureEnemyVoxelSource(HWDrawInfo&, const HWSprite&, const ActorPresentationState&, uint64_t captureFrame);
bool HasEnemyVoxelSource(const ActorPresentationState&);
void SetEnemyVoxelSourceMode(uint64_t world, uint64_t lifetime, bool voxel);
void ResetEnemyVoxelSources();
using EnemyVoxelSourceSink = bool (*)(const ActorPresentationState&, HWSprite&);
void RefreshEnemyVoxelSources(HWDrawInfo&, uint64_t captureFrame, EnemyVoxelSourceSink);
}
