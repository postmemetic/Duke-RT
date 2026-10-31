#pragma once

#include <cstdint>
#include <vector>

namespace nri_scene
{
	struct GeometryData;
	struct MaterialData;
}

enum NRIFilterCandidatePolicyBits : uint32_t
{
	NRIFilterCandidatePolicy_None = 0,
	NRIFilterCandidatePolicy_ReflectionOnly = 1u << 0,
	NRIFilterCandidatePolicy_OneWay = 1u << 1,
	NRIFilterCandidatePolicy_NoShadow = 1u << 2,
	NRIFilterCandidatePolicy_Alpha = 1u << 3,
	NRIFilterCandidatePolicy_Visibility = 1u << 4,
};

struct NRIFilterCandidateRun
{
	uint32_t primitiveOffset = 0;
	uint32_t primitiveCount = 0;
	uint32_t indexOffset = 0;
	uint32_t indexCount = 0;
	uint32_t policyMask = NRIFilterCandidatePolicy_None;

	bool RequiresCandidateTraversal() const { return policyMask != NRIFilterCandidatePolicy_None; }
};

// Validates an entire primitive span and returns the union of enabled candidate
// policies. A mixed opaque/candidate span can use one non-opaque instance: the
// shader accepts ordinary triangles and applies each candidate's own predicate.
// Empty/ordinary spans return true with no policy. Invalid primitive/material
// references return false with no policy, preserving the legacy opaque route.
bool GetFilterCandidatePolicyForSpan(
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t enabledPolicyMask,
	uint32_t& outPolicyMask);

// Splits one triangle-list upload span into exact, adjacent policy runs. The
// primitive order and GPU buffer layout stay unchanged; callers may build one
// single-geometry BLAS per run and retain primitiveBase lookup semantics.
// Invalid material/range data fails closed to the legacy opaque route.
bool BuildFilterCandidateRuns(
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t enabledPolicyMask,
	uint32_t maxRuns,
	std::vector<NRIFilterCandidateRun>& outRuns);
