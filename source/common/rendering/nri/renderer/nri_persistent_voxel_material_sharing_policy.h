#pragma once

#include "nri_persistent_voxel_material_payload.h"
#include "nri_persistent_voxel_material_lifecycle.h"

// Live actor lookup is built lazily, at most once per presentation that actually
// creates/recertifies a material binding. It never stores live actor pointers.
class NRIVoxelMaterialSharingPolicy
{
public:
	bool BeginFrame(uint64_t worldBuildSerial, uint32_t frameIndex);
	NRIVoxelMaterialSharingCertificate Certify(const nri_scene::MaterialBridgeData& bridge);
	void Reset();
	uint32_t RulesGeneration() const { return rules; }
	const std::vector<int32_t>& ChangedActors() const { return lifecycleChanges.ChangedActors(); }
private:
	void BuildActorClassEligibility();
	uint64_t world = 0;
	uint64_t actorWorld = 0;
	uint64_t actorLifecycle = 0;
	uint32_t frame = UINT32_MAX, rules = 0;
	bool actorsReady = false;
	std::unordered_map<int32_t, bool> actors;
	NRIVoxelMaterialLifecycle lifecycleChanges;
};
