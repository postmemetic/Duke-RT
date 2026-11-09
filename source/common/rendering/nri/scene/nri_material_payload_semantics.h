#pragma once
#include "nri_material_bridge.h"

namespace nri_scene
{
// Hash is only a lookup accelerator. Equality checks every GPU, metadata,
// ordered texture and palette field except the separately owned actor index.
uint64_t HashMaterialPayload(const MaterialBridgeData& bridge, uint64_t* bytesHashed = nullptr);
struct MaterialPayloadHashWork
{
	// Actual hash-loop input: descriptor/row bytes plus either raw immutable
	// bytes or digest words AND any immutable bytes scanned for cold digests.
	uint64_t bytesHashed = 0;
	uint64_t immutableBytesScanned = 0, immutableBytesAvoided = 0;
	uint64_t digestHits = 0, digestMisses = 0;
};
// The aggregate hash changes with mode; never mix modes in a live lookup.
// Empty payloads use a constant and do not count as cache hits or misses.
uint64_t HashMaterialPayloadWithWork(const MaterialBridgeData& bridge, bool useDigests,
	MaterialPayloadHashWork& work);
bool MaterialPayloadsEquivalent(const MaterialBridgeData& left, const MaterialBridgeData& right);
}
