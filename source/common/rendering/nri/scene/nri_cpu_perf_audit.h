#pragma once

#include "nri_material_bridge.h"
#include "../../../engine/perf_audit.h"

// CPU vector storage only. Immutable texture/palette payloads are shared and
// deliberately excluded: copying their descriptors does not copy their pixels.
inline uint64_t NRIAuditMaterialBridgeBytes(const nri_scene::MaterialBridgeData& bridge, bool capacity = false)
{
	return (capacity ? bridge.materials.capacity() : bridge.materials.size()) * sizeof(nri_scene::MaterialData) +
		(capacity ? bridge.lightMetadata.capacity() : bridge.lightMetadata.size()) * sizeof(nri_scene::MaterialLightingMetadata) +
		(capacity ? bridge.textures.capacity() : bridge.textures.size()) * sizeof(nri_scene::TextureUpload);
}

inline uint64_t NRIAuditSurfaceBytes(const nri_scene::SurfaceRef& surface, bool capacity = false)
{
	return (capacity ? surface.vertices.capacity() : surface.vertices.size()) * sizeof(nri_scene::CapturedVertex) +
		(capacity ? surface.indices.capacity() : surface.indices.size()) * sizeof(uint32_t) +
		(capacity ? surface.primitiveLocalMaterialSlots.capacity() : surface.primitiveLocalMaterialSlots.size()) * sizeof(uint8_t);
}

inline void NRIAuditActorBridgeCopy(const nri_scene::MaterialBridgeData& source)
{
	if (!PerfAuditActive()) return;
	PerfAuditAdd(PerfAuditMetric::VoxelActorBridgeCopies);
	PerfAuditAdd(PerfAuditMetric::VoxelActorBridgeRows, source.materials.size());
	PerfAuditAdd(PerfAuditMetric::VoxelActorBridgeBytes, NRIAuditMaterialBridgeBytes(source));
}

inline void NRIAuditFrameBridgeCopy(const nri_scene::MaterialBridgeData& source)
{
	if (!PerfAuditActive()) return;
	PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeRowsCopied, source.materials.size());
	PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeBytesCopied, NRIAuditMaterialBridgeBytes(source));
}
