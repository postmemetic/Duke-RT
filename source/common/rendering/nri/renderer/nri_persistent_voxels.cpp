#include "nri_persistent_voxels.h"
#include "nri_persistent_voxel_geometry_arena_policy.h"
#include "nri_persistent_voxel_pressure_policy.h"
#include "nri_scene_instance_visibility.h"

#include "../scene/nri_hash.h"
#include "nri_cvars.h"
#include "nri_ray_scene_builder.h"
#include "nri_shader_contracts.h"
#include "nri_upload_hash.h"
#include "nri_voxel_compute_meshing.h"
#include "nri_voxel_compute_preload.h"
#include "printf.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "stats.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <limits>

namespace
{
	bool IsPersistentVoxelCacheEntryPublicationCurrent(
		const nri_scene::PersistentVoxelCacheEntryView& entry)
	{
		return entry.ownerWorldEpoch != 0 &&
			entry.placementGeneration != 0 &&
			entry.placementStateHash != 0 &&
			entry.physicalSectorIndex >= 0 &&
			entry.authorityCurrent &&
			entry.publicationEligible &&
			!entry.pendingRemoval;
	}

	bool IsPersistentVoxelActorPublicationCurrent(
		const PersistentVoxelBatch::ActorEntry& actor)
	{
		return actor.ownerWorldEpoch != 0 &&
			actor.placementGeneration != 0 &&
			actor.placementStateHash != 0 &&
			actor.physicalSectorIndex >= 0 &&
			actor.authorityCurrent &&
			actor.publicationEligible &&
			!actor.pendingRemoval;
	}

	void CopyPersistentVoxelActorAuthority(
		const nri_scene::PersistentVoxelCacheEntryView& source,
		PersistentVoxelBatch::ActorEntry& target)
	{
		target.ownerWorldEpoch = source.ownerWorldEpoch;
		target.ownerLifetimeGeneration = source.ownerLifetimeGeneration;
		target.placementGeneration = source.placementGeneration;
		target.placementStateHash = source.placementStateHash;
		target.physicalSectorIndex = source.physicalSectorIndex;
		target.authorityCurrent = source.authorityCurrent;
		target.publicationEligible = source.publicationEligible;
		target.pendingRemoval = source.pendingRemoval;
	}

	void CopyPersistentVoxelInstanceAuthority(
		const nri_scene::PersistentVoxelCacheEntryView& source,
		PersistentVoxelInstanceRecord& target)
	{
		target.ownerWorldEpoch = source.ownerWorldEpoch;
		target.ownerLifetimeGeneration = source.ownerLifetimeGeneration;
		target.placementGeneration = source.placementGeneration;
		target.placementStateHash = source.placementStateHash;
		target.physicalSectorIndex = source.physicalSectorIndex;
		target.authorityCurrent = source.authorityCurrent;
		target.publicationEligible = source.publicationEligible;
		target.pendingRemoval = source.pendingRemoval;
	}

	uint64_t BuildPersistentVoxelActorBindingGeneration(
		const PersistentVoxelBatch::ActorEntry& actor)
	{
		uint64_t hash = nri_scene::HashCombine64(actor.meshResourceKey, actor.materialKeyHash);
		hash = nri_scene::HashCombine64(hash, actor.geometrySignature);
		hash = nri_scene::HashCombine64(hash, ((uint64_t)actor.primitiveOffset << 32u) | actor.primitiveCount);
		hash = nri_scene::HashCombine64(hash, ((uint64_t)actor.indexOffset << 32u) | actor.indexCount);
		hash = nri_scene::HashCombine64(hash, ((uint64_t)actor.materialOffset << 32u) | actor.materialCount);
		hash = nri_scene::HashCombine64(hash, actor.materialSlotGeneration);
		return hash != 0 ? hash : 1u;
	}

	void TransformPersistentVoxelLightCenter(
		const std::array<float, 12>& transform,
		const float source[3],
		float target[3])
	{
		target[0] = transform[0] * source[0] + transform[1] * source[1] + transform[2] * source[2] + transform[3];
		target[1] = transform[4] * source[0] + transform[5] * source[1] + transform[6] * source[2] + transform[7];
		target[2] = transform[8] * source[0] + transform[9] * source[1] + transform[10] * source[2] + transform[11];
	}

	float ResolvePersistentVoxelLightScale(const std::array<float, 12>& transform)
	{
		const float sx = std::sqrt(transform[0] * transform[0] + transform[4] * transform[4] + transform[8] * transform[8]);
		const float sy = std::sqrt(transform[1] * transform[1] + transform[5] * transform[5] + transform[9] * transform[9]);
		const float sz = std::sqrt(transform[2] * transform[2] + transform[6] * transform[6] + transform[10] * transform[10]);
		return std::max(0.0f, std::max(sx, std::max(sy, sz)));
	}

	std::unordered_set<uint64_t> CollectActivePersistentVoxelMaterialKeys(const PersistentVoxelBatch& batch)
	{
		std::unordered_set<uint64_t> keys;
		keys.reserve(batch.actors.size());
		for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
		{
			if (actor.active && IsPersistentVoxelActorPublicationCurrent(actor) && actor.materialKeyHash != 0)
			{
				keys.insert(actor.materialKeyHash);
			}
		}
		return keys;
	}

	template<typename Services>
	void RetirePersistentVoxelShadowProxy(
		NRIVoxelShadowProxyResource& proxy,
		const Services& services)
	{
		services.RetireBuffer(proxy.vertexBuffer);
		services.RetireBuffer(proxy.indexBuffer);
		services.RetireAccelerationStructure(proxy.accelerationStructure);
		proxy = {};
	}
}

const char* GetPersistentVoxelBakeSpaceName(nri_scene::VoxelMeshBakeSpace bakeSpace)
{
	switch (bakeSpace)
	{
	case nri_scene::VoxelMeshBakeSpace::LocalSpace: return "local";
	case nri_scene::VoxelMeshBakeSpace::BakedTransform: return "baked";
	default: return "unknown";
	}
}

void CopyPersistentVoxelInstanceTransform(const float source[12], std::array<float, 12>& target)
{
	for (size_t i = 0; i < target.size(); ++i)
	{
		target[i] = source[i];
	}
}

bool SamePersistentVoxelInstanceTransform(const std::array<float, 12>& left, const float right[12])
{
	constexpr float Epsilon = 0.0001f;
	for (size_t i = 0; i < left.size(); ++i)
	{
		if (std::abs(left[i] - right[i]) > Epsilon)
		{
			return false;
		}
	}
	return true;
}

void FillPersistentVoxelInstanceTransform(
	const float currentTranslation[3],
	const float bakedTranslation[3],
	std::array<float, 12>& target)
{
	target = { 1.0f, 0.0f, 0.0f, currentTranslation[0] - bakedTranslation[0],
		0.0f, 1.0f, 0.0f, currentTranslation[1] - bakedTranslation[1],
		0.0f, 0.0f, 1.0f, currentTranslation[2] - bakedTranslation[2] };
}

void FillPersistentVoxelActorInstanceTransform(
	const nri_scene::PersistentVoxelCacheEntryView& cacheEntry,
	const PersistentVoxelMeshVariantResource& meshResource,
	std::array<float, 12>& target)
{
	if (cacheEntry.meshBakeSpace == nri_scene::VoxelMeshBakeSpace::LocalSpace)
	{
		CopyPersistentVoxelInstanceTransform(cacheEntry.instanceTransform, target);
		return;
	}
	FillPersistentVoxelInstanceTransform(cacheEntry.currentTranslation, meshResource.bakedTranslation, target);
}

uint64_t EstimatePersistentVoxelActorUploadBytes(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry)
{
	if (cacheEntry.surface == nullptr)
	{
		if (!cacheEntry.directOnlyAdmission || cacheEntry.primitiveCount == 0)
		{
			return 0;
		}
		const uint64_t vertexBytes = (uint64_t)cacheEntry.primitiveCount * 2ull * sizeof(nri_scene::SceneVertex);
		const uint64_t indexBytes = (uint64_t)cacheEntry.primitiveCount * 3ull * sizeof(uint32_t);
		const uint64_t primitiveBytes = (uint64_t)cacheEntry.primitiveCount * sizeof(nri_scene::PrimitiveData);
		const uint64_t materialBytes = sizeof(nri_scene::MaterialData);
		return vertexBytes + indexBytes + primitiveBytes + materialBytes;
	}

	const uint64_t vertexBytes = (uint64_t)cacheEntry.surface->vertices.size() * sizeof(nri_scene::SceneVertex);
	const uint64_t indexBytes = (uint64_t)cacheEntry.surface->indices.size() * sizeof(uint32_t);
	const uint64_t primitiveBytes = (uint64_t)cacheEntry.primitiveCount * sizeof(nri_scene::PrimitiveData);
	const uint64_t materialBytes = sizeof(nri_scene::MaterialData);
	return vertexBytes + indexBytes + primitiveBytes + materialBytes;
}

void FillPersistentVoxelMeshBounds(const std::vector<nri_scene::SceneVertex>& vertices, PersistentVoxelMeshVariantResource& meshResource)
{
	meshResource.boundsValid = false;
	meshResource.boundsMin[0] = meshResource.boundsMin[1] = meshResource.boundsMin[2] = 0.0f;
	meshResource.boundsMax[0] = meshResource.boundsMax[1] = meshResource.boundsMax[2] = 0.0f;
	meshResource.boundsCenterMagnitude = 0.0f;
	meshResource.boundsMaxAbs = 0.0f;
	meshResource.surfaceArea = 0.0f;
	if (vertices.empty())
	{
		return;
	}
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		const float value = vertices[0].position[axis];
		if (!std::isfinite(value))
		{
			return;
		}
		meshResource.boundsMin[axis] = value;
		meshResource.boundsMax[axis] = value;
		meshResource.boundsMaxAbs = std::max(meshResource.boundsMaxAbs, std::abs(value));
	}
	for (size_t vertexIndex = 1; vertexIndex < vertices.size(); ++vertexIndex)
	{
		for (uint32_t axis = 0; axis < 3; ++axis)
		{
			const float value = vertices[vertexIndex].position[axis];
			if (!std::isfinite(value))
			{
				meshResource.boundsValid = false;
				return;
			}
			meshResource.boundsMin[axis] = std::min(meshResource.boundsMin[axis], value);
			meshResource.boundsMax[axis] = std::max(meshResource.boundsMax[axis], value);
			meshResource.boundsMaxAbs = std::max(meshResource.boundsMaxAbs, std::abs(value));
		}
	}
	const float centerX = (meshResource.boundsMin[0] + meshResource.boundsMax[0]) * 0.5f;
	const float centerY = (meshResource.boundsMin[1] + meshResource.boundsMax[1]) * 0.5f;
	const float centerZ = (meshResource.boundsMin[2] + meshResource.boundsMax[2]) * 0.5f;
	meshResource.boundsCenterMagnitude = std::sqrt(centerX * centerX + centerY * centerY + centerZ * centerZ);
	meshResource.boundsValid = true;
}

void FillPersistentVoxelMeshBounds(
	const NRIVoxelComputeDirectPublishBounds& bounds,
	PersistentVoxelMeshVariantResource& meshResource)
{
	meshResource.boundsValid = false;
	meshResource.boundsCenterMagnitude = 0.0f;
	meshResource.boundsMaxAbs = 0.0f;
	if (!bounds.valid)
	{
		return;
	}
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		if (!std::isfinite(bounds.min[axis]) || !std::isfinite(bounds.max[axis]) || bounds.min[axis] > bounds.max[axis])
		{
			return;
		}
		meshResource.boundsMin[axis] = bounds.min[axis];
		meshResource.boundsMax[axis] = bounds.max[axis];
		meshResource.boundsMaxAbs = std::max(
			meshResource.boundsMaxAbs,
			std::max(std::abs(bounds.min[axis]), std::abs(bounds.max[axis])));
	}
	const float centerX = (meshResource.boundsMin[0] + meshResource.boundsMax[0]) * 0.5f;
	const float centerY = (meshResource.boundsMin[1] + meshResource.boundsMax[1]) * 0.5f;
	const float centerZ = (meshResource.boundsMin[2] + meshResource.boundsMax[2]) * 0.5f;
	meshResource.boundsCenterMagnitude = std::sqrt(centerX * centerX + centerY * centerY + centerZ * centerZ);
	meshResource.boundsValid = true;
}

bool IsPersistentVoxelMeshResourceTransformKeyed(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry, const NRIPersistentVoxelSettings& settings)
{
	return settings.transformKeyed ||
		cacheEntry.meshBakeSpace != nri_scene::VoxelMeshBakeSpace::LocalSpace;
}

uint64_t BuildPersistentVoxelContentMeshResourceKey(uint64_t renderPrimitiveHash, uint64_t geometryContentHash)
{
	(void)geometryContentHash;
	if (renderPrimitiveHash != 0)
	{
		return nri_scene::HashCombine64(0x50564d4553485233ull, renderPrimitiveHash); // PVMESHR3: adjacency-coalesced smooth-normal payload
	}
	return 0;
}

uint64_t BuildPersistentVoxelVariantMeshResourceKey(const nri_scene::PrecachedVoxelVariantView& variant)
{
	const uint64_t contentKey = BuildPersistentVoxelContentMeshResourceKey(variant.renderPrimitiveHash, variant.geometryContentHash);
	return contentKey != 0 ? contentKey : variant.meshKeyHash;
}

uint64_t ResolvePersistentVoxelVariantGeometrySignature(const nri_scene::PrecachedVoxelVariantView& variant)
{
	return variant.geometryContentHash != 0 ?
		variant.geometryContentHash :
		(variant.geometrySignature != 0 ? variant.geometrySignature : variant.meshVariantHash);
}

uint64_t ResolvePersistentVoxelCacheEntryGeometrySignature(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry)
{
	return cacheEntry.geometryContentHash != 0 ? cacheEntry.geometryContentHash : cacheEntry.geometrySignature;
}

uint64_t BuildPersistentVoxelMeshResourceKey(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry, const NRIPersistentVoxelSettings& settings)
{
	if (!IsPersistentVoxelMeshResourceTransformKeyed(cacheEntry, settings))
	{
		const uint64_t contentKey = BuildPersistentVoxelContentMeshResourceKey(cacheEntry.renderPrimitiveHash, cacheEntry.geometryContentHash);
		return contentKey != 0 ? contentKey : cacheEntry.meshKeyHash;
	}
	uint64_t hash = cacheEntry.meshKeyHash;
	hash = nri_scene::HashCombine64(hash, cacheEntry.transformBasisSignature);
	return hash;
}

uint32_t ResolvePersistentVoxelActorVisibilityChunk(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry)
{
	(void)cacheEntry;
	// Persistent voxel actors move independently from the cached mesh surface that sourced them.
	// Let static map geometry own chunk gating; dynamic actor instances stay ray-visible by position.
	return UINT32_MAX;
}

namespace
{
	nri::BufferUsageBits PersistentVoxelBufferUsageFlags(nri::BufferUsageBits a, nri::BufferUsageBits b)
	{
		return (nri::BufferUsageBits)((uint32_t)a | (uint32_t)b);
	}

	nri::AccessStage PersistentVoxelComputeShaderResourceAccess()
	{
		nri::AccessStage access = {};
		access.stages = nri::StageBits::COMPUTE_SHADER;
		access.access = nri::AccessBits::SHADER_RESOURCE;
		return access;
	}

	nri::AccessStage PersistentVoxelAccelerationStructureBuildInputAccess()
	{
		nri::AccessStage access = {};
		access.access = nri::AccessBits::SHADER_RESOURCE;
		access.stages = nri::StageBits::ALL_SHADERS;
		return access;
	}

	enum class PersistentVoxelSharedBlasBuildResult : uint8_t
	{
		Skipped,
		Disabled,
		Reused,
		Built,
		Rejected,
		Failed
	};

	PersistentVoxelSharedBlasBuildResult BuildOrReusePersistentVoxelSharedBlas(
		uint64_t meshResourceKey,
		uint64_t geometrySignature,
		PersistentVoxelMeshVariantResource& meshResource,
		uint32_t frameIndex,
		const NRIPersistentVoxelSettings& settings,
		uint32_t& sharedBlasBuilds,
		NRIPersistentVoxelSharedBlasCache& sharedBlasCache,
		const NRIPersistentVoxelResetServices& resetServices,
		const NRIPersistentVoxelAccelerationServices& accelerationServices)
	{
		if (meshResourceKey == 0)
		{
			return PersistentVoxelSharedBlasBuildResult::Skipped;
		}
		if (!settings.sharedBlasBuildEnabled)
		{
			sharedBlasCache.RecordBuildDisabled(meshResourceKey);
			return PersistentVoxelSharedBlasBuildResult::Disabled;
		}
		if (geometrySignature == 0)
		{
			geometrySignature = meshResource.geometrySignature != 0 ? meshResource.geometrySignature : meshResource.meshKeyHash;
		}
		if (meshResource.meshBakeSpace != nri_scene::VoxelMeshBakeSpace::LocalSpace)
		{
			sharedBlasCache.RecordBuildReject(meshResourceKey, "non-local");
			return PersistentVoxelSharedBlasBuildResult::Rejected;
		}
		if (settings.transformKeyed)
		{
			sharedBlasCache.RecordBuildReject(meshResourceKey, "transform-keyed");
			return PersistentVoxelSharedBlasBuildResult::Rejected;
		}
		if (meshResource.vertexBuffer.buffer == nullptr || meshResource.indexBuffer.buffer == nullptr)
		{
			sharedBlasCache.RecordBuildReject(meshResourceKey, "missing-buffers");
			return PersistentVoxelSharedBlasBuildResult::Rejected;
		}
		if (meshResource.vertexCount == 0 || meshResource.indexCount == 0 || meshResource.primitiveCount == 0)
		{
			sharedBlasCache.RecordBuildReject(meshResourceKey, "invalid-counts");
			return PersistentVoxelSharedBlasBuildResult::Rejected;
		}
		const NRIPersistentVoxelSharedBlasEntry* existingSharedEntry = sharedBlasCache.Find(meshResourceKey);
		if (existingSharedEntry != nullptr && existingSharedEntry->state == NRIPersistentVoxelSharedBlasState::Resident)
		{
			if (existingSharedEntry->geometrySignature != geometrySignature ||
				existingSharedEntry->vertexCount != meshResource.vertexCount ||
				existingSharedEntry->indexCount != meshResource.indexCount ||
				existingSharedEntry->primitiveCount != meshResource.primitiveCount)
			{
				sharedBlasCache.RecordBuildReject(meshResourceKey, "geometry-mismatch");
				return PersistentVoxelSharedBlasBuildResult::Rejected;
			}
			return PersistentVoxelSharedBlasBuildResult::Reused;
		}
		if (sharedBlasBuilds >= settings.sharedBlasBuildsPerFrame)
		{
			sharedBlasCache.RecordBuildReject(meshResourceKey, "build-budget");
			return PersistentVoxelSharedBlasBuildResult::Rejected;
		}

		NRIPersistentVoxelSharedBlasEntry& sharedEntry = sharedBlasCache.PrepareBuild(
			meshResourceKey,
			geometrySignature,
			meshResource.vertexCount,
			meshResource.indexCount,
			meshResource.primitiveCount,
			frameIndex);
		if (!accelerationServices.BuildBottomLevel(
			meshResource.vertexBuffer,
			meshResource.indexBuffer,
			0u,
			meshResource.vertexCount,
			0u,
			meshResource.indexCount,
			meshResource.primitiveCount,
			sharedEntry.accelerationStructure))
		{
			resetServices.RetireAccelerationStructure(sharedEntry.accelerationStructure);
			sharedBlasCache.MarkBuildFailure(sharedEntry);
			return PersistentVoxelSharedBlasBuildResult::Failed;
		}
		if (!accelerationServices.BarrierBuildInputs(meshResource.vertexBuffer, meshResource.indexBuffer))
		{
			resetServices.RetireAccelerationStructure(sharedEntry.accelerationStructure);
			sharedBlasCache.MarkBuildFailure(sharedEntry);
			return PersistentVoxelSharedBlasBuildResult::Failed;
		}
		sharedBlasCache.MarkBuildSuccess(sharedEntry, frameIndex);
		sharedBlasBuilds++;
		return PersistentVoxelSharedBlasBuildResult::Built;
	}

	bool PersistentVoxelTransformFinite(const std::array<float, 12>& transform)
	{
		for (float value : transform)
		{
			if (!std::isfinite(value))
			{
				return false;
			}
		}
		return true;
	}

	NRIPersistentVoxelMaterialRangeHandle PersistentVoxelMaterialRangeHandle(
		const PersistentVoxelMaterialVariantResource& resource)
	{
		return { resource.materialOffset, resource.materialCapacity, resource.materialSlotGeneration };
	}

	bool PersistentVoxelMaterialRangeMatches(
		const PersistentVoxelBatch::ActorEntry& actor,
		const PersistentVoxelMaterialVariantResource& resource)
	{
		return actor.materialKeyHash != 0 &&
			actor.materialKeyHash == resource.materialKeyHash &&
			actor.materialOffset == resource.materialOffset &&
			actor.materialCount == resource.materialCount &&
			actor.materialSlotGeneration != 0 &&
			actor.materialSlotGeneration == resource.materialSlotGeneration;
	}

	bool PersistentVoxelAdmissionSchedulerQuiescent(const NRIVoxelAdmissionSnapshot& snapshot)
	{
		return snapshot.activeJobs == 0 &&
			snapshot.activeBindings == 0 &&
			snapshot.computeInFlight == 0 &&
			snapshot.blasInFlight == 0 &&
			snapshot.activeReservationBytes == 0 &&
			snapshot.activeBlasBytes == 0 &&
			snapshot.heldReservationBytes == 0 &&
			snapshot.heldBlasBytes == 0 &&
			snapshot.retirePendingBytes == 0 &&
			snapshot.retirePendingBlasBytes == 0;
	}

	struct PersistentVoxelSharedBlasRouteEvaluation
	{
		const NRIPersistentVoxelSharedBlasEntry* sharedEntry = nullptr;
		const char* rejectReason = nullptr;
		bool routeEligible = false;
		bool canRoute = false;
	};

	PersistentVoxelSharedBlasRouteEvaluation EvaluatePersistentVoxelSharedBlasRoute(
		const PersistentVoxelBatch::ActorEntry& actor,
		const PersistentVoxelMeshVariantResource& meshResource,
		const NRIPersistentVoxelSettings& settings,
		uint32_t arenaPrimitiveCursor,
		uint32_t arenaMaterialCursor,
		const NRIPersistentVoxelSharedBlasCache& sharedBlasCache)
	{
		PersistentVoxelSharedBlasRouteEvaluation result = {};
		if (meshResource.meshBakeSpace != nri_scene::VoxelMeshBakeSpace::LocalSpace)
		{
			result.rejectReason = "non-local";
			return result;
		}
		if (settings.transformKeyed)
		{
			result.rejectReason = "transform-keyed";
			return result;
		}
		result.routeEligible = true;

		const bool primitiveArenaRangeValid =
			actor.primitiveCount != 0 &&
			(uint64_t)actor.primitiveOffset + (uint64_t)actor.primitiveCount <= (uint64_t)arenaPrimitiveCursor;
		const bool materialArenaRangeValid =
			actor.materialCount != 0 &&
			(uint64_t)actor.materialOffset + (uint64_t)actor.materialCount <= (uint64_t)arenaMaterialCursor;
		if (!primitiveArenaRangeValid || !materialArenaRangeValid)
		{
			result.rejectReason = "invalid-material";
			return result;
		}
		if (!PersistentVoxelTransformFinite(actor.instanceTransform) ||
			!PersistentVoxelTransformFinite(actor.previousInstanceTransform))
		{
			result.rejectReason = "invalid-transform";
			return result;
		}

		const NRIPersistentVoxelSharedBlasEntry* sharedEntry = sharedBlasCache.Find(actor.meshResourceKey);
		if (sharedEntry == nullptr ||
			sharedEntry->state != NRIPersistentVoxelSharedBlasState::Resident ||
			sharedEntry->accelerationStructure.accelerationStructure == nullptr)
		{
			result.rejectReason = "missing-resident";
			return result;
		}
		if (sharedEntry->geometrySignature != actor.geometrySignature ||
			sharedEntry->vertexCount != meshResource.vertexCount ||
			sharedEntry->indexCount != meshResource.indexCount ||
			sharedEntry->primitiveCount != meshResource.primitiveCount)
		{
			result.rejectReason = "geometry-mismatch";
			return result;
		}
		result.sharedEntry = sharedEntry;
		result.canRoute = true;
		return result;
	}

	uint64_t HashPersistentVoxelMaterialPayloadData(const nri_scene::MaterialBridgeData& materialBridge)
	{
		const uint64_t materialSize = (uint64_t)materialBridge.materials.size() * sizeof(nri_scene::MaterialData);
		return NRIHashUploadPayloadBytes(
			materialBridge.materials.empty() ? nullptr : materialBridge.materials.data(),
			materialSize);
	}

	double PersistentVoxelDurationMs(
		const std::chrono::steady_clock::time_point& start,
		const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	struct PersistentVoxelScopedTimer
	{
		explicit PersistentVoxelScopedTimer(double& target)
			: targetMs(target), start(std::chrono::steady_clock::now())
		{
		}

		~PersistentVoxelScopedTimer()
		{
			targetMs += PersistentVoxelDurationMs(start, std::chrono::steady_clock::now());
		}

		double& targetMs;
		std::chrono::steady_clock::time_point start;
	};
}

void NRIPersistentVoxelResetServices::RetireBuffer(NRIBufferResource& resource) const
{
	if (retireBuffer != nullptr)
	{
		retireBuffer(user, resource);
	}
}

void NRIPersistentVoxelResetServices::RetireAccelerationStructure(NRIAccelerationStructureResource& resource) const
{
	if (retireAccelerationStructure != nullptr)
	{
		retireAccelerationStructure(user, resource);
	}
}

void NRIPersistentVoxelResetServices::InvalidateSceneDataDescriptors() const
{
	if (invalidateSceneDataDescriptors != nullptr)
	{
		invalidateSceneDataDescriptors(user);
	}
}

bool NRIPersistentVoxelMaterialClosureServices::Register(
	uint64_t buildSerial,
	uint64_t materialKey,
	uint64_t validatedSignature,
	const nri_scene::MaterialBridgeData& materials,
	NRIPersistentVoxelMaterialClosureSource source,
	NRIPersistentVoxelMaterialClosureResult& outResult) const
{
	return registerClosure != nullptr && registerClosure(
		user,
		buildSerial,
		materialKey,
		validatedSignature,
		materials,
		source,
		outResult);
}

bool NRIPersistentVoxelMaterialClosureServices::TryReuse(
	uint64_t buildSerial,
	uint64_t materialKey,
	uint64_t validatedSignature,
	NRIPersistentVoxelMaterialClosureSource source,
	nri_scene::MaterialBridgeData& outMaterials,
	NRIPersistentVoxelMaterialClosureResult& outResult) const
{
	return tryReuse != nullptr && tryReuse(
		user,
		buildSerial,
		materialKey,
		validatedSignature,
		source,
		outMaterials,
		outResult);
}

bool NRIPersistentVoxelPreloadServices::PumpAdmissionQueue(const char* phase) const
{
	if (pumpAdmissionQueue == nullptr)
	{
		return false;
	}
	return pumpAdmissionQueue(user, phase);
}

void NRIPersistentVoxelPreloadServices::PumpComputeJobs(uint32_t frameIndex) const
{
	if (pumpComputeJobs != nullptr)
	{
		pumpComputeJobs(user, frameIndex);
	}
}

bool NRIPersistentVoxelPreloadServices::EnsureBatch(NRIPersistentVoxelBatchStats* outStats) const
{
	return ensureBatch != nullptr && ensureBatch(user, outStats);
}

bool NRIPersistentVoxelPreloadServices::WarmSharedBlas(const std::vector<nri_scene::PrecachedVoxelVariantView>& variants, uint32_t frameIndex) const
{
	return warmSharedBlas == nullptr || warmSharedBlas(user, variants, frameIndex);
}

bool NRIPersistentVoxelPreloadServices::IsSubmitBudgetHit() const
{
	return isSubmitBudgetHit != nullptr && isSubmitBudgetHit(user);
}

void NRIPersistentVoxelPreloadServices::BuildMaterials(
	nri_scene::SceneView& sceneView,
	nri_scene::MaterialBridgeData& materials,
	const char* label) const
{
	if (buildMaterials != nullptr)
	{
		buildMaterials(user, sceneView, materials, label);
	}
}

bool NRIPersistentVoxelPreloadServices::PrewarmTexture(const nri_scene::TextureUpload& upload) const
{
	return prewarmTexture == nullptr || prewarmTexture(user, upload);
}

bool NRIPersistentVoxelAdmissionServices::AdmitVariantResource(
	PersistentVoxelAdmissionEntry& entry,
	uint64_t byteBudget,
	uint32_t& blasBudget,
	uint64_t& outUploadBytes,
	bool& outReusedMesh,
	bool& outReusedMaterial,
	bool& outInProgress,
	bool isolateBlasBuild,
	const char*& outFailureReason,
	PersistentVoxelAdmissionStats* outStats) const
{
	if (admitVariantResource == nullptr)
	{
		outFailureReason = "admission-service-missing";
		return false;
	}
	return admitVariantResource(
		user,
		entry,
		byteBudget,
		blasBudget,
		outUploadBytes,
		outReusedMesh,
		outReusedMaterial,
		outInProgress,
		isolateBlasBuild,
		outFailureReason,
		outStats);
}

bool NRIPersistentVoxelAdmissionServices::SubmitWaitAndRestart(const char* reason) const
{
	return submitWaitAndRestart != nullptr && submitWaitAndRestart(user, reason);
}

bool NRIPersistentVoxelAdmissionServices::IsSubmitBudgetHit() const
{
	return isSubmitBudgetHit != nullptr && isSubmitBudgetHit(user);
}

void NRIPersistentVoxelAdmissionServices::RetireBuffer(NRIBufferResource& resource) const
{
	if (retireBuffer != nullptr)
	{
		retireBuffer(user, resource);
	}
}

void NRIPersistentVoxelAdmissionServices::RetireAccelerationStructure(NRIAccelerationStructureResource& resource) const
{
	if (retireAccelerationStructure != nullptr)
	{
		retireAccelerationStructure(user, resource);
	}
}

void NRIPersistentVoxelAdmissionServices::BuildMaterials(
	nri_scene::SceneView& sceneView,
	nri_scene::MaterialBridgeData& materials,
	const char* label) const
{
	if (buildMaterials != nullptr)
	{
		buildMaterials(user, sceneView, materials, label);
	}
}

bool NRIPersistentVoxelAdmissionServices::PrewarmTexture(const nri_scene::TextureUpload& upload) const
{
	return prewarmTexture != nullptr && prewarmTexture(user, upload);
}

void NRIPersistentVoxelAdmissionServices::AssignGeometryPortalIndices(nri_scene::GeometryData& geometry) const
{
	if (assignGeometryPortalIndices != nullptr)
	{
		assignGeometryPortalIndices(user, geometry);
	}
}

bool NRIPersistentVoxelAdmissionServices::CreateStructuredBufferNoUpload(
	NRIBufferResource& resource,
	uint64_t size,
	uint32_t stride,
	nri::BufferUsageBits usage) const
{
	return createStructuredBufferNoUpload != nullptr && createStructuredBufferNoUpload(user, resource, size, stride, usage);
}

bool NRIPersistentVoxelAdmissionServices::EnsureArenaBuffer(
	NRIBufferResource& resource,
	uint64_t requiredSize,
	uint32_t stride,
	nri::BufferUsageBits usage,
	nri::AccessStage after) const
{
	return ensureArenaBuffer != nullptr && ensureArenaBuffer(user, resource, requiredSize, stride, usage, after);
}

bool NRIPersistentVoxelAdmissionServices::StageBufferCopyRange(
	NRIBufferResource& resource,
	uint64_t byteOffset,
	const void* data,
	uint64_t size,
	nri::AccessStage after,
	int uploadKind) const
{
	return stageBufferCopyRange != nullptr && stageBufferCopyRange(user, resource, byteOffset, data, size, after, uploadKind);
}

void NRIPersistentVoxelAdmissionServices::NoteBufferUpload(int uploadKind, uint64_t size, const char* reason) const
{
	if (noteBufferUpload != nullptr)
	{
		noteBufferUpload(user, uploadKind, size, reason);
	}
}

bool NRIPersistentVoxelAdmissionServices::BuildBottomLevel(
	const NRIBufferResource& vertexBuffer,
	const NRIBufferResource& indexBuffer,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure,
	NRIBufferResource* buildScratchBuffer) const
{
	return buildBottomLevel != nullptr &&
		buildBottomLevel(user, vertexBuffer, indexBuffer, vertexOffset, vertexCount, indexOffset, indexCount, primitiveCount, outAccelerationStructure, buildScratchBuffer);
}

uint64_t NRIPersistentVoxelAdmissionServices::GetRecordingCommandFenceValue() const
{
	return getRecordingCommandFenceValue != nullptr ? getRecordingCommandFenceValue(user) : 0;
}

bool NRIPersistentVoxelAdmissionServices::IsCommandFenceValueComplete(uint64_t fenceValue) const
{
	return isCommandFenceValueComplete != nullptr && isCommandFenceValueComplete(user, fenceValue);
}

bool NRIPersistentVoxelAdmissionServices::BarrierBuildInputs(const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) const
{
	return barrierBuildInputs != nullptr && barrierBuildInputs(user, vertexBuffer, indexBuffer);
}

bool NRIPersistentVoxelAdmissionServices::BarrierComputeToBuildInputs(const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) const
{
	return barrierComputeToBuildInputs != nullptr && barrierComputeToBuildInputs(user, vertexBuffer, indexBuffer);
}

bool NRIPersistentVoxelAccelerationServices::BuildBottomLevel(
	const NRIBufferResource& vertexBuffer,
	const NRIBufferResource& indexBuffer,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure) const
{
	return buildBottomLevel != nullptr && buildBottomLevel(
		user,
		vertexBuffer,
		indexBuffer,
		vertexOffset,
		vertexCount,
		indexOffset,
		indexCount,
		primitiveCount,
		outAccelerationStructure);
}

bool NRIPersistentVoxelAccelerationServices::BarrierBuildInputs(const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) const
{
	return barrierBuildInputs != nullptr && barrierBuildInputs(user, vertexBuffer, indexBuffer);
}

bool NRIPersistentVoxelAccelerationServices::EnsureStructuredBuffer(
	NRIBufferResource& resource,
	const void* data,
	uint64_t size,
	uint32_t stride,
	nri::BufferUsageBits usage,
	nri::AccessStage after,
	const char* reason,
	int uploadKind) const
{
	return ensureStructuredBuffer != nullptr &&
		ensureStructuredBuffer(user, resource, data, size, stride, usage, after, reason, uploadKind);
}

void NRIPersistentVoxelBatchServices::BuildMaterials(
	nri_scene::SceneView& sceneView,
	nri_scene::MaterialBridgeData& materials,
	const char* label) const
{
	if (buildMaterials != nullptr)
	{
		buildMaterials(user, sceneView, materials, label);
	}
}

bool NRIPersistentVoxelBatchServices::IsTextureCached(const nri_scene::TextureUpload& upload) const
{
	return isTextureCached != nullptr && isTextureCached(user, upload);
}

bool NRIPersistentVoxelBatchServices::PrewarmTexture(const nri_scene::TextureUpload& upload, double* outMs) const
{
	return prewarmTexture != nullptr && prewarmTexture(user, upload, outMs);
}

void NRIPersistentVoxelBatchServices::AssignGeometryPortalIndices(nri_scene::GeometryData& geometry) const
{
	if (assignGeometryPortalIndices != nullptr)
	{
		assignGeometryPortalIndices(user, geometry);
	}
}

bool NRIPersistentVoxelBatchServices::EnsureStructuredBuffer(
	NRIBufferResource& resource,
	const void* data,
	uint64_t size,
	uint32_t stride,
	nri::BufferUsageBits usage,
	nri::AccessStage after,
	const char* reason,
	int uploadKind) const
{
	return ensureStructuredBuffer != nullptr && ensureStructuredBuffer(user, resource, data, size, stride, usage, after, reason, uploadKind);
}

bool NRIPersistentVoxelBatchServices::EnsureArenaBuffer(
	NRIBufferResource& resource,
	uint64_t requiredSize,
	uint32_t stride,
	nri::BufferUsageBits usage,
	nri::AccessStage after) const
{
	return ensureArenaBuffer != nullptr && ensureArenaBuffer(user, resource, requiredSize, stride, usage, after);
}

bool NRIPersistentVoxelBatchServices::StageBufferCopyRange(
	NRIBufferResource& resource,
	uint64_t byteOffset,
	const void* data,
	uint64_t size,
	nri::AccessStage after,
	int uploadKind) const
{
	return stageBufferCopyRange != nullptr && stageBufferCopyRange(user, resource, byteOffset, data, size, after, uploadKind);
}

void NRIPersistentVoxelBatchServices::NoteBufferUpload(int uploadKind, uint64_t size, const char* reason) const
{
	if (noteBufferUpload != nullptr)
	{
		noteBufferUpload(user, uploadKind, size, reason);
	}
}

void NRIPersistentVoxelBatchServices::RetireAccelerationStructure(NRIAccelerationStructureResource& resource) const
{
	if (retireAccelerationStructure != nullptr)
	{
		retireAccelerationStructure(user, resource);
	}
}

bool NRIPersistentVoxelBatchServices::MaterialWouldEmit(const nri_scene::MaterialLightingMetadata& metadata) const
{
	return materialWouldEmit != nullptr && materialWouldEmit(user, metadata);
}

SceneLightSystem::SurfaceRecord NRIPersistentVoxelBatchServices::BuildSurfaceRecord(
	const nri_scene::SurfaceRef& surface,
	const nri_scene::MaterialBridgeData& materials,
	SceneLightRecordSource source,
	uint32_t materialIndex,
	uint32_t primitiveIndex) const
{
	if (buildSurfaceRecord != nullptr)
	{
		return buildSurfaceRecord(user, surface, materials, source, materialIndex, primitiveIndex);
	}
	return {};
}

bool NRIPersistentVoxelMaterialWarmupServices::EnsurePalette(const nri_scene::MaterialBridgeData& materials) const
{
	return ensurePalette != nullptr && ensurePalette(user, materials);
}

bool NRIPersistentVoxelMaterialWarmupServices::WarmTextures(
	const nri_scene::MaterialBridgeData& materials,
	NRIPersistentVoxelMaterialWarmupStats& stats) const
{
	return warmTextures != nullptr && warmTextures(user, materials, stats);
}

bool NRIPersistentVoxelMaterialUploadServices::EnsureMaterialArenaBuffer(NRIBufferResource& resource, uint64_t sizeBytes) const
{
	return ensureMaterialArenaBuffer != nullptr && ensureMaterialArenaBuffer(user, resource, sizeBytes);
}

bool NRIPersistentVoxelMaterialUploadServices::StageMaterialRanges(
	const NRIBufferResource& targetBuffer,
	const std::vector<RuntimeMutationResidentUploadRange>& ranges,
	const uint8_t* data,
	uint64_t availableBytes) const
{
	return stageMaterialRanges != nullptr && stageMaterialRanges(user, targetBuffer, ranges, data, availableBytes);
}

void NRIPersistentVoxelMaterialUploadServices::NoteMaterialUpload(uint64_t sizeBytes) const
{
	if (noteMaterialUpload != nullptr)
	{
		noteMaterialUpload(user, sizeBytes);
	}
}

uint64_t NRIPersistentVoxelTlasServices::GetAccelerationStructureHandle(const NRIAccelerationStructureResource& resource) const
{
	return getAccelerationStructureHandle != nullptr ? getAccelerationStructureHandle(user, resource) : 0ull;
}

NRIVoxelRepresentationDecision NRIPersistentVoxelTlasServices::EvaluateRepresentation(
	const NRIVoxelRepresentationFacts& facts) const
{
	if (evaluateRepresentation != nullptr)
	{
		return evaluateRepresentation(user, facts);
	}

	NRIVoxelRepresentationDecision decision = {};
	decision.sourceIdentityKey = facts.sourceIdentityKey;
	decision.meshResourceKey = facts.meshResourceKey;
	decision.materialKeyHash = facts.materialKeyHash;
	decision.actorIndex = facts.actorIndex;
	decision.resolvedVoxelIndex = facts.resolvedVoxelIndex;
	decision.primitiveCount = facts.primitiveCount;
	decision.retainedFrameAge = facts.retainedFrameAge;
	decision.requestedWorkloadMask = facts.workloadMask;
	decision.exactWorkloadMask = facts.workloadMask;
	decision.primaryWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_MAIN);
	decision.shadowWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_SHADOW);
	decision.reflectionWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_REFLECTION);
	decision.giWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_GI);
	decision.emissiveWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_EMISSIVE);
	decision.debugWorkloadMask = (uint8_t)(facts.workloadMask & (uint8_t)NRI_TLAS_MASK_DEBUG);
	decision.capturedThisFrame = facts.capturedThisFrame;
	decision.routedThroughSharedBlas = facts.routedThroughSharedBlas;
	return decision;
}

NRIPersistentVoxelOverlayStats NRIPersistentVoxelResidency::BuildOverlayStats() const
{
	NRIPersistentVoxelOverlayStats stats = {};
	stats.actorCount = batch.activeActorCount;
	stats.primitiveCount = batch.primitiveCount;
	stats.indexCount = batch.indexCount;
	stats.materialCount = (uint32_t)batch.materialBridge.materials.size();
	stats.byteCount =
		(uint64_t)batch.primitiveCount * sizeof(nri_scene::PrimitiveData) +
		nri_scene::EstimateMaterialBridgeBytes(batch.materialBridge);
	stats.byteCount += (uint64_t)stats.indexCount * sizeof(uint32_t);
	return stats;
}

bool NRIPersistentVoxelResidency::HasValidBatch() const
{
	return batch.valid;
}

bool NRIPersistentVoxelResidency::HasRenderableOverlay() const
{
	return
		batch.valid &&
		batch.activeActorCount > 0 &&
		batch.primitiveCount > 0 &&
		!batch.materialBridge.materials.empty();
}

bool NRIPersistentVoxelResidency::HasResidentIndirectOnlyActor(int32_t actorIndex) const
{
	if (actorIndex < 0 || !batch.valid)
	{
		return false;
	}

	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (!actor.active || !IsPersistentVoxelActorPublicationCurrent(actor) ||
			!actor.indirectOnly || actor.actorIndex != actorIndex)
		{
			continue;
		}
		const auto mesh = meshVariantResources.find(actor.meshResourceKey);
		if (mesh != meshVariantResources.end() &&
			mesh->second.accelerationStructure.accelerationStructure != nullptr)
		{
			return true;
		}
	}
	return false;
}

const std::unordered_set<int32_t>* NRIPersistentVoxelResidency::GetSuppressedActorIndices(uint32_t frameIndex) const
{
	return actorOccurrencePolicyFrameIndex == frameIndex ? &suppressedActorIndices : nullptr;
}

uint32_t NRIPersistentVoxelResidency::EvaluateActorOccurrencePolicies(
	uint32_t frameIndex,
	const NRIActorOccurrencePolicyContext& context)
{
	actorOccurrencePolicyFrameIndex = frameIndex;
	actorOccurrencePolicyDecisions.clear();
	suppressedActorIndices.clear();
	if (!context.enabled || !batch.valid)
	{
		return 0u;
	}

	std::unordered_map<int32_t, uint32_t> activeOccurrenceCounts;
	activeOccurrenceCounts.reserve(batch.actors.size());
	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (actor.active && actor.actorIndex >= 0)
		{
			activeOccurrenceCounts[actor.actorIndex]++;
		}
	}

	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (!actor.active || !IsPersistentVoxelActorPublicationCurrent(actor) || actor.actorIndex < 0)
		{
			continue;
		}
		NRIActorOccurrencePolicyCandidate candidate = {};
		candidate.identityKey = actor.identityKey;
		candidate.actorIndex = actor.actorIndex;
		candidate.requestedWorkloadMask = NRI_TLAS_MASK_ALL_WORKLOADS;
		candidate.active = true;
		candidate.uniqueActiveOccurrence = activeOccurrenceCounts[actor.actorIndex] == 1u;
		candidate.capturedThisFrame = actor.capturedThisFrame;
		const auto meshResourceIt = meshVariantResources.find(actor.meshResourceKey);
		if (meshResourceIt != meshVariantResources.end())
		{
			const PersistentVoxelMeshVariantResource& mesh = meshResourceIt->second;
			candidate.boundsValid = mesh.boundsValid && ComputeNRIActorOccurrenceWorldBounds(
				actor.instanceTransform,
				mesh.boundsMin,
				mesh.boundsMax,
				candidate.boundsMin,
				candidate.boundsMax);
		}
		NRIActorOccurrencePolicyDecision decision = EvaluateNRIActorOccurrencePolicy(context, candidate);
		actorOccurrencePolicyDecisions[actor.actorIndex] = decision;
		if (decision.suppress)
		{
			suppressedActorIndices.insert(actor.actorIndex);
		}
	}
	return (uint32_t)suppressedActorIndices.size();
}

bool NRIPersistentVoxelResidency::IsIndirectOnlyActorTlasAppendEligible(
	int32_t actorIndex,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	const NRIPersistentVoxelTlasServices& services) const
{
	if (actorIndex < 0 || !batch.valid)
	{
		return false;
	}

	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (actor.active && actor.indirectOnly && actor.actorIndex == actorIndex &&
			IsActorTlasAppendEligible(actor, frameIndex, settings, services))
		{
			return true;
		}
	}
	return false;
}

bool NRIPersistentVoxelResidency::HasOverlayPreparationEligibleActor(
	const NRIPersistentVoxelSettings& settings) const
{
	if (!batch.valid)
	{
		return false;
	}

	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (actor.active && IsActorOverlayPreparationEligible(actor, settings))
		{
			return true;
		}
	}
	return false;
}

bool NRIPersistentVoxelResidency::IsActorTlasAppendEligible(
	const PersistentVoxelBatch::ActorEntry& actor,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	const NRIPersistentVoxelTlasServices& services) const
{
	if (services.getAccelerationStructureHandle == nullptr ||
		!IsActorOverlayPreparationEligible(actor, settings))
	{
		return false;
	}

	const PersistentVoxelMeshVariantResource& mesh = meshVariantResources.find(actor.meshResourceKey)->second;
	if (mesh.accelerationStructure.accelerationStructure == nullptr ||
		vertexBuffer.shaderView == nullptr ||
		indexBuffer.shaderView == nullptr ||
		primitiveBuffer.shaderView == nullptr ||
		materialBuffer.shaderView == nullptr)
	{
		return false;
	}
	return
		(mesh.tlasPublished || mesh.tlasReadyFrame <= frameIndex) &&
		services.GetAccelerationStructureHandle(mesh.accelerationStructure) != 0;
}

bool NRIPersistentVoxelResidency::IsActorOverlayPreparationEligible(
	const PersistentVoxelBatch::ActorEntry& actor,
	const NRIPersistentVoxelSettings& settings) const
{
	if (!IsPersistentVoxelActorPublicationCurrent(actor) ||
		settings.omitTlasOccurrences ||
			(actor.resolvedVoxelIndex >= 0 &&
				(actor.resolvedVoxelIndex == settings.excludeIndices[0] ||
				 actor.resolvedVoxelIndex == settings.excludeIndices[1] ||
				 actor.resolvedVoxelIndex == settings.excludeIndices[2])) ||
		(settings.excludeMinPrimitives > 0 && actor.primitiveCount >= settings.excludeMinPrimitives))
	{
		return false;
	}

	const auto meshIt = meshVariantResources.find(actor.meshResourceKey);
	const auto materialIt = materialVariantResources.find(actor.materialKeyHash);
	if (meshIt == meshVariantResources.end() || materialIt == materialVariantResources.end())
	{
		return false;
	}
	const PersistentVoxelMeshVariantResource& mesh = meshIt->second;
	const PersistentVoxelMaterialVariantResource& material = materialIt->second;
	if (!materialRangeAllocator.Owns(PersistentVoxelMaterialRangeHandle(material)) ||
		!PersistentVoxelMaterialRangeMatches(actor, material) ||
		(!mesh.directComputePublished &&
			(mesh.indexBuffer.shaderView == nullptr || mesh.vertexBuffer.shaderView == nullptr)))
	{
		return false;
	}
	const bool primitiveRangeValid =
		(uint64_t)actor.primitiveOffset + (uint64_t)actor.primitiveCount <= (uint64_t)arenaPrimitiveCursor;
	const bool materialRangeValid =
		(uint64_t)actor.materialOffset + (uint64_t)actor.materialCount <= materialRangeAllocator.Stats().cursorRows;
	const bool publishedMaterialRangeValid =
		(uint64_t)actor.materialOffset + (uint64_t)actor.materialCount <= batch.materialBridge.materials.size();
	const bool meshRangeMatches =
		actor.primitiveOffset == mesh.primitiveOffset &&
		actor.primitiveCount == mesh.primitiveCount &&
		actor.indexOffset == mesh.indexOffset &&
		actor.indexCount == mesh.indexCount;
	const auto transformFinite = [](const std::array<float, 12>& transform)
	{
		for (float value : transform)
		{
			if (!std::isfinite(value))
			{
				return false;
			}
		}
		return true;
	};
	return
		primitiveRangeValid &&
		materialRangeValid &&
		publishedMaterialRangeValid &&
		meshRangeMatches &&
		transformFinite(actor.instanceTransform) &&
		transformFinite(actor.previousInstanceTransform);
}

bool NRIPersistentVoxelResidency::HasPreloadPending() const
{
	return preloadPending;
}

NRIPersistentVoxelPreloadStatus NRIPersistentVoxelResidency::BuildPreloadStatusSnapshot() const
{
	return lastPreloadStatus;
}

uint32_t NRIPersistentVoxelResidency::OverlayMaterialCount() const
{
	return (uint32_t)batch.materialBridge.materials.size();
}

uint32_t NRIPersistentVoxelResidency::EstimatePrimitiveCountForInstanceOffset(uint32_t primitiveOffset) const
{
	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (actor.active && actor.primitiveOffset == primitiveOffset && actor.primitiveCount > 0)
		{
			return actor.primitiveCount;
		}
	}
	return 0;
}

nri_scene::SceneDebugStats NRIPersistentVoxelResidency::BuildOverlayDebugStats() const
{
	nri_scene::SceneDebugStats stats = batch.stats;
	stats.voxelStableCandidates = 0;
	stats.voxelStableUncacheable = 0;
	stats.voxelStableSignatureHits = 0;
	stats.voxelStableSignatureMisses = 0;
	stats.voxelStableSignatureChanges = 0;
	stats.voxelStableSplitStable = 0;
	stats.voxelStableSplitLive = 0;
	stats.voxelCacheEntries = 0;
	stats.voxelCacheSurfaceHits = 0;
	stats.voxelCacheSurfaceStores = 0;
	stats.voxelCacheSurfaceRebuilds = 0;
	stats.voxelCacheTransformRebakes = 0;
	stats.voxelCacheSurfaceRemoves = 0;
	stats.voxelCacheNotCaptured = 0;
	stats.voxelCachePrimitives = 0;
	return stats;
}

uint64_t NRIPersistentVoxelResidency::BuildSceneGenerationHash() const
{
	if (!HasRenderableOverlay())
	{
		return 0;
	}

	uint64_t generation = nri_scene::HashCombine64(nri_scene::HashCombine64(
		nri_scene::HashCombine64(
			nri_scene::HashCombine64(
				nri_scene::HashCombine64(batch.sourceSerial, (uint64_t)batch.rebuildCount),
				(uint64_t)batch.activeActorCount),
			(uint64_t)batch.primitiveCount),
		(uint64_t)batch.materialCount),
		batchMaterialPublicationGeneration);
	generation = nri_scene::HashCombine64(generation, actorOccurrenceLedger.PublicationSerial());
	generation = nri_scene::HashCombine64(generation, actorOccurrenceLedger.CommittedPublicationHash());
	return generation;
}

bool NRIPersistentVoxelResidency::PublishCanonicalMaterialResource(
	PersistentVoxelMaterialVariantResource& candidate,
	bool& outReused)
{
	outReused = false;
	if (candidate.materialKeyHash == 0 || candidate.materialSignature == 0 ||
		candidate.materialCount == 0 || candidate.materialBridge.materials.empty())
	{
		return false;
	}

	auto existingIt = materialVariantResources.find(candidate.materialKeyHash);
	if (existingIt != materialVariantResources.end() &&
		existingIt->second.materialSignature == candidate.materialSignature &&
		existingIt->second.materialBridgeBuildSerial == candidate.materialBridgeBuildSerial &&
		existingIt->second.materialCount != 0 &&
		existingIt->second.materialSlotGeneration != 0 &&
		!existingIt->second.materialBridge.materials.empty())
	{
		PersistentVoxelMaterialVariantResource& existing = existingIt->second;
		existing.lastDesiredMapGeneration = std::max(existing.lastDesiredMapGeneration, candidate.lastDesiredMapGeneration);
		existing.lastUsedMapGeneration = std::max(existing.lastUsedMapGeneration, candidate.lastUsedMapGeneration);
		existing.lastUsedFrame = std::max(existing.lastUsedFrame, candidate.lastUsedFrame);
		existing.sourceBits |= candidate.sourceBits;
		existing.activeActorReferences = std::max(existing.activeActorReferences, candidate.activeActorReferences);
		existing.priority = std::min(existing.priority, candidate.priority);
		existing.gpuForce = existing.gpuForce || candidate.gpuForce;
		existing.gpuPrefer = existing.gpuPrefer || candidate.gpuPrefer;
		existing.cold = existing.cold && candidate.cold;
		candidate = existing;
		outReused = true;
		return true;
	}

	const NRIPersistentVoxelMaterialRangeHandle previousRange =
		existingIt != materialVariantResources.end() ?
		PersistentVoxelMaterialRangeHandle(existingIt->second) :
		NRIPersistentVoxelMaterialRangeHandle{};
	NRIPersistentVoxelMaterialRangeHandle replacementRange = {};
	bool materialSliceMoved = false;
	if (!materialRangeAllocator.Reallocate(
		previousRange,
		candidate.materialCount,
		replacementRange,
		materialSliceMoved))
	{
		return false;
	}
	(void)materialSliceMoved;
	candidate.materialOffset = replacementRange.offset;
	candidate.materialCapacity = replacementRange.capacity;
	candidate.materialSlotGeneration = replacementRange.generation;
	candidate.materialUploadHash = 0;

	materialVariantResources[candidate.materialKeyHash] = candidate;
	dirtyMaterialResourceKeys.insert(candidate.materialKeyHash);
	materialResourceGeneration++;
	publishedMaterialKeys.insert(candidate.materialKeyHash);
	candidate = materialVariantResources[candidate.materialKeyHash];
	MarkMaintenanceMutation();
	return true;
}

void NRIPersistentVoxelResidency::RebuildBatchMaterialBridge(PersistentVoxelBatch& targetBatch)
{
	nri_scene::ClearMaterialBridgeRetainingCapacity(targetBatch.materialBridge);
	const uint32_t materialCursor = materialRangeAllocator.Stats().cursorRows;
	targetBatch.materialBridge.materials.reserve(materialCursor);
	targetBatch.materialBridge.lightMetadata.reserve(materialCursor);
	const std::unordered_set<uint64_t> activeMaterialKeys =
		CollectActivePersistentVoxelMaterialKeys(targetBatch);
	std::unordered_map<uint64_t, uint32_t> textureLookup;
	textureLookup.reserve(targetBatch.materialBridge.textures.capacity() + activeMaterialKeys.size());
	std::vector<PersistentVoxelMaterialVariantResource*> materialResources;
	materialResources.reserve(activeMaterialKeys.size());
	for (uint64_t materialKey : activeMaterialKeys)
	{
		auto resourceIt = materialVariantResources.find(materialKey);
		if (resourceIt != materialVariantResources.end() && resourceIt->second.materialCount > 0)
		{
			materialResources.push_back(&resourceIt->second);
		}
	}
	std::sort(
		materialResources.begin(),
		materialResources.end(),
		[](const PersistentVoxelMaterialVariantResource* left, const PersistentVoxelMaterialVariantResource* right)
		{
			return left->materialOffset < right->materialOffset;
		});
	for (PersistentVoxelMaterialVariantResource* resource : materialResources)
	{
		if (resource == nullptr)
		{
			continue;
		}
		if (targetBatch.materialBridge.materials.size() < resource->materialOffset)
		{
			targetBatch.materialBridge.materials.resize(resource->materialOffset);
			targetBatch.materialBridge.lightMetadata.resize(resource->materialOffset);
		}
		nri_scene::AppendMaterialBridge(resource->materialBridge, targetBatch.materialBridge, textureLookup);
		const uint64_t materialSize = (uint64_t)resource->materialCount * sizeof(nri_scene::MaterialData);
		if ((resource->materialPayloadHash == 0 ||
			dirtyMaterialResourceKeys.find(resource->materialKeyHash) != dirtyMaterialResourceKeys.end()) &&
			resource->materialOffset <= targetBatch.materialBridge.materials.size() &&
			resource->materialCount <= targetBatch.materialBridge.materials.size() - resource->materialOffset)
		{
			resource->materialPayloadHash = NRIHashUploadPayloadBytes(
				targetBatch.materialBridge.materials.data() + resource->materialOffset,
				materialSize);
		}
	}
	pendingMaterialActorRebinds = 0;
	for (PersistentVoxelBatch::ActorEntry& actor : targetBatch.actors)
	{
		if (!actor.active || !IsPersistentVoxelActorPublicationCurrent(actor))
		{
			actor.materialOffset = 0;
			actor.materialSlotGeneration = 0;
			continue;
		}
		const auto materialIt = materialVariantResources.find(actor.materialKeyHash);
		if (materialIt == materialVariantResources.end() || materialIt->second.materialCount == 0)
		{
			continue;
		}
		const PersistentVoxelMaterialVariantResource& materialResource = materialIt->second;
		if (NRIPersistentVoxelMaterialBindingNeedsRebind(
			actor.materialOffset,
			actor.materialCount,
			materialResource.materialOffset,
			materialResource.materialCount) ||
			actor.materialSlotGeneration != materialResource.materialSlotGeneration)
		{
			actor.materialOffset = materialResource.materialOffset;
			actor.materialCount = materialResource.materialCount;
			actor.materialSlotGeneration = materialResource.materialSlotGeneration;
			actor.materialBridge = materialResource.materialBridge;
			pendingMaterialActorRebinds++;
		}
	}
	std::vector<uint64_t> rebuiltTextureKeys;
	rebuiltTextureKeys.reserve(targetBatch.materialBridge.textures.size());
	for (const nri_scene::TextureUpload& texture : targetBatch.materialBridge.textures)
	{
		rebuiltTextureKeys.push_back(texture.key);
	}
	if (!NRIPersistentVoxelMaterialTextureLayoutPreservesPrefix(uploadedMaterialTextureKeys, rebuiltTextureKeys))
	{
		pendingMaterialLayoutInvalidatedResources = 0;
		for (uint64_t materialKey : activeMaterialKeys)
		{
			auto resourceIt = materialVariantResources.find(materialKey);
			if (resourceIt == materialVariantResources.end() || resourceIt->second.materialCount == 0)
			{
				continue;
			}
			resourceIt->second.materialUploadHash = 0;
			dirtyMaterialResourceKeys.insert(materialKey);
			pendingMaterialLayoutInvalidatedResources++;
		}
	}
	batchMaterialResourceGeneration = materialResourceGeneration;
	batchMaterialPublicationGeneration++;
}

void NRIPersistentVoxelResidency::RecomputeBatchState(PersistentVoxelBatch& targetBatch) const
{
	targetBatch.primitiveCount = 0;
	targetBatch.indexCount = 0;
	targetBatch.materialCount = 0;
	targetBatch.activeActorCount = 0;
	for (const PersistentVoxelBatch::ActorEntry& actor : targetBatch.actors)
	{
		if (actor.active && IsPersistentVoxelActorPublicationCurrent(actor))
		{
			targetBatch.activeActorCount++;
			targetBatch.primitiveCount += actor.primitiveCount;
			targetBatch.indexCount += actor.indexCount;
		}
	}
	targetBatch.materialCount = (uint32_t)targetBatch.materialBridge.materials.size();
	targetBatch.surfaceCount = targetBatch.activeActorCount;
	targetBatch.stats = {};
	targetBatch.stats.triangleEstimate = targetBatch.primitiveCount;
	targetBatch.stats.voxelCachePrimitives = targetBatch.primitiveCount;
	targetBatch.stats.materialRefs = targetBatch.materialCount;
	targetBatch.stats.spriteDrawItems = targetBatch.surfaceCount;
	targetBatch.stats.modelDrawItems = targetBatch.surfaceCount;
	targetBatch.stats.voxelProxyDrawItems = targetBatch.surfaceCount;
	targetBatch.stats.voxelCacheEntries = targetBatch.surfaceCount;
	targetBatch.stats.totalDrawItems = targetBatch.surfaceCount;
	targetBatch.valid =
		targetBatch.activeActorCount > 0 &&
		targetBatch.primitiveCount > 0 &&
		!targetBatch.materialBridge.materials.empty();
}

void NRIPersistentVoxelResidency::RefreshActiveResourceReferences(uint32_t frameIndex)
{
	std::unordered_map<uint64_t, uint32_t> nextMeshReferences;
	std::unordered_map<uint64_t, uint32_t> nextMaterialReferences;
	nextMeshReferences.reserve(batch.activeActorCount);
	nextMaterialReferences.reserve(batch.activeActorCount);
	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (actor.active && IsPersistentVoxelActorPublicationCurrent(actor))
		{
			if (actor.meshResourceKey != 0)
			{
				nextMeshReferences[actor.meshResourceKey]++;
			}
			if (actor.materialKeyHash != 0)
			{
				nextMaterialReferences[actor.materialKeyHash]++;
			}
		}
	}
	if (activeMeshReferences == nextMeshReferences &&
		activeMaterialReferences == nextMaterialReferences)
	{
		return;
	}
	maintenanceStats.pressureMembershipChanges++;

	const bool updateCachedStatus = cachedResourceStatusGeneration == maintenanceMutationGeneration;
	for (const auto& pair : activeMeshReferences)
	{
		auto resource = meshVariantResources.find(pair.first);
		if (resource != meshVariantResources.end())
		{
			if (updateCachedStatus && resource->second.activeActorReferences != 0)
			{
				cachedResourceStatus.zeroRefMeshResourceCount++;
				cachedResourceStatus.zeroRefResourceBytes += resource->second.residentBytes;
			}
			resource->second.activeActorReferences = 0;
		}
	}
	for (const auto& pair : activeMaterialReferences)
	{
		auto resource = materialVariantResources.find(pair.first);
		if (resource != materialVariantResources.end())
		{
			if (updateCachedStatus && resource->second.activeActorReferences != 0)
			{
				cachedResourceStatus.zeroRefMaterialResourceCount++;
				cachedResourceStatus.zeroRefResourceBytes += resource->second.residentBytes;
			}
			resource->second.activeActorReferences = 0;
		}
	}
	activeMeshReferences = std::move(nextMeshReferences);
	activeMaterialReferences = std::move(nextMaterialReferences);
	for (const auto& pair : activeMeshReferences)
	{
		auto resource = meshVariantResources.find(pair.first);
		if (resource != meshVariantResources.end())
		{
			if (updateCachedStatus && resource->second.activeActorReferences == 0)
			{
				if (cachedResourceStatus.zeroRefMeshResourceCount != 0)
				{
					cachedResourceStatus.zeroRefMeshResourceCount--;
				}
				cachedResourceStatus.zeroRefResourceBytes =
					cachedResourceStatus.zeroRefResourceBytes >= resource->second.residentBytes ?
					cachedResourceStatus.zeroRefResourceBytes - resource->second.residentBytes : 0ull;
			}
			resource->second.activeActorReferences = pair.second;
			resource->second.lastUsedFrame = frameIndex;
			resource->second.lastUsedMapGeneration = residencyMapGeneration;
			resource->second.cold = false;
		}
	}
	for (const auto& pair : activeMaterialReferences)
	{
		auto resource = materialVariantResources.find(pair.first);
		if (resource != materialVariantResources.end())
		{
			if (updateCachedStatus && resource->second.activeActorReferences == 0)
			{
				if (cachedResourceStatus.zeroRefMaterialResourceCount != 0)
				{
					cachedResourceStatus.zeroRefMaterialResourceCount--;
				}
				cachedResourceStatus.zeroRefResourceBytes =
					cachedResourceStatus.zeroRefResourceBytes >= resource->second.residentBytes ?
					cachedResourceStatus.zeroRefResourceBytes - resource->second.residentBytes : 0ull;
			}
			resource->second.activeActorReferences = pair.second;
			resource->second.lastUsedFrame = frameIndex;
			resource->second.lastUsedMapGeneration = residencyMapGeneration;
			resource->second.cold = false;
		}
	}
	if (ShouldInvalidateNRIPersistentVoxelPressureForMembershipChange(pressureProtectionBlocked))
	{
		pressureEvaluationValid = false;
		maintenanceStats.pressureMembershipInvalidations++;
	}
}

void NRIPersistentVoxelResidency::ClearActorInstances(const NRIPersistentVoxelResetServices& services)
{
	batch = {};
	actorOccurrenceLedger.Reset();
	actorOccurrencePolicyFrameIndex = UINT32_MAX;
	actorOccurrencePolicyDecisions.clear();
	suppressedActorIndices.clear();
	RefreshActiveResourceReferences(0);
	instances.clear();
	const bool keepSharedVariantArena = !meshVariantResources.empty();
	if (!keepSharedVariantArena)
	{
		services.RetireBuffer(vertexBuffer);
		services.RetireBuffer(indexBuffer);
		services.RetireBuffer(primitiveBuffer);
		arenaVertexCursor = 0;
		arenaIndexCursor = 0;
		arenaPrimitiveCursor = 0;
	}
	services.InvalidateSceneDataDescriptors();
}

bool NRIPersistentVoxelResidency::ValidateActorGeometry(
	uint64_t identityKey,
	uint64_t surfaceSignature,
	const nri_scene::GeometryData& actorGeometry,
	uint32_t materialCount,
	uint32_t frameIndex,
	bool voxelStatsEnabled)
{
	auto reject = [&](const char* reason, uint32_t value, uint32_t limit) -> bool
	{
		actorRejectedSignatures[identityKey] = surfaceSignature;
		Printf("NRI PT persistent voxel actor rejected: actor_key=0x%llx surface_sig=0x%llx reason=%s value=%u limit=%u vertices=%u indices=%u primitives=%u materials=%u\n",
			(unsigned long long)identityKey,
			(unsigned long long)surfaceSignature,
			reason != nullptr ? reason : "unknown",
			value,
			limit,
			(uint32_t)actorGeometry.vertices.size(),
			(uint32_t)actorGeometry.indices.size(),
			(uint32_t)actorGeometry.primitives.size(),
			materialCount);
		if (voxelStatsEnabled)
		{
			Printf("PERF pt voxel validation NRI: frame=%u action=quarantine reason=%s actor_key=0x%llx surface_sig=0x%llx value=%u limit=%u vertices=%u indices=%u prims=%u materials=%u ready=0\n",
				frameIndex,
				reason != nullptr ? reason : "unknown",
				(unsigned long long)identityKey,
				(unsigned long long)surfaceSignature,
				value,
				limit,
				(uint32_t)actorGeometry.vertices.size(),
				(uint32_t)actorGeometry.indices.size(),
				(uint32_t)actorGeometry.primitives.size(),
				materialCount);
		}
		return false;
	};

	if (actorGeometry.vertices.empty() || actorGeometry.indices.empty() || actorGeometry.primitives.empty())
	{
		return reject("empty", 0u, 1u);
	}
	if ((actorGeometry.indices.size() % 3u) != 0u)
	{
		return reject("index-count", (uint32_t)actorGeometry.indices.size(), 3u);
	}
	if (actorGeometry.primitives.size() != actorGeometry.indices.size() / 3u)
	{
		return reject("primitive-triangle-count", (uint32_t)actorGeometry.primitives.size(), (uint32_t)(actorGeometry.indices.size() / 3u));
	}

	const uint32_t vertexCount = (uint32_t)actorGeometry.vertices.size();
	for (const uint32_t index : actorGeometry.indices)
	{
		if (index >= vertexCount)
		{
			return reject("index-range", index, vertexCount);
		}
	}

	for (const nri_scene::SceneVertex& vertex : actorGeometry.vertices)
	{
		for (float component : vertex.position)
		{
			if (!std::isfinite(component) || std::abs(component) > 100000000.0f)
			{
				return reject("vertex-position", 0u, 0u);
			}
		}
		for (float component : vertex.prevPosition)
		{
			if (!std::isfinite(component) || std::abs(component) > 100000000.0f)
			{
				return reject("vertex-prev-position", 0u, 0u);
			}
		}
	}

	for (const nri_scene::PrimitiveData& primitive : actorGeometry.primitives)
	{
		if (primitive.indices[0] >= vertexCount ||
			primitive.indices[1] >= vertexCount ||
			primitive.indices[2] >= vertexCount)
		{
			return reject("primitive-index-range", std::max({ primitive.indices[0], primitive.indices[1], primitive.indices[2] }), vertexCount);
		}
		if (materialCount != 0 && primitive.materialIndex >= materialCount)
		{
			return reject("primitive-material-range", primitive.materialIndex, materialCount);
		}
		for (float component : primitive.normal)
		{
			if (!std::isfinite(component))
			{
				return reject("primitive-normal", 0u, 0u);
			}
		}
	}

	auto rejectedIt = actorRejectedSignatures.find(identityKey);
	if (rejectedIt != actorRejectedSignatures.end() && rejectedIt->second == surfaceSignature)
	{
		actorRejectedSignatures.erase(rejectedIt);
	}
	return true;
}

void NRIPersistentVoxelResidency::AppendMaterialBridgeTo(nri_scene::MaterialBridgeData& destination) const
{
	nri_scene::AppendMaterialBridge(batch.materialBridge, destination);
}

bool NRIPersistentVoxelResidency::WarmMaterialResources(
	const NRIPersistentVoxelMaterialWarmupServices& services,
	NRIPersistentVoxelMaterialWarmupResult& outResult) const
{
	outResult = {};
	outResult.paletteReady = true;
	outResult.hasMaterials = batch.valid && !batch.materialBridge.materials.empty();
	outResult.materialCount = outResult.hasMaterials ? (uint32_t)batch.materialBridge.materials.size() : 0u;
	outResult.variantResourceCount = (uint32_t)materialVariantResources.size();
	if (!outResult.hasMaterials)
	{
		return true;
	}

	outResult.paletteReady = services.EnsurePalette(batch.materialBridge);
	if (!outResult.paletteReady || !services.WarmTextures(batch.materialBridge, outResult.textureStats))
	{
		return false;
	}
	outResult.pending = outResult.textureStats.pending;
	return true;
}

bool NRIPersistentVoxelResidency::UploadArenaMaterialBuffers(
	const std::vector<nri_scene::MaterialData>& materials,
	const NRIPersistentVoxelMaterialUploadServices& services,
	uint32_t frameIndex,
	bool validateActiveMaterialPayloads,
	bool voxelStatsEnabled,
	NRIPersistentVoxelMaterialUploadStats& outStats)
{
	outStats = {};
	outStats.layoutInvalidatedResources = pendingMaterialLayoutInvalidatedResources;
	outStats.actorMaterialRebinds = pendingMaterialActorRebinds;
	if (!batch.valid)
	{
		return true;
	}
	std::unordered_set<uint64_t> activeMaterialKeys;
	activeMaterialKeys.reserve(batch.actors.size());
	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (!actor.active || actor.materialKeyHash == 0 ||
			materialVariantResources.find(actor.materialKeyHash) == materialVariantResources.end())
		{
			continue;
		}
		activeMaterialKeys.insert(actor.materialKeyHash);
	}
	if (validateActiveMaterialPayloads ||
		uploadedMaterialPublicationGeneration != batchMaterialPublicationGeneration)
	{
		dirtyMaterialResourceKeys.insert(activeMaterialKeys.begin(), activeMaterialKeys.end());
	}
	for (auto dirtyIt = dirtyMaterialResourceKeys.begin(); dirtyIt != dirtyMaterialResourceKeys.end(); )
	{
		if (activeMaterialKeys.find(*dirtyIt) == activeMaterialKeys.end())
		{
			dirtyIt = dirtyMaterialResourceKeys.erase(dirtyIt);
			continue;
		}
		++dirtyIt;
	}
	if (dirtyMaterialResourceKeys.empty() &&
		(materialBuffer.buffer == nullptr || uploadedMaterialResourceGeneration != materialResourceGeneration))
	{
		dirtyMaterialResourceKeys.insert(activeMaterialKeys.begin(), activeMaterialKeys.end());
	}
	outStats.activeValidatedResources = (uint32_t)activeMaterialKeys.size();
	if (uploadedMaterialResourceGeneration == materialResourceGeneration && materialBuffer.buffer != nullptr)
	{
		if (dirtyMaterialResourceKeys.empty())
		{
			return true;
		}
	}

	const uint64_t materialBufferBytesBefore = materialBuffer.memorySize;
	if (!services.EnsureMaterialArenaBuffer(
		materialBuffer,
		materialRangeAllocator.Stats().cursorRows * sizeof(nri_scene::MaterialData)))
	{
		return false;
	}
	if (materialBuffer.memorySize != materialBufferBytesBefore)
	{
		MarkMaintenanceMutation();
	}

	struct PendingMaterialUpload
	{
		PersistentVoxelMaterialVariantResource* resource = nullptr;
		uint64_t materialHash = 0;
	};

	std::vector<RuntimeMutationResidentUploadRange> dirtyMaterialRanges;
	std::vector<PendingMaterialUpload> pendingMaterialUploads;
	dirtyMaterialRanges.reserve(dirtyMaterialResourceKeys.size());
	pendingMaterialUploads.reserve(dirtyMaterialResourceKeys.size());
	std::vector<uint64_t> resolvedDirtyKeys;
	resolvedDirtyKeys.reserve(dirtyMaterialResourceKeys.size());

	for (uint64_t materialKey : dirtyMaterialResourceKeys)
	{
		auto resourceIt = materialVariantResources.find(materialKey);
		if (resourceIt == materialVariantResources.end())
		{
			resolvedDirtyKeys.push_back(materialKey);
			continue;
		}
		PersistentVoxelMaterialVariantResource& resource = resourceIt->second;
		if (resource.materialCount == 0)
		{
			continue;
		}
		if ((uint64_t)resource.materialOffset + resource.materialCount > materials.size())
		{
			continue;
		}

		const nri_scene::MaterialData* actorMaterials = materials.data() + resource.materialOffset;
		const uint64_t materialSize = (uint64_t)resource.materialCount * sizeof(nri_scene::MaterialData);
		outStats.requestedBytes += materialSize;
		outStats.domainPayloadBytes += materialSize;
		outStats.domainMaterialPayloadBytes += materialSize;
		outStats.domainHashChecks++;
		const uint64_t materialHash = NRIHashUploadPayloadBytes(actorMaterials, materialSize);
		resolvedDirtyKeys.push_back(materialKey);
		const bool uploadMaterials = resource.materialUploadHash != materialHash;
		if (uploadMaterials)
		{
			if (activeMaterialKeys.find(materialKey) != activeMaterialKeys.end())
			{
				outStats.activeHashMisses++;
			}
			outStats.domainHashMisses++;
			outStats.uploads++;
			outStats.dirtyBytes += materialSize;
			dirtyMaterialRanges.push_back({
				ResidentUploadKind_Material,
				(uint64_t)resource.materialOffset * sizeof(nri_scene::MaterialData),
				materialSize,
				materialSize });
			pendingMaterialUploads.push_back({ &resource, materialHash });
		}
		else
		{
			resource.materialUploadHash = materialHash;
		}
	}

	if (dirtyMaterialRanges.empty())
	{
		for (uint64_t materialKey : resolvedDirtyKeys)
		{
			dirtyMaterialResourceKeys.erase(materialKey);
		}
		if (!dirtyMaterialResourceKeys.empty())
		{
			return false;
		}
		uploadedMaterialResourceGeneration = materialResourceGeneration;
		uploadedMaterialPublicationGeneration = batchMaterialPublicationGeneration;
		pendingMaterialLayoutInvalidatedResources = 0;
		pendingMaterialActorRebinds = 0;
		uploadedMaterialTextureKeys.clear();
		uploadedMaterialTextureKeys.reserve(batch.materialBridge.textures.size());
		for (const nri_scene::TextureUpload& texture : batch.materialBridge.textures)
		{
			uploadedMaterialTextureKeys.push_back(texture.key);
		}
		return true;
	}

	constexpr uint64_t kMaterialUploadCoalesceMaxGapBytes = 4ull * 1024ull;
	constexpr uint64_t kMaterialUploadCoalesceMaxByteExpansion = 2ull;
	std::sort(
		dirtyMaterialRanges.begin(),
		dirtyMaterialRanges.end(),
		[](const RuntimeMutationResidentUploadRange& a, const RuntimeMutationResidentUploadRange& b)
		{
			return a.byteOffset < b.byteOffset;
		});

	std::vector<RuntimeMutationResidentUploadRange> coalescedRanges;
	coalescedRanges.reserve(dirtyMaterialRanges.size());
	for (const RuntimeMutationResidentUploadRange& range : dirtyMaterialRanges)
	{
		if (coalescedRanges.empty())
		{
			coalescedRanges.push_back(range);
			continue;
		}

		RuntimeMutationResidentUploadRange& tail = coalescedRanges.back();
		const uint64_t tailEnd = tail.byteOffset + tail.size;
		const uint64_t rangeEnd = range.byteOffset + range.size;
		const uint64_t gapBytes = range.byteOffset > tailEnd ? range.byteOffset - tailEnd : 0;
		const uint64_t candidateSize = rangeEnd > tailEnd ? rangeEnd - tail.byteOffset : tail.size;
		const uint64_t candidateDirtySize = tail.dirtySize + range.size;
		const bool acceptableByteExpansion =
			candidateDirtySize > UINT64_MAX / kMaterialUploadCoalesceMaxByteExpansion ||
			candidateSize <= candidateDirtySize * kMaterialUploadCoalesceMaxByteExpansion;
		if (gapBytes <= kMaterialUploadCoalesceMaxGapBytes && acceptableByteExpansion)
		{
			if (rangeEnd > tailEnd)
			{
				tail.size = rangeEnd - tail.byteOffset;
			}
			tail.dirtySize += range.size;
			continue;
		}

		outStats.batchRejects++;
		coalescedRanges.push_back(range);
	}

	uint64_t uploadedBytes = 0;
	for (const RuntimeMutationResidentUploadRange& range : coalescedRanges)
	{
		uploadedBytes += range.size;
		if (range.size > range.dirtySize)
		{
			outStats.batchGapBytes += range.size - range.dirtySize;
		}
		services.NoteMaterialUpload(range.size);
	}

	outStats.uploadedBytes += uploadedBytes;
	outStats.domainUploadedBytes += uploadedBytes;
	outStats.domainMaterialUploadedBytes += uploadedBytes;

	const uint64_t materialArenaSize = materials.size() * sizeof(nri_scene::MaterialData);
	if (!services.StageMaterialRanges(
		materialBuffer,
		coalescedRanges,
		reinterpret_cast<const uint8_t*>(materials.data()),
		materialArenaSize))
	{
		return false;
	}

	for (const PendingMaterialUpload& upload : pendingMaterialUploads)
	{
		if (upload.resource == nullptr)
		{
			continue;
		}
		upload.resource->materialUploadHash = upload.materialHash;
		if (voxelStatsEnabled)
		{
			const uint64_t materialSize =
				(uint64_t)upload.resource->materialCount * sizeof(nri_scene::MaterialData);
			Printf("PERF pt voxel material variant NRI: frame=%u action=upload reason=arena-sync actor_key=0x0 mat_key=0x%llx ref_count=0 material_offset=%u material_count=%u material_capacity=%u upload_hash=0x%llx upload_bytes=%llu ready=1\n",
				frameIndex,
				(unsigned long long)upload.resource->materialKeyHash,
				upload.resource->materialOffset,
				upload.resource->materialCount,
				upload.resource->materialCapacity,
				(unsigned long long)upload.materialHash,
				(unsigned long long)materialSize);
		}
	}
	for (uint64_t materialKey : resolvedDirtyKeys)
	{
		dirtyMaterialResourceKeys.erase(materialKey);
	}
	if (!dirtyMaterialResourceKeys.empty())
	{
		return false;
	}
	if (dirtyMaterialResourceKeys.empty())
	{
		uploadedMaterialResourceGeneration = materialResourceGeneration;
		uploadedMaterialPublicationGeneration = batchMaterialPublicationGeneration;
		pendingMaterialLayoutInvalidatedResources = 0;
		pendingMaterialActorRebinds = 0;
		uploadedMaterialTextureKeys.clear();
		uploadedMaterialTextureKeys.reserve(batch.materialBridge.textures.size());
		for (const nri_scene::TextureUpload& texture : batch.materialBridge.textures)
		{
			uploadedMaterialTextureKeys.push_back(texture.key);
		}
	}

	return true;
}

bool NRIPersistentVoxelResidency::AppendTlasInstances(
	std::vector<nri::TopLevelInstance>& instances,
	std::vector<SceneInstanceData>& sceneInstances,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	const NRISceneInstanceVisibilityContext& visibilityContext,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelTlasServices& services,
	NRIPersistentVoxelTlasBuildStats& outStats)
{
	constexpr uint32_t PersistentVoxelSceneDataSource = 2u;
	outStats = {};
	committedWorldTlasFrameIndex = UINT32_MAX;
	actorOccurrenceLedger.BeginFrame(frameIndex);
	if (actorOccurrencePolicyFrameIndex != frameIndex)
	{
		EvaluateActorOccurrencePolicies(frameIndex, services.occurrencePolicy);
	}
	NRIActorOccurrenceCensus occurrenceCensus(services.occurrenceTrace, frameIndex);
	auto evaluateActorLedgerState = [](const PersistentVoxelBatch::ActorEntry& actor)
	{
		NRIActorOccurrencePublicationFacts facts;
		facts.owner = { actor.ownerWorldEpoch, actor.ownerLifetimeGeneration };
		facts.placementGeneration = actor.placementGeneration;
		facts.placementStateHash = actor.placementStateHash;
		facts.bindingGeneration = actor.bindingGeneration;
		facts.physicalSectorIndex = actor.physicalSectorIndex;
		facts.roleMask = NRI_ACTOR_OCCURRENCE_ROLE_EXACT;
		facts.exactWorkloadMask = 1u;
		facts.exactBlasHandle = 1u;
		facts.authorityCurrent = actor.authorityCurrent;
		facts.publicationEligible = actor.publicationEligible;
		facts.pendingRemoval = actor.pendingRemoval;
		facts.transform = actor.instanceTransform;
		return EvaluateNRIActorOccurrencePublication(facts);
	};
	auto recordOccurrenceCandidate = [&](const PersistentVoxelBatch::ActorEntry& actor, bool admitted, const char* reason)
	{
		if (!occurrenceCensus.Targets(actor.actorIndex))
		{
			return;
		}
		NRIActorOccurrenceCandidate candidate;
		candidate.identityKey = actor.identityKey;
		candidate.lifecycleGeneration = actor.actorIndex >= 0 ? (uint64_t)(uint32_t)actor.actorIndex : 0;
		candidate.ownerWorldEpoch = actor.ownerWorldEpoch;
		candidate.ownerLifetimeGeneration = actor.ownerLifetimeGeneration;
		candidate.placementGeneration = actor.placementGeneration;
		candidate.placementStateHash = actor.placementStateHash;
		candidate.bindingGeneration = actor.bindingGeneration;
		candidate.publicationHash = actor.worldTlasPublicationHash;
		candidate.meshResourceKey = actor.meshResourceKey;
		candidate.meshKeyHash = actor.meshKeyHash;
		candidate.materialKeyHash = actor.materialKeyHash;
		candidate.actorIndex = actor.actorIndex;
		candidate.physicalSectorIndex = actor.physicalSectorIndex;
		candidate.active = actor.active;
		candidate.capturedThisFrame = actor.capturedThisFrame;
		candidate.admitted = admitted;
		candidate.publishReady = admitted;
		candidate.authorityCurrent = actor.authorityCurrent;
		candidate.publicationEligible = actor.publicationEligible;
		candidate.pendingRemoval = actor.pendingRemoval;
		candidate.ledgerReason = evaluateActorLedgerState(actor).reason;
		candidate.reason = reason != nullptr ? reason : "none";
		occurrenceCensus.RecordCandidate(candidate);
	};
	auto recordSuppressedOccurrenceCandidate = [&](
		const PersistentVoxelBatch::ActorEntry& actor,
		const NRIActorOccurrencePolicyDecision& policyDecision)
	{
		if (!occurrenceCensus.Targets(actor.actorIndex))
		{
			return;
		}
		NRIActorOccurrenceCandidate candidate;
		candidate.identityKey = actor.identityKey;
		candidate.lifecycleGeneration = actor.actorIndex >= 0 ? (uint64_t)(uint32_t)actor.actorIndex : 0;
		candidate.ownerWorldEpoch = actor.ownerWorldEpoch;
		candidate.ownerLifetimeGeneration = actor.ownerLifetimeGeneration;
		candidate.placementGeneration = actor.placementGeneration;
		candidate.placementStateHash = actor.placementStateHash;
		candidate.bindingGeneration = actor.bindingGeneration;
		candidate.publicationHash = actor.worldTlasPublicationHash;
		candidate.meshResourceKey = actor.meshResourceKey;
		candidate.meshKeyHash = actor.meshKeyHash;
		candidate.materialKeyHash = actor.materialKeyHash;
		candidate.actorIndex = actor.actorIndex;
		candidate.physicalSectorIndex = actor.physicalSectorIndex;
		candidate.active = actor.active;
		candidate.capturedThisFrame = actor.capturedThisFrame;
		candidate.admitted = false;
		candidate.publishReady = true;
		candidate.authorityCurrent = actor.authorityCurrent;
		candidate.publicationEligible = actor.publicationEligible;
		candidate.pendingRemoval = actor.pendingRemoval;
		candidate.ledgerReason = evaluateActorLedgerState(actor).reason;
		candidate.suppressionAuthorized = policyDecision.suppress;
		candidate.suppressedWorkloadMask = policyDecision.suppressedWorkloadMask;
		candidate.reason = GetNRIActorOccurrencePolicyReasonName(policyDecision.reason);
		occurrenceCensus.RecordCandidate(candidate);
	};

	std::unordered_set<uint64_t> persistentVoxelTlasMeshResources;
	persistentVoxelTlasMeshResources.reserve(batch.actors.size());
	struct PersistentVoxelTlasGroupStats
	{
		uint64_t meshResourceKey = 0;
		uint64_t meshKeyHash = 0;
		uint32_t primitiveCount = 0;
		uint32_t instanceCount = 0;
		uint32_t capturedCount = 0;
		uint32_t retainedCount = 0;
		uint64_t instancePrimitiveCount = 0;
		uint32_t newInstanceCount = 0;
		uint64_t newInstancePrimitiveCount = 0;
		uint64_t maxRetainedFrameAge = 0;
		uint32_t tlasReadyFrame = 0;
		int32_t resolvedVoxelIndex = -1;
		bool newlyPublished = false;
	};
	const bool tracePersistentVoxelTlasSummary = voxelStatsEnabled;
	std::unordered_map<uint64_t, PersistentVoxelTlasGroupStats> persistentVoxelTlasGroups;
	std::unordered_set<uint64_t> persistentVoxelTlasNewMeshResources;
	if (tracePersistentVoxelTlasSummary)
	{
		persistentVoxelTlasGroups.reserve(batch.actors.size());
		persistentVoxelTlasNewMeshResources.reserve(batch.actors.size());
	}
	uint32_t persistentVoxelTlasCandidateCount = 0;
	uint32_t persistentVoxelTlasPublishedCount = 0;
	uint32_t persistentVoxelTlasSkippedCount = 0;
	uint32_t persistentVoxelTlasMissingSkipCount = 0;
	uint32_t persistentVoxelTlasReadyFrameSkipCount = 0;
	uint32_t persistentVoxelTlasExcludedSkipCount = 0;
	uint32_t persistentVoxelTlasNewInstanceCount = 0;
	uint32_t persistentVoxelTlasNewMeshCount = 0;
	uint32_t persistentVoxelTlasCapturedCount = 0;
	uint32_t persistentVoxelTlasRetainedCount = 0;
	uint64_t persistentVoxelTlasMissingSkipPrimitiveCount = 0;
	uint64_t persistentVoxelTlasReadyFrameSkipPrimitiveCount = 0;
	uint64_t persistentVoxelTlasExcludedSkipPrimitiveCount = 0;
	uint64_t persistentVoxelTlasNewInstancePrimitiveCount = 0;
	uint64_t persistentVoxelTlasNewUniquePrimitiveCount = 0;
	uint64_t persistentVoxelTlasInstancePrimitiveCount = 0;
	uint64_t persistentVoxelTlasUniquePrimitiveCount = 0;
	uint64_t persistentVoxelTlasMaxRetainedFrameAge = 0;
	uint32_t persistentVoxelTlasDirectPublishedCount = 0;
	uint64_t persistentVoxelTlasDirectLatencyFrames = 0;
	uint32_t persistentVoxelTlasDirectMaxLatencyFrames = 0;
	const int32_t persistentVoxelExcludeIndex0 = settings.excludeIndices[0];
	const int32_t persistentVoxelExcludeIndex1 = settings.excludeIndices[1];
	const int32_t persistentVoxelExcludeIndex2 = settings.excludeIndices[2];
	const uint32_t persistentVoxelExcludeMinPrims = settings.excludeMinPrimitives;
	uint64_t persistentVoxelRetainedTlasPrimitives = 0;
	auto computePersistentVoxelRetainedAge = [frameIndex](const PersistentVoxelBatch::ActorEntry& actor) -> uint64_t
	{
		if (actor.capturedThisFrame)
		{
			return 0;
		}
		const uint64_t frameAge = actor.lastSeenFrame != 0 && (uint64_t)frameIndex >= actor.lastSeenFrame ?
			(uint64_t)frameIndex - actor.lastSeenFrame :
			0;
		return std::max(actor.retainedFrameAge, frameAge);
	};
	std::vector<PersistentVoxelBatch::ActorEntry*> persistentVoxelTlasActors;
	persistentVoxelTlasActors.reserve(batch.actors.size());
	std::unordered_map<NRIActorOccurrenceOwnerKey, uint32_t, NRIActorOccurrenceOwnerKeyHash> activeOwnerCounts;
	activeOwnerCounts.reserve(batch.actors.size());
	for (PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		actor.inWorldTlasThisFrame = false;
		actor.worldTlasInstanceIndex = UINT32_MAX;
		actor.worldTlasOccurrenceGeneration = 0u;
		actor.worldTlasPublicationHash = 0u;
		if (actor.active && IsPersistentVoxelActorPublicationCurrent(actor))
		{
			persistentVoxelTlasActors.push_back(&actor);
			activeOwnerCounts[{ actor.ownerWorldEpoch, actor.ownerLifetimeGeneration }]++;
		}
		else if (actor.active && voxelStatsEnabled)
		{
			NRIActorOccurrencePublicationFacts staleFacts;
			staleFacts.owner = { actor.ownerWorldEpoch, actor.ownerLifetimeGeneration };
			staleFacts.placementGeneration = actor.placementGeneration;
			staleFacts.placementStateHash = actor.placementStateHash;
			staleFacts.bindingGeneration = actor.bindingGeneration;
			staleFacts.physicalSectorIndex = actor.physicalSectorIndex;
			staleFacts.roleMask = NRI_ACTOR_OCCURRENCE_ROLE_EXACT;
			staleFacts.exactWorkloadMask = 1u;
			staleFacts.exactBlasHandle = 1u;
			staleFacts.authorityCurrent = actor.authorityCurrent;
			staleFacts.publicationEligible = actor.publicationEligible;
			staleFacts.pendingRemoval = actor.pendingRemoval;
			staleFacts.transform = actor.instanceTransform;
			const NRIActorOccurrenceLedgerDecision decision =
				EvaluateNRIActorOccurrencePublication(staleFacts);
			Printf("PERF pt actor occurrence ledger NRI: frame=%u actor=%d owner_epoch=%llu owner_lifetime=%llu placement=%llu binding=%llu sector=%d action=skip reason=%s\n",
				frameIndex, actor.actorIndex,
				(unsigned long long)actor.ownerWorldEpoch,
				(unsigned long long)actor.ownerLifetimeGeneration,
				(unsigned long long)actor.placementGeneration,
				(unsigned long long)actor.bindingGeneration,
				actor.physicalSectorIndex,
				GetNRIActorOccurrenceLedgerReasonName(decision.reason));
		}
	}
	std::stable_sort(persistentVoxelTlasActors.begin(), persistentVoxelTlasActors.end(),
		[&](const PersistentVoxelBatch::ActorEntry* left, const PersistentVoxelBatch::ActorEntry* right)
		{
			if (left->capturedThisFrame != right->capturedThisFrame)
			{
				return left->capturedThisFrame;
			}
			const uint64_t leftRetainedAge = computePersistentVoxelRetainedAge(*left);
			const uint64_t rightRetainedAge = computePersistentVoxelRetainedAge(*right);
			if (leftRetainedAge != rightRetainedAge)
			{
				return leftRetainedAge < rightRetainedAge;
			}
			if (left->primitiveCount != right->primitiveCount)
			{
				return left->primitiveCount < right->primitiveCount;
			}
			return left->identityKey < right->identityKey;
		});
	auto persistentVoxelTransformFinite = [](const std::array<float, 12>& transform) -> bool
	{
		for (float value : transform)
		{
			if (!std::isfinite(value))
			{
				return false;
			}
		}
		return true;
	};
	auto persistentVoxelTransformIdentity = [](const std::array<float, 12>& transform) -> bool
	{
		constexpr float Epsilon = 0.0001f;
		constexpr float Identity[12] =
		{
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f
		};
		for (uint32_t i = 0; i < 12; ++i)
		{
			if (std::abs(transform[i] - Identity[i]) > Epsilon)
			{
				return false;
			}
		}
		return true;
	};
	if (settings.diagnosticsEnabled)
	{
		NRIPersistentVoxelLocalShareProfileStats localShareProfile = {};
		std::unordered_map<uint64_t, uint32_t> localShareableKeyRefs;
		localShareableKeyRefs.reserve(persistentVoxelTlasActors.size());
		std::unordered_set<uint64_t> localShareableResidentKeys;
		localShareableResidentKeys.reserve(persistentVoxelTlasActors.size());
		for (const PersistentVoxelBatch::ActorEntry* actorPtr : persistentVoxelTlasActors)
		{
			const PersistentVoxelBatch::ActorEntry& actor = *actorPtr;
			localShareProfile.activeActors++;
			auto meshResourceIt = meshVariantResources.find(actor.meshResourceKey);
			if (meshResourceIt == meshVariantResources.end())
			{
				localShareProfile.rejectMissingMesh++;
				continue;
			}
			const PersistentVoxelMeshVariantResource& meshResource = meshResourceIt->second;
			if (meshResource.meshBakeSpace == nri_scene::VoxelMeshBakeSpace::LocalSpace)
			{
				localShareProfile.localSpaceActors++;
				if (persistentVoxelTransformIdentity(actor.instanceTransform))
				{
					localShareProfile.localIdentityTransformActors++;
				}
				else
				{
					localShareProfile.localNonIdentityTransformActors++;
				}
			}
			else if (meshResource.meshBakeSpace == nri_scene::VoxelMeshBakeSpace::BakedTransform)
			{
				localShareProfile.bakedTransformActors++;
				localShareProfile.rejectNonLocal++;
				continue;
			}
			else
			{
				localShareProfile.unknownSpaceActors++;
				localShareProfile.rejectNonLocal++;
				continue;
			}
			if (settings.transformKeyed)
			{
				localShareProfile.transformKeyedActors++;
				localShareProfile.rejectTransformKeyed++;
				continue;
			}
			if (meshResource.vertexBuffer.buffer == nullptr || meshResource.indexBuffer.buffer == nullptr)
			{
				localShareProfile.rejectMissingBuffers++;
				continue;
			}
			if (meshResource.vertexCount == 0 || meshResource.indexCount == 0 || meshResource.primitiveCount == 0 ||
				actor.primitiveCount == 0 || actor.indexCount == 0)
			{
				localShareProfile.rejectInvalidCounts++;
				continue;
			}
			const bool primitiveArenaRangeValid =
				(uint64_t)actor.primitiveOffset + (uint64_t)actor.primitiveCount <= (uint64_t)arenaPrimitiveCursor;
			const bool materialArenaRangeValid =
				actor.materialCount != 0 &&
				(uint64_t)actor.materialOffset + (uint64_t)actor.materialCount <= materialRangeAllocator.Stats().cursorRows;
			if (!primitiveArenaRangeValid || !materialArenaRangeValid)
			{
				localShareProfile.rejectInvalidMaterial++;
				continue;
			}
			const bool meshRangeMatches =
				actor.primitiveOffset == meshResource.primitiveOffset &&
				actor.primitiveCount == meshResource.primitiveCount &&
				actor.indexOffset == meshResource.indexOffset &&
				actor.indexCount == meshResource.indexCount &&
				actor.geometrySignature == meshResource.geometrySignature;
			if (!meshRangeMatches)
			{
				localShareProfile.rejectGeometryMismatch++;
				continue;
			}
			if (!persistentVoxelTransformFinite(actor.instanceTransform) ||
				!persistentVoxelTransformFinite(actor.previousInstanceTransform))
			{
				localShareProfile.rejectInvalidTransform++;
				continue;
			}
			localShareProfile.shareableLocalActors++;
			localShareableKeyRefs[actor.meshResourceKey]++;
			const NRIPersistentVoxelSharedBlasEntry* sharedEntry = sharedBlasCache.Find(actor.meshResourceKey);
			const bool residentSharedEntry =
				sharedEntry != nullptr &&
				sharedEntry->state == NRIPersistentVoxelSharedBlasState::Resident &&
				sharedEntry->accelerationStructure.accelerationStructure != nullptr &&
				sharedEntry->geometrySignature == actor.geometrySignature &&
				sharedEntry->vertexCount == meshResource.vertexCount &&
				sharedEntry->indexCount == meshResource.indexCount &&
				sharedEntry->primitiveCount == meshResource.primitiveCount;
			if (residentSharedEntry)
			{
				localShareableResidentKeys.insert(actor.meshResourceKey);
			}
			else
			{
				localShareProfile.eligibleNotResidentActors++;
			}
		}
		localShareProfile.shareableUniqueKeys = (uint32_t)localShareableKeyRefs.size();
		localShareProfile.residentShareableKeys = (uint32_t)localShareableResidentKeys.size();
		for (const auto& pair : localShareableKeyRefs)
		{
			if (pair.second > 1u)
			{
				localShareProfile.shareableMultiActorKeys++;
				localShareProfile.shareableDuplicateActorRefs += pair.second - 1u;
			}
			else
			{
				localShareProfile.shareableSingleActorKeys++;
			}
		}
		sharedBlasCache.RecordLocalShareProfile(localShareProfile);

		struct SharedKeyAuditGroup
		{
			uint64_t key = 0;
			uint64_t firstGeometrySignature = 0;
			uint64_t lastGeometrySignature = 0;
			uint64_t firstMaterialKeyHash = 0;
			uint64_t lastMaterialKeyHash = 0;
			uint64_t firstTransformBasisSignature = 0;
			uint64_t lastTransformBasisSignature = 0;
			uint32_t firstPrimitiveCount = 0;
			uint32_t lastPrimitiveCount = 0;
			uint32_t firstIndexCount = 0;
			uint32_t lastIndexCount = 0;
			uint32_t firstVertexCount = 0;
			uint32_t lastVertexCount = 0;
			uint32_t firstMaterialCount = 0;
			uint32_t lastMaterialCount = 0;
			uint32_t actorCount = 0;
			int32_t firstSourcePicnum = -1;
			int32_t lastSourcePicnum = -1;
			int32_t firstResolvedVoxelIndex = -1;
			int32_t lastResolvedVoxelIndex = -1;
			nri_scene::VoxelMeshBakeSpace firstBakeSpace = nri_scene::VoxelMeshBakeSpace::Unknown;
			nri_scene::VoxelMeshBakeSpace lastBakeSpace = nri_scene::VoxelMeshBakeSpace::Unknown;
			bool localShareable = true;
			bool geometryMismatch = false;
			bool countMismatch = false;
			bool materialVariant = false;
			bool materialCountMismatch = false;
			bool sourcePicnumAlias = false;
			bool voxelIndexAlias = false;
			bool bakeSpaceMismatch = false;
			bool transformBasisMismatch = false;
		};
		std::unordered_map<uint64_t, SharedKeyAuditGroup> sharedKeyAuditGroups;
		sharedKeyAuditGroups.reserve(persistentVoxelTlasActors.size());
		for (const PersistentVoxelBatch::ActorEntry* actorPtr : persistentVoxelTlasActors)
		{
			const PersistentVoxelBatch::ActorEntry& actor = *actorPtr;
			auto meshResourceIt = meshVariantResources.find(actor.meshResourceKey);
			if (meshResourceIt == meshVariantResources.end())
			{
				continue;
			}
			const PersistentVoxelMeshVariantResource& meshResource = meshResourceIt->second;
			SharedKeyAuditGroup& group = sharedKeyAuditGroups[actor.meshResourceKey];
			const bool firstActorForKey = group.actorCount == 0;
			if (firstActorForKey)
			{
				group.key = actor.meshResourceKey;
				group.firstGeometrySignature = actor.geometrySignature;
				group.firstMaterialKeyHash = actor.materialKeyHash;
				group.firstTransformBasisSignature = meshResource.transformBasisSignature;
				group.firstPrimitiveCount = actor.primitiveCount;
				group.firstIndexCount = actor.indexCount;
				group.firstVertexCount = meshResource.vertexCount;
				group.firstMaterialCount = actor.materialCount;
				group.firstSourcePicnum = actor.sourcePicnum;
				group.firstResolvedVoxelIndex = actor.resolvedVoxelIndex;
				group.firstBakeSpace = meshResource.meshBakeSpace;
			}
			group.actorCount++;
			group.lastGeometrySignature = actor.geometrySignature;
			group.lastMaterialKeyHash = actor.materialKeyHash;
			group.lastTransformBasisSignature = meshResource.transformBasisSignature;
			group.lastPrimitiveCount = actor.primitiveCount;
			group.lastIndexCount = actor.indexCount;
			group.lastVertexCount = meshResource.vertexCount;
			group.lastMaterialCount = actor.materialCount;
			group.lastSourcePicnum = actor.sourcePicnum;
			group.lastResolvedVoxelIndex = actor.resolvedVoxelIndex;
			group.lastBakeSpace = meshResource.meshBakeSpace;
			group.geometryMismatch = group.geometryMismatch ||
				actor.geometrySignature != group.firstGeometrySignature ||
				meshResource.geometrySignature != group.firstGeometrySignature;
			group.countMismatch = group.countMismatch ||
				actor.primitiveCount != group.firstPrimitiveCount ||
				actor.indexCount != group.firstIndexCount ||
				meshResource.vertexCount != group.firstVertexCount ||
				meshResource.indexCount != group.firstIndexCount ||
				meshResource.primitiveCount != group.firstPrimitiveCount;
			group.materialVariant = group.materialVariant || actor.materialKeyHash != group.firstMaterialKeyHash;
			group.materialCountMismatch = group.materialCountMismatch || actor.materialCount != group.firstMaterialCount;
			group.sourcePicnumAlias = group.sourcePicnumAlias || actor.sourcePicnum != group.firstSourcePicnum;
			group.voxelIndexAlias = group.voxelIndexAlias || actor.resolvedVoxelIndex != group.firstResolvedVoxelIndex;
			group.bakeSpaceMismatch = group.bakeSpaceMismatch || meshResource.meshBakeSpace != group.firstBakeSpace;
			group.transformBasisMismatch = group.transformBasisMismatch ||
				meshResource.transformBasisSignature != group.firstTransformBasisSignature;
			const bool primitiveArenaRangeValid =
				(uint64_t)actor.primitiveOffset + (uint64_t)actor.primitiveCount <= (uint64_t)arenaPrimitiveCursor;
			const bool materialArenaRangeValid =
				actor.materialCount != 0 &&
				(uint64_t)actor.materialOffset + (uint64_t)actor.materialCount <= materialRangeAllocator.Stats().cursorRows;
			const bool meshRangeMatches =
				actor.primitiveOffset == meshResource.primitiveOffset &&
				actor.primitiveCount == meshResource.primitiveCount &&
				actor.indexOffset == meshResource.indexOffset &&
				actor.indexCount == meshResource.indexCount &&
				actor.geometrySignature == meshResource.geometrySignature;
			group.localShareable = group.localShareable &&
				meshResource.meshBakeSpace == nri_scene::VoxelMeshBakeSpace::LocalSpace &&
				!settings.transformKeyed &&
				meshResource.vertexBuffer.buffer != nullptr &&
				meshResource.indexBuffer.buffer != nullptr &&
				meshResource.vertexCount != 0 &&
				meshResource.indexCount != 0 &&
				meshResource.primitiveCount != 0 &&
				actor.primitiveCount != 0 &&
				actor.indexCount != 0 &&
				primitiveArenaRangeValid &&
				materialArenaRangeValid &&
				meshRangeMatches &&
				persistentVoxelTransformFinite(actor.instanceTransform) &&
				persistentVoxelTransformFinite(actor.previousInstanceTransform);
		}
		NRIPersistentVoxelSharedKeyAuditStats sharedKeyAudit = {};
		sharedKeyAudit.keys = (uint32_t)sharedKeyAuditGroups.size();
		for (const auto& pair : sharedKeyAuditGroups)
		{
			const SharedKeyAuditGroup& group = pair.second;
			sharedKeyAudit.actors += group.actorCount;
			const bool unsafe =
				group.geometryMismatch ||
				group.countMismatch ||
				group.bakeSpaceMismatch ||
				group.transformBasisMismatch;
			if (unsafe)
			{
				sharedKeyAudit.unsafeKeys++;
				if (group.localShareable)
				{
					sharedKeyAudit.localShareableUnsafeKeys++;
				}
			}
			else
			{
				sharedKeyAudit.safeKeys++;
			}
			if (group.geometryMismatch)
			{
				sharedKeyAudit.geometryMismatchKeys++;
			}
			if (group.countMismatch)
			{
				sharedKeyAudit.countMismatchKeys++;
			}
			if (group.materialVariant)
			{
				sharedKeyAudit.materialVariantKeys++;
			}
			if (group.materialCountMismatch)
			{
				sharedKeyAudit.materialCountMismatchKeys++;
			}
			if (group.sourcePicnumAlias)
			{
				sharedKeyAudit.sourcePicnumAliasKeys++;
				sharedKeyAudit.sourceStateAliasActorRefs += group.actorCount > 1u ? group.actorCount - 1u : 0u;
			}
			if (group.voxelIndexAlias)
			{
				sharedKeyAudit.voxelIndexAliasKeys++;
				if (!group.sourcePicnumAlias)
				{
					sharedKeyAudit.sourceStateAliasActorRefs += group.actorCount > 1u ? group.actorCount - 1u : 0u;
				}
			}
			if (group.bakeSpaceMismatch)
			{
				sharedKeyAudit.bakeSpaceMismatchKeys++;
			}
			if (group.transformBasisMismatch)
			{
				sharedKeyAudit.transformBasisMismatchKeys++;
			}
		}
		sharedBlasCache.RecordSharedKeyAudit(sharedKeyAudit);
		NRIPersistentVoxelLocalSpaceInvariantStats localInvariants = {};
		struct LocalSpaceInvariantSuspect
		{
			uint64_t actorKey = 0;
			uint64_t meshResourceKey = 0;
			uint64_t meshKeyHash = 0;
			int32_t sourcePicnum = -1;
			int32_t resolvedVoxelIndex = -1;
			float boundsCenterMagnitude = 0.0f;
			float boundsMaxAbs = 0.0f;
			float boundsMin[3] = {};
			float boundsMax[3] = {};
			std::array<float, 12> transform = {};
		};
		std::vector<LocalSpaceInvariantSuspect> invariantSuspects;
		constexpr float SuspiciousLocalBoundsCenterMagnitude = 4096.0f;
		for (const PersistentVoxelBatch::ActorEntry* actorPtr : persistentVoxelTlasActors)
		{
			const PersistentVoxelBatch::ActorEntry& actor = *actorPtr;
			auto meshResourceIt = meshVariantResources.find(actor.meshResourceKey);
			if (meshResourceIt == meshVariantResources.end())
			{
				continue;
			}
			const PersistentVoxelMeshVariantResource& meshResource = meshResourceIt->second;
			const bool transformValid =
				persistentVoxelTransformFinite(actor.instanceTransform) &&
				persistentVoxelTransformFinite(actor.previousInstanceTransform);
			if (!transformValid)
			{
				localInvariants.invalidTransformActors++;
			}
			if (meshResource.meshBakeSpace == nri_scene::VoxelMeshBakeSpace::LocalSpace)
			{
				localInvariants.localActors++;
				if (persistentVoxelTransformIdentity(actor.instanceTransform))
				{
					localInvariants.localIdentityTransformActors++;
				}
				else
				{
					localInvariants.localNonIdentityTransformActors++;
				}
				if (!meshResource.boundsValid)
				{
					localInvariants.missingBoundsActors++;
					continue;
				}
				localInvariants.maxBoundsCenterMagnitude = std::max(localInvariants.maxBoundsCenterMagnitude, meshResource.boundsCenterMagnitude);
				localInvariants.maxBoundsAbs = std::max(localInvariants.maxBoundsAbs, meshResource.boundsMaxAbs);
				if (meshResource.boundsCenterMagnitude > SuspiciousLocalBoundsCenterMagnitude)
				{
					localInvariants.suspiciousWorldBoundsActors++;
					LocalSpaceInvariantSuspect suspect = {};
					suspect.actorKey = actor.identityKey;
					suspect.meshResourceKey = actor.meshResourceKey;
					suspect.meshKeyHash = actor.meshKeyHash;
					suspect.sourcePicnum = actor.sourcePicnum;
					suspect.resolvedVoxelIndex = actor.resolvedVoxelIndex;
					suspect.boundsCenterMagnitude = meshResource.boundsCenterMagnitude;
					suspect.boundsMaxAbs = meshResource.boundsMaxAbs;
					for (uint32_t axis = 0; axis < 3; ++axis)
					{
						suspect.boundsMin[axis] = meshResource.boundsMin[axis];
						suspect.boundsMax[axis] = meshResource.boundsMax[axis];
					}
					suspect.transform = actor.instanceTransform;
					invariantSuspects.push_back(suspect);
				}
			}
			else if (meshResource.meshBakeSpace == nri_scene::VoxelMeshBakeSpace::BakedTransform)
			{
				localInvariants.bakedFallbackActors++;
			}
			else
			{
				localInvariants.unknownSpaceActors++;
			}
		}
		sharedBlasCache.RecordLocalSpaceInvariantStats(localInvariants);
		if (!invariantSuspects.empty())
		{
			std::sort(invariantSuspects.begin(), invariantSuspects.end(), [](const LocalSpaceInvariantSuspect& left, const LocalSpaceInvariantSuspect& right)
			{
				if (left.boundsCenterMagnitude != right.boundsCenterMagnitude)
				{
					return left.boundsCenterMagnitude > right.boundsCenterMagnitude;
				}
				return left.actorKey < right.actorKey;
			});
			const uint32_t emitCount = std::min<uint32_t>(8u, (uint32_t)invariantSuspects.size());
			for (uint32_t i = 0; i < emitCount; ++i)
			{
				const LocalSpaceInvariantSuspect& suspect = invariantSuspects[i];
				Printf("PERF pt voxel local invariant suspect NRI: frame=%u rank=%u actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx pic=%d voxel=%d bounds_center=%.3f bounds_max_abs=%.3f bounds_min=(%.3f,%.3f,%.3f) bounds_max=(%.3f,%.3f,%.3f) transform_t=(%.3f,%.3f,%.3f)\n",
					frameIndex,
					i + 1u,
					(unsigned long long)suspect.actorKey,
					(unsigned long long)suspect.meshResourceKey,
					(unsigned long long)suspect.meshKeyHash,
					suspect.sourcePicnum,
					suspect.resolvedVoxelIndex,
					suspect.boundsCenterMagnitude,
					suspect.boundsMaxAbs,
					suspect.boundsMin[0],
					suspect.boundsMin[1],
					suspect.boundsMin[2],
					suspect.boundsMax[0],
					suspect.boundsMax[1],
					suspect.boundsMax[2],
					suspect.transform[3],
					suspect.transform[7],
					suspect.transform[11]);
			}
		}
		if (sharedKeyAudit.unsafeKeys != 0)
		{
			uint32_t emitted = 0;
			for (const auto& pair : sharedKeyAuditGroups)
			{
				const SharedKeyAuditGroup& group = pair.second;
				const bool unsafe =
					group.geometryMismatch ||
					group.countMismatch ||
					group.bakeSpaceMismatch ||
					group.transformBasisMismatch;
				if (!unsafe)
				{
					continue;
				}
				Printf("PERF pt voxel shared key collision NRI: frame=%u key=0x%llx actors=%u geometry_mismatch=%u count_mismatch=%u bake_space_mismatch=%u transform_basis_mismatch=%u local_shareable=%u first_geo=0x%llx last_geo=0x%llx first_prims=%u last_prims=%u first_indices=%u last_indices=%u first_vertices=%u last_vertices=%u first_pic=%d last_pic=%d first_voxel=%d last_voxel=%d first_space=%s last_space=%s first_basis=0x%llx last_basis=0x%llx\n",
					frameIndex,
					(unsigned long long)group.key,
					group.actorCount,
					group.geometryMismatch ? 1u : 0u,
					group.countMismatch ? 1u : 0u,
					group.bakeSpaceMismatch ? 1u : 0u,
					group.transformBasisMismatch ? 1u : 0u,
					group.localShareable ? 1u : 0u,
					(unsigned long long)group.firstGeometrySignature,
					(unsigned long long)group.lastGeometrySignature,
					group.firstPrimitiveCount,
					group.lastPrimitiveCount,
					group.firstIndexCount,
					group.lastIndexCount,
					group.firstVertexCount,
					group.lastVertexCount,
					group.firstSourcePicnum,
					group.lastSourcePicnum,
					group.firstResolvedVoxelIndex,
					group.lastResolvedVoxelIndex,
					GetPersistentVoxelBakeSpaceName(group.firstBakeSpace),
					GetPersistentVoxelBakeSpaceName(group.lastBakeSpace),
					(unsigned long long)group.firstTransformBasisSignature,
					(unsigned long long)group.lastTransformBasisSignature);
				emitted++;
				if (emitted >= 8u)
				{
					break;
				}
			}
		}
	}
	NRIRaySceneBuilder raySceneBuilder(instances, sceneInstances);
	struct PendingShadowProxyInstance
	{
		nri::TopLevelInstance instance = {};
		SceneInstanceData scene = {};
		uint64_t actorIdentityKey = 0;
		uint64_t meshResourceKey = 0;
		uint32_t exactPrimitiveCount = 0;
		uint32_t proxyPrimitiveCount = 0;
		NRIActorOccurrence diagnosticOccurrence;
	};
	std::vector<PendingShadowProxyInstance> pendingShadowProxyInstances;
	pendingShadowProxyInstances.reserve(persistentVoxelTlasActors.size());
	for (PersistentVoxelBatch::ActorEntry* actorPtr : persistentVoxelTlasActors)
	{
		PersistentVoxelBatch::ActorEntry& actor = *actorPtr;
		persistentVoxelTlasCandidateCount++;
		const NRIActorOccurrenceOwnerKey actorOwner = {
			actor.ownerWorldEpoch, actor.ownerLifetimeGeneration
		};
		if (activeOwnerCounts[actorOwner] != 1u)
		{
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasExcludedSkipCount++;
			persistentVoxelTlasExcludedSkipPrimitiveCount += actor.primitiveCount;
			recordOccurrenceCandidate(actor, false, "duplicate-owner");
			if (voxelStatsEnabled)
			{
				Printf("PERF pt actor occurrence ledger NRI: frame=%u actor=%d owner_epoch=%llu owner_lifetime=%llu placement=%llu action=skip reason=duplicate-owner\n",
					frameIndex, actor.actorIndex,
					(unsigned long long)actor.ownerWorldEpoch,
					(unsigned long long)actor.ownerLifetimeGeneration,
					(unsigned long long)actor.placementGeneration);
			}
			continue;
		}
		const auto policyIt = actorOccurrencePolicyDecisions.find(actor.actorIndex);
		const NRIActorOccurrencePolicyDecision* policyDecision =
			policyIt != actorOccurrencePolicyDecisions.end() ? &policyIt->second : nullptr;
		if (policyDecision != nullptr)
		{
			occurrenceCensus.RecordPolicyDecision(*policyDecision);
			if (voxelStatsEnabled || occurrenceCensus.Targets(actor.actorIndex))
			{
				const auto residentMeshIt = meshVariantResources.find(actor.meshResourceKey);
				const bool blasResident = residentMeshIt != meshVariantResources.end() &&
					residentMeshIt->second.accelerationStructure.accelerationStructure != nullptr;
				Printf("NRI PT actor occurrence policy: frame=%u actor=%d actor_key=0x%llx enabled=%u logical_main=%u context_risks=0x%x action=%s reason=%s mask=0x%x cache_resident=1 blas_resident=%u captured=%u census_sphere=%u physical_sector=%d physical_chunk=%u root_sector=%d conflict_positive=%u conflict_negative=%u bounds_overlap=%u actor_inside=%u\n",
					frameIndex, actor.actorIndex, (unsigned long long)actor.identityKey,
					services.occurrencePolicy.enabled ? 1u : 0u,
					services.occurrencePolicy.logicalMainRoot ? 1u : 0u,
					services.occurrencePolicy.contextRiskFlags,
					policyDecision->suppress ? "suppress" : "keep",
					GetNRIActorOccurrencePolicyReasonName(policyDecision->reason),
					policyDecision->suppressedWorkloadMask,
					blasResident ? 1u : 0u,
					policyDecision->capturedThisFrame ? 1u : 0u,
					policyDecision->insideCensusSphere ? 1u : 0u,
					policyDecision->physicalSectorIndex, policyDecision->physicalChunkIndex,
					policyDecision->rootSectorIndex, policyDecision->conflictPositiveChunk,
					policyDecision->conflictNegativeChunk,
					policyDecision->boundsOverlapConflict ? 1u : 0u,
					policyDecision->actorPositionInsideConflict ? 1u : 0u);
			}
			if (policyDecision->suppress)
			{
				recordSuppressedOccurrenceCandidate(actor, *policyDecision);
				outStats.actorOccurrenceSuppressedCount++;
				outStats.suppressedActorIndices.push_back(actor.actorIndex);
				persistentVoxelTlasSkippedCount++;
				persistentVoxelTlasExcludedSkipCount++;
				persistentVoxelTlasExcludedSkipPrimitiveCount += actor.primitiveCount;
				continue;
			}
		}
		const bool omittedByDiagnostic = settings.omitTlasOccurrences;
		const bool excludedByIndex = actor.resolvedVoxelIndex >= 0 &&
			(actor.resolvedVoxelIndex == persistentVoxelExcludeIndex0 ||
				actor.resolvedVoxelIndex == persistentVoxelExcludeIndex1 ||
				actor.resolvedVoxelIndex == persistentVoxelExcludeIndex2);
		const bool excludedByPrimitiveCount = persistentVoxelExcludeMinPrims > 0 &&
			actor.primitiveCount >= persistentVoxelExcludeMinPrims;
		if (omittedByDiagnostic || excludedByIndex || excludedByPrimitiveCount)
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel tlas NRI: frame=%u action=skip reason=%s actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx voxel=%d instance_id=%u primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u blas=0 tlas_ready=0 tlas_published=0 ready=0\n",
					frameIndex,
					omittedByDiagnostic ? "diagnostic-omit-all" : (excludedByIndex ? "excluded-index" : "excluded-prims"),
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					(unsigned long long)actor.materialKeyHash,
					actor.resolvedVoxelIndex,
					(uint32_t)sceneInstances.size(),
					actor.primitiveOffset,
					actor.primitiveCount,
					actor.materialOffset,
					actor.materialCount);
			}
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasExcludedSkipCount++;
			persistentVoxelTlasExcludedSkipPrimitiveCount += actor.primitiveCount;
			recordOccurrenceCandidate(actor, false,
				omittedByDiagnostic ? "diagnostic-omit-all" : (excludedByIndex ? "excluded-index" : "excluded-prims"));
			continue;
		}
		const uint64_t actorRetainedFrameAge = computePersistentVoxelRetainedAge(actor);

		auto meshResourceIt = meshVariantResources.find(actor.meshResourceKey);
		auto materialResourceIt = materialVariantResources.find(actor.materialKeyHash);
		const char* tlasSkipReason = nullptr;
		if (meshResourceIt == meshVariantResources.end())
		{
			tlasSkipReason = "missing-mesh";
		}
		else if (materialResourceIt == materialVariantResources.end())
		{
			tlasSkipReason = "missing-material";
		}
		else if (!materialRangeAllocator.Owns(PersistentVoxelMaterialRangeHandle(materialResourceIt->second)) ||
			!PersistentVoxelMaterialRangeMatches(actor, materialResourceIt->second))
		{
			tlasSkipReason = "material-handle-mismatch";
		}
		else if (meshResourceIt->second.accelerationStructure.accelerationStructure == nullptr)
		{
			tlasSkipReason = "missing-blas";
		}
		else if (!meshResourceIt->second.directComputePublished &&
			(meshResourceIt->second.indexBuffer.shaderView == nullptr ||
			 meshResourceIt->second.vertexBuffer.shaderView == nullptr))
		{
			tlasSkipReason = "missing-mesh-view";
		}
		else if (vertexBuffer.shaderView == nullptr ||
			indexBuffer.shaderView == nullptr ||
			primitiveBuffer.shaderView == nullptr ||
			materialBuffer.shaderView == nullptr)
		{
			tlasSkipReason = "missing-arena-view";
		}
		if (tlasSkipReason != nullptr)
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel tlas NRI: frame=%u action=skip reason=%s actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx instance_id=%u primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u blas=0 tlas_ready=0 tlas_published=0 ready=0\n",
					frameIndex,
					tlasSkipReason,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					(unsigned long long)actor.materialKeyHash,
					(uint32_t)sceneInstances.size(),
					actor.primitiveOffset,
					actor.primitiveCount,
					actor.materialOffset,
					actor.materialCount);
			}
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasMissingSkipCount++;
			persistentVoxelTlasMissingSkipPrimitiveCount += actor.primitiveCount;
			recordOccurrenceCandidate(actor, false, tlasSkipReason);
			continue;
		}
		const bool primitiveArenaRangeValid =
			(uint64_t)actor.primitiveOffset + (uint64_t)actor.primitiveCount <= (uint64_t)arenaPrimitiveCursor;
		const bool materialArenaRangeValid =
			(uint64_t)actor.materialOffset + (uint64_t)actor.materialCount <= materialRangeAllocator.Stats().cursorRows;
		const bool meshRangeMatches =
			actor.primitiveOffset == meshResourceIt->second.primitiveOffset &&
			actor.primitiveCount == meshResourceIt->second.primitiveCount &&
			actor.indexOffset == meshResourceIt->second.indexOffset &&
			actor.indexCount == meshResourceIt->second.indexCount;
		const bool transformValid =
			persistentVoxelTransformFinite(actor.instanceTransform) &&
			persistentVoxelTransformFinite(actor.previousInstanceTransform);
		const char* invalidTlasReason = nullptr;
		if (!primitiveArenaRangeValid)
		{
			invalidTlasReason = "invalid-primitive-range";
		}
		else if (!materialArenaRangeValid)
		{
			invalidTlasReason = "invalid-material-range";
		}
		else if (!meshRangeMatches)
		{
			invalidTlasReason = "mesh-range-mismatch";
		}
		else if (!transformValid)
		{
			invalidTlasReason = "invalid-transform";
		}
		if (invalidTlasReason != nullptr)
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel tlas NRI: frame=%u action=skip reason=%s actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx instance_id=%u primitive_offset=%u primitive_count=%u primitive_cursor=%u mesh_primitive_offset=%u mesh_primitive_count=%u index_offset=%u index_count=%u mesh_index_offset=%u mesh_index_count=%u material_offset=%u material_count=%u material_cursor=%u blas=1 tlas_ready=%u tlas_published=%u ready=0\n",
					frameIndex,
					invalidTlasReason,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					(unsigned long long)actor.materialKeyHash,
					(uint32_t)sceneInstances.size(),
					actor.primitiveOffset,
					actor.primitiveCount,
					arenaPrimitiveCursor,
					meshResourceIt->second.primitiveOffset,
					meshResourceIt->second.primitiveCount,
					actor.indexOffset,
					actor.indexCount,
					meshResourceIt->second.indexOffset,
					meshResourceIt->second.indexCount,
					actor.materialOffset,
					actor.materialCount,
					(uint32_t)materialRangeAllocator.Stats().cursorRows,
					meshResourceIt->second.tlasReadyFrame,
					meshResourceIt->second.tlasPublished ? 1u : 0u);
			}
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasMissingSkipCount++;
			persistentVoxelTlasMissingSkipPrimitiveCount += actor.primitiveCount;
			recordOccurrenceCandidate(actor, false, invalidTlasReason);
			continue;
		}
		if (!meshResourceIt->second.tlasPublished &&
			meshResourceIt->second.tlasReadyFrame > frameIndex)
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel tlas NRI: frame=%u action=skip reason=ready-frame actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx instance_id=%u primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u blas=1 tlas_ready=%u tlas_published=%u ready=0\n",
					frameIndex,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					(unsigned long long)actor.materialKeyHash,
					(uint32_t)sceneInstances.size(),
					actor.primitiveOffset,
					actor.primitiveCount,
					actor.materialOffset,
					actor.materialCount,
					meshResourceIt->second.tlasReadyFrame,
					meshResourceIt->second.tlasPublished ? 1u : 0u);
			}
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasReadyFrameSkipCount++;
			persistentVoxelTlasReadyFrameSkipPrimitiveCount += actor.primitiveCount;
			recordOccurrenceCandidate(actor, false, "ready-frame");
			continue;
		}
		const bool meshResourceFirstPublish = !meshResourceIt->second.tlasPublished;
		const bool meshResourceNewThisFrame = meshResourceFirstPublish ||
			(tracePersistentVoxelTlasSummary && persistentVoxelTlasNewMeshResources.find(actor.meshResourceKey) != persistentVoxelTlasNewMeshResources.end());

		nri::TopLevelInstance persistentVoxelInstance = {};
		for (uint32_t row = 0; row < 3; ++row)
		{
			for (uint32_t column = 0; column < 4; ++column)
			{
				persistentVoxelInstance.transform[row][column] = actor.instanceTransform[row * 4u + column];
			}
		}
		const NRISceneInstanceVisibility instanceVisibility =
			ResolveNRIPersistentVoxelInstanceVisibility(actor.indirectOnly, actor.actorIndex, visibilityContext);
		persistentVoxelInstance.mask = instanceVisibility.tlasMask;
		persistentVoxelInstance.shaderBindingTableLocalOffset = 0;
		persistentVoxelInstance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		const NRIAccelerationStructureResource* selectedAccelerationStructure = &meshResourceIt->second.accelerationStructure;
		bool routedThroughSharedBlas = false;
		const char* sharedRouteFallbackReason = nullptr;
		if (settings.sharedBlasRouteEnabled)
		{
			const PersistentVoxelSharedBlasRouteEvaluation sharedRoute = EvaluatePersistentVoxelSharedBlasRoute(
				actor,
				meshResourceIt->second,
				settings,
				arenaPrimitiveCursor,
				(uint32_t)materialRangeAllocator.Stats().cursorRows,
				sharedBlasCache);
			if (sharedRoute.routeEligible)
			{
				sharedBlasCache.RecordRouteEligibleActor();
			}
			if (sharedRoute.canRoute && sharedRoute.sharedEntry != nullptr)
			{
				selectedAccelerationStructure = &sharedRoute.sharedEntry->accelerationStructure;
				routedThroughSharedBlas = true;
			}
			else
			{
				sharedRouteFallbackReason = sharedRoute.rejectReason;
			}
		}
		persistentVoxelInstance.accelerationStructureHandle = services.GetAccelerationStructureHandle(*selectedAccelerationStructure);
		if (persistentVoxelInstance.accelerationStructureHandle == 0 && routedThroughSharedBlas)
		{
			selectedAccelerationStructure = &meshResourceIt->second.accelerationStructure;
			routedThroughSharedBlas = false;
			sharedRouteFallbackReason = "missing-resident";
			persistentVoxelInstance.accelerationStructureHandle = services.GetAccelerationStructureHandle(*selectedAccelerationStructure);
		}
		if (persistentVoxelInstance.accelerationStructureHandle == 0)
		{
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasMissingSkipCount++;
			persistentVoxelTlasMissingSkipPrimitiveCount += actor.primitiveCount;
			recordOccurrenceCandidate(actor, false, "missing-as-handle");
			continue;
		}

		NRIVoxelRepresentationFacts representationFacts = {};
		representationFacts.sourceIdentityKey = actor.identityKey;
		representationFacts.meshResourceKey = actor.meshResourceKey;
		representationFacts.materialKeyHash = actor.materialKeyHash;
		representationFacts.actorIndex = actor.actorIndex;
		representationFacts.resolvedVoxelIndex = actor.resolvedVoxelIndex;
		representationFacts.primitiveCount = actor.primitiveCount;
		representationFacts.retainedFrameAge = actorRetainedFrameAge;
		representationFacts.workloadMask = (uint8_t)persistentVoxelInstance.mask;
		representationFacts.capturedThisFrame = actor.capturedThisFrame;
		representationFacts.routedThroughSharedBlas = routedThroughSharedBlas;
		representationFacts.boundsValid = meshResourceIt->second.boundsValid;
		NRIVoxelShadowProxyRejectReason shadowProxyMaterialReason = NRIVoxelShadowProxyRejectReason::None;
		const NRIVoxelShadowProxyResource& shadowProxy = meshResourceIt->second.shadowProxy;
		const bool shadowProxyMaterialCertified =
			settings.shadowProxyRouteEnabled &&
			shadowProxy.state == NRIVoxelShadowProxyResourceState::Resident &&
			CertifyNRIVoxelShadowProxyMaterialClosure(
				materialResourceIt->second.materialBridge,
				!actor.lightRecords.empty(),
				shadowProxyMaterialReason);
		const bool shadowProxyResourceCompatible =
			settings.shadowProxyRouteEnabled &&
			meshResourceIt->second.meshBakeSpace == nri_scene::VoxelMeshBakeSpace::LocalSpace &&
			meshResourceIt->second.shadowProxyPrimitiveSemanticsCertified &&
			shadowProxy.state == NRIVoxelShadowProxyResourceState::Resident &&
			shadowProxy.accelerationStructure.accelerationStructure != nullptr &&
			shadowProxy.readyFrame <= frameIndex &&
			shadowProxy.sourceModel == meshResourceIt->second.sourceModel &&
			shadowProxy.geometrySignature == meshResourceIt->second.geometrySignature &&
			shadowProxy.exactPrimitiveCount == actor.primitiveCount &&
			shadowProxy.proxyPrimitiveCount != 0u &&
			shadowProxy.proxyPrimitiveCount <= actor.primitiveCount &&
			IsNRIVoxelShadowProxyTransformValid(actor.instanceTransform) &&
			IsNRIVoxelShadowProxyTransformValid(actor.previousInstanceTransform);
		const uint64_t shadowProxyHandle = shadowProxyResourceCompatible && shadowProxyMaterialCertified ?
			services.GetAccelerationStructureHandle(shadowProxy.accelerationStructure) : 0ull;
		representationFacts.shadowProxyCertified = shadowProxyResourceCompatible && shadowProxyMaterialCertified;
		representationFacts.shadowProxyReady = representationFacts.shadowProxyCertified && shadowProxyHandle != 0ull;
		representationFacts.shadowProxyPrimitiveCount = shadowProxy.proxyPrimitiveCount;
		uint64_t shadowProxyCompatibilityKey = nri_scene::HashCombine64(
			shadowProxy.sourceArchiveSerial, shadowProxy.sourceContentHash);
		shadowProxyCompatibilityKey = nri_scene::HashCombine64(shadowProxyCompatibilityKey, shadowProxy.geometrySignature);
		shadowProxyCompatibilityKey = nri_scene::HashCombine64(shadowProxyCompatibilityKey, materialResourceIt->second.materialSignature);
		shadowProxyCompatibilityKey = nri_scene::HashCombine64(shadowProxyCompatibilityKey, actor.materialSlotGeneration);
		representationFacts.shadowProxyCompatibilityKey = shadowProxyCompatibilityKey;
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			representationFacts.boundsMin[axis] = meshResourceIt->second.boundsMin[axis];
			representationFacts.boundsMax[axis] = meshResourceIt->second.boundsMax[axis];
		}
		representationFacts.transform = actor.instanceTransform;
		const NRIVoxelRepresentationDecision representationDecision =
			services.EvaluateRepresentation(representationFacts);
		const bool proxyMaskContract =
			(representationDecision.proxyWorkloadMask & ~(uint8_t)NRI_TLAS_MASK_SHADOW) == 0u &&
			(representationDecision.exactWorkloadMask & representationDecision.proxyWorkloadMask) == 0u &&
			(uint8_t)(representationDecision.exactWorkloadMask | representationDecision.proxyWorkloadMask) == representationFacts.workloadMask;
		const bool canUseShadowProxy =
			representationDecision.representation == NRIVoxelRepresentationKind::ExactWithCertifiedShadowProxy &&
			representationFacts.shadowProxyReady &&
			representationDecision.proxyWorkloadMask != 0u &&
			proxyMaskContract;
		const uint8_t exactWorkloadMask = canUseShadowProxy ?
			representationDecision.exactWorkloadMask : representationFacts.workloadMask;
		const uint8_t proxyWorkloadMask = canUseShadowProxy ?
			representationDecision.proxyWorkloadMask : 0u;
		if (representationDecision.representation != NRIVoxelRepresentationKind::Exact && !canUseShadowProxy && voxelStatsEnabled)
		{
			Printf("PERF pt voxel representation invariant NRI: frame=%u actor_key=0x%llx actor=%d requested_mask=0x%x exact_mask=0x%x proxy_mask=0x%x representation=%s action=force-exact reason=proxy-contract\n",
				frameIndex,
				(unsigned long long)actor.identityKey,
				actor.actorIndex,
				(uint32_t)representationFacts.workloadMask,
				(uint32_t)representationDecision.exactWorkloadMask,
				(uint32_t)representationDecision.proxyWorkloadMask,
				GetNRIVoxelRepresentationKindName(representationDecision.representation));
		}
		persistentVoxelInstance.mask = exactWorkloadMask;
		actor.bindingGeneration = BuildPersistentVoxelActorBindingGeneration(actor);
		auto publicationInstance = this->instances.find(actor.identityKey);
		if (publicationInstance != this->instances.end())
		{
			publicationInstance->second.bindingGeneration = actor.bindingGeneration;
		}
		NRIActorOccurrencePublicationFacts publicationFacts;
		publicationFacts.owner = actorOwner;
		publicationFacts.placementGeneration = actor.placementGeneration;
		publicationFacts.placementStateHash = actor.placementStateHash;
		publicationFacts.bindingGeneration = actor.bindingGeneration;
		publicationFacts.physicalSectorIndex = actor.physicalSectorIndex;
		publicationFacts.roleMask =
			(exactWorkloadMask != 0u ? NRI_ACTOR_OCCURRENCE_ROLE_EXACT : 0u) |
			(proxyWorkloadMask != 0u ? NRI_ACTOR_OCCURRENCE_ROLE_SHADOW_PROXY : 0u) |
			(actor.lightRecords.empty() ? 0u : NRI_ACTOR_OCCURRENCE_ROLE_EMISSIVE_SURFACE) |
			(actor.actorIndex >= 0 ? NRI_ACTOR_OCCURRENCE_ROLE_LIGHT_ANCHOR : 0u);
		publicationFacts.exactWorkloadMask = exactWorkloadMask;
		publicationFacts.proxyWorkloadMask = proxyWorkloadMask;
		publicationFacts.exactBlasHandle = persistentVoxelInstance.accelerationStructureHandle;
		publicationFacts.proxyBlasHandle = proxyWorkloadMask != 0u ? shadowProxyHandle : 0u;
		publicationFacts.authorityCurrent = actor.authorityCurrent;
		publicationFacts.publicationEligible = actor.publicationEligible;
		publicationFacts.pendingRemoval = actor.pendingRemoval;
		publicationFacts.transform = actor.instanceTransform;
		const NRIActorOccurrenceLedgerDecision publicationDecision =
			actorOccurrenceLedger.CommitPublication(publicationFacts);
		if (!publicationDecision.eligible)
		{
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasExcludedSkipCount++;
			persistentVoxelTlasExcludedSkipPrimitiveCount += actor.primitiveCount;
			recordOccurrenceCandidate(actor, false,
				GetNRIActorOccurrenceLedgerReasonName(publicationDecision.reason));
			if (voxelStatsEnabled)
			{
				Printf("PERF pt actor occurrence ledger NRI: frame=%u actor=%d owner_epoch=%llu owner_lifetime=%llu placement=%llu binding=%llu sector=%d role=0x%x mask=0x%x action=skip reason=%s\n",
					frameIndex, actor.actorIndex,
					(unsigned long long)actor.ownerWorldEpoch,
					(unsigned long long)actor.ownerLifetimeGeneration,
					(unsigned long long)actor.placementGeneration,
					(unsigned long long)actor.bindingGeneration,
					actor.physicalSectorIndex,
					publicationFacts.roleMask,
					publicationFacts.exactWorkloadMask | publicationFacts.proxyWorkloadMask,
					GetNRIActorOccurrenceLedgerReasonName(publicationDecision.reason));
			}
			continue;
		}
		actor.worldTlasPublicationHash = publicationDecision.publicationHash;
		SceneInstanceData sceneInstance = {};
		sceneInstance.primitiveBase = actor.primitiveOffset;
		sceneInstance.dataSource = PersistentVoxelSceneDataSource;
		sceneInstance.materialBase = actor.materialOffset;
		sceneInstance.materialCount = actor.materialCount;
		sceneInstance.visibilityChunk = actor.visibilityChunkIndex;
		actor.worldTlasOccurrenceGeneration = (uint32_t)(
			actor.worldTlasPublicationHash ^ (actor.worldTlasPublicationHash >> 32u));
		sceneInstance.metadata0 = (uint32_t)(actor.identityKey & 0xffffffffu);
		sceneInstance.metadata1 = (uint32_t)(actor.identityKey >> 32u);
		sceneInstance.metadata2 = actor.worldTlasOccurrenceGeneration;
		for (uint32_t i = 0; i < 12; ++i)
		{
			sceneInstance.currentTransform[i] = actor.instanceTransform[i];
			sceneInstance.previousTransform[i] = actor.previousInstanceTransform[i];
		}
		persistentVoxelInstance.instanceId = raySceneBuilder.AddLegacyInstance(persistentVoxelInstance, sceneInstance);
		actor.worldTlasInstanceIndex = persistentVoxelInstance.instanceId;
		if (occurrenceCensus.Targets(actor.actorIndex))
		{
			recordOccurrenceCandidate(actor, true, "publish-exact");
			NRIActorOccurrence occurrence;
			occurrence.role = NRIActorOccurrenceRole::Exact;
			occurrence.identityKey = actor.identityKey;
			occurrence.lifecycleGeneration = (uint64_t)(uint32_t)actor.actorIndex;
			occurrence.ownerWorldEpoch = actor.ownerWorldEpoch;
			occurrence.ownerLifetimeGeneration = actor.ownerLifetimeGeneration;
			occurrence.placementGeneration = actor.placementGeneration;
			occurrence.placementStateHash = actor.placementStateHash;
			occurrence.bindingGeneration = actor.bindingGeneration;
			occurrence.publicationHash = actor.worldTlasPublicationHash;
			occurrence.meshResourceKey = actor.meshResourceKey;
			occurrence.meshKeyHash = actor.meshKeyHash;
			occurrence.blasHandle = persistentVoxelInstance.accelerationStructureHandle;
			occurrence.actorIndex = actor.actorIndex;
			occurrence.physicalSectorIndex = actor.physicalSectorIndex;
			occurrence.tlasInstanceIndex = persistentVoxelInstance.instanceId;
			occurrence.occurrenceGeneration = actor.worldTlasOccurrenceGeneration;
			occurrence.expectedOccurrenceGeneration = sceneInstance.metadata2;
			occurrence.workloadMask = persistentVoxelInstance.mask;
			occurrence.authorityCurrent = actor.authorityCurrent;
			occurrence.publicationEligible = actor.publicationEligible;
			occurrence.pendingRemoval = actor.pendingRemoval;
			occurrence.ledgerReason = publicationDecision.reason;
			occurrence.capturedThisFrame = actor.capturedThisFrame;
			occurrence.transform = actor.instanceTransform;
			occurrence.boundsValid = meshResourceIt->second.boundsValid &&
				ComputeNRIActorOccurrenceWorldBounds(
					occurrence.transform,
					meshResourceIt->second.boundsMin,
					meshResourceIt->second.boundsMax,
					occurrence.boundsMin,
					occurrence.boundsMax);
			occurrenceCensus.RecordOccurrence(occurrence);
		}
		if (proxyWorkloadMask != 0u)
		{
			PendingShadowProxyInstance pending = {};
			pending.instance = persistentVoxelInstance;
			pending.instance.mask = proxyWorkloadMask;
			pending.instance.accelerationStructureHandle = shadowProxyHandle;
			pending.scene = sceneInstance;
			pending.scene.visibilityChunk = EncodeNRIVoxelShadowProxyVisibility(shadowProxy.proxyPrimitiveCount);
			pending.actorIdentityKey = actor.identityKey;
			pending.meshResourceKey = actor.meshResourceKey;
			pending.exactPrimitiveCount = actor.primitiveCount;
			pending.proxyPrimitiveCount = shadowProxy.proxyPrimitiveCount;
			if (occurrenceCensus.Targets(actor.actorIndex))
			{
				pending.diagnosticOccurrence.role = NRIActorOccurrenceRole::ShadowProxy;
				pending.diagnosticOccurrence.identityKey = actor.identityKey;
				pending.diagnosticOccurrence.lifecycleGeneration = (uint64_t)(uint32_t)actor.actorIndex;
				pending.diagnosticOccurrence.ownerWorldEpoch = actor.ownerWorldEpoch;
				pending.diagnosticOccurrence.ownerLifetimeGeneration = actor.ownerLifetimeGeneration;
				pending.diagnosticOccurrence.placementGeneration = actor.placementGeneration;
				pending.diagnosticOccurrence.placementStateHash = actor.placementStateHash;
				pending.diagnosticOccurrence.bindingGeneration = actor.bindingGeneration;
				pending.diagnosticOccurrence.publicationHash = actor.worldTlasPublicationHash;
				pending.diagnosticOccurrence.meshResourceKey = actor.meshResourceKey;
				pending.diagnosticOccurrence.meshKeyHash = actor.meshKeyHash;
				pending.diagnosticOccurrence.blasHandle = shadowProxyHandle;
				pending.diagnosticOccurrence.actorIndex = actor.actorIndex;
				pending.diagnosticOccurrence.physicalSectorIndex = actor.physicalSectorIndex;
				pending.diagnosticOccurrence.occurrenceGeneration = actor.worldTlasOccurrenceGeneration;
				pending.diagnosticOccurrence.expectedOccurrenceGeneration = sceneInstance.metadata2;
				pending.diagnosticOccurrence.workloadMask = proxyWorkloadMask;
				pending.diagnosticOccurrence.authorityCurrent = actor.authorityCurrent;
				pending.diagnosticOccurrence.publicationEligible = actor.publicationEligible;
				pending.diagnosticOccurrence.pendingRemoval = actor.pendingRemoval;
				pending.diagnosticOccurrence.ledgerReason = publicationDecision.reason;
				pending.diagnosticOccurrence.capturedThisFrame = actor.capturedThisFrame;
				pending.diagnosticOccurrence.transform = actor.instanceTransform;
				pending.diagnosticOccurrence.boundsValid = meshResourceIt->second.boundsValid &&
					ComputeNRIActorOccurrenceWorldBounds(
						pending.diagnosticOccurrence.transform,
						meshResourceIt->second.boundsMin,
						meshResourceIt->second.boundsMax,
						pending.diagnosticOccurrence.boundsMin,
						pending.diagnosticOccurrence.boundsMax);
			}
			pendingShadowProxyInstances.push_back(pending);
		}
		if (actor.indirectOnly && ((int)perf_looptraceframes > 0 || (int)nri_pttraceframes > 0 || voxelStatsEnabled))
		{
			Printf("PERF pt local player voxel instance NRI: frame=%u actor=%d voxel=%d prims=%u actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx material_key=0x%llx instance_id=%u mask=0x%x metadata=0x%x primary_visible=%u captured=%u retained_age=%llu blas=1\n",
				frameIndex,
				actor.actorIndex,
				actor.resolvedVoxelIndex,
				actor.primitiveCount,
				(unsigned long long)actor.identityKey,
				(unsigned long long)actor.meshResourceKey,
				(unsigned long long)actor.meshKeyHash,
				(unsigned long long)actor.materialKeyHash,
				persistentVoxelInstance.instanceId,
				(uint32_t)persistentVoxelInstance.mask,
				sceneInstance.metadata2,
				(actor.actorIndex == visibilityContext.localPlayerActorIndex && visibilityContext.localPlayerPrimaryVisible) ? 1u : 0u,
				actor.capturedThisFrame ? 1u : 0u,
				(unsigned long long)actor.retainedFrameAge);
		}
		actor.inWorldTlasThisFrame = true;
		actor.worldTlasFrameIndex = frameIndex;
		if (meshResourceFirstPublish)
		{
			persistentVoxelTlasNewMeshCount++;
			persistentVoxelTlasNewUniquePrimitiveCount += actor.primitiveCount;
			if (tracePersistentVoxelTlasSummary)
			{
				persistentVoxelTlasNewMeshResources.insert(actor.meshResourceKey);
			}
		}
		if (meshResourceNewThisFrame)
		{
			persistentVoxelTlasNewInstanceCount++;
			persistentVoxelTlasNewInstancePrimitiveCount += actor.primitiveCount;
		}
		if (meshResourceIt->second.directComputePublished)
		{
			const uint32_t directReadyFrame = meshResourceIt->second.directComputeReadyFrame;
			const uint32_t directLatency =
				directReadyFrame != 0 && frameIndex >= directReadyFrame ?
				frameIndex - directReadyFrame :
				0u;
			persistentVoxelTlasDirectPublishedCount++;
			persistentVoxelTlasDirectLatencyFrames += directLatency;
			persistentVoxelTlasDirectMaxLatencyFrames = std::max(persistentVoxelTlasDirectMaxLatencyFrames, directLatency);
		}
		const bool runtimeTailMesh = IsNRIVoxelComputePreloadRuntimeWithheldMesh(residencyLastBuildSerial, actor.meshResourceKey);
		const bool runtimeProbeMesh = IsNRIVoxelComputePreloadRuntimeProbeMesh(residencyLastBuildSerial, actor.meshResourceKey);
		const bool traceDirectGeneratedTlas =
			meshResourceIt->second.directComputePublished &&
			(int)nri_ptvoxelcomputetrace > 0 &&
			meshResourceFirstPublish;
		meshResourceIt->second.tlasPublished = true;
		const uint64_t schedulerTokenId = admissionScheduler.FindTlasPending({ residencyMapGeneration, actor.meshResourceKey });
		if (schedulerTokenId != 0)
		{
			admissionScheduler.MarkTlasReady(schedulerTokenId);
		}
		if (voxelStatsEnabled || traceDirectGeneratedTlas)
		{
			Printf("PERF pt voxel tlas NRI: frame=%u action=publish reason=none actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx instance_id=%u primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u blas=1 shared_route=%u direct_generated=%u tlas_ready=%u tlas_published=1 first_publish=%u new_this_frame=%u ready=1\n",
				frameIndex,
				(unsigned long long)actor.identityKey,
				(unsigned long long)actor.meshResourceKey,
				(unsigned long long)actor.meshKeyHash,
				(unsigned long long)actor.materialKeyHash,
				persistentVoxelInstance.instanceId,
				actor.primitiveOffset,
				actor.primitiveCount,
				actor.materialOffset,
				actor.materialCount,
				routedThroughSharedBlas ? 1u : 0u,
				meshResourceIt->second.directComputePublished ? 1u : 0u,
				meshResourceIt->second.tlasReadyFrame,
				meshResourceFirstPublish ? 1u : 0u,
				meshResourceNewThisFrame ? 1u : 0u);
		}
		if (runtimeTailMesh && meshResourceFirstPublish)
		{
			const uint32_t requestFrame = meshResourceIt->second.directComputeRequestFrame;
			const uint32_t readyFrame = meshResourceIt->second.directComputeReadyFrame;
			const uint32_t blasFrame = meshResourceIt->second.directComputeBlasFrame;
			const uint32_t publishFrame = meshResourceIt->second.directComputePublishedFrame;
			const uint32_t requestToReady =
				requestFrame != UINT32_MAX && readyFrame >= requestFrame ? readyFrame - requestFrame : 0u;
			const uint32_t readyToBlas =
				blasFrame != UINT32_MAX && blasFrame >= readyFrame ? blasFrame - readyFrame : 0u;
			const uint32_t blasToPublish =
				blasFrame != UINT32_MAX && publishFrame >= blasFrame ? publishFrame - blasFrame : 0u;
			const uint32_t publishToTlas =
				publishFrame != UINT32_MAX && frameIndex >= publishFrame ? frameIndex - publishFrame : 0u;
			const uint32_t requestToTlas =
				requestFrame != UINT32_MAX && frameIndex >= requestFrame ? frameIndex - requestFrame : 0u;
			Printf("PERF pt voxel runtime tail NRI: action=tlas-visible build_serial=%llu frame=%u mesh_resource=0x%llx mesh_variant=0x%llx material=0x%llx probe=%u request_frame=%u ready_frame=%u blas_frame=%u publish_frame=%u tlas_frame=%u request_to_ready=%u ready_to_blas=%u blas_to_publish=%u publish_to_tlas=%u request_to_tlas=%u primitives=%u\n",
				(unsigned long long)residencyLastBuildSerial,
				frameIndex,
				(unsigned long long)actor.meshResourceKey,
				(unsigned long long)actor.meshKeyHash,
				(unsigned long long)actor.materialKeyHash,
				runtimeProbeMesh ? 1u : 0u,
				requestFrame,
				readyFrame,
				blasFrame,
				publishFrame,
				frameIndex,
				requestToReady,
				readyToBlas,
				blasToPublish,
				publishToTlas,
				requestToTlas,
				actor.primitiveCount);
		}
		persistentVoxelTlasMeshResources.insert(actor.meshResourceKey);
		if (routedThroughSharedBlas)
		{
			sharedBlasCache.RecordSharedActor(actor.meshResourceKey);
		}
		else
		{
			if (settings.sharedBlasRouteEnabled && sharedRouteFallbackReason != nullptr)
			{
				sharedBlasCache.RecordRouteFallback(actor.meshResourceKey, sharedRouteFallbackReason);
			}
			sharedBlasCache.RecordLegacyActor(actor.meshResourceKey);
		}
		persistentVoxelTlasPublishedCount++;
		persistentVoxelTlasInstancePrimitiveCount += actor.primitiveCount;
		if (actor.capturedThisFrame)
		{
			persistentVoxelTlasCapturedCount++;
		}
		else
		{
			persistentVoxelTlasRetainedCount++;
			persistentVoxelRetainedTlasPrimitives += actor.primitiveCount;
			persistentVoxelTlasMaxRetainedFrameAge = std::max(persistentVoxelTlasMaxRetainedFrameAge, actorRetainedFrameAge);
		}
		if (tracePersistentVoxelTlasSummary)
		{
			PersistentVoxelTlasGroupStats& group = persistentVoxelTlasGroups[actor.meshResourceKey];
			if (group.instanceCount == 0)
			{
				group.meshResourceKey = actor.meshResourceKey;
				group.meshKeyHash = actor.meshKeyHash;
				group.primitiveCount = actor.primitiveCount;
				group.resolvedVoxelIndex = actor.resolvedVoxelIndex;
				group.tlasReadyFrame = meshResourceIt->second.tlasReadyFrame;
			}
			group.instanceCount++;
			group.instancePrimitiveCount += actor.primitiveCount;
			if (meshResourceFirstPublish)
			{
				group.newlyPublished = true;
			}
			if (meshResourceNewThisFrame)
			{
				group.newInstanceCount++;
				group.newInstancePrimitiveCount += actor.primitiveCount;
			}
			group.primitiveCount = std::max(group.primitiveCount, actor.primitiveCount);
			if (actor.capturedThisFrame)
			{
				group.capturedCount++;
			}
			else
			{
				group.retainedCount++;
				group.maxRetainedFrameAge = std::max(group.maxRetainedFrameAge, actorRetainedFrameAge);
			}
		}
		outStats.instanceCount++;
		outStats.instancePrimitiveCount += actor.primitiveCount;
		if (meshResourceIt->second.meshBakeSpace != nri_scene::VoxelMeshBakeSpace::LocalSpace)
		{
			outStats.bakedFallbackInstanceCount++;
		}
	}
	for (PendingShadowProxyInstance& pending : pendingShadowProxyInstances)
	{
		pending.instance.instanceId = raySceneBuilder.AddLegacyInstance(pending.instance, pending.scene);
		if (pending.diagnosticOccurrence.actorIndex >= 0)
		{
			pending.diagnosticOccurrence.tlasInstanceIndex = pending.instance.instanceId;
			occurrenceCensus.RecordOccurrence(pending.diagnosticOccurrence);
		}
		outStats.instanceCount++;
		outStats.instancePrimitiveCount += pending.proxyPrimitiveCount;
		outStats.shadowProxyInstanceCount++;
		outStats.shadowProxyPrimitiveCount += pending.proxyPrimitiveCount;
		outStats.exactShadowPrimitiveCountRemoved += pending.exactPrimitiveCount;
		if (voxelStatsEnabled)
		{
			Printf("PERF pt voxel shadow proxy NRI: frame=%u action=route actor_key=0x%llx mesh_resource=0x%llx instance_id=%u mask=0x%x exact_prims=%u proxy_prims=%u visibility=0x%x\n",
				frameIndex,
				(unsigned long long)pending.actorIdentityKey,
				(unsigned long long)pending.meshResourceKey,
				pending.instance.instanceId,
				(uint32_t)pending.instance.mask,
				pending.exactPrimitiveCount,
				pending.proxyPrimitiveCount,
				pending.scene.visibilityChunk);
		}
	}
	if (settings.diagnosticsEnabled)
	{
		Printf("PERF pt voxel shadow proxy route NRI: frame=%u enabled=%u routed_instances=%u routed_prims=%llu exact_shadow_prims_removed=%llu pending_tail=%u\n",
			frameIndex, settings.shadowProxyRouteEnabled ? 1u : 0u,
			outStats.shadowProxyInstanceCount,
			(unsigned long long)outStats.shadowProxyPrimitiveCount,
			(unsigned long long)outStats.exactShadowPrimitiveCountRemoved,
			(uint32_t)pendingShadowProxyInstances.size());
	}
	outStats.sharedMeshResourceCount = (uint32_t)persistentVoxelTlasMeshResources.size();
	outStats.occurrenceFrame = occurrenceCensus.FinishPersistent();
	sharedBlasCache.EndFrame();
	if (tracePersistentVoxelTlasSummary)
	{
		for (const auto& groupPair : persistentVoxelTlasGroups)
		{
			persistentVoxelTlasUniquePrimitiveCount += groupPair.second.primitiveCount;
		}
		Printf("PERF pt voxel tlas summary NRI: frame=%u candidates=%u published=%u skipped=%u captured=%u retained=%u unique_meshes=%u instance_prims=%llu unique_prims=%llu direct_published=%u direct_latency_sum=%llu direct_latency_max=%u max_retained_age=%llu actors=%u active=%u\n",
			frameIndex,
			persistentVoxelTlasCandidateCount,
			persistentVoxelTlasPublishedCount,
			persistentVoxelTlasSkippedCount,
			persistentVoxelTlasCapturedCount,
			persistentVoxelTlasRetainedCount,
			(uint32_t)persistentVoxelTlasMeshResources.size(),
			(unsigned long long)persistentVoxelTlasInstancePrimitiveCount,
			(unsigned long long)persistentVoxelTlasUniquePrimitiveCount,
			persistentVoxelTlasDirectPublishedCount,
			(unsigned long long)persistentVoxelTlasDirectLatencyFrames,
			persistentVoxelTlasDirectMaxLatencyFrames,
			(unsigned long long)persistentVoxelTlasMaxRetainedFrameAge,
			(uint32_t)batch.actors.size(),
			batch.activeActorCount);
		Printf("PERF pt voxel tlas pressure NRI: frame=%u new_meshes=%u new_instances=%u new_instance_prims=%llu new_unique_prims=%llu ready_frame_skips=%u ready_frame_skip_prims=%llu missing_skips=%u missing_skip_prims=%llu excluded_skips=%u excluded_skip_prims=%llu retained_prims=%llu active_instances=%u active_instance_prims=%llu active_unique_prims=%llu active_unique_meshes=%u actors=%u active=%u\n",
			frameIndex,
			persistentVoxelTlasNewMeshCount,
			persistentVoxelTlasNewInstanceCount,
			(unsigned long long)persistentVoxelTlasNewInstancePrimitiveCount,
			(unsigned long long)persistentVoxelTlasNewUniquePrimitiveCount,
			persistentVoxelTlasReadyFrameSkipCount,
			(unsigned long long)persistentVoxelTlasReadyFrameSkipPrimitiveCount,
			persistentVoxelTlasMissingSkipCount,
			(unsigned long long)persistentVoxelTlasMissingSkipPrimitiveCount,
			persistentVoxelTlasExcludedSkipCount,
			(unsigned long long)persistentVoxelTlasExcludedSkipPrimitiveCount,
			(unsigned long long)persistentVoxelRetainedTlasPrimitives,
			persistentVoxelTlasPublishedCount,
			(unsigned long long)persistentVoxelTlasInstancePrimitiveCount,
			(unsigned long long)persistentVoxelTlasUniquePrimitiveCount,
			(uint32_t)persistentVoxelTlasMeshResources.size(),
			(uint32_t)batch.actors.size(),
			batch.activeActorCount);

		std::vector<PersistentVoxelTlasGroupStats> sortedTlasGroups;
		sortedTlasGroups.reserve(persistentVoxelTlasGroups.size());
		for (const auto& groupPair : persistentVoxelTlasGroups)
		{
			sortedTlasGroups.push_back(groupPair.second);
		}
		std::sort(sortedTlasGroups.begin(), sortedTlasGroups.end(), [](const PersistentVoxelTlasGroupStats& left, const PersistentVoxelTlasGroupStats& right)
		{
			if (left.instancePrimitiveCount != right.instancePrimitiveCount)
			{
				return left.instancePrimitiveCount > right.instancePrimitiveCount;
			}
			if (left.instanceCount != right.instanceCount)
			{
				return left.instanceCount > right.instanceCount;
			}
			return left.meshResourceKey < right.meshResourceKey;
		});
		const uint32_t topCount = std::min<uint32_t>(8u, (uint32_t)sortedTlasGroups.size());
		for (uint32_t i = 0; i < topCount; ++i)
		{
			const PersistentVoxelTlasGroupStats& group = sortedTlasGroups[i];
			Printf("PERF pt voxel tlas top NRI: frame=%u rank=%u mesh_resource=0x%llx mesh_key=0x%llx voxel=%d instances=%u captured=%u retained=%u primitive_count=%u instance_prims=%llu max_retained_age=%llu tlas_ready=%u\n",
				frameIndex,
				i + 1u,
				(unsigned long long)group.meshResourceKey,
				(unsigned long long)group.meshKeyHash,
				group.resolvedVoxelIndex,
				group.instanceCount,
				group.capturedCount,
				group.retainedCount,
				group.primitiveCount,
				(unsigned long long)group.instancePrimitiveCount,
				(unsigned long long)group.maxRetainedFrameAge,
				group.tlasReadyFrame);
		}
		std::vector<PersistentVoxelTlasGroupStats> sortedNewTlasGroups;
		sortedNewTlasGroups.reserve(persistentVoxelTlasGroups.size());
		for (const PersistentVoxelTlasGroupStats& group : sortedTlasGroups)
		{
			if (group.newlyPublished || group.newInstanceCount > 0)
			{
				sortedNewTlasGroups.push_back(group);
			}
		}
		std::sort(sortedNewTlasGroups.begin(), sortedNewTlasGroups.end(), [](const PersistentVoxelTlasGroupStats& left, const PersistentVoxelTlasGroupStats& right)
		{
			if (left.newInstancePrimitiveCount != right.newInstancePrimitiveCount)
			{
				return left.newInstancePrimitiveCount > right.newInstancePrimitiveCount;
			}
			if (left.primitiveCount != right.primitiveCount)
			{
				return left.primitiveCount > right.primitiveCount;
			}
			return left.meshResourceKey < right.meshResourceKey;
		});
		const uint32_t topNewCount = std::min<uint32_t>(8u, (uint32_t)sortedNewTlasGroups.size());
		for (uint32_t i = 0; i < topNewCount; ++i)
		{
			const PersistentVoxelTlasGroupStats& group = sortedNewTlasGroups[i];
			Printf("PERF pt voxel tlas new top NRI: frame=%u rank=%u mesh_resource=0x%llx mesh_key=0x%llx voxel=%d new_mesh=%u new_instances=%u primitive_count=%u new_instance_prims=%llu tlas_ready=%u\n",
				frameIndex,
				i + 1u,
				(unsigned long long)group.meshResourceKey,
				(unsigned long long)group.meshKeyHash,
				group.resolvedVoxelIndex,
				group.newlyPublished ? 1u : 0u,
				group.newInstanceCount,
				group.primitiveCount,
				(unsigned long long)group.newInstancePrimitiveCount,
				group.tlasReadyFrame);
		}
	}

	return true;
}

NRIPersistentVoxelDescriptorSnapshot NRIPersistentVoxelResidency::BuildDescriptorSnapshot(
	const NRIBufferResource& fallbackVertexBuffer,
	const NRIBufferResource& fallbackIndexBuffer,
	const NRIBufferResource& fallbackPrimitiveBuffer,
	const NRIBufferResource& fallbackMaterialBuffer) const
{
	auto selectView = [](const NRIBufferResource& primary, const NRIBufferResource& fallback) -> nri::Descriptor*
	{
		return primary.shaderView != nullptr ? primary.shaderView : fallback.shaderView;
	};

	NRIPersistentVoxelDescriptorSnapshot snapshot = {};
	snapshot.vertex = selectView(vertexBuffer, fallbackVertexBuffer);
	snapshot.index = selectView(indexBuffer, fallbackIndexBuffer);
	snapshot.primitive = selectView(primitiveBuffer, fallbackPrimitiveBuffer);
	snapshot.material = selectView(materialBuffer, fallbackMaterialBuffer);
	snapshot.primitiveCount = BoundPrimitiveCount();
	snapshot.materialCount = BoundMaterialCount();
	return snapshot;
}

uint32_t NRIPersistentVoxelResidency::BoundPrimitiveCount() const
{
	return primitiveBuffer.shaderView != nullptr ? arenaPrimitiveCursor : 0u;
}

uint32_t NRIPersistentVoxelResidency::BoundMaterialCount() const
{
	return materialBuffer.shaderView != nullptr ? (uint32_t)materialRangeAllocator.Stats().cursorRows : 0u;
}

void NRIPersistentVoxelDestroyServices::DestroyBuffer(NRIBufferResource& resource) const
{
	if (destroyBuffer != nullptr)
	{
		destroyBuffer(user, resource);
	}
}

void NRIPersistentVoxelResidency::DestroyArenaBuffers(const NRIPersistentVoxelDestroyServices& services)
{
	MarkMaintenanceMutation();
	services.DestroyBuffer(vertexBuffer);
	services.DestroyBuffer(indexBuffer);
	services.DestroyBuffer(primitiveBuffer);
	services.DestroyBuffer(materialBuffer);
}

NRIPersistentVoxelLightAppendStats NRIPersistentVoxelResidency::AppendSceneLights(
	SceneLightSystem& sceneLights,
	uint32_t frameIndex,
	bool voxelStatsEnabled) const
{
	NRIPersistentVoxelLightAppendStats stats = {};
	if (!batch.valid || batch.materialBridge.materials.empty())
	{
		return stats;
	}

	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (!actor.active || !IsPersistentVoxelActorPublicationCurrent(actor) || actor.materialCount == 0)
		{
			continue;
		}
		const uint32_t requiredLightRoles =
			(actor.actorIndex >= 0 ? NRI_ACTOR_OCCURRENCE_ROLE_LIGHT_ANCHOR : 0u) |
			(actor.lightRecords.empty() ? 0u : NRI_ACTOR_OCCURRENCE_ROLE_EMISSIVE_SURFACE);
		if (!actor.inWorldTlasThisFrame ||
			actor.worldTlasFrameIndex != frameIndex ||
			committedWorldTlasFrameIndex != frameIndex ||
			actor.worldTlasInstanceIndex == UINT32_MAX ||
			actor.primitiveCount == 0u ||
			!actorOccurrenceLedger.IsCommitted(
				frameIndex,
				{ actor.ownerWorldEpoch, actor.ownerLifetimeGeneration },
				actor.placementGeneration,
				actor.bindingGeneration,
				actor.worldTlasPublicationHash,
				requiredLightRoles))
		{
			stats.skippedActors++;
			stats.skippedRecords += (uint32_t)actor.lightRecords.size();
			continue;
		}
		if (actor.actorIndex >= 0)
		{
			sceneLights.MarkActorPublishedForOverlayActivation(actor.actorIndex);
			stats.activationAnchors++;
		}
		if (voxelStatsEnabled)
		{
			const nri_scene::MaterialLightingMetadata* metadata = nullptr;
			for (const nri_scene::MaterialLightingMetadata& candidateMetadata : actor.materialBridge.lightMetadata)
			{
				if (sceneLights.MaterialWouldEmit(candidateMetadata))
				{
					metadata = &candidateMetadata;
					break;
				}
			}
			if (metadata != nullptr || !actor.lightRecords.empty())
			{
				Printf("PERF pt voxel light material NRI: frame=%u actor=%d source_pic=%d voxel=%d actor_key=0x%llx mat_key=0x%llx instance=%u primitive_base=%u primitive_count=%u metadata=%u emits=%u records=%u texture_id=%u base_texture_id=%u material_flags=0x%x lighting_flags=0x%x emissive_mode=%u emissive_texture=%u emissive_intensity=%.3f\n",
					frameIndex,
					actor.actorIndex,
					actor.sourcePicnum,
					actor.resolvedVoxelIndex,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.materialKeyHash,
					actor.worldTlasInstanceIndex,
					actor.primitiveOffset,
					actor.primitiveCount,
					metadata != nullptr ? 1u : 0u,
					metadata != nullptr ? 1u : 0u,
					(uint32_t)actor.lightRecords.size(),
					metadata != nullptr ? metadata->textureId : 0u,
					metadata != nullptr ? metadata->baseTextureId : 0u,
					metadata != nullptr ? metadata->materialFlags : 0u,
					metadata != nullptr ? metadata->lightingFlags : 0u,
					metadata != nullptr ? metadata->emissiveMode : 0u,
					metadata != nullptr ? metadata->emissiveTextureIndex : UINT32_MAX,
					metadata != nullptr ? metadata->emissiveIntensity : 0.0f);
			}
		}
		if (actor.lightRecords.empty())
		{
			continue;
		}

		std::vector<SceneLightSystem::SurfaceRecord> placedRecords = actor.lightRecords;
		for (SceneLightSystem::SurfaceRecord& record : placedRecords)
		{
			record.placedPrimitiveBase = actor.primitiveOffset;
			record.placedPrimitiveCount = actor.primitiveCount;
			record.sceneInstanceIndex = actor.worldTlasInstanceIndex;
			record.occurrenceKeyLo = (uint32_t)(actor.identityKey & 0xffffffffu);
			record.occurrenceKeyHi = (uint32_t)(actor.identityKey >> 32u);
			record.occurrenceGeneration = actor.worldTlasOccurrenceGeneration;
		}
		sceneLights.AppendSurfaceRecords(placedRecords, actor.materialOffset);
		stats.appendedActors++;
		stats.appendedRecords += (uint32_t)actor.lightRecords.size();
	}
	if (voxelStatsEnabled && (stats.activationAnchors != 0 || stats.appendedActors != 0 || stats.skippedActors != 0))
	{
		Printf("PERF pt voxel light NRI: frame=%u activation_anchors=%u appended_actors=%u skipped_not_tlas=%u appended_records=%u skipped_records=%u actors=%u active=%u\n",
			frameIndex,
			stats.activationAnchors,
			stats.appendedActors,
			stats.skippedActors,
			stats.appendedRecords,
			stats.skippedRecords,
			(uint32_t)batch.actors.size(),
			batch.activeActorCount);
	}
	return stats;
}

void NRIPersistentVoxelResidency::CommitWorldTlasFrame(uint32_t frameIndex)
{
	committedWorldTlasFrameIndex = frameIndex;
	actorOccurrenceLedger.CommitFrame(frameIndex);
}

NRIPersistentVoxelMemoryUsage NRIPersistentVoxelResidency::GetMemoryUsage() const
{
	if (cachedMemoryUsageGeneration == maintenanceMutationGeneration)
	{
		maintenanceStats.memorySnapshotHits++;
		return cachedMemoryUsage;
	}
	NRIPersistentVoxelMemoryUsage usage = {};
	auto accumulateBuffer = [](const NRIBufferResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};
	auto accumulateAs = [](const NRIAccelerationStructureResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};

	usage.arenaVertexCommittedBytes = vertexBuffer.memorySize;
	usage.arenaIndexCommittedBytes = indexBuffer.memorySize;
	usage.arenaPrimitiveCommittedBytes = primitiveBuffer.memorySize;
	usage.arenaMaterialCommittedBytes = materialBuffer.memorySize;
	usage.arenaVertexUsedBytes = (uint64_t)arenaVertexCursor * sizeof(nri_scene::SceneVertex);
	usage.arenaIndexUsedBytes = (uint64_t)arenaIndexCursor * sizeof(uint32_t);
	usage.arenaPrimitiveUsedBytes = (uint64_t)arenaPrimitiveCursor * sizeof(nri_scene::PrimitiveData);
	usage.arenaMaterialUsedBytes = materialRangeAllocator.Stats().cursorRows * sizeof(nri_scene::MaterialData);
	accumulateBuffer(vertexBuffer, usage.sceneBufferBytes);
	accumulateBuffer(indexBuffer, usage.sceneBufferBytes);
	accumulateBuffer(primitiveBuffer, usage.sceneBufferBytes);
	accumulateBuffer(materialBuffer, usage.sceneBufferBytes);
	for (const auto& pair : meshVariantResources)
	{
		usage.privateVertexBytes += pair.second.vertexBuffer.memorySize;
		usage.privateIndexBytes += pair.second.indexBuffer.memorySize;
		usage.directBlasBytes += pair.second.accelerationStructure.memorySize;
		accumulateBuffer(pair.second.vertexBuffer, usage.sceneBufferBytes);
		accumulateBuffer(pair.second.indexBuffer, usage.sceneBufferBytes);
		accumulateAs(pair.second.accelerationStructure, usage.accelerationStructureBytes);
		usage.shadowProxyVertexBytes += pair.second.shadowProxy.vertexBuffer.memorySize;
		usage.shadowProxyIndexBytes += pair.second.shadowProxy.indexBuffer.memorySize;
		usage.shadowProxyBlasBytes += pair.second.shadowProxy.accelerationStructure.memorySize;
		accumulateBuffer(pair.second.shadowProxy.vertexBuffer, usage.sceneBufferBytes);
		accumulateBuffer(pair.second.shadowProxy.indexBuffer, usage.sceneBufferBytes);
		accumulateAs(pair.second.shadowProxy.accelerationStructure, usage.accelerationStructureBytes);
	}
	for (const auto& pair : materialVariantResources)
	{
		usage.materialLogicalBytes += pair.second.residentBytes;
	}
	for (const auto& pair : admissionQueue)
	{
		const PersistentVoxelAdmissionEntry& entry = pair.second;
		usage.admissionTransientBufferBytes +=
			entry.uploadMeshResource.vertexBuffer.memorySize +
			entry.uploadMeshResource.indexBuffer.memorySize +
			entry.directBlasScratchBuffer.memorySize;
		usage.admissionTransientAsBytes += entry.uploadMeshResource.accelerationStructure.memorySize;
		usage.admissionCpuGeometryBytes +=
			(uint64_t)entry.uploadGeometry.vertices.capacity() * sizeof(nri_scene::SceneVertex) +
			(uint64_t)entry.uploadGeometry.indices.capacity() * sizeof(uint32_t) +
			(uint64_t)entry.uploadGeometry.primitives.capacity() * sizeof(nri_scene::PrimitiveData) +
			(uint64_t)entry.uploadGpuIndices.capacity() * sizeof(uint32_t) +
			(uint64_t)entry.uploadGpuPrimitives.capacity() * sizeof(nri_scene::PrimitiveData);
	}
	const NRIPersistentVoxelSharedBlasFrameStats& sharedStats = sharedBlasCache.LastFrameStats();
	usage.sharedBlasBytes = sharedStats.residentBytes;
	usage.accelerationStructureBytes += sharedStats.residentBytes;
	cachedMemoryUsage = usage;
	cachedMemoryUsageGeneration = maintenanceMutationGeneration;
	maintenanceStats.memorySnapshotRebuilds++;
	return usage;
}

void NRIPersistentVoxelResidency::CollectResidentAccelerationStructures(
	std::vector<NRIAccelerationStructureResource*>& outResources)
{
	outResources.clear();
	outResources.reserve(meshVariantResources.size());
	for (auto& pair : meshVariantResources)
	{
		NRIAccelerationStructureResource& resource = pair.second.accelerationStructure;
		if (resource.accelerationStructure != nullptr && !resource.compacted)
		{
			outResources.push_back(&resource);
		}
	}
}

const NRIPersistentVoxelSharedBlasFrameStats& NRIPersistentVoxelResidency::GetSharedBlasFrameStats() const
{
	return sharedBlasCache.LastFrameStats();
}

NRIPersistentVoxelStatusSnapshot NRIPersistentVoxelResidency::BuildStatusSnapshot() const
{
	NRIPersistentVoxelStatusSnapshot snapshot = {};
	FillResourceStatusSnapshot(snapshot);
	FillBatchStatusSnapshot(snapshot);
	return snapshot;
}

void NRIPersistentVoxelResidency::FillResourceStatusSnapshot(NRIPersistentVoxelStatusSnapshot& snapshot) const
{
	if (cachedResourceStatusGeneration == maintenanceMutationGeneration)
	{
		snapshot = cachedResourceStatus;
		maintenanceStats.resourceStatusHits++;
		return;
	}
	snapshot = {};
	snapshot.meshVariantResourceCount = (uint32_t)meshVariantResources.size();
	snapshot.materialVariantResourceCount = (uint32_t)materialVariantResources.size();
	snapshot.admissionQueueCount = (uint32_t)admissionQueue.size();
	CountAdmissionWork(
		snapshot.requiredAdmissionPendingCount,
		snapshot.requiredAdmissionReadyCount,
		snapshot.optionalAdmissionPendingCount,
		snapshot.failedAdmissionCount);
	for (uint64_t key : admissionIndex.ActiveKeys())
	{
		const auto found = admissionQueue.find(key);
		if (found == admissionQueue.end())
		{
			continue;
		}
		const PersistentVoxelAdmissionEntry& entry = found->second;
		snapshot.computeInFlightCount += entry.state == PersistentVoxelAdmissionState::DirectComputePending ? 1u : 0u;
		snapshot.blasInFlightCount +=
			entry.state == PersistentVoxelAdmissionState::BuildingBlas ||
			entry.state == PersistentVoxelAdmissionState::DirectBlasPending ? 1u : 0u;
	}
	snapshot.cpuGeometryBuildCount = cumulativeCpuGeometryBuildCount;
	snapshot.cpuGeometryUploadCount = cumulativeCpuGeometryUploadCount;
	snapshot.cpuGeometryUploadBytes = cumulativeCpuGeometryUploadBytes;
	snapshot.cpuGeometryFallbackCount = cumulativeCpuGeometryFallbackCount;
	snapshot.residencyGeneration = residencyMapGeneration;
	snapshot.residencyBuildSerial = residencyLastBuildSerial;
	snapshot.lastDesiredResidencyCount = lastDesiredResidencyCount;
	snapshot.lastDesiredPreloadCount = lastDesiredPreloadCount;
	snapshot.lastDesiredActorCount = lastDesiredActorCount;
	snapshot.lastCpuReadyCount = lastCpuReadyCount;
	snapshot.lastGpuReadyCount = lastGpuReadyCount;
	snapshot.lastRetainedCount = lastRetainedCount;
	snapshot.lastQueuedCount = lastQueuedCount;
	snapshot.lastQueuedUploadBytes = lastQueuedUploadBytes;
	snapshot.lastMeshReadyCount = lastMeshReadyCount;
	snapshot.lastMaterialReadyCount = lastMaterialReadyCount;
	snapshot.lastBlasReadyCount = lastBlasReadyCount;
	snapshot.lastMeshMissingCount = lastMeshMissingCount;
	snapshot.lastMaterialOnlyCount = lastMaterialOnlyCount;
	snapshot.lastBlasOnlyCount = lastBlasOnlyCount;
	snapshot.lastColdMeshCount = lastColdMeshCount;
	snapshot.lastColdMaterialCount = lastColdMaterialCount;
	snapshot.lastColdPrimitiveCount = lastColdPrimitiveCount;
	snapshot.lastForcedCount = lastForcedCount;
	snapshot.lastPreferredCount = lastPreferredCount;

	for (const auto& meshPair : meshVariantResources)
	{
		const PersistentVoxelMeshVariantResource& resource = meshPair.second;
		snapshot.residentResourceBytes += resource.residentBytes;
		if (resource.shadowProxy.state == NRIVoxelShadowProxyResourceState::Resident)
		{
			snapshot.shadowProxyResidentCount++;
			snapshot.shadowProxyPrimitiveCount += resource.shadowProxy.proxyPrimitiveCount;
			snapshot.shadowProxyResidentBytes += resource.shadowProxy.residentBytes;
		}
		else if (resource.shadowProxy.state == NRIVoxelShadowProxyResourceState::Failed)
		{
			snapshot.shadowProxyFailedCount++;
		}
		if (resource.activeActorReferences == 0)
		{
			snapshot.zeroRefMeshResourceCount++;
			snapshot.zeroRefResourceBytes += resource.residentBytes;
		}
	}

	for (const auto& materialPair : materialVariantResources)
	{
		const PersistentVoxelMaterialVariantResource& resource = materialPair.second;
		snapshot.residentResourceBytes += resource.residentBytes;
		if (resource.activeActorReferences == 0)
		{
			snapshot.zeroRefMaterialResourceCount++;
			snapshot.zeroRefResourceBytes += resource.residentBytes;
		}
	}
	cachedResourceStatus = snapshot;
	cachedResourceStatusGeneration = maintenanceMutationGeneration;
	maintenanceStats.resourceStatusRebuilds++;
}

void NRIPersistentVoxelResidency::FillBatchStatusSnapshot(NRIPersistentVoxelStatusSnapshot& snapshot) const
{
	snapshot.batchActorCount = (uint32_t)batch.actors.size();
	snapshot.instanceRecordCount = (uint32_t)instances.size();
	for (const auto& instancePair : instances)
	{
		if (instancePair.second.pending)
		{
			snapshot.pendingInstanceCount++;
		}
	}

	snapshot.instanceMinPrimitiveCount = UINT32_MAX;
	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (!actor.active || actor.primitiveCount == 0)
		{
			continue;
		}
		snapshot.activeInstanceCount++;
		snapshot.instancePrimitiveCount += actor.primitiveCount;
		snapshot.instanceMaterialCount += actor.materialCount;
		snapshot.instanceMinPrimitiveCount = std::min(snapshot.instanceMinPrimitiveCount, actor.primitiveCount);
		snapshot.instanceMaxPrimitiveCount = std::max(snapshot.instanceMaxPrimitiveCount, actor.primitiveCount);
	}

	if (snapshot.activeInstanceCount == 0)
	{
		snapshot.instanceMinPrimitiveCount = 0;
	}
}

void NRIPersistentVoxelResidency::ApplyPressurePolicy(
	const char* phase,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	uint64_t totalTrackedBytes,
	uint64_t adapterLocalBudget,
	bool traceEnabled,
	const NRIPersistentVoxelResetServices& services)
{
	maintenanceStats.pressureEvaluatedLast = false;
	maintenanceStats.pressureSkippedLast = false;
	maintenanceStats.pressureProtectionBlockedLast = pressureProtectionBlocked;
	maintenanceStats.pressureEvaluationReasonMaskLast = NRIPersistentVoxelPressureEvaluationReason_None;
	maintenanceStats.pressureEntriesScannedLast = 0;
	maintenanceStats.pressureResourceRowsScannedLast = 0;
	const bool loadingPhase = phase != nullptr && std::strcmp(phase, "loading") == 0;
	uint64_t settingsSignature = nri_scene::HashCombine64(settings.residentMaxBytes, settings.residentMinHeadroomBytes);
	settingsSignature = nri_scene::HashCombine64(settingsSignature, settings.residentMaxColdMaps);
	settingsSignature = nri_scene::HashCombine64(settingsSignature, settings.trimColdOnLoading ? 1ull : 0ull);
	settingsSignature = nri_scene::HashCombine64(settingsSignature, loadingPhase ? 1ull : 0ull);
	const bool externalPressure =
		settings.residentMaxBytes == 0 &&
		adapterLocalBudget > settings.residentMinHeadroomBytes &&
		totalTrackedBytes > adapterLocalBudget - settings.residentMinHeadroomBytes;
	NRIPersistentVoxelPressureEvaluationInput pressureEvaluationInput = {};
	pressureEvaluationInput.traceEnabled = traceEnabled;
	pressureEvaluationInput.evaluationValid = pressureEvaluationValid;
	pressureEvaluationInput.externalPressure = externalPressure;
	pressureEvaluationInput.evaluationGeneration = pressureEvaluationGeneration;
	pressureEvaluationInput.maintenanceGeneration = maintenanceMutationGeneration;
	pressureEvaluationInput.evaluationSettingsSignature = pressureSettingsSignature;
	pressureEvaluationInput.settingsSignature = settingsSignature;
	pressureEvaluationInput.evaluationAdapterBudget = pressureAdapterBudget;
	pressureEvaluationInput.adapterBudget = adapterLocalBudget;
	pressureEvaluationInput.evaluationFrame = pressureEvaluationFrame;
	pressureEvaluationInput.frameIndex = frameIndex;
	pressureEvaluationInput.safetyAuditFrames = settings.pressureSafetyAuditFrames;
	const NRIPersistentVoxelPressureEvaluationDecision pressureEvaluationDecision =
		DecideNRIPersistentVoxelPressureEvaluation(pressureEvaluationInput);
	maintenanceStats.pressureEvaluationReasonMaskLast = pressureEvaluationDecision.reasonMask;
	if (!pressureEvaluationDecision.evaluate)
	{
		maintenanceStats.pressureSkippedLast = true;
		maintenanceStats.pressureSkips++;
		return;
	}
	maintenanceStats.pressureEvaluatedLast = true;
	maintenanceStats.pressureEvaluations++;
	if ((pressureEvaluationDecision.reasonMask & NRIPersistentVoxelPressureEvaluationReason_SafetyAudit) != 0)
	{
		maintenanceStats.pressureSafetyAudits++;
	}
	if (meshVariantResources.empty() && materialVariantResources.empty())
	{
		pressureProtectionBlocked = false;
		maintenanceStats.pressureProtectionBlockedLast = false;
		pressureEvaluationGeneration = maintenanceMutationGeneration;
		pressureSettingsSignature = settingsSignature;
		pressureAdapterBudget = adapterLocalBudget;
		pressureEvaluationFrame = frameIndex;
		pressureEvaluationValid = true;
		maintenanceStats.pressureNoops++;
		return;
	}

	std::unordered_set<uint64_t> admissionMeshes;
	std::unordered_set<uint64_t> admissionMaterials;
	uint64_t queuedBytes = 0;
	maintenanceStats.pressureEntriesScannedLast = (uint32_t)admissionQueue.size();
	maintenanceStats.pressureEntriesScanned += admissionQueue.size();
	for (const auto& pair : admissionQueue)
	{
		const PersistentVoxelAdmissionEntry& entry = pair.second;
		if (entry.mapGeneration != residencyMapGeneration ||
			entry.state == PersistentVoxelAdmissionState::Failed)
		{
			continue;
		}
		admissionMeshes.insert(BuildPersistentVoxelVariantMeshResourceKey(entry.variant));
		admissionMaterials.insert(entry.variant.materialKeyHash);
		queuedBytes += entry.estimatedBytes;
	}
	const bool materialReleaseQuiescent =
		!admissionIndex.HasActiveWork() &&
		PersistentVoxelAdmissionSchedulerQuiescent(admissionScheduler.GetSnapshot());

	uint64_t voxelResidentBytes = 0;
	uint64_t coldBytes = 0;
	uint32_t coldMeshCount = 0;
	uint32_t coldMaterialCount = 0;
	maintenanceStats.pressureResourceRowsScannedLast =
		(uint32_t)(meshVariantResources.size() + materialVariantResources.size());
	maintenanceStats.pressureResourceRowsScanned += maintenanceStats.pressureResourceRowsScannedLast;
	for (auto& pair : meshVariantResources)
	{
		PersistentVoxelMeshVariantResource& resource = pair.second;
		resource.activeActorReferences = 0;
		auto activeIt = activeMeshReferences.find(pair.first);
		if (activeIt != activeMeshReferences.end())
		{
			resource.activeActorReferences = activeIt->second;
			resource.lastUsedFrame = frameIndex;
			resource.lastUsedMapGeneration = residencyMapGeneration;
			resource.cold = false;
		}
		resource.residentBytes =
			resource.vertexBuffer.memorySize +
			resource.indexBuffer.memorySize +
			resource.accelerationStructure.memorySize +
			resource.shadowProxy.vertexBuffer.memorySize +
			resource.shadowProxy.indexBuffer.memorySize +
			resource.shadowProxy.accelerationStructure.memorySize;
		voxelResidentBytes += resource.residentBytes;
		if (resource.cold)
		{
			coldMeshCount++;
			coldBytes += resource.residentBytes;
		}
	}
	for (auto& pair : materialVariantResources)
	{
		PersistentVoxelMaterialVariantResource& resource = pair.second;
		resource.activeActorReferences = 0;
		auto activeIt = activeMaterialReferences.find(pair.first);
		if (activeIt != activeMaterialReferences.end())
		{
			resource.activeActorReferences = activeIt->second;
			resource.lastUsedFrame = frameIndex;
			resource.lastUsedMapGeneration = residencyMapGeneration;
			resource.cold = false;
		}
		resource.residentBytes = (uint64_t)resource.materialBridge.materials.size() * (uint64_t)sizeof(nri_scene::MaterialData);
		if (resource.cold)
		{
			coldMaterialCount++;
		}
	}

	const uint64_t maxResidentBytes = settings.residentMaxBytes;
	const uint64_t minHeadroomBytes = settings.residentMinHeadroomBytes;
	const uint32_t maxColdMaps = settings.residentMaxColdMaps;
	const bool loadingTrimCold = settings.trimColdOnLoading && loadingPhase;
	uint64_t pressureBytes = 0;
	if (maxResidentBytes != 0 && voxelResidentBytes > maxResidentBytes)
	{
		pressureBytes = std::max<uint64_t>(pressureBytes, voxelResidentBytes - maxResidentBytes);
	}
	if (maxResidentBytes == 0 && adapterLocalBudget > minHeadroomBytes && totalTrackedBytes + minHeadroomBytes > adapterLocalBudget)
	{
		pressureBytes = std::max<uint64_t>(pressureBytes, totalTrackedBytes + minHeadroomBytes - adapterLocalBudget);
	}

	struct MeshEvictionCandidate
	{
		uint64_t key = 0;
		uint64_t bytes = 0;
		uint32_t primitiveCount = 0;
		uint32_t lastMap = 0;
		uint32_t lastFrame = 0;
		uint32_t sourceBits = 0;
		int32_t priority = 0;
		bool force = false;
		bool prefer = false;
		bool coldAge = false;
		bool loadingTrim = false;
	};
	std::vector<MeshEvictionCandidate> meshCandidates;
	meshCandidates.reserve(meshVariantResources.size());
	for (const auto& pair : meshVariantResources)
	{
		const PersistentVoxelMeshVariantResource& resource = pair.second;
		if (!resource.cold || resource.activeActorReferences != 0 || admissionMeshes.find(pair.first) != admissionMeshes.end())
		{
			continue;
		}
		const uint32_t ageMaps = residencyMapGeneration >= resource.lastDesiredMapGeneration ?
			residencyMapGeneration - resource.lastDesiredMapGeneration : 0u;
		const bool oldEnough = maxColdMaps != UINT32_MAX && ageMaps > maxColdMaps;
		if (pressureBytes == 0 && !oldEnough && !loadingTrimCold)
		{
			continue;
		}
		meshCandidates.push_back({ pair.first, resource.residentBytes, resource.primitiveCount, resource.lastDesiredMapGeneration,
			resource.lastUsedFrame, resource.sourceBits, resource.priority, resource.gpuForce, resource.gpuPrefer, oldEnough, loadingTrimCold });
	}
	std::sort(meshCandidates.begin(), meshCandidates.end(), [](const MeshEvictionCandidate& left, const MeshEvictionCandidate& right)
	{
		if (left.force != right.force)
		{
			return !left.force;
		}
		if (left.prefer != right.prefer)
		{
			return !left.prefer;
		}
		if (left.priority != right.priority)
		{
			return left.priority > right.priority;
		}
		if (left.lastMap != right.lastMap)
		{
			return left.lastMap < right.lastMap;
		}
		if (left.lastFrame != right.lastFrame)
		{
			return left.lastFrame < right.lastFrame;
		}
		return left.bytes > right.bytes;
	});

	uint64_t evictedBytes = 0;
	uint32_t evictedMeshes = 0;
	for (const MeshEvictionCandidate& candidate : meshCandidates)
	{
		if (pressureBytes != 0 && evictedBytes >= pressureBytes && !candidate.coldAge && !candidate.loadingTrim)
		{
			continue;
		}
		auto it = meshVariantResources.find(candidate.key);
		if (it == meshVariantResources.end())
		{
			continue;
		}
		PersistentVoxelMeshVariantResource& resource = it->second;
		const char* reason = candidate.coldAge ? "cold-age" : (candidate.loadingTrim ? "loading-cold" : "pressure");
		if (traceEnabled)
		{
			Printf("NRI PT voxel residency evict: reason=%s phase=%s tex=-1 voxel=-1 mesh_variant=0x%llx bytes=%llu prims=%u last_map=%u last_frame=%u source_bits=0x%x priority=%d force=%u prefer=%u active_refs=%u\n",
				reason,
				phase != nullptr ? phase : "unknown",
				(unsigned long long)candidate.key,
				(unsigned long long)candidate.bytes,
				resource.primitiveCount,
				resource.lastDesiredMapGeneration,
				resource.lastUsedFrame,
				resource.sourceBits,
				resource.priority,
				resource.gpuForce ? 1u : 0u,
				resource.gpuPrefer ? 1u : 0u,
				resource.activeActorReferences);
		}
		services.RetireBuffer(resource.vertexBuffer);
		services.RetireBuffer(resource.indexBuffer);
		services.RetireAccelerationStructure(resource.accelerationStructure);
		RetirePersistentVoxelShadowProxy(resource.shadowProxy, services);
		for (auto instIt = instances.begin(); instIt != instances.end(); )
		{
			if (instIt->second.meshResourceKey == candidate.key)
			{
				instIt = instances.erase(instIt);
			}
			else
			{
				++instIt;
			}
		}
		evictedBytes += candidate.bytes;
		evictedMeshes++;
		meshVariantResources.erase(it);
		publishedMeshKeys.erase(candidate.key);
	}

	uint32_t evictedMaterials = 0;
	for (auto it = materialVariantResources.begin();
		materialReleaseQuiescent && it != materialVariantResources.end(); )
	{
		PersistentVoxelMaterialVariantResource& resource = it->second;
		if (!resource.cold || resource.activeActorReferences != 0 || admissionMaterials.find(it->first) != admissionMaterials.end())
		{
			++it;
			continue;
		}
		const uint32_t ageMaps = residencyMapGeneration >= resource.lastDesiredMapGeneration ?
			residencyMapGeneration - resource.lastDesiredMapGeneration : 0u;
		const bool oldEnough = maxColdMaps != UINT32_MAX && ageMaps > maxColdMaps;
		if (pressureBytes == 0 && !oldEnough && !loadingTrimCold)
		{
			++it;
			continue;
		}
		if (traceEnabled)
		{
			Printf("NRI PT voxel residency evict: reason=%s phase=%s tex=-1 voxel=-1 mesh_variant=0x0 mat_variant=0x%llx bytes=%llu last_map=%u last_frame=%u source_bits=0x%x priority=%d force=%u prefer=%u active_refs=%u\n",
				oldEnough ? "cold-age" : (loadingTrimCold ? "loading-cold" : "pressure"),
				phase != nullptr ? phase : "unknown",
				(unsigned long long)it->first,
				(unsigned long long)resource.residentBytes,
				resource.lastDesiredMapGeneration,
				resource.lastUsedFrame,
				resource.sourceBits,
				resource.priority,
				resource.gpuForce ? 1u : 0u,
				resource.gpuPrefer ? 1u : 0u,
				resource.activeActorReferences);
		}
		const NRIPersistentVoxelMaterialRangeHandle releasedRange =
			PersistentVoxelMaterialRangeHandle(resource);
		if (!releasedRange || !materialRangeAllocator.Release(releasedRange))
		{
			++it;
			continue;
		}
		publishedMaterialKeys.erase(it->first);
		dirtyMaterialResourceKeys.erase(it->first);
		it = materialVariantResources.erase(it);
		materialResourceGeneration++;
		evictedMaterials++;
	}

	if (traceEnabled || evictedMeshes != 0 || evictedMaterials != 0)
	{
		const NRIPersistentVoxelMaterialRangeStats& rangeStats = materialRangeAllocator.Stats();
		Printf("NRI PT voxel residency pressure: phase=%s tracked=%llu adapter_budget=%llu headroom=%llu voxel_bytes=%llu max_bytes=%llu queued_bytes=%llu cold_mesh=%u cold_material=%u cold_bytes=%llu pressure_bytes=%llu evicted_mesh=%u evicted_material=%u evicted_bytes=%llu material_release_quiescent=%u material_cursor_rows=%llu material_live_rows=%llu material_hole_rows=%llu material_reused_rows=%llu action=%s\n",
			phase != nullptr ? phase : "unknown",
			(unsigned long long)totalTrackedBytes,
			(unsigned long long)adapterLocalBudget,
			(unsigned long long)minHeadroomBytes,
			(unsigned long long)voxelResidentBytes,
			(unsigned long long)maxResidentBytes,
			(unsigned long long)queuedBytes,
			coldMeshCount,
			coldMaterialCount,
			(unsigned long long)coldBytes,
			(unsigned long long)pressureBytes,
			evictedMeshes,
			evictedMaterials,
			(unsigned long long)evictedBytes,
			materialReleaseQuiescent ? 1u : 0u,
			(unsigned long long)rangeStats.cursorRows,
			(unsigned long long)rangeStats.liveRows,
			(unsigned long long)rangeStats.holeRows,
			(unsigned long long)rangeStats.reusedRows,
			(evictedMeshes != 0 || evictedMaterials != 0) ? "evict" : "none");
	}
	if (evictedMeshes != 0 || evictedMaterials != 0)
	{
		MarkMaintenanceMutation();
		RebuildAdmissionIndex(true);
	}
	else
	{
		maintenanceStats.pressureNoops++;
	}
	pressureProtectionBlocked = pressureBytes != 0 && evictedBytes < pressureBytes;
	maintenanceStats.pressureProtectionBlockedLast = pressureProtectionBlocked;
	if (pressureProtectionBlocked)
	{
		maintenanceStats.pressureProtectionBlockedEvaluations++;
	}
	pressureEvaluationGeneration = maintenanceMutationGeneration;
	pressureSettingsSignature = settingsSignature;
	pressureAdapterBudget = adapterLocalBudget;
	pressureEvaluationFrame = frameIndex;
	pressureEvaluationValid = true;
}

bool NRIPersistentVoxelResidency::PumpAdmissionQueue(
	const char* phase,
	uint64_t buildSerial,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	uint64_t totalTrackedBytes,
	uint64_t adapterLocalBudget,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& resetServices,
	const NRIPersistentVoxelAdmissionServices& admissionServices)
{
	const bool loadingPhase = phase != nullptr && std::strcmp(phase, "loading") == 0;
	const bool traceLevel1 = loadingTraceLevel >= 1 || voxelStatsEnabled;
	const bool traceLevel2 = loadingTraceLevel >= 2 || voxelStatsEnabled;
	maintenanceStats.pumpCalls++;
	maintenanceStats.pumpFastReturnLast = false;
	maintenanceStats.entriesScannedLastPump = 0;
	auto refreshMaintenanceCounts = [&]()
	{
		maintenanceStats.registryEntries = (uint32_t)admissionQueue.size();
		maintenanceStats.activeEntries = admissionIndex.ActiveCount();
		maintenanceStats.requiredReadyEntries = admissionIndex.RequiredReadyCount();
		maintenanceStats.optionalReadyEntries = admissionIndex.OptionalReadyCount();
		maintenanceStats.failedEntries = admissionIndex.FailedCount();
	};
	auto emitMaintenanceTrace = [&]()
	{
		refreshMaintenanceCounts();
		if ((int)perf_looptraceframes > 0 || voxelStatsEnabled)
		{
			const NRIPersistentVoxelMaterialRangeStats& rangeStats = materialRangeAllocator.Stats();
			Printf("PERF pt voxel maintenance NRI: frame=%u phase=%s registry=%u active=%u required_ready=%u optional_ready=%u failed=%u entries_scanned=%u pressure_entries_scanned=%u pressure_resource_rows=%u pump_fast_return=%u pressure_evaluated=%u pressure_skipped=%u pressure_reason=0x%x pressure_blocked=%u pressure_audit_frames=%u membership_changes=%llu membership_invalidations=%llu safety_audits=%llu blocked_evaluations=%llu pump_calls=%llu pump_fast_returns=%llu entries_scanned_total=%llu pressure_entries_scanned_total=%llu pressure_resource_rows_total=%llu pressure_evaluations=%llu pressure_noops=%llu pressure_skips=%llu memory_rebuilds=%llu memory_hits=%llu status_rebuilds=%llu status_hits=%llu mutation_generation=%llu material_cursor_rows=%llu material_live_rows=%llu material_hole_rows=%llu material_high_water_rows=%llu material_allocations=%llu material_releases=%llu material_reuse_allocations=%llu material_reused_rows=%llu material_compactions=%llu material_compacted_rows=%llu\n",
				frameIndex,
				phase != nullptr ? phase : "unknown",
				maintenanceStats.registryEntries,
				maintenanceStats.activeEntries,
				maintenanceStats.requiredReadyEntries,
				maintenanceStats.optionalReadyEntries,
				maintenanceStats.failedEntries,
				maintenanceStats.entriesScannedLastPump,
				maintenanceStats.pressureEntriesScannedLast,
				maintenanceStats.pressureResourceRowsScannedLast,
				maintenanceStats.pumpFastReturnLast ? 1u : 0u,
				maintenanceStats.pressureEvaluatedLast ? 1u : 0u,
				maintenanceStats.pressureSkippedLast ? 1u : 0u,
				maintenanceStats.pressureEvaluationReasonMaskLast,
				maintenanceStats.pressureProtectionBlockedLast ? 1u : 0u,
				settings.pressureSafetyAuditFrames,
				(unsigned long long)maintenanceStats.pressureMembershipChanges,
				(unsigned long long)maintenanceStats.pressureMembershipInvalidations,
				(unsigned long long)maintenanceStats.pressureSafetyAudits,
				(unsigned long long)maintenanceStats.pressureProtectionBlockedEvaluations,
				(unsigned long long)maintenanceStats.pumpCalls,
				(unsigned long long)maintenanceStats.pumpFastReturns,
				(unsigned long long)maintenanceStats.entriesScanned,
				(unsigned long long)maintenanceStats.pressureEntriesScanned,
				(unsigned long long)maintenanceStats.pressureResourceRowsScanned,
				(unsigned long long)maintenanceStats.pressureEvaluations,
				(unsigned long long)maintenanceStats.pressureNoops,
				(unsigned long long)maintenanceStats.pressureSkips,
				(unsigned long long)maintenanceStats.memorySnapshotRebuilds,
				(unsigned long long)maintenanceStats.memorySnapshotHits,
				(unsigned long long)maintenanceStats.resourceStatusRebuilds,
				(unsigned long long)maintenanceStats.resourceStatusHits,
				(unsigned long long)maintenanceMutationGeneration,
				(unsigned long long)rangeStats.cursorRows,
				(unsigned long long)rangeStats.liveRows,
				(unsigned long long)rangeStats.holeRows,
				(unsigned long long)rangeStats.highWaterRows,
				(unsigned long long)rangeStats.allocations,
				(unsigned long long)rangeStats.releases,
				(unsigned long long)rangeStats.reuseAllocations,
				(unsigned long long)rangeStats.reusedRows,
				(unsigned long long)materialRangeCompactions,
				(unsigned long long)materialRangeCompactedRows);
		}
	};
	SyncMapGeneration(
		buildSerial,
		"pump-map-generation",
		traceLevel1,
		resetServices);
	if (loadingPhase)
	{
		MarkMaintenanceMutation();
		RebuildAdmissionIndex(true);
	}
	else if (admissionIndex.HasActiveWork())
	{
		MarkMaintenanceMutation();
	}
	ApplyPressurePolicy(
		phase,
		frameIndex,
		settings,
		totalTrackedBytes,
		adapterLocalBudget,
		traceLevel1,
		resetServices);
	const uint32_t variantBudget = loadingPhase ?
		settings.admissionLoadVariants :
		settings.admissionRuntimeVariants;
	const uint64_t legacyByteBudget = loadingPhase ?
		settings.admissionLoadBytes :
		settings.admissionRuntimeBytes;
	const uint64_t chunkByteBudget = loadingPhase ?
		settings.admitMaxBytesLoading :
		settings.admitMaxBytesRuntime;
	auto combineNonZeroBudget = [](uint64_t left, uint64_t right) -> uint64_t
	{
		if (left == 0)
		{
			return right;
		}
		if (right == 0)
		{
			return left;
		}
		return std::min(left, right);
	};
	const uint64_t byteBudget = combineNonZeroBudget(legacyByteBudget, chunkByteBudget);
	const int configuredMsBudget = loadingPhase ?
		(int)settings.admitMaxMsLoading :
		(int)settings.admitMaxMsRuntime;
	const double msBudget = loadingPhase ?
		(configuredMsBudget > 0 ? (double)configuredMsBudget : 250.0) :
		(double)configuredMsBudget;
	const uint32_t blasBudgetLimit = loadingPhase ?
		settings.admitMaxBlasLoading :
		settings.admitMaxBlasRuntime;
	uint32_t blasBudgetRemaining = blasBudgetLimit;
	if (admissionScheduler.GetSnapshot().activeMapGeneration != residencyMapGeneration)
	{
		const uint64_t batchBytes = std::max<uint64_t>(
			std::max(settings.admitMaxBytesLoading, settings.admitMaxBytesRuntime),
			64ull * 1024ull * 1024ull);
		const uint64_t maxJobs = std::max<uint64_t>(1u, settings.computeMaxJobs);
		const uint64_t maxReservedBytes = batchBytes > UINT64_MAX / maxJobs ? UINT64_MAX : batchBytes * maxJobs;
		NRIVoxelAdmissionLimits limits = {};
		limits.activeMapGeneration = residencyMapGeneration;
		limits.maxActiveJobs = settings.computeMaxJobs;
		limits.maxReservedBytes = maxReservedBytes;
		limits.maxReservedBlasBytes = maxReservedBytes;
		limits.maxComputeSlots = settings.computeMaxJobs;
		limits.maxBlasLanes = settings.computeMaxJobs;
		limits.oversizedReservationBytes = batchBytes;
		limits.oversizedBlasBytes = batchBytes;
		limits.optionalActiveJobReserve = settings.computeMaxJobs > 1 ? 1u : 0u;
		limits.optionalByteReserve = settings.computeMaxJobs > 1 ? batchBytes : 0u;
		limits.optionalBlasByteReserve = settings.computeMaxJobs > 1 ? batchBytes : 0u;
		limits.optionalComputeSlotReserve = settings.computeMaxJobs > 1 ? 1u : 0u;
		limits.optionalBlasLaneReserve = settings.computeMaxJobs > 1 ? 1u : 0u;
		admissionScheduler = NRIVoxelAdmissionScheduler(limits);
	}
	const NRIVoxelAdmissionSnapshot initialScheduler = admissionScheduler.GetSnapshot();
	const bool schedulerQuiescent = PersistentVoxelAdmissionSchedulerQuiescent(initialScheduler);
	if (!admissionIndex.HasActiveWork() && schedulerQuiescent)
	{
		maintenanceStats.pumpFastReturnLast = true;
		maintenanceStats.pumpFastReturns++;
		emitMaintenanceTrace();
		return true;
	}
	maintenanceStats.entriesScannedLastPump = admissionIndex.ActiveCount();
	maintenanceStats.entriesScanned += maintenanceStats.entriesScannedLastPump;
	const auto pumpStart = std::chrono::steady_clock::now();
	auto elapsedMs = [&]() -> double
	{
		return PersistentVoxelDurationMs(pumpStart, std::chrono::steady_clock::now());
	};
	auto isUploadState = [](PersistentVoxelAdmissionState state) -> bool
	{
		return state == PersistentVoxelAdmissionState::DirectComputePending ||
			state == PersistentVoxelAdmissionState::DirectBlasPending ||
			state == PersistentVoxelAdmissionState::UploadingVertices ||
			state == PersistentVoxelAdmissionState::UploadingIndices ||
			state == PersistentVoxelAdmissionState::UploadingPrimitives ||
			state == PersistentVoxelAdmissionState::BuildingBlas;
	};

	uint32_t requiredPendingAtStart = 0;
	uint32_t requiredReadyAtStart = 0;
	uint32_t optionalPendingAtStart = 0;
	uint32_t failedAtStart = 0;
	CountAdmissionWork(requiredPendingAtStart, requiredReadyAtStart, optionalPendingAtStart, failedAtStart);
	const bool requiredOnlyPump = loadingPhase && requiredPendingAtStart != 0;
	const bool postLoadGraceActive = !loadingPhase && IsPostLoadAdmissionGraceActive(frameIndex);

	PersistentVoxelAdmissionStats stats = {};
	auto frameAge = [frameIndex](uint32_t startFrame) -> uint32_t
	{
		return startFrame != UINT32_MAX && frameIndex >= startFrame ? frameIndex - startFrame : 0u;
	};
	auto stampQueuedEntry = [&](PersistentVoxelAdmissionEntry& entry)
	{
		if (entry.firstQueuedFrame == UINT32_MAX)
		{
			entry.firstQueuedFrame = frameIndex;
		}
		if (entry.runtimeRequested && entry.runtimeFirstQueuedFrame == UINT32_MAX)
		{
			entry.runtimeFirstQueuedFrame = frameIndex;
		}
	};
	auto noteQueuedEntry = [&](const PersistentVoxelAdmissionEntry& entry)
	{
		switch (entry.state)
		{
		case PersistentVoxelAdmissionState::Pending:
		case PersistentVoxelAdmissionState::Deferred:
			stats.statePending++;
			break;
		case PersistentVoxelAdmissionState::DirectComputePending:
			stats.stateDirectPending++;
			break;
		case PersistentVoxelAdmissionState::DirectBlasPending:
			stats.stateDirectBlasPending++;
			stats.stateBuildingBlas++;
			break;
		case PersistentVoxelAdmissionState::UploadingVertices:
		case PersistentVoxelAdmissionState::UploadingIndices:
		case PersistentVoxelAdmissionState::UploadingPrimitives:
			stats.stateUploading++;
			break;
		case PersistentVoxelAdmissionState::BuildingBlas:
			stats.stateBuildingBlas++;
			break;
		default:
			break;
		}
		stats.oldestQueueAgeFrames = std::max(stats.oldestQueueAgeFrames, frameAge(entry.firstQueuedFrame));
		if (entry.runtimeRequested)
		{
			stats.runtimePending++;
			stats.oldestRuntimeAgeFrames = std::max(stats.oldestRuntimeAgeFrames, frameAge(entry.runtimeFirstQueuedFrame));
			if (entry.state == PersistentVoxelAdmissionState::DirectComputePending ||
				entry.state == PersistentVoxelAdmissionState::DirectBlasPending)
			{
				stats.runtimeDirectPending++;
			}
		}
		if (entry.state == PersistentVoxelAdmissionState::DirectComputePending ||
			entry.state == PersistentVoxelAdmissionState::DirectBlasPending)
		{
			stats.oldestDirectAgeFrames = std::max(stats.oldestDirectAgeFrames, frameAge(entry.directComputeRequestFrame));
		}
	};
	std::vector<PersistentVoxelAdmissionEntry*> candidates;
	candidates.reserve(admissionIndex.ActiveCount());
	std::vector<uint64_t> drainedReadyKeys;
	drainedReadyKeys.reserve(admissionIndex.ActiveCount());
	for (uint64_t key : admissionIndex.ActiveKeys())
	{
		auto found = admissionQueue.find(key);
		if (found == admissionQueue.end())
		{
			continue;
		}
		PersistentVoxelAdmissionEntry& entry = found->second;
		const uint64_t meshResourceKey = BuildPersistentVoxelVariantMeshResourceKey(entry.variant);
		const PersistentVoxelReadinessStatus readiness = GetSharedVariantReadiness(meshResourceKey, entry.variant.materialKeyHash);
		const bool resourcesReady = readiness.ready;
		if (resourcesReady)
		{
			if (entry.state != PersistentVoxelAdmissionState::Ready && entry.runtimeRequested)
			{
				TraceReadiness("skip-ready", phase, &entry, meshResourceKey, entry.variant.materialKeyHash, readiness, traceLevel2);
			}
			if (entry.uploadPrepared)
			{
				DiscardAdmissionEntry(entry, resetServices);
			}
			entry.state = PersistentVoxelAdmissionState::Ready;
			entry.lastReason = "resident";
			stats.ready++;
			drainedReadyKeys.push_back(key);
			continue;
		}
		if (entry.state == PersistentVoxelAdmissionState::Ready)
		{
			TraceReadiness("stale-ready", phase, &entry, meshResourceKey, entry.variant.materialKeyHash, readiness, traceLevel2);
			entry.state = PersistentVoxelAdmissionState::Pending;
			entry.lastReason = "stale-ready";
		}
		if (entry.state == PersistentVoxelAdmissionState::Failed)
		{
			stats.failed++;
			continue;
		}
		stampQueuedEntry(entry);
		if (entry.state == PersistentVoxelAdmissionState::Deferred)
		{
			stats.deferred++;
		}
		noteQueuedEntry(entry);
		stats.queued++;
		stats.bytesPending += entry.estimatedBytes;
		if (entry.gpuForce)
		{
			stats.force++;
		}
		if (entry.gpuPrefer)
		{
			stats.prefer++;
		}
		if (entry.runtimeRequested)
		{
			stats.runtime++;
		}
		candidates.push_back(&entry);
	}
	for (uint64_t key : drainedReadyKeys)
	{
		const auto found = admissionQueue.find(key);
		if (found != admissionQueue.end())
		{
			admissionIndex.Transition(
				key,
				NRIPersistentVoxelAdmissionBucket::Active,
				GetAdmissionBucket(found->second));
		}
	}

	std::sort(candidates.begin(), candidates.end(), [&](const PersistentVoxelAdmissionEntry* left, const PersistentVoxelAdmissionEntry* right)
	{
		const bool leftUploading = isUploadState(left->state);
		const bool rightUploading = isUploadState(right->state);
		if (leftUploading != rightUploading)
		{
			return leftUploading;
		}
		if (!loadingPhase && left->runtimeRequested != right->runtimeRequested)
		{
			return left->runtimeRequested;
		}
		const bool leftDirectPending =
			left->state == PersistentVoxelAdmissionState::DirectComputePending ||
			left->state == PersistentVoxelAdmissionState::DirectBlasPending;
		const bool rightDirectPending =
			right->state == PersistentVoxelAdmissionState::DirectComputePending ||
			right->state == PersistentVoxelAdmissionState::DirectBlasPending;
		if (leftDirectPending != rightDirectPending)
		{
			return leftDirectPending;
		}
		if (left->admissionRank != right->admissionRank)
		{
			return left->admissionRank < right->admissionRank;
		}
		if (left->priority != right->priority)
		{
			return left->priority < right->priority;
		}
		if (left->gpuForce != right->gpuForce)
		{
			return left->gpuForce;
		}
		if (left->gpuPrefer != right->gpuPrefer)
		{
			return left->gpuPrefer;
		}
		if (left->runtimeRequested != right->runtimeRequested)
		{
			return left->runtimeRequested;
		}
		if (left->variant.primitiveCount != right->variant.primitiveCount)
		{
			return left->variant.primitiveCount > right->variant.primitiveCount;
		}
		return left->pairKey < right->pairKey;
	});

	uint32_t admitted = 0;
	uint32_t blasUsed = 0;
	uint32_t requiredAdmitted = 0;
	uint32_t optionalAdmitted = 0;
	uint32_t optionalSkippedGrace = 0;
	const char* stopReason = "queue-drained";
	for (PersistentVoxelAdmissionEntry* entry : candidates)
	{
		const uint64_t meshResourceKey = BuildPersistentVoxelVariantMeshResourceKey(entry->variant);
		const PersistentVoxelReadinessStatus currentReadiness = GetSharedVariantReadiness(meshResourceKey, entry->variant.materialKeyHash);
		if (currentReadiness.ready)
		{
			if (entry->state != PersistentVoxelAdmissionState::Ready || entry->runtimeRequested)
			{
				TraceReadiness("skip-ready", phase, entry, meshResourceKey, entry->variant.materialKeyHash, currentReadiness, traceLevel2);
			}
			if (entry->uploadPrepared)
			{
				DiscardAdmissionEntry(*entry, resetServices);
			}
			entry->state = PersistentVoxelAdmissionState::Ready;
			entry->lastReason = "resident";
			stats.ready++;
			continue;
		}
		if (entry->state == PersistentVoxelAdmissionState::Ready)
		{
			TraceReadiness("stale-ready", phase, entry, meshResourceKey, entry->variant.materialKeyHash, currentReadiness, traceLevel2);
			entry->state = PersistentVoxelAdmissionState::Pending;
			entry->lastReason = "stale-ready";
		}
		if (requiredOnlyPump && !IsRequiredAdmission(*entry))
		{
			continue;
		}
		const bool requiredAdmission = IsRequiredAdmission(*entry);
		const bool uploadState = isUploadState(entry->state);
		if (!loadingPhase && !entry->runtimeRequested && !requiredAdmission && !uploadState)
		{
			stats.skippedBudget++;
			entry->state = PersistentVoxelAdmissionState::Deferred;
			entry->lastReason = "optional-preload-runtime";
			stopReason = entry->lastReason;
			continue;
		}
		if (postLoadGraceActive && !entry->runtimeRequested && !requiredAdmission && !uploadState && optionalAdmitted >= settings.admissionGraceVariants)
		{
			stats.skippedBudget++;
			optionalSkippedGrace++;
			entry->state = PersistentVoxelAdmissionState::Deferred;
			entry->lastReason = "post-load-grace";
			stopReason = "post-load-grace";
			continue;
		}
		if (msBudget > 0.0 && elapsedMs() >= msBudget)
		{
			stopReason = "ms-budget";
			break;
		}
		if (admitted >= variantBudget && !uploadState)
		{
			stats.skippedBudget++;
			entry->state = PersistentVoxelAdmissionState::Deferred;
			entry->lastReason = "variant-budget";
			continue;
		}
		if (byteBudget != 0 && stats.bytesUploaded >= byteBudget)
		{
			stats.skippedBudget++;
			if (!uploadState)
			{
				entry->state = PersistentVoxelAdmissionState::Deferred;
			}
			entry->lastReason = "byte-budget";
			stopReason = "byte-budget";
			break;
		}

		uint64_t uploadBytes = 0;
		bool reusedMesh = false;
		bool reusedMaterial = false;
		bool inProgress = false;
		const bool directRequestWasPending =
			(entry->state == PersistentVoxelAdmissionState::DirectComputePending ||
			 entry->state == PersistentVoxelAdmissionState::DirectBlasPending) &&
			entry->directComputeRequested;
		const char* failureReason = "none";
		const bool exclusiveRuntimeAdmission =
			!loadingPhase &&
			entry->runtimeRequested &&
			byteBudget != 0 &&
			entry->estimatedBytes > byteBudget;
		const uint64_t remainingByteBudget = exclusiveRuntimeAdmission ? 0ull : (byteBudget == 0 ? 0ull : byteBudget - stats.bytesUploaded);
		const uint32_t blasBefore = blasBudgetRemaining;
		const int isolateBlasPrimitiveThreshold = settings.admitIsolateBlasPrimitives;
		const bool isolateBlasBuild =
			isolateBlasPrimitiveThreshold > 0 &&
			entry->variant.primitiveCount >= (uint32_t)isolateBlasPrimitiveThreshold;
		if (!admissionServices.AdmitVariantResource(*entry, remainingByteBudget, blasBudgetRemaining, uploadBytes, reusedMesh, reusedMaterial, inProgress, isolateBlasBuild, failureReason, &stats))
		{
			if (admissionServices.IsSubmitBudgetHit())
			{
				entry->lastReason = "preload-submit-budget";
				stopReason = entry->lastReason;
				break;
			}
			entry->state = PersistentVoxelAdmissionState::Failed;
			entry->retryCount++;
			entry->lastReason = failureReason != nullptr ? failureReason : "admit-failed";
			stats.failedThisPump++;
			if (traceLevel1)
			{
				Printf("NRI PT voxel admission queue: event=failed source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu reason=%s\n",
					entry->sourceBits,
					entry->priority,
					entry->admissionRank,
					entry->gpuForce ? 1u : 0u,
					entry->gpuPrefer ? 1u : 0u,
					entry->runtimeRequested ? 1u : 0u,
					entry->variant.sourcePicnum,
					entry->variant.resolvedVoxelIndex,
					(unsigned long long)entry->variant.meshKeyHash,
					(unsigned long long)entry->variant.materialKeyHash,
					entry->variant.primitiveCount,
					(unsigned long long)entry->estimatedBytes,
					entry->lastReason);
			}
			break;
		}
		const uint32_t blasBuiltThisEntry = blasBefore - blasBudgetRemaining;
		blasUsed += blasBuiltThisEntry;
		entry->bytesUploaded += uploadBytes;
		stats.bytesUploaded += uploadBytes;
		if (uploadBytes != 0 && !entry->uploadGeometryFromCompute)
		{
			cumulativeCpuGeometryUploadBytes += uploadBytes;
			if (!entry->cpuGeometryUploadCounted)
			{
				entry->cpuGeometryUploadCounted = true;
				cumulativeCpuGeometryUploadCount++;
			}
		}
		if (inProgress)
		{
			if (entry->state == PersistentVoxelAdmissionState::BuildingBlas && blasBudgetRemaining == 0)
			{
				entry->lastReason = "blas-budget";
			}
			else if (entry->lastReason == nullptr || std::strcmp(entry->lastReason, "none") == 0)
			{
				entry->lastReason = "uploading";
			}
			const bool directRequestNowPending =
				(entry->state == PersistentVoxelAdmissionState::DirectComputePending ||
				 entry->state == PersistentVoxelAdmissionState::DirectBlasPending) &&
				entry->directComputeRequested;
			stopReason = entry->lastReason;
			if (traceLevel2)
			{
				Printf("NRI PT voxel admission queue: event=in-progress phase=%s source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu upload_bytes=%llu state=%u reason=%s\n",
					phase != nullptr ? phase : "unknown",
					entry->sourceBits,
					entry->priority,
					entry->admissionRank,
					entry->gpuForce ? 1u : 0u,
					entry->gpuPrefer ? 1u : 0u,
					entry->runtimeRequested ? 1u : 0u,
					entry->variant.sourcePicnum,
					entry->variant.resolvedVoxelIndex,
					(unsigned long long)entry->variant.meshKeyHash,
					(unsigned long long)entry->variant.materialKeyHash,
					entry->variant.primitiveCount,
					(unsigned long long)entry->estimatedBytes,
					(unsigned long long)uploadBytes,
					(uint32_t)entry->state,
					entry->lastReason);
			}
			if (directRequestNowPending)
			{
				if (!directRequestWasPending)
				{
					admitted++;
					if (requiredAdmission)
					{
						requiredAdmitted++;
					}
					else
					{
						optionalAdmitted++;
					}
				}
				continue;
			}
			break;
		}
		if (entry->state == PersistentVoxelAdmissionState::Deferred)
		{
			stats.skippedBudget++;
			continue;
		}

		entry->state = PersistentVoxelAdmissionState::Ready;
		entry->lastReason = reusedMesh && reusedMaterial ? "already-resident" : "admitted";
		stats.uploaded++;
		admitted++;
		if (requiredAdmission)
		{
			requiredAdmitted++;
		}
		else
		{
			optionalAdmitted++;
		}
		if (traceLevel2)
		{
			Printf("NRI PT voxel admission queue: event=ready phase=%s source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu upload_bytes=%llu reused_mesh=%u reused_material=%u\n",
				phase != nullptr ? phase : "unknown",
				entry->sourceBits,
				entry->priority,
				entry->admissionRank,
				entry->gpuForce ? 1u : 0u,
				entry->gpuPrefer ? 1u : 0u,
				entry->runtimeRequested ? 1u : 0u,
				entry->variant.sourcePicnum,
				entry->variant.resolvedVoxelIndex,
				(unsigned long long)entry->variant.meshKeyHash,
				(unsigned long long)entry->variant.materialKeyHash,
				entry->variant.primitiveCount,
				(unsigned long long)entry->estimatedBytes,
				(unsigned long long)uploadBytes,
				reusedMesh ? 1u : 0u,
				reusedMaterial ? 1u : 0u);
		}
		if (blasBuiltThisEntry != 0 &&
			(loadingPhase || (isolateBlasBuild && entry->state != PersistentVoxelAdmissionState::DirectBlasPending)))
		{
			const char* submitReason = loadingPhase ? "voxel-loading-blas" : "voxel-runtime-large-blas";
			if (traceLevel1 && !loadingPhase)
			{
				Printf("NRI PT voxel admission entry: event=post-blas-submit tex=%d voxel=%d mesh_variant=0x%llx prims=%u reason=isolate-large-runtime-blas\n",
					entry->variant.sourcePicnum,
					entry->variant.resolvedVoxelIndex,
					(unsigned long long)entry->variant.meshKeyHash,
					entry->variant.primitiveCount);
			}
			if (!admissionServices.SubmitWaitAndRestart(submitReason))
			{
				if (admissionServices.IsSubmitBudgetHit())
				{
					entry->lastReason = "preload-submit-budget";
					stopReason = entry->lastReason;
					break;
				}
				entry->state = PersistentVoxelAdmissionState::Failed;
				entry->retryCount++;
				entry->lastReason = "blas-submit-wait-failed";
				stats.failedThisPump++;
				stopReason = entry->lastReason;
				break;
			}
			stopReason = loadingPhase ? "blas-submit-wait" : "runtime-blas-submit-wait";
			break;
		}
	}
	for (PersistentVoxelAdmissionEntry* entry : candidates)
	{
		if (entry != nullptr &&
			(entry->state == PersistentVoxelAdmissionState::Ready ||
			 entry->state == PersistentVoxelAdmissionState::Failed))
		{
			admissionIndex.Transition(
				entry->pairKey,
				NRIPersistentVoxelAdmissionBucket::Active,
				GetAdmissionBucket(*entry));
		}
	}

	const bool hasQueueActivity = !candidates.empty() || stats.uploaded != 0 || stats.skippedBudget != 0 || stats.failedThisPump != 0;
	const bool perfLoopTraceActive = (int)perf_looptraceframes > 0;
	if (((loadingTraceLevel >= 1) && (loadingPhase || hasQueueActivity)) ||
		(voxelStatsEnabled && hasQueueActivity) ||
		(perfLoopTraceActive && hasQueueActivity))
	{
		uint32_t requiredPending = 0;
		uint32_t requiredReady = 0;
		uint32_t optionalPending = 0;
		uint32_t failed = 0;
		CountAdmissionWork(requiredPending, requiredReady, optionalPending, failed);
		auto printAdmissionSummary = [&](const char* prefix)
		{
			Printf("%s phase=%s queued=%u ready=%u deferred=%u failed=%u uploaded=%u skipped_budget=%u bytes_pending=%llu bytes_uploaded=%llu force=%u prefer=%u runtime=%u state_pending=%u state_direct_pending=%u state_uploading=%u state_building_blas=%u runtime_pending=%u runtime_direct_pending=%u oldest_queue_age=%u oldest_runtime_age=%u oldest_direct_age=%u direct_requests=%u direct_ready=%u direct_pending=%u direct_failures=%u direct_rejected=%u direct_stale_drops=%u direct_ready_latency_samples=%u direct_ready_latency_avg=%.3f direct_ready_latency_max=%u direct_blas_latency_samples=%u direct_blas_latency_avg=%.3f direct_blas_latency_max=%u cpu_geometry_avoided=%u cpu_geometry_fallback=%u material_builds=%u material_reuses=%u material_bytes=%llu required_pending=%u required_ready=%u optional_pending=%u grace_active=%u grace_end=%u grace_variants=%u grace_skipped_optional=%u required_admitted=%u optional_admitted=%u variants_budget=%u bytes_budget=%llu ms_budget=%.3f ms_used=%.3f blas_budget=%u blas_used=%u stop=%s\n",
				prefix,
				phase != nullptr ? phase : "unknown",
				stats.queued,
				stats.ready,
				stats.deferred,
				stats.failed + stats.failedThisPump,
				stats.uploaded,
				stats.skippedBudget,
				(unsigned long long)stats.bytesPending,
				(unsigned long long)stats.bytesUploaded,
				stats.force,
				stats.prefer,
				stats.runtime,
				stats.statePending,
				stats.stateDirectPending,
				stats.stateUploading,
				stats.stateBuildingBlas,
				stats.runtimePending,
				stats.runtimeDirectPending,
				stats.oldestQueueAgeFrames,
				stats.oldestRuntimeAgeFrames,
				stats.oldestDirectAgeFrames,
				stats.directRequests,
				stats.directReady,
				stats.directPending,
				stats.directFailures,
				stats.directRejected,
				stats.directStaleDrops,
				stats.directReadyLatencySamples,
				stats.directReadyLatencySamples != 0 ? (double)stats.totalDirectReadyLatencyFrames / (double)stats.directReadyLatencySamples : 0.0,
				stats.maxDirectReadyLatencyFrames,
				stats.directBlasLatencySamples,
				stats.directBlasLatencySamples != 0 ? (double)stats.totalDirectBlasLatencyFrames / (double)stats.directBlasLatencySamples : 0.0,
				stats.maxDirectBlasLatencyFrames,
				stats.cpuGeometryAvoided,
				stats.cpuGeometryFallback,
				stats.materialPayloadBuilds,
				stats.materialPayloadReuses,
				(unsigned long long)stats.materialPayloadBytes,
				requiredPending,
				requiredReady,
				optionalPending,
				postLoadGraceActive ? 1u : 0u,
				postLoadGraceActive ? postLoadAdmissionGraceEndFrame : 0u,
				settings.admissionGraceVariants,
				optionalSkippedGrace,
				requiredAdmitted,
				optionalAdmitted,
				variantBudget,
				(unsigned long long)byteBudget,
				msBudget,
				elapsedMs(),
				blasBudgetLimit,
				blasUsed,
				stopReason);
		};
		printAdmissionSummary("NRI PT voxel admission summary:");
		printAdmissionSummary("PERF pt voxel admission summary NRI:");
		const NRIVoxelAdmissionSnapshot scheduler = admissionScheduler.GetSnapshot();
		const NRIVoxelAdmissionInvariantReport invariants = admissionScheduler.ValidateInvariants();
		Printf("PERF pt voxel scheduler NRI: phase=%s generation=%llu active=%u bindings=%u owners=%u pair_owners=%u dependency=%u compute_queued=%u compute_submitted=%u compute_ready=%u blas_queued=%u blas_submitted=%u publication=%u tlas_pending=%u ready=%u compute_inflight=%u blas_inflight=%u reserved=%llu blas_reserved=%llu held=%llu held_blas=%llu retire_pending=%llu abandoned=%llu accepted=%llu attached=%llu rejected=%llu invalid_transitions=%llu invariant_valid=%u invariant_flags=0x%x\n",
			phase != nullptr ? phase : "unknown",
			(unsigned long long)scheduler.activeMapGeneration,
			scheduler.activeJobs,
			scheduler.activeBindings,
			scheduler.meshOwnerCount,
			scheduler.pairOwnerCount,
			scheduler.stageCounts[(size_t)NRIVoxelAdmissionStage::DependencyPending],
			scheduler.stageCounts[(size_t)NRIVoxelAdmissionStage::ComputeQueued],
			scheduler.stageCounts[(size_t)NRIVoxelAdmissionStage::ComputeSubmitted],
			scheduler.stageCounts[(size_t)NRIVoxelAdmissionStage::ComputeReady],
			scheduler.stageCounts[(size_t)NRIVoxelAdmissionStage::BlasQueued],
			scheduler.stageCounts[(size_t)NRIVoxelAdmissionStage::BlasSubmitted],
			scheduler.stageCounts[(size_t)NRIVoxelAdmissionStage::PublicationPending],
			scheduler.stageCounts[(size_t)NRIVoxelAdmissionStage::TlasPending],
			scheduler.stageCounts[(size_t)NRIVoxelAdmissionStage::Ready],
			scheduler.computeInFlight,
			scheduler.blasInFlight,
			(unsigned long long)scheduler.activeReservationBytes,
			(unsigned long long)scheduler.activeBlasBytes,
			(unsigned long long)scheduler.heldReservationBytes,
			(unsigned long long)scheduler.heldBlasBytes,
			(unsigned long long)scheduler.retirePendingBytes,
			(unsigned long long)scheduler.abandonedBytes,
			(unsigned long long)scheduler.acceptedJobs,
			(unsigned long long)scheduler.attachedBindings,
			(unsigned long long)scheduler.rejectedJobs,
			(unsigned long long)scheduler.invalidTransitions,
			invariants.valid ? 1u : 0u,
			(uint32_t)invariants.flags);
	}
	emitMaintenanceTrace();
	return stats.failedThisPump == 0;
}

void NRIPersistentVoxelResidency::ArmPostLoadAdmissionGrace(uint32_t frameIndex, const NRIPersistentVoxelSettings& settings, int loadingTraceLevel)
{
	if (settings.admissionGraceFrames == 0)
	{
		postLoadAdmissionGraceEndFrame = 0;
		postLoadAdmissionGraceMapGeneration = 0;
		return;
	}

	const uint32_t requestedEndFrame = frameIndex + settings.admissionGraceFrames;
	const bool sameGeneration = postLoadAdmissionGraceMapGeneration == residencyMapGeneration;
	postLoadAdmissionGraceEndFrame =
		sameGeneration ?
		std::max(postLoadAdmissionGraceEndFrame, requestedEndFrame) :
		requestedEndFrame;
	postLoadAdmissionGraceMapGeneration = residencyMapGeneration;
	if (loadingTraceLevel >= 1)
	{
		Printf("NRI PT voxel admission grace: event=arm frame=%u end_frame=%u frames=%u variants=%u generation=%u\n",
			frameIndex,
			postLoadAdmissionGraceEndFrame,
			settings.admissionGraceFrames,
			settings.admissionGraceVariants,
			residencyMapGeneration);
	}
}

bool NRIPersistentVoxelResidency::IsPostLoadAdmissionGraceActive(uint32_t frameIndex) const
{
	return
		postLoadAdmissionGraceEndFrame != 0 &&
		postLoadAdmissionGraceMapGeneration == residencyMapGeneration &&
		frameIndex < postLoadAdmissionGraceEndFrame;
}

bool NRIPersistentVoxelResidency::AdmitVariantResource(
	PersistentVoxelAdmissionEntry& entry,
	uint64_t byteBudget,
	uint32_t& blasBudget,
	uint64_t& outUploadBytes,
	bool& outReusedMesh,
	bool& outReusedMaterial,
	bool& outInProgress,
	bool isolateBlasBuild,
	const char*& outFailureReason,
	PersistentVoxelAdmissionStats* outStats,
	uint32_t frameIndex,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelAdmissionServices& services)
{
	outUploadBytes = 0;
	outReusedMesh = false;
	outReusedMaterial = false;
	outInProgress = false;
	outFailureReason = "none";
	const nri_scene::PrecachedVoxelVariantView& variant = entry.variant;
	const uint64_t meshResourceKey = BuildPersistentVoxelVariantMeshResourceKey(variant);
	const bool runtimeTailRequest =
		IsNRIVoxelComputePreloadRuntimeWithheldMesh(residencyLastBuildSerial, meshResourceKey) &&
		IsNRIVoxelComputePreloadRuntimeTailReleased(residencyLastBuildSerial);
	const bool runtimeProbeRequest = IsNRIVoxelComputePreloadRuntimeProbeMesh(residencyLastBuildSerial, meshResourceKey);
	const bool directOnlyAdmission = variant.directOnlyAdmission && ShouldDirectPublishNRIVoxelComputeMeshing();
	const uint32_t computeMaxJobs = (uint32_t)std::max(1, (int)nri_ptvoxelcomputemaxjobs);
	const uint32_t runtimeComputeMaxJobs = (uint32_t)std::clamp(
		(int)nri_ptvoxelcomputeruntimemaxjobs,
		1,
		(int)computeMaxJobs);
	if ((!directOnlyAdmission && variant.surface == nullptr) ||
		variant.meshKeyHash == 0 ||
		meshResourceKey == 0 ||
		variant.materialKeyHash == 0)
	{
		outFailureReason = "invalid-variant";
		return false;
	}

	auto cleanupPendingUpload = [&]()
	{
		if (entry.schedulerTokenId != 0 && entry.directComputeRequested)
		{
			admissionScheduler.Cancel(entry.schedulerTokenId);
			entry.schedulerTokenId = 0;
		}
		services.RetireBuffer(entry.directBlasScratchBuffer);
		services.RetireBuffer(entry.uploadMeshResource.vertexBuffer);
		services.RetireBuffer(entry.uploadMeshResource.indexBuffer);
		services.RetireAccelerationStructure(entry.uploadMeshResource.accelerationStructure);
		entry.uploadMeshResource = {};
		entry.uploadMaterialResource = {};
		entry.uploadGeometry = {};
		entry.uploadGpuIndices.clear();
		entry.uploadGpuPrimitives.clear();
		entry.uploadPrepared = false;
		entry.vertexBytesUploaded = 0;
		entry.vertexArenaBytesUploaded = 0;
		entry.indexBytesUploaded = 0;
		entry.indexArenaBytesUploaded = 0;
		entry.primitiveBytesUploaded = 0;
		entry.uploadSubmittedBeforeBlas = false;
		entry.uploadGeometryFromCompute = false;
		entry.computeGeometryJobId = 0;
		if (entry.directComputeRequested)
		{
			CancelNRIVoxelComputeDirectPublication(meshResourceKey, entry.directComputeGeneration);
		}
		entry.directComputeRequested = false;
		entry.directComputeFailed = false;
		entry.directComputeGeneration = 0;
		entry.directComputeJobId = 0;
		entry.directComputeRequestFrame = UINT32_MAX;
		entry.directComputeOutputKind = NRIVoxelComputeDirectPublishOutputKind::None;
		entry.directComputeFailure = NRIVoxelComputeDirectPublishFailure::None;
		entry.directBlasFenceValue = 0;
		entry.directBlasRecordedFrame = UINT32_MAX;
		entry.directBlasExclusive = false;
	};

	auto rollbackAdmission = [&](const char* reason, const char* step) -> bool
	{
		outFailureReason = reason != nullptr ? reason : "failed";
		const bool submittedDirectReservation = entry.directComputeRequested;
		const uint32_t abandonedVertexCapacity = entry.uploadMeshResource.vertexCapacity;
		const uint32_t abandonedIndexCapacity = entry.uploadMeshResource.indexCapacity;
		const uint32_t abandonedPrimitiveCapacity = entry.uploadMeshResource.primitiveCapacity;
		if (!submittedDirectReservation)
		{
			arenaVertexCursor = entry.savedVertexCursor;
			arenaIndexCursor = entry.savedIndexCursor;
			arenaPrimitiveCursor = entry.savedPrimitiveCursor;
		}
		cleanupPendingUpload();
		if (loadingTraceLevel >= 1 || voxelStatsEnabled)
		{
			Printf("NRI PT voxel admission transaction: event=%s reason=%s tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu step=%s vertex_capacity=%u index_capacity=%u primitive_capacity=%u\n",
				submittedDirectReservation ? "abandon-submitted-reservation" : "rollback",
				outFailureReason,
				variant.sourcePicnum,
				variant.resolvedVoxelIndex,
				(unsigned long long)variant.meshKeyHash,
				(unsigned long long)variant.materialKeyHash,
				variant.primitiveCount,
				(unsigned long long)entry.estimatedBytes,
				step != nullptr ? step : "unknown",
				abandonedVertexCapacity,
				abandonedIndexCapacity,
				abandonedPrimitiveCapacity);
		}
		return false;
	};

	auto allocateArenaSlice = [](uint32_t count, uint32_t& cursor, uint32_t& offset, uint32_t& capacity) -> bool
	{
		if (capacity >= count && count > 0)
		{
			return false;
		}
		offset = cursor;
		capacity = std::max<uint32_t>(count, capacity > 0 ? capacity * 2u : count);
		cursor += capacity;
		return true;
	};
	auto traceChunk = [&](const char* stream, uint64_t offset, uint64_t bytes, uint64_t total)
	{
		if (loadingTraceLevel >= 2 || voxelStatsEnabled)
		{
			Printf("NRI PT voxel admission entry: event=upload-chunk stream=%s offset=%llu bytes=%llu total=%llu tex=%d voxel=%d mesh_variant=0x%llx\n",
				stream != nullptr ? stream : "unknown",
				(unsigned long long)offset,
				(unsigned long long)bytes,
				(unsigned long long)total,
				variant.sourcePicnum,
				variant.resolvedVoxelIndex,
				(unsigned long long)variant.meshKeyHash);
		}
	};
	auto existingMeshResourceReady = [&]() -> bool
	{
		auto existingMeshIt = meshVariantResources.find(meshResourceKey);
		const bool existingMeshPresent =
			existingMeshIt != meshVariantResources.end() &&
			existingMeshIt->second.resourceKey == meshResourceKey &&
			existingMeshIt->second.vertexCount != 0 &&
			existingMeshIt->second.indexCount != 0 &&
			existingMeshIt->second.primitiveCount != 0 &&
			existingMeshIt->second.accelerationStructure.accelerationStructure != nullptr &&
			vertexBuffer.buffer != nullptr &&
			indexBuffer.buffer != nullptr &&
			primitiveBuffer.buffer != nullptr;
		const bool existingPrivateBuffersReady =
			existingMeshPresent &&
			existingMeshIt->second.vertexBuffer.buffer != nullptr &&
			existingMeshIt->second.indexBuffer.buffer != nullptr;
		const bool existingDirectBuffersReady =
			existingMeshPresent &&
			existingMeshIt->second.directComputePublished;
		return
			existingPrivateBuffersReady ||
			existingDirectBuffersReady;
	};
	auto directPublishOwnerPending = [&]() -> bool
	{
		const uint64_t ownerToken = admissionScheduler.FindMeshOwner({ residencyMapGeneration, meshResourceKey });
		if (ownerToken != 0)
		{
			NRIVoxelAdmissionTokenSnapshot owner = {};
			return entry.schedulerTokenId == 0 ||
				ownerToken != entry.schedulerTokenId ||
				(admissionScheduler.GetTokenSnapshot(ownerToken, owner) &&
				 !owner.bindings.empty() && owner.bindings.front().pairKey != entry.pairKey);
		}
		return false;
	};
	auto directBlasLaneBlocked = [&](bool exclusive) -> bool
	{
		const NRIVoxelAdmissionSnapshot scheduler = admissionScheduler.GetSnapshot();
		if (exclusive && scheduler.blasInFlight != 0)
		{
			return true;
		}
		for (const NRIVoxelAdmissionTokenSnapshot& token : admissionScheduler.GetTokenSnapshots())
		{
			if (exclusive && token.tokenId != entry.schedulerTokenId &&
				token.stage == NRIVoxelAdmissionStage::ComputeSubmitted &&
				token.oversizedExclusive && token.tokenId < entry.schedulerTokenId)
			{
				return true;
			}
			if (token.tokenId != entry.schedulerTokenId &&
				token.stage == NRIVoxelAdmissionStage::BlasSubmitted &&
				token.oversizedExclusive)
			{
				return true;
			}
		}
		return false;
	};
	if (entry.schedulerTokenId == 0 &&
		!existingMeshResourceReady() &&
		(entry.runtimeRequested || variant.directOnlyAdmission) &&
		ShouldDirectPublishNRIVoxelComputeMeshing() &&
		!entry.directComputeFailed &&
		variant.model != nullptr &&
		variant.primitiveCount != 0)
	{
		const NRIVoxelAdmissionSnapshot schedulerSnapshot = admissionScheduler.GetSnapshot();
		const uint32_t activeRuntimeJobs = schedulerSnapshot.activeFairnessCounts[
			(size_t)NRIVoxelAdmissionFairnessClass::VisibleNoFallback];
		if (entry.runtimeRequested && activeRuntimeJobs >= runtimeComputeMaxJobs)
		{
			outInProgress = true;
			entry.state = PersistentVoxelAdmissionState::Deferred;
			entry.lastReason = "runtime-compute-capacity";
			return true;
		}
		NRIVoxelAdmissionRequest schedulerRequest = {};
		schedulerRequest.mesh = { residencyMapGeneration, meshResourceKey };
		schedulerRequest.pairKey = entry.pairKey;
		schedulerRequest.materialKey = variant.materialKeyHash;
		schedulerRequest.reservation.vertexCapacity = std::max(1u, variant.primitiveCount * 2u);
		schedulerRequest.reservation.indexCapacity = std::max(1u, variant.primitiveCount * 3u);
		schedulerRequest.reservation.primitiveCapacity = variant.primitiveCount;
		schedulerRequest.reservation.materialBindingCapacity = 256;
		schedulerRequest.reservation.vertexBytes =
			(uint64_t)schedulerRequest.reservation.vertexCapacity * sizeof(nri_scene::SceneVertex);
		schedulerRequest.reservation.indexBytes =
			(uint64_t)schedulerRequest.reservation.indexCapacity * sizeof(uint32_t);
		schedulerRequest.reservation.primitiveBytes =
			(uint64_t)variant.primitiveCount * sizeof(nri_scene::PrimitiveData);
		schedulerRequest.reservation.materialBytes =
			(uint64_t)schedulerRequest.reservation.materialBindingCapacity * sizeof(nri_scene::MaterialData);
		schedulerRequest.reservation.blasBytes = (uint64_t)variant.primitiveCount * 64ull;
		schedulerRequest.reservation.blasScratchBytes = (uint64_t)variant.primitiveCount * 12ull;
		schedulerRequest.fairnessClass = entry.runtimeRequested ?
			NRIVoxelAdmissionFairnessClass::VisibleNoFallback :
			(IsRequiredAdmission(entry) ? NRIVoxelAdmissionFairnessClass::RequiredLoading : NRIVoxelAdmissionFairnessClass::OptionalLoading);
		schedulerRequest.age = entry.firstQueuedFrame != UINT32_MAX && frameIndex >= entry.firstQueuedFrame ?
			(uint64_t)(frameIndex - entry.firstQueuedFrame) : 0u;
		schedulerRequest.dependenciesReady = true;
		schedulerRequest.forceExclusive = isolateBlasBuild;
		const NRIVoxelAdmissionResult schedulerResult = admissionScheduler.Admit(schedulerRequest);
		if (schedulerResult.code != NRIVoxelAdmissionResultCode::Accepted &&
			schedulerResult.code != NRIVoxelAdmissionResultCode::BindingAttached &&
			schedulerResult.code != NRIVoxelAdmissionResultCode::DuplicateBinding)
		{
			outInProgress = true;
			entry.state = PersistentVoxelAdmissionState::Deferred;
			entry.lastReason = "scheduler-capacity";
			return true;
		}
		entry.schedulerTokenId = schedulerResult.tokenId;
	}
	auto finalizeDirectBlas = [&]() -> bool
	{
		PersistentVoxelMeshVariantResource& meshResource = entry.uploadMeshResource;
		meshResource.directComputeBlasFrame = frameIndex;
		meshResource.directComputePublishedFrame = frameIndex;
		meshResource.directComputePublished = true;
		meshResource.residentBytes = meshResource.accelerationStructure.memorySize;
		auto existingMeshIt = meshVariantResources.find(meshResourceKey);
		if (existingMeshIt != meshVariantResources.end())
		{
			services.RetireBuffer(existingMeshIt->second.vertexBuffer);
			services.RetireBuffer(existingMeshIt->second.indexBuffer);
			services.RetireAccelerationStructure(existingMeshIt->second.accelerationStructure);
			RetirePersistentVoxelShadowProxy(existingMeshIt->second.shadowProxy, services);
		}
		entry.uploadMaterialResource.lastUsedFrame = frameIndex;
		entry.uploadMaterialResource.sourceBits |= entry.sourceBits;
		entry.uploadMaterialResource.priority = entry.priority;
		entry.uploadMaterialResource.gpuForce = entry.uploadMaterialResource.gpuForce || entry.gpuForce;
		entry.uploadMaterialResource.gpuPrefer = entry.uploadMaterialResource.gpuPrefer || entry.gpuPrefer;
		entry.uploadMaterialResource.residentBytes =
			(uint64_t)entry.uploadMaterialResource.materialBridge.materials.size() * (uint64_t)sizeof(nri_scene::MaterialData);
		bool canonicalMaterialReused = false;
		if (!PublishCanonicalMaterialResource(entry.uploadMaterialResource, canonicalMaterialReused))
		{
			return rollbackAdmission("material-publish-failed", "direct_blas_publish");
		}
		if (entry.schedulerTokenId == 0 ||
			!admissionScheduler.MarkBlasReady(entry.schedulerTokenId) ||
			!admissionScheduler.MarkPublished(entry.schedulerTokenId))
		{
			return rollbackAdmission("scheduler-publication-transition", "direct_blas_publish");
		}
		meshVariantResources[meshResourceKey] = std::move(entry.uploadMeshResource);
		publishedMeshKeys.insert(meshResourceKey);
		if (loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
		{
			const PersistentVoxelMeshVariantResource& published = meshVariantResources[meshResourceKey];
			const uint32_t readyFrame = published.directComputeReadyFrame != 0 ? published.directComputeReadyFrame : frameIndex;
			const uint32_t requestToBlas =
				entry.directComputeRequestFrame != UINT32_MAX && frameIndex >= entry.directComputeRequestFrame ?
				frameIndex - entry.directComputeRequestFrame : 0u;
			const uint32_t readyToBlas = frameIndex >= readyFrame ? frameIndex - readyFrame : 0u;
			const uint32_t submitToReady =
				entry.directBlasRecordedFrame != UINT32_MAX && frameIndex >= entry.directBlasRecordedFrame ?
				frameIndex - entry.directBlasRecordedFrame : 0u;
			Printf("PERF pt voxel compute direct blas NRI: action=ready tex=%d voxel=%d mesh_variant=0x%llx generation=%llu job=%u vertex_offset=%u vertices=%u index_offset=%u indices=%u primitive_offset=%u primitives=%u as_bytes=%llu scratch_bytes=%llu source_serial=%llu request_frame=%u ready_frame=%u blas_frame=%u request_to_blas=%u ready_to_blas=%u submit_to_ready=%u fence=%llu runtime_tail=%u tlas_ready=0\n",
				variant.sourcePicnum,
				variant.resolvedVoxelIndex,
				(unsigned long long)variant.meshKeyHash,
				(unsigned long long)published.directComputeGeneration,
				published.directComputeJobId,
				published.vertexOffset,
				published.vertexCount,
				published.indexOffset,
				published.indexCount,
				published.primitiveOffset,
				published.primitiveCount,
				(unsigned long long)published.accelerationStructure.memorySize,
				(unsigned long long)published.accelerationStructure.buildScratchSize,
				(unsigned long long)published.directComputeSourceArchiveSerial,
				entry.directComputeRequestFrame,
				readyFrame,
				frameIndex,
				requestToBlas,
				readyToBlas,
				submitToReady,
				(unsigned long long)entry.directBlasFenceValue,
				runtimeTailRequest ? 1u : 0u);
		}
		if (outStats != nullptr && entry.directComputeRequestFrame != UINT32_MAX)
		{
			const uint32_t blasLatency = frameIndex >= entry.directComputeRequestFrame ? frameIndex - entry.directComputeRequestFrame : 0u;
			outStats->directBlasLatencySamples++;
			outStats->totalDirectBlasLatencyFrames += blasLatency;
			outStats->maxDirectBlasLatencyFrames = std::max(outStats->maxDirectBlasLatencyFrames, blasLatency);
		}
		if (outStats != nullptr)
		{
			outStats->cpuGeometryAvoided++;
		}
		services.RetireBuffer(entry.directBlasScratchBuffer);
		entry.uploadMeshResource = {};
		entry.uploadMaterialResource = {};
		entry.uploadGeometry = {};
		entry.uploadGpuIndices.clear();
		entry.uploadGpuPrimitives.clear();
		entry.uploadPrepared = false;
		entry.uploadGeometryFromCompute = false;
		entry.computeGeometryFailed = false;
		entry.computeGeometryJobId = 0;
		entry.vertexBytesUploaded = 0;
		entry.vertexArenaBytesUploaded = 0;
		entry.indexBytesUploaded = 0;
		entry.indexArenaBytesUploaded = 0;
		entry.primitiveBytesUploaded = 0;
		entry.state = PersistentVoxelAdmissionState::Ready;
		entry.directComputeRequested = false;
		entry.directComputeFailed = false;
		entry.directComputeGeneration = 0;
		entry.directComputeJobId = 0;
		entry.directComputeRequestFrame = UINT32_MAX;
		entry.directComputeOutputKind = NRIVoxelComputeDirectPublishOutputKind::None;
		entry.directComputeFailure = NRIVoxelComputeDirectPublishFailure::None;
		entry.directBlasFenceValue = 0;
		entry.directBlasRecordedFrame = UINT32_MAX;
		entry.directBlasExclusive = false;
		entry.lastReason = "direct-blas-ready";
		return true;
	};

	if (entry.state == PersistentVoxelAdmissionState::DirectBlasPending)
	{
		if (entry.directBlasFenceValue == 0)
		{
			return rollbackAdmission("direct-blas-missing-fence", "direct_blas_poll");
		}
		if (!services.IsCommandFenceValueComplete(entry.directBlasFenceValue))
		{
			if (outStats != nullptr)
			{
				outStats->directPending++;
			}
			outInProgress = true;
			entry.lastReason = "direct-blas-fence-pending";
			return true;
		}
		return finalizeDirectBlas();
	}

	if (entry.state == PersistentVoxelAdmissionState::DirectComputePending)
	{
		NRIVoxelComputeDirectPublishedMesh directMesh = {};
		if (blasBudget == 0)
		{
			outInProgress = true;
			entry.lastReason = "direct-blas-budget";
			return true;
		}
		if (directPublishOwnerPending())
		{
			outInProgress = true;
			entry.lastReason = "direct-blas-shared-owner";
			return true;
		}
		if (directBlasLaneBlocked(isolateBlasBuild))
		{
			outInProgress = true;
			entry.lastReason = isolateBlasBuild ? "direct-blas-exclusive-lane" : "direct-blas-exclusive-owner";
			return true;
		}
		if (TakeNRIVoxelComputeDirectPublication(meshResourceKey, entry.directComputeGeneration, directMesh))
		{
			if (outStats != nullptr)
			{
				outStats->directReady++;
				if (entry.directComputeRequestFrame != UINT32_MAX)
				{
					const uint32_t readyFrame = directMesh.readyFrame != 0 ? directMesh.readyFrame : frameIndex;
					const uint32_t readyLatency =
						readyFrame >= entry.directComputeRequestFrame ?
						readyFrame - entry.directComputeRequestFrame :
						frameIndex - entry.directComputeRequestFrame;
					outStats->directReadyLatencySamples++;
					outStats->totalDirectReadyLatencyFrames += readyLatency;
					outStats->maxDirectReadyLatencyFrames = std::max(outStats->maxDirectReadyLatencyFrames, readyLatency);
				}
			}
			if (loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
			{
				const uint32_t readyFrame = directMesh.readyFrame != 0 ? directMesh.readyFrame : frameIndex;
				const uint32_t requestToReady =
					entry.directComputeRequestFrame != UINT32_MAX && readyFrame >= entry.directComputeRequestFrame ?
					readyFrame - entry.directComputeRequestFrame : 0u;
				Printf("PERF pt voxel compute direct publish NRI: action=ready tex=%d voxel=%d mesh_variant=0x%llx generation=%llu job=%u vertices=%u indices=%u primitives=%u material_base=%u material_count=%u request_frame=%u ready_frame=%u request_to_ready=%u runtime_tail=%u\n",
					variant.sourcePicnum,
					variant.resolvedVoxelIndex,
					(unsigned long long)variant.meshKeyHash,
					(unsigned long long)directMesh.generation,
					directMesh.jobId,
					directMesh.vertices.count,
					directMesh.indices.count,
					directMesh.primitives.count,
					directMesh.materialBase,
					directMesh.materialCount,
					entry.directComputeRequestFrame,
					readyFrame,
					requestToReady,
					runtimeTailRequest ? 1u : 0u);
			}
			PersistentVoxelMeshVariantResource& meshResource = entry.uploadMeshResource;
			const bool directRangeValid =
				directMesh.vertices.count != 0 &&
				directMesh.indices.count != 0 &&
				directMesh.primitives.count != 0 &&
				vertexBuffer.buffer != nullptr &&
				indexBuffer.buffer != nullptr &&
				primitiveBuffer.buffer != nullptr;
			if (!directRangeValid)
			{
				if (outStats != nullptr)
				{
					outStats->directFailures++;
				}
				if (loadingTraceLevel >= 1 || voxelStatsEnabled)
				{
					Printf("NRI PT voxel admission transaction: event=abandon-submitted-reservation reason=direct-publish-invalid-range tex=%d voxel=%d mesh_variant=0x%llx vertex_capacity=%u index_capacity=%u primitive_capacity=%u\n",
						variant.sourcePicnum,
						variant.resolvedVoxelIndex,
						(unsigned long long)variant.meshKeyHash,
						entry.uploadMeshResource.vertexCapacity,
						entry.uploadMeshResource.indexCapacity,
						entry.uploadMeshResource.primitiveCapacity);
				}
				entry.uploadMeshResource = {};
				entry.directComputeRequested = false;
				entry.directComputeFailed = true;
				entry.directComputeRequestFrame = UINT32_MAX;
				entry.directComputeFailure = NRIVoxelComputeDirectPublishFailure::StatusFailed;
				entry.state = PersistentVoxelAdmissionState::Pending;
				entry.lastReason = "direct-publish-invalid-range";
			}
			else
			{
				meshResource.resourceKey = meshResourceKey;
				meshResource.sourceModel = variant.model;
				meshResource.shadowProxyPrimitiveSemanticsCertified = true; // VoxelComputeEmit contract: flags=0, portal=invalid.
				meshResource.meshKeyHash = variant.meshKeyHash;
				meshResource.geometrySignature = ResolvePersistentVoxelVariantGeometrySignature(variant);
				meshResource.geometryContentHash = variant.geometryContentHash;
				meshResource.renderPrimitiveHash = variant.renderPrimitiveHash;
				meshResource.transformBasisSignature = 0;
				meshResource.meshBakeSpace = nri_scene::VoxelMeshBakeSpace::LocalSpace;
				meshResource.vertexOffset = directMesh.vertices.offset;
				meshResource.vertexCapacity = directMesh.vertices.count;
				meshResource.indexOffset = directMesh.indices.offset;
				meshResource.indexCapacity = directMesh.indices.count;
				meshResource.primitiveOffset = directMesh.primitives.offset;
				meshResource.primitiveCapacity = directMesh.primitives.count;
				meshResource.vertexCount = directMesh.vertices.count;
				meshResource.indexCount = directMesh.indices.count;
				meshResource.primitiveCount = directMesh.primitives.count;
				FillPersistentVoxelMeshBounds(directMesh.bounds, meshResource);
				meshResource.surfaceArea = directMesh.surfaceArea;
				meshResource.bakedTranslation[0] = 0.0f;
				meshResource.bakedTranslation[1] = 0.0f;
				meshResource.bakedTranslation[2] = 0.0f;
				meshResource.tlasReadyFrame = 0;
				meshResource.tlasPublished = false;
				meshResource.lastDesiredMapGeneration = residencyMapGeneration;
				meshResource.lastUsedMapGeneration = residencyMapGeneration;
				meshResource.lastUsedFrame = frameIndex;
				meshResource.directComputeGeneration = directMesh.generation;
				meshResource.directComputeSourceArchiveSerial = directMesh.sourceArchiveSerial;
				meshResource.directComputeJobId = directMesh.jobId;
				meshResource.directComputeRequestFrame = entry.directComputeRequestFrame;
				meshResource.directComputeReadyFrame = directMesh.readyFrame;
				meshResource.directComputeBlasFrame = UINT32_MAX;
				meshResource.directComputePublishedFrame = UINT32_MAX;
				meshResource.directComputePublished = false;
				meshResource.directComputeOutputKind = directMesh.outputKind;
				meshResource.directComputeFailure = NRIVoxelComputeDirectPublishFailure::None;
				meshResource.sourceBits |= entry.sourceBits;
				meshResource.priority = entry.priority;
				meshResource.gpuForce = meshResource.gpuForce || entry.gpuForce;
				meshResource.gpuPrefer = meshResource.gpuPrefer || entry.gpuPrefer;
				meshResource.cold = false;
				if (entry.schedulerTokenId != 0 &&
					(!admissionScheduler.MarkComputeReady(entry.schedulerTokenId) ||
					 !admissionScheduler.SubmitBlas(entry.schedulerTokenId)))
				{
					return rollbackAdmission("scheduler-blas-lane-unavailable", "direct_blas_schedule");
				}
				if (!services.BarrierComputeToBuildInputs(vertexBuffer, indexBuffer))
				{
					return rollbackAdmission("direct-blas-input-barrier-failed", "direct_blas");
				}
				if (!services.BuildBottomLevel(
					vertexBuffer,
					indexBuffer,
					meshResource.vertexOffset,
					meshResource.vertexCount,
					meshResource.indexOffset,
					meshResource.indexCount,
					meshResource.primitiveCount,
					meshResource.accelerationStructure,
					entry.runtimeRequested ? &entry.directBlasScratchBuffer : nullptr))
				{
					return rollbackAdmission("direct-blas-build-failed", "direct_blas");
				}
				if (!services.BarrierBuildInputs(vertexBuffer, indexBuffer))
				{
					return rollbackAdmission("direct-blas-input-post-barrier-failed", "direct_blas");
				}
				blasBudget--;
				if (entry.runtimeRequested)
				{
					const uint32_t readyFrame = directMesh.readyFrame != 0 ? directMesh.readyFrame : frameIndex;
					entry.directBlasFenceValue = services.GetRecordingCommandFenceValue();
					if (entry.directBlasFenceValue == 0)
					{
						return rollbackAdmission("direct-blas-fence-unavailable", "direct_blas_submit");
					}
					entry.directBlasRecordedFrame = frameIndex;
					entry.directBlasExclusive = isolateBlasBuild;
					entry.state = PersistentVoxelAdmissionState::DirectBlasPending;
					if (loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
					{
						Printf("PERF pt voxel compute direct blas NRI: action=submit tex=%d voxel=%d mesh_variant=0x%llx generation=%llu job=%u primitives=%u as_bytes=%llu scratch_bytes=%llu request_frame=%u ready_frame=%u submit_frame=%u fence=%llu exclusive=%u runtime_tail=%u\n",
							variant.sourcePicnum,
							variant.resolvedVoxelIndex,
							(unsigned long long)variant.meshKeyHash,
							(unsigned long long)directMesh.generation,
							directMesh.jobId,
							directMesh.primitives.count,
							(unsigned long long)meshResource.accelerationStructure.memorySize,
							(unsigned long long)entry.directBlasScratchBuffer.memorySize,
							entry.directComputeRequestFrame,
							readyFrame,
							frameIndex,
							(unsigned long long)entry.directBlasFenceValue,
							entry.directBlasExclusive ? 1u : 0u,
							runtimeTailRequest ? 1u : 0u);
					}
					outInProgress = true;
					entry.lastReason = "direct-blas-fence-pending";
					return true;
				}
				return finalizeDirectBlas();
			}
		}
		else if (directMesh.status == NRIVoxelComputeGeneratedGeometryStatus::Failed)
		{
			if (outStats != nullptr)
			{
				outStats->directFailures++;
			}
			if (loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
			{
				Printf("NRI PT voxel admission transaction: event=abandon-submitted-reservation reason=direct-publish-failed tex=%d voxel=%d mesh_variant=0x%llx vertex_capacity=%u index_capacity=%u primitive_capacity=%u\n",
					variant.sourcePicnum,
					variant.resolvedVoxelIndex,
					(unsigned long long)variant.meshKeyHash,
					entry.uploadMeshResource.vertexCapacity,
					entry.uploadMeshResource.indexCapacity,
					entry.uploadMeshResource.primitiveCapacity);
			}
			entry.uploadMeshResource = {};
			entry.directComputeRequested = false;
			entry.directComputeFailed = true;
			if (entry.schedulerTokenId != 0)
			{
				admissionScheduler.Fail(entry.schedulerTokenId);
				entry.schedulerTokenId = 0;
			}
			entry.directComputeRequestFrame = UINT32_MAX;
			entry.directComputeFailure = NRIVoxelComputeDirectPublishFailure::StatusFailed;
			entry.state = PersistentVoxelAdmissionState::Pending;
			entry.lastReason = "direct-publish-failed";
		}
		else
		{
			if (outStats != nullptr)
			{
				outStats->directPending++;
			}
			outInProgress = true;
			entry.lastReason = "direct-publish-pending";
			return true;
		}
	}

	if (!entry.uploadPrepared &&
		!entry.uploadGeometryFromCompute &&
		!entry.computeGeometryFailed &&
		!existingMeshResourceReady() &&
		entry.runtimeRequested &&
		ShouldConsumeNRIVoxelComputeMeshing() &&
		!ShouldDirectPublishNRIVoxelComputeMeshing() &&
		variant.model != nullptr)
	{
		uint32_t computeJobId = 0;
		if (TakeNRIVoxelComputeGeneratedGeometry(meshResourceKey, entry.uploadGeometry, &computeJobId))
		{
			entry.uploadGeometryFromCompute = true;
			entry.computeGeometryJobId = computeJobId;
			if (loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
			{
				Printf("PERF pt voxel compute consume NRI: action=import tex=%d voxel=%d mesh_variant=0x%llx job=%u vertices=%u indices=%u primitives=%u\n",
					variant.sourcePicnum,
					variant.resolvedVoxelIndex,
					(unsigned long long)variant.meshKeyHash,
					computeJobId,
					(uint32_t)entry.uploadGeometry.vertices.size(),
					(uint32_t)entry.uploadGeometry.indices.size(),
					(uint32_t)entry.uploadGeometry.primitives.size());
			}
		}
		else
		{
			const NRIVoxelComputeGeneratedGeometryStatus computeStatus =
				RequestNRIVoxelComputeGeneratedGeometry(meshResourceKey, variant.model);
			if (computeStatus == NRIVoxelComputeGeneratedGeometryStatus::Queued)
			{
				outInProgress = true;
				entry.lastReason = "compute-geometry-pending";
				if (loadingTraceLevel >= 2 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
				{
					Printf("PERF pt voxel compute consume NRI: action=defer reason=pending tex=%d voxel=%d mesh_variant=0x%llx\n",
						variant.sourcePicnum,
						variant.resolvedVoxelIndex,
						(unsigned long long)variant.meshKeyHash);
				}
				return true;
			}
			if (computeStatus == NRIVoxelComputeGeneratedGeometryStatus::Failed)
			{
				entry.computeGeometryFailed = true;
				if (loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
				{
					Printf("PERF pt voxel compute consume NRI: action=fallback reason=compute-failed tex=%d voxel=%d mesh_variant=0x%llx\n",
						variant.sourcePicnum,
						variant.resolvedVoxelIndex,
						(unsigned long long)variant.meshKeyHash);
				}
			}
			else
			{
				entry.computeGeometryFailed = true;
				if (loadingTraceLevel >= 2 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
				{
					Printf("PERF pt voxel compute consume NRI: action=fallback reason=unavailable tex=%d voxel=%d mesh_variant=0x%llx\n",
						variant.sourcePicnum,
						variant.resolvedVoxelIndex,
						(unsigned long long)variant.meshKeyHash);
				}
			}
		}
	}

	if (!entry.uploadPrepared)
	{
		entry.savedVertexCursor = arenaVertexCursor;
		entry.savedIndexCursor = arenaIndexCursor;
		entry.savedPrimitiveCursor = arenaPrimitiveCursor;
		entry.vertexBytesUploaded = 0;
		entry.vertexArenaBytesUploaded = 0;
		entry.indexBytesUploaded = 0;
		entry.indexArenaBytesUploaded = 0;
		entry.primitiveBytesUploaded = 0;
		entry.uploadSubmittedBeforeBlas = false;
		if (loadingTraceLevel >= 2 || voxelStatsEnabled)
		{
			Printf("NRI PT voxel admission transaction: event=begin reason=none tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu step=prepare\n",
				variant.sourcePicnum,
				variant.resolvedVoxelIndex,
				(unsigned long long)variant.meshKeyHash,
				(unsigned long long)variant.materialKeyHash,
				variant.primitiveCount,
				(unsigned long long)entry.estimatedBytes);
		}

		nri_scene::SurfaceRef surface =
			(directOnlyAdmission && variant.surface == nullptr) ? variant.materialSurface : *variant.surface;
		surface.material = variant.material;
		nri_scene::SceneView variantSceneView = {};
		variantSceneView.opaqueSprites.push_back(std::move(surface));
		variantSceneView.stats.spriteDrawItems = 1;
		variantSceneView.stats.modelDrawItems = 1;
		variantSceneView.stats.voxelProxyDrawItems = 1;
		variantSceneView.stats.totalDrawItems = 1;
		variantSceneView.stats.materialRefs = 1;
		variantSceneView.stats.triangleEstimate = variant.primitiveCount;
		variantSceneView.stats.voxelCachePrimitives = variant.primitiveCount;

		const auto existingMaterialIt = materialVariantResources.find(variant.materialKeyHash);
		entry.uploadMaterialResource =
			existingMaterialIt != materialVariantResources.end() ? existingMaterialIt->second : PersistentVoxelMaterialVariantResource{};
		const uint64_t validatedMaterialSignature =
			variant.materialVariantHash != 0 ? variant.materialVariantHash : variant.materialKeyHash;
		const bool materialReady =
			entry.uploadMaterialResource.materialKeyHash == variant.materialKeyHash &&
			entry.uploadMaterialResource.materialSignature == validatedMaterialSignature &&
			entry.uploadMaterialResource.materialBridgeBuildSerial == residencyLastBuildSerial &&
			entry.uploadMaterialResource.materialCount != 0 &&
			entry.uploadMaterialResource.materialSlotGeneration != 0 &&
			!entry.uploadMaterialResource.materialBridge.materials.empty();
		if (materialReady)
		{
			if (outStats != nullptr)
			{
				outStats->materialPayloadReuses++;
			}
			if (entry.uploadMaterialResource.materialPayloadHash == 0)
			{
				entry.uploadMaterialResource.materialPayloadHash =
					HashPersistentVoxelMaterialPayloadData(entry.uploadMaterialResource.materialBridge);
			}
			outReusedMaterial = true;
		}
		else
		{
			nri_scene::MaterialBridgeData builtMaterials;
			NRIPersistentVoxelMaterialClosureResult closure = {};
			const bool reusedClosure = services.materialClosure.TryReuse(
				residencyLastBuildSerial,
				variant.materialKeyHash,
				validatedMaterialSignature,
				NRIPersistentVoxelMaterialClosureSource::RuntimeUnknown,
				builtMaterials,
				closure);
			if (!reusedClosure)
			{
				if (closure.state == NRIPersistentVoxelMaterialClosureState::Pending)
				{
					outInProgress = true;
					entry.lastReason = "material-closure-pending";
					return true;
				}
				services.BuildMaterials(variantSceneView, builtMaterials, "persistent_voxel_admission_variant");
				if (builtMaterials.materials.empty())
				{
					return rollbackAdmission("material-build-failed", "materials");
				}
				if (!services.materialClosure.Register(
						residencyLastBuildSerial,
						variant.materialKeyHash,
						validatedMaterialSignature,
						builtMaterials,
						NRIPersistentVoxelMaterialClosureSource::RuntimeUnknown,
						closure))
				{
					if (closure.state == NRIPersistentVoxelMaterialClosureState::Pending)
					{
						outInProgress = true;
						entry.lastReason = "material-closure-pending";
						return true;
					}
					return rollbackAdmission("material-build-failed", "materials");
				}
			}
			if (builtMaterials.materials.empty())
			{
				return rollbackAdmission("material-build-failed", "materials");
			}
			entry.uploadMaterialResource = {};
			entry.uploadMaterialResource.materialKeyHash = variant.materialKeyHash;
			entry.uploadMaterialResource.materialSignature = validatedMaterialSignature;
			entry.uploadMaterialResource.materialBridge = std::move(builtMaterials);
			entry.uploadMaterialResource.materialBridgeBuildSerial = residencyLastBuildSerial;
			entry.uploadMaterialResource.materialCount = (uint32_t)entry.uploadMaterialResource.materialBridge.materials.size();
			entry.uploadMaterialResource.materialPayloadHash =
				HashPersistentVoxelMaterialPayloadData(entry.uploadMaterialResource.materialBridge);
			entry.uploadMaterialResource.materialUploadHash = 0;
			if (outStats != nullptr)
			{
				if (reusedClosure)
				{
					outStats->materialPayloadReuses++;
				}
				else
				{
					outStats->materialPayloadBuilds++;
				}
				outStats->materialPayloadBytes +=
					(uint64_t)entry.uploadMaterialResource.materialBridge.materials.size() * (uint64_t)sizeof(nri_scene::MaterialData);
			}
		}
		entry.uploadMaterialResource.lastDesiredMapGeneration = residencyMapGeneration;
		entry.uploadMaterialResource.lastUsedMapGeneration = residencyMapGeneration;
		entry.uploadMaterialResource.lastUsedFrame = frameIndex;
		entry.uploadMaterialResource.sourceBits |= entry.sourceBits;
		entry.uploadMaterialResource.priority = entry.priority;
		entry.uploadMaterialResource.gpuForce = entry.uploadMaterialResource.gpuForce || entry.gpuForce;
		entry.uploadMaterialResource.gpuPrefer = entry.uploadMaterialResource.gpuPrefer || entry.gpuPrefer;
		entry.uploadMaterialResource.residentBytes =
			(uint64_t)entry.uploadMaterialResource.materialBridge.materials.size() * (uint64_t)sizeof(nri_scene::MaterialData);
		entry.uploadMaterialResource.cold = false;
		bool canonicalMaterialReused = false;
		if (!PublishCanonicalMaterialResource(entry.uploadMaterialResource, canonicalMaterialReused))
		{
			return rollbackAdmission("material-publish-failed", "materials");
		}
		outReusedMaterial = outReusedMaterial || canonicalMaterialReused;

		auto existingMeshIt = meshVariantResources.find(meshResourceKey);
		const bool existingMeshPresent =
			existingMeshIt != meshVariantResources.end() &&
			existingMeshIt->second.resourceKey == meshResourceKey &&
			existingMeshIt->second.vertexCount != 0 &&
			existingMeshIt->second.indexCount != 0 &&
			existingMeshIt->second.primitiveCount != 0 &&
			existingMeshIt->second.accelerationStructure.accelerationStructure != nullptr &&
			vertexBuffer.buffer != nullptr &&
			indexBuffer.buffer != nullptr &&
			primitiveBuffer.buffer != nullptr;
		const bool existingMeshReady =
			existingMeshPresent &&
			((existingMeshIt->second.vertexBuffer.buffer != nullptr &&
				existingMeshIt->second.indexBuffer.buffer != nullptr) ||
			 existingMeshIt->second.directComputePublished);
		if (existingMeshReady)
		{
			existingMeshIt->second.geometrySignature = ResolvePersistentVoxelVariantGeometrySignature(variant);
			existingMeshIt->second.geometryContentHash = variant.geometryContentHash;
			existingMeshIt->second.renderPrimitiveHash = variant.renderPrimitiveHash;
			existingMeshIt->second.lastDesiredMapGeneration = residencyMapGeneration;
			existingMeshIt->second.lastUsedMapGeneration = residencyMapGeneration;
			existingMeshIt->second.lastUsedFrame = frameIndex;
			existingMeshIt->second.sourceBits |= entry.sourceBits;
			existingMeshIt->second.priority = entry.priority;
			existingMeshIt->second.gpuForce = existingMeshIt->second.gpuForce || entry.gpuForce;
			existingMeshIt->second.gpuPrefer = existingMeshIt->second.gpuPrefer || entry.gpuPrefer;
			existingMeshIt->second.cold = false;
			bool canonicalMaterialReused = false;
			if (!PublishCanonicalMaterialResource(entry.uploadMaterialResource, canonicalMaterialReused))
			{
				return rollbackAdmission("material-publish-failed", "publish");
			}
			publishedMeshKeys.insert(meshResourceKey);
			entry.uploadMaterialResource = {};
			outReusedMesh = true;
			entry.uploadPrepared = false;
			entry.uploadGeometryFromCompute = false;
			entry.computeGeometryFailed = false;
			entry.computeGeometryJobId = 0;
			entry.directComputeRequested = false;
			entry.directComputeFailed = false;
			entry.directComputeGeneration = 0;
			entry.directComputeJobId = 0;
			entry.directComputeRequestFrame = UINT32_MAX;
			entry.directComputeOutputKind = NRIVoxelComputeDirectPublishOutputKind::None;
			entry.directComputeFailure = NRIVoxelComputeDirectPublishFailure::None;
			if (loadingTraceLevel >= 2 || voxelStatsEnabled)
			{
				Printf("NRI PT voxel admission transaction: event=commit reason=already-resident tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=0 step=publish\n",
					variant.sourcePicnum,
					variant.resolvedVoxelIndex,
					(unsigned long long)variant.meshKeyHash,
					(unsigned long long)variant.materialKeyHash,
					variant.primitiveCount);
			}
			return true;
		}

		if ((entry.runtimeRequested || variant.directOnlyAdmission) &&
			ShouldDirectPublishNRIVoxelComputeMeshing() &&
			!entry.directComputeFailed &&
			variant.model != nullptr &&
			variant.primitiveCount != 0)
		{
			if (directPublishOwnerPending())
			{
				entry.state = PersistentVoxelAdmissionState::Deferred;
				entry.lastReason = "direct-publish-shared-pending";
				return true;
			}
			const uint32_t maxDirectJobs = computeMaxJobs;
			const uint32_t queuedDirectJobs = GetNRIVoxelComputeQueuedJobCount();
			if (maxDirectJobs != 0 && queuedDirectJobs >= maxDirectJobs)
			{
				entry.state = PersistentVoxelAdmissionState::Pending;
				entry.lastReason = "direct-publish-queue-full";
				outInProgress = true;
				if (outStats != nullptr)
				{
					outStats->directPending++;
				}
				if (loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
				{
					Printf("PERF pt voxel compute direct publish NRI: action=defer reason=queue_full_preflight tex=%d voxel=%d mesh_variant=0x%llx queued_jobs=%u max_jobs=%u\n",
						variant.sourcePicnum,
						variant.resolvedVoxelIndex,
						(unsigned long long)variant.meshKeyHash,
						queuedDirectJobs,
						maxDirectJobs);
				}
				return true;
			}
			PersistentVoxelMeshVariantResource& meshResource = entry.uploadMeshResource;
			const uint32_t directVertexCapacity = variant.primitiveCount * 2u;
			const uint32_t directIndexCapacity = variant.primitiveCount * 3u;
			const uint32_t directPrimitiveCapacity = variant.primitiveCount;
			allocateArenaSlice(directVertexCapacity, arenaVertexCursor, meshResource.vertexOffset, meshResource.vertexCapacity);
			allocateArenaSlice(directIndexCapacity, arenaIndexCursor, meshResource.indexOffset, meshResource.indexCapacity);
			allocateArenaSlice(directPrimitiveCapacity, arenaPrimitiveCursor, meshResource.primitiveOffset, meshResource.primitiveCapacity);
			if (!services.EnsureArenaBuffer(
					vertexBuffer,
					(uint64_t)arenaVertexCursor * sizeof(nri_scene::SceneVertex),
					sizeof(nri_scene::SceneVertex),
					NRIResourceFlags(NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE), nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
					PersistentVoxelComputeShaderResourceAccess()) ||
				!services.EnsureArenaBuffer(
					indexBuffer,
					(uint64_t)arenaIndexCursor * sizeof(uint32_t),
					sizeof(uint32_t),
					NRIResourceFlags(NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE), nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
					PersistentVoxelComputeShaderResourceAccess()) ||
				!services.EnsureArenaBuffer(
					primitiveBuffer,
					(uint64_t)arenaPrimitiveCursor * sizeof(nri_scene::PrimitiveData),
					sizeof(nri_scene::PrimitiveData),
					NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE),
					PersistentVoxelComputeShaderResourceAccess()))
			{
				arenaVertexCursor = entry.savedVertexCursor;
				arenaIndexCursor = entry.savedIndexCursor;
				arenaPrimitiveCursor = entry.savedPrimitiveCursor;
				entry.uploadMeshResource = {};
				entry.directComputeFailed = true;
				if (outStats != nullptr)
				{
					outStats->directFailures++;
				}
			}
			else
			{
				const uint64_t generation = ((uint64_t)residencyMapGeneration << 32) | (uint64_t)(entry.retryCount + 1u);
				NRIVoxelComputeDirectPublishRequest request = {};
				request.meshResourceKey = meshResourceKey;
				request.geometryKey = variant.meshKeyHash != 0 ? variant.meshKeyHash : meshResourceKey;
				request.materialBindingKey = variant.materialKeyHash;
				request.generation = generation != 0 ? generation : 1u;
				request.model = variant.model;
				request.outputKind = NRIVoxelComputeDirectPublishOutputKind::PrivateBlasInputsAndSharedArena;
				request.outputBuffers.vertices = &vertexBuffer;
				request.outputBuffers.indices = &indexBuffer;
				request.outputBuffers.primitives = &primitiveBuffer;
				request.vertices.offset = meshResource.vertexOffset;
				request.vertices.capacity = meshResource.vertexCapacity;
				request.indices.offset = meshResource.indexOffset;
				request.indices.capacity = meshResource.indexCapacity;
				request.primitives.offset = meshResource.primitiveOffset;
				request.primitives.capacity = meshResource.primitiveCapacity;
				request.materialBase = entry.uploadMaterialResource.materialOffset;
				request.materialCount = entry.uploadMaterialResource.materialCount;
				const uint32_t normalizedPriority =
					(uint32_t)((int64_t)entry.priority - (int64_t)INT32_MIN);
				request.priority = UINT32_MAX - normalizedPriority;
				request.age = entry.firstQueuedFrame != UINT32_MAX && frameIndex >= entry.firstQueuedFrame ?
					(uint64_t)(frameIndex - entry.firstQueuedFrame) : 0u;
				auto deferSchedulerCompute = [&]()
				{
					CancelNRIVoxelComputeDirectPublication(meshResourceKey, request.generation);
					arenaVertexCursor = entry.savedVertexCursor;
					arenaIndexCursor = entry.savedIndexCursor;
					arenaPrimitiveCursor = entry.savedPrimitiveCursor;
					entry.uploadMeshResource = {};
					entry.directComputeRequested = false;
					entry.directComputeFailed = false;
					entry.directComputeRequestFrame = UINT32_MAX;
					entry.directComputeFailure = NRIVoxelComputeDirectPublishFailure::QueueFull;
					if (entry.schedulerTokenId != 0)
					{
						admissionScheduler.Cancel(entry.schedulerTokenId);
						entry.schedulerTokenId = 0;
					}
					entry.state = PersistentVoxelAdmissionState::Pending;
					entry.lastReason = "scheduler-compute-lane";
					outInProgress = true;
					if (outStats != nullptr)
					{
						outStats->directPending++;
					}
				};
				const NRIVoxelComputeGeneratedGeometryStatus directStatus = RequestNRIVoxelComputeDirectPublication(request);
				if (directStatus == NRIVoxelComputeGeneratedGeometryStatus::Queued)
				{
					if (entry.schedulerTokenId == 0 || !admissionScheduler.SubmitCompute(entry.schedulerTokenId))
					{
						deferSchedulerCompute();
						return true;
					}
					if (outStats != nullptr)
					{
						outStats->directRequests++;
						outStats->directPending++;
					}
					entry.directComputeRequested = true;
					entry.directComputeFailed = false;
					entry.directComputeGeneration = request.generation;
					entry.directComputeRequestFrame = frameIndex;
					entry.directComputeOutputKind = request.outputKind;
					entry.directComputeFailure = NRIVoxelComputeDirectPublishFailure::None;
					entry.state = PersistentVoxelAdmissionState::DirectComputePending;
					entry.lastReason = "direct-publish-pending";
					outInProgress = true;
					if (loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
					{
						Printf("PERF pt voxel compute direct publish NRI: action=request tex=%d voxel=%d mesh_variant=0x%llx generation=%llu vertex_offset=%u vertex_capacity=%u index_offset=%u index_capacity=%u primitive_offset=%u primitive_capacity=%u material_base=%u material_count=%u request_frame=%u runtime_tail=%u\n",
							variant.sourcePicnum,
							variant.resolvedVoxelIndex,
							(unsigned long long)variant.meshKeyHash,
							(unsigned long long)request.generation,
							request.vertices.offset,
							request.vertices.capacity,
							request.indices.offset,
							request.indices.capacity,
							request.primitives.offset,
							request.primitives.capacity,
							request.materialBase,
							request.materialCount,
							frameIndex,
							runtimeTailRequest ? 1u : 0u);
						if (runtimeTailRequest)
						{
							Printf("PERF pt voxel runtime tail NRI: action=request build_serial=%llu frame=%u mesh_resource=0x%llx mesh_variant=0x%llx material=0x%llx probe=%u tex=%d voxel=%d primitives=%u\n",
								(unsigned long long)residencyLastBuildSerial,
								frameIndex,
								(unsigned long long)meshResourceKey,
								(unsigned long long)variant.meshKeyHash,
								(unsigned long long)variant.materialKeyHash,
								runtimeProbeRequest ? 1u : 0u,
								variant.sourcePicnum,
								variant.resolvedVoxelIndex,
								variant.primitiveCount);
						}
					}
					return true;
				}
				if (directStatus == NRIVoxelComputeGeneratedGeometryStatus::Ready)
				{
					if (entry.schedulerTokenId == 0 || !admissionScheduler.SubmitCompute(entry.schedulerTokenId))
					{
						deferSchedulerCompute();
						return true;
					}
					arenaVertexCursor = entry.savedVertexCursor;
					arenaIndexCursor = entry.savedIndexCursor;
					arenaPrimitiveCursor = entry.savedPrimitiveCursor;
					entry.uploadMeshResource = {};
					entry.directComputeRequested = true;
					entry.directComputeFailed = false;
					entry.directComputeGeneration = request.generation;
					entry.directComputeRequestFrame = frameIndex;
					entry.directComputeOutputKind = request.outputKind;
					entry.directComputeFailure = NRIVoxelComputeDirectPublishFailure::None;
					entry.state = PersistentVoxelAdmissionState::DirectComputePending;
					entry.lastReason = "direct-publish-ready";
					outInProgress = true;
					if (outStats != nullptr)
					{
						outStats->directPending++;
					}
					return true;
				}
				if (directStatus == NRIVoxelComputeGeneratedGeometryStatus::Unavailable &&
					std::max(0, (int)nri_ptvoxelcomputemaxjobs) > 0)
				{
					arenaVertexCursor = entry.savedVertexCursor;
					arenaIndexCursor = entry.savedIndexCursor;
					arenaPrimitiveCursor = entry.savedPrimitiveCursor;
					entry.uploadMeshResource = {};
					entry.directComputeRequested = false;
					entry.directComputeFailed = false;
					entry.directComputeRequestFrame = UINT32_MAX;
					entry.directComputeFailure = NRIVoxelComputeDirectPublishFailure::QueueFull;
					if (entry.schedulerTokenId != 0)
					{
						admissionScheduler.Cancel(entry.schedulerTokenId);
						entry.schedulerTokenId = 0;
					}
					entry.state = PersistentVoxelAdmissionState::Pending;
					entry.lastReason = "direct-publish-queue-full";
					outInProgress = true;
					if (outStats != nullptr)
					{
						outStats->directPending++;
					}
					if (loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
					{
						Printf("PERF pt voxel compute direct publish NRI: action=defer reason=queue_full tex=%d voxel=%d mesh_variant=0x%llx generation=%llu max_jobs=%u\n",
							variant.sourcePicnum,
							variant.resolvedVoxelIndex,
							(unsigned long long)variant.meshKeyHash,
							(unsigned long long)request.generation,
							(uint32_t)std::max(0, (int)nri_ptvoxelcomputemaxjobs));
					}
					return true;
				}
				arenaVertexCursor = entry.savedVertexCursor;
				arenaIndexCursor = entry.savedIndexCursor;
				arenaPrimitiveCursor = entry.savedPrimitiveCursor;
				entry.uploadMeshResource = {};
				entry.directComputeFailed = true;
				if (entry.schedulerTokenId != 0)
				{
					admissionScheduler.Fail(entry.schedulerTokenId);
					entry.schedulerTokenId = 0;
				}
				entry.directComputeFailure =
					directStatus == NRIVoxelComputeGeneratedGeometryStatus::Failed ?
					NRIVoxelComputeDirectPublishFailure::StatusFailed :
					NRIVoxelComputeDirectPublishFailure::Disabled;
				if (outStats != nullptr)
				{
					outStats->directRejected++;
				}
			}
		}

		if (directOnlyAdmission && variant.surface == nullptr)
		{
			entry.state = PersistentVoxelAdmissionState::Pending;
			entry.lastReason = "direct-only-no-cpu-fallback";
			outInProgress = true;
			return true;
		}

		if (!entry.uploadGeometryFromCompute)
		{
			if (!entry.cpuGeometryBuildCounted)
			{
				entry.cpuGeometryBuildCounted = true;
				cumulativeCpuGeometryBuildCount++;
			}
			if (outStats != nullptr &&
				entry.runtimeRequested &&
				ShouldDirectPublishNRIVoxelComputeMeshing() &&
				variant.model != nullptr)
			{
				outStats->cpuGeometryFallback++;
				cumulativeCpuGeometryFallbackCount++;
			}
			nri_scene::BuildGeometry(variantSceneView, entry.uploadGeometry);
			services.AssignGeometryPortalIndices(entry.uploadGeometry);
		}
		if (entry.uploadGeometry.vertices.empty() || entry.uploadGeometry.indices.empty() || entry.uploadGeometry.primitives.empty())
		{
			return rollbackAdmission("empty-geometry", "geometry");
		}

		PersistentVoxelMeshVariantResource& meshResource = entry.uploadMeshResource;
		allocateArenaSlice((uint32_t)entry.uploadGeometry.vertices.size(), arenaVertexCursor, meshResource.vertexOffset, meshResource.vertexCapacity);
		allocateArenaSlice((uint32_t)entry.uploadGeometry.indices.size(), arenaIndexCursor, meshResource.indexOffset, meshResource.indexCapacity);
		allocateArenaSlice((uint32_t)entry.uploadGeometry.primitives.size(), arenaPrimitiveCursor, meshResource.primitiveOffset, meshResource.primitiveCapacity);
		entry.shaderVertexOffset = meshResource.vertexOffset;
		entry.shaderIndexOffset = meshResource.indexOffset;
		entry.shaderPrimitiveOffset = meshResource.primitiveOffset;
		entry.uploadGpuIndices = entry.uploadGeometry.indices;
		for (uint32_t& index : entry.uploadGpuIndices)
		{
			index += entry.shaderVertexOffset;
		}
		entry.uploadGpuPrimitives = entry.uploadGeometry.primitives;
		for (nri_scene::PrimitiveData& primitive : entry.uploadGpuPrimitives)
		{
			primitive.reserved0 = UINT32_MAX;
			primitive.indices[0] += entry.shaderVertexOffset;
			primitive.indices[1] += entry.shaderVertexOffset;
			primitive.indices[2] += entry.shaderVertexOffset;
		}

		const uint64_t vertexBytes = entry.uploadGeometry.vertices.size() * sizeof(nri_scene::SceneVertex);
		const uint64_t indexBytes = entry.uploadGeometry.indices.size() * sizeof(uint32_t);
		if (!services.CreateStructuredBufferNoUpload(
				meshResource.vertexBuffer,
				vertexBytes,
				sizeof(nri_scene::SceneVertex),
				PersistentVoxelBufferUsageFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT)) ||
			!services.CreateStructuredBufferNoUpload(
				meshResource.indexBuffer,
				indexBytes,
				sizeof(uint32_t),
				PersistentVoxelBufferUsageFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT)) ||
			!services.EnsureArenaBuffer(
				vertexBuffer,
				(uint64_t)arenaVertexCursor * sizeof(nri_scene::SceneVertex),
				sizeof(nri_scene::SceneVertex),
				nri::BufferUsageBits::SHADER_RESOURCE,
				PersistentVoxelComputeShaderResourceAccess()) ||
			!services.EnsureArenaBuffer(
				indexBuffer,
				(uint64_t)arenaIndexCursor * sizeof(uint32_t),
				sizeof(uint32_t),
				nri::BufferUsageBits::SHADER_RESOURCE,
				PersistentVoxelComputeShaderResourceAccess()) ||
			!services.EnsureArenaBuffer(
				primitiveBuffer,
				(uint64_t)arenaPrimitiveCursor * sizeof(nri_scene::PrimitiveData),
				sizeof(nri_scene::PrimitiveData),
				nri::BufferUsageBits::SHADER_RESOURCE,
				PersistentVoxelComputeShaderResourceAccess()))
		{
			return rollbackAdmission("buffer-allocation-failed", "buffers");
		}

		meshResource.resourceKey = meshResourceKey;
		meshResource.sourceModel = variant.model;
		meshResource.shadowProxyPrimitiveSemanticsCertified =
			CertifyNRIVoxelShadowProxyPrimitiveSemantics(entry.uploadGeometry.primitives, 1u);
		meshResource.meshKeyHash = variant.meshKeyHash;
		meshResource.geometrySignature = ResolvePersistentVoxelVariantGeometrySignature(variant);
		meshResource.geometryContentHash = variant.geometryContentHash;
		meshResource.renderPrimitiveHash = variant.renderPrimitiveHash;
		meshResource.transformBasisSignature = 0;
		meshResource.meshBakeSpace = nri_scene::VoxelMeshBakeSpace::LocalSpace;
		meshResource.vertexCount = (uint32_t)entry.uploadGeometry.vertices.size();
		meshResource.indexCount = (uint32_t)entry.uploadGeometry.indices.size();
		meshResource.primitiveCount = (uint32_t)entry.uploadGeometry.primitives.size();
		FillPersistentVoxelMeshBounds(entry.uploadGeometry.vertices, meshResource);
		meshResource.bakedTranslation[0] = 0.0f;
		meshResource.bakedTranslation[1] = 0.0f;
		meshResource.bakedTranslation[2] = 0.0f;
		meshResource.tlasReadyFrame = 0;
		meshResource.tlasPublished = false;
		meshResource.lastDesiredMapGeneration = residencyMapGeneration;
		meshResource.lastUsedMapGeneration = residencyMapGeneration;
		meshResource.lastUsedFrame = frameIndex;
		meshResource.sourceBits |= entry.sourceBits;
		meshResource.priority = entry.priority;
		meshResource.gpuForce = meshResource.gpuForce || entry.gpuForce;
		meshResource.gpuPrefer = meshResource.gpuPrefer || entry.gpuPrefer;
		meshResource.residentBytes =
			meshResource.vertexBuffer.memorySize +
			meshResource.indexBuffer.memorySize +
			meshResource.accelerationStructure.memorySize;
		meshResource.cold = false;
		entry.state = PersistentVoxelAdmissionState::UploadingVertices;
		entry.uploadPrepared = true;
		if (entry.uploadGeometryFromCompute && (loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0))
		{
			Printf("PERF pt voxel compute consume NRI: action=prepare-resource tex=%d voxel=%d mesh_variant=0x%llx job=%u vertices=%u indices=%u primitives=%u\n",
				variant.sourcePicnum,
				variant.resolvedVoxelIndex,
				(unsigned long long)variant.meshKeyHash,
				entry.computeGeometryJobId,
				meshResource.vertexCount,
				meshResource.indexCount,
				meshResource.primitiveCount);
		}
	}

	auto uploadBytes = [&](const char* stream, NRIBufferResource& target, uint64_t targetOffset, const void* source, uint64_t sourceOffset, uint64_t totalBytes, uint32_t stride, nri::AccessStage after, int uploadKind, uint64_t& uploadedBytes) -> bool
	{
		if (uploadedBytes >= totalBytes)
		{
			return true;
		}
		const uint64_t remaining = totalBytes - uploadedBytes;
		uint64_t chunkBytes = remaining;
		if (byteBudget != 0)
		{
			if (outUploadBytes >= byteBudget)
			{
				outInProgress = true;
				return true;
			}
			chunkBytes = std::min(chunkBytes, byteBudget - outUploadBytes);
			if (chunkBytes < stride && remaining >= stride)
			{
				outInProgress = true;
				return true;
			}
			chunkBytes -= chunkBytes % stride;
			if (chunkBytes == 0)
			{
				outInProgress = true;
				return true;
			}
		}
		const uint8_t* bytes = static_cast<const uint8_t*>(source);
		if (!services.StageBufferCopyRange(target, targetOffset + uploadedBytes, bytes + sourceOffset + uploadedBytes, chunkBytes, after, uploadKind))
		{
			return false;
		}
		services.NoteBufferUpload(
			uploadKind,
			chunkBytes,
			uploadKind == ResidentUploadKind_Index ? "persistent_voxel_mesh_index" : (uploadKind == ResidentUploadKind_Primitive ? "persistent_voxel_mesh_primitive" : "persistent_voxel_mesh_vertex"));
		traceChunk(stream, uploadedBytes, chunkBytes, totalBytes);
		uploadedBytes += chunkBytes;
		outUploadBytes += chunkBytes;
		outInProgress = uploadedBytes < totalBytes;
		return true;
	};

	const uint64_t vertexBytes = entry.uploadGeometry.vertices.size() * sizeof(nri_scene::SceneVertex);
	const uint64_t indexBytes = entry.uploadGeometry.indices.size() * sizeof(uint32_t);
	const uint64_t primitiveBytes = entry.uploadGpuPrimitives.size() * sizeof(nri_scene::PrimitiveData);
	while (entry.state == PersistentVoxelAdmissionState::UploadingVertices)
	{
		if (!uploadBytes("vertex-private", entry.uploadMeshResource.vertexBuffer, 0, entry.uploadGeometry.vertices.data(), 0, vertexBytes, sizeof(nri_scene::SceneVertex), PersistentVoxelAccelerationStructureBuildInputAccess(), ResidentUploadKind_Vertex, entry.vertexBytesUploaded))
		{
			return rollbackAdmission("buffer-stage-failed", "vertex-private");
		}
		if (outInProgress)
		{
			return true;
		}
		if (!uploadBytes("vertex-arena", vertexBuffer, (uint64_t)entry.shaderVertexOffset * sizeof(nri_scene::SceneVertex), entry.uploadGeometry.vertices.data(), 0, vertexBytes, sizeof(nri_scene::SceneVertex), PersistentVoxelComputeShaderResourceAccess(), ResidentUploadKind_Vertex, entry.vertexArenaBytesUploaded))
		{
			return rollbackAdmission("buffer-stage-failed", "vertex-arena");
		}
		if (outInProgress)
		{
			return true;
		}
		entry.state = PersistentVoxelAdmissionState::UploadingIndices;
	}
	while (entry.state == PersistentVoxelAdmissionState::UploadingIndices)
	{
		if (!uploadBytes("index-private", entry.uploadMeshResource.indexBuffer, 0, entry.uploadGeometry.indices.data(), 0, indexBytes, sizeof(uint32_t), PersistentVoxelAccelerationStructureBuildInputAccess(), ResidentUploadKind_Index, entry.indexBytesUploaded))
		{
			return rollbackAdmission("buffer-stage-failed", "index-private");
		}
		if (outInProgress)
		{
			return true;
		}
		if (!uploadBytes("index-arena", indexBuffer, (uint64_t)entry.shaderIndexOffset * sizeof(uint32_t), entry.uploadGpuIndices.data(), 0, indexBytes, sizeof(uint32_t), PersistentVoxelComputeShaderResourceAccess(), ResidentUploadKind_Index, entry.indexArenaBytesUploaded))
		{
			return rollbackAdmission("buffer-stage-failed", "index-arena");
		}
		if (outInProgress)
		{
			return true;
		}
		entry.state = PersistentVoxelAdmissionState::UploadingPrimitives;
	}
	while (entry.state == PersistentVoxelAdmissionState::UploadingPrimitives)
	{
		if (!uploadBytes("primitive", primitiveBuffer, (uint64_t)entry.shaderPrimitiveOffset * sizeof(nri_scene::PrimitiveData), entry.uploadGpuPrimitives.data(), 0, primitiveBytes, sizeof(nri_scene::PrimitiveData), PersistentVoxelComputeShaderResourceAccess(), ResidentUploadKind_Primitive, entry.primitiveBytesUploaded))
		{
			return rollbackAdmission("buffer-stage-failed", "primitive");
		}
		if (outInProgress)
		{
			return true;
		}
		entry.state = PersistentVoxelAdmissionState::BuildingBlas;
	}

	if (entry.state == PersistentVoxelAdmissionState::BuildingBlas)
	{
		if (blasBudget == 0)
		{
			outInProgress = true;
			return true;
		}
		if (isolateBlasBuild && !entry.uploadSubmittedBeforeBlas)
		{
			if (loadingTraceLevel >= 1 || voxelStatsEnabled)
			{
				Printf("NRI PT voxel admission entry: event=pre-blas-submit tex=%d voxel=%d mesh_variant=0x%llx prims=%u upload_bytes=%llu reason=isolate-large-blas\n",
					variant.sourcePicnum,
					variant.resolvedVoxelIndex,
					(unsigned long long)variant.meshKeyHash,
					entry.uploadMeshResource.primitiveCount,
					(unsigned long long)(
						entry.vertexBytesUploaded +
						entry.vertexArenaBytesUploaded +
						entry.indexBytesUploaded +
						entry.indexArenaBytesUploaded +
						entry.primitiveBytesUploaded));
			}
			if (!services.SubmitWaitAndRestart("voxel-pre-blas-upload"))
			{
				if (services.IsSubmitBudgetHit())
				{
					outInProgress = true;
					entry.lastReason = "preload-submit-budget";
					return true;
				}
				return rollbackAdmission("pre-blas-submit-wait-failed", "pre_blas_submit");
			}
			entry.uploadSubmittedBeforeBlas = true;
		}
		if (!services.BuildBottomLevel(
			entry.uploadMeshResource.vertexBuffer,
			entry.uploadMeshResource.indexBuffer,
			0u,
			entry.uploadMeshResource.vertexCount,
			0u,
			entry.uploadMeshResource.indexCount,
			entry.uploadMeshResource.primitiveCount,
			entry.uploadMeshResource.accelerationStructure))
		{
			return rollbackAdmission("blas-build-failed", "building_blas");
		}
		blasBudget--;
		if (loadingTraceLevel >= 2 || voxelStatsEnabled)
		{
			Printf("NRI PT voxel admission entry: event=blas-build tex=%d voxel=%d mesh_variant=0x%llx prims=%u bytes=%llu\n",
				variant.sourcePicnum,
				variant.resolvedVoxelIndex,
				(unsigned long long)variant.meshKeyHash,
				entry.uploadMeshResource.primitiveCount,
				(unsigned long long)(vertexBytes + indexBytes + primitiveBytes));
		}
		if (!services.BarrierBuildInputs(entry.uploadMeshResource.vertexBuffer, entry.uploadMeshResource.indexBuffer))
		{
			return rollbackAdmission("blas-input-barrier-failed", "building_blas");
		}
	}

	auto existingMeshIt = meshVariantResources.find(meshResourceKey);
	if (existingMeshIt != meshVariantResources.end())
	{
		services.RetireBuffer(existingMeshIt->second.vertexBuffer);
		services.RetireBuffer(existingMeshIt->second.indexBuffer);
		services.RetireAccelerationStructure(existingMeshIt->second.accelerationStructure);
		RetirePersistentVoxelShadowProxy(existingMeshIt->second.shadowProxy, services);
	}
	entry.uploadMeshResource.residentBytes =
		entry.uploadMeshResource.vertexBuffer.memorySize +
		entry.uploadMeshResource.indexBuffer.memorySize +
		entry.uploadMeshResource.accelerationStructure.memorySize;
	entry.uploadMeshResource.lastUsedFrame = frameIndex;
	entry.uploadMeshResource.sourceBits |= entry.sourceBits;
	entry.uploadMeshResource.priority = entry.priority;
	entry.uploadMeshResource.gpuForce = entry.uploadMeshResource.gpuForce || entry.gpuForce;
	entry.uploadMeshResource.gpuPrefer = entry.uploadMeshResource.gpuPrefer || entry.gpuPrefer;
	entry.uploadMaterialResource.lastUsedFrame = frameIndex;
	entry.uploadMaterialResource.sourceBits |= entry.sourceBits;
	entry.uploadMaterialResource.priority = entry.priority;
	entry.uploadMaterialResource.gpuForce = entry.uploadMaterialResource.gpuForce || entry.gpuForce;
	entry.uploadMaterialResource.gpuPrefer = entry.uploadMaterialResource.gpuPrefer || entry.gpuPrefer;
	entry.uploadMaterialResource.residentBytes =
		(uint64_t)entry.uploadMaterialResource.materialBridge.materials.size() * (uint64_t)sizeof(nri_scene::MaterialData);
	bool canonicalMaterialReused = false;
	if (!PublishCanonicalMaterialResource(entry.uploadMaterialResource, canonicalMaterialReused))
	{
		return rollbackAdmission("material-publish-failed", "publish");
	}
	meshVariantResources[meshResourceKey] = std::move(entry.uploadMeshResource);
	publishedMeshKeys.insert(meshResourceKey);
	entry.uploadMeshResource = {};
	entry.uploadMaterialResource = {};
	entry.uploadGeometry = {};
	entry.uploadGpuIndices.clear();
	entry.uploadGpuPrimitives.clear();
	entry.uploadPrepared = false;
	entry.uploadGeometryFromCompute = false;
	entry.computeGeometryFailed = false;
	entry.computeGeometryJobId = 0;
	entry.directComputeRequested = false;
	entry.directComputeFailed = false;
	entry.directComputeGeneration = 0;
	entry.directComputeJobId = 0;
	entry.directComputeRequestFrame = UINT32_MAX;
	entry.directComputeOutputKind = NRIVoxelComputeDirectPublishOutputKind::None;
	entry.directComputeFailure = NRIVoxelComputeDirectPublishFailure::None;
	entry.vertexBytesUploaded = 0;
	entry.vertexArenaBytesUploaded = 0;
	entry.indexBytesUploaded = 0;
	entry.indexArenaBytesUploaded = 0;
	entry.primitiveBytesUploaded = 0;
	entry.state = PersistentVoxelAdmissionState::Ready;
	if (loadingTraceLevel >= 2 || voxelStatsEnabled)
	{
		Printf("NRI PT voxel admission entry: event=ready tex=%d voxel=%d mesh_variant=0x%llx prims=%u bytes=%llu\n",
			variant.sourcePicnum,
			variant.resolvedVoxelIndex,
			(unsigned long long)variant.meshKeyHash,
			variant.primitiveCount,
			(unsigned long long)(vertexBytes + indexBytes + primitiveBytes));
		Printf("NRI PT voxel admission transaction: event=commit reason=admitted tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu step=publish\n",
			variant.sourcePicnum,
			variant.resolvedVoxelIndex,
			(unsigned long long)variant.meshKeyHash,
			(unsigned long long)variant.materialKeyHash,
			variant.primitiveCount,
			(unsigned long long)(vertexBytes + indexBytes + primitiveBytes));
	}
	return true;
}

bool NRIPersistentVoxelResidency::EnsureBatch(
	uint64_t buildSerial,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& resetServices,
	const NRIPersistentVoxelBatchServices& batchServices,
	NRIPersistentVoxelBatchStats& outStats)
{
	const uint64_t cacheSerial = nri_scene::GetPersistentVoxelCacheSerial();
	std::vector<nri_scene::PersistentVoxelCacheEntryView> cacheEntries;
	bool hasPersistentVoxelCacheEntries = false;
	{
		PersistentVoxelScopedTimer perfTimer(outStats.persistentVoxelBatchCacheEntryMs);
		hasPersistentVoxelCacheEntries = nri_scene::BuildPersistentVoxelCacheEntries(cacheEntries);
	}

	if (!hasPersistentVoxelCacheEntries)
	{
		ClearActorInstances(resetServices);
		return false;
	}

	if (batch.valid &&
		cacheSerial == batch.sourceSerial &&
		batchMaterialResourceGeneration == materialResourceGeneration &&
		cacheEntries.size() == batch.activeActorCount)
	{
		std::unordered_map<uint64_t, PersistentVoxelBatch::ActorEntry*> activeActors;
		activeActors.reserve(batch.activeActorCount);
		for (PersistentVoxelBatch::ActorEntry& actor : batch.actors)
		{
			if (actor.active)
			{
				activeActors.emplace(actor.identityKey, &actor);
			}
		}
		bool serialFastPathValid = activeActors.size() == cacheEntries.size();
		for (const nri_scene::PersistentVoxelCacheEntryView& cacheEntry : cacheEntries)
		{
			auto actorIt = activeActors.find(cacheEntry.identityKey);
			auto instanceIt = instances.find(cacheEntry.identityKey);
			if (!IsPersistentVoxelCacheEntryPublicationCurrent(cacheEntry) ||
				actorIt == activeActors.end() || instanceIt == instances.end() || instanceIt->second.pending)
			{
				serialFastPathValid = false;
				break;
			}
			PersistentVoxelBatch::ActorEntry& actor = *actorIt->second;
			auto meshIt = meshVariantResources.find(actor.meshResourceKey);
			auto materialIt = materialVariantResources.find(actor.materialKeyHash);
			if (meshIt == meshVariantResources.end() ||
				materialIt == materialVariantResources.end() ||
				!materialRangeAllocator.Owns(PersistentVoxelMaterialRangeHandle(materialIt->second)) ||
				!PersistentVoxelMaterialRangeMatches(actor, materialIt->second) ||
				meshIt->second.accelerationStructure.accelerationStructure == nullptr ||
				actor.signature != cacheEntry.signature ||
				actor.geometrySignature != ResolvePersistentVoxelCacheEntryGeometrySignature(cacheEntry) ||
				actor.surfaceSignature != cacheEntry.surfaceSignature ||
				actor.bakedSurfaceSignature != cacheEntry.bakedSurfaceSignature ||
				actor.materialSignature != cacheEntry.materialSignature ||
				actor.meshKeyHash != cacheEntry.meshKeyHash ||
				actor.materialKeyHash != cacheEntry.materialKeyHash ||
				actor.indirectOnly != cacheEntry.indirectOnly ||
				actor.ownerWorldEpoch != cacheEntry.ownerWorldEpoch ||
				actor.ownerLifetimeGeneration != cacheEntry.ownerLifetimeGeneration ||
				actor.placementGeneration != cacheEntry.placementGeneration ||
				actor.placementStateHash != cacheEntry.placementStateHash ||
				actor.physicalSectorIndex != cacheEntry.physicalSectorIndex ||
				actor.authorityCurrent != cacheEntry.authorityCurrent ||
				actor.publicationEligible != cacheEntry.publicationEligible ||
				actor.pendingRemoval != cacheEntry.pendingRemoval ||
				actor.visibilityChunkIndex != ResolvePersistentVoxelActorVisibilityChunk(cacheEntry))
			{
				serialFastPathValid = false;
				break;
			}
			meshIt->second.lastUsedFrame = frameIndex;
			meshIt->second.lastUsedMapGeneration = residencyMapGeneration;
			meshIt->second.cold = false;
			materialIt->second.lastUsedFrame = frameIndex;
			materialIt->second.lastUsedMapGeneration = residencyMapGeneration;
			materialIt->second.cold = false;
			std::array<float, 12> expectedTransform = {};
			FillPersistentVoxelActorInstanceTransform(cacheEntry, meshIt->second, expectedTransform);
			if (!SamePersistentVoxelInstanceTransform(actor.instanceTransform, expectedTransform.data()) ||
				!SamePersistentVoxelInstanceTransform(instanceIt->second.currentTransform, expectedTransform.data()))
			{
				serialFastPathValid = false;
				break;
			}
		}
		if (serialFastPathValid)
		{
			for (const nri_scene::PersistentVoxelCacheEntryView& cacheEntry : cacheEntries)
			{
				PersistentVoxelBatch::ActorEntry& actor = *activeActors[cacheEntry.identityKey];
				PersistentVoxelInstanceRecord& instance = instances[cacheEntry.identityKey];
				instance.previousTransform = instance.currentTransform;
				instance.lastSeenFrame = frameIndex;
				instance.active = true;
				instance.pending = false;
				CopyPersistentVoxelInstanceAuthority(cacheEntry, instance);
				actor.previousInstanceTransform = actor.instanceTransform;
				CopyPersistentVoxelActorAuthority(cacheEntry, actor);
				actor.bindingGeneration = BuildPersistentVoxelActorBindingGeneration(actor);
				actor.lastSeenFrame = cacheEntry.lastSeenFrame;
				actor.retainedFrameAge = cacheEntry.retainedFrameAge;
				actor.sourcePicnum = cacheEntry.sourcePicnum;
				actor.resolvedVoxelIndex = cacheEntry.resolvedVoxelIndex;
				actor.capturedThisFrame = cacheEntry.capturedThisFrame;
			}
			outStats.persistentVoxelBatchSerialFastPathCount++;
			return true;
		}
	}

	{
		PersistentVoxelScopedTimer perfTimer(outStats.persistentVoxelBatchExistingActorMapMs);
		std::unordered_set<uint64_t> currentActorKeys;
		currentActorKeys.reserve(batch.actors.size());
		if (batch.valid)
		{
			for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
			{
				if (actor.active)
				{
					currentActorKeys.insert(actor.identityKey);
				}
			}
		}

		{
			PersistentVoxelScopedTimer sortTimer(outStats.persistentVoxelBatchSortMs);
			std::sort(cacheEntries.begin(), cacheEntries.end(), [&](const auto& left, const auto& right)
			{
				const auto leftPendingIt = instances.find(left.identityKey);
				const auto rightPendingIt = instances.find(right.identityKey);
				const bool leftPending = leftPendingIt != instances.end() && leftPendingIt->second.pending;
				const bool rightPending = rightPendingIt != instances.end() && rightPendingIt->second.pending;
				if (leftPending != rightPending)
				{
					return leftPending;
				}
				const bool leftHasResidentActor = currentActorKeys.find(left.identityKey) != currentActorKeys.end();
				const bool rightHasResidentActor = currentActorKeys.find(right.identityKey) != currentActorKeys.end();
				if (leftHasResidentActor != rightHasResidentActor)
				{
					return !leftHasResidentActor;
				}
				if (left.primitiveCount != right.primitiveCount)
				{
					return left.primitiveCount > right.primitiveCount;
				}
				return left.identityKey < right.identityKey;
			});
		}
	}

	uint32_t voxelPromotionQueued = 0;
	uint32_t voxelPromotionPromoted = 0;
	uint32_t voxelPromotionSkippedBudget = 0;
	uint32_t voxelPromotionCpuReady = (uint32_t)cacheEntries.size();
	uint32_t voxelPromotionGpuReady = 0;
	uint64_t voxelPromotionUploadBytes = 0;

	struct PersistentVoxelRuntimeBudget
	{
		uint32_t buildActors = 0;
		uint32_t buildPrimitives = 0;
		uint64_t buildBytes = 0;
		uint32_t texturePrewarms = 0;
		uint64_t textureBytes = 0;
		int mode = 0;
	};

	auto getPersistentVoxelRuntimeBudget = [&]() -> PersistentVoxelRuntimeBudget
	{
		PersistentVoxelRuntimeBudget budget = {};
		budget.mode = (int)settings.runtimeBudgetMode;
		if (loadingWarmupActive)
		{
			budget.mode = 4;
		}
		switch (budget.mode)
		{
		case 1:
			budget.buildActors = 1;
			budget.buildPrimitives = 50000;
			budget.buildBytes = 2ull * 1024ull * 1024ull;
			budget.texturePrewarms = 1;
			budget.textureBytes = 512ull * 1024ull;
			return budget;
		case 2:
			budget.buildActors = 2;
			budget.buildPrimitives = 100000;
			budget.buildBytes = 4ull * 1024ull * 1024ull;
			budget.texturePrewarms = 2;
			budget.textureBytes = 1024ull * 1024ull;
			return budget;
		case 3:
			budget.buildActors = 4;
			budget.buildPrimitives = 250000;
			budget.buildBytes = 16ull * 1024ull * 1024ull;
			budget.texturePrewarms = 4;
			budget.textureBytes = 4ull * 1024ull * 1024ull;
			return budget;
		case 4:
			budget.buildActors = UINT32_MAX;
			budget.buildPrimitives = 0;
			budget.buildBytes = 0;
			budget.texturePrewarms = 0;
			budget.textureBytes = 0;
			return budget;
		default:
			budget.buildActors = settings.buildActors;
			budget.buildPrimitives = settings.buildPrimitives;
			budget.buildBytes = settings.buildBytes;
			budget.texturePrewarms = settings.texturePrewarms;
			budget.textureBytes = settings.textureBytes;
			return budget;
		}
	};
	const PersistentVoxelRuntimeBudget runtimeBudget = getPersistentVoxelRuntimeBudget();

	{
		PersistentVoxelScopedTimer perfTimer(outStats.persistentVoxelBatchInstanceSyncMs);
		std::unordered_set<uint64_t> activeInstanceKeys;
		activeInstanceKeys.reserve(cacheEntries.size());
		for (const nri_scene::PersistentVoxelCacheEntryView& cacheEntry : cacheEntries)
		{
			activeInstanceKeys.insert(cacheEntry.identityKey);
			PersistentVoxelInstanceRecord& instance = instances[cacheEntry.identityKey];
			const bool newInstance = instance.identityKey == 0;
			if (newInstance)
			{
				instance.currentTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
			}
			instance.previousTransform = instance.currentTransform;
			instance.identityKey = cacheEntry.identityKey;
			instance.signature = cacheEntry.signature;
			instance.geometrySignature = cacheEntry.geometrySignature;
			instance.surfaceSignature = cacheEntry.surfaceSignature;
			instance.bakedSurfaceSignature = cacheEntry.bakedSurfaceSignature;
			instance.materialSignature = cacheEntry.materialSignature;
			instance.meshKeyHash = cacheEntry.meshKeyHash;
			instance.materialKeyHash = cacheEntry.materialKeyHash;
			instance.meshVariantHash = cacheEntry.meshVariantHash;
			instance.materialVariantHash = cacheEntry.materialVariantHash;
			instance.primitiveCount = cacheEntry.primitiveCount;
			instance.lastSeenFrame = frameIndex;
			instance.active = IsPersistentVoxelCacheEntryPublicationCurrent(cacheEntry);
			instance.pending = false;
			CopyPersistentVoxelInstanceAuthority(cacheEntry, instance);
			CopyPersistentVoxelInstanceTransform(cacheEntry.instanceTransform, instance.currentTransform);
			if (newInstance)
			{
				instance.previousTransform = instance.currentTransform;
			}
		}
		for (auto it = instances.begin(); it != instances.end(); )
		{
			if (activeInstanceKeys.find(it->first) == activeInstanceKeys.end())
			{
				it = instances.erase(it);
				continue;
			}
			++it;
		}
	}

	const bool persistentVoxelTexturePrewarmUnlimited = runtimeBudget.texturePrewarms == 0;
	const uint32_t persistentVoxelTexturePrewarmBudget =
		persistentVoxelTexturePrewarmUnlimited ? 0u : runtimeBudget.texturePrewarms;
	const uint64_t persistentVoxelTexturePrewarmByteBudget = runtimeBudget.textureBytes;
	uint32_t persistentVoxelPrewarmedTextures = 0;
	uint64_t persistentVoxelPrewarmedTextureBytes = 0;
	outStats.persistentVoxelTexturePrewarmByteBudget = persistentVoxelTexturePrewarmByteBudget;

	auto estimateTextureUploadBytes = [](const nri_scene::TextureUpload& upload) -> uint64_t
	{
		const uint64_t bytesPerPixel = upload.indexed ? 1ull : 4ull;
		return (uint64_t)upload.width * (uint64_t)upload.height * bytesPerPixel;
	};

	auto isTextureUploadCached = [&](const nri_scene::TextureUpload& upload) -> bool
	{
		return batchServices.IsTextureCached(upload);
	};

	auto canPrewarmTextureUpload = [&](uint64_t estimatedBytes) -> bool
	{
		if (persistentVoxelPrewarmedTextures == 0)
		{
			return true;
		}
		if (!persistentVoxelTexturePrewarmUnlimited &&
			persistentVoxelPrewarmedTextures >= persistentVoxelTexturePrewarmBudget)
		{
			return false;
		}
		if (persistentVoxelTexturePrewarmByteBudget != 0 &&
			persistentVoxelPrewarmedTextureBytes + estimatedBytes > persistentVoxelTexturePrewarmByteBudget)
		{
			return false;
		}
		return true;
	};

	auto prewarmPersistentVoxelActorTextures = [&](const nri_scene::MaterialBridgeData& actorMaterials) -> bool
	{
		for (const nri_scene::TextureUpload& upload : actorMaterials.textures)
		{
			if (upload.width == 0 || upload.height == 0)
			{
				continue;
			}
			if (upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas())
			{
				continue;
			}
			if (isTextureUploadCached(upload))
			{
				outStats.persistentVoxelTexturePrewarmHitCount++;
				if (voxelStatsEnabled)
				{
					Printf("PERF pt voxel preload NRI: frame=%u action=reuse reason=texture-cache texture_key=0x%llx width=%u height=%u indexed=%u upload_bytes=0 ready=1\n",
						frameIndex,
						(unsigned long long)upload.key,
						upload.width,
						upload.height,
						upload.indexed ? 1u : 0u);
				}
				continue;
			}

			const uint64_t estimatedBytes = estimateTextureUploadBytes(upload);
			outStats.persistentVoxelTexturePrewarmQueuedCount++;
			outStats.persistentVoxelTexturePrewarmMissCount++;
			outStats.persistentVoxelTexturePrewarmEstimatedBytes += estimatedBytes;
			if (!canPrewarmTextureUpload(estimatedBytes))
			{
				outStats.persistentVoxelTexturePrewarmDeferredCount++;
				outStats.persistentVoxelTexturePrewarmDeferredBytes += estimatedBytes;
				if (voxelStatsEnabled)
				{
					Printf("PERF pt voxel preload NRI: frame=%u action=defer reason=texture-budget texture_key=0x%llx width=%u height=%u indexed=%u upload_bytes=%llu ready=0\n",
						frameIndex,
						(unsigned long long)upload.key,
						upload.width,
						upload.height,
						upload.indexed ? 1u : 0u,
						(unsigned long long)estimatedBytes);
				}
				return false;
			}

			double prewarmMs = 0.0;
			if (!batchServices.PrewarmTexture(upload, &prewarmMs))
			{
				if (voxelStatsEnabled)
				{
					Printf("PERF pt voxel preload NRI: frame=%u action=failed reason=texture-cache texture_key=0x%llx width=%u height=%u indexed=%u upload_bytes=%llu ready=0\n",
						frameIndex,
						(unsigned long long)upload.key,
						upload.width,
						upload.height,
						upload.indexed ? 1u : 0u,
						(unsigned long long)estimatedBytes);
				}
				return false;
			}
			persistentVoxelPrewarmedTextures++;
			persistentVoxelPrewarmedTextureBytes += estimatedBytes;
			outStats.persistentVoxelTexturePrewarmProcessedCount++;
			outStats.persistentVoxelTexturePrewarmProcessedBytes += estimatedBytes;
			outStats.persistentVoxelTexturePrewarmMs += prewarmMs;
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel preload NRI: frame=%u action=upload reason=texture-cache texture_key=0x%llx width=%u height=%u indexed=%u upload_bytes=%llu ms=%.3f ready=1\n",
					frameIndex,
					(unsigned long long)upload.key,
					upload.width,
					upload.height,
					upload.indexed ? 1u : 0u,
					(unsigned long long)estimatedBytes,
					prewarmMs);
			}
		}

		return true;
	};

	enum class PersistentVoxelActorDeferredReason : uint8_t
	{
		None,
		TexturePrewarm,
		AdmissionPending,
		MaterialInvalid
	};

	auto appendActorToBatch = [&](PersistentVoxelBatch& batch, const nri_scene::PersistentVoxelCacheEntryView& cacheEntry, PersistentVoxelBatch::ActorEntry* existingActor = nullptr, PersistentVoxelActorDeferredReason* outDeferredReason = nullptr) -> bool
	{
		if (outDeferredReason != nullptr)
		{
			*outDeferredReason = PersistentVoxelActorDeferredReason::None;
		}
		if (!IsPersistentVoxelCacheEntryPublicationCurrent(cacheEntry))
		{
			if (existingActor != nullptr)
			{
				CopyPersistentVoxelActorAuthority(cacheEntry, *existingActor);
				existingActor->active = false;
				existingActor->worldTlasPublicationHash = 0;
			}
			return true;
		}
		if (cacheEntry.surface == nullptr && !cacheEntry.directOnlyAdmission)
		{
			if (existingActor != nullptr)
			{
				existingActor->active = false;
			}
			return true;
		}
		auto rejectedSignatureIt = actorRejectedSignatures.find(cacheEntry.identityKey);
		if (rejectedSignatureIt != actorRejectedSignatures.end() &&
			rejectedSignatureIt->second == cacheEntry.surfaceSignature)
		{
			if (existingActor != nullptr)
			{
				existingActor->active = false;
			}
			return true;
		}
		auto countBatchActorsUsingMeshResource = [&](uint64_t meshResourceKey) -> uint32_t
		{
			uint32_t count = 0;
			for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
			{
				if (actor.active && actor.meshResourceKey == meshResourceKey)
				{
					count++;
				}
			}
			return count;
		};
		auto countBatchActorsUsingMaterialVariant = [&](uint64_t materialKeyHash) -> uint32_t
		{
			uint32_t count = 0;
			for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
			{
				if (actor.active && actor.materialKeyHash == materialKeyHash)
				{
					count++;
				}
			}
			return count;
		};

		auto buildSingleVoxelSceneView = [&](const nri_scene::SurfaceRef& surface) -> nri_scene::SceneView
		{
			nri_scene::SceneView sceneView = {};
			sceneView.opaqueSprites.push_back(surface);
			sceneView.stats.spriteDrawItems = 1;
			sceneView.stats.modelDrawItems = 1;
			sceneView.stats.voxelProxyDrawItems = 1;
			sceneView.stats.totalDrawItems = 1;
			sceneView.stats.materialRefs = 1;
			sceneView.stats.triangleEstimate = cacheEntry.primitiveCount;
			sceneView.stats.voxelCachePrimitives = cacheEntry.primitiveCount;
			return sceneView;
		};

		auto allocateArenaSlice = [](uint32_t count, uint32_t& cursor, uint32_t& offset, uint32_t& capacity) -> bool
		{
			if (capacity >= count && count > 0)
			{
				return false;
			}

			offset = cursor;
			capacity = std::max<uint32_t>(count, capacity > 0 ? capacity * 2u : count);
			cursor += capacity;
			return true;
		};
		auto allocateExactMaterialSlice = [&](PersistentVoxelMaterialVariantResource& resource, uint32_t count, bool& moved) -> bool
		{
			NRIPersistentVoxelMaterialRangeHandle replacementRange = {};
			if (!materialRangeAllocator.Reallocate(
				PersistentVoxelMaterialRangeHandle(resource),
				count,
				replacementRange,
				moved))
			{
				return false;
			}
			resource.materialOffset = replacementRange.offset;
			resource.materialCapacity = replacementRange.capacity;
			resource.materialSlotGeneration = replacementRange.generation;
			return true;
		};

		auto materialInsert = materialVariantResources.try_emplace(cacheEntry.materialKeyHash);
		PersistentVoxelMaterialVariantResource& materialResource = materialInsert.first->second;
		if (materialInsert.second)
		{
			MarkMaintenanceMutation();
		}
		{
			PersistentVoxelScopedTimer perfTimer(outStats.persistentVoxelBatchMaterialVariantMs);
			const uint64_t validatedMaterialSignature =
				cacheEntry.materialSignature != 0 ? cacheEntry.materialSignature : cacheEntry.materialKeyHash;
			const bool materialVariantWasReady =
				materialResource.materialKeyHash == cacheEntry.materialKeyHash &&
				materialResource.materialSignature == validatedMaterialSignature &&
				materialResource.materialBridgeBuildSerial == residencyLastBuildSerial &&
				materialResource.materialSlotGeneration != 0 &&
				!materialResource.materialBridge.materials.empty();
			if (materialVariantWasReady && materialResource.materialPayloadHash == 0)
			{
				materialResource.materialPayloadHash = HashPersistentVoxelMaterialPayloadData(materialResource.materialBridge);
			}
			if (!materialVariantWasReady)
			{
				nri_scene::MaterialBridgeData builtMaterials;
				NRIPersistentVoxelMaterialClosureResult closure = {};
				const bool reusedClosure = batchServices.materialClosure.TryReuse(
					residencyLastBuildSerial,
					cacheEntry.materialKeyHash,
					validatedMaterialSignature,
					NRIPersistentVoxelMaterialClosureSource::RuntimeUnknown,
					builtMaterials,
					closure);
				if (!reusedClosure)
				{
					if (closure.state == NRIPersistentVoxelMaterialClosureState::Pending)
					{
						if (existingActor != nullptr)
						{
							existingActor->active = true;
						}
						if (outDeferredReason != nullptr)
						{
							*outDeferredReason = PersistentVoxelActorDeferredReason::TexturePrewarm;
						}
						return true;
					}
					Clocker materialClock(NriPTMaterialBuild);
					const nri_scene::SurfaceRef* materialSurface =
						cacheEntry.lightSurface != nullptr ? cacheEntry.lightSurface :
						(cacheEntry.surface != nullptr ? cacheEntry.surface : &cacheEntry.materialSurface);
					nri_scene::SceneView materialSceneView = buildSingleVoxelSceneView(
						*materialSurface);
					batchServices.BuildMaterials(materialSceneView, builtMaterials, "persistent_voxel_material_variant");
				}
				if (builtMaterials.materials.empty())
				{
					outStats.persistentVoxelOnboardingMaterialInvalidCount++;
					if (voxelStatsEnabled)
					{
						Printf("PERF pt voxel material variant NRI: frame=%u action=invalid reason=empty-materials actor_key=0x%llx mat_key=0x%llx ref_count=%u material_offset=%u material_count=0 material_capacity=%u upload_hash=0x%llx ready=0\n",
							frameIndex,
							(unsigned long long)cacheEntry.identityKey,
							(unsigned long long)cacheEntry.materialKeyHash,
							countBatchActorsUsingMaterialVariant(cacheEntry.materialKeyHash),
							materialResource.materialOffset,
							materialResource.materialCapacity,
							(unsigned long long)materialResource.materialUploadHash);
					}
					if (existingActor != nullptr)
					{
						existingActor->active = false;
					}
					return true;
				}
				if (!reusedClosure && !prewarmPersistentVoxelActorTextures(builtMaterials))
				{
					batchServices.materialClosure.Register(
						residencyLastBuildSerial,
						cacheEntry.materialKeyHash,
						validatedMaterialSignature,
						builtMaterials,
						NRIPersistentVoxelMaterialClosureSource::RuntimeUnknown,
						closure);
					if (voxelStatsEnabled)
					{
						Printf("PERF pt voxel material variant NRI: frame=%u action=defer reason=texture-prewarm actor_key=0x%llx mat_key=0x%llx ref_count=%u material_offset=%u material_count=%u material_capacity=%u upload_hash=0x%llx ready=0\n",
							frameIndex,
							(unsigned long long)cacheEntry.identityKey,
							(unsigned long long)cacheEntry.materialKeyHash,
							countBatchActorsUsingMaterialVariant(cacheEntry.materialKeyHash),
							materialResource.materialOffset,
							(uint32_t)builtMaterials.materials.size(),
							materialResource.materialCapacity,
							(unsigned long long)materialResource.materialUploadHash);
					}
					if (existingActor != nullptr)
					{
						existingActor->active = true;
					}
					if (outDeferredReason != nullptr)
					{
						*outDeferredReason = PersistentVoxelActorDeferredReason::TexturePrewarm;
					}
					return true;
				}
				if (!reusedClosure &&
					!batchServices.materialClosure.Register(
						residencyLastBuildSerial,
						cacheEntry.materialKeyHash,
						validatedMaterialSignature,
						builtMaterials,
						NRIPersistentVoxelMaterialClosureSource::RuntimeUnknown,
						closure))
				{
					if (existingActor != nullptr)
					{
						existingActor->active = true;
					}
					if (outDeferredReason != nullptr)
					{
						*outDeferredReason = PersistentVoxelActorDeferredReason::TexturePrewarm;
					}
					return true;
				}

				materialResource.materialKeyHash = cacheEntry.materialKeyHash;
				materialResource.materialSignature = validatedMaterialSignature;
				materialResource.materialBridge = std::move(builtMaterials);
				materialResource.materialBridgeBuildSerial = residencyLastBuildSerial;
				materialResource.materialCount = (uint32_t)materialResource.materialBridge.materials.size();
				materialResource.materialPayloadHash = HashPersistentVoxelMaterialPayloadData(materialResource.materialBridge);
				materialResource.materialUploadHash = 0;
				if (voxelStatsEnabled)
				{
					Printf("PERF pt voxel material variant NRI: frame=%u action=%s reason=%s actor_key=0x%llx mat_key=0x%llx ref_count=%u material_offset=%u material_count=%u material_capacity=%u upload_hash=0x%llx ready=1\n",
						frameIndex,
						reusedClosure ? "reuse" : "build",
						reusedClosure ? "preload-closure" : "none",
						(unsigned long long)cacheEntry.identityKey,
						(unsigned long long)cacheEntry.materialKeyHash,
						countBatchActorsUsingMaterialVariant(cacheEntry.materialKeyHash),
						materialResource.materialOffset,
						materialResource.materialCount,
						materialResource.materialCapacity,
						(unsigned long long)materialResource.materialUploadHash);
				}
			}

			bool materialSliceMoved = false;
			if (!allocateExactMaterialSlice(
				materialResource,
				(uint32_t)materialResource.materialBridge.materials.size(),
				materialSliceMoved))
			{
				return false;
			}
			if (materialSliceMoved)
			{
				materialResource.materialUploadHash = 0;
			}
			if (!materialVariantWasReady || materialSliceMoved)
			{
				dirtyMaterialResourceKeys.insert(cacheEntry.materialKeyHash);
				materialResourceGeneration++;
				if (!materialInsert.second)
				{
					MarkMaintenanceMutation();
				}
			}
			materialResource.materialCount = (uint32_t)materialResource.materialBridge.materials.size();
			materialResource.lastDesiredMapGeneration = residencyMapGeneration;
			materialResource.lastUsedMapGeneration = residencyMapGeneration;
			materialResource.lastUsedFrame = frameIndex;
			materialResource.residentBytes =
				(uint64_t)materialResource.materialBridge.materials.size() * (uint64_t)sizeof(nri_scene::MaterialData);
			materialResource.cold = false;
			if (materialResource.materialCount == 0)
			{
				outStats.persistentVoxelOnboardingMaterialInvalidCount++;
				if (voxelStatsEnabled)
				{
					Printf("PERF pt voxel material variant NRI: frame=%u action=invalid reason=empty-after-allocation actor_key=0x%llx mat_key=0x%llx ref_count=%u material_offset=%u material_count=0 material_capacity=%u upload_hash=0x%llx ready=0\n",
						frameIndex,
						(unsigned long long)cacheEntry.identityKey,
						(unsigned long long)cacheEntry.materialKeyHash,
						countBatchActorsUsingMaterialVariant(cacheEntry.materialKeyHash),
						materialResource.materialOffset,
						materialResource.materialCapacity,
						(unsigned long long)materialResource.materialUploadHash);
				}
				if (existingActor != nullptr)
				{
					existingActor->active = false;
				}
				return true;
			}
			if (materialVariantWasReady && voxelStatsEnabled)
			{
				Printf("PERF pt voxel material variant NRI: frame=%u action=reuse reason=none actor_key=0x%llx mat_key=0x%llx ref_count=%u material_offset=%u material_count=%u material_capacity=%u upload_hash=0x%llx ready=1\n",
					frameIndex,
					(unsigned long long)cacheEntry.identityKey,
					(unsigned long long)cacheEntry.materialKeyHash,
					countBatchActorsUsingMaterialVariant(cacheEntry.materialKeyHash),
					materialResource.materialOffset,
					materialResource.materialCount,
					materialResource.materialCapacity,
					(unsigned long long)materialResource.materialUploadHash);
			}
		}

		auto findPersistentVoxelEmissiveMaterial = [&](const nri_scene::MaterialBridgeData& materialBridge) -> uint32_t
		{
			const uint32_t materialCount = (uint32_t)std::min(materialBridge.lightMetadata.size(), materialBridge.materials.size());
			for (uint32_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
			{
				if (batchServices.MaterialWouldEmit(materialBridge.lightMetadata[materialIndex]))
				{
					return materialIndex;
				}
			}
			return UINT32_MAX;
		};
		auto ensurePersistentVoxelMeshLightTemplate = [&](PersistentVoxelMeshVariantResource& meshResource, const nri_scene::SurfaceRef* surface, const nri_scene::MaterialBridgeData& materialBridge, uint32_t materialIndex)
		{
			if (meshResource.lightTemplateValid)
			{
				return;
			}
			if (surface != nullptr)
			{
				const SceneLightSystem::SurfaceRecord templateRecord = batchServices.BuildSurfaceRecord(
					*surface,
					materialBridge,
					SceneLightRecordSource::PersistentVoxelScene,
					materialIndex,
					materialIndex);
				meshResource.lightTemplateCenter[0] = templateRecord.center[0];
				meshResource.lightTemplateCenter[1] = templateRecord.center[1];
				meshResource.lightTemplateCenter[2] = templateRecord.center[2];
				meshResource.lightTemplateBoundsRadius = templateRecord.boundsRadius;
				meshResource.lightTemplateSurfaceArea = templateRecord.surfaceArea;
				meshResource.lightTemplateValid = templateRecord.boundsRadius > 0.0f && templateRecord.surfaceArea > 0.0f;
				return;
			}
			if (!meshResource.boundsValid)
			{
				return;
			}

			const float extentX = std::max(meshResource.boundsMax[0] - meshResource.boundsMin[0], 0.0f);
			const float extentY = std::max(meshResource.boundsMax[1] - meshResource.boundsMin[1], 0.0f);
			const float extentZ = std::max(meshResource.boundsMax[2] - meshResource.boundsMin[2], 0.0f);
			for (uint32_t axis = 0; axis < 3; ++axis)
			{
				meshResource.lightTemplateCenter[axis] = (meshResource.boundsMin[axis] + meshResource.boundsMax[axis]) * 0.5f;
			}
			meshResource.lightTemplateBoundsRadius = 0.5f * std::sqrt(
				extentX * extentX + extentY * extentY + extentZ * extentZ);
			meshResource.lightTemplateSurfaceArea = meshResource.surfaceArea;
			meshResource.lightTemplateValid =
				meshResource.lightTemplateBoundsRadius > 0.0f && meshResource.lightTemplateSurfaceArea > 0.0f;
		};
		auto rebuildPersistentVoxelActorLightRecords = [&](PersistentVoxelBatch::ActorEntry& actor, PersistentVoxelMeshVariantResource& meshResource)
		{
			actor.lightRecords.clear();
			const uint32_t emissiveMaterialIndex = findPersistentVoxelEmissiveMaterial(actor.materialBridge);
			if (emissiveMaterialIndex == UINT32_MAX)
			{
				return;
			}
			ensurePersistentVoxelMeshLightTemplate(meshResource, cacheEntry.surface, actor.materialBridge, emissiveMaterialIndex);
			if (!meshResource.lightTemplateValid)
			{
				return;
			}

			SceneLightSystem::SurfaceRecord record = {};
			record.source = SceneLightRecordSource::PersistentVoxelScene;
			record.materialIndex = emissiveMaterialIndex;
			record.material = actor.materialBridge.lightMetadata[emissiveMaterialIndex];
			record.provenance = cacheEntry.lightSurface != nullptr ? cacheEntry.lightSurface->provenance :
				(cacheEntry.surface != nullptr ? cacheEntry.surface->provenance : cacheEntry.materialSurface.provenance);
			TransformPersistentVoxelLightCenter(actor.instanceTransform, meshResource.lightTemplateCenter, record.center);
			const float scale = ResolvePersistentVoxelLightScale(actor.instanceTransform);
			record.boundsRadius = meshResource.lightTemplateBoundsRadius * scale;
			record.surfaceArea = meshResource.lightTemplateSurfaceArea * scale * scale;
			record.identityKey = SceneLightSystem::ComputeSurfaceIdentityKey(record.source, record.provenance, record.center);
			actor.lightRecords.push_back(record);
		};

		const uint64_t baseMeshResourceKey = BuildPersistentVoxelMeshResourceKey(cacheEntry, settings);
		auto publishPersistentVoxelActor = [&](
			uint64_t meshResourceKey,
			PersistentVoxelMeshVariantResource& meshResource,
			uint32_t primitiveCount,
			uint32_t indexCount) -> bool
		{
			PersistentVoxelBatch::ActorEntry actor = existingActor != nullptr ? *existingActor : PersistentVoxelBatch::ActorEntry{};
			actor.identityKey = cacheEntry.identityKey;
			actor.actorIndex = cacheEntry.actorIndex;
			actor.indirectOnly = cacheEntry.indirectOnly;
			actor.signature = cacheEntry.signature;
			actor.geometrySignature = ResolvePersistentVoxelCacheEntryGeometrySignature(cacheEntry);
			actor.surfaceSignature = cacheEntry.surfaceSignature;
			actor.bakedSurfaceSignature = cacheEntry.bakedSurfaceSignature;
			actor.materialSignature = cacheEntry.materialSignature;
			actor.meshResourceKey = meshResourceKey;
			actor.meshKeyHash = cacheEntry.meshKeyHash;
			actor.materialKeyHash = cacheEntry.materialKeyHash;
			actor.meshVariantHash = cacheEntry.meshVariantHash;
			actor.materialVariantHash = cacheEntry.materialVariantHash;
			actor.active = true;
			FillPersistentVoxelActorInstanceTransform(cacheEntry, meshResource, actor.instanceTransform);
			actor.previousInstanceTransform = actor.instanceTransform;
			actor.primitiveOffset = meshResource.primitiveOffset;
			actor.primitiveCount = primitiveCount;
			actor.indexOffset = meshResource.indexOffset;
			actor.indexCount = indexCount;
			actor.materialOffset = materialResource.materialOffset;
			actor.materialCount = materialResource.materialCount;
			actor.materialSlotGeneration = materialResource.materialSlotGeneration;
			CopyPersistentVoxelActorAuthority(cacheEntry, actor);
			actor.bindingGeneration = BuildPersistentVoxelActorBindingGeneration(actor);
			actor.materialBridge = materialResource.materialBridge;
			rebuildPersistentVoxelActorLightRecords(actor, meshResource);
			auto instanceIt = instances.find(cacheEntry.identityKey);
			if (instanceIt != instances.end())
			{
				actor.previousInstanceTransform = instanceIt->second.previousTransform;
				instanceIt->second.meshResourceKey = meshResourceKey;
				instanceIt->second.currentTransform = actor.instanceTransform;
				instanceIt->second.bindingGeneration = actor.bindingGeneration;
				instanceIt->second.pending = false;
			}

			if (existingActor != nullptr)
			{
				*existingActor = actor;
			}
			else
			{
				batch.actors.push_back(actor);
			}
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel instance NRI: frame=%u action=%s reason=none actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx surface_sig=0x%llx baked_surface=0x%llx primitive_offset=%u primitive_count=%u index_offset=%u index_count=%u material_offset=%u material_count=%u ready=%u pending=0 active=1\n",
					frameIndex,
					existingActor != nullptr ? "update" : "add",
					(unsigned long long)cacheEntry.identityKey,
					(unsigned long long)meshResourceKey,
					(unsigned long long)cacheEntry.meshKeyHash,
					(unsigned long long)cacheEntry.materialKeyHash,
					(unsigned long long)cacheEntry.surfaceSignature,
					(unsigned long long)cacheEntry.bakedSurfaceSignature,
					actor.primitiveOffset,
					actor.primitiveCount,
					actor.indexOffset,
					actor.indexCount,
					actor.materialOffset,
					actor.materialCount,
					meshResource.accelerationStructure.accelerationStructure != nullptr ? 1u : 0u);
			}
			return true;
		};

		{
			PersistentVoxelScopedTimer perfTimer(outStats.persistentVoxelBatchMeshAdmissionMs);
			auto reusableMeshResourceIt = meshVariantResources.find(baseMeshResourceKey);
			if (reusableMeshResourceIt != meshVariantResources.end() &&
				reusableMeshResourceIt->second.resourceKey == baseMeshResourceKey &&
				reusableMeshResourceIt->second.vertexCount != 0 &&
				reusableMeshResourceIt->second.indexCount != 0 &&
				reusableMeshResourceIt->second.primitiveCount != 0 &&
				((reusableMeshResourceIt->second.vertexBuffer.buffer != nullptr &&
				  reusableMeshResourceIt->second.indexBuffer.buffer != nullptr) ||
				 reusableMeshResourceIt->second.directComputePublished) &&
				vertexBuffer.buffer != nullptr &&
				indexBuffer.buffer != nullptr &&
				primitiveBuffer.buffer != nullptr)
			{
				PersistentVoxelMeshVariantResource& meshResource = reusableMeshResourceIt->second;
				meshResource.lastDesiredMapGeneration = residencyMapGeneration;
				meshResource.lastUsedMapGeneration = residencyMapGeneration;
				meshResource.lastUsedFrame = frameIndex;
				meshResource.residentBytes =
					meshResource.vertexBuffer.memorySize +
					meshResource.indexBuffer.memorySize +
					meshResource.accelerationStructure.memorySize;
				meshResource.cold = false;
				if (voxelStatsEnabled)
				{
					Printf("PERF pt voxel mesh variant NRI: frame=%u action=reuse reason=none actor_key=0x%llx resource_key=0x%llx mesh_key=0x%llx mat_key=0x%llx ref_count=%u prims=%u vertices=%u indices=%u vertex_offset=%u index_offset=%u primitive_offset=%u space=%s basis_sig=0x%llx transform_keyed=%u blas=%u tlas_ready=%u tlas_published=%u upload_bytes=0 ready=1\n",
						frameIndex,
						(unsigned long long)cacheEntry.identityKey,
						(unsigned long long)baseMeshResourceKey,
						(unsigned long long)cacheEntry.meshKeyHash,
						(unsigned long long)cacheEntry.materialKeyHash,
						countBatchActorsUsingMeshResource(baseMeshResourceKey),
						meshResource.primitiveCount,
						meshResource.vertexCount,
						meshResource.indexCount,
						meshResource.vertexOffset,
						meshResource.indexOffset,
						meshResource.primitiveOffset,
						GetPersistentVoxelBakeSpaceName(meshResource.meshBakeSpace),
						(unsigned long long)meshResource.transformBasisSignature,
						IsPersistentVoxelMeshResourceTransformKeyed(cacheEntry, settings) ? 1u : 0u,
						meshResource.accelerationStructure.accelerationStructure != nullptr ? 1u : 0u,
						meshResource.tlasReadyFrame,
						meshResource.tlasPublished ? 1u : 0u);
				}
				return publishPersistentVoxelActor(
					baseMeshResourceKey,
					meshResource,
					meshResource.primitiveCount,
					meshResource.indexCount);
			}

			if (!IsSharedVariantReady(baseMeshResourceKey, cacheEntry.materialKeyHash))
			{
				const bool admissionTransformKeyed = IsPersistentVoxelMeshResourceTransformKeyed(cacheEntry, settings);
				nri_scene::PrecachedVoxelVariantView admissionVariant = {};
				admissionVariant.meshKeyHash = admissionTransformKeyed ? baseMeshResourceKey : cacheEntry.meshKeyHash;
				admissionVariant.materialKeyHash = cacheEntry.materialKeyHash;
				admissionVariant.geometrySignature = cacheEntry.geometrySignature;
				admissionVariant.geometryContentHash = admissionTransformKeyed ? 0ull : cacheEntry.geometryContentHash;
				admissionVariant.renderPrimitiveHash = admissionTransformKeyed ? 0ull : cacheEntry.renderPrimitiveHash;
				admissionVariant.meshVariantHash = cacheEntry.meshVariantHash;
				admissionVariant.materialVariantHash = cacheEntry.materialVariantHash;
				admissionVariant.sourcePicnum = cacheEntry.sourcePicnum;
				admissionVariant.resolvedVoxelIndex = cacheEntry.resolvedVoxelIndex;
				admissionVariant.primitiveCount = cacheEntry.primitiveCount;
				admissionVariant.sourceBits = loadingWarmupActive ? nri_scene::PrecachedVoxelSourceBit_MountedVoxelPreload : 0u;
				admissionVariant.priority = loadingWarmupActive ? 0 : 1;
				admissionVariant.admissionRank = admissionVariant.priority * 10000 + 9900;
				admissionVariant.gpuForce = loadingWarmupActive;
				admissionVariant.model = cacheEntry.model;
				admissionVariant.surface = cacheEntry.surface;
				admissionVariant.materialSurface = cacheEntry.materialSurface;
				admissionVariant.directOnlyAdmission = cacheEntry.directOnlyAdmission;
				admissionVariant.material =
					cacheEntry.lightSurface != nullptr ? cacheEntry.lightSurface->material :
					(cacheEntry.surface != nullptr ? cacheEntry.surface->material : cacheEntry.materialSurface.material);
				EnqueueAdmission(
					admissionVariant,
					!loadingWarmupActive,
					"runtime-actor",
					buildSerial,
					settings,
					loadingTraceLevel,
					voxelStatsEnabled,
					resetServices);
				if (existingActor != nullptr)
				{
					existingActor->active = true;
				}
				if (outDeferredReason != nullptr)
				{
					*outDeferredReason = PersistentVoxelActorDeferredReason::AdmissionPending;
				}
				return true;
			}
		}

		nri_scene::GeometryData actorGeometry;
		{
			PersistentVoxelScopedTimer perfTimer(outStats.geometryBuildPersistentVoxelVariantMs);
			nri_scene::SceneView actorSceneView = buildSingleVoxelSceneView(*cacheEntry.surface);
			nri_scene::BuildGeometry(actorSceneView, actorGeometry);
			batchServices.AssignGeometryPortalIndices(actorGeometry);
		}
		outStats.geometryBuildPersistentVoxelVariantCalls++;
		outStats.geometryBuildPersistentVoxelVariantPrimitives += (uint32_t)actorGeometry.primitives.size();
		if (actorGeometry.primitives.empty() || actorGeometry.indices.empty())
		{
			if (existingActor != nullptr)
			{
				existingActor->active = false;
			}
			return true;
		}
		auto rejectedIt = actorRejectedSignatures.find(cacheEntry.identityKey);
		if (rejectedIt != actorRejectedSignatures.end() && rejectedIt->second == cacheEntry.surfaceSignature)
		{
			if (existingActor != nullptr)
			{
				existingActor->active = false;
			}
			return true;
		}
		if (!ValidateActorGeometry(
			cacheEntry.identityKey,
			cacheEntry.surfaceSignature,
			actorGeometry,
			materialResource.materialCount,
			frameIndex,
			voxelStatsEnabled))
		{
			if (existingActor != nullptr)
			{
				existingActor->active = false;
			}
			return true;
		}

		uint64_t meshResourceKey = baseMeshResourceKey;
		const bool meshResourceTransformKeyed = IsPersistentVoxelMeshResourceTransformKeyed(cacheEntry, settings);
		const bool contentResourceKeyed = !meshResourceTransformKeyed &&
			(cacheEntry.renderPrimitiveHash != 0 || cacheEntry.geometryContentHash != 0);
		auto existingMeshResourceIt = meshVariantResources.find(meshResourceKey);
		if (existingMeshResourceIt != meshVariantResources.end() &&
			(existingMeshResourceIt->second.vertexCount != (uint32_t)actorGeometry.vertices.size() ||
				existingMeshResourceIt->second.indexCount != (uint32_t)actorGeometry.indices.size() ||
				existingMeshResourceIt->second.primitiveCount != (uint32_t)actorGeometry.primitives.size()))
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel mesh variant NRI: frame=%u action=key-split reason=count-mismatch actor_key=0x%llx resource_key=0x%llx mesh_key=0x%llx mat_key=0x%llx ref_count=%u prims=%u vertices=%u indices=%u existing_prims=%u existing_vertices=%u existing_indices=%u space=%s basis_sig=0x%llx transform_keyed=%u blas=%u tlas_ready=%u tlas_published=%u upload_bytes=0 ready=0\n",
					frameIndex,
					(unsigned long long)cacheEntry.identityKey,
					(unsigned long long)meshResourceKey,
					(unsigned long long)cacheEntry.meshKeyHash,
					(unsigned long long)cacheEntry.materialKeyHash,
					countBatchActorsUsingMeshResource(meshResourceKey),
					(uint32_t)actorGeometry.primitives.size(),
					(uint32_t)actorGeometry.vertices.size(),
					(uint32_t)actorGeometry.indices.size(),
					existingMeshResourceIt->second.primitiveCount,
					existingMeshResourceIt->second.vertexCount,
					existingMeshResourceIt->second.indexCount,
					GetPersistentVoxelBakeSpaceName(cacheEntry.meshBakeSpace),
					(unsigned long long)cacheEntry.transformBasisSignature,
					IsPersistentVoxelMeshResourceTransformKeyed(cacheEntry, settings) ? 1u : 0u,
					existingMeshResourceIt->second.accelerationStructure.accelerationStructure != nullptr ? 1u : 0u,
					existingMeshResourceIt->second.tlasReadyFrame,
					existingMeshResourceIt->second.tlasPublished ? 1u : 0u);
			}
			meshResourceKey = nri_scene::HashCombine64(baseMeshResourceKey, cacheEntry.bakedSurfaceSignature);
		}

		PersistentVoxelMeshVariantResource& meshResource = meshVariantResources[meshResourceKey];
		const bool vertexSliceMoved = allocateArenaSlice(
			(uint32_t)actorGeometry.vertices.size(),
			arenaVertexCursor,
			meshResource.vertexOffset,
			meshResource.vertexCapacity);
		const bool indexSliceMoved = allocateArenaSlice(
			(uint32_t)actorGeometry.indices.size(),
			arenaIndexCursor,
			meshResource.indexOffset,
			meshResource.indexCapacity);
		const bool primitiveSliceMoved = allocateArenaSlice(
			(uint32_t)actorGeometry.primitives.size(),
			arenaPrimitiveCursor,
			meshResource.primitiveOffset,
			meshResource.primitiveCapacity);

		const bool meshResourceChanged =
			meshResource.resourceKey != meshResourceKey ||
			(contentResourceKeyed ?
				(meshResource.renderPrimitiveHash != cacheEntry.renderPrimitiveHash ||
					meshResource.geometryContentHash != cacheEntry.geometryContentHash) :
				meshResource.meshKeyHash != cacheEntry.meshKeyHash) ||
			(meshResourceTransformKeyed && meshResource.transformBasisSignature != cacheEntry.transformBasisSignature) ||
			meshResource.meshBakeSpace != cacheEntry.meshBakeSpace ||
			meshResource.vertexCount != (uint32_t)actorGeometry.vertices.size() ||
			meshResource.indexCount != (uint32_t)actorGeometry.indices.size() ||
			meshResource.primitiveCount != (uint32_t)actorGeometry.primitives.size() ||
			meshResource.vertexBuffer.buffer == nullptr ||
			meshResource.indexBuffer.buffer == nullptr;
		const bool sharedArenaChanged =
			meshResourceChanged ||
			vertexSliceMoved ||
			indexSliceMoved ||
			primitiveSliceMoved ||
			vertexBuffer.buffer == nullptr ||
			indexBuffer.buffer == nullptr ||
			primitiveBuffer.buffer == nullptr;
		const bool actorGeometryChanged = sharedArenaChanged;
		if (actorGeometryChanged)
		{
			const uint32_t shaderVertexOffset = meshResource.vertexOffset;
			const uint32_t shaderIndexOffset = meshResource.indexOffset;
			const uint32_t shaderPrimitiveOffset = meshResource.primitiveOffset;
			std::vector<uint32_t> gpuIndices = actorGeometry.indices;
			for (uint32_t& index : gpuIndices)
			{
				index += shaderVertexOffset;
			}

			std::vector<nri_scene::PrimitiveData> gpuPrimitives = actorGeometry.primitives;
			const size_t primitiveCount = std::min(gpuPrimitives.size(), actorGeometry.primitiveProvenance.size());
			for (size_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
			{
				const int32_t chunkIndex = actorGeometry.primitiveProvenance[primitiveIndex].mapChunkIndex;
				gpuPrimitives[primitiveIndex].reserved0 = chunkIndex >= 0 ? (uint32_t)chunkIndex : UINT32_MAX;
			}
			for (size_t primitiveIndex = primitiveCount; primitiveIndex < gpuPrimitives.size(); ++primitiveIndex)
			{
				gpuPrimitives[primitiveIndex].reserved0 = UINT32_MAX;
			}
			for (nri_scene::PrimitiveData& primitive : gpuPrimitives)
			{
				primitive.indices[0] += shaderVertexOffset;
				primitive.indices[1] += shaderVertexOffset;
				primitive.indices[2] += shaderVertexOffset;
			}

			if ((meshResourceChanged &&
					(!batchServices.EnsureStructuredBuffer(
						meshResource.vertexBuffer,
						actorGeometry.vertices.data(),
						actorGeometry.vertices.size() * sizeof(nri_scene::SceneVertex),
						sizeof(nri_scene::SceneVertex),
						PersistentVoxelBufferUsageFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
						PersistentVoxelAccelerationStructureBuildInputAccess(),
						"persistent_voxel_mesh_vertex",
						ResidentUploadKind_Vertex) ||
					!batchServices.EnsureStructuredBuffer(
						meshResource.indexBuffer,
						actorGeometry.indices.data(),
						actorGeometry.indices.size() * sizeof(uint32_t),
						sizeof(uint32_t),
						PersistentVoxelBufferUsageFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
						PersistentVoxelAccelerationStructureBuildInputAccess(),
						"persistent_voxel_mesh_index",
						ResidentUploadKind_Index))) ||
				!batchServices.EnsureArenaBuffer(
					vertexBuffer,
					(uint64_t)arenaVertexCursor * sizeof(nri_scene::SceneVertex),
					sizeof(nri_scene::SceneVertex),
					nri::BufferUsageBits::SHADER_RESOURCE,
					PersistentVoxelComputeShaderResourceAccess()) ||
				!batchServices.EnsureArenaBuffer(
					indexBuffer,
					(uint64_t)arenaIndexCursor * sizeof(uint32_t),
					sizeof(uint32_t),
					nri::BufferUsageBits::SHADER_RESOURCE,
					PersistentVoxelComputeShaderResourceAccess()) ||
				!batchServices.EnsureArenaBuffer(
					primitiveBuffer,
					(uint64_t)arenaPrimitiveCursor * sizeof(nri_scene::PrimitiveData),
					sizeof(nri_scene::PrimitiveData),
					nri::BufferUsageBits::SHADER_RESOURCE,
					PersistentVoxelComputeShaderResourceAccess()))
			{
				Reset("persistent-voxel-buffer-allocation-failed", true, loadingTraceLevel >= 1 || voxelStatsEnabled, resetServices);
				return false;
			}
			if (meshResourceChanged)
			{
				batchServices.NoteBufferUpload(
					ResidentUploadKind_Vertex,
					actorGeometry.vertices.size() * sizeof(nri_scene::SceneVertex),
					"persistent_voxel_mesh_vertex");
				batchServices.NoteBufferUpload(
					ResidentUploadKind_Index,
					actorGeometry.indices.size() * sizeof(uint32_t),
					"persistent_voxel_mesh_index");
			}
			batchServices.NoteBufferUpload(
				ResidentUploadKind_Vertex,
				actorGeometry.vertices.size() * sizeof(nri_scene::SceneVertex),
				"persistent_voxel_mesh_vertex");
			batchServices.NoteBufferUpload(
				ResidentUploadKind_Index,
				gpuIndices.size() * sizeof(uint32_t),
				"persistent_voxel_mesh_index");
			batchServices.NoteBufferUpload(
				ResidentUploadKind_Primitive,
				gpuPrimitives.size() * sizeof(nri_scene::PrimitiveData),
				"persistent_voxel_mesh_primitive");
			if (!batchServices.StageBufferCopyRange(
					vertexBuffer,
					(uint64_t)shaderVertexOffset * sizeof(nri_scene::SceneVertex),
					actorGeometry.vertices.data(),
					actorGeometry.vertices.size() * sizeof(nri_scene::SceneVertex),
					PersistentVoxelComputeShaderResourceAccess(),
					ResidentUploadKind_Vertex) ||
				!batchServices.StageBufferCopyRange(
					indexBuffer,
					(uint64_t)shaderIndexOffset * sizeof(uint32_t),
					gpuIndices.data(),
					gpuIndices.size() * sizeof(uint32_t),
					PersistentVoxelComputeShaderResourceAccess(),
					ResidentUploadKind_Index) ||
				!batchServices.StageBufferCopyRange(
					primitiveBuffer,
					(uint64_t)shaderPrimitiveOffset * sizeof(nri_scene::PrimitiveData),
					gpuPrimitives.data(),
					gpuPrimitives.size() * sizeof(nri_scene::PrimitiveData),
					PersistentVoxelComputeShaderResourceAccess(),
					ResidentUploadKind_Primitive))
			{
				Reset("persistent-voxel-buffer-stage-failed", true, loadingTraceLevel >= 1 || voxelStatsEnabled, resetServices);
				return false;
			}
			if (meshResourceChanged)
			{
				batchServices.RetireAccelerationStructure(meshResource.accelerationStructure);
				RetirePersistentVoxelShadowProxy(meshResource.shadowProxy, resetServices);
				meshResource.resourceKey = meshResourceKey;
				meshResource.sourceModel = cacheEntry.model;
				meshResource.shadowProxyPrimitiveSemanticsCertified =
					CertifyNRIVoxelShadowProxyPrimitiveSemantics(actorGeometry.primitives, materialResource.materialCount);
				meshResource.meshKeyHash = cacheEntry.meshKeyHash;
				meshResource.geometrySignature = ResolvePersistentVoxelCacheEntryGeometrySignature(cacheEntry);
				meshResource.geometryContentHash = cacheEntry.geometryContentHash;
				meshResource.renderPrimitiveHash = cacheEntry.renderPrimitiveHash;
				meshResource.transformBasisSignature = cacheEntry.transformBasisSignature;
				meshResource.meshBakeSpace = cacheEntry.meshBakeSpace;
				meshResource.vertexCount = (uint32_t)actorGeometry.vertices.size();
				meshResource.indexCount = (uint32_t)actorGeometry.indices.size();
				meshResource.primitiveCount = (uint32_t)actorGeometry.primitives.size();
				FillPersistentVoxelMeshBounds(actorGeometry.vertices, meshResource);
				meshResource.bakedTranslation[0] = cacheEntry.bakedTranslation[0];
				meshResource.bakedTranslation[1] = cacheEntry.bakedTranslation[1];
				meshResource.bakedTranslation[2] = cacheEntry.bakedTranslation[2];
				meshResource.lightTemplateValid = false;
				meshResource.tlasReadyFrame = 0;
				meshResource.tlasPublished = false;
			}
			meshResource.lastDesiredMapGeneration = residencyMapGeneration;
			meshResource.lastUsedMapGeneration = residencyMapGeneration;
			meshResource.lastUsedFrame = frameIndex;
			meshResource.residentBytes =
				meshResource.vertexBuffer.memorySize +
				meshResource.indexBuffer.memorySize +
				meshResource.accelerationStructure.memorySize;
			meshResource.cold = false;
			if (voxelStatsEnabled)
			{
				const uint64_t uploadBytes =
					(uint64_t)actorGeometry.vertices.size() * sizeof(nri_scene::SceneVertex) +
					(uint64_t)gpuIndices.size() * sizeof(uint32_t) +
					(uint64_t)gpuPrimitives.size() * sizeof(nri_scene::PrimitiveData);
				Printf("PERF pt voxel mesh variant NRI: frame=%u action=%s reason=none actor_key=0x%llx resource_key=0x%llx mesh_key=0x%llx mat_key=0x%llx ref_count=%u prims=%u vertices=%u indices=%u vertex_offset=%u index_offset=%u primitive_offset=%u space=%s basis_sig=0x%llx transform_keyed=%u blas=%u tlas_ready=%u tlas_published=%u upload_bytes=%llu ready=%u\n",
					frameIndex,
					meshResourceChanged ? "build" : "upload",
					(unsigned long long)cacheEntry.identityKey,
					(unsigned long long)meshResourceKey,
					(unsigned long long)cacheEntry.meshKeyHash,
					(unsigned long long)cacheEntry.materialKeyHash,
					countBatchActorsUsingMeshResource(meshResourceKey),
					meshResource.primitiveCount,
					meshResource.vertexCount,
					meshResource.indexCount,
					meshResource.vertexOffset,
					meshResource.indexOffset,
					meshResource.primitiveOffset,
					GetPersistentVoxelBakeSpaceName(meshResource.meshBakeSpace),
					(unsigned long long)meshResource.transformBasisSignature,
					meshResourceTransformKeyed ? 1u : 0u,
					meshResource.accelerationStructure.accelerationStructure != nullptr ? 1u : 0u,
					meshResource.tlasReadyFrame,
					meshResource.tlasPublished ? 1u : 0u,
					(unsigned long long)uploadBytes,
					(meshResource.vertexBuffer.buffer != nullptr && meshResource.indexBuffer.buffer != nullptr) ||
						meshResource.directComputePublished ? 1u : 0u);
			}
		}

		PersistentVoxelBatch::ActorEntry actor = existingActor != nullptr ? *existingActor : PersistentVoxelBatch::ActorEntry{};
		actor.identityKey = cacheEntry.identityKey;
		actor.actorIndex = cacheEntry.actorIndex;
		actor.indirectOnly = cacheEntry.indirectOnly;
		actor.signature = cacheEntry.signature;
		actor.geometrySignature = ResolvePersistentVoxelCacheEntryGeometrySignature(cacheEntry);
		actor.surfaceSignature = cacheEntry.surfaceSignature;
		actor.bakedSurfaceSignature = cacheEntry.bakedSurfaceSignature;
		actor.materialSignature = cacheEntry.materialSignature;
		actor.meshResourceKey = meshResourceKey;
		actor.meshKeyHash = cacheEntry.meshKeyHash;
		actor.materialKeyHash = cacheEntry.materialKeyHash;
		actor.meshVariantHash = cacheEntry.meshVariantHash;
		actor.materialVariantHash = cacheEntry.materialVariantHash;
		actor.lastSeenFrame = cacheEntry.lastSeenFrame;
		actor.retainedFrameAge = cacheEntry.retainedFrameAge;
		actor.sourcePicnum = cacheEntry.sourcePicnum;
		actor.resolvedVoxelIndex = cacheEntry.resolvedVoxelIndex;
		actor.visibilityChunkIndex = ResolvePersistentVoxelActorVisibilityChunk(cacheEntry);
		actor.capturedThisFrame = cacheEntry.capturedThisFrame;
		actor.active = true;
		FillPersistentVoxelActorInstanceTransform(cacheEntry, meshResource, actor.instanceTransform);
		actor.previousInstanceTransform = actor.instanceTransform;
		actor.primitiveOffset = meshResource.primitiveOffset;
		actor.primitiveCount = (uint32_t)actorGeometry.primitives.size();
		actor.indexOffset = meshResource.indexOffset;
		actor.indexCount = (uint32_t)actorGeometry.indices.size();
		actor.materialOffset = materialResource.materialOffset;
		actor.materialCount = materialResource.materialCount;
		actor.materialSlotGeneration = materialResource.materialSlotGeneration;
		CopyPersistentVoxelActorAuthority(cacheEntry, actor);
		actor.bindingGeneration = BuildPersistentVoxelActorBindingGeneration(actor);
		actor.materialBridge = materialResource.materialBridge;
		rebuildPersistentVoxelActorLightRecords(actor, meshResource);
		auto instanceIt = instances.find(cacheEntry.identityKey);
		if (instanceIt != instances.end())
		{
			actor.previousInstanceTransform = instanceIt->second.previousTransform;
			instanceIt->second.meshResourceKey = meshResourceKey;
			instanceIt->second.currentTransform = actor.instanceTransform;
			instanceIt->second.bindingGeneration = actor.bindingGeneration;
			instanceIt->second.pending = false;
		}

		if (existingActor != nullptr)
		{
			*existingActor = actor;
		}
		else
		{
			batch.actors.push_back(actor);
		}
		return true;
	};

	const uint32_t persistentVoxelBuildActorBudget = runtimeBudget.buildActors;
	const uint32_t persistentVoxelBuildPrimitiveBudget = runtimeBudget.buildPrimitives;
	const uint64_t persistentVoxelBuildByteBudget = runtimeBudget.buildBytes;
	uint32_t persistentVoxelBuiltActors = 0;
	uint32_t persistentVoxelBuiltPrimitives = 0;
	uint64_t persistentVoxelBuiltBytes = 0;
	bool persistentVoxelBuildPending = false;

	auto canBuildPersistentVoxelVariant = [&](uint32_t primitiveCount, uint64_t estimatedBytes) -> bool
	{
		outStats.persistentVoxelOnboardingCandidateCount++;
		outStats.persistentVoxelOnboardingEstimatedBytes += estimatedBytes;
		if (persistentVoxelBuiltActors == 0)
		{
			return true;
		}
		if (persistentVoxelBuildPrimitiveBudget != 0 && primitiveCount > persistentVoxelBuildPrimitiveBudget)
		{
			persistentVoxelBuildPending = true;
			outStats.persistentVoxelOnboardingDeferredCount++;
			outStats.persistentVoxelOnboardingBudgetDeferredCount++;
			outStats.persistentVoxelOnboardingPrimitiveBudgetHits++;
			outStats.persistentVoxelOnboardingDeferredBytes += estimatedBytes;
			return false;
		}
		if (persistentVoxelBuildByteBudget != 0 && estimatedBytes > persistentVoxelBuildByteBudget)
		{
			persistentVoxelBuildPending = true;
			outStats.persistentVoxelOnboardingDeferredCount++;
			outStats.persistentVoxelOnboardingBudgetDeferredCount++;
			outStats.persistentVoxelOnboardingByteBudgetHits++;
			outStats.persistentVoxelOnboardingDeferredBytes += estimatedBytes;
			return false;
		}
		if (persistentVoxelBuiltActors >= persistentVoxelBuildActorBudget)
		{
			persistentVoxelBuildPending = true;
			outStats.persistentVoxelOnboardingDeferredCount++;
			outStats.persistentVoxelOnboardingBudgetDeferredCount++;
			outStats.persistentVoxelOnboardingActorBudgetHits++;
			outStats.persistentVoxelOnboardingDeferredBytes += estimatedBytes;
			return false;
		}
		if (persistentVoxelBuildPrimitiveBudget != 0 &&
			persistentVoxelBuiltPrimitives + primitiveCount > persistentVoxelBuildPrimitiveBudget)
		{
			persistentVoxelBuildPending = true;
			outStats.persistentVoxelOnboardingDeferredCount++;
			outStats.persistentVoxelOnboardingBudgetDeferredCount++;
			outStats.persistentVoxelOnboardingPrimitiveBudgetHits++;
			outStats.persistentVoxelOnboardingDeferredBytes += estimatedBytes;
			return false;
		}
		if (persistentVoxelBuildByteBudget != 0 &&
			persistentVoxelBuiltBytes + estimatedBytes > persistentVoxelBuildByteBudget)
		{
			persistentVoxelBuildPending = true;
			outStats.persistentVoxelOnboardingDeferredCount++;
			outStats.persistentVoxelOnboardingBudgetDeferredCount++;
			outStats.persistentVoxelOnboardingByteBudgetHits++;
			outStats.persistentVoxelOnboardingDeferredBytes += estimatedBytes;
			return false;
		}
		return true;
	};

	auto directOnlyAdmissionBypassesBuildBudget = [](const nri_scene::PersistentVoxelCacheEntryView& cacheEntry) -> bool
	{
		return cacheEntry.directOnlyAdmission && ShouldDirectPublishNRIVoxelComputeMeshing();
	};

	auto notePersistentVoxelActorBuilt = [&](uint32_t primitiveCount, uint64_t estimatedBytes)
	{
		persistentVoxelBuiltActors++;
		persistentVoxelBuiltPrimitives += primitiveCount;
		persistentVoxelBuiltBytes += estimatedBytes;
		outStats.persistentVoxelOnboardingAdmittedCount++;
		outStats.persistentVoxelOnboardingAdmittedBytes += estimatedBytes;
	};
	auto notePersistentVoxelActorDeferred = [&](PersistentVoxelActorDeferredReason reason, uint64_t estimatedBytes)
	{
		switch (reason)
		{
		case PersistentVoxelActorDeferredReason::TexturePrewarm:
			outStats.persistentVoxelOnboardingDeferredCount++;
			outStats.persistentVoxelOnboardingTextureBudgetHits++;
			outStats.persistentVoxelOnboardingTexturePrewarmDeferredCount++;
			outStats.persistentVoxelOnboardingDeferredBytes += estimatedBytes;
			return;
		case PersistentVoxelActorDeferredReason::AdmissionPending:
			outStats.persistentVoxelOnboardingDeferredCount++;
			outStats.persistentVoxelOnboardingAdmissionPendingCount++;
			outStats.persistentVoxelOnboardingDeferredBytes += estimatedBytes;
			return;
		case PersistentVoxelActorDeferredReason::MaterialInvalid:
		case PersistentVoxelActorDeferredReason::None:
			return;
		}
	};
	outStats.persistentVoxelOnboardingByteBudget = persistentVoxelBuildByteBudget;

	auto canReusePersistentVoxelMesh = [&](const nri_scene::PersistentVoxelCacheEntryView& cacheEntry) -> bool
	{
		const uint64_t meshResourceKey = BuildPersistentVoxelMeshResourceKey(cacheEntry, settings);
		auto meshResourceIt = meshVariantResources.find(meshResourceKey);
		return meshResourceIt != meshVariantResources.end() &&
			meshResourceIt->second.resourceKey == meshResourceKey &&
			meshResourceIt->second.vertexCount != 0 &&
			meshResourceIt->second.indexCount != 0 &&
			meshResourceIt->second.primitiveCount != 0 &&
			((meshResourceIt->second.vertexBuffer.buffer != nullptr &&
			  meshResourceIt->second.indexBuffer.buffer != nullptr) ||
			 meshResourceIt->second.directComputePublished) &&
			vertexBuffer.buffer != nullptr &&
			indexBuffer.buffer != nullptr &&
			primitiveBuffer.buffer != nullptr;
	};

	auto canReusePersistentVoxelVariant = [&](const nri_scene::PersistentVoxelCacheEntryView& cacheEntry) -> bool
	{
		auto materialIt = materialVariantResources.find(cacheEntry.materialKeyHash);
		return materialIt != materialVariantResources.end() &&
			materialIt->second.materialCount != 0 &&
			materialIt->second.materialSlotGeneration != 0 &&
			canReusePersistentVoxelMesh(cacheEntry);
	};

	auto noteVoxelPromotionDeferred = [&](uint64_t estimatedBytes)
	{
		voxelPromotionQueued++;
		voxelPromotionSkippedBudget++;
		voxelPromotionUploadBytes += estimatedBytes;
	};

	auto noteVoxelPromotionPromoted = [&](bool reusableVariant, uint64_t estimatedBytes)
	{
		voxelPromotionPromoted++;
		if (reusableVariant)
		{
			voxelPromotionGpuReady++;
		}
		else
		{
			voxelPromotionQueued++;
			voxelPromotionUploadBytes += estimatedBytes;
		}
	};

	auto emitVoxelPromotionTrace = [&]()
	{
		if (voxelStatsEnabled)
		{
			Printf("PERF pt voxel promotion NRI: frame=%u queued=%u promoted=%u skipped_budget=%u cpu_ready=%u gpu_ready=%u bytes=%llu pending=%u actors=%u active=%u runtime_budget=%d actor_budget=%u prim_budget=%u byte_budget=%llu texture_budget=%u texture_bytes=%llu\n",
				frameIndex,
				voxelPromotionQueued,
				voxelPromotionPromoted,
				voxelPromotionSkippedBudget,
				voxelPromotionCpuReady,
				voxelPromotionGpuReady,
				(unsigned long long)voxelPromotionUploadBytes,
				persistentVoxelBuildPending ? 1u : 0u,
				(uint32_t)batch.actors.size(),
				batch.activeActorCount,
				runtimeBudget.mode,
				persistentVoxelBuildActorBudget,
				persistentVoxelBuildPrimitiveBudget,
				(unsigned long long)persistentVoxelBuildByteBudget,
				persistentVoxelTexturePrewarmBudget,
				(unsigned long long)persistentVoxelTexturePrewarmByteBudget);
		}
	};

	if (batch.valid)
	{
		const std::unordered_set<uint64_t> previousActiveMaterialKeys =
			CollectActivePersistentVoxelMaterialKeys(batch);
		batch.actors.reserve(batch.actors.size() + cacheEntries.size());
		std::unordered_map<uint64_t, PersistentVoxelBatch::ActorEntry*> existingActors;
		existingActors.reserve(batch.actors.size());
		std::unordered_set<uint64_t> previouslyActiveActors;
		previouslyActiveActors.reserve(batch.actors.size());
		for (PersistentVoxelBatch::ActorEntry& actor : batch.actors)
		{
			existingActors[actor.identityKey] = &actor;
			if (actor.active)
			{
				previouslyActiveActors.insert(actor.identityKey);
			}
		}

		for (PersistentVoxelBatch::ActorEntry& actor : batch.actors)
		{
			actor.active = false;
			auto instanceIt = instances.find(actor.identityKey);
			if (instanceIt != instances.end())
			{
				instanceIt->second.active = false;
			}
		}

		uint32_t updatedActorCount = 0;
		{
			PersistentVoxelScopedTimer perfTimer(outStats.persistentVoxelBatchActorLoopMs);
			for (const nri_scene::PersistentVoxelCacheEntryView& cacheEntry : cacheEntries)
			{
				auto found = existingActors.find(cacheEntry.identityKey);
				if (!IsPersistentVoxelCacheEntryPublicationCurrent(cacheEntry))
				{
					PersistentVoxelInstanceRecord& instance = instances[cacheEntry.identityKey];
					instance.active = false;
					instance.pending = cacheEntry.desiredPending;
					if (found != existingActors.end())
					{
						PersistentVoxelBatch::ActorEntry& actor = *found->second;
						CopyPersistentVoxelActorAuthority(cacheEntry, actor);
						actor.active = false;
						actor.inWorldTlasThisFrame = false;
						actor.worldTlasPublicationHash = 0;
					}
					continue;
				}
				PersistentVoxelInstanceRecord& currentInstance = instances[cacheEntry.identityKey];
				currentInstance.active = true;
				currentInstance.pending = false;
				CopyPersistentVoxelInstanceAuthority(cacheEntry, currentInstance);
				if (found == existingActors.end())
				{
					const bool reusableVariant = canReusePersistentVoxelVariant(cacheEntry);
					const bool reusableMesh = reusableVariant || canReusePersistentVoxelMesh(cacheEntry);
					const uint64_t estimatedUploadBytes = reusableMesh ? 0ull : EstimatePersistentVoxelActorUploadBytes(cacheEntry);
					const bool bypassBuildBudget = directOnlyAdmissionBypassesBuildBudget(cacheEntry);
					if (!reusableMesh && !bypassBuildBudget && !canBuildPersistentVoxelVariant(cacheEntry.primitiveCount, estimatedUploadBytes))
					{
						instances[cacheEntry.identityKey].active = false;
						instances[cacheEntry.identityKey].pending = true;
						noteVoxelPromotionDeferred(estimatedUploadBytes);
						if (voxelStatsEnabled)
						{
							Printf("PERF pt voxel instance NRI: frame=%u action=defer reason=onboarding-budget actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx surface_sig=0x%llx baked_surface=0x%llx primitive_offset=0 primitive_count=%u index_offset=0 index_count=0 material_offset=0 material_count=0 ready=0 pending=1 active=0\n",
								frameIndex,
								(unsigned long long)cacheEntry.identityKey,
								(unsigned long long)BuildPersistentVoxelMeshResourceKey(cacheEntry, settings),
								(unsigned long long)cacheEntry.meshKeyHash,
								(unsigned long long)cacheEntry.materialKeyHash,
								(unsigned long long)cacheEntry.surfaceSignature,
								(unsigned long long)cacheEntry.bakedSurfaceSignature,
								cacheEntry.primitiveCount);
						}
						continue;
					}
					Clocker geometryClock(NriPTGeometryBuild);
					PersistentVoxelScopedTimer appendTimer(outStats.geometryBuildPersistentVoxelAppendMs);
					PersistentVoxelActorDeferredReason actorDeferredReason = PersistentVoxelActorDeferredReason::None;
					if (!appendActorToBatch(batch, cacheEntry, nullptr, &actorDeferredReason))
					{
						Reset("persistent-voxel-append-failed", true, loadingTraceLevel >= 1 || voxelStatsEnabled, resetServices);
						return false;
					}
					if (actorDeferredReason != PersistentVoxelActorDeferredReason::None)
					{
						instances[cacheEntry.identityKey].active = false;
						instances[cacheEntry.identityKey].pending = true;
						persistentVoxelBuildPending = true;
						noteVoxelPromotionDeferred(estimatedUploadBytes);
						notePersistentVoxelActorDeferred(actorDeferredReason, estimatedUploadBytes);
						if (voxelStatsEnabled)
						{
							Printf("PERF pt voxel instance NRI: frame=%u action=defer reason=%s actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx surface_sig=0x%llx baked_surface=0x%llx primitive_offset=0 primitive_count=%u index_offset=0 index_count=0 material_offset=0 material_count=0 ready=0 pending=1 active=1\n",
								frameIndex,
								actorDeferredReason == PersistentVoxelActorDeferredReason::AdmissionPending ? "admission-pending" : "texture-prewarm",
								(unsigned long long)cacheEntry.identityKey,
								(unsigned long long)BuildPersistentVoxelMeshResourceKey(cacheEntry, settings),
								(unsigned long long)cacheEntry.meshKeyHash,
								(unsigned long long)cacheEntry.materialKeyHash,
								(unsigned long long)cacheEntry.surfaceSignature,
								(unsigned long long)cacheEntry.bakedSurfaceSignature,
								cacheEntry.primitiveCount);
						}
						continue;
					}
					if (!reusableMesh && !bypassBuildBudget)
					{
						notePersistentVoxelActorBuilt(cacheEntry.primitiveCount, estimatedUploadBytes);
					}
					noteVoxelPromotionPromoted(reusableVariant, estimatedUploadBytes);
					updatedActorCount++;
					continue;
				}

				PersistentVoxelBatch::ActorEntry& actor = *found->second;
				auto meshResourceIt = actor.meshResourceKey != 0 ? meshVariantResources.find(actor.meshResourceKey) : meshVariantResources.end();
				auto materialResourceIt = actor.materialKeyHash != 0 ? materialVariantResources.find(actor.materialKeyHash) : materialVariantResources.end();
			std::array<float, 12> expectedInstanceTransform = {};
			CopyPersistentVoxelInstanceTransform(cacheEntry.instanceTransform, expectedInstanceTransform);
			const uint64_t expectedGeometrySignature = ResolvePersistentVoxelCacheEntryGeometrySignature(cacheEntry);
			if (meshResourceIt != meshVariantResources.end())
			{
				FillPersistentVoxelActorInstanceTransform(cacheEntry, meshResourceIt->second, expectedInstanceTransform);
			}
			const bool actorGeometryNeedsUpdate =
				actor.bakedSurfaceSignature != cacheEntry.bakedSurfaceSignature ||
				actor.meshKeyHash != cacheEntry.meshKeyHash ||
				meshResourceIt == meshVariantResources.end() ||
				meshResourceIt->second.accelerationStructure.accelerationStructure == nullptr;
			const bool actorInstanceTransformNeedsUpdate = !SamePersistentVoxelInstanceTransform(actor.instanceTransform, expectedInstanceTransform.data());
			const uint32_t expectedVisibilityChunkIndex = ResolvePersistentVoxelActorVisibilityChunk(cacheEntry);
			const bool actorVisibilityChunkNeedsUpdate = actor.visibilityChunkIndex != expectedVisibilityChunkIndex;
			auto holdPreviousRepresentation = [&]() -> bool
			{
				if (previouslyActiveActors.find(cacheEntry.identityKey) == previouslyActiveActors.end() ||
					actor.ownerWorldEpoch != cacheEntry.ownerWorldEpoch ||
					actor.ownerLifetimeGeneration != cacheEntry.ownerLifetimeGeneration)
				{
					return false;
				}
				const auto heldMeshIt = meshVariantResources.find(actor.meshResourceKey);
				const auto heldMaterialIt = materialVariantResources.find(actor.materialKeyHash);
				if (heldMeshIt == meshVariantResources.end() ||
					heldMeshIt->second.accelerationStructure.accelerationStructure == nullptr ||
					heldMaterialIt == materialVariantResources.end() ||
					!PersistentVoxelMaterialRangeMatches(actor, heldMaterialIt->second))
				{
					return false;
				}

				CopyPersistentVoxelActorAuthority(cacheEntry, actor);
				actor.active = true;
				actor.inWorldTlasThisFrame = false;
				actor.worldTlasInstanceIndex = UINT32_MAX;
				actor.worldTlasOccurrenceGeneration = 0u;
				actor.worldTlasPublicationHash = 0u;
				actor.lastSeenFrame = cacheEntry.lastSeenFrame;
				actor.retainedFrameAge = cacheEntry.retainedFrameAge;
				actor.capturedThisFrame = cacheEntry.capturedThisFrame;
				actor.indirectOnly = cacheEntry.indirectOnly;
				PersistentVoxelInstanceRecord& heldInstance = instances[cacheEntry.identityKey];
				actor.previousInstanceTransform = heldInstance.previousTransform;
				actor.instanceTransform = expectedInstanceTransform;
				actor.visibilityChunkIndex = expectedVisibilityChunkIndex;
				actor.bindingGeneration = BuildPersistentVoxelActorBindingGeneration(actor);
				if (heldMeshIt->second.lightTemplateValid)
				{
					const float lightScale = ResolvePersistentVoxelLightScale(actor.instanceTransform);
					for (SceneLightSystem::SurfaceRecord& record : actor.lightRecords)
					{
						TransformPersistentVoxelLightCenter(
							actor.instanceTransform, heldMeshIt->second.lightTemplateCenter, record.center);
						record.boundsRadius = heldMeshIt->second.lightTemplateBoundsRadius * lightScale;
						record.surfaceArea = heldMeshIt->second.lightTemplateSurfaceArea * lightScale * lightScale;
						record.identityKey = SceneLightSystem::ComputeSurfaceIdentityKey(
							record.source, record.provenance, record.center);
					}
				}

				heldInstance.signature = actor.signature;
				heldInstance.geometrySignature = actor.geometrySignature;
				heldInstance.surfaceSignature = actor.surfaceSignature;
				heldInstance.bakedSurfaceSignature = actor.bakedSurfaceSignature;
				heldInstance.materialSignature = actor.materialSignature;
				heldInstance.meshKeyHash = actor.meshKeyHash;
				heldInstance.materialKeyHash = actor.materialKeyHash;
				heldInstance.meshVariantHash = actor.meshVariantHash;
				heldInstance.materialVariantHash = actor.materialVariantHash;
				heldInstance.meshResourceKey = actor.meshResourceKey;
				heldInstance.primitiveCount = actor.primitiveCount;
				heldInstance.lastSeenFrame = frameIndex;
				heldInstance.bindingGeneration = actor.bindingGeneration;
				heldInstance.currentTransform = actor.instanceTransform;
				heldInstance.active = true;
				heldInstance.pending = true;
				CopyPersistentVoxelInstanceAuthority(cacheEntry, heldInstance);
				return true;
			};
			const bool actorAuthorityNeedsUpdate =
				actor.ownerWorldEpoch != cacheEntry.ownerWorldEpoch ||
				actor.ownerLifetimeGeneration != cacheEntry.ownerLifetimeGeneration ||
				actor.placementGeneration != cacheEntry.placementGeneration ||
				actor.placementStateHash != cacheEntry.placementStateHash ||
				actor.physicalSectorIndex != cacheEntry.physicalSectorIndex ||
				actor.authorityCurrent != cacheEntry.authorityCurrent ||
				actor.publicationEligible != cacheEntry.publicationEligible ||
				actor.pendingRemoval != cacheEntry.pendingRemoval;
			const bool actorNeedsUpdate =
				actor.signature != cacheEntry.signature ||
				actor.geometrySignature != expectedGeometrySignature ||
				actor.surfaceSignature != cacheEntry.surfaceSignature ||
				actor.bakedSurfaceSignature != cacheEntry.bakedSurfaceSignature ||
				actor.materialSignature != cacheEntry.materialSignature ||
				actor.meshKeyHash != cacheEntry.meshKeyHash ||
				actor.materialKeyHash != cacheEntry.materialKeyHash ||
				materialResourceIt == materialVariantResources.end() ||
				!PersistentVoxelMaterialRangeMatches(actor, materialResourceIt->second) ||
				actor.indirectOnly != cacheEntry.indirectOnly ||
				actorInstanceTransformNeedsUpdate ||
				actorVisibilityChunkNeedsUpdate ||
				actorAuthorityNeedsUpdate;
			if (actorNeedsUpdate)
			{
				const bool transformOnlyUpdate =
					!actorGeometryNeedsUpdate &&
					(actorInstanceTransformNeedsUpdate || actorVisibilityChunkNeedsUpdate ||
						actor.indirectOnly != cacheEntry.indirectOnly || actorAuthorityNeedsUpdate) &&
					actor.signature == cacheEntry.signature &&
					actor.geometrySignature == expectedGeometrySignature &&
					actor.materialSignature == cacheEntry.materialSignature &&
					actor.meshKeyHash == cacheEntry.meshKeyHash &&
					actor.materialKeyHash == cacheEntry.materialKeyHash &&
					materialResourceIt != materialVariantResources.end() &&
					PersistentVoxelMaterialRangeMatches(actor, materialResourceIt->second) &&
					meshResourceIt != meshVariantResources.end();
				if (transformOnlyUpdate)
				{
					actor.surfaceSignature = cacheEntry.surfaceSignature;
					actor.lastSeenFrame = cacheEntry.lastSeenFrame;
					actor.retainedFrameAge = cacheEntry.retainedFrameAge;
					actor.sourcePicnum = cacheEntry.sourcePicnum;
					actor.resolvedVoxelIndex = cacheEntry.resolvedVoxelIndex;
					actor.capturedThisFrame = cacheEntry.capturedThisFrame;
					actor.indirectOnly = cacheEntry.indirectOnly;
					actor.instanceTransform = expectedInstanceTransform;
					actor.visibilityChunkIndex = expectedVisibilityChunkIndex;
					CopyPersistentVoxelActorAuthority(cacheEntry, actor);
					actor.bindingGeneration = BuildPersistentVoxelActorBindingGeneration(actor);
					actor.active = true;
					auto instanceIt = instances.find(cacheEntry.identityKey);
					if (instanceIt != instances.end())
					{
						actor.previousInstanceTransform = instanceIt->second.previousTransform;
						instanceIt->second.meshResourceKey = actor.meshResourceKey;
						instanceIt->second.currentTransform = actor.instanceTransform;
						instanceIt->second.bindingGeneration = actor.bindingGeneration;
						instanceIt->second.pending = false;
					}
					if (voxelStatsEnabled)
					{
						Printf("PERF pt voxel instance NRI: frame=%u action=transform reason=none actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx surface_sig=0x%llx baked_surface=0x%llx primitive_offset=%u primitive_count=%u index_offset=%u index_count=%u material_offset=%u material_count=%u ready=1 pending=0 active=1\n",
							frameIndex,
							(unsigned long long)cacheEntry.identityKey,
							(unsigned long long)actor.meshResourceKey,
							(unsigned long long)actor.meshKeyHash,
							(unsigned long long)actor.materialKeyHash,
							(unsigned long long)cacheEntry.surfaceSignature,
							(unsigned long long)cacheEntry.bakedSurfaceSignature,
							actor.primitiveOffset,
							actor.primitiveCount,
							actor.indexOffset,
							actor.indexCount,
							actor.materialOffset,
							actor.materialCount);
					}
					outStats.persistentVoxelInstanceTransformUpdates++;
					updatedActorCount++;
					continue;
				}

				const bool genuineVariantChange =
					actor.meshVariantHash != cacheEntry.meshVariantHash ||
					actor.materialVariantHash != cacheEntry.materialVariantHash;
				const int syntheticVariantDelayFrames =
					std::clamp((int)nri_ptvoxelvariantdelayframes, 0, 4096);
				if (genuineVariantChange &&
					actor.actorIndex == (int)nri_ptvoxelvariantdelayactor &&
					syntheticVariantDelayFrames > 0 &&
					holdPreviousRepresentation())
				{
					nri_ptvoxelvariantdelayframes = syntheticVariantDelayFrames - 1;
					persistentVoxelBuildPending = true;
					Printf("PERF pt voxel instance NRI: frame=%u action=hold reason=synthetic-variant-delay actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx surface_sig=0x%llx baked_surface=0x%llx primitive_offset=%u primitive_count=%u index_offset=%u index_count=%u material_offset=%u material_count=%u ready=1 pending=1 active=1\n",
						frameIndex,
						(unsigned long long)cacheEntry.identityKey,
						(unsigned long long)actor.meshResourceKey,
						(unsigned long long)actor.meshKeyHash,
						(unsigned long long)actor.materialKeyHash,
						(unsigned long long)actor.surfaceSignature,
						(unsigned long long)actor.bakedSurfaceSignature,
						actor.primitiveOffset,
						actor.primitiveCount,
						actor.indexOffset,
						actor.indexCount,
						actor.materialOffset,
						actor.materialCount);
					continue;
				}

				const bool reusableVariant = actorGeometryNeedsUpdate && canReusePersistentVoxelVariant(cacheEntry);
				const bool reusableMesh = actorGeometryNeedsUpdate && (reusableVariant || canReusePersistentVoxelMesh(cacheEntry));
				const uint64_t estimatedUploadBytes = actorGeometryNeedsUpdate && !reusableMesh ? EstimatePersistentVoxelActorUploadBytes(cacheEntry) : 0ull;
				const bool bypassBuildBudget = directOnlyAdmissionBypassesBuildBudget(cacheEntry);
				if (actorGeometryNeedsUpdate && !reusableMesh && !bypassBuildBudget && !canBuildPersistentVoxelVariant(cacheEntry.primitiveCount, estimatedUploadBytes))
				{
					const bool heldPrevious = holdPreviousRepresentation();
					if (!heldPrevious)
					{
						CopyPersistentVoxelActorAuthority(cacheEntry, actor);
						actor.active = false;
						actor.inWorldTlasThisFrame = false;
						actor.worldTlasPublicationHash = 0;
						instances[cacheEntry.identityKey].active = false;
						instances[cacheEntry.identityKey].pending = true;
					}
					noteVoxelPromotionDeferred(estimatedUploadBytes);
					if (voxelStatsEnabled)
					{
						Printf("PERF pt voxel instance NRI: frame=%u action=%s reason=onboarding-budget actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx surface_sig=0x%llx baked_surface=0x%llx primitive_offset=%u primitive_count=%u index_offset=%u index_count=%u material_offset=%u material_count=%u ready=%u pending=1 active=%u\n",
							frameIndex,
							heldPrevious ? "hold" : "defer",
							(unsigned long long)cacheEntry.identityKey,
							(unsigned long long)actor.meshResourceKey,
							(unsigned long long)cacheEntry.meshKeyHash,
							(unsigned long long)cacheEntry.materialKeyHash,
							(unsigned long long)cacheEntry.surfaceSignature,
							(unsigned long long)cacheEntry.bakedSurfaceSignature,
							actor.primitiveOffset,
							cacheEntry.primitiveCount,
							actor.indexOffset,
							actor.indexCount,
							actor.materialOffset,
							actor.materialCount,
							heldPrevious ? 1u : 0u,
							heldPrevious ? 1u : 0u);
					}
					continue;
				}
				Clocker geometryClock(NriPTGeometryBuild);
				PersistentVoxelScopedTimer perfTimer(outStats.geometryBuildPersistentVoxelAppendMs);
				PersistentVoxelActorDeferredReason actorDeferredReason = PersistentVoxelActorDeferredReason::None;
				if (!appendActorToBatch(batch, cacheEntry, &actor, &actorDeferredReason))
				{
					Reset("persistent-voxel-update-failed", true, loadingTraceLevel >= 1 || voxelStatsEnabled, resetServices);
					return false;
				}
				if (actorDeferredReason != PersistentVoxelActorDeferredReason::None)
				{
					const bool heldPrevious = holdPreviousRepresentation();
					if (!heldPrevious)
					{
						CopyPersistentVoxelActorAuthority(cacheEntry, actor);
						actor.active = false;
						actor.inWorldTlasThisFrame = false;
						actor.worldTlasPublicationHash = 0;
						instances[cacheEntry.identityKey].active = false;
						instances[cacheEntry.identityKey].pending = true;
					}
					persistentVoxelBuildPending = true;
					noteVoxelPromotionDeferred(estimatedUploadBytes);
					notePersistentVoxelActorDeferred(actorDeferredReason, estimatedUploadBytes);
					if (voxelStatsEnabled)
					{
						Printf("PERF pt voxel instance NRI: frame=%u action=%s reason=%s actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx surface_sig=0x%llx baked_surface=0x%llx primitive_offset=%u primitive_count=%u index_offset=%u index_count=%u material_offset=%u material_count=%u ready=%u pending=1 active=%u\n",
							frameIndex,
							heldPrevious ? "hold" : "defer",
							actorDeferredReason == PersistentVoxelActorDeferredReason::AdmissionPending ? "admission-pending" : "texture-prewarm",
							(unsigned long long)cacheEntry.identityKey,
							(unsigned long long)actor.meshResourceKey,
							(unsigned long long)cacheEntry.meshKeyHash,
							(unsigned long long)cacheEntry.materialKeyHash,
							(unsigned long long)cacheEntry.surfaceSignature,
							(unsigned long long)cacheEntry.bakedSurfaceSignature,
							actor.primitiveOffset,
							cacheEntry.primitiveCount,
							actor.indexOffset,
							actor.indexCount,
							actor.materialOffset,
							actor.materialCount,
							heldPrevious ? 1u : 0u,
							heldPrevious ? 1u : 0u);
					}
					continue;
				}
				if (actorGeometryNeedsUpdate && !reusableMesh && !bypassBuildBudget)
				{
					notePersistentVoxelActorBuilt(cacheEntry.primitiveCount, estimatedUploadBytes);
				}
				noteVoxelPromotionPromoted(reusableVariant, estimatedUploadBytes);
				updatedActorCount++;
				continue;
			}

			actor.active = true;
			actor.lastSeenFrame = cacheEntry.lastSeenFrame;
			actor.retainedFrameAge = cacheEntry.retainedFrameAge;
			actor.sourcePicnum = cacheEntry.sourcePicnum;
			actor.resolvedVoxelIndex = cacheEntry.resolvedVoxelIndex;
			actor.capturedThisFrame = cacheEntry.capturedThisFrame;
			actor.indirectOnly = cacheEntry.indirectOnly;
			actor.visibilityChunkIndex = ResolvePersistentVoxelActorVisibilityChunk(cacheEntry);
			CopyPersistentVoxelActorAuthority(cacheEntry, actor);
			actor.bindingGeneration = BuildPersistentVoxelActorBindingGeneration(actor);
			auto instanceIt = instances.find(cacheEntry.identityKey);
			if (instanceIt != instances.end())
			{
				actor.previousInstanceTransform = instanceIt->second.previousTransform;
				instanceIt->second.meshResourceKey = actor.meshResourceKey;
				instanceIt->second.currentTransform = actor.instanceTransform;
				instanceIt->second.bindingGeneration = actor.bindingGeneration;
				instanceIt->second.pending = false;
			}
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel instance NRI: frame=%u action=hit reason=none actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx surface_sig=0x%llx baked_surface=0x%llx primitive_offset=%u primitive_count=%u index_offset=%u index_count=%u material_offset=%u material_count=%u ready=1 pending=0 active=1\n",
					frameIndex,
					(unsigned long long)cacheEntry.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					(unsigned long long)actor.materialKeyHash,
					(unsigned long long)cacheEntry.surfaceSignature,
					(unsigned long long)cacheEntry.bakedSurfaceSignature,
					actor.primitiveOffset,
					actor.primitiveCount,
					actor.indexOffset,
					actor.indexCount,
					actor.materialOffset,
					actor.materialCount);
			}
			}
			for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
			{
				if (!actor.active)
				{
					if (voxelStatsEnabled)
					{
						Printf("PERF pt voxel instance NRI: frame=%u action=remove reason=not-captured actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx surface_sig=0x%llx baked_surface=0x%llx primitive_offset=%u primitive_count=%u index_offset=%u index_count=%u material_offset=%u material_count=%u ready=0 pending=0 active=0\n",
							frameIndex,
							(unsigned long long)actor.identityKey,
							(unsigned long long)actor.meshResourceKey,
							(unsigned long long)actor.meshKeyHash,
							(unsigned long long)actor.materialKeyHash,
							(unsigned long long)actor.surfaceSignature,
							(unsigned long long)actor.bakedSurfaceSignature,
							actor.primitiveOffset,
							actor.primitiveCount,
							actor.indexOffset,
							actor.indexCount,
							actor.materialOffset,
							actor.materialCount);
					}
					break;
				}
			}
		}
		const std::unordered_set<uint64_t> currentActiveMaterialKeys =
			CollectActivePersistentVoxelMaterialKeys(batch);
		if (batchMaterialResourceGeneration != materialResourceGeneration ||
			previousActiveMaterialKeys != currentActiveMaterialKeys)
		{
			PersistentVoxelScopedTimer perfTimer(outStats.persistentVoxelBatchMaterialBridgeMs);
			RebuildBatchMaterialBridge(batch);
		}
		if (!persistentVoxelBuildPending)
		{
			batch.sourceSerial = cacheSerial;
		}
		if (updatedActorCount != 0)
		{
			batch.rebuildCount++;
		}
		{
			PersistentVoxelScopedTimer perfTimer(outStats.persistentVoxelBatchStateMs);
			RecomputeBatchState(batch);
		}
		RefreshActiveResourceReferences(frameIndex);
		emitVoxelPromotionTrace();
		return batch.valid;
	}

	PersistentVoxelBatch next = {};
	next.sourceSerial = cacheSerial;
	next.actors.reserve(cacheEntries.size());

	{
		Clocker geometryClock(NriPTGeometryBuild);
		PersistentVoxelScopedTimer perfTimer(outStats.geometryBuildPersistentVoxelRebuildMs);
		PersistentVoxelScopedTimer actorLoopTimer(outStats.persistentVoxelBatchActorLoopMs);
		for (const nri_scene::PersistentVoxelCacheEntryView& cacheEntry : cacheEntries)
		{
			if (!IsPersistentVoxelCacheEntryPublicationCurrent(cacheEntry))
			{
				instances[cacheEntry.identityKey].active = false;
				instances[cacheEntry.identityKey].pending = cacheEntry.desiredPending;
				continue;
			}
			const bool reusableVariant = canReusePersistentVoxelVariant(cacheEntry);
			const bool reusableMesh = reusableVariant || canReusePersistentVoxelMesh(cacheEntry);
			const uint64_t estimatedUploadBytes = reusableMesh ? 0ull : EstimatePersistentVoxelActorUploadBytes(cacheEntry);
			const bool bypassBuildBudget = directOnlyAdmissionBypassesBuildBudget(cacheEntry);
			if (!reusableMesh && !bypassBuildBudget && !canBuildPersistentVoxelVariant(cacheEntry.primitiveCount, estimatedUploadBytes))
			{
				instances[cacheEntry.identityKey].active = false;
				instances[cacheEntry.identityKey].pending = true;
				noteVoxelPromotionDeferred(estimatedUploadBytes);
				if (voxelStatsEnabled)
				{
					Printf("PERF pt voxel instance NRI: frame=%u action=defer reason=onboarding-budget actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx surface_sig=0x%llx baked_surface=0x%llx primitive_offset=0 primitive_count=%u index_offset=0 index_count=0 material_offset=0 material_count=0 ready=0 pending=1 active=0\n",
						frameIndex,
						(unsigned long long)cacheEntry.identityKey,
						(unsigned long long)BuildPersistentVoxelMeshResourceKey(cacheEntry, settings),
						(unsigned long long)cacheEntry.meshKeyHash,
						(unsigned long long)cacheEntry.materialKeyHash,
						(unsigned long long)cacheEntry.surfaceSignature,
						(unsigned long long)cacheEntry.bakedSurfaceSignature,
						cacheEntry.primitiveCount);
				}
				continue;
			}
			PersistentVoxelActorDeferredReason actorDeferredReason = PersistentVoxelActorDeferredReason::None;
			if (!appendActorToBatch(next, cacheEntry, nullptr, &actorDeferredReason))
			{
				Reset("persistent-voxel-new-actor-failed", true, loadingTraceLevel >= 1 || voxelStatsEnabled, resetServices);
				return false;
			}
			if (actorDeferredReason != PersistentVoxelActorDeferredReason::None)
			{
				instances[cacheEntry.identityKey].active = false;
				instances[cacheEntry.identityKey].pending = true;
				persistentVoxelBuildPending = true;
				noteVoxelPromotionDeferred(estimatedUploadBytes);
				notePersistentVoxelActorDeferred(actorDeferredReason, estimatedUploadBytes);
				if (voxelStatsEnabled)
				{
					Printf("PERF pt voxel instance NRI: frame=%u action=defer reason=%s actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx surface_sig=0x%llx baked_surface=0x%llx primitive_offset=0 primitive_count=%u index_offset=0 index_count=0 material_offset=0 material_count=0 ready=0 pending=1 active=0\n",
						frameIndex,
						actorDeferredReason == PersistentVoxelActorDeferredReason::AdmissionPending ? "admission-pending" : "texture-prewarm",
						(unsigned long long)cacheEntry.identityKey,
						(unsigned long long)BuildPersistentVoxelMeshResourceKey(cacheEntry, settings),
						(unsigned long long)cacheEntry.meshKeyHash,
						(unsigned long long)cacheEntry.materialKeyHash,
						(unsigned long long)cacheEntry.surfaceSignature,
						(unsigned long long)cacheEntry.bakedSurfaceSignature,
						cacheEntry.primitiveCount);
				}
				continue;
			}
			if (!reusableMesh && !bypassBuildBudget)
			{
				notePersistentVoxelActorBuilt(cacheEntry.primitiveCount, estimatedUploadBytes);
			}
			noteVoxelPromotionPromoted(reusableVariant, estimatedUploadBytes);
		}
	}

	if (persistentVoxelBuildPending)
	{
		next.sourceSerial = 0;
	}
	next.rebuildCount = batch.rebuildCount + 1u;
	{
		PersistentVoxelScopedTimer perfTimer(outStats.persistentVoxelBatchMaterialBridgeMs);
		RebuildBatchMaterialBridge(next);
	}
	{
		PersistentVoxelScopedTimer perfTimer(outStats.persistentVoxelBatchStateMs);
		RecomputeBatchState(next);
	}
	if (!next.valid)
	{
		if (persistentVoxelBuildPending && next.actors.empty())
		{
			if (loadingTraceLevel >= 1 || voxelStatsEnabled)
			{
				Printf("NRI PT persistent voxel batch: event=defer reason=all-actors-pending entries=%u instances=%u mesh_resources=%u material_resources=%u\n",
					(uint32_t)cacheEntries.size(),
					(uint32_t)instances.size(),
					(uint32_t)meshVariantResources.size(),
					(uint32_t)materialVariantResources.size());
			}
			emitVoxelPromotionTrace();
			return false;
		}
		Reset("persistent-voxel-invalid-instance-batch", false, loadingTraceLevel >= 1 || voxelStatsEnabled, resetServices);
		return false;
	}

	batch = std::move(next);
	RefreshActiveResourceReferences(frameIndex);
	emitVoxelPromotionTrace();
	return true;
}


bool NRIPersistentVoxelResidency::BuildAccelerationStructures(
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& resetServices,
	const NRIPersistentVoxelAccelerationServices& accelerationServices,
	NRIPersistentVoxelAccelerationBuildStats& outStats)
{
	outStats.calls++;
	sharedBlasCache.BeginFrame();
	if (!batch.valid || batch.actors.empty())
	{
		Reset("persistent-voxel-empty-instance-batch", false, voxelStatsEnabled, resetServices);
		return true;
	}

	std::unordered_set<uint64_t> builtMeshKeys;
	builtMeshKeys.reserve(batch.actors.size());
	std::unordered_set<uint64_t> consideredSharedBlasKeys;
	consideredSharedBlasKeys.reserve(batch.actors.size());
	uint32_t sharedBlasBuildsThisFrame = 0;
	auto countActiveActorsUsingMeshResource = [&](uint64_t meshResourceKey) -> uint32_t
	{
		uint32_t count = 0;
		for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
		{
			if (actor.active && actor.meshResourceKey == meshResourceKey)
			{
				count++;
			}
		}
		return count;
	};
	auto maybeBuildSharedBlas = [&](const PersistentVoxelBatch::ActorEntry& actor, PersistentVoxelMeshVariantResource& meshResource) -> void
	{
		if (actor.meshResourceKey == 0 || !consideredSharedBlasKeys.insert(actor.meshResourceKey).second)
		{
			return;
		}
		(void)BuildOrReusePersistentVoxelSharedBlas(
			actor.meshResourceKey,
			actor.geometrySignature,
			meshResource,
			frameIndex,
			settings,
			sharedBlasBuildsThisFrame,
			sharedBlasCache,
			resetServices,
			accelerationServices);
	};
	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (actor.active)
		{
			outStats.instances++;
		}
	}

	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (!actor.active)
		{
			continue;
		}

		auto meshResourceIt = meshVariantResources.find(actor.meshResourceKey);
		if (meshResourceIt == meshVariantResources.end())
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel blas NRI: frame=%u action=skip reason=missing-mesh actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx ref_count=0 prims=%u vertices=0 indices=%u blas=0 tlas_ready=0 tlas_published=0 ready=0\n",
					frameIndex,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					actor.primitiveCount,
					actor.indexCount);
			}
			continue;
		}
		PersistentVoxelMeshVariantResource& meshResource = meshResourceIt->second;
		if (meshResource.directComputePublished)
		{
			const bool directReady =
				meshResource.accelerationStructure.accelerationStructure != nullptr &&
				vertexBuffer.buffer != nullptr &&
				indexBuffer.buffer != nullptr &&
				primitiveBuffer.buffer != nullptr;
			if (directReady)
			{
				if (!meshResource.tlasPublished && meshResource.tlasReadyFrame == 0)
				{
					meshResource.tlasReadyFrame = frameIndex;
				}
				if (voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
				{
					Printf("PERF pt voxel blas NRI: frame=%u action=reuse reason=direct-generated actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx ref_count=%u prims=%u vertices=%u indices=%u blas=1 tlas_ready=%u tlas_published=%u ready=1\n",
						frameIndex,
						(unsigned long long)actor.identityKey,
						(unsigned long long)actor.meshResourceKey,
						(unsigned long long)actor.meshKeyHash,
						countActiveActorsUsingMeshResource(actor.meshResourceKey),
						meshResource.primitiveCount,
						meshResource.vertexCount,
						meshResource.indexCount,
						meshResource.tlasReadyFrame,
						meshResource.tlasPublished ? 1u : 0u);
				}
			}
			else if (voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
			{
				Printf("PERF pt voxel blas NRI: frame=%u action=skip reason=direct-generated-incomplete actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx ref_count=%u prims=%u vertices=%u indices=%u blas=%u tlas_ready=%u tlas_published=%u ready=0\n",
					frameIndex,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					countActiveActorsUsingMeshResource(actor.meshResourceKey),
					meshResource.primitiveCount,
					meshResource.vertexCount,
					meshResource.indexCount,
					meshResource.accelerationStructure.accelerationStructure != nullptr ? 1u : 0u,
					meshResource.tlasReadyFrame,
					meshResource.tlasPublished ? 1u : 0u);
			}
			continue;
		}
		const bool needsBuild =
			meshResource.accelerationStructure.accelerationStructure == nullptr ||
			meshResource.vertexBuffer.buffer == nullptr ||
			meshResource.indexBuffer.buffer == nullptr;
		if (!needsBuild)
		{
			maybeBuildSharedBlas(actor, meshResource);
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel blas NRI: frame=%u action=reuse reason=none actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx ref_count=%u prims=%u vertices=%u indices=%u blas=1 tlas_ready=%u tlas_published=%u ready=1\n",
					frameIndex,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					countActiveActorsUsingMeshResource(actor.meshResourceKey),
					meshResource.primitiveCount,
					meshResource.vertexCount,
					meshResource.indexCount,
					meshResource.tlasReadyFrame,
					meshResource.tlasPublished ? 1u : 0u);
			}
			continue;
		}

		outStats.builds++;
		if (actor.meshKeyHash != 0)
		{
			builtMeshKeys.insert(actor.meshKeyHash);
		}
		if (!accelerationServices.BuildBottomLevel(
			meshResource.vertexBuffer,
			meshResource.indexBuffer,
			0u,
			meshResource.vertexCount,
			0u,
			meshResource.indexCount,
			meshResource.primitiveCount,
			meshResource.accelerationStructure))
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel blas NRI: frame=%u action=failed reason=build actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx ref_count=%u prims=%u vertices=%u indices=%u blas=0 tlas_ready=%u tlas_published=%u ready=0\n",
					frameIndex,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					countActiveActorsUsingMeshResource(actor.meshResourceKey),
					meshResource.primitiveCount,
					meshResource.vertexCount,
					meshResource.indexCount,
					meshResource.tlasReadyFrame,
					meshResource.tlasPublished ? 1u : 0u);
			}
			return false;
		}

		if (!accelerationServices.BarrierBuildInputs(meshResource.vertexBuffer, meshResource.indexBuffer))
		{
			return false;
		}

		if (!meshResource.tlasPublished && meshResource.tlasReadyFrame == 0)
		{
			meshResource.tlasReadyFrame = loadingWarmupActive ? frameIndex : frameIndex + 1u;
		}
		if (voxelStatsEnabled)
		{
			Printf("PERF pt voxel blas NRI: frame=%u action=build reason=none actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx ref_count=%u prims=%u vertices=%u indices=%u blas=1 tlas_ready=%u tlas_published=%u ready=1\n",
				frameIndex,
				(unsigned long long)actor.identityKey,
				(unsigned long long)actor.meshResourceKey,
				(unsigned long long)actor.meshKeyHash,
				countActiveActorsUsingMeshResource(actor.meshResourceKey),
				meshResource.primitiveCount,
				meshResource.vertexCount,
				meshResource.indexCount,
				meshResource.tlasReadyFrame,
				meshResource.tlasPublished ? 1u : 0u);
		}
		maybeBuildSharedBlas(actor, meshResource);
	}

	uint32_t shadowProxyBuildsThisFrame = 0;
	std::unordered_set<uint64_t> consideredShadowProxyKeys;
	consideredShadowProxyKeys.reserve(batch.actors.size());
	if (settings.shadowProxyBuildEnabled && settings.shadowProxyBuildsPerFrame != 0u)
	{
		for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
		{
			if (!actor.active || actor.meshResourceKey == 0)
			{
				continue;
			}
			auto meshIt = meshVariantResources.find(actor.meshResourceKey);
			if (meshIt == meshVariantResources.end()) continue;
			PersistentVoxelMeshVariantResource& mesh = meshIt->second;
			NRIVoxelShadowProxyResource& proxy = mesh.shadowProxy;
			if (proxy.state == NRIVoxelShadowProxyResourceState::Resident ||
				proxy.state == NRIVoxelShadowProxyResourceState::Failed)
			{
				continue;
			}
			NRIVoxelShadowProxyRejectReason materialReason = NRIVoxelShadowProxyRejectReason::None;
			const bool occurrenceCertified = CertifyNRIVoxelShadowProxyMaterialClosure(
				actor.materialBridge, !actor.lightRecords.empty(), materialReason);
			if (!occurrenceCertified)
			{
				outStats.shadowProxy.materialRejects++;
				continue;
			}
			if (mesh.meshBakeSpace != nri_scene::VoxelMeshBakeSpace::LocalSpace ||
				!mesh.shadowProxyPrimitiveSemanticsCertified || mesh.sourceModel == nullptr)
			{
				outStats.shadowProxy.geometryRejects++;
				continue;
			}
			if (mesh.accelerationStructure.accelerationStructure == nullptr)
			{
				outStats.shadowProxy.resourceRejects++;
				continue;
			}
			if (!consideredShadowProxyKeys.insert(actor.meshResourceKey).second)
			{
				continue;
			}
			outStats.shadowProxy.candidates++;
			if (shadowProxyBuildsThisFrame >= settings.shadowProxyBuildsPerFrame)
			{
				continue;
			}
			if (proxy.firstRequestFrame == UINT32_MAX) proxy.firstRequestFrame = frameIndex;

			NRIVoxelComputeRawSourceArchiveSnapshot source = {};
			if (!CopyNRIVoxelComputeRawSourceArchiveSnapshot(mesh.sourceModel, source))
			{
				outStats.shadowProxy.archiveMisses++;
				continue; // Archive production is owned by ordinary voxel loading; never re-decode here.
			}
			outStats.shadowProxy.archiveHits++;
			if (source.exactPrimitiveCount != mesh.primitiveCount ||
				(mesh.directComputeSourceArchiveSerial != 0 && source.recordSerial != mesh.directComputeSourceArchiveSerial))
			{
				proxy.state = NRIVoxelShadowProxyResourceState::Failed;
				proxy.rejectReason = NRIVoxelShadowProxyRejectReason::ArchiveMismatch;
				proxy.failedFrame = frameIndex;
				outStats.shadowProxy.failures++;
				continue;
			}

			NRIVoxelShadowProxyCpuGeometry cpuGeometry = {};
			NRIVoxelShadowProxyRejectReason buildReason = NRIVoxelShadowProxyRejectReason::None;
			const auto cpuStart = std::chrono::steady_clock::now();
			const bool cpuReady = BuildNRIVoxelShadowProxyGeometry(
				source, NRIVoxelShadowProxyBuildLimits{}, cpuGeometry, buildReason);
			outStats.shadowProxy.cpuBuildMs += std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - cpuStart).count();
			outStats.shadowProxy.temporaryMaskCells += cpuGeometry.temporaryMaskCells;
			if (!cpuReady || !mesh.boundsValid || !cpuGeometry.boundsValid ||
				!IsNRIVoxelShadowProxyBoundsEquivalent(
					mesh.boundsMin, mesh.boundsMax, cpuGeometry.boundsMin, cpuGeometry.boundsMax))
			{
				proxy.state = NRIVoxelShadowProxyResourceState::Failed;
				proxy.rejectReason = cpuReady ? NRIVoxelShadowProxyRejectReason::BoundsMismatch : buildReason;
				proxy.failedFrame = frameIndex;
				outStats.shadowProxy.failures++;
				continue;
			}

			outStats.shadowProxy.cpuBuilds++;
			outStats.shadowProxy.exactPrimitives += mesh.primitiveCount;
			outStats.shadowProxy.proxyPrimitives += cpuGeometry.proxyPrimitiveCount;
			const uint64_t vertexBytes = cpuGeometry.vertices.size() * sizeof(NRIVoxelShadowProxyVertex);
			const uint64_t indexBytes = cpuGeometry.indices.size() * sizeof(uint32_t);
			if (!accelerationServices.EnsureStructuredBuffer(
					proxy.vertexBuffer, cpuGeometry.vertices.data(), vertexBytes,
					sizeof(NRIVoxelShadowProxyVertex),
					PersistentVoxelBufferUsageFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
					PersistentVoxelAccelerationStructureBuildInputAccess(),
					"persistent_voxel_shadow_proxy_vertex", ResidentUploadKind_Vertex) ||
				!accelerationServices.EnsureStructuredBuffer(
					proxy.indexBuffer, cpuGeometry.indices.data(), indexBytes,
					sizeof(uint32_t),
					PersistentVoxelBufferUsageFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
					PersistentVoxelAccelerationStructureBuildInputAccess(),
					"persistent_voxel_shadow_proxy_index", ResidentUploadKind_Index) ||
				!accelerationServices.BuildBottomLevel(
					proxy.vertexBuffer, proxy.indexBuffer, 0u,
					(uint32_t)cpuGeometry.vertices.size(), 0u,
					(uint32_t)cpuGeometry.indices.size(), cpuGeometry.proxyPrimitiveCount,
					proxy.accelerationStructure) ||
				!accelerationServices.BarrierBuildInputs(proxy.vertexBuffer, proxy.indexBuffer))
			{
				RetirePersistentVoxelShadowProxy(proxy, resetServices);
				proxy.state = NRIVoxelShadowProxyResourceState::Failed;
				proxy.rejectReason = NRIVoxelShadowProxyRejectReason::ResourceUnavailable;
				proxy.failedFrame = frameIndex;
				outStats.shadowProxy.failures++;
				continue;
			}

			proxy.sourceModel = mesh.sourceModel;
			proxy.sourceArchiveSerial = source.recordSerial;
			proxy.sourceContentHash = source.contentHash;
			proxy.geometrySignature = mesh.geometrySignature;
			proxy.exactPrimitiveCount = mesh.primitiveCount;
			proxy.proxyPrimitiveCount = cpuGeometry.proxyPrimitiveCount;
			proxy.vertexCount = (uint32_t)cpuGeometry.vertices.size();
			proxy.indexCount = (uint32_t)cpuGeometry.indices.size();
			proxy.readyFrame = loadingWarmupActive ? frameIndex : frameIndex + 1u;
			proxy.state = NRIVoxelShadowProxyResourceState::Resident;
			proxy.rejectReason = NRIVoxelShadowProxyRejectReason::None;
			proxy.residentBytes = proxy.vertexBuffer.memorySize + proxy.indexBuffer.memorySize + proxy.accelerationStructure.memorySize;
			mesh.residentBytes += proxy.residentBytes;
			outStats.shadowProxy.uploads++;
			outStats.shadowProxy.blasBuilds++;
			outStats.shadowProxy.uploadBytes += vertexBytes + indexBytes;
			shadowProxyBuildsThisFrame++;
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel shadow proxy NRI: frame=%u action=resident mesh_resource=0x%llx exact_prims=%u proxy_prims=%u saved_prims=%u archive_serial=%llu upload_bytes=%llu as_bytes=%llu ready_frame=%u\n",
					frameIndex,
					(unsigned long long)actor.meshResourceKey,
					mesh.primitiveCount,
					proxy.proxyPrimitiveCount,
					mesh.primitiveCount - proxy.proxyPrimitiveCount,
					(unsigned long long)proxy.sourceArchiveSerial,
					(unsigned long long)(vertexBytes + indexBytes),
					(unsigned long long)proxy.accelerationStructure.memorySize,
					proxy.readyFrame);
			}
		}
	}

	outStats.uniqueMeshBuilds += (uint32_t)builtMeshKeys.size();
	if (settings.diagnosticsEnabled)
	{
		uint32_t residentProxyResources = 0;
		uint32_t failedProxyResources = 0;
		uint64_t residentProxyPrimitives = 0;
		uint64_t residentProxyBytes = 0;
		for (const auto& meshPair : meshVariantResources)
		{
			const NRIVoxelShadowProxyResource& proxy = meshPair.second.shadowProxy;
			residentProxyResources += proxy.state == NRIVoxelShadowProxyResourceState::Resident ? 1u : 0u;
			failedProxyResources += proxy.state == NRIVoxelShadowProxyResourceState::Failed ? 1u : 0u;
			if (proxy.state == NRIVoxelShadowProxyResourceState::Resident)
			{
				residentProxyPrimitives += proxy.proxyPrimitiveCount;
				residentProxyBytes += proxy.residentBytes;
			}
		}
		Printf("PERF pt voxel shadow proxy build NRI: frame=%u enabled=%u candidates=%u archive_hits=%u archive_misses=%u cpu_builds=%u uploads=%u upload_bytes=%llu blas_builds=%u failures=%u reject_material=%u reject_geometry=%u reject_resource=%u exact_prims=%llu proxy_prims=%llu temp_cells=%llu cpu_ms=%.3f resident=%u resident_prims=%llu resident_bytes=%llu failed_resident=%u\n",
			frameIndex, settings.shadowProxyBuildEnabled ? 1u : 0u,
			outStats.shadowProxy.candidates, outStats.shadowProxy.archiveHits,
			outStats.shadowProxy.archiveMisses, outStats.shadowProxy.cpuBuilds,
			outStats.shadowProxy.uploads, (unsigned long long)outStats.shadowProxy.uploadBytes,
			outStats.shadowProxy.blasBuilds, outStats.shadowProxy.failures,
			outStats.shadowProxy.materialRejects, outStats.shadowProxy.geometryRejects,
			outStats.shadowProxy.resourceRejects,
			(unsigned long long)outStats.shadowProxy.exactPrimitives,
			(unsigned long long)outStats.shadowProxy.proxyPrimitives,
			(unsigned long long)outStats.shadowProxy.temporaryMaskCells,
			outStats.shadowProxy.cpuBuildMs, residentProxyResources,
			(unsigned long long)residentProxyPrimitives,
			(unsigned long long)residentProxyBytes, failedProxyResources);
	}
	if (outStats.builds != 0 || sharedBlasBuildsThisFrame != 0 || shadowProxyBuildsThisFrame != 0)
	{
		MarkMaintenanceMutation();
	}
	return true;
}

bool NRIPersistentVoxelResidency::WarmSharedBlasForLoading(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& resetServices,
	const NRIPersistentVoxelAccelerationServices& accelerationServices)
{
	if (!settings.sharedBlasBuildEnabled ||
		!settings.sharedBlasLoadingWarmupEnabled ||
		settings.sharedBlasBuildsPerFrame == 0 ||
		variants.empty())
	{
		return true;
	}

	uint32_t consideredActors = batch.valid ? batch.activeActorCount : 0;
	uint32_t consideredKeys = 0;
	uint32_t residentBefore = 0;
	uint32_t built = 0;
	uint32_t reusedResident = 0;
	uint32_t rejectedMissingMesh = 0;
	uint32_t rejectedNonLocal = 0;
	uint32_t rejectedTransformKeyed = 0;
	uint32_t rejectedMissingBuffers = 0;
	uint32_t rejectedInvalidCounts = 0;
	uint32_t rejectedGeometryMismatch = 0;
	uint32_t rejectedBudget = 0;
	std::unordered_set<uint64_t> consideredSharedBlasKeys;
	consideredSharedBlasKeys.reserve(variants.size() + (batch.valid ? batch.actors.size() : 0u));
	for (const auto& pair : meshVariantResources)
	{
		const NRIPersistentVoxelSharedBlasEntry* entry = sharedBlasCache.Find(pair.first);
		if (entry != nullptr && entry->state == NRIPersistentVoxelSharedBlasState::Resident)
		{
			residentBefore++;
		}
	}

	auto warmSharedMesh = [&](uint64_t meshResourceKey, uint64_t geometrySignature) -> void
	{
		if (meshResourceKey == 0 || !consideredSharedBlasKeys.insert(meshResourceKey).second)
		{
			return;
		}
		consideredKeys++;

		auto meshResourceIt = meshVariantResources.find(meshResourceKey);
		if (meshResourceIt == meshVariantResources.end())
		{
			rejectedMissingMesh++;
			sharedBlasCache.RecordBuildReject(meshResourceKey, "missing-buffers");
			return;
		}
		PersistentVoxelMeshVariantResource& meshResource = meshResourceIt->second;
		const nri_scene::VoxelMeshBakeSpace meshBakeSpace = meshResource.meshBakeSpace;
		const bool transformKeyed = settings.transformKeyed;
		const bool missingBuffers = meshResource.vertexBuffer.buffer == nullptr || meshResource.indexBuffer.buffer == nullptr;
		const bool invalidCounts = meshResource.vertexCount == 0 || meshResource.indexCount == 0 || meshResource.primitiveCount == 0;
		const NRIPersistentVoxelSharedBlasEntry* existingSharedEntry = sharedBlasCache.Find(meshResourceKey);
		const bool geometryMismatch =
			existingSharedEntry != nullptr &&
			existingSharedEntry->state == NRIPersistentVoxelSharedBlasState::Resident &&
			(existingSharedEntry->geometrySignature != (geometrySignature != 0 ? geometrySignature : (meshResource.geometrySignature != 0 ? meshResource.geometrySignature : meshResource.meshKeyHash)) ||
				existingSharedEntry->vertexCount != meshResource.vertexCount ||
				existingSharedEntry->indexCount != meshResource.indexCount ||
				existingSharedEntry->primitiveCount != meshResource.primitiveCount);

		const PersistentVoxelSharedBlasBuildResult result = BuildOrReusePersistentVoxelSharedBlas(
			meshResourceKey,
			geometrySignature,
			meshResource,
			frameIndex,
			settings,
			built,
			sharedBlasCache,
			resetServices,
			accelerationServices);
		switch (result)
		{
		case PersistentVoxelSharedBlasBuildResult::Reused:
			reusedResident++;
			break;
		case PersistentVoxelSharedBlasBuildResult::Rejected:
			if (meshBakeSpace != nri_scene::VoxelMeshBakeSpace::LocalSpace)
			{
				rejectedNonLocal++;
			}
			else if (transformKeyed)
			{
				rejectedTransformKeyed++;
			}
			else if (missingBuffers)
			{
				rejectedMissingBuffers++;
			}
			else if (invalidCounts)
			{
				rejectedInvalidCounts++;
			}
			else if (geometryMismatch)
			{
				rejectedGeometryMismatch++;
			}
			else
			{
				rejectedBudget++;
			}
			break;
		default:
			break;
		}
	};

	if (batch.valid && !batch.actors.empty())
	{
		for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
		{
			if (actor.active)
			{
				warmSharedMesh(actor.meshResourceKey, actor.geometrySignature);
			}
		}
	}
	for (const nri_scene::PrecachedVoxelVariantView& variant : variants)
	{
		if (!variant.preloadGeometry)
		{
			continue;
		}
		warmSharedMesh(BuildPersistentVoxelVariantMeshResourceKey(variant), ResolvePersistentVoxelVariantGeometrySignature(variant));
	}

	if (voxelStatsEnabled || loadingTraceLevel >= 1)
	{
		uint32_t residentAfter = 0;
		for (const auto& pair : meshVariantResources)
		{
			const NRIPersistentVoxelSharedBlasEntry* entry = sharedBlasCache.Find(pair.first);
			if (entry != nullptr && entry->state == NRIPersistentVoxelSharedBlasState::Resident)
			{
				residentAfter++;
			}
		}
		Printf("NRI PT loading voxel shared blas: event=warmup active_actors=%u unique_keys=%u resident_before=%u resident_after=%u built=%u reused_resident=%u reject_missing_mesh=%u reject_non_local=%u reject_transform_keyed=%u reject_missing_buffers=%u reject_invalid_counts=%u reject_geometry_mismatch=%u reject_budget=%u\n",
			consideredActors,
			consideredKeys,
			residentBefore,
			residentAfter,
			built,
			reusedResident,
			rejectedMissingMesh,
			rejectedNonLocal,
			rejectedTransformKeyed,
			rejectedMissingBuffers,
			rejectedInvalidCounts,
			rejectedGeometryMismatch,
			rejectedBudget);
	}
	return true;
}

void NRIPersistentVoxelResidency::Reset(
	const char* reason,
	bool clearSharedResources,
	bool traceReset,
	const NRIPersistentVoxelResetServices& services)
{
	MarkMaintenanceMutation();
	if (traceReset &&
		(!batch.actors.empty() ||
			!instances.empty() ||
			!meshVariantResources.empty() ||
			!materialVariantResources.empty() ||
			vertexBuffer.buffer != nullptr ||
			indexBuffer.buffer != nullptr ||
			primitiveBuffer.buffer != nullptr))
	{
		Printf("NRI PT voxel reset: action=%s reason=%s actors=%u instances=%u mesh_resources=%u material_resources=%u arena_vertex=%u arena_index=%u arena_primitive=%u published_mesh=%u published_material=%u\n",
			clearSharedResources ? "clear-shared" : "clear-instances",
			reason != nullptr ? reason : "unknown",
			(uint32_t)batch.actors.size(),
			(uint32_t)instances.size(),
			(uint32_t)meshVariantResources.size(),
			(uint32_t)materialVariantResources.size(),
			vertexBuffer.buffer != nullptr ? 1u : 0u,
			indexBuffer.buffer != nullptr ? 1u : 0u,
			primitiveBuffer.buffer != nullptr ? 1u : 0u,
			(uint32_t)publishedMeshKeys.size(),
			(uint32_t)publishedMaterialKeys.size());
	}

	batch = {};
	actorOccurrenceLedger.Reset();
	actorOccurrencePolicyFrameIndex = UINT32_MAX;
	actorOccurrencePolicyDecisions.clear();
	suppressedActorIndices.clear();
	RefreshActiveResourceReferences(0);
	instances.clear();
	actorRejectedSignatures.clear();
	lastDesiredResidencyCount = 0;
	lastDesiredPreloadCount = 0;
	lastDesiredActorCount = 0;
	lastCpuReadyCount = 0;
	lastGpuReadyCount = 0;
	lastRetainedCount = 0;
	lastQueuedCount = 0;
	lastQueuedUploadBytes = 0;
	lastMeshReadyCount = 0;
	lastMaterialReadyCount = 0;
	lastBlasReadyCount = 0;
	lastMeshMissingCount = 0;
	lastMaterialOnlyCount = 0;
	lastBlasOnlyCount = 0;
	lastColdMeshCount = 0;
	lastColdMaterialCount = 0;
	lastColdPrimitiveCount = 0;
	lastForcedCount = 0;
	lastPreferredCount = 0;
	cumulativeCpuGeometryBuildCount = 0;
	cumulativeCpuGeometryUploadCount = 0;
	cumulativeCpuGeometryUploadBytes = 0;
	cumulativeCpuGeometryFallbackCount = 0;
	postLoadAdmissionGraceEndFrame = 0;
	postLoadAdmissionGraceMapGeneration = 0;
	committedWorldTlasFrameIndex = UINT32_MAX;
	services.InvalidateSceneDataDescriptors();
	if (!clearSharedResources)
	{
		sharedBlasCache.BeginFrame();
		return;
	}

	services.RetireBuffer(vertexBuffer);
	services.RetireBuffer(indexBuffer);
	services.RetireBuffer(primitiveBuffer);
	services.RetireBuffer(materialBuffer);
	arenaVertexCursor = 0;
	arenaIndexCursor = 0;
	arenaPrimitiveCursor = 0;
	materialRangeAllocator.Reset();
	materialRangeCompactions = 0;
	materialRangeCompactedRows = 0;
	arenaPresizeBuildSerial = 0;
	blasPolicyTraceBuildSerial = 0;
	for (auto& pair : meshVariantResources)
	{
		services.RetireBuffer(pair.second.vertexBuffer);
		services.RetireBuffer(pair.second.indexBuffer);
		services.RetireAccelerationStructure(pair.second.accelerationStructure);
		RetirePersistentVoxelShadowProxy(pair.second.shadowProxy, services);
	}
	sharedBlasCache.RetireAll([&](NRIAccelerationStructureResource& resource)
	{
		services.RetireAccelerationStructure(resource);
	});
	meshVariantResources.clear();
	materialVariantResources.clear();
	dirtyMaterialResourceKeys.clear();
	pendingMaterialLayoutInvalidatedResources = 0;
	pendingMaterialActorRebinds = 0;
	uploadedMaterialTextureKeys.clear();
	materialResourceGeneration++;
	batchMaterialResourceGeneration = 0;
	batchMaterialPublicationGeneration++;
	uploadedMaterialResourceGeneration = 0;
	uploadedMaterialPublicationGeneration = 0;
	publishedMeshKeys.clear();
	publishedMaterialKeys.clear();
	RebuildAdmissionIndex(true);
}

void NRIPersistentVoxelResidency::ResetLevelSchedulingState(
	const char* reason,
	bool traceReset,
	const NRIPersistentVoxelResetServices& services)
{
	MarkMaintenanceMutation();
	const uint32_t actorCount = (uint32_t)batch.actors.size();
	const uint32_t instanceCount = (uint32_t)instances.size();
	const uint32_t rejectedActorCount = (uint32_t)actorRejectedSignatures.size();
	const uint32_t admissionCount = (uint32_t)admissionQueue.size();
	if (traceReset && (actorCount != 0 || instanceCount != 0 || rejectedActorCount != 0 || admissionCount != 0 || preloadPending))
	{
		const NRIPersistentVoxelMemoryUsage memoryUsage = GetMemoryUsage();
		Printf("NRI PT voxel level scheduling reset: reason=%s actors=%u active=%u instances=%u rejected=%u admissions=%u preload_pending=%u mesh_resources=%u material_resources=%u scene_buffer_bytes=%llu as_bytes=%llu\n",
			reason != nullptr ? reason : "unknown",
			actorCount,
			batch.activeActorCount,
			instanceCount,
			rejectedActorCount,
			admissionCount,
			preloadPending ? 1u : 0u,
			(uint32_t)meshVariantResources.size(),
			(uint32_t)materialVariantResources.size(),
			(unsigned long long)memoryUsage.sceneBufferBytes,
			(unsigned long long)memoryUsage.accelerationStructureBytes);
	}

	for (auto& pair : admissionQueue)
	{
		DiscardAdmissionEntry(pair.second, services);
	}
	admissionQueue.clear();
	admissionIndex.Clear();

	batch = {};
	RefreshActiveResourceReferences(0);
	instances.clear();
	actorRejectedSignatures.clear();
	preloadPending = false;
	preloadMaterialClosureCached = false;
	preloadMaterialClosureBuildSerial = 0;
	preloadMaterialClosureSignature = 0;
	preloadMaterialClosureCacheHits = 0;
	lastPreloadStatus = {};
	postLoadAdmissionGraceEndFrame = 0;
	postLoadAdmissionGraceMapGeneration = 0;
	lastDesiredResidencyCount = 0;
	lastDesiredPreloadCount = 0;
	lastDesiredActorCount = 0;
	lastCpuReadyCount = 0;
	lastGpuReadyCount = 0;
	lastRetainedCount = 0;
	lastQueuedCount = 0;
	lastQueuedUploadBytes = 0;
	lastMeshReadyCount = 0;
	lastMaterialReadyCount = 0;
	lastBlasReadyCount = 0;
	lastMeshMissingCount = 0;
	lastMaterialOnlyCount = 0;
	lastBlasOnlyCount = 0;
	lastColdMeshCount = 0;
	lastColdMaterialCount = 0;
	lastColdPrimitiveCount = 0;
	lastForcedCount = 0;
	lastPreferredCount = 0;
	cumulativeCpuGeometryBuildCount = 0;
	cumulativeCpuGeometryUploadCount = 0;
	cumulativeCpuGeometryUploadBytes = 0;
	cumulativeCpuGeometryFallbackCount = 0;
	services.InvalidateSceneDataDescriptors();
}

bool NRIPersistentVoxelResidency::CompactMaterialRangesForQuiescentLevelTransition(
	const char* reason,
	bool traceEnabled)
{
	const NRIPersistentVoxelMaterialRangeStats before = materialRangeAllocator.Stats();
	if (before.holeRows == 0 || materialVariantResources.empty())
	{
		return false;
	}
	if (admissionIndex.HasActiveWork() ||
		!PersistentVoxelAdmissionSchedulerQuiescent(admissionScheduler.GetSnapshot()) ||
		!batch.actors.empty() || !instances.empty())
	{
		if (traceEnabled)
		{
			Printf("NRI PT voxel material compaction: reason=%s action=defer cursor_rows=%llu live_rows=%llu hole_rows=%llu\n",
				reason != nullptr ? reason : "unknown",
				(unsigned long long)before.cursorRows,
				(unsigned long long)before.liveRows,
				(unsigned long long)before.holeRows);
		}
		return false;
	}

	std::vector<PersistentVoxelMaterialVariantResource*> resources;
	resources.reserve(materialVariantResources.size());
	for (auto& pair : materialVariantResources)
	{
		if (!materialRangeAllocator.Owns(PersistentVoxelMaterialRangeHandle(pair.second)))
		{
			return false;
		}
		resources.push_back(&pair.second);
	}
	std::sort(resources.begin(), resources.end(), [](const auto* left, const auto* right)
	{
		if (left->materialOffset != right->materialOffset)
			return left->materialOffset < right->materialOffset;
		return left->materialKeyHash < right->materialKeyHash;
	});

	for (auto it = resources.rbegin(); it != resources.rend(); ++it)
	{
		if (!materialRangeAllocator.Release(PersistentVoxelMaterialRangeHandle(**it)))
			return false;
	}
	uint64_t movedRows = 0;
	for (PersistentVoxelMaterialVariantResource* resource : resources)
	{
		const uint32_t oldOffset = resource->materialOffset;
		const NRIPersistentVoxelMaterialRangeHandle replacement =
			materialRangeAllocator.Allocate(resource->materialCapacity);
		if (!replacement)
			return false;
		if (replacement.offset != oldOffset)
			movedRows += resource->materialCapacity;
		resource->materialOffset = replacement.offset;
		resource->materialSlotGeneration = replacement.generation;
		resource->materialUploadHash = 0;
		dirtyMaterialResourceKeys.insert(resource->materialKeyHash);
		publishedMaterialKeys.insert(resource->materialKeyHash);
	}

	materialResourceGeneration++;
	RebuildBatchMaterialBridge(batch);
	materialRangeCompactions++;
	materialRangeCompactedRows += movedRows;
	MarkMaintenanceMutation();
	if (traceEnabled || movedRows != 0)
	{
		const auto& after = materialRangeAllocator.Stats();
		Printf("NRI PT voxel material compaction: reason=%s action=compact resources=%u moved_rows=%llu cursor_before=%llu cursor_after=%llu holes_before=%llu holes_after=%llu compactions=%llu compacted_rows=%llu\n",
			reason != nullptr ? reason : "unknown",
			(uint32_t)resources.size(),
			(unsigned long long)movedRows,
			(unsigned long long)before.cursorRows,
			(unsigned long long)after.cursorRows,
			(unsigned long long)before.holeRows,
			(unsigned long long)after.holeRows,
			(unsigned long long)materialRangeCompactions,
			(unsigned long long)materialRangeCompactedRows);
	}
	return true;
}

bool NRIPersistentVoxelResidency::SyncMapGeneration(
	uint64_t buildSerial,
	const char* reason,
	bool traceEnabled,
	const NRIPersistentVoxelResetServices& services)
{
	if (residencyLastBuildSerial == buildSerial)
	{
		return false;
	}

	residencyLastBuildSerial = buildSerial;
	residencyMapGeneration++;
	MarkMaintenanceMutation();
	admissionIndex.Clear();
	postLoadAdmissionGraceEndFrame = 0;
	postLoadAdmissionGraceMapGeneration = 0;

	if (!admissionQueue.empty())
	{
		if (traceEnabled)
		{
			Printf("NRI PT voxel admission queue: event=clear-stale reason=%s generation=%u entries=%u\n",
				reason != nullptr ? reason : "map-generation",
				residencyMapGeneration,
				(uint32_t)admissionQueue.size());
		}
		for (auto& pair : admissionQueue)
		{
			DiscardAdmissionEntry(pair.second, services);
		}
		admissionQueue.clear();
	}
	return true;
}

void NRIPersistentVoxelResidency::ReconcileResidency(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	const std::vector<nri_scene::PersistentVoxelCacheEntryView>& cacheEntries,
	uint64_t buildSerial,
	const char* levelName,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	int loadingTraceLevel,
	const NRIPersistentVoxelResetServices& services)
{
	MarkMaintenanceMutation();
	SyncMapGeneration(
		buildSerial,
		"reconcile-map-generation",
		loadingTraceLevel >= 1,
		services);
	const uint32_t generation = residencyMapGeneration;

	struct DesiredVoxelResidency
	{
		uint64_t pairKey = 0;
		uint64_t meshKey = 0;
		uint64_t materialKey = 0;
		uint32_t sourceBits = 0;
		int32_t priority = 0;
		int32_t sourcePicnum = -1;
		int32_t resolvedVoxelIndex = -1;
		uint32_t primitiveCount = 0;
		uint64_t uploadBytes = 0;
		bool fromPreload = false;
		bool fromActor = false;
		bool gpuForce = false;
		bool gpuPrefer = false;
		bool cpuReady = false;
	};

	std::unordered_map<uint64_t, DesiredVoxelResidency> desired;
	desired.reserve(variants.size() + cacheEntries.size());
	std::unordered_set<uint64_t> desiredMeshes;
	std::unordered_set<uint64_t> desiredMaterials;
	desiredMeshes.reserve(variants.size() + cacheEntries.size());
	desiredMaterials.reserve(variants.size() + cacheEntries.size());

	auto estimateUploadBytes = [](const nri_scene::SurfaceRef* surface, uint32_t primitiveCount, bool directOnlyAdmission = false) -> uint64_t
	{
		if (surface == nullptr)
		{
			if (directOnlyAdmission && primitiveCount != 0)
			{
				return (uint64_t)primitiveCount * 2ull * (uint64_t)sizeof(nri_scene::SceneVertex) +
					(uint64_t)primitiveCount * 3ull * (uint64_t)sizeof(uint32_t) +
					(uint64_t)primitiveCount * (uint64_t)sizeof(nri_scene::PrimitiveData);
			}
			return 0;
		}
		return (uint64_t)surface->vertices.size() * (uint64_t)sizeof(nri_scene::SceneVertex) +
			(uint64_t)surface->indices.size() * (uint64_t)sizeof(uint32_t) +
			(uint64_t)primitiveCount * (uint64_t)sizeof(nri_scene::PrimitiveData);
	};

	auto addDesired = [&](uint64_t meshKey, uint64_t materialKey, uint32_t sourceBits, int32_t priority, int32_t sourcePicnum, int32_t resolvedVoxelIndex, uint32_t primitiveCount, uint64_t uploadBytes, bool fromPreload, bool fromActor, bool gpuForce, bool gpuPrefer, bool cpuReady)
	{
		if (meshKey == 0 || materialKey == 0)
		{
			return;
		}
		const uint64_t pairKey = nri_scene::HashCombine64(meshKey, materialKey);
		DesiredVoxelResidency& entry = desired[pairKey];
		if (entry.pairKey == 0)
		{
			entry.pairKey = pairKey;
			entry.meshKey = meshKey;
			entry.materialKey = materialKey;
			entry.sourcePicnum = sourcePicnum;
			entry.resolvedVoxelIndex = resolvedVoxelIndex;
			entry.primitiveCount = primitiveCount;
			entry.uploadBytes = uploadBytes;
			entry.priority = priority;
		}
		entry.sourceBits |= sourceBits;
		entry.fromPreload = entry.fromPreload || fromPreload;
		entry.fromActor = entry.fromActor || fromActor;
		entry.gpuForce = entry.gpuForce || gpuForce;
		entry.gpuPrefer = entry.gpuPrefer || gpuPrefer;
		entry.cpuReady = entry.cpuReady || cpuReady;
		if (entry.primitiveCount == 0 && primitiveCount != 0)
		{
			entry.primitiveCount = primitiveCount;
		}
		if (entry.uploadBytes == 0 && uploadBytes != 0)
		{
			entry.uploadBytes = uploadBytes;
		}
		desiredMeshes.insert(meshKey);
		desiredMaterials.insert(materialKey);
	};

	for (const nri_scene::PrecachedVoxelVariantView& variant : variants)
	{
		addDesired(
			BuildPersistentVoxelVariantMeshResourceKey(variant),
			variant.materialKeyHash,
			variant.sourceBits,
			variant.priority,
			variant.sourcePicnum,
			variant.resolvedVoxelIndex,
			variant.primitiveCount,
			estimateUploadBytes(variant.surface, variant.primitiveCount, variant.directOnlyAdmission),
			true,
			false,
			variant.gpuForce,
			variant.gpuPrefer,
			(variant.surface != nullptr || variant.directOnlyAdmission) && variant.primitiveCount != 0);
	}

	for (const nri_scene::PersistentVoxelCacheEntryView& cacheEntry : cacheEntries)
	{
		addDesired(
			BuildPersistentVoxelMeshResourceKey(cacheEntry, settings),
			cacheEntry.materialKeyHash,
			0,
			0,
			cacheEntry.sourcePicnum,
			cacheEntry.resolvedVoxelIndex,
			cacheEntry.primitiveCount,
			estimateUploadBytes(cacheEntry.surface, cacheEntry.primitiveCount, cacheEntry.directOnlyAdmission),
			false,
			true,
			false,
			false,
			(cacheEntry.surface != nullptr || cacheEntry.directOnlyAdmission) && cacheEntry.primitiveCount != 0);
	}

	for (auto it = admissionQueue.begin(); it != admissionQueue.end(); )
	{
		if (it->second.mapGeneration != generation && desired.find(it->first) == desired.end())
		{
			admissionIndex.Remove(it->first, GetAdmissionBucket(it->second));
			DiscardAdmissionEntry(it->second, services);
			it = admissionQueue.erase(it);
			continue;
		}
		++it;
	}

	uint32_t desiredPreload = 0;
	uint32_t desiredActors = 0;
	uint32_t cpuReady = 0;
	uint32_t gpuReady = 0;
	uint32_t queued = 0;
	uint32_t retained = 0;
	uint32_t forceCount = 0;
	uint32_t preferCount = 0;
	uint32_t meshReadyCount = 0;
	uint32_t materialReadyCount = 0;
	uint32_t blasReadyCount = 0;
	uint32_t materialOnlyCount = 0;
	uint32_t blasOnlyCount = 0;
	uint32_t meshMissingCount = 0;
	uint64_t queuedUploadBytes = 0;

	auto meshReady = [&](uint64_t meshKey, const PersistentVoxelMeshVariantResource** outResource = nullptr) -> bool
	{
		auto it = meshVariantResources.find(meshKey);
		if (it == meshVariantResources.end())
		{
			return false;
		}
		const PersistentVoxelMeshVariantResource& resource = it->second;
		if (outResource != nullptr)
		{
			*outResource = &resource;
		}
		return resource.resourceKey == meshKey &&
			resource.vertexCount != 0 &&
			resource.indexCount != 0 &&
			resource.primitiveCount != 0 &&
			((resource.vertexBuffer.buffer != nullptr &&
			  resource.indexBuffer.buffer != nullptr) ||
			 resource.directComputePublished) &&
			vertexBuffer.buffer != nullptr &&
			indexBuffer.buffer != nullptr &&
			primitiveBuffer.buffer != nullptr;
	};

	auto materialReady = [&](uint64_t materialKey) -> bool
	{
		auto it = materialVariantResources.find(materialKey);
		return it != materialVariantResources.end() &&
			it->second.materialKeyHash == materialKey &&
			it->second.materialCount != 0 &&
			it->second.materialSlotGeneration != 0 &&
			!it->second.materialBridge.materials.empty();
	};

	auto blasReady = [&](uint64_t meshKey) -> bool
	{
		auto it = meshVariantResources.find(meshKey);
		return it != meshVariantResources.end() &&
			it->second.accelerationStructure.accelerationStructure != nullptr;
	};

	for (const auto& desiredPair : desired)
	{
		const DesiredVoxelResidency& entry = desiredPair.second;
		if (entry.fromPreload)
		{
			desiredPreload++;
		}
		if (entry.fromActor)
		{
			desiredActors++;
		}
		if (entry.gpuForce)
		{
			forceCount++;
		}
		if (entry.gpuPrefer)
		{
			preferCount++;
		}
		if (entry.cpuReady)
		{
			cpuReady++;
		}

		const bool hasMesh = meshReady(entry.meshKey);
		const bool hasMaterial = materialReady(entry.materialKey);
		const bool hasBlas = blasReady(entry.meshKey);
		if (hasMesh)
		{
			meshReadyCount++;
		}
		if (hasMaterial)
		{
			materialReadyCount++;
		}
		if (hasBlas)
		{
			blasReadyCount++;
		}

		auto meshIt = meshVariantResources.find(entry.meshKey);
		if (meshIt != meshVariantResources.end())
		{
			meshIt->second.lastDesiredMapGeneration = generation;
			meshIt->second.lastUsedMapGeneration = generation;
			meshIt->second.lastUsedFrame = frameIndex;
			meshIt->second.sourceBits |= entry.sourceBits;
			meshIt->second.priority = entry.priority;
			meshIt->second.gpuForce = meshIt->second.gpuForce || entry.gpuForce;
			meshIt->second.gpuPrefer = meshIt->second.gpuPrefer || entry.gpuPrefer;
			meshIt->second.cold = false;
		}
		auto materialIt = materialVariantResources.find(entry.materialKey);
		if (materialIt != materialVariantResources.end())
		{
			materialIt->second.lastDesiredMapGeneration = generation;
			materialIt->second.lastUsedMapGeneration = generation;
			materialIt->second.lastUsedFrame = frameIndex;
			materialIt->second.sourceBits |= entry.sourceBits;
			materialIt->second.priority = entry.priority;
			materialIt->second.gpuForce = materialIt->second.gpuForce || entry.gpuForce;
			materialIt->second.gpuPrefer = materialIt->second.gpuPrefer || entry.gpuPrefer;
			materialIt->second.cold = false;
		}

		const bool ready = hasMesh && hasMaterial && hasBlas;
		if (ready)
		{
			gpuReady++;
			retained++;
		}
		else if (entry.cpuReady)
		{
			queued++;
			queuedUploadBytes += entry.uploadBytes;
			if (!hasMesh)
			{
				meshMissingCount++;
			}
			else if (!hasMaterial)
			{
				materialOnlyCount++;
			}
			else if (!hasBlas)
			{
				blasOnlyCount++;
			}
		}

		if (loadingTraceLevel >= 2)
		{
			const char* source =
				entry.fromPreload && entry.fromActor ? "preload|actor" :
				(entry.fromPreload ? "preload" : "actor");
			const char* action = ready ? "ready" : (entry.cpuReady ? "queue" : "missing-cpu");
			const char* reason =
				ready ? "resident" :
				(!entry.cpuReady ? "cpu-missing" :
				(!hasMesh ? "mesh-missing" :
				(!hasMaterial ? "material-missing" :
				(!hasBlas ? "blas-missing" : "unknown"))));
			Printf("NRI PT voxel residency entry: action=%s reason=%s source=%s source_bits=0x%x priority=%d force=%u prefer=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu mesh_ready=%u material_ready=%u blas_ready=%u generation=%u\n",
				action,
				reason,
				source,
				entry.sourceBits,
				entry.priority,
				entry.gpuForce ? 1u : 0u,
				entry.gpuPrefer ? 1u : 0u,
				entry.sourcePicnum,
				entry.resolvedVoxelIndex,
				(unsigned long long)entry.meshKey,
				(unsigned long long)entry.materialKey,
				entry.primitiveCount,
				(unsigned long long)entry.uploadBytes,
				hasMesh ? 1u : 0u,
				hasMaterial ? 1u : 0u,
				hasBlas ? 1u : 0u,
				generation);
		}
	}

	uint32_t coldMeshes = 0;
	uint32_t coldMaterials = 0;
	uint64_t coldPrimitiveCount = 0;
	for (auto& pair : meshVariantResources)
	{
		PersistentVoxelMeshVariantResource& resource = pair.second;
		if (resource.resourceKey == 0 || desiredMeshes.find(pair.first) != desiredMeshes.end())
		{
			continue;
		}
		resource.cold = true;
		coldMeshes++;
		coldPrimitiveCount += resource.primitiveCount;
		if (loadingTraceLevel >= 2)
		{
			Printf("NRI PT voxel residency entry: action=cold reason=map-not-desired source=mesh source_bits=0x0 priority=0 force=0 prefer=0 tex=-1 voxel=-1 mesh_variant=0x%llx mat_variant=0x0 prims=%u bytes=0 mesh_ready=%u material_ready=0 blas_ready=%u generation=%u last_desired=%u\n",
				(unsigned long long)pair.first,
				resource.primitiveCount,
				meshReady(pair.first) ? 1u : 0u,
				blasReady(pair.first) ? 1u : 0u,
				generation,
				resource.lastDesiredMapGeneration);
		}
	}
	for (auto& pair : materialVariantResources)
	{
		PersistentVoxelMaterialVariantResource& resource = pair.second;
		if (resource.materialKeyHash == 0 || desiredMaterials.find(pair.first) != desiredMaterials.end())
		{
			continue;
		}
		resource.cold = true;
		coldMaterials++;
		if (loadingTraceLevel >= 2)
		{
			Printf("NRI PT voxel residency entry: action=cold reason=map-not-desired source=material source_bits=0x0 priority=0 force=0 prefer=0 tex=-1 voxel=-1 mesh_variant=0x0 mat_variant=0x%llx prims=0 bytes=0 mesh_ready=0 material_ready=%u blas_ready=0 generation=%u last_desired=%u\n",
				(unsigned long long)pair.first,
				materialReady(pair.first) ? 1u : 0u,
				generation,
				resource.lastDesiredMapGeneration);
		}
	}

	lastDesiredResidencyCount = (uint32_t)desired.size();
	lastDesiredPreloadCount = desiredPreload;
	lastDesiredActorCount = desiredActors;
	lastCpuReadyCount = cpuReady;
	lastGpuReadyCount = gpuReady;
	lastRetainedCount = retained;
	lastQueuedCount = queued;
	lastQueuedUploadBytes = queuedUploadBytes;
	lastMeshReadyCount = meshReadyCount;
	lastMaterialReadyCount = materialReadyCount;
	lastBlasReadyCount = blasReadyCount;
	lastMeshMissingCount = meshMissingCount;
	lastMaterialOnlyCount = materialOnlyCount;
	lastBlasOnlyCount = blasOnlyCount;
	lastColdMeshCount = coldMeshes;
	lastColdMaterialCount = coldMaterials;
	lastColdPrimitiveCount = coldPrimitiveCount;
	lastForcedCount = forceCount;
	lastPreferredCount = preferCount;

	if (loadingTraceLevel >= 1)
	{
		Printf("NRI PT voxel residency reconcile: level=%s build_serial=%llu generation=%u desired=%u desired_preload=%u desired_actor=%u cpu_ready=%u gpu_ready=%u retained=%u queued=%u queue_bytes=%llu mesh_ready=%u material_ready=%u blas_ready=%u mesh_missing=%u material_only=%u blas_only=%u cold_mesh=%u cold_material=%u cold_prims=%llu evicted=0 forced=%u preferred=%u mesh_resources=%u material_resources=%u actors=%u active=%u prims=%u\n",
			levelName != nullptr ? levelName : "(none)",
			(unsigned long long)buildSerial,
			generation,
			(uint32_t)desired.size(),
			desiredPreload,
			desiredActors,
			cpuReady,
			gpuReady,
			retained,
			queued,
			(unsigned long long)queuedUploadBytes,
			meshReadyCount,
			materialReadyCount,
			blasReadyCount,
			meshMissingCount,
			materialOnlyCount,
			blasOnlyCount,
			coldMeshes,
			coldMaterials,
			(unsigned long long)coldPrimitiveCount,
			forceCount,
			preferCount,
			(uint32_t)meshVariantResources.size(),
			(uint32_t)materialVariantResources.size(),
			(uint32_t)batch.actors.size(),
			batch.activeActorCount,
			batch.primitiveCount);
	}
}

void NRIPersistentVoxelResidency::DiscardAdmissionEntry(PersistentVoxelAdmissionEntry& entry, const NRIPersistentVoxelResetServices& services)
{
	if (entry.directComputeRequested)
	{
		CancelNRIVoxelComputeDirectPublication(BuildPersistentVoxelVariantMeshResourceKey(entry.variant), entry.directComputeGeneration);
		if (entry.schedulerTokenId != 0)
		{
			admissionScheduler.Cancel(entry.schedulerTokenId);
		}
	}
	services.RetireBuffer(entry.directBlasScratchBuffer);
	services.RetireBuffer(entry.uploadMeshResource.vertexBuffer);
	services.RetireBuffer(entry.uploadMeshResource.indexBuffer);
	services.RetireAccelerationStructure(entry.uploadMeshResource.accelerationStructure);
	entry.uploadMeshResource = {};
	entry.uploadMaterialResource = {};
	entry.uploadPrepared = false;
	entry.shaderVertexOffset = 0;
	entry.shaderIndexOffset = 0;
	entry.shaderPrimitiveOffset = 0;
	entry.savedVertexCursor = 0;
	entry.savedIndexCursor = 0;
	entry.savedPrimitiveCursor = 0;
	entry.vertexBytesUploaded = 0;
	entry.vertexArenaBytesUploaded = 0;
	entry.indexBytesUploaded = 0;
	entry.indexArenaBytesUploaded = 0;
	entry.primitiveBytesUploaded = 0;
	entry.bytesUploaded = 0;
	entry.uploadGeometry = {};
	entry.uploadGpuIndices.clear();
	entry.uploadGpuPrimitives.clear();
	entry.uploadGeometryFromCompute = false;
	entry.computeGeometryFailed = false;
	entry.computeGeometryJobId = 0;
	entry.directComputeRequested = false;
	entry.directComputeFailed = false;
	entry.directComputeGeneration = 0;
	entry.directComputeJobId = 0;
	entry.directComputeRequestFrame = UINT32_MAX;
	entry.directComputeOutputKind = NRIVoxelComputeDirectPublishOutputKind::None;
	entry.directComputeFailure = NRIVoxelComputeDirectPublishFailure::None;
	entry.directBlasFenceValue = 0;
	entry.directBlasRecordedFrame = UINT32_MAX;
	entry.directBlasExclusive = false;
	entry.schedulerTokenId = 0;
}

bool NRIPersistentVoxelResidency::EnqueueAdmission(
	const nri_scene::PrecachedVoxelVariantView& variant,
	bool runtimeRequested,
	const char* sourceLabel,
	uint64_t buildSerial,
	const NRIPersistentVoxelSettings& settings,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& services)
{
	const uint64_t meshResourceKey = BuildPersistentVoxelVariantMeshResourceKey(variant);
	if ((!variant.directOnlyAdmission && variant.surface == nullptr) ||
		variant.meshKeyHash == 0 ||
		meshResourceKey == 0 ||
		variant.materialKeyHash == 0)
	{
		return false;
	}

	SyncMapGeneration(
		buildSerial,
		"admission-map-generation",
		loadingTraceLevel >= 1 || voxelStatsEnabled,
		services);
	if (IsNRIVoxelComputePreloadRuntimeWithheldMesh(buildSerial, meshResourceKey) &&
		!IsNRIVoxelComputePreloadRuntimeTailReleased(buildSerial))
	{
		if (loadingTraceLevel >= 1 || voxelStatsEnabled)
		{
			Printf("NRI PT voxel admission queue: event=skip source=%s runtime=%u mesh_resource=0x%llx mesh_variant=0x%llx mat_variant=0x%llx reason=runtime-tail-held generation=%u\n",
				sourceLabel != nullptr ? sourceLabel : "unknown",
				runtimeRequested ? 1u : 0u,
				(unsigned long long)meshResourceKey,
				(unsigned long long)variant.meshKeyHash,
				(unsigned long long)variant.materialKeyHash,
				residencyMapGeneration);
		}
		return false;
	}

	const uint64_t pairKey = nri_scene::HashCombine64(meshResourceKey, variant.materialKeyHash);
	auto estimateAdmissionBytes = [](const nri_scene::PrecachedVoxelVariantView& admissionVariant) -> uint64_t
	{
		if (admissionVariant.directOnlyAdmission || admissionVariant.surface == nullptr)
		{
			return
				(uint64_t)admissionVariant.primitiveCount * 2ull * (uint64_t)sizeof(nri_scene::SceneVertex) +
				(uint64_t)admissionVariant.primitiveCount * 3ull * (uint64_t)sizeof(uint32_t) +
				(uint64_t)admissionVariant.primitiveCount * (uint64_t)sizeof(nri_scene::PrimitiveData);
		}
		return
			(uint64_t)admissionVariant.surface->vertices.size() * (uint64_t)sizeof(nri_scene::SceneVertex) +
			(uint64_t)admissionVariant.surface->indices.size() * (uint64_t)sizeof(uint32_t) +
			(uint64_t)admissionVariant.primitiveCount * (uint64_t)sizeof(nri_scene::PrimitiveData);
	};
	const uint64_t estimatedBytes = estimateAdmissionBytes(variant);
	const int32_t variantAdmissionRank =
		variant.admissionRank != 0 || variant.priority <= 0 ? variant.admissionRank : variant.priority * 10000 + 9900;
	const uint32_t maxBlasPrimitives = settings.admitMaxBlasPrimitives;
	auto traceAdmissionSkip = [&](const nri_scene::PrecachedVoxelVariantView& skippedVariant, uint64_t skippedBytes, const char* reason)
	{
		if (loadingTraceLevel >= 1 || voxelStatsEnabled)
		{
			Printf("NRI PT voxel admission queue: event=skip source=%s source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u max_prims=%u bytes=%llu reason=%s generation=%u\n",
				sourceLabel != nullptr ? sourceLabel : "unknown",
				skippedVariant.sourceBits,
				skippedVariant.priority,
				variantAdmissionRank,
				skippedVariant.gpuForce ? 1u : 0u,
				skippedVariant.gpuPrefer ? 1u : 0u,
				runtimeRequested ? 1u : 0u,
				skippedVariant.sourcePicnum,
				skippedVariant.resolvedVoxelIndex,
				(unsigned long long)skippedVariant.meshKeyHash,
				(unsigned long long)skippedVariant.materialKeyHash,
				skippedVariant.primitiveCount,
				maxBlasPrimitives,
				(unsigned long long)skippedBytes,
				reason != nullptr ? reason : "unknown",
				residencyMapGeneration);
		}
	};

	auto found = admissionQueue.find(pairKey);
	if (found != admissionQueue.end() && found->second.mapGeneration != residencyMapGeneration)
	{
		admissionIndex.Remove(found->first, GetAdmissionBucket(found->second));
		DiscardAdmissionEntry(found->second, services);
		admissionQueue.erase(found);
		MarkMaintenanceMutation();
		found = admissionQueue.end();
	}
	if (found != admissionQueue.end())
	{
		PersistentVoxelAdmissionEntry& entry = found->second;
		const NRIPersistentVoxelAdmissionBucket oldBucket = GetAdmissionBucket(entry);
		const uint32_t oldSourceBits = entry.sourceBits;
		const int32_t oldPriorityValue = entry.priority;
		const int32_t oldAdmissionRank = entry.admissionRank;
		const bool oldGpuForce = entry.gpuForce;
		const bool oldGpuPrefer = entry.gpuPrefer;
		const bool oldRuntimeRequested = entry.runtimeRequested;
		const nri_scene::SurfaceRef* oldSurface = entry.variant.surface;
		const bool oldDirectOnlyAdmission = entry.variant.directOnlyAdmission;
		const int32_t oldPriority = entry.priority;
		const bool oldForce = entry.gpuForce;
		const bool wasReady = entry.state == PersistentVoxelAdmissionState::Ready;
		const uint64_t entryMeshResourceKey = BuildPersistentVoxelVariantMeshResourceKey(entry.variant);
		const PersistentVoxelReadinessStatus readiness = GetSharedVariantReadiness(entryMeshResourceKey, entry.variant.materialKeyHash);
		const bool resourcesReady = readiness.ready;
		if (!resourcesReady && entry.variant.primitiveCount > maxBlasPrimitives)
		{
			traceAdmissionSkip(entry.variant, entry.estimatedBytes, "blas-primitive-budget");
			admissionIndex.Remove(found->first, oldBucket);
			DiscardAdmissionEntry(entry, services);
			admissionQueue.erase(found);
			MarkMaintenanceMutation();
			return false;
		}
		const bool runtimeDirectCandidate =
			runtimeRequested &&
			ShouldDirectPublishNRIVoxelComputeMeshing() &&
			variant.model != nullptr &&
			variant.primitiveCount != 0 &&
			entry.state != PersistentVoxelAdmissionState::DirectComputePending &&
			entry.state != PersistentVoxelAdmissionState::DirectBlasPending;
		const bool preloadDirectCandidate =
			!runtimeRequested &&
			variant.directOnlyAdmission &&
			ShouldDirectPublishNRIVoxelComputeMeshing() &&
			variant.model != nullptr &&
			variant.primitiveCount != 0 &&
			entry.state != PersistentVoxelAdmissionState::DirectComputePending &&
			entry.state != PersistentVoxelAdmissionState::DirectBlasPending;
		const bool shouldPromoteRuntimeVariant =
			!resourcesReady &&
			runtimeDirectCandidate &&
			(!entry.runtimeRequested ||
			 entry.variant.model == nullptr ||
			 (variant.directOnlyAdmission && !entry.variant.directOnlyAdmission) ||
			 entry.directComputeFailed ||
			 entry.uploadPrepared ||
			 entry.state == PersistentVoxelAdmissionState::UploadingVertices ||
			 entry.state == PersistentVoxelAdmissionState::UploadingIndices ||
			 entry.state == PersistentVoxelAdmissionState::UploadingPrimitives ||
			 entry.state == PersistentVoxelAdmissionState::BuildingBlas);
		const bool shouldPromotePreloadDirectVariant =
			!resourcesReady &&
			preloadDirectCandidate &&
			!entry.variant.directOnlyAdmission &&
			!entry.uploadPrepared &&
			entry.state != PersistentVoxelAdmissionState::UploadingVertices &&
			entry.state != PersistentVoxelAdmissionState::UploadingIndices &&
			entry.state != PersistentVoxelAdmissionState::UploadingPrimitives &&
			entry.state != PersistentVoxelAdmissionState::BuildingBlas;
		bool admissionPayloadChanged = false;
		if (shouldPromoteRuntimeVariant || shouldPromotePreloadDirectVariant)
		{
			nri_scene::PrecachedVoxelVariantView promotedVariant = variant;
			if (promotedVariant.surface == nullptr && entry.variant.surface != nullptr)
			{
				promotedVariant.surface = entry.variant.surface;
				if (promotedVariant.geometryContentHash == 0)
				{
					promotedVariant.geometryContentHash = entry.variant.geometryContentHash;
				}
				if (promotedVariant.renderPrimitiveHash == 0)
				{
					promotedVariant.renderPrimitiveHash = entry.variant.renderPrimitiveHash;
				}
			}
			DiscardAdmissionEntry(entry, services);
			entry.variant = promotedVariant;
			entry.state = PersistentVoxelAdmissionState::Pending;
			entry.retryCount = 0;
			entry.estimatedBytes = estimateAdmissionBytes(entry.variant);
			entry.lastReason = shouldPromotePreloadDirectVariant ? "preload-direct-promote" : "runtime-direct-promote";
			admissionPayloadChanged = true;
		}
		else if (!resourcesReady &&
			entry.variant.directOnlyAdmission &&
			entry.variant.surface == nullptr &&
			variant.surface != nullptr)
		{
			entry.variant.surface = variant.surface;
			if (entry.variant.geometryContentHash == 0)
			{
				entry.variant.geometryContentHash = variant.geometryContentHash;
			}
			if (entry.variant.renderPrimitiveHash == 0)
			{
				entry.variant.renderPrimitiveHash = variant.renderPrimitiveHash;
			}
			if (entry.lastReason == nullptr || std::strcmp(entry.lastReason, "queued") == 0)
			{
				entry.lastReason = "direct-fallback-surface";
			}
			admissionPayloadChanged = true;
		}
		entry.sourceBits |= variant.sourceBits;
		entry.priority = std::min(entry.priority, variant.priority);
		entry.admissionRank = std::min(entry.admissionRank, variantAdmissionRank);
		entry.gpuForce = entry.gpuForce || variant.gpuForce;
		entry.gpuPrefer = entry.gpuPrefer || variant.gpuPrefer;
		entry.runtimeRequested = entry.runtimeRequested || runtimeRequested;
		entry.variant.sourceBits = entry.sourceBits;
		entry.variant.priority = entry.priority;
		entry.variant.admissionRank = entry.admissionRank;
		entry.variant.gpuForce = entry.gpuForce;
		entry.variant.gpuPrefer = entry.gpuPrefer;
		if (!resourcesReady && entry.state == PersistentVoxelAdmissionState::Ready)
		{
			entry.state = PersistentVoxelAdmissionState::Pending;
			entry.lastReason = "stale-ready";
		}
		if (entry.state == PersistentVoxelAdmissionState::Deferred &&
			(entry.priority != oldPriority || (!oldForce && entry.gpuForce)))
		{
			entry.state = PersistentVoxelAdmissionState::Pending;
			entry.lastReason = "reprioritized";
		}
		else if (entry.state == PersistentVoxelAdmissionState::Failed &&
			(entry.priority != oldPriority || (!oldForce && entry.gpuForce)))
		{
			entry.state = PersistentVoxelAdmissionState::Pending;
			entry.lastReason = "retry-priority";
		}
		admissionIndex.Transition(found->first, oldBucket, GetAdmissionBucket(entry));
		if (admissionPayloadChanged ||
			oldBucket != GetAdmissionBucket(entry) ||
			oldSourceBits != entry.sourceBits ||
			oldPriorityValue != entry.priority ||
			oldAdmissionRank != entry.admissionRank ||
			oldGpuForce != entry.gpuForce ||
			oldGpuPrefer != entry.gpuPrefer ||
			oldRuntimeRequested != entry.runtimeRequested ||
			oldSurface != entry.variant.surface ||
			oldDirectOnlyAdmission != entry.variant.directOnlyAdmission)
		{
			MarkMaintenanceMutation();
		}
		if (loadingTraceLevel >= 2 || voxelStatsEnabled)
		{
			Printf("NRI PT voxel admission queue: event=%s source=%s source_bits=0x%x priority=%d old_priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu generation=%u\n",
				resourcesReady && runtimeRequested ? "dedupe-ready" : (wasReady && !resourcesReady ? "stale-ready" : (entry.priority != oldPriority ? "promote" : "dedupe")),
				sourceLabel != nullptr ? sourceLabel : "unknown",
				entry.sourceBits,
				entry.priority,
				oldPriority,
				entry.admissionRank,
				entry.gpuForce ? 1u : 0u,
				entry.gpuPrefer ? 1u : 0u,
				entry.runtimeRequested ? 1u : 0u,
				entry.variant.sourcePicnum,
				entry.variant.resolvedVoxelIndex,
				(unsigned long long)entry.variant.meshKeyHash,
				(unsigned long long)entry.variant.materialKeyHash,
				entry.variant.primitiveCount,
				(unsigned long long)entry.estimatedBytes,
				entry.mapGeneration);
		}
		return true;
	}

	const PersistentVoxelReadinessStatus readiness = GetSharedVariantReadiness(meshResourceKey, variant.materialKeyHash);
	const bool resourcesReady = readiness.ready;
	if (!resourcesReady && variant.primitiveCount > maxBlasPrimitives)
	{
		traceAdmissionSkip(variant, estimatedBytes, "blas-primitive-budget");
		return false;
	}

	PersistentVoxelAdmissionEntry entry = {};
	entry.pairKey = pairKey;
	entry.variant = variant;
	entry.state = resourcesReady ?
		PersistentVoxelAdmissionState::Ready :
		PersistentVoxelAdmissionState::Pending;
	entry.sourceBits = variant.sourceBits;
	entry.priority = variant.priority;
	entry.admissionRank = variantAdmissionRank;
	entry.gpuForce = variant.gpuForce;
	entry.gpuPrefer = variant.gpuPrefer;
	entry.runtimeRequested = runtimeRequested;
	entry.mapGeneration = residencyMapGeneration;
	entry.estimatedBytes = estimatedBytes;
	entry.lastReason = entry.state == PersistentVoxelAdmissionState::Ready ? "resident" : "queued";
	admissionQueue[pairKey] = entry;
	admissionIndex.Add(pairKey, GetAdmissionBucket(admissionQueue[pairKey]));
	MarkMaintenanceMutation();
	if (resourcesReady && runtimeRequested)
	{
		TraceReadiness(
			"dedupe-ready",
			sourceLabel,
			&admissionQueue[pairKey],
			meshResourceKey,
			variant.materialKeyHash,
			readiness,
			loadingTraceLevel >= 2 || voxelStatsEnabled);
	}

	if (loadingTraceLevel >= 2 || voxelStatsEnabled)
	{
		Printf("NRI PT voxel admission queue: event=%s source=%s source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu generation=%u\n",
			entry.state == PersistentVoxelAdmissionState::Ready && runtimeRequested ? "dedupe-ready" : (entry.state == PersistentVoxelAdmissionState::Ready ? "ready" : "enqueue"),
			sourceLabel != nullptr ? sourceLabel : "unknown",
			entry.sourceBits,
			entry.priority,
			entry.admissionRank,
			entry.gpuForce ? 1u : 0u,
			entry.gpuPrefer ? 1u : 0u,
			entry.runtimeRequested ? 1u : 0u,
			entry.variant.sourcePicnum,
			entry.variant.resolvedVoxelIndex,
			(unsigned long long)entry.variant.meshKeyHash,
			(unsigned long long)entry.variant.materialKeyHash,
			entry.variant.primitiveCount,
			(unsigned long long)entry.estimatedBytes,
			entry.mapGeneration);
	}
	return true;
}

bool NRIPersistentVoxelResidency::PreSizeDirectGeometryArenas(
	uint64_t buildSerial,
	uint64_t uniqueGeometryBytes,
	uint64_t plannedRuntimeTailBytes,
	uint64_t largestKnownGeometryBytes,
	int loadingTraceLevel,
	const NRIPersistentVoxelAdmissionServices& services)
{
	if (buildSerial == 0 || uniqueGeometryBytes == 0 || arenaPresizeBuildSerial == buildSerial)
	{
		return true;
	}
	if (arenaVertexCursor != 0 || arenaIndexCursor != 0 || arenaPrimitiveCursor != 0)
	{
		arenaPresizeBuildSerial = buildSerial;
		if (loadingTraceLevel >= 1 || (int)nri_ptvoxelcomputetrace > 0)
		{
			Printf("PERF pt voxel arena presize NRI: build_serial=%llu result=retained-arena-skip vertex_cursor=%u index_cursor=%u primitive_cursor=%u\n",
				(unsigned long long)buildSerial,
				arenaVertexCursor,
				arenaIndexCursor,
				arenaPrimitiveCursor);
		}
		return true;
	}

	constexpr uint64_t BytesPerPrimitive =
		2ull * sizeof(nri_scene::SceneVertex) +
		3ull * sizeof(uint32_t) +
		sizeof(nri_scene::PrimitiveData);
	if (uniqueGeometryBytes % BytesPerPrimitive != 0)
	{
		return false;
	}
	const NRIPersistentVoxelGeometryArenaPlan arenaPlan =
		BuildNRIPersistentVoxelGeometryArenaPlan(
			uniqueGeometryBytes,
			plannedRuntimeTailBytes,
			largestKnownGeometryBytes);
	if (arenaPlan.overflow || arenaPlan.targetGeometryBytes == 0)
	{
		return false;
	}
	if (arenaPlan.targetGeometryBytes >
		std::numeric_limits<uint64_t>::max() - (BytesPerPrimitive - 1ull))
	{
		return false;
	}
	const uint64_t primitiveCount =
		(arenaPlan.targetGeometryBytes + BytesPerPrimitive - 1ull) / BytesPerPrimitive;
	if (primitiveCount > std::numeric_limits<uint32_t>::max() / 3ull)
	{
		return false;
	}

	const uint64_t vertexBytes = primitiveCount * 2ull * sizeof(nri_scene::SceneVertex);
	const uint64_t indexBytes = primitiveCount * 3ull * sizeof(uint32_t);
	const uint64_t primitiveBytes = primitiveCount * sizeof(nri_scene::PrimitiveData);
	const nri::BufferUsageBits geometryUsage = NRIResourceFlags(
		NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE),
		nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT);
	const nri::BufferUsageBits primitiveUsage = NRIResourceFlags(
		nri::BufferUsageBits::SHADER_RESOURCE,
		nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
	if (!services.EnsureArenaBuffer(
			vertexBuffer,
			vertexBytes,
			sizeof(nri_scene::SceneVertex),
			geometryUsage,
			PersistentVoxelComputeShaderResourceAccess()) ||
		!services.EnsureArenaBuffer(
			indexBuffer,
			indexBytes,
			sizeof(uint32_t),
			geometryUsage,
			PersistentVoxelComputeShaderResourceAccess()) ||
		!services.EnsureArenaBuffer(
			primitiveBuffer,
			primitiveBytes,
			sizeof(nri_scene::PrimitiveData),
			primitiveUsage,
			PersistentVoxelComputeShaderResourceAccess()))
	{
		return false;
	}

	arenaPresizeBuildSerial = buildSerial;
	if (loadingTraceLevel >= 1 || (int)nri_ptvoxelcomputetrace > 0)
	{
		Printf("PERF pt voxel arena presize NRI: build_serial=%llu planned_bytes=%llu planned_runtime_tail_bytes=%llu late_alias_reserve_bytes=%llu reserve_bytes=%llu target_bytes=%llu capacity_bytes=%llu primitives=%llu vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu total_bytes=%llu\n",
			(unsigned long long)buildSerial,
			(unsigned long long)arenaPlan.plannedGeometryBytes,
			(unsigned long long)arenaPlan.plannedRuntimeTailBytes,
			(unsigned long long)arenaPlan.lateAliasReserveBytes,
			(unsigned long long)arenaPlan.totalReserveBytes,
			(unsigned long long)arenaPlan.targetGeometryBytes,
			(unsigned long long)(primitiveCount * BytesPerPrimitive),
			(unsigned long long)primitiveCount,
			(unsigned long long)vertexBytes,
			(unsigned long long)indexBytes,
			(unsigned long long)primitiveBytes,
			(unsigned long long)(vertexBytes + indexBytes + primitiveBytes));
	}
	return true;
}

bool NRIPersistentVoxelResidency::PreloadVariantResources(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	uint64_t buildSerial,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& resetServices,
	const NRIPersistentVoxelPreloadServices& preloadServices)
{
	preloadPending = false;
	if (variants.empty())
	{
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT loading voxel resources: event=variant-skip reason=no-shared-variants variants=0 mesh_resources=%u material_resources=%u prims=0\n",
				(uint32_t)meshVariantResources.size(),
				(uint32_t)materialVariantResources.size());
		}
		return true;
	}

	uint32_t runtimeWithheldVariants = 0;
	for (const nri_scene::PrecachedVoxelVariantView& variant : variants)
	{
		if (!variant.preloadGeometry)
		{
			runtimeWithheldVariants++;
			continue;
		}
		EnqueueAdmission(
			variant,
			false,
			"preload",
			buildSerial,
			settings,
			loadingTraceLevel,
			voxelStatsEnabled,
			resetServices);
	}
	if (runtimeWithheldVariants != 0 && loadingTraceLevel >= 1)
	{
		Printf("NRI PT loading voxel resources: event=runtime-tail-withhold variants=%u admitted_for_geometry=%u\n",
			runtimeWithheldVariants,
			(uint32_t)variants.size() - runtimeWithheldVariants);
	}

	const bool blockOptionalPreloadAdmissions = (bool)nri_ptloadingvoxelblockoptional;
	auto isBlockingAdmission = [&](const PersistentVoxelAdmissionEntry& entry) -> bool
	{
		return
			entry.mapGeneration == residencyMapGeneration &&
			!entry.runtimeRequested &&
			(IsRequiredAdmission(entry) || blockOptionalPreloadAdmissions);
	};
	auto blockingPendingCount = [&](uint32_t requiredPending, uint32_t optionalPending) -> uint32_t
	{
		return requiredPending + (blockOptionalPreloadAdmissions ? optionalPending : 0u);
	};
	auto readyReason = [&]() -> const char*
	{
		return blockOptionalPreloadAdmissions ? "optional-drained" : "required-drained";
	};
	auto hasBlockingUploadInProgress = [&]() -> bool
	{
		for (const auto& pair : admissionQueue)
		{
			const PersistentVoxelAdmissionEntry& entry = pair.second;
			if (!isBlockingAdmission(entry))
			{
				continue;
			}
			if (entry.state == PersistentVoxelAdmissionState::UploadingVertices ||
				entry.state == PersistentVoxelAdmissionState::UploadingIndices ||
				entry.state == PersistentVoxelAdmissionState::UploadingPrimitives ||
				entry.state == PersistentVoxelAdmissionState::BuildingBlas ||
				entry.state == PersistentVoxelAdmissionState::DirectComputePending ||
				entry.state == PersistentVoxelAdmissionState::DirectBlasPending)
			{
				return true;
			}
		}
		return false;
	};

	bool ok = true;
	const auto preloadAdmissionStart = std::chrono::steady_clock::now();
	const int configuredLoadingMsBudget = (int)settings.admitMaxMsLoading;
	const double preloadTickBudgetMs = configuredLoadingMsBudget > 0 ? (double)configuredLoadingMsBudget : 250.0;
	const uint32_t maxPumps = std::max<uint32_t>(1024u, (uint32_t)admissionQueue.size() * 64u + 64u);
	for (uint32_t pump = 0; pump < maxPumps; ++pump)
	{
		uint32_t requiredPendingBefore = 0;
		uint32_t requiredReadyBefore = 0;
		uint32_t optionalPendingBefore = 0;
		uint32_t failedBefore = 0;
		CountAdmissionWork(requiredPendingBefore, requiredReadyBefore, optionalPendingBefore, failedBefore);
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT voxel admission pump: phase=loading pass=%u required_pending=%u required_ready=%u optional_pending=%u failed=%u stop=%s\n",
				pump,
				requiredPendingBefore,
				requiredReadyBefore,
				optionalPendingBefore,
				failedBefore,
				blockingPendingCount(requiredPendingBefore, optionalPendingBefore) == 0 ? readyReason() : "none");
		}
		if (blockingPendingCount(requiredPendingBefore, optionalPendingBefore) == 0)
		{
			if (loadingTraceLevel >= 1)
			{
				Printf("NRI PT loading gate: event=voxel-admission result=ready reason=%s required_pending=%u required_ready=%u optional_pending=%u failed=%u block_optional=%u\n",
					readyReason(),
					requiredPendingBefore,
					requiredReadyBefore,
					optionalPendingBefore,
					failedBefore,
					blockOptionalPreloadAdmissions ? 1u : 0u);
			}
			break;
		}

		preloadServices.PumpComputeJobs(frameIndex);
		ok = preloadServices.PumpAdmissionQueue("loading") && ok;
		preloadServices.PumpComputeJobs(frameIndex);

		uint32_t requiredPendingAfter = 0;
		uint32_t requiredReadyAfter = 0;
		uint32_t optionalPendingAfter = 0;
		uint32_t failedAfter = 0;
		CountAdmissionWork(requiredPendingAfter, requiredReadyAfter, optionalPendingAfter, failedAfter);
		if (preloadServices.IsSubmitBudgetHit())
		{
			preloadPending = true;
			if (loadingTraceLevel >= 1)
			{
				Printf("NRI PT loading gate: event=voxel-admission result=wait reason=preload-submit-budget required_pending=%u required_ready=%u optional_pending=%u failed=%u\n",
					requiredPendingAfter,
					requiredReadyAfter,
					optionalPendingAfter,
					failedAfter);
			}
			return false;
		}
		if (!ok)
		{
			if (loadingTraceLevel >= 1)
			{
				Printf("NRI PT loading gate: event=voxel-admission result=continue reason=failure required_pending=%u required_ready=%u optional_pending=%u failed=%u\n",
					requiredPendingAfter,
					requiredReadyAfter,
					optionalPendingAfter,
					failedAfter);
			}
			break;
		}
		if (blockingPendingCount(requiredPendingAfter, optionalPendingAfter) == 0)
		{
			if (loadingTraceLevel >= 1)
			{
				Printf("NRI PT loading gate: event=voxel-admission result=ready reason=%s required_pending=%u required_ready=%u optional_pending=%u failed=%u block_optional=%u\n",
					readyReason(),
					requiredPendingAfter,
					requiredReadyAfter,
					optionalPendingAfter,
					failedAfter,
					blockOptionalPreloadAdmissions ? 1u : 0u);
			}
			break;
		}
		const double preloadTickMs = PersistentVoxelDurationMs(preloadAdmissionStart, std::chrono::steady_clock::now());
		if (preloadTickBudgetMs > 0.0 && preloadTickMs >= preloadTickBudgetMs)
		{
			preloadPending = true;
			if (loadingTraceLevel >= 1)
			{
				Printf("NRI PT loading gate: event=voxel-admission result=wait reason=tick-budget pass=%u required_pending=%u required_ready=%u optional_pending=%u failed=%u ms_budget=%.3f ms_used=%.3f\n",
					pump,
					requiredPendingAfter,
					requiredReadyAfter,
					optionalPendingAfter,
					failedAfter,
					preloadTickBudgetMs,
					preloadTickMs);
			}
			return false;
		}
		if (blockingPendingCount(requiredPendingAfter, optionalPendingAfter) >= blockingPendingCount(requiredPendingBefore, optionalPendingBefore) &&
			requiredReadyAfter <= requiredReadyBefore &&
			!hasBlockingUploadInProgress())
		{
			if (loadingTraceLevel >= 1)
			{
				Printf("NRI PT loading gate: event=voxel-admission result=continue reason=no-progress required_pending=%u required_ready=%u optional_pending=%u failed=%u\n",
					requiredPendingAfter,
					requiredReadyAfter,
					optionalPendingAfter,
					failedAfter);
			}
			break;
		}
		preloadPending = true;
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT loading gate: event=voxel-admission result=wait reason=pump-budget required_pending=%u optional_pending=%u required_ready=%u failed=%u\n",
				requiredPendingAfter,
				optionalPendingAfter,
				requiredReadyAfter,
				failedAfter);
		}
		return false;
	}
	return ok;
}

bool NRIPersistentVoxelResidency::PreloadMaterialPayloads(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	uint64_t buildSerial,
	const char* levelName,
	uint32_t frameIndex,
	uint32_t maxMaterialRows,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelPreloadServices& preloadServices,
	NRIPersistentVoxelMaterialPreloadStats& outStats)
{
	const auto start = std::chrono::steady_clock::now();
	outStats = {};
	outStats.attempted = true;
	outStats.variants = (uint32_t)variants.size();
	std::vector<std::pair<uint64_t, uint64_t>> closureKeys;
	closureKeys.reserve(variants.size());
	for (const nri_scene::PrecachedVoxelVariantView& variant : variants)
	{
		if (!variant.directOnlyAdmission)
		{
			continue;
		}
		outStats.directOnlyVariants++;
		const uint64_t validatedMaterialSignature =
			variant.materialVariantHash != 0 ? variant.materialVariantHash : variant.materialKeyHash;
		closureKeys.emplace_back(variant.materialKeyHash, validatedMaterialSignature);
	}
	std::sort(closureKeys.begin(), closureKeys.end());
	closureKeys.erase(std::unique(closureKeys.begin(), closureKeys.end()), closureKeys.end());
	uint64_t closureSignature = nri_scene::HashCombine64(0x50564d4154434831ull, maxMaterialRows); // PVMATCH1
	for (const auto& key : closureKeys)
	{
		closureSignature = nri_scene::HashCombine64(closureSignature, key.first);
		closureSignature = nri_scene::HashCombine64(closureSignature, key.second);
	}
	if (preloadMaterialClosureCached &&
		preloadMaterialClosureBuildSerial == buildSerial &&
		preloadMaterialClosureSignature == closureSignature)
	{
		preloadMaterialClosureCacheHits++;
		if ((loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0) &&
			(preloadMaterialClosureCacheHits == 1 ||
				(preloadMaterialClosureCacheHits & (preloadMaterialClosureCacheHits - 1u)) == 0))
		{
			Printf("NRI PT voxel preload material cache: level=%s build_serial=%llu frame=%u signature=0x%llx direct=%u resources=%u hits=%llu\n",
				levelName != nullptr ? levelName : "unknown",
				(unsigned long long)buildSerial,
				frameIndex,
				(unsigned long long)closureSignature,
				outStats.directOnlyVariants,
				(uint32_t)materialVariantResources.size(),
				(unsigned long long)preloadMaterialClosureCacheHits);
		}
		return true;
	}

	std::unordered_set<uint64_t> seenMaterials;
	seenMaterials.reserve(variants.size());
	bool ok = true;
	for (const nri_scene::PrecachedVoxelVariantView& variant : variants)
	{
		if (!variant.directOnlyAdmission)
		{
			continue;
		}
		if (variant.materialKeyHash == 0 ||
			variant.material.texture == nullptr ||
			variant.materialSurface.material.texture == nullptr)
		{
			outStats.skippedInvalid++;
			continue;
		}
		if (!seenMaterials.insert(variant.materialKeyHash).second)
		{
			continue;
		}

		outStats.candidates++;
		const uint64_t validatedMaterialSignature =
			variant.materialVariantHash != 0 ? variant.materialVariantHash : variant.materialKeyHash;
		if (variant.materialVariantHash != 0 && variant.materialVariantHash != variant.materialKeyHash)
		{
			outStats.actorScopedMaterials++;
		}

		auto existingIt = materialVariantResources.find(variant.materialKeyHash);
		const bool existingReady =
			existingIt != materialVariantResources.end() &&
			existingIt->second.materialKeyHash == variant.materialKeyHash &&
			existingIt->second.materialSignature == validatedMaterialSignature &&
			existingIt->second.materialBridgeBuildSerial == buildSerial &&
			existingIt->second.materialCount != 0 &&
			existingIt->second.materialSlotGeneration != 0 &&
			!existingIt->second.materialBridge.materials.empty();
		if (existingReady)
		{
			PersistentVoxelMaterialVariantResource& resource = existingIt->second;
			NRIPersistentVoxelMaterialClosureResult closure = {};
			if (!preloadServices.materialClosure.Register(
					buildSerial,
					variant.materialKeyHash,
					validatedMaterialSignature,
					resource.materialBridge,
					NRIPersistentVoxelMaterialClosureSource::PreloadKnown,
					closure))
			{
				outStats.failed++;
				ok = false;
				continue;
			}
			resource.lastDesiredMapGeneration = residencyMapGeneration;
			resource.lastUsedMapGeneration = residencyMapGeneration;
			resource.lastUsedFrame = frameIndex;
			resource.sourceBits |= variant.sourceBits;
			resource.priority = std::min(resource.priority, variant.priority);
			resource.gpuForce = resource.gpuForce || variant.gpuForce;
			resource.gpuPrefer = resource.gpuPrefer || variant.gpuPrefer;
			resource.cold = false;
			outStats.reused++;
			outStats.materialRows += resource.materialCount;
			outStats.materialBytes +=
				(uint64_t)resource.materialBridge.materials.size() * (uint64_t)sizeof(nri_scene::MaterialData);
			outStats.textureRequests += closure.textureDependencies;
			for (const NRIPersistentVoxelTextureDependency& dependency : closure.textures)
			{
				outStats.textureBytes += dependency.estimatedBytes;
			}
			continue;
		}

		nri_scene::SurfaceRef surface = variant.materialSurface;
		surface.material = variant.material;
		nri_scene::SceneView variantSceneView = {};
		variantSceneView.opaqueSprites.push_back(std::move(surface));
		variantSceneView.stats.spriteDrawItems = 1;
		variantSceneView.stats.modelDrawItems = 1;
		variantSceneView.stats.voxelProxyDrawItems = 1;
		variantSceneView.stats.totalDrawItems = 1;
		variantSceneView.stats.materialRefs = 1;
		variantSceneView.stats.triangleEstimate = variant.primitiveCount;
		variantSceneView.stats.voxelCachePrimitives = variant.primitiveCount;

		nri_scene::MaterialBridgeData builtMaterials;
		preloadServices.BuildMaterials(variantSceneView, builtMaterials, "persistent_voxel_preload_material");
		if (builtMaterials.materials.empty())
		{
			outStats.failed++;
			ok = false;
			continue;
		}
		if (maxMaterialRows != 0 && outStats.materialRows + (uint32_t)builtMaterials.materials.size() > maxMaterialRows)
		{
			outStats.skippedMaterialBudget++;
			continue;
		}

		NRIPersistentVoxelMaterialClosureResult closure = {};
		if (!preloadServices.materialClosure.Register(
				buildSerial,
				variant.materialKeyHash,
				validatedMaterialSignature,
				builtMaterials,
				NRIPersistentVoxelMaterialClosureSource::PreloadKnown,
				closure))
		{
			outStats.failed++;
			ok = false;
			continue;
		}
		outStats.textureRequests += closure.textureDependencies;
		for (const NRIPersistentVoxelTextureDependency& dependency : closure.textures)
		{
			outStats.textureBytes += dependency.estimatedBytes;
		}

		PersistentVoxelMaterialVariantResource resource = {};
		resource.materialKeyHash = variant.materialKeyHash;
		resource.materialSignature = validatedMaterialSignature;
		resource.materialBridge = std::move(builtMaterials);
		resource.materialBridgeBuildSerial = buildSerial;
		resource.materialCount = (uint32_t)resource.materialBridge.materials.size();
		resource.lastDesiredMapGeneration = residencyMapGeneration;
		resource.lastUsedMapGeneration = residencyMapGeneration;
		resource.lastUsedFrame = frameIndex;
		resource.sourceBits |= variant.sourceBits;
		resource.priority = variant.priority;
		resource.gpuForce = variant.gpuForce;
		resource.gpuPrefer = variant.gpuPrefer;
		resource.materialPayloadHash = HashPersistentVoxelMaterialPayloadData(resource.materialBridge);
		resource.materialUploadHash = 0;
		resource.residentBytes =
			(uint64_t)resource.materialBridge.materials.size() * (uint64_t)sizeof(nri_scene::MaterialData);
		resource.cold = false;

		bool canonicalMaterialReused = false;
		if (!PublishCanonicalMaterialResource(resource, canonicalMaterialReused))
		{
			outStats.failed++;
			ok = false;
			continue;
		}

		outStats.selected++;
		if (canonicalMaterialReused)
		{
			outStats.reused++;
		}
		else
		{
			outStats.built++;
		}
		outStats.materialRows += resource.materialCount;
		outStats.materialBytes += resource.residentBytes;
	}

	outStats.uniqueMaterials = (uint32_t)seenMaterials.size();
	if (batch.valid)
	{
		RebuildBatchMaterialBridge(batch);
		RecomputeBatchState(batch);
	}
	outStats.buildMs = PersistentVoxelDurationMs(start, std::chrono::steady_clock::now());
	if (ok)
	{
		preloadMaterialClosureCached = true;
		preloadMaterialClosureBuildSerial = buildSerial;
		preloadMaterialClosureSignature = closureSignature;
		preloadMaterialClosureCacheHits = 0;
	}
	if (loadingTraceLevel >= 1 || voxelStatsEnabled || (int)nri_ptvoxelcomputetrace > 0)
	{
		Printf("NRI PT voxel preload material: level=%s build_serial=%llu frame=%u variants=%u direct=%u candidates=%u unique=%u selected=%u built=%u reused=%u failed=%u skipped_invalid=%u skipped_material_budget=%u actor_scoped=%u material_rows=%u material_bytes=%llu texture_requests=%u texture_bytes=%llu ms=%.3f\n",
			levelName != nullptr ? levelName : "unknown",
			(unsigned long long)buildSerial,
			frameIndex,
			outStats.variants,
			outStats.directOnlyVariants,
			outStats.candidates,
			outStats.uniqueMaterials,
			outStats.selected,
			outStats.built,
			outStats.reused,
			outStats.failed,
			outStats.skippedInvalid,
			outStats.skippedMaterialBudget,
			outStats.actorScopedMaterials,
			outStats.materialRows,
			(unsigned long long)outStats.materialBytes,
			outStats.textureRequests,
			(unsigned long long)outStats.textureBytes,
			outStats.buildMs);
		Printf("PERF pt voxel preload material NRI: level=%s build_serial=%llu frame=%u variants=%u direct=%u candidates=%u unique=%u selected=%u built=%u reused=%u failed=%u skipped_invalid=%u skipped_material_budget=%u actor_scoped=%u material_rows=%u material_bytes=%llu texture_requests=%u texture_bytes=%llu ms=%.3f\n",
			levelName != nullptr ? levelName : "unknown",
			(unsigned long long)buildSerial,
			frameIndex,
			outStats.variants,
			outStats.directOnlyVariants,
			outStats.candidates,
			outStats.uniqueMaterials,
			outStats.selected,
			outStats.built,
			outStats.reused,
			outStats.failed,
			outStats.skippedInvalid,
			outStats.skippedMaterialBudget,
			outStats.actorScopedMaterials,
			outStats.materialRows,
			(unsigned long long)outStats.materialBytes,
			outStats.textureRequests,
			(unsigned long long)outStats.textureBytes,
			outStats.buildMs);
	}
	return ok;
}

bool NRIPersistentVoxelResidency::PreloadResources(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	const std::vector<nri_scene::PersistentVoxelCacheEntryView>& cacheEntries,
	bool hasCacheEntries,
	bool gpuLoadingEnabled,
	bool preloadMaterialPayloads,
	uint32_t preloadMaterialMaxRows,
	uint64_t buildSerial,
	const char* levelName,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& resetServices,
	const NRIPersistentVoxelPreloadServices& preloadServices)
{
	lastPreloadStatus = {};
	lastPreloadStatus.gpuLoadingEnabled = gpuLoadingEnabled;
	lastPreloadStatus.hasCacheEntries = hasCacheEntries;
	lastPreloadStatus.batchReady = true;

	auto refreshAdmissionStatus = [&]()
	{
		CountAdmissionWork(
			lastPreloadStatus.requiredPending,
			lastPreloadStatus.requiredReady,
			lastPreloadStatus.optionalPending,
			lastPreloadStatus.failed);
		lastPreloadStatus.batchReadyActors = batch.activeActorCount;
	};

	if (!gpuLoadingEnabled)
	{
		refreshAdmissionStatus();
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT loading voxel resources: event=skip reason=gpu-disabled mesh_resources=%u material_resources=%u actors=%u active=%u prims=%u\n",
				(uint32_t)meshVariantResources.size(),
				(uint32_t)materialVariantResources.size(),
				(uint32_t)batch.actors.size(),
				batch.activeActorCount,
				batch.primitiveCount);
		}
		return true;
	}

	ReconcileResidency(
		variants,
		cacheEntries,
		buildSerial,
		levelName,
		frameIndex,
		settings,
		loadingTraceLevel,
		resetServices);

	if (preloadMaterialPayloads)
	{
		NRIPersistentVoxelMaterialPreloadStats materialPreloadStats = {};
		PreloadMaterialPayloads(
			variants,
			buildSerial,
			levelName,
			frameIndex,
			preloadMaterialMaxRows,
			loadingTraceLevel,
			voxelStatsEnabled,
			preloadServices,
			materialPreloadStats);
	}

	if (!PreloadVariantResources(
		variants,
		buildSerial,
		frameIndex,
		settings,
		loadingTraceLevel,
		voxelStatsEnabled,
		resetServices,
		preloadServices))
	{
		refreshAdmissionStatus();
		if (preloadPending)
		{
			lastPreloadStatus.batchReady = false;
			if (loadingTraceLevel >= 1)
			{
				uint32_t requiredPending = 0;
				uint32_t requiredReady = 0;
				uint32_t optionalPending = 0;
				uint32_t failed = 0;
				CountAdmissionWork(requiredPending, requiredReady, optionalPending, failed);
				Printf("NRI PT loading voxel resources: event=wait reason=variant-admission-pending required_pending=%u required_ready=%u optional_pending=%u failed=%u mesh_resources=%u material_resources=%u actors=%u active=%u prims=%u\n",
					requiredPending,
					requiredReady,
					optionalPending,
					failed,
					(uint32_t)meshVariantResources.size(),
					(uint32_t)materialVariantResources.size(),
					(uint32_t)batch.actors.size(),
					batch.activeActorCount,
					batch.primitiveCount);
			}
			return false;
		}
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT loading voxel resources: event=skip reason=variant-preload-disabled mesh_resources=%u material_resources=%u actors=%u active=%u prims=%u\n",
				(uint32_t)meshVariantResources.size(),
				(uint32_t)materialVariantResources.size(),
				(uint32_t)batch.actors.size(),
				batch.activeActorCount,
				batch.primitiveCount);
		}
		return true;
	}

	if (!hasCacheEntries)
	{
		const bool sharedBlasWarmupReady = preloadServices.WarmSharedBlas(variants, frameIndex);
		refreshAdmissionStatus();
		lastPreloadStatus.batchReady = sharedBlasWarmupReady;
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT loading voxel resources: event=skip reason=no-durable-entries entries=0 mesh_resources=%u material_resources=%u actors=%u active=%u prims=%u shared_blas_warmup=%u\n",
				(uint32_t)meshVariantResources.size(),
				(uint32_t)materialVariantResources.size(),
				(uint32_t)batch.actors.size(),
				batch.activeActorCount,
				batch.primitiveCount,
				sharedBlasWarmupReady ? 1u : 0u);
		}
		return sharedBlasWarmupReady;
	}

	const uint32_t meshResourcesBefore = (uint32_t)meshVariantResources.size();
	const uint32_t materialResourcesBefore = (uint32_t)materialVariantResources.size();
	const uint32_t actorsBefore = (uint32_t)batch.actors.size();
	const uint32_t activeActorsBefore = batch.activeActorCount;
	const uint32_t primitivesBefore = batch.primitiveCount;
	const auto start = std::chrono::steady_clock::now();

	struct LoadingWarmupScope
	{
		bool& active;
		explicit LoadingWarmupScope(bool& value) : active(value) { active = true; }
		~LoadingWarmupScope() { active = false; }
	} loadingWarmupScope(loadingWarmupActive);

	NRIPersistentVoxelBatchStats batchStats = {};
	const bool ready = preloadServices.EnsureBatch(&batchStats);
	bool sharedBlasWarmupReady = true;
	if (ready)
	{
		sharedBlasWarmupReady = preloadServices.WarmSharedBlas(variants, frameIndex);
	}
	const auto end = std::chrono::steady_clock::now();
	refreshAdmissionStatus();
	const bool admissionOnlyDeadEnd =
		!ready &&
		meshVariantResources.empty() &&
		batch.actors.empty() &&
		batchStats.persistentVoxelOnboardingAdmissionPendingCount > 0 &&
		batchStats.persistentVoxelOnboardingAdmissionPendingCount == batchStats.persistentVoxelOnboardingDeferredCount &&
		batchStats.persistentVoxelTexturePrewarmDeferredCount == 0 &&
		batchStats.persistentVoxelOnboardingTexturePrewarmDeferredCount == 0 &&
		batchStats.persistentVoxelOnboardingTextureBudgetHits == 0;
	uint32_t runtimeTailHeldActors = 0;
	if (!IsNRIVoxelComputePreloadRuntimeTailReleased(buildSerial))
	{
		for (const nri_scene::PersistentVoxelCacheEntryView& cacheEntry : cacheEntries)
		{
			if (IsNRIVoxelComputePreloadRuntimeWithheldMesh(
					buildSerial,
					BuildPersistentVoxelMeshResourceKey(cacheEntry, settings)))
			{
				runtimeTailHeldActors++;
			}
		}
	}
	const bool runtimeTailHoldDeadEnd =
		!ready &&
		lastPreloadStatus.requiredPending == 0 &&
		lastPreloadStatus.optionalPending == 0 &&
		runtimeTailHeldActors != 0 &&
		batchStats.persistentVoxelOnboardingAdmissionPendingCount != 0 &&
		batchStats.persistentVoxelOnboardingAdmissionPendingCount <= runtimeTailHeldActors &&
		batchStats.persistentVoxelTexturePrewarmDeferredCount == 0 &&
		batchStats.persistentVoxelOnboardingTexturePrewarmDeferredCount == 0 &&
		batchStats.persistentVoxelOnboardingTextureBudgetHits == 0;
	lastPreloadStatus.batchReady = (ready || admissionOnlyDeadEnd || runtimeTailHoldDeadEnd) && sharedBlasWarmupReady;
	lastPreloadStatus.batchReadyActors = batch.activeActorCount;
	lastPreloadStatus.deferredTexturePrewarm =
		batchStats.persistentVoxelTexturePrewarmDeferredCount +
		batchStats.persistentVoxelOnboardingTextureBudgetHits;
	lastPreloadStatus.deferredOnboarding = (admissionOnlyDeadEnd || runtimeTailHoldDeadEnd)
		? 0u
		: (batchStats.persistentVoxelOnboardingDeferredCount > batchStats.persistentVoxelOnboardingTextureBudgetHits
			? batchStats.persistentVoxelOnboardingDeferredCount - batchStats.persistentVoxelOnboardingTextureBudgetHits
			: 0u);
	lastPreloadStatus.batchPendingActors = lastPreloadStatus.batchReady ? 0u : std::max<uint32_t>(
		1u,
		lastPreloadStatus.deferredTexturePrewarm + lastPreloadStatus.deferredOnboarding);
	if (!lastPreloadStatus.batchReady)
	{
		preloadPending = true;
	}

	if (loadingTraceLevel >= 1)
	{
		Printf("NRI PT loading voxel resources: event=%s entries=%u mesh_resources=%u mesh_delta=%d material_resources=%u material_delta=%d actors=%u actor_delta=%d active=%u active_delta=%d prims=%u prim_delta=%d batch_pending=%u deferred_texture_prewarm=%u deferred_onboarding=%u ms=%.3f\n",
			ready && sharedBlasWarmupReady ? "admit" : "defer",
			(uint32_t)cacheEntries.size(),
			(uint32_t)meshVariantResources.size(),
			(int32_t)meshVariantResources.size() - (int32_t)meshResourcesBefore,
			(uint32_t)materialVariantResources.size(),
			(int32_t)materialVariantResources.size() - (int32_t)materialResourcesBefore,
			(uint32_t)batch.actors.size(),
			(int32_t)batch.actors.size() - (int32_t)actorsBefore,
			batch.activeActorCount,
			(int32_t)batch.activeActorCount - (int32_t)activeActorsBefore,
			batch.primitiveCount,
			(int32_t)batch.primitiveCount - (int32_t)primitivesBefore,
			lastPreloadStatus.batchPendingActors,
			lastPreloadStatus.deferredTexturePrewarm,
			lastPreloadStatus.deferredOnboarding,
			PersistentVoxelDurationMs(start, end));
		if (admissionOnlyDeadEnd)
		{
			Printf("NRI PT loading voxel resources: event=release reason=admission-only-dead-end entries=%u material_resources=%u admission_pending=%u\n",
				(uint32_t)cacheEntries.size(),
				(uint32_t)materialVariantResources.size(),
				batchStats.persistentVoxelOnboardingAdmissionPendingCount);
		}
		if (runtimeTailHoldDeadEnd)
		{
			Printf("NRI PT loading voxel resources: event=release reason=runtime-tail-held entries=%u admission_pending=%u held_actors=%u\n",
				(uint32_t)cacheEntries.size(),
				batchStats.persistentVoxelOnboardingAdmissionPendingCount,
				runtimeTailHeldActors);
		}
	}
	return lastPreloadStatus.batchReady;
}

PersistentVoxelReadinessStatus NRIPersistentVoxelResidency::GetSharedVariantReadiness(uint64_t meshResourceKey, uint64_t materialKeyHash) const
{
	PersistentVoxelReadinessStatus status = {};
	status.meshPublished = publishedMeshKeys.find(meshResourceKey) != publishedMeshKeys.end();
	status.materialPublished = publishedMaterialKeys.find(materialKeyHash) != publishedMaterialKeys.end();

	auto meshIt = meshVariantResources.find(meshResourceKey);
	if (meshIt == meshVariantResources.end())
	{
		status.reason = "mesh-missing";
		return status;
	}
	const PersistentVoxelMeshVariantResource& meshResource = meshIt->second;
	status.meshPresent = true;
	status.meshResourceKey = meshResource.resourceKey;
	status.meshKeyMatches = meshResource.resourceKey == meshResourceKey;
	status.meshVertexCount = meshResource.vertexCount;
	status.meshIndexCount = meshResource.indexCount;
	status.meshPrimitiveCount = meshResource.primitiveCount;
	status.meshCountsValid =
		meshResource.vertexCount != 0 &&
		meshResource.indexCount != 0 &&
		meshResource.primitiveCount != 0;
	status.meshPrivateBuffersReady =
		meshResource.vertexBuffer.buffer != nullptr &&
		meshResource.indexBuffer.buffer != nullptr;
	status.meshDirectBuffersReady = meshResource.directComputePublished;
	status.meshArenaBuffersReady =
		vertexBuffer.buffer != nullptr &&
		indexBuffer.buffer != nullptr &&
		primitiveBuffer.buffer != nullptr;
	status.blasReady = meshResource.accelerationStructure.accelerationStructure != nullptr;
	if (!status.meshKeyMatches || !status.meshCountsValid || (!status.meshPrivateBuffersReady && !status.meshDirectBuffersReady))
	{
		status.reason = "mesh-invalid";
		return status;
	}
	if (!status.meshArenaBuffersReady)
	{
		status.reason = "arena-missing";
		return status;
	}
	if (!status.blasReady)
	{
		status.reason = "blas-missing";
		return status;
	}

	auto materialIt = materialVariantResources.find(materialKeyHash);
	if (materialIt == materialVariantResources.end())
	{
		status.reason = "material-missing";
		return status;
	}
	const PersistentVoxelMaterialVariantResource& materialResource = materialIt->second;
	status.materialPresent = true;
	status.materialKeyMatches = materialResource.materialKeyHash == materialKeyHash;
	status.materialCount = materialResource.materialCount;
	status.materialBridgeCount = (uint32_t)materialResource.materialBridge.materials.size();
	status.materialCountValid = materialResource.materialCount != 0;
	status.materialBridgeReady = !materialResource.materialBridge.materials.empty();
	if (!status.materialKeyMatches || !status.materialCountValid || !status.materialBridgeReady)
	{
		status.reason = "material-invalid";
		return status;
	}

	status.reason = "ready";
	status.ready = true;
	return status;
}

bool NRIPersistentVoxelResidency::AppendMaterialTextureKeys(
	uint64_t materialKeyHash,
	std::vector<uint64_t>& outKeys) const
{
	const auto resourceIt = materialVariantResources.find(materialKeyHash);
	if (resourceIt == materialVariantResources.end() ||
		resourceIt->second.materialKeyHash != materialKeyHash ||
		resourceIt->second.materialCount == 0 ||
		resourceIt->second.materialBridge.materials.empty())
	{
		return false;
	}
	for (const nri_scene::TextureUpload& texture : resourceIt->second.materialBridge.textures)
	{
		if (texture.key != 0)
		{
			outKeys.push_back(texture.key);
		}
	}
	return true;
}

bool NRIPersistentVoxelResidency::IsSharedVariantReady(uint64_t meshResourceKey, uint64_t materialKeyHash) const
{
	return GetSharedVariantReadiness(meshResourceKey, materialKeyHash).ready;
}

bool NRIPersistentVoxelResidency::IsRequiredAdmission(const PersistentVoxelAdmissionEntry& entry) const
{
	return
		entry.mapGeneration == residencyMapGeneration &&
		!entry.runtimeRequested &&
		entry.priority <= 0 &&
		(entry.sourceBits & nri_scene::PrecachedVoxelSourceBit_MountedVoxelPreload) != 0 &&
		(entry.gpuForce ||
			(entry.gpuPrefer && (entry.sourceBits & nri_scene::PrecachedVoxelSourceBit_MountedPreloadMap) != 0));
}

NRIPersistentVoxelAdmissionBucket NRIPersistentVoxelResidency::GetAdmissionBucket(
	const PersistentVoxelAdmissionEntry& entry) const
{
	if (entry.state == PersistentVoxelAdmissionState::Ready)
	{
		return IsRequiredAdmission(entry) ?
			NRIPersistentVoxelAdmissionBucket::RequiredReady :
			NRIPersistentVoxelAdmissionBucket::OptionalReady;
	}
	if (entry.state == PersistentVoxelAdmissionState::Failed)
	{
		return NRIPersistentVoxelAdmissionBucket::Failed;
	}
	return NRIPersistentVoxelAdmissionBucket::Active;
}

void NRIPersistentVoxelResidency::RebuildAdmissionIndex(bool reactivateStaleReady)
{
	admissionIndex.Clear();
	for (auto& pair : admissionQueue)
	{
		PersistentVoxelAdmissionEntry& entry = pair.second;
		if (entry.mapGeneration != residencyMapGeneration)
		{
			continue;
		}
		if (reactivateStaleReady &&
			entry.state == PersistentVoxelAdmissionState::Ready &&
			!IsSharedVariantReady(BuildPersistentVoxelVariantMeshResourceKey(entry.variant), entry.variant.materialKeyHash))
		{
			entry.state = PersistentVoxelAdmissionState::Pending;
			entry.lastReason = "resource-invalidated";
		}
		admissionIndex.Add(pair.first, GetAdmissionBucket(entry));
	}
}

void NRIPersistentVoxelResidency::MarkMaintenanceMutation()
{
	maintenanceMutationGeneration++;
	if (maintenanceMutationGeneration == 0)
	{
		maintenanceMutationGeneration = 1;
		cachedMemoryUsageGeneration = 0;
		cachedResourceStatusGeneration = 0;
		pressureEvaluationGeneration = 0;
	}
}

void NRIPersistentVoxelResidency::CountAdmissionWork(uint32_t& requiredPending, uint32_t& requiredReady, uint32_t& optionalPending, uint32_t& failed) const
{
	requiredPending = 0;
	requiredReady = admissionIndex.RequiredReadyCount();
	optionalPending = 0;
	failed = admissionIndex.FailedCount();

	for (uint64_t key : admissionIndex.ActiveKeys())
	{
		const auto found = admissionQueue.find(key);
		if (found == admissionQueue.end())
		{
			continue;
		}
		const PersistentVoxelAdmissionEntry& entry = found->second;
		const bool required = IsRequiredAdmission(entry);
		if (required)
		{
			requiredPending++;
		}
		else
		{
			optionalPending++;
		}
	}
}

void NRIPersistentVoxelResidency::TraceReadiness(
	const char* event,
	const char* phase,
	const PersistentVoxelAdmissionEntry* entry,
	uint64_t meshResourceKey,
	uint64_t materialKeyHash,
	const PersistentVoxelReadinessStatus& status,
	bool traceEnabled) const
{
	if (!traceEnabled)
	{
		return;
	}
	auto stateName = [](PersistentVoxelAdmissionState state) -> const char*
	{
		switch (state)
		{
		case PersistentVoxelAdmissionState::Pending: return "pending";
		case PersistentVoxelAdmissionState::DirectComputePending: return "direct-compute-pending";
		case PersistentVoxelAdmissionState::DirectBlasPending: return "direct-blas-pending";
		case PersistentVoxelAdmissionState::UploadingVertices: return "uploading-vertices";
		case PersistentVoxelAdmissionState::UploadingIndices: return "uploading-indices";
		case PersistentVoxelAdmissionState::UploadingPrimitives: return "uploading-primitives";
		case PersistentVoxelAdmissionState::BuildingBlas: return "building-blas";
		case PersistentVoxelAdmissionState::Ready: return "ready";
		case PersistentVoxelAdmissionState::Deferred: return "deferred";
		case PersistentVoxelAdmissionState::Failed: return "failed";
		default: return "unknown";
		}
	};

	Printf("NRI PT voxel readiness: event=%s phase=%s reason=%s source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx queue_state=%s published_mesh=%u published_material=%u generation=%u mesh_present=%u mesh_resource=0x%llx mesh_key_match=%u mesh_counts=%u mesh_private=%u mesh_direct=%u arena=%u blas=%u material_present=%u material_key_match=%u material_count=%u material_bridge=%u vertices=%u indices=%u prims=%u material_count_value=%u material_bridge_count=%u\n",
		event != nullptr ? event : "unknown",
		phase != nullptr ? phase : "unknown",
		status.reason != nullptr ? status.reason : "unknown",
		entry != nullptr ? entry->sourceBits : 0u,
		entry != nullptr ? entry->priority : 0,
		entry != nullptr ? entry->admissionRank : 0,
		entry != nullptr && entry->gpuForce ? 1u : 0u,
		entry != nullptr && entry->gpuPrefer ? 1u : 0u,
		entry != nullptr && entry->runtimeRequested ? 1u : 0u,
		entry != nullptr ? entry->variant.sourcePicnum : -1,
		entry != nullptr ? entry->variant.resolvedVoxelIndex : -1,
		(unsigned long long)meshResourceKey,
		(unsigned long long)materialKeyHash,
		entry != nullptr ? stateName(entry->state) : "none",
		status.meshPublished ? 1u : 0u,
		status.materialPublished ? 1u : 0u,
		residencyMapGeneration,
		status.meshPresent ? 1u : 0u,
		(unsigned long long)status.meshResourceKey,
		status.meshKeyMatches ? 1u : 0u,
		status.meshCountsValid ? 1u : 0u,
		status.meshPrivateBuffersReady ? 1u : 0u,
		status.meshDirectBuffersReady ? 1u : 0u,
		status.meshArenaBuffersReady ? 1u : 0u,
		status.blasReady ? 1u : 0u,
		status.materialPresent ? 1u : 0u,
		status.materialKeyMatches ? 1u : 0u,
		status.materialCountValid ? 1u : 0u,
		status.materialBridgeReady ? 1u : 0u,
		status.meshVertexCount,
		status.meshIndexCount,
		status.meshPrimitiveCount,
		status.materialCount,
		status.materialBridgeCount);
}
