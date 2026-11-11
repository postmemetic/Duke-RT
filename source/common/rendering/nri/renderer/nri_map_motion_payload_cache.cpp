#include "nri_map_motion_payload_cache.h"

#include <cstring>

namespace
{
	bool ProvenanceEqual(const nri_scene::SurfaceProvenance& a, const nri_scene::SurfaceProvenance& b)
	{
		return a.sourceType == b.sourceType && a.sectorIndex == b.sectorIndex &&
			a.wallIndex == b.wallIndex && a.sectionIndex == b.sectionIndex &&
			a.mapChunkIndex == b.mapChunkIndex && a.nextSectorIndex == b.nextSectorIndex &&
			a.actorIndex == b.actorIndex && a.drawListType == b.drawListType &&
			a.cstat == b.cstat && a.materialFlags == b.materialFlags &&
			a.actorOverlayRuleCount == b.actorOverlayRuleCount &&
			std::memcmp(a.actorOverlayRuleIds, b.actorOverlayRuleIds, sizeof(a.actorOverlayRuleIds)) == 0;
	}
}

bool NRIMapMotionPayloadEqual(const nri_scene::PTMapTemporalSurfacePayload& a,
	const nri_scene::PTMapTemporalSurfacePayload& b)
{
	if (a.occurrenceId != b.occurrenceId || a.topologyKey != b.topologyKey ||
		a.generation != b.generation || a.chunkIndex != b.chunkIndex ||
		a.materialUvAnchorValid != b.materialUvAnchorValid ||
		(a.materialUvAnchorValid &&
			(std::memcmp(a.materialUvAnchor, b.materialUvAnchor, sizeof(a.materialUvAnchor)) != 0 ||
				std::memcmp(a.materialUvTexelScale, b.materialUvTexelScale, sizeof(a.materialUvTexelScale)) != 0)) ||
		a.materialVerticalReferenceValid != b.materialVerticalReferenceValid ||
		(a.materialVerticalReferenceValid && std::memcmp(&a.materialVerticalReference,
			&b.materialVerticalReference, sizeof(a.materialVerticalReference)) != 0) ||
		a.corners.size() != b.corners.size()) return false;
	for (size_t i = 0; i < a.corners.size(); ++i)
		if (a.corners[i].key != b.corners[i].key ||
			std::memcmp(a.corners[i].position, b.corners[i].position, sizeof(a.corners[i].position)) != 0)
			return false;
	return true;
}

bool NRIMapMotionPayloadExactEqual(const nri_scene::PTMapTemporalSurfacePayload& a,
	const nri_scene::PTMapTemporalSurfacePayload& b)
{
	return NRIMapMotionPayloadEqual(a, b) && ProvenanceEqual(a.provenance, b.provenance) &&
		std::memcmp(&a.materialVerticalReference, &b.materialVerticalReference, sizeof(a.materialVerticalReference)) == 0 &&
		std::memcmp(a.materialUvAnchor, b.materialUvAnchor, sizeof(a.materialUvAnchor)) == 0 &&
		std::memcmp(a.materialUvTexelScale, b.materialUvTexelScale, sizeof(a.materialUvTexelScale)) == 0;
}

bool NRIMapMotionPayloadSnapshot::Matches(const nri_scene::SurfaceRef& surface, uint64_t& comparedBytes) const
{
	const auto& temporal = surface.temporal;
	if (!valid || !temporal.identityValid || payload.occurrenceId != temporal.occurrenceId ||
		payload.topologyKey != temporal.topologyKey || payload.generation != temporal.generation ||
		payload.materialVerticalReferenceValid != temporal.materialVerticalReferenceValid ||
		std::memcmp(&payload.materialVerticalReference, &temporal.materialVerticalReference,
			sizeof(payload.materialVerticalReference)) != 0 ||
		payload.materialUvAnchorValid != temporal.materialUvAnchorValid ||
		(temporal.materialUvAnchorValid &&
			(std::memcmp(payload.materialUvAnchor, temporal.materialUvAnchor, sizeof(payload.materialUvAnchor)) != 0 ||
				std::memcmp(payload.materialUvTexelScale, temporal.materialUvTexelScale, sizeof(payload.materialUvTexelScale)) != 0)) ||
		!ProvenanceEqual(payload.provenance, surface.provenance) || sourceCorners.size() != surface.vertices.size())
		return false;
	for (size_t i = 0; i < sourceCorners.size(); ++i)
	{
		comparedBytes += sizeof(sourceCorners[i].key) + sizeof(sourceCorners[i].position);
		if (sourceCorners[i].key != surface.vertices[i].temporalCornerKey ||
			std::memcmp(sourceCorners[i].position, surface.vertices[i].position, sizeof(sourceCorners[i].position)) != 0)
			return false;
	}
	return true;
}

bool NRIMapMotionPayloadSnapshot::Build(const nri_scene::SurfaceRef& surface, nri_scene::MotionValidityReason& reason)
{
	valid = false;
	certificate = {};
	if (!nri_scene::BuildMapTemporalSurfacePayload(surface, payload, reason)) return false;
	sourceCorners.resize(surface.vertices.size());
	for (size_t i = 0; i < surface.vertices.size(); ++i)
	{
		sourceCorners[i].key = surface.vertices[i].temporalCornerKey;
		std::memcpy(sourceCorners[i].position, surface.vertices[i].position, sizeof(sourceCorners[i].position));
	}
	valid = true;
	return true;
}

size_t NRIMapMotionPayloadSnapshot::PayloadCapacityBytes() const
{
	return payload.corners.capacity() * sizeof(nri_scene::PTMapTemporalCorner);
}

size_t NRIMapMotionPayloadSnapshot::InputCapacityBytes() const
{
	return sourceCorners.capacity() * sizeof(nri_scene::PTMapTemporalCorner);
}
