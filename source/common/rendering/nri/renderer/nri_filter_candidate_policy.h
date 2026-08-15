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

enum class NRIFilterCandidateCertificateOutcome : uint32_t
{
	Certified = 0,
	Empty,
	OutOfBounds,
	MixedPolicy,
};

struct NRIFilterCandidateCertificate
{
	NRIFilterCandidateCertificateOutcome outcome = NRIFilterCandidateCertificateOutcome::Empty;
	uint32_t primitiveCount = 0;

	bool IsCertified() const { return outcome == NRIFilterCandidateCertificateOutcome::Certified; }
};

// Certifies the exact primitive span referenced by one BLAS geometry. The
// certificate is deliberately whole-span: an instance flag affects every
// triangle in that BLAS, so a partial or mixed-policy result must fail open.
NRIFilterCandidateCertificate ClassifyReflectionOnlyFilterCandidateSpan(
	const nri_scene::GeometryData& geometry,
	uint32_t primitiveOffset,
	uint32_t primitiveCount);

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
