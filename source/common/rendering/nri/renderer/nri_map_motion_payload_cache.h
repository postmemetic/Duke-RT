#pragma once

#include "../scene/nri_map_motion_correspondence.h"

// Exact consumer inputs, not producer stamps: one snapshot owns the original
// ordered corner inputs as well as the original builder's canonical output.
struct NRIMapMotionPayloadSnapshot
{
	nri_scene::PTMapTemporalSurfacePayload payload;
	std::vector<nri_scene::PTMapTemporalCorner> sourceCorners;
	bool valid = false;

	bool Matches(const nri_scene::SurfaceRef& surface, uint64_t& comparedBytes) const;
	bool Build(const nri_scene::SurfaceRef& surface, nri_scene::MotionValidityReason& reason);
	size_t PayloadCapacityBytes() const;
	size_t InputCapacityBytes() const;
};

bool NRIMapMotionPayloadEqual(const nri_scene::PTMapTemporalSurfacePayload& a,
	const nri_scene::PTMapTemporalSurfacePayload& b);
bool NRIMapMotionPayloadExactEqual(const nri_scene::PTMapTemporalSurfacePayload& a,
	const nri_scene::PTMapTemporalSurfacePayload& b);
