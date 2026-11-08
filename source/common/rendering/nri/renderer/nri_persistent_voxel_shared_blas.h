#pragma once

#include "nri_resources.h"

#include <cstring>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

enum class NRIPersistentVoxelSharedBlasState : uint8_t
{
	Missing,
	Building,
	Resident,
	Failed
};

struct NRIPersistentVoxelSharedBlasFrameStats
{
	uint32_t activeActors = 0;
	uint32_t uniqueDesiredKeys = 0;
	uint32_t residentSharedAssets = 0;
	uint32_t queuedSharedAssets = 0;
	uint32_t eligibleBuildKeys = 0;
	uint32_t buildAttempts = 0;
	uint32_t buildSuccesses = 0;
	uint32_t buildFailures = 0;
	uint32_t cacheHits = 0;
	uint32_t cacheMisses = 0;
	uint32_t actorRefs = 0;
	uint32_t routedLegacy = 0;
	uint32_t routedShared = 0;
	uint32_t fallbackLastValid = 0;
	uint32_t activeReferencedAssets = 0;
	uint32_t unreferencedResidentAssets = 0;
	uint64_t residentBytes = 0;
	uint64_t activeReferencedBytes = 0;
	uint64_t unreferencedResidentBytes = 0;
	uint32_t routeEligibleActors = 0;
	uint32_t routeRejectMissingResident = 0;
	uint32_t routeRejectNonLocal = 0;
	uint32_t routeRejectTransformKeyed = 0;
	uint32_t routeRejectInvalidMaterial = 0;
	uint32_t routeRejectInvalidTransform = 0;
	uint32_t routeRejectGeometryMismatch = 0;
	uint32_t rejectMissingKey = 0;
	uint32_t rejectDisabled = 0;
	uint32_t rejectNonLocal = 0;
	uint32_t rejectTransformKeyed = 0;
	uint32_t rejectMissingBuffers = 0;
	uint32_t rejectInvalidCounts = 0;
	uint32_t rejectBuildBudget = 0;
	uint32_t rejectGeometryMismatch = 0;
	uint32_t profileActiveActors = 0;
	uint32_t profileLocalSpaceActors = 0;
	uint32_t profileBakedTransformActors = 0;
	uint32_t profileUnknownSpaceActors = 0;
	uint32_t profileTransformKeyedActors = 0;
	uint32_t profileLocalIdentityTransformActors = 0;
	uint32_t profileLocalNonIdentityTransformActors = 0;
	uint32_t profileShareableLocalActors = 0;
	uint32_t profileShareableUniqueKeys = 0;
	uint32_t profileShareableDuplicateActorRefs = 0;
	uint32_t profileShareableSingleActorKeys = 0;
	uint32_t profileShareableMultiActorKeys = 0;
	uint32_t profileResidentShareableKeys = 0;
	uint32_t profileEligibleNotResidentActors = 0;
	uint32_t profileRejectMissingMesh = 0;
	uint32_t profileRejectNonLocal = 0;
	uint32_t profileRejectTransformKeyed = 0;
	uint32_t profileRejectMissingBuffers = 0;
	uint32_t profileRejectInvalidCounts = 0;
	uint32_t profileRejectInvalidMaterial = 0;
	uint32_t profileRejectInvalidTransform = 0;
	uint32_t profileRejectGeometryMismatch = 0;
	uint32_t keyAuditActors = 0;
	uint32_t keyAuditKeys = 0;
	uint32_t keyAuditSafeKeys = 0;
	uint32_t keyAuditUnsafeKeys = 0;
	uint32_t keyAuditGeometryMismatchKeys = 0;
	uint32_t keyAuditCountMismatchKeys = 0;
	uint32_t keyAuditMaterialVariantKeys = 0;
	uint32_t keyAuditMaterialCountMismatchKeys = 0;
	uint32_t keyAuditSourcePicnumAliasKeys = 0;
	uint32_t keyAuditVoxelIndexAliasKeys = 0;
	uint32_t keyAuditSourceStateAliasActorRefs = 0;
	uint32_t keyAuditBakeSpaceMismatchKeys = 0;
	uint32_t keyAuditTransformBasisMismatchKeys = 0;
	uint32_t keyAuditLocalShareableUnsafeKeys = 0;
	uint32_t invariantLocalActors = 0;
	uint32_t invariantLocalIdentityTransformActors = 0;
	uint32_t invariantLocalNonIdentityTransformActors = 0;
	uint32_t invariantSuspiciousWorldBoundsActors = 0;
	uint32_t invariantMissingBoundsActors = 0;
	uint32_t invariantInvalidTransformActors = 0;
	uint32_t invariantBakedFallbackActors = 0;
	uint32_t invariantUnknownSpaceActors = 0;
	float invariantMaxBoundsCenterMagnitude = 0.0f;
	float invariantMaxBoundsAbs = 0.0f;
};

struct NRIPersistentVoxelLocalShareProfileStats
{
	uint32_t activeActors = 0;
	uint32_t localSpaceActors = 0;
	uint32_t bakedTransformActors = 0;
	uint32_t unknownSpaceActors = 0;
	uint32_t transformKeyedActors = 0;
	uint32_t localIdentityTransformActors = 0;
	uint32_t localNonIdentityTransformActors = 0;
	uint32_t shareableLocalActors = 0;
	uint32_t shareableUniqueKeys = 0;
	uint32_t shareableDuplicateActorRefs = 0;
	uint32_t shareableSingleActorKeys = 0;
	uint32_t shareableMultiActorKeys = 0;
	uint32_t residentShareableKeys = 0;
	uint32_t eligibleNotResidentActors = 0;
	uint32_t rejectMissingMesh = 0;
	uint32_t rejectNonLocal = 0;
	uint32_t rejectTransformKeyed = 0;
	uint32_t rejectMissingBuffers = 0;
	uint32_t rejectInvalidCounts = 0;
	uint32_t rejectInvalidMaterial = 0;
	uint32_t rejectInvalidTransform = 0;
	uint32_t rejectGeometryMismatch = 0;
};

struct NRIPersistentVoxelSharedKeyAuditStats
{
	uint32_t actors = 0;
	uint32_t keys = 0;
	uint32_t safeKeys = 0;
	uint32_t unsafeKeys = 0;
	uint32_t geometryMismatchKeys = 0;
	uint32_t countMismatchKeys = 0;
	uint32_t materialVariantKeys = 0;
	uint32_t materialCountMismatchKeys = 0;
	uint32_t sourcePicnumAliasKeys = 0;
	uint32_t voxelIndexAliasKeys = 0;
	uint32_t sourceStateAliasActorRefs = 0;
	uint32_t bakeSpaceMismatchKeys = 0;
	uint32_t transformBasisMismatchKeys = 0;
	uint32_t localShareableUnsafeKeys = 0;
};

struct NRIPersistentVoxelLocalSpaceInvariantStats
{
	uint32_t localActors = 0;
	uint32_t localIdentityTransformActors = 0;
	uint32_t localNonIdentityTransformActors = 0;
	uint32_t suspiciousWorldBoundsActors = 0;
	uint32_t missingBoundsActors = 0;
	uint32_t invalidTransformActors = 0;
	uint32_t bakedFallbackActors = 0;
	uint32_t unknownSpaceActors = 0;
	float maxBoundsCenterMagnitude = 0.0f;
	float maxBoundsAbs = 0.0f;
};

struct NRIPersistentVoxelSharedBlasEntry
{
	uint64_t key = 0;
	uint64_t geometrySignature = 0;
	uint32_t primitiveCount = 0;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t lastUsedFrame = 0;
	uint32_t frameReferences = 0;
	uint64_t memoryEstimateBytes = 0;
	NRIPersistentVoxelSharedBlasState state = NRIPersistentVoxelSharedBlasState::Missing;
	NRIAccelerationStructureResource accelerationStructure;
};

class NRIPersistentVoxelSharedBlasCache
{
public:
	template <typename RetireAccelerationStructureFn>
	void RetireKey(uint64_t key, RetireAccelerationStructureFn&& retireAccelerationStructure)
	{
		auto found = mEntries.find(key);
		if (found == mEntries.end()) return;
		retireAccelerationStructure(found->second.accelerationStructure);
		mEntries.erase(found);
	}

	template <typename RetireAccelerationStructureFn>
	void RetireAll(RetireAccelerationStructureFn&& retireAccelerationStructure)
	{
		for (auto& pair : mEntries)
		{
			retireAccelerationStructure(pair.second.accelerationStructure);
		}
		mEntries.clear();
		ResetFrameStats();
	}

	void BeginFrame()
	{
		ResetFrameStats();
		for (auto& pair : mEntries)
		{
			pair.second.frameReferences = 0;
		}
	}

	void RecordLegacyActor(uint64_t sharedBlasKey)
	{
		mFrameStats.activeActors++;
		mFrameStats.actorRefs++;
		mFrameStats.routedLegacy++;
		if (sharedBlasKey == 0)
		{
			mFrameStats.rejectMissingKey++;
			return;
		}
		mDesiredKeys.insert(sharedBlasKey);
		auto entryIt = mEntries.find(sharedBlasKey);
		if (entryIt != mEntries.end() && entryIt->second.state == NRIPersistentVoxelSharedBlasState::Resident)
		{
			entryIt->second.frameReferences++;
			mFrameStats.cacheHits++;
		}
		else
		{
			mFrameStats.cacheMisses++;
		}
	}

	void RecordSharedActor(uint64_t sharedBlasKey)
	{
		mFrameStats.activeActors++;
		mFrameStats.actorRefs++;
		mFrameStats.routedShared++;
		if (sharedBlasKey == 0)
		{
			mFrameStats.rejectMissingKey++;
			return;
		}
		mDesiredKeys.insert(sharedBlasKey);
		auto entryIt = mEntries.find(sharedBlasKey);
		if (entryIt != mEntries.end() && entryIt->second.state == NRIPersistentVoxelSharedBlasState::Resident)
		{
			entryIt->second.frameReferences++;
			mFrameStats.cacheHits++;
		}
		else
		{
			mFrameStats.cacheMisses++;
		}
	}

	void RecordRouteEligibleActor()
	{
		mFrameStats.routeEligibleActors++;
	}

	void RecordLocalShareProfile(const NRIPersistentVoxelLocalShareProfileStats& profile)
	{
		mFrameStats.profileActiveActors = profile.activeActors;
		mFrameStats.profileLocalSpaceActors = profile.localSpaceActors;
		mFrameStats.profileBakedTransformActors = profile.bakedTransformActors;
		mFrameStats.profileUnknownSpaceActors = profile.unknownSpaceActors;
		mFrameStats.profileTransformKeyedActors = profile.transformKeyedActors;
		mFrameStats.profileLocalIdentityTransformActors = profile.localIdentityTransformActors;
		mFrameStats.profileLocalNonIdentityTransformActors = profile.localNonIdentityTransformActors;
		mFrameStats.profileShareableLocalActors = profile.shareableLocalActors;
		mFrameStats.profileShareableUniqueKeys = profile.shareableUniqueKeys;
		mFrameStats.profileShareableDuplicateActorRefs = profile.shareableDuplicateActorRefs;
		mFrameStats.profileShareableSingleActorKeys = profile.shareableSingleActorKeys;
		mFrameStats.profileShareableMultiActorKeys = profile.shareableMultiActorKeys;
		mFrameStats.profileResidentShareableKeys = profile.residentShareableKeys;
		mFrameStats.profileEligibleNotResidentActors = profile.eligibleNotResidentActors;
		mFrameStats.profileRejectMissingMesh = profile.rejectMissingMesh;
		mFrameStats.profileRejectNonLocal = profile.rejectNonLocal;
		mFrameStats.profileRejectTransformKeyed = profile.rejectTransformKeyed;
		mFrameStats.profileRejectMissingBuffers = profile.rejectMissingBuffers;
		mFrameStats.profileRejectInvalidCounts = profile.rejectInvalidCounts;
		mFrameStats.profileRejectInvalidMaterial = profile.rejectInvalidMaterial;
		mFrameStats.profileRejectInvalidTransform = profile.rejectInvalidTransform;
		mFrameStats.profileRejectGeometryMismatch = profile.rejectGeometryMismatch;
	}

	void RecordSharedKeyAudit(const NRIPersistentVoxelSharedKeyAuditStats& audit)
	{
		mFrameStats.keyAuditActors = audit.actors;
		mFrameStats.keyAuditKeys = audit.keys;
		mFrameStats.keyAuditSafeKeys = audit.safeKeys;
		mFrameStats.keyAuditUnsafeKeys = audit.unsafeKeys;
		mFrameStats.keyAuditGeometryMismatchKeys = audit.geometryMismatchKeys;
		mFrameStats.keyAuditCountMismatchKeys = audit.countMismatchKeys;
		mFrameStats.keyAuditMaterialVariantKeys = audit.materialVariantKeys;
		mFrameStats.keyAuditMaterialCountMismatchKeys = audit.materialCountMismatchKeys;
		mFrameStats.keyAuditSourcePicnumAliasKeys = audit.sourcePicnumAliasKeys;
		mFrameStats.keyAuditVoxelIndexAliasKeys = audit.voxelIndexAliasKeys;
		mFrameStats.keyAuditSourceStateAliasActorRefs = audit.sourceStateAliasActorRefs;
		mFrameStats.keyAuditBakeSpaceMismatchKeys = audit.bakeSpaceMismatchKeys;
		mFrameStats.keyAuditTransformBasisMismatchKeys = audit.transformBasisMismatchKeys;
		mFrameStats.keyAuditLocalShareableUnsafeKeys = audit.localShareableUnsafeKeys;
	}

	void RecordLocalSpaceInvariantStats(const NRIPersistentVoxelLocalSpaceInvariantStats& invariants)
	{
		mFrameStats.invariantLocalActors = invariants.localActors;
		mFrameStats.invariantLocalIdentityTransformActors = invariants.localIdentityTransformActors;
		mFrameStats.invariantLocalNonIdentityTransformActors = invariants.localNonIdentityTransformActors;
		mFrameStats.invariantSuspiciousWorldBoundsActors = invariants.suspiciousWorldBoundsActors;
		mFrameStats.invariantMissingBoundsActors = invariants.missingBoundsActors;
		mFrameStats.invariantInvalidTransformActors = invariants.invalidTransformActors;
		mFrameStats.invariantBakedFallbackActors = invariants.bakedFallbackActors;
		mFrameStats.invariantUnknownSpaceActors = invariants.unknownSpaceActors;
		mFrameStats.invariantMaxBoundsCenterMagnitude = invariants.maxBoundsCenterMagnitude;
		mFrameStats.invariantMaxBoundsAbs = invariants.maxBoundsAbs;
	}

	void RecordRouteFallback(uint64_t sharedBlasKey, const char* reason)
	{
		mFrameStats.fallbackLastValid++;
		if (sharedBlasKey == 0 || reason == nullptr)
		{
			return;
		}
		if (std::strcmp(reason, "missing-resident") == 0)
		{
			mFrameStats.routeRejectMissingResident++;
		}
		else if (std::strcmp(reason, "non-local") == 0)
		{
			mFrameStats.routeRejectNonLocal++;
		}
		else if (std::strcmp(reason, "transform-keyed") == 0)
		{
			mFrameStats.routeRejectTransformKeyed++;
		}
		else if (std::strcmp(reason, "invalid-material") == 0)
		{
			mFrameStats.routeRejectInvalidMaterial++;
		}
		else if (std::strcmp(reason, "invalid-transform") == 0)
		{
			mFrameStats.routeRejectInvalidTransform++;
		}
		else if (std::strcmp(reason, "geometry-mismatch") == 0)
		{
			mFrameStats.routeRejectGeometryMismatch++;
		}
	}

	void RecordBuildDisabled(uint64_t sharedBlasKey)
	{
		if (sharedBlasKey != 0 && mRejectedBuildKeys.insert(sharedBlasKey).second)
		{
			mFrameStats.rejectDisabled++;
		}
	}

	void RecordBuildReject(uint64_t sharedBlasKey, const char* reason)
	{
		if (sharedBlasKey == 0 || !mRejectedBuildKeys.insert(sharedBlasKey).second)
		{
			return;
		}
		if (reason == nullptr)
		{
			return;
		}
		if (std::strcmp(reason, "non-local") == 0)
		{
			mFrameStats.rejectNonLocal++;
		}
		else if (std::strcmp(reason, "transform-keyed") == 0)
		{
			mFrameStats.rejectTransformKeyed++;
		}
		else if (std::strcmp(reason, "missing-buffers") == 0)
		{
			mFrameStats.rejectMissingBuffers++;
		}
		else if (std::strcmp(reason, "invalid-counts") == 0)
		{
			mFrameStats.rejectInvalidCounts++;
		}
		else if (std::strcmp(reason, "build-budget") == 0)
		{
			mFrameStats.rejectBuildBudget++;
		}
		else if (std::strcmp(reason, "geometry-mismatch") == 0)
		{
			mFrameStats.rejectGeometryMismatch++;
		}
	}

	NRIPersistentVoxelSharedBlasEntry& PrepareBuild(
		uint64_t sharedBlasKey,
		uint64_t geometrySignature,
		uint32_t vertexCount,
		uint32_t indexCount,
		uint32_t primitiveCount,
		uint32_t frameIndex)
	{
		NRIPersistentVoxelSharedBlasEntry& entry = mEntries[sharedBlasKey];
		entry.key = sharedBlasKey;
		entry.geometrySignature = geometrySignature;
		entry.vertexCount = vertexCount;
		entry.indexCount = indexCount;
		entry.primitiveCount = primitiveCount;
		entry.lastUsedFrame = frameIndex;
		entry.state = NRIPersistentVoxelSharedBlasState::Building;
		mFrameStats.eligibleBuildKeys++;
		mFrameStats.buildAttempts++;
		return entry;
	}

	void MarkBuildSuccess(NRIPersistentVoxelSharedBlasEntry& entry, uint32_t frameIndex)
	{
		entry.lastUsedFrame = frameIndex;
		entry.memoryEstimateBytes = entry.accelerationStructure.memorySize;
		entry.state = NRIPersistentVoxelSharedBlasState::Resident;
		mFrameStats.buildSuccesses++;
	}

	void MarkBuildFailure(NRIPersistentVoxelSharedBlasEntry& entry)
	{
		entry.state = NRIPersistentVoxelSharedBlasState::Failed;
		entry.memoryEstimateBytes = 0;
		mFrameStats.buildFailures++;
	}

	const NRIPersistentVoxelSharedBlasEntry* Find(uint64_t sharedBlasKey) const
	{
		auto entryIt = mEntries.find(sharedBlasKey);
		return entryIt != mEntries.end() ? &entryIt->second : nullptr;
	}

	NRIPersistentVoxelSharedBlasFrameStats EndFrame()
	{
		mFrameStats.uniqueDesiredKeys = (uint32_t)mDesiredKeys.size();
		mFrameStats.residentSharedAssets = 0;
		mFrameStats.queuedSharedAssets = 0;
		for (const auto& pair : mEntries)
		{
			if (pair.second.state == NRIPersistentVoxelSharedBlasState::Resident)
			{
				mFrameStats.residentSharedAssets++;
				mFrameStats.residentBytes += pair.second.memoryEstimateBytes;
				if (pair.second.frameReferences != 0)
				{
					mFrameStats.activeReferencedAssets++;
					mFrameStats.activeReferencedBytes += pair.second.memoryEstimateBytes;
				}
				else
				{
					mFrameStats.unreferencedResidentAssets++;
					mFrameStats.unreferencedResidentBytes += pair.second.memoryEstimateBytes;
				}
			}
			else if (pair.second.state == NRIPersistentVoxelSharedBlasState::Building)
			{
				mFrameStats.queuedSharedAssets++;
			}
		}
		return mFrameStats;
	}

	const NRIPersistentVoxelSharedBlasFrameStats& LastFrameStats() const
	{
		return mFrameStats;
	}

private:
	void ResetFrameStats()
	{
		mDesiredKeys.clear();
		mRejectedBuildKeys.clear();
		mFrameStats = {};
	}

	std::unordered_map<uint64_t, NRIPersistentVoxelSharedBlasEntry> mEntries;
	std::unordered_set<uint64_t> mDesiredKeys;
	std::unordered_set<uint64_t> mRejectedBuildKeys;
	NRIPersistentVoxelSharedBlasFrameStats mFrameStats = {};
};
