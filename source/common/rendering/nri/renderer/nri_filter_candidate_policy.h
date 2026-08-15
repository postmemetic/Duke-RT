#pragma once

#include <cstdint>

namespace nri_scene
{
	struct GeometryData;
}

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
