#pragma once

#include "../scene/nri_map_motion_correspondence.h"

// Issued only by the retained scene publication owner. The complete ordered
// view, including prevPosition and materialUvReactive, is immutable under this
// namespace/revision until the synchronous staging lease returns.
struct NRIMapMotionViewCertificate
{
	uint64_t contentSerial = 0;
	uint64_t publicationRevision = 0;
	uint32_t chunkIndex = UINT32_MAX;
	uint32_t viewSlot = UINT32_MAX;
	bool Valid() const { return contentSerial && publicationRevision && chunkIndex != UINT32_MAX && viewSlot != UINT32_MAX; }
	bool Equals(const NRIMapMotionViewCertificate& other) const
	{
		return contentSerial == other.contentSerial && publicationRevision == other.publicationRevision &&
			chunkIndex == other.chunkIndex && viewSlot == other.viewSlot;
	}
};

struct NRIMapMotionSurfaceCertificate
{
	NRIMapMotionViewCertificate view;
	uint32_t ordinal = 0; // walls followed by flats, within the complete view
	bool hasMotion = false;
};

// Exact consumer inputs, not producer stamps: one snapshot owns the original
// ordered corner inputs as well as the original builder's canonical output.
struct NRIMapMotionPayloadSnapshot
{
	nri_scene::PTMapTemporalSurfacePayload payload;
	std::vector<nri_scene::PTMapTemporalCorner> sourceCorners;
	bool valid = false;
	NRIMapMotionSurfaceCertificate certificate;

	bool Matches(const nri_scene::SurfaceRef& surface, uint64_t& comparedBytes) const;
	bool Build(const nri_scene::SurfaceRef& surface, nri_scene::MotionValidityReason& reason);
	size_t PayloadCapacityBytes() const;
	size_t InputCapacityBytes() const;
};

bool NRIMapMotionPayloadEqual(const nri_scene::PTMapTemporalSurfacePayload& a,
	const nri_scene::PTMapTemporalSurfacePayload& b);
bool NRIMapMotionPayloadExactEqual(const nri_scene::PTMapTemporalSurfacePayload& a,
	const nri_scene::PTMapTemporalSurfacePayload& b);
