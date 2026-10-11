#include "nri_persistent_voxel_publication_services.h"
#include "../scene/nri_hash.h"
#include "nri_cvars.h"
#include "printf.h"

#include <algorithm>
#include <cstring>

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

void NRIPersistentVoxelResidency::ResetActorPublicationConsumer()
{
	actorPublicationCursor.Reset();
	actorPublication.reset();
	publicationActorIndices.clear();
	publicationLegacyEntries.clear();
	publicationTransformDirty.clear();
	publicationFastActors.clear();
	publicationAdmissionOrder.Reset();
	publicationTlasOrder.Reset();
	publicationTlasActors.clear();
	publicationBatchReady = false;
	publicationConsumerQuarantined = false;
	publicationResynchronized = false;
	publicationValidateThisFrame = false;
}

void NRIPersistentVoxelResidency::RememberPublishedActors(bool buildPending)
{
	publicationBatchReady = false;
	publicationActorIndices.clear();
	if (!actorPublication || !actorPublicationCursor.Accepted() || !batch.valid || buildPending ||
		batch.activeActorCount != actorPublicationCursor.Entries().size()) return;
	publicationActorIndices.reserve(batch.activeActorCount);
	for (uint32_t i = 0; i < batch.actors.size(); ++i)
	{
		if (batch.actors[i].active && !publicationActorIndices.emplace(batch.actors[i].identityKey, i).second)
			return; // Duplicate identity: preserve the full path and its existing diagnostics.
	}
	if (publicationActorIndices.size() != actorPublicationCursor.Entries().size()) return;
	for (const auto& entry : actorPublicationCursor.Entries())
	{
		const auto instance = instances.find(entry.identityKey);
		if (publicationActorIndices.find(entry.identityKey) == publicationActorIndices.end() ||
			instance == instances.end() || instance->second.pending) return;
	}
	batchPublicationGenerations = actorPublicationCursor.Generations();
	publicationBatchReady = true;
}

bool NRIPersistentVoxelResidency::TryApplyPublishedActors(uint64_t cacheSerial, uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings, bool validate, NRIPersistentVoxelBatchStats& stats)
{
	if (!actorPublication || !publicationBatchReady || publicationConsumerQuarantined ||
		!actorPublicationCursor.Accepted() || !batch.valid ||
		batchMaterialResourceGeneration != materialResourceGeneration) return false;
	const auto& generations = actorPublication->generations;
	if (batchPublicationGenerations.topology != generations.topology ||
		batchPublicationGenerations.binding != generations.binding ||
		batchPublicationGenerations.authority != generations.authority) return false;
	const auto& entries = actorPublicationCursor.Entries();
	if (entries.size() != batch.activeActorCount || entries.size() != publicationActorIndices.size()) return false;
	const bool missedBatchDelta = batchPublicationGenerations.revision != actorPublication->baseRevision &&
		batchPublicationGenerations.revision != generations.revision;
	publicationTransformDirty.assign(entries.size(), publicationResynchronized || missedBatchDelta ? 1u : 0u);
	if (batchPublicationGenerations.revision != generations.revision)
	{
		for (const auto& delta : actorPublication->deltas)
		{
			if (delta.presentationChanged)
			{
				const uint32_t index = actorPublicationCursor.FindIndex(delta.identity);
				if (index == UINT32_MAX || index >= entries.size()) return false;
				publicationTransformDirty[index] = 1;
			}
		}
	}
	publicationFastActors.clear();
	publicationFastActors.reserve(entries.size());
	bool shadowMatches = true;
	for (uint32_t i = 0; i < entries.size(); ++i)
	{
		const auto& entry = entries[i];
		const auto actorIt = publicationActorIndices.find(entry.identityKey);
		const auto instanceIt = instances.find(entry.identityKey);
		if (actorIt == publicationActorIndices.end() || actorIt->second >= batch.actors.size() ||
			instanceIt == instances.end() || instanceIt->second.pending) return false;
		auto& actor = batch.actors[actorIt->second];
		const auto meshIt = meshVariantResources.find(actor.meshResourceKey);
		const auto materialIt = materialVariantResources.find(actor.materialKeyHash);
		// Resource lifetime is independent of producer identity. Keep these cheap
		// residency/range checks before any state mutation, even on unchanged frames.
		if (!actor.active || !IsPersistentVoxelCacheEntryPublicationCurrent(entry) ||
			BuildPersistentVoxelMeshResourceKey(entry, settings) != actor.meshResourceKey ||
			meshIt == meshVariantResources.end() || materialIt == materialVariantResources.end() ||
			meshIt->second.accelerationStructure.accelerationStructure == nullptr ||
			!materialRangeAllocator.Owns(PersistentVoxelMaterialRangeHandle(materialIt->second)) ||
			!PersistentVoxelMaterialRangeMatches(actor, materialIt->second)) return false;
		PublicationFastActor next;
		next.actorIndex = actorIt->second; next.entryIndex = i;
		next.instance = &instanceIt->second; next.mesh = &meshIt->second; next.material = &materialIt->second;
		next.transform = actor.instanceTransform;
		next.changed = publicationTransformDirty[i] != 0;
		if (next.changed)
		{
			FillPersistentVoxelActorInstanceTransform(entry, *next.mesh, next.transform);
			// Preserve the full path's exact transform-only eligibility. A surface
			// signature change without any placement/visibility change is not motion.
			const bool transformChanged = next.transform != actor.instanceTransform;
			const bool placementChanged = actor.placementGeneration != entry.placementGeneration ||
				actor.placementStateHash != entry.placementStateHash || actor.physicalSectorIndex != entry.physicalSectorIndex;
			if (actor.surfaceSignature != entry.surfaceSignature && !transformChanged && !placementChanged &&
				actor.indirectOnly == entry.indirectOnly && actor.visibilityChunkIndex == ResolvePersistentVoxelActorVisibilityChunk(entry))
				return false;
		}
		if (validate)
		{
			std::array<float, 12> expected;
			FillPersistentVoxelActorInstanceTransform(entry, *next.mesh, expected);
			shadowMatches = shadowMatches && expected == next.transform && actor.identityKey == entry.identityKey &&
				actor.signature == entry.signature && actor.geometrySignature == ResolvePersistentVoxelCacheEntryGeometrySignature(entry) &&
				actor.bakedSurfaceSignature == entry.bakedSurfaceSignature && actor.materialSignature == entry.materialSignature &&
				actor.meshKeyHash == entry.meshKeyHash && actor.materialKeyHash == entry.materialKeyHash &&
				actor.ownerWorldEpoch == entry.ownerWorldEpoch && actor.ownerLifetimeGeneration == entry.ownerLifetimeGeneration &&
				BuildPersistentVoxelMeshResourceKey(entry, settings) == actor.meshResourceKey &&
				(next.changed || (actor.surfaceSignature == entry.surfaceSignature && actor.indirectOnly == entry.indirectOnly &&
				actor.placementGeneration == entry.placementGeneration && actor.placementStateHash == entry.placementStateHash &&
				actor.physicalSectorIndex == entry.physicalSectorIndex));
			++stats.persistentVoxelPublicationStateChecks;
		}
		publicationFastActors.push_back(next);
	}
	if (!shadowMatches)
	{
		publicationConsumerQuarantined = true;
		++stats.persistentVoxelPublicationStateMismatches;
		Printf("NRI PT voxel publication state mismatch: frame=%u action=quarantine\n", frameIndex);
		return false; // Nothing has been changed; the original actor path owns this frame.
	}
	uint32_t changed = 0;
	for (const auto& update : publicationFastActors)
	{
		const auto& entry = entries[update.entryIndex];
		auto& actor = batch.actors[update.actorIndex];
		auto& instance = *update.instance;
		instance.previousTransform = instance.currentTransform;
		actor.previousInstanceTransform = instance.previousTransform;
		instance.lastSeenFrame = frameIndex; instance.active = true; instance.pending = false;
		CopyPersistentVoxelInstanceAuthority(entry, instance);
		CopyPersistentVoxelActorAuthority(entry, actor);
		actor.lastSeenFrame = entry.lastSeenFrame; actor.retainedFrameAge = entry.retainedFrameAge;
		actor.capturedThisFrame = entry.capturedThisFrame;
		actor.sourcePicnum = entry.sourcePicnum; actor.resolvedVoxelIndex = entry.resolvedVoxelIndex;
		if (update.changed)
		{
			actor.surfaceSignature = entry.surfaceSignature; actor.indirectOnly = entry.indirectOnly;
			actor.instanceTransform = update.transform;
			actor.visibilityChunkIndex = ResolvePersistentVoxelActorVisibilityChunk(entry);
			instance.surfaceSignature = entry.surfaceSignature;
			instance.currentTransform = update.transform;
			++changed;
		}
		actor.bindingGeneration = BuildPersistentVoxelActorBindingGeneration(actor);
		instance.meshResourceKey = actor.meshResourceKey; instance.bindingGeneration = actor.bindingGeneration;
		update.mesh->lastUsedFrame = frameIndex; update.mesh->lastUsedMapGeneration = residencyMapGeneration; update.mesh->cold = false;
		update.material->lastUsedFrame = frameIndex; update.material->lastUsedMapGeneration = residencyMapGeneration; update.material->cold = false;
	}
	if (changed != 0) ++batch.rebuildCount;
	stats.persistentVoxelInstanceTransformUpdates += changed;
	++stats.persistentVoxelPublicationFastPaths;
	++stats.persistentVoxelBatchSerialFastPathCount;
	batch.sourceSerial = cacheSerial;
	batchPublicationGenerations = generations;
	return true;
}
