#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_scene_material_texture_slots.h"
#include "../scene/nri_fluid_material_binding.h"
#include "../scene/nri_material_bridge_ranges.h"
#include "perf_audit.h"
#include "lightoverlay.h"
#include "fluidoverlay.h"
#include "engineerrors.h"

#include <cstring>
#include <unordered_set>

void NRIRenderer::ValidateVoxelMaterialSharingProduct(const nri_scene::MaterialBridgeData& source,
	const std::vector<nri_scene::MaterialData>& output, size_t staticCount, bool postLight,
	const std::vector<uint32_t>* deferredIndices)
{
	if (!(bool)nri_ptmaterialrangesvalidate || !mSceneMaterialFrameCache.PersistentMaterialCount()) return;
	PerfAuditAdd(postLight ? PerfAuditMetric::VoxelMaterialSharingPostLightChecks : PerfAuditMetric::VoxelMaterialSharingPreLightChecks);
	const std::unordered_set<uint32_t> deferred = deferredIndices ?
		std::unordered_set<uint32_t>(deferredIndices->begin(), deferredIndices->end()) : std::unordered_set<uint32_t>{};
	std::unordered_map<uint64_t, uint32_t> textureLookup;
	if (!mSceneTextureStableSlotsActive)
		for (uint32_t i = 0; i < source.textures.size(); ++i) textureLookup.emplace(source.textures[i].key, i);
	for (const auto& actor : mPersistentVoxels.batch.actors)
	{
		if (!actor.active || !actor.ownerWorldEpoch || !actor.placementGeneration || !actor.placementStateHash ||
			actor.physicalSectorIndex < 0 || !actor.authorityCurrent || !actor.publicationEligible || actor.pendingRemoval) continue;
		auto fail = [&](uint32_t slot, const char* reason)
		{
			PerfAuditAdd(PerfAuditMetric::VoxelMaterialSharingReferenceMismatches);
			I_FatalError("NRI material sharing validation: stage=%s actor=%d owner=0x%llx key=0x%llx offset=%u slot=%u reason=%s",
				postLight ? "postlight" : "prelight", actor.actorIndex, (unsigned long long)actor.identityKey,
				(unsigned long long)actor.materialKeyHash, actor.materialOffset, slot, reason);
		};
		// Build the old per-occurrence product from its original provenance, then
		// independently resolve its local texture namespace and current policy.
		nri_scene::MaterialBridgeData reference = actor.materialBridge;
		if (reference.materials.size() != actor.materialCount || reference.lightMetadata.size() != actor.materialCount)
			fail(0, "occurrence-row-count");
		if (mSceneTextureStableSlotsActive)
			NRIResolveSceneMaterialBridgeTextureSlots(mSceneTextures.SlotTable(), reference, 0);
		else
		{
			const auto local = reference;
			if (!nri_scene::CopyMaterialBridgeRowRange(local, {0, actor.materialCount}, reference, 0, textureLookup))
				fail(0, "occurrence-texture-remap");
		}
		auto expected = reference.materials;
		ApplyEmissiveMaterialOverrides(reference, expected);
		ApplyActorShadowMaterialOverrides(reference, expected);
		if (!postLight) nri_scene::ApplyFluidMaterialBindings(GetResolvedFluidOverlaySet(), reference, expected);
		for (uint32_t slot = 0; slot < actor.materialCount; ++slot)
		{
			const uint64_t index = staticCount + (uint64_t)actor.materialOffset + slot;
			if (index >= output.size()) fail(slot, "occurrence-address");
			if (deferred.find((uint32_t)index) != deferred.end())
			{
				PerfAuditAdd(PerfAuditMetric::VoxelMaterialSharingDeferredReferenceRows);
				continue; // Existing pending texture proxy intentionally preserves prior bytes.
			}
			PerfAuditAdd(postLight ? PerfAuditMetric::VoxelMaterialSharingPostLightRows : PerfAuditMetric::VoxelMaterialSharingPreLightRows);
			if (mPersistentVoxels.materialPayloads.BindingIsShared(actor.materialKeyHash))
				PerfAuditAdd(postLight ? PerfAuditMetric::VoxelMaterialSharingPostLightSharedRows : PerfAuditMetric::VoxelMaterialSharingPreLightSharedRows);
			if (std::memcmp(&expected[slot], &output[index], sizeof(nri_scene::MaterialData))) fail(slot, "final-policy-bytes");
		}
	}
}
