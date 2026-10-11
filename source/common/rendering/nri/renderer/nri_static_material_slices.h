#pragma once

#include "../scene/nri_material_bridge.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

struct NRIStaticAnimatedMaterialStats
{
	uint32_t candidatesVisited = 0;
	uint32_t eligibleChunks = 0;
	uint32_t signaturesChecked = 0;
	uint32_t unchangedChunks = 0;
	uint32_t chunksCloned = 0;
	uint32_t chunksPatched = 0;
	uint32_t rowsPatched = 0;
	uint32_t fullRebuilds = 0;
	uint32_t candidateChecks = 0;
	uint32_t bindingChecks = 0;
	uint32_t bridgeChecks = 0;
	uint32_t mismatches = 0;
};

// One current map publication, never a history of animated states. The list is
// sorted by canonical static chunk index; suppressed chunks retain membership
// so their later admission does not depend on a visibility observation.
struct NRIStaticAnimatedMaterialState
{
	std::vector<uint32_t> candidates;
	std::vector<uint32_t> changedChunks;
	std::vector<uint32_t> referenceCandidates;
	std::vector<FGameTexture*> resolvedBindings;
	std::unordered_map<uint64_t, uint32_t> textureSlots;
	nri_scene::MaterialBridgeData remappedScratch;
	uint64_t canonicalMaterialGeneration = UINT64_MAX;
	uint64_t canonicalTopologyRevision = 0;
	bool canonicalLayoutValid = false;
	bool aggregateSkyPreserved = false;
	bool quarantined = false;
	uint32_t traceRows = 0;
	NRIStaticAnimatedMaterialStats lastFrame;
};

namespace nri_static_material_slices
{
void UpdateCandidate(std::vector<uint32_t>& candidates, uint32_t chunkListIndex, bool enabled);
// Equal row counts are insufficient: preserve the complete first-use traversal
// of texture references, payload identities and palette ownership as well.
bool LayoutCompatible(const nri_scene::MaterialBridgeData& previous, const nri_scene::MaterialBridgeData& current);
bool BridgeEqual(const nri_scene::MaterialBridgeData& lhs, const nri_scene::MaterialBridgeData& rhs);
bool Patch(
	const nri_scene::MaterialBridgeData& previous,
	const nri_scene::MaterialBridgeData& current,
	uint32_t offset,
	uint32_t count,
	nri_scene::MaterialBridgeData& destination,
	const std::unordered_map<uint64_t, uint32_t>& textureSlots,
	nri_scene::MaterialBridgeData& scratch);
}
