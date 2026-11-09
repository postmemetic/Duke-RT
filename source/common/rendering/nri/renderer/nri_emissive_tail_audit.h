#pragma once

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "perf_audit.h"

// Logical CPU copy volume and retained vector capacity, not allocator totals.
// Shared immutable texture/palette/policy payloads are not charged repeatedly.
inline void NRIAuditEmissiveTailViewCopy(const nri_scene::SceneView& view)
{
	if (!PerfAuditActive()) return;
	uint64_t surfaces = 0, vertexBytes = 0, indexBytes = 0, slotBytes = 0;
	const auto count = [&](const auto& list)
	{
		surfaces += list.size();
		for (const auto& surface : list)
		{
			vertexBytes += surface.vertices.size() * sizeof(nri_scene::CapturedVertex);
			indexBytes += surface.indices.size() * sizeof(uint32_t);
			slotBytes += surface.primitiveLocalMaterialSlots.size() * sizeof(uint8_t);
		}
	};
	count(view.opaqueWalls); count(view.opaqueFlats); count(view.opaqueSprites);
	PerfAuditAdd(PerfAuditMetric::EmissiveTailViewCopies);
	PerfAuditAdd(PerfAuditMetric::EmissiveTailCopiedSurfaces, surfaces);
	PerfAuditAdd(PerfAuditMetric::EmissiveTailCopiedSurfaceBytes, surfaces * sizeof(nri_scene::SurfaceRef));
	PerfAuditAdd(PerfAuditMetric::EmissiveTailCopiedVertexBytes, vertexBytes);
	PerfAuditAdd(PerfAuditMetric::EmissiveTailCopiedIndexBytes, indexBytes);
	PerfAuditAdd(PerfAuditMetric::EmissiveTailCopiedSlotBytes, slotBytes);
}

inline void NRIAuditEmissiveTailBuildProduct(const nri_scene::GeometryData& geometry,
	const nri_scene::MaterialBridgeData& materials)
{
	if (!PerfAuditActive()) return;
	PerfAuditAdd(PerfAuditMetric::EmissiveTailBuiltVertices, geometry.vertices.size());
	PerfAuditAdd(PerfAuditMetric::EmissiveTailBuiltPrimitives, geometry.primitives.size());
	PerfAuditAdd(PerfAuditMetric::EmissiveTailBuiltMaterialRows, materials.materials.size());
	PerfAuditSet(PerfAuditMetric::EmissiveTailGeometryCapacityBytes,
		geometry.vertices.capacity() * sizeof(nri_scene::SceneVertex) + geometry.indices.capacity() * sizeof(uint32_t) +
		geometry.primitives.capacity() * sizeof(nri_scene::PrimitiveData) +
		geometry.primitiveProvenance.capacity() * sizeof(nri_scene::SurfaceProvenance));
	PerfAuditSet(PerfAuditMetric::EmissiveTailMaterialCapacityBytes,
		materials.materials.capacity() * sizeof(nri_scene::MaterialData) +
		materials.lightMetadata.capacity() * sizeof(nri_scene::MaterialLightingMetadata) +
		materials.textures.capacity() * sizeof(nri_scene::TextureUpload));
}
