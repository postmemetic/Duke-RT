#pragma once

#include "nri_scene_surface_types.h"
#include <cstdint>

class FVoxelModel;

namespace nri_scene
{
enum class VoxelMeshBakeSpace : uint8_t
{
	Unknown = 0,
	LocalSpace,
	BakedTransform,
};

struct PersistentVoxelCacheEntryView
{
	uint64_t identityKey = 0;
	uint64_t ownerWorldEpoch = 0;
	uint64_t ownerLifetimeGeneration = 0;
	uint64_t placementGeneration = 0;
	uint64_t placementStateHash = 0;
	uint64_t signature = 0;
	uint64_t geometrySignature = 0;
	uint64_t surfaceSignature = 0;
	uint64_t bakedSurfaceSignature = 0;
	uint64_t materialSignature = 0;
	uint64_t transformBasisSignature = 0;
	uint64_t meshKeyHash = 0;
	uint64_t materialKeyHash = 0;
	uint64_t geometryContentHash = 0;
	uint64_t renderPrimitiveHash = 0;
	uint64_t meshVariantHash = 0;
	uint64_t materialVariantHash = 0;
	VoxelMeshBakeSpace meshBakeSpace = VoxelMeshBakeSpace::Unknown;
	int32_t actorIndex = -1;
	int32_t physicalSectorIndex = -1;
	int32_t sourcePicnum = -1;
	int32_t resolvedVoxelIndex = -1;
	uint32_t primitiveCount = 0;
	uint64_t lastSeenFrame = 0;
	uint64_t retainedFrameAge = 0;
	bool capturedThisFrame = false;
	bool indirectOnly = false;
	bool authorityCurrent = false;
	bool publicationEligible = false;
	bool pendingRemoval = false;
	float instanceTransform[12] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
	float currentTranslation[3] = {};
	float bakedTranslation[3] = {};
	FVoxelModel* model = nullptr;
	const SurfaceRef* surface = nullptr;
	const SurfaceRef* lightSurface = nullptr;
	SurfaceRef materialSurface;
	bool sharedVariantSurface = false;
	bool desiredPending = false;
	bool directOnlyAdmission = false;
};
}
