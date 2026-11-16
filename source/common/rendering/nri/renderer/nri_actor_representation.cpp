#include "nri_actor_representation.h"
#include "nri_actor_representation_selection.h"
#include "nri_actor_occurrence_policy.h"
#include "nri_persistent_voxels.h"
#include "../scene/nri_actor_sprite_fallback.h"
#include "../scene/nri_scene_stats.h"
#include "c_cvars.h"
#include "perf_audit.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_set>

CVAR(Bool, nri_ptactorspritefallback, false, 0)
CVAR(Int, nri_ptactorholdframes, 8, 0)
// Session-only fault injection. A valid sprite must exist before hiding a voxel.
CVAR(Int, nri_ptactorspriteforce, -1, 0)
CVAR(Bool, nri_ptactorrepresentationtrace, false, 0)
CVAR(Int, nri_ptactorrepresentationactor, -1, 0)

namespace
{
	bool ForceSprite(int32_t actorIndex)
	{
		return (int)nri_ptactorspriteforce == -2 || (int)nri_ptactorspriteforce == actorIndex;
	}

	NRIActorOccurrencePolicyCandidate MakeOccurrenceCandidate(const nri_scene::ActorSpriteFallbackCandidate& source)
	{
		NRIActorOccurrencePolicyCandidate candidate;
		candidate.identityKey = source.identityKey;
		candidate.actorIndex = source.actorIndex;
		candidate.active = candidate.uniqueActiveOccurrence = true;
		candidate.capturedThisFrame = source.capturedThisFrame;
		candidate.requestedWorkloadMask = NRI_TLAS_MASK_ALL_WORKLOADS;
		candidate.boundsValid = !source.surface.vertices.empty();
		for (int axis = 0; axis < 3; ++axis)
		{
			candidate.boundsMin[axis] = std::numeric_limits<float>::max();
			candidate.boundsMax[axis] = -std::numeric_limits<float>::max();
			for (const auto& vertex : source.surface.vertices)
			{
				candidate.boundsMin[axis] = std::min(candidate.boundsMin[axis], vertex.position[axis]);
				candidate.boundsMax[axis] = std::max(candidate.boundsMax[axis], vertex.position[axis]);
			}
		}
		return candidate;
	}
}

bool NRIActorRepresentations::Build(uint32_t frameIndex, NRIPersistentVoxelResidency& voxels,
	const NRIPersistentVoxelSettings& settings, const NRIPersistentVoxelTlasServices& services,
	const NRIActorOccurrencePolicyContext& occurrenceContext,
	const NRISceneInstanceVisibilityContext& visibilityContext,
	const nri_scene::SceneView* dynamicScene, int32_t separateReflectionActor)
{
	mSelected.clear();
	mDecisions.clear();
	mPriorityMaterials.clear();
	mFrameIndex = frameIndex;
	mStats = {};
	voxels.spriteFallbackActors.clear();
	voxels.spriteFallbackFrame = frameIndex;
	if (!(bool)nri_ptactorspritefallback) return false;

	const auto& candidates = nri_scene::GetActorSpriteFallbackCandidates();
	NRIVoxelFrameKeyCounts activeOwners;
	activeOwners.Begin((uint32_t)voxels.batch.actors.size());
	for (const auto& actor : voxels.batch.actors)
	{
		if (actor.active && actor.ownerWorldEpoch != 0 && actor.placementGeneration != 0 && actor.placementStateHash != 0 &&
			actor.physicalSectorIndex >= 0 && actor.authorityCurrent && actor.publicationEligible && !actor.pendingRemoval)
			activeOwners.Add(actor.ownerWorldEpoch, actor.ownerLifetimeGeneration);
	}
	bool sceneEdited = false;
	const auto editScene = [&]()
	{
		if (sceneEdited) return;
		if (dynamicScene != nullptr) mScene = *dynamicScene;
		else mScene = {};
		sceneEdited = true;
	};
	NRIVoxelFrameKeyCounts owners;
	owners.Begin((uint32_t)candidates.size());
	std::unordered_set<int32_t> actorIndices;
	uint32_t traced = 0;
	for (const auto& candidate : candidates)
	{
		++mStats.candidates;
		const char* selection = "unavailable";
		const uint32_t slot = voxels.batch.actorIndex.Find(candidate.identityKey);
		const PersistentVoxelBatch::ActorEntry* actor = slot < voxels.batch.actors.size() ? &voxels.batch.actors[slot] : nullptr;
		const bool sameOwner = actor != nullptr && actor->ownerWorldEpoch == candidate.ownerWorldEpoch &&
			actor->ownerLifetimeGeneration == candidate.ownerLifetimeGeneration;
		const bool ready = sameOwner && actor->active && activeOwners.Count(candidate.ownerWorldEpoch, candidate.ownerLifetimeGeneration) == 1u &&
			voxels.IsActorTlasAppendEligible(*actor, frameIndex, settings, services);
		const auto* suppressed = voxels.GetSuppressedActorIndices(frameIndex);
		const bool intentionalAbsence = candidate.presentationHidden || settings.omitTlasOccurrences ||
			(sameOwner && ((actor->resolvedVoxelIndex >= 0 &&
				(actor->resolvedVoxelIndex == settings.excludeIndices[0] || actor->resolvedVoxelIndex == settings.excludeIndices[1] ||
				 actor->resolvedVoxelIndex == settings.excludeIndices[2])) ||
				(settings.excludeMinPrimitives > 0 && actor->primitiveCount >= settings.excludeMinPrimitives))) ||
			(suppressed != nullptr && suppressed->count(candidate.actorIndex) != 0) ||
			EvaluateNRIActorOccurrencePolicy(occurrenceContext, MakeOccurrenceCandidate(candidate)).suppress;
		if (owners.Add(candidate.ownerWorldEpoch, candidate.ownerLifetimeGeneration) != 1u ||
			!actorIndices.insert(candidate.actorIndex).second)
		{
			++mStats.duplicates;
			continue;
		}
		const auto selected = SelectNRIActorRepresentation({intentionalAbsence,
			candidate.transientGeometryCaptured || candidate.actorIndex == separateReflectionActor,
			ready, candidate.descriptorCurrent && candidate.ownerWorldEpoch != 0 && candidate.surface.vertices.size() >= 3 &&
				voxels.spriteFallbackReadyFrame == frameIndex && voxels.spriteFallbackReadyActors.count(candidate.identityKey) != 0,
			ForceSprite(candidate.actorIndex)});
		// Retained emissive surfaces are not a lifetime-qualified actor body.
		// A current voxel candidate owns this actor's body; replace any old
		// cached sprite aliases even when the chosen voxel is already ready.
		if (selected != NRIActorSelection::ExistingDynamic && dynamicScene != nullptr &&
			std::any_of(dynamicScene->opaqueSprites.begin(), dynamicScene->opaqueSprites.end(),
				[&](const nri_scene::SurfaceRef& surface) { return surface.provenance.actorIndex == candidate.actorIndex; }))
		{
			editScene();
			mScene.opaqueSprites.erase(std::remove_if(mScene.opaqueSprites.begin(), mScene.opaqueSprites.end(),
				[&](const nri_scene::SurfaceRef& surface) { return surface.provenance.actorIndex == candidate.actorIndex; }),
				mScene.opaqueSprites.end());
		}
		if (selected == NRIActorSelection::Suppressed)
		{
			selection = "suppressed";
			++mStats.suppressed;
			// Pure game presentation can intentionally hide an otherwise live
			// voxel owner. Final voxel and light append consume this decision too.
			voxels.spriteFallbackActors.insert(candidate.identityKey);
		}
		else if (selected == NRIActorSelection::ExistingDynamic)
		{
			selection = "existing-dynamic";
			++mStats.transient;
			voxels.spriteFallbackActors.insert(candidate.identityKey);
		}
		else if (selected == NRIActorSelection::Voxel)
		{
			selection = "voxel";
			++mStats.voxel;
		}
		else if (selected == NRIActorSelection::Sprite)
		{
			editScene();
			mScene.opaqueSprites.push_back(candidate.surface);
			const auto visibility = ResolveNRIPersistentVoxelInstanceVisibility(candidate.indirectOnly, candidate.actorIndex, visibilityContext);
			mSelected.emplace(candidate.actorIndex, SelectedSprite{
				ResolveNRIPersistentVoxelOwnerChunk(candidate.physicalSectorIndex, visibilityContext),
				(visibility.metadataFlags & SceneInstanceMetadataFlag_IndirectOnly) != 0, candidate.primaryHidden});
			voxels.spriteFallbackActors.insert(candidate.identityKey);
			voxels.NotifyActorRepresentation(candidate.identityKey, candidate.ownerWorldEpoch, candidate.ownerLifetimeGeneration,
				NRIVoxelActorRepresentation::SpriteFallback, 0, 0, frameIndex);
			selection = "sprite";
			++mStats.sprite;
		}
		else ++mStats.unavailable;

		const auto demand = voxels.actorDemand.GetActor(candidate.identityKey, candidate.ownerWorldEpoch, candidate.ownerLifetimeGeneration, frameIndex);
		if (selected == NRIActorSelection::Voxel)
			selection = demand.found && actor->meshResourceKey == demand.desiredMesh && actor->materialKeyHash == demand.desiredMaterial ? "exact" : "held";
		mDecisions.push_back({candidate.identityKey, candidate.ownerWorldEpoch, candidate.ownerLifetimeGeneration,
			demand.desiredMesh, demand.desiredMaterial, sameOwner ? actor->meshResourceKey : 0,
			sameOwner ? actor->materialKeyHash : 0, demand.exactMissingAge, candidate.actorIndex, selection,
			candidate.descriptorReason, candidate.descriptorTexture, candidate.descriptorCurrent, candidate.capturedThisFrame,
			candidate.animationCurrent, candidate.recipeCertified, ready,
			(bool)nri_ptactorrepresentationtrace && voxels.spriteFallbackReadyFrame == frameIndex &&
				voxels.spriteFallbackReadyActors.count(candidate.identityKey) != 0});

		if ((bool)nri_ptactorrepresentationtrace && traced < 128u &&
			((int)nri_ptactorrepresentationactor < 0 || (int)nri_ptactorrepresentationactor == candidate.actorIndex))
		{
			char fields[768];
			std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"actor\":%d,\"identity\":%llu,\"world\":%llu,\"lifetime\":%llu,\"placement\":%llu,\"selection\":\"%s\",\"descriptor_current\":%s,\"captured\":%s,\"voxel_ready\":%s,\"sprite_primary_hidden\":%s",
				frameIndex, candidate.actorIndex, (unsigned long long)candidate.identityKey,
				(unsigned long long)candidate.ownerWorldEpoch, (unsigned long long)candidate.ownerLifetimeGeneration,
				(unsigned long long)candidate.placementGeneration, selection,
				candidate.descriptorCurrent ? "true" : "false", candidate.capturedThisFrame ? "true" : "false", ready ? "true" : "false",
				candidate.primaryHidden ? "true" : "false");
			PerfAuditWriteEvent("actor_representation", PerfAuditCurrentToken(), fields);
			++traced;
		}
	}
	if ((bool)nri_ptactorrepresentationtrace || frameIndex % 120u == 0u)
	{
		char fields[512];
		std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"candidates\":%u,\"voxel\":%u,\"sprite\":%u,\"transient\":%u,\"suppressed\":%u,\"unavailable\":%u,\"duplicates\":%u,\"descriptor_bytes\":%llu",
			frameIndex, mStats.candidates, mStats.voxel, mStats.sprite, mStats.transient,
			mStats.suppressed, mStats.unavailable, mStats.duplicates,
			(unsigned long long)(candidates.size() * sizeof(nri_scene::ActorSpriteFallbackCandidate)));
		PerfAuditWriteEvent("actor_representation_summary", PerfAuditCurrentToken(), fields);
	}
	return sceneEdited;
}

void NRIActorRepresentations::RecordAssembly(uint32_t frameIndex, const NRIPersistentVoxelResidency& voxels,
	const nri_scene::GeometryData& geometry, const std::vector<nri_scene::MaterialData>& materials,
	const std::vector<std::pair<uint32_t, uint32_t>>& publishedRanges, bool ready) const
{
	if (mFrameIndex != frameIndex || !(bool)nri_ptactorspritefallback || !PerfAuditCurrentToken()) return;
	std::unordered_map<int32_t, uint32_t> carrierPrimitives, materialPrimitives, primaryHiddenPrimitives, indirectPrimitives;
	for (const auto& range : publishedRanges)
	{
		for (uint64_t index = range.first; index < uint64_t(range.first) + range.second && index < geometry.primitives.size(); ++index)
		{
			if (index >= geometry.primitiveProvenance.size()) continue;
			const auto& primitive = geometry.primitives[index];
			if ((primitive.flags & nri_scene::PrimitiveFlag_ActorSpriteFallback) == 0) continue;
			const auto actorIndex = geometry.primitiveProvenance[index].actorIndex;
			++carrierPrimitives[actorIndex];
			if ((primitive.flags & nri_scene::PrimitiveFlag_ActorSpritePrimaryHidden) != 0) ++primaryHiddenPrimitives[actorIndex];
			if ((bool)nri_ptactorrepresentationtrace && (primitive.flags & nri_scene::PrimitiveFlag_ReflectionOnly) != 0)
				++indirectPrimitives[actorIndex];
			if (primitive.materialIndex < materials.size() && materials[primitive.materialIndex].alpha > 1.0f / 255.0f)
				++materialPrimitives[actorIndex];
		}
	}
	uint32_t spritesPublished = 0, voxelsPublished = 0, absent = 0, duplicates = 0, suppressedPublished = 0, traced = 0;
	uint32_t missingTraced = 0;
	for (const auto& row : mDecisions)
	{
		const uint32_t slot = voxels.batch.actorIndex.Find(row.identity);
		const auto* actor = slot < voxels.batch.actors.size() ? &voxels.batch.actors[slot] : nullptr;
		const bool voxelPublished = ready && actor != nullptr && actor->ownerWorldEpoch == row.world &&
			actor->ownerLifetimeGeneration == row.lifetime && actor->inWorldTlasThisFrame && actor->worldTlasFrameIndex == frameIndex;
		const bool spritePublished = ready && carrierPrimitives[row.actorIndex] != 0 &&
			materialPrimitives[row.actorIndex] == carrierPrimitives[row.actorIndex];
		const bool intendedBody = std::strcmp(row.selection, "suppressed") != 0 && std::strcmp(row.selection, "existing-dynamic") != 0;
		spritesPublished += spritePublished;
		voxelsPublished += voxelPublished;
		absent += intendedBody && !spritePublished && !voxelPublished;
		duplicates += spritePublished && voxelPublished;
		suppressedPublished += std::strcmp(row.selection, "suppressed") == 0 && (spritePublished || voxelPublished);
		// Independent of the chosen detail actor and its 128-row cap. A small
		// failure-only stream identifies any other missing owner without turning
		// a targeted diagnostic into a full-route all-actor trace.
		if ((bool)nri_ptactorrepresentationtrace && missingTraced < 16u && intendedBody && !spritePublished && !voxelPublished)
		{
			char missingFields[1280];
			std::snprintf(missingFields, sizeof(missingFields), "\"renderer_frame\":%u,\"actor\":%d,\"identity\":%llu,\"world\":%llu,\"lifetime\":%llu,\"selection\":\"%s\",\"descriptor_reason\":\"%s\",\"descriptor_texture\":%d,\"descriptor_current\":%s,\"captured\":%s,\"animation_current\":%s,\"recipe_certified\":%s,\"voxel_ready\":%s,\"texture_ready\":%s,\"scene_ready\":%s,\"sprite_primitives\":%u,\"sprite_material_primitives\":%u,\"desired_mesh\":%llu,\"desired_material\":%llu,\"exact_wait_frames\":%u",
				frameIndex, row.actorIndex, (unsigned long long)row.identity, (unsigned long long)row.world,
				(unsigned long long)row.lifetime, row.selection, row.descriptorReason, row.descriptorTexture,
				row.descriptorCurrent ? "true" : "false", row.captured ? "true" : "false",
				row.animationCurrent ? "true" : "false", row.recipeCertified ? "true" : "false",
				row.voxelReady ? "true" : "false", row.textureReady ? "true" : "false", ready ? "true" : "false",
				carrierPrimitives[row.actorIndex], materialPrimitives[row.actorIndex],
				(unsigned long long)row.desiredMesh, (unsigned long long)row.desiredMaterial, row.waitFrames);
			PerfAuditWriteEvent("actor_representation_missing", PerfAuditCurrentToken(), missingFields);
			++missingTraced;
		}
		if ((bool)nri_ptactorrepresentationtrace && traced < 128u &&
			((int)nri_ptactorrepresentationactor < 0 || (int)nri_ptactorrepresentationactor == row.actorIndex))
		{
			char fields[1024];
			std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"actor\":%d,\"identity\":%llu,\"world\":%llu,\"lifetime\":%llu,\"selection\":\"%s\",\"desired_mesh\":%llu,\"desired_material\":%llu,\"selected_mesh\":%llu,\"selected_material\":%llu,\"exact_wait_frames\":%u,\"scene_ready\":%s,\"voxel_in_tlas\":%s,\"sprite_primitives\":%u,\"sprite_material_primitives\":%u,\"sprite_in_tlas\":%s,\"sprite_primary_hidden_primitives\":%u,\"sprite_indirect_primitives\":%u",
				frameIndex, row.actorIndex, (unsigned long long)row.identity, (unsigned long long)row.world,
				(unsigned long long)row.lifetime, row.selection, (unsigned long long)row.desiredMesh,
				(unsigned long long)row.desiredMaterial, (unsigned long long)row.selectedMesh,
				(unsigned long long)row.selectedMaterial, row.waitFrames, ready ? "true" : "false",
				voxelPublished ? "true" : "false", carrierPrimitives[row.actorIndex], materialPrimitives[row.actorIndex], spritePublished ? "true" : "false", primaryHiddenPrimitives[row.actorIndex], indirectPrimitives[row.actorIndex]);
			PerfAuditWriteEvent("actor_representation_assembly", PerfAuditCurrentToken(), fields);
			++traced;
		}
	}
	char fields[768];
	std::snprintf(fields, sizeof(fields), "\"renderer_frame\":%u,\"scene_ready\":%s,\"candidates\":%u,\"selected_sprite\":%u,\"selected_voxel\":%u,\"sprite_published\":%u,\"voxel_published\":%u,\"unrepresented\":%u,\"duplicates\":%u,\"suppressed_published\":%u,\"vertex_bytes\":%llu,\"index_bytes\":%llu,\"primitive_bytes\":%llu",
		frameIndex, ready ? "true" : "false", mStats.candidates, mStats.sprite, mStats.voxel,
		spritesPublished, voxelsPublished, absent, duplicates, suppressedPublished, (unsigned long long)mStats.vertexBytes,
		(unsigned long long)mStats.indexBytes, (unsigned long long)mStats.primitiveBytes);
	PerfAuditWriteEvent("actor_representation_frame", PerfAuditCurrentToken(), fields);
	if ((bool)nri_ptactorrepresentationtrace && absent > 0)
	{
		char coverage[192];
		std::snprintf(coverage, sizeof(coverage), "\"renderer_frame\":%u,\"missing\":%u,\"emitted\":%u,\"omitted\":%u,\"limit\":16",
			frameIndex, absent, missingTraced, absent - missingTraced);
		PerfAuditWriteEvent("actor_representation_missing_coverage", PerfAuditCurrentToken(), coverage);
	}
}

bool NRIActorRepresentations::HasIndirectSprites(uint32_t frameIndex) const
{
	return mFrameIndex == frameIndex && std::any_of(mSelected.begin(), mSelected.end(),
		[](const auto& item) { return item.second.indirectOnly; });
}

void NRIActorRepresentations::FinishGeometry()
{
	nri_scene::BuildGeometry(mScene, mGeometry, nullptr, true);
	std::unordered_set<uint32_t> materialIndices;
	for (size_t index = 0; index < mGeometry.primitives.size(); ++index)
	{
		const auto found = mSelected.find(mGeometry.primitiveProvenance[index].actorIndex);
		if (found == mSelected.end()) continue;
		auto& primitive = mGeometry.primitives[index];
		primitive.flags |= nri_scene::PrimitiveFlag_ActorSpriteFallback;
		if (found->second.indirectOnly) primitive.flags |= nri_scene::PrimitiveFlag_ReflectionOnly;
		if (found->second.primaryHidden) primitive.flags |= nri_scene::PrimitiveFlag_ActorSpritePrimaryHidden;
		primitive.reserved0 = found->second.ownerChunk;
		if (materialIndices.insert(primitive.materialIndex).second) mPriorityMaterials.push_back(primitive.materialIndex);
		mStats.primitiveBytes += sizeof(nri_scene::PrimitiveData);
		mStats.indexBytes += 3 * sizeof(uint32_t);
	}
	// BuildGeometry currently emits triangle vertices; report the actual carrier
	// footprint, separately from texture residency and aggregate dynamic buffers.
	mStats.vertexBytes = mStats.primitiveBytes / sizeof(nri_scene::PrimitiveData) * 3 * sizeof(nri_scene::SceneVertex);
}
