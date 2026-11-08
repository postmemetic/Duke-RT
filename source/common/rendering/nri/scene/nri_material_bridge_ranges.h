#pragma once

#include "nri_material_bridge.h"

namespace nri_scene
{
struct MaterialRowRange
{
	uint32_t first = 0;
	uint32_t count = 0;
};

// A single publication transition. A consumer must match both its source
// generation and row count; missing transitions require a full reconstruction.
struct MaterialBridgeChanges
{
	uint64_t beforeGeneration = 0;
	uint64_t generation = 0;
	uint32_t beforeRows = 0;
	uint32_t afterRows = 0;
	bool fullRebuild = true;
	std::vector<MaterialRowRange> rows;
};

void MergeMaterialRowRanges(std::vector<MaterialRowRange>& ranges, uint32_t rowCount);
// GPU bytes are exact; metadata compares semantic fields, never struct padding.
bool MaterialBridgesEquivalent(const MaterialBridgeData& a, const MaterialBridgeData& b);

// Uses the same first-reference order as AppendMaterialBridge, including
// metadata-only texture references. No row storage is copied.
void CollectMaterialBridgeTextureLayout(
	const MaterialBridgeData& source,
	MaterialBridgeData& destination,
	std::unordered_map<uint64_t, uint32_t>& textureLookup);

// The caller supplies aligned row/metadata spans and a complete destination
// texture-key mapping. Unmapped source indices retain AppendMaterialBridge's
// invalid-index behavior. Returns false without writing on a malformed span.
bool CopyMaterialBridgeRowRange(
	const MaterialBridgeData& source,
	MaterialRowRange range,
	MaterialBridgeData& destination,
	uint32_t destinationFirst,
	const std::unordered_map<uint64_t, uint32_t>& textureLookup);
}
