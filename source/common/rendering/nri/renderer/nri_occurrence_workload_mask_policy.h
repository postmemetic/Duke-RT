#pragma once

#include "nri_scene_upload_identity.h"

#include <array>
#include <cstdint>
#include <vector>

namespace nri_scene
{
	struct GeometryData;
	struct MaterialData;
}

enum class NRIOccurrenceWorkloadMaskScope : uint8_t
{
	UploadSpan,
	CapturedGeometry,
	AmbiguousAggregate,
};

enum class NRIOccurrenceWorkloadCertificate : uint8_t
{
	NotApplicable,
	Certified,
	Empty,
	PrimitiveRange,
	MixedReflection,
	MaterialRange,
	MaterialReference,
	MissingNoShadow,
	AmbiguousOccurrence,
	Count,
};

struct NRIOccurrenceWorkloadMaskFacts
{
	bool enabled = false;
	// Every consumer sharing the workload bit must implement the same reject
	// predicate. Smoke's optional force-opaque paths do not satisfy that proof.
	bool shadowRemovalPermitted = false;
	bool giRemovalPermitted = false;
};

struct NRIOccurrenceWorkloadMaskDecision
{
	uint32_t requestedMask = 0;
	uint32_t publishedMask = 0;
	uint32_t certifiedRemovalMask = 0;
	uint32_t withheldMask = 0;
	uint32_t removedMask = 0;
	uint32_t primitiveCount = 0;
	NRIOccurrenceWorkloadCertificate reflection = NRIOccurrenceWorkloadCertificate::NotApplicable;
	NRIOccurrenceWorkloadCertificate surfaceLight = NRIOccurrenceWorkloadCertificate::NotApplicable;
};

struct NRIOccurrenceWorkloadMaskCounts
{
	uint32_t occurrences = 0;
	uint32_t reflectionCertified = 0;
	uint32_t surfaceLightCertified = 0;
	uint32_t certifiedPrimitives = 0;
	uint32_t appliedOccurrences = 0;
	uint32_t removedMainPrimitives = 0;
	uint32_t removedShadowPrimitives = 0;
	uint32_t removedGiPrimitives = 0;
	uint32_t certifiedMask = 0;
	uint32_t removedMask = 0;
	uint32_t withheldMask = 0;
	std::array<uint32_t, (size_t)NRIOccurrenceWorkloadCertificate::Count> reflectionOutcomes = {};
	std::array<uint32_t, (size_t)NRIOccurrenceWorkloadCertificate::Count> surfaceLightOutcomes = {};
};

constexpr size_t NRI_OCCURRENCE_MASK_CAPTURED_DOMAIN = (size_t)NRISceneBufferUploadDomain::Count;
constexpr size_t NRI_OCCURRENCE_MASK_AGGREGATE_DOMAIN = NRI_OCCURRENCE_MASK_CAPTURED_DOMAIN + 1u;
constexpr size_t NRI_OCCURRENCE_MASK_DOMAIN_COUNT = NRI_OCCURRENCE_MASK_AGGREGATE_DOMAIN + 1u;

struct NRIOccurrenceWorkloadMaskStats
{
	NRIOccurrenceWorkloadMaskFacts facts = {};
	NRIOccurrenceWorkloadMaskCounts total = {};
	std::array<NRIOccurrenceWorkloadMaskCounts, NRI_OCCURRENCE_MASK_DOMAIN_COUNT> domains = {};
};

// A monolithic live-overlay occurrence has upload-domain authority only when a
// single nonempty span covers exactly the geometry submitted to its BLAS.
// Duplicate, partial, or malformed spans must not lend it a domain certificate.
const NRISceneBufferUploadDomainSpan* FindNRIUniqueCoveringWorkloadMaskSpan(
	const std::vector<NRISceneBufferUploadDomainSpan>& spans,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t indexOffset,
	uint32_t indexCount);

NRIOccurrenceWorkloadMaskDecision EvaluateNRIOccurrenceWorkloadMaskPolicy(
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& finalMaterials,
	const NRISceneBufferUploadDomainSpan& span,
	NRIOccurrenceWorkloadMaskScope scope,
	uint32_t requestedMask,
	const NRIOccurrenceWorkloadMaskFacts& facts);

// Records a fresh whole-occurrence proof for both A/B legs. The toggle controls
// only mask publication; it does not change BLAS geometry or its retained key.
uint32_t ApplyNRIOccurrenceWorkloadMaskPolicy(
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& finalMaterials,
	const NRISceneBufferUploadDomainSpan& span,
	NRIOccurrenceWorkloadMaskScope scope,
	uint32_t requestedMask,
	const NRIOccurrenceWorkloadMaskFacts& facts,
	NRIOccurrenceWorkloadMaskStats& stats);
