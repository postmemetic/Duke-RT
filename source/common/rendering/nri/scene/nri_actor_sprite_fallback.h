#pragma once

#include "nri_scene_surface_types.h"

#include <cstdint>
#include <vector>

struct HWDrawInfo;
class HWSprite;

namespace nri_scene
{
// CPU presentation candidate, independent of voxel primitive-count/readiness.
// No actor/tsprite pointers escape capture. The renderer selects one candidate
// or a ready voxel before dynamic upload and final occurrence publication.
struct ActorSpriteFallbackCandidate
{
	uint64_t identityKey = 0;
	uint64_t ownerWorldEpoch = 0;
	uint64_t ownerLifetimeGeneration = 0;
	uint64_t placementGeneration = 0;
	uint64_t placementStateHash = 0;
	uint64_t capturedFrame = 0;
	uint64_t simulationGeneration = 0;
	int32_t actorIndex = -1;
	int32_t physicalSectorIndex = -1;
	bool capturedThisFrame = false;
	bool descriptorCurrent = false;
	bool animationCurrent = false;
	bool indirectOnly = false;
	bool transientVoxel = false;
	bool transientGeometryCaptured = false;
	SurfaceRef surface;
};

void BeginActorSpriteFallbackFrame(uint64_t frame);
bool CaptureActorSpriteFallbackCandidate(HWDrawInfo& di, const HWSprite& sprite,
	uint64_t identityKey, uint32_t drawListType, bool transientVoxel, bool indirectOnly);
void MarkActorSpriteFallbackTransientGeometry(uint64_t identityKey);
void DiscardRetainedActorSpriteFallbackCandidate(int32_t actorIndex);
void RefreshRetainedActorSpriteFallbackCandidates(HWDrawInfo& rootView);
const std::vector<ActorSpriteFallbackCandidate>& GetActorSpriteFallbackCandidates();
bool HasCurrentActorSpriteFallbackCandidate(uint64_t identityKey, uint64_t worldEpoch, uint64_t lifetimeGeneration);
void ResetActorSpriteFallbackCandidates();
}
