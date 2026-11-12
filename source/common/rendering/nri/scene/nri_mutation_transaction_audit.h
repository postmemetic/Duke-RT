#pragma once

#include "nri_geometry_bridge.h"
#include "nri_map_builder.h"
#include "nri_material_bridge.h"
#include "nri_mutation_followup_audit.h"

// Diagnostic accounting only. Copy extents are constant-time vector row
// extents, not allocator traffic. Scene views exclude nested surface vectors;
// bridges exclude shared palette/pixel payloads. Extent bookkeeping occurs
// before the timed assignment so observer work is not reported as copy time.
namespace nri_scene::mutation_transaction_audit
{
namespace followup = mutation_followup_audit;

enum class CopyKind { View, Geometry, Material, Baseline, CacheHit, CacheStore };

inline uint64_t DescriptorBytes(const SceneView& view)
{
	return uint64_t(view.opaqueWalls.size() + view.opaqueFlats.size() + view.opaqueSprites.size()) * sizeof(SurfaceRef);
}
inline uint64_t DescriptorBytes(const MaterialBridgeData& bridge)
{
	return uint64_t(bridge.materials.size()) * sizeof(MaterialData) +
		uint64_t(bridge.lightMetadata.size()) * sizeof(MaterialLightingMetadata) +
		uint64_t(bridge.textures.size()) * sizeof(TextureUpload);
}
inline uint64_t DescriptorBytes(const GeometryData& geometry)
{
	return uint64_t(geometry.vertices.size()) * sizeof(SceneVertex) +
		uint64_t(geometry.indices.size()) * sizeof(uint32_t) +
		uint64_t(geometry.primitives.size()) * sizeof(PrimitiveData) +
		uint64_t(geometry.primitiveProvenance.size()) * sizeof(SurfaceProvenance);
}
inline uint64_t DescriptorBytes(const PTMapChunkMutationBaseline& baseline)
{
	return uint64_t(baseline.walls.size()) * sizeof(PTMapWallMutationSnapshot) +
		uint64_t(baseline.sectionIndices.size()) * sizeof(int32_t);
}

class CopyScope
{
	CopyKind mKind;
	uint64_t mStart = 0;
public:
	template<class Product> CopyScope(CopyKind kind, const Product& source) : mKind(kind)
	{
		if (followup::current == nullptr) return;
		using C = followup::Counter;
		static constexpr C counts[] = { C::MutationTransactionViewCopies, C::MutationTransactionGeometryCopies,
			C::MutationTransactionMaterialCopies, C::MutationTransactionBaselineCopies };
		static constexpr C bytes[] = { C::MutationTransactionViewDescriptorBytes, C::MutationTransactionGeometryBytes,
			C::MutationTransactionMaterialDescriptorBytes, C::MutationTransactionBaselineBytes,
			C::MutationMaterialCacheHitCopyDescriptorBytes, C::MutationMaterialCacheStoreCopyDescriptorBytes };
		const size_t index = size_t(kind);
		if (index < 4) followup::Add(counts[index]);
		followup::Add(bytes[index], DescriptorBytes(source));
		mStart = PerfAuditClockNs();
	}
	~CopyScope()
	{
		if (mStart == 0) return;
		static constexpr followup::Counter times[] = {
			followup::Counter::MutationTransactionViewNs, followup::Counter::MutationTransactionGeometryNs,
			followup::Counter::MutationTransactionMaterialNs, followup::Counter::MutationTransactionBaselineNs,
			followup::Counter::MutationMaterialCacheHitCopyNs, followup::Counter::MutationMaterialCacheStoreCopyNs };
		followup::Add(times[size_t(mKind)], PerfAuditClockNs() - mStart);
	}
	CopyScope(const CopyScope&) = delete;
	CopyScope& operator=(const CopyScope&) = delete;
};

class MaterialSignatureScope
{
	uint64_t mStart = 0;
public:
	MaterialSignatureScope()
	{
		if (followup::current == nullptr) return;
		followup::Add(followup::Counter::MutationTransactionMaterialSignatureCalls);
		mStart = PerfAuditClockNs();
	}
	~MaterialSignatureScope()
	{
		if (mStart != 0) followup::Add(followup::Counter::MutationTransactionMaterialSignatureNs, PerfAuditClockNs() - mStart);
	}
};

// Sampled storage census, not a lifetime high-water mark. Entry descriptors
// own their vectors, while palette/indexed payloads remain shared elsewhere.
template<class Chunks> void CensusMaterialCache(const Chunks& chunks)
{
	if (!PerfAuditCpuDetailActive() || !PerfAuditCensusRequested()) return;
	const uint64_t start = PerfAuditClockNs();
	uint64_t entries = 0, rows = 0, descriptorCapacity = 0, canonicalCapacity = 0;
	for (const auto& replacement : chunks)
	{
		descriptorCapacity += uint64_t(replacement.materialStateCache.capacity()) * sizeof(typename Chunks::value_type::MaterialStateCacheEntry);
		entries += replacement.materialStateCache.size();
		for (const auto& entry : replacement.materialStateCache)
		{
			const auto& bridge = entry.materialBridge;
			rows += bridge.materials.size();
			descriptorCapacity += uint64_t(bridge.materials.capacity()) * sizeof(MaterialData) +
				uint64_t(bridge.lightMetadata.capacity()) * sizeof(MaterialLightingMetadata) +
				uint64_t(bridge.textures.capacity()) * sizeof(TextureUpload);
			canonicalCapacity += uint64_t(entry.canonicalMaterialStateKeys.capacity()) * sizeof(uint64_t);
		}
	}
	PerfAuditSet(PerfAuditMetric::MutationMaterialCacheEntries, entries);
	PerfAuditSet(PerfAuditMetric::MutationMaterialCacheRows, rows);
	PerfAuditSet(PerfAuditMetric::MutationMaterialCacheDescriptorCapacityBytes, descriptorCapacity);
	PerfAuditSet(PerfAuditMetric::MutationMaterialCacheCanonicalKeyCapacityBytes, canonicalCapacity);
	PerfAuditAdd(PerfAuditMetric::MutationMaterialCacheCensusNs, PerfAuditClockNs() - start);
}
}
