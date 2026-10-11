#pragma once

#include "nri_map_world.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nri_scene
{
struct PTMapTemporalCorner
{
	uint64_t key = UINT64_MAX;
	float position[3] = {};
};

struct PTMapTemporalSurfacePayload
{
	uint64_t occurrenceId = 0;
	uint64_t topologyKey = 0;
	uint32_t generation = 0;
	uint32_t chunkIndex = UINT32_MAX;
	float materialVerticalReference = 0.0f;
	bool materialVerticalReferenceValid = false;
	float materialUvAnchor[2] = {};
	float materialUvTexelScale[2] = {};
	bool materialUvAnchorValid = false;
	SurfaceProvenance provenance;
	std::vector<PTMapTemporalCorner> corners;
};

void InitializeMapTemporalSurface(
	const PTMapSurface& mapSurface,
	uint64_t mapEpoch,
	SurfaceRef& surface);

bool BuildMapTemporalSurfacePayload(
	const SurfaceRef& surface,
	PTMapTemporalSurfacePayload& outPayload,
	MotionValidityReason& outReason);

bool ApplyMapTemporalSurfacePayload(
	const PTMapTemporalSurfacePayload& previous,
	SurfaceRef& current,
	MotionValidityReason& outReason);

// A material-only descriptor refresh cannot publish changed flat UV anchors
// or their temporal rejection flags; those updates need the primitive lane.
bool RequiresMapMaterialUvUpload(const SceneView& resident, const SceneView& current);

bool RunNRIMapMotionCorrespondenceSelfTests(std::string* failureReason = nullptr);
}
