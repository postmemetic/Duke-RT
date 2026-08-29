#pragma once

#include "nri_persistent_voxel_material_closure.h"
#include "nri_persistent_voxels.h"

class NRIRenderer;

struct NRIPersistentVoxelMaterialClosureTelemetry
{
	uint64_t buildSerial = 0;
	uint32_t preloadRuleClassifications = 0;
	uint32_t preloadSeeds = 0;
	uint32_t preloadSeedsReady = 0;
	uint32_t preloadSeedsDeferred = 0;
	uint32_t preloadSeedsFailed = 0;
	uint32_t preloadActorSensitiveSeeds = 0;
	uint32_t preloadSeedReuses = 0;
	uint32_t runtimeSeedReuses = 0;
	uint32_t runtimeBroadMaterialBuilds = 0;
	uint32_t materialPresentationAttempts = 0;
	uint32_t materialPresentationHits = 0;
	uint32_t materialPresentationMisses = 0;
	uint32_t materialPresentationFailOpen = 0;
	uint32_t materialPresentationRows = 0;
	uint32_t textureRequests = 0;
	uint32_t textureRealizations = 0;
	uint32_t textureReuses = 0;
	uint32_t textureDeferred = 0;
	uint32_t textureFailures = 0;
	uint64_t textureRealizedBytes = 0;
	double textureRealizeMs = 0.0;
	NRIPersistentVoxelMaterialClosureRegistryStats registry = {};
};

NRIPersistentVoxelResetServices BuildNRIPersistentVoxelResetServices(NRIRenderer& renderer);
NRIPersistentVoxelAdmissionServices BuildNRIPersistentVoxelAdmissionServices(NRIRenderer& renderer);
NRIPersistentVoxelAccelerationServices BuildNRIPersistentVoxelAccelerationServices(NRIRenderer& renderer);

void ResetNRIPersistentVoxelMaterialClosure(NRIRenderer& renderer, uint64_t buildSerial);
bool RegisterNRIPersistentVoxelMaterialClosure(
	NRIRenderer& renderer,
	uint64_t buildSerial,
	uint64_t materialKey,
	uint64_t validatedSignature,
	const nri_scene::MaterialBridgeData& materials,
	NRIPersistentVoxelMaterialClosureSource source,
	NRIPersistentVoxelMaterialClosureResult& outResult);
bool TryReuseNRIPersistentVoxelMaterialClosure(
	NRIRenderer& renderer,
	uint64_t buildSerial,
	uint64_t materialKey,
	uint64_t validatedSignature,
	NRIPersistentVoxelMaterialClosureSource source,
	nri_scene::MaterialBridgeData& outMaterials,
	NRIPersistentVoxelMaterialClosureResult& outResult);
NRIPersistentVoxelMaterialClosureTelemetry GetNRIPersistentVoxelMaterialClosureTelemetry(NRIRenderer& renderer);
