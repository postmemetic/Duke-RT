#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nri_scene { struct GeometryData; }

enum class NRIEmissiveGeometryDomain : uint32_t
{
	Static, Captured, RuntimeMutation, Dynamic, SurfaceLightOverlay, Count
};

struct NRIEmissiveMaterialPrimitiveRange
{
	uint32_t first = UINT32_MAX;
	uint32_t count = 0;
};

struct NRIEmissiveGeometryCopyRange
{
	uint32_t vertexOffset = 0;
	uint32_t vertexCount = 0;
	uint32_t primitiveOffset = 0;
	uint32_t primitiveCount = 0;
	uint32_t materialOffset = 0;
};

struct NRIEmissivePrimitiveGeometry
{
	float area = 0.0f;
	float center[3] = {};
	float radius = 0.0f;
	bool valid = false;
};

struct NRIEmissiveGeometryCopyChanges
{
	// Preserve the existing full fact/payload invalidation, including material
	// changes. Membership alone excludes positions and vertex connectivity.
	bool samplingGeometry = false;
	bool materialMembership = false;
};

struct NRIEmissiveGeometryCacheStats
{
	double identityMs = 0.0;
	double validationHashMs = 0.0;
	double topologyMs = 0.0;
	// Reuses existing fact clocks; no additional per-primitive timing.
	double primitiveFactMs = 0.0;
	double staticPrimitiveFactMs = 0.0;
	uint32_t staticPrimitiveBuilds = 0;
	uint32_t staticPrimitiveReuses = 0;
	uint32_t staticPrimitiveInvalidations = 0;
	uint64_t hashedVertices = 0;
	uint64_t hashedPrimitives = 0;
	uint64_t staticPrimitivesScanned = 0;
	uint64_t dynamicPrimitivesScanned = 0;
	uint32_t identityChecks = 0;
	uint32_t identityMismatches = 0;
	uint32_t quarantinedDomains = 0;
	uint32_t topologyBuilds = 0;
	uint32_t topologyReuses = 0;
	uint32_t primitiveBuilds = 0;
	uint32_t primitiveReuses = 0;
	uint32_t capacityGrowths = 0;
	uint64_t capacityGrowthBytes = 0;
	uint64_t snapshotComparedBytes = 0;
	uint64_t snapshotWrittenBytes = 0;
	uint32_t snapshotChecks = 0;
	uint32_t snapshotHits = 0;
	uint32_t snapshotPositionOnlyChanges = 0;
	uint32_t primitiveInvalidations = 0;
	uint32_t factResetReuseInvalidations = 0;
	uint32_t factResetReuseModeResets = 0;
	uint32_t factResetReuseWraps = 0;
	uint64_t factResetReuseSkippedRows = 0;
	uint64_t factResetReuseStampRowsWritten = 0;
	uint64_t factResetReuseStampCapacityBytes = 0;
	uint32_t factResetReuseValidationChecks = 0;
	uint32_t factResetReuseValidationMismatches = 0;
	uint32_t factResetReuseQuarantines = 0;
	double factResetReuseValidationMs = 0.0;
};

// Geometry facts only. Active lights and sector/material responses remain
// authoritative in SceneLightSystem and are evaluated for every payload build.
// Exactly one current topology is retained per domain; reset releases map data.
class NRIEmissiveGeometryCache
{
public:
	void Reset();
	void BeginUpdate(bool collectTiming);
	uint64_t ResolveIdentity(NRIEmissiveGeometryDomain domain,
		const nri_scene::GeometryData* geometry, uint64_t producerIdentity, bool validate,
		uint64_t staticMembershipIdentity = 0);
	const std::vector<NRIEmissiveMaterialPrimitiveRange>& Ranges(
		NRIEmissiveGeometryDomain domain, const nri_scene::GeometryData* geometry);
	const NRIEmissivePrimitiveGeometry& Primitive(NRIEmissiveGeometryDomain domain,
		const nri_scene::GeometryData& geometry, uint32_t primitiveIndex);
	const NRIEmissiveGeometryCacheStats& Stats() const { return mStats; }
	bool TimingEnabled() const { return mCollectTiming; }
	uint64_t CapacityBytes() const;
	uint64_t SnapshotCapacityBytes() const;
	void QuarantineAll();
	static uint64_t HashGeometry(const nri_scene::GeometryData* geometry);
	// Compare exactly the sampling fields written by a resident atlas copy.
	// Motion history, UVs, normals, flags and provenance do not affect sampling.
	static bool CopyChangesSamplingGeometry(const nri_scene::GeometryData& source,
		const NRIEmissiveGeometryCopyRange& sourceRange, const nri_scene::GeometryData& destination,
		const NRIEmissiveGeometryCopyRange& destinationRange, bool copyPrimitives);
	static NRIEmissiveGeometryCopyChanges ClassifyCopyChanges(const nri_scene::GeometryData& source,
		const NRIEmissiveGeometryCopyRange& sourceRange, const nri_scene::GeometryData& destination,
		const NRIEmissiveGeometryCopyRange& destinationRange, bool copyPrimitives);
	// For captured products without a reusable producer stamp. Each publication
	// is distinct even for nested/offscreen views in the same engine frame.
	static uint64_t PublishTransientIdentity();
	static void BuildRanges(const nri_scene::GeometryData* geometry,
		std::vector<NRIEmissiveMaterialPrimitiveRange>& outRanges);
	static NRIEmissivePrimitiveGeometry BuildPrimitive(
		const nri_scene::GeometryData& geometry, uint32_t primitiveIndex);

private:
	struct SamplingPosition { uint32_t bits[3] = {}; };
	struct SamplingPrimitive { uint32_t indices[3] = {}; uint32_t material = 0; };
	struct DomainState
	{
		uint64_t identity = 0;
		uint64_t rangesIdentity = 0;
		uint64_t primitiveIdentity = 0;
		uint64_t cachedPrimitiveIdentity = 0;
		uint64_t topologyIdentity = 0;
		size_t topologyPrimitiveCount = 0;
		bool topologyPresent = false;
		bool rangesMembershipIdentity = false;
		bool topologyMembershipIdentity = false;
		bool rangesUncertified = false;
		uint64_t validatedIdentity = 0;
		uint64_t validatedHash = 0;
		bool topologyValid = false;
		bool validationValid = false;
		bool quarantined = false;
		bool snapshotValid = false;
		bool snapshotPresent = false;
		uint64_t snapshotIdentity = 0;
		uint64_t snapshotRangesIdentity = 0;
		uint64_t snapshotPrimitiveIdentity = 0;
		std::vector<SamplingPosition> positions;
		std::vector<SamplingPrimitive> topology;
		std::vector<NRIEmissiveMaterialPrimitiveRange> ranges;
		std::vector<NRIEmissivePrimitiveGeometry> primitives;
		// A generation is a full-domain invalidation, not a per-primitive lifetime
		// certificate. Zero stamps are uncomputed, including after growth/reset.
		std::vector<uint32_t> primitiveStamps;
		uint32_t primitiveGeneration = 0;
		bool factResetReuse = false;
		bool validateFacts = false;
		bool factResetQuarantined = false;
	};
	uint64_t ResolveSnapshot(DomainState& state, const nri_scene::GeometryData* geometry);
	void AdvancePrimitiveGeneration(DomainState& state);
	std::array<DomainState, (size_t)NRIEmissiveGeometryDomain::Count> mDomains = {};
	NRIEmissiveGeometryCacheStats mStats = {};
	bool mCollectTiming = false;
};
