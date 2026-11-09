#include "nri_debug_reporters.h"
#include "nri_cvars.h"

#include "nri_renderer.h"
#include "nri_diagnostic_names.h"
#include "nri_scene_frame_mirrors.h"
#include "c_cvars.h"
#include "printf.h"
void NRIRenderer::PrintDynamicSceneStatus() const
{
	const PersistentDynamicSurfaceStats persistentStats = mSceneLights.GatherPersistentDynamicEmissiveSurfaceStats();
	const PersistentDynamicEmissiveCache& persistentCache = mSceneLights.GetPersistentDynamicEmissiveCache();
	const SceneLightSystem::PersistentDynamicEmissiveHighWaterStats& persistentHighWater = mSceneLights.GetPersistentDynamicEmissiveHighWaterStats();
	const ActorSpriteDebugStats& actorSpriteDebugStats = mSceneLights.GetActorSpriteDebugStats();
	const bool residentLocalPlayerVoxel =
		mPersistentVoxels.HasResidentIndirectOnlyActor(ResolveNRILocalPlayerActorIndex());
	const char* const localPlayerReflectionPolicy =
		residentLocalPlayerVoxel ? "resident_indirect_voxel" :
		mDynamicSceneLastFrame.localPlayerReflectionSurfaceCount > 0 ? "reflection_only_fallback" :
		"disabled";

	Printf("NRI PT mirror policy: wall_mode=material_reflection mirror_overlay=removed local_player_reflection=%s\n",
		localPlayerReflectionPolicy);
	Printf("NRI PT dynamic scene: active=%s sprite_surfaces=%u tris=%u materials=%u models=%u unsupported_models=%u local_player_reflection_surfaces=%u local_player_reflection_tris=%u local_player_reflection_materials=%u local_player_reflection_models=%u local_player_reflection_unsupported_models=%u dynamic_as_builds=%u last_frame_as_build=%s active_tlas_instances=%u emissive_cache=%s cache_surfaces=%u cache_tris=%u cache_materials=%u\n",
		mUsedDynamicSceneLastFrame ? "yes" : "no",
		mDynamicSceneLastFrame.spriteSurfaceCount,
		mDynamicSceneLastFrame.primitiveCount,
		mDynamicSceneLastFrame.materialCount,
		mDynamicSceneLastFrame.modelCount,
		mDynamicSceneLastFrame.unsupportedModelCount,
		mDynamicSceneLastFrame.localPlayerReflectionSurfaceCount,
		mDynamicSceneLastFrame.localPlayerReflectionPrimitiveCount,
		mDynamicSceneLastFrame.localPlayerReflectionMaterialCount,
		mDynamicSceneLastFrame.localPlayerReflectionModelCount,
		mDynamicSceneLastFrame.localPlayerReflectionUnsupportedModelCount,
		mDynamicSceneLastFrame.asBuildCount,
		mBuiltDynamicSceneASLastFrame ? "yes" : "no",
		mActiveTlasInstanceCount,
		persistentCache.valid ? "yes" : "no",
		persistentCache.surfaceCount,
		persistentCache.primitiveCount,
		persistentCache.materialCount);
	Printf("NRI PT dynamic cache: actor_surfaces=%u non_actor_surfaces=%u walls=%u flats=%u sprites=%u actor_facing=%u actor_voxel=%u highwater=surfaces:%u tris:%u mats:%u actor:%u non_actor:%u walls:%u flats:%u sprites:%u actor_facing:%u actor_voxel:%u\n",
		persistentStats.actorSurfaceCount,
		persistentStats.nonActorSurfaceCount,
		persistentStats.wallSurfaceCount,
		persistentStats.flatSurfaceCount,
		persistentStats.spriteSurfaceCount,
		persistentStats.actorFacingSpriteCount,
		persistentStats.actorVoxelSpriteCount,
		persistentHighWater.surfaceCount,
		persistentHighWater.primitiveCount,
		persistentHighWater.materialCount,
		persistentHighWater.surfaceStats.actorSurfaceCount,
		persistentHighWater.surfaceStats.nonActorSurfaceCount,
		persistentHighWater.surfaceStats.wallSurfaceCount,
		persistentHighWater.surfaceStats.flatSurfaceCount,
		persistentHighWater.surfaceStats.spriteSurfaceCount,
		persistentHighWater.surfaceStats.actorFacingSpriteCount,
		persistentHighWater.surfaceStats.actorVoxelSpriteCount);
	Printf("NRI PT actor sprite diag: trace=%d cache_actor_facing=%u cache_actor_voxel=%u prune_checks=%u prune_matches=%u drop_missing_actor=%u drop_missing_actor_index=%u drop_null_live_texture=%u drop_texture_mismatch=%u drop_palette_mismatch=%u\n",
		(int)nri_ptactorspritetrace,
		persistentStats.actorFacingSpriteCount,
		persistentStats.actorVoxelSpriteCount,
		actorSpriteDebugStats.lastPruneChecks,
		actorSpriteDebugStats.lastPruneMatches,
		actorSpriteDebugStats.lastPruneDroppedMissingActor,
		actorSpriteDebugStats.lastPruneDroppedMissingActorIndex,
		actorSpriteDebugStats.lastPruneDroppedNullLiveTexture,
		actorSpriteDebugStats.lastPruneDroppedTextureMismatch,
		actorSpriteDebugStats.lastPruneDroppedPaletteMismatch);
	Printf("NRI PT scene texture overflow: textures=%u truncated=%u clamps=base:%u normal:%u metallic:%u roughness:%u emissive:%u builds=%llu warned=%s\n",
		mSceneTextures.OverflowStats().textureCountLastBuild,
		mSceneTextures.OverflowStats().truncatedTextureCountLastBuild,
		mSceneTextures.OverflowStats().baseTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().normalTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild,
		mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild,
		(unsigned long long)mSceneTextures.OverflowStats().totalOverflowBuilds,
		mSceneTextures.OverflowStats().warningLogged ? "yes" : "no");
	Printf("NRI PT scene texture attribution: reason=%s requested=%u actor_materials=%u base=%u glow=%u normal=%u metallic=%u roughness=%u emissive=%u\n",
		mLastPerfShellTraceStats.sceneTextureReason.empty() ? "none" : mLastPerfShellTraceStats.sceneTextureReason.c_str(),
		mLastPerfShellTraceStats.sceneTextureRequestedCount,
		mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount,
		mLastPerfShellTraceStats.sceneTextureReferencedBaseCount,
		mLastPerfShellTraceStats.sceneTextureReferencedGlowCount,
		mLastPerfShellTraceStats.sceneTextureReferencedNormalCount,
		mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount,
		mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount,
		mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount);
	Printf("NRI PT actor overflow: materials=%u clamps=base:%u normal:%u metallic:%u roughness:%u emissive:%u omitted=%u\n",
		mLastPerfShellTraceStats.actorOverflowMaterialCount,
		mLastPerfShellTraceStats.actorOverflowBaseClampCount,
		mLastPerfShellTraceStats.actorOverflowNormalClampCount,
		mLastPerfShellTraceStats.actorOverflowMetallicClampCount,
		mLastPerfShellTraceStats.actorOverflowRoughnessClampCount,
		mLastPerfShellTraceStats.actorOverflowEmissiveClampCount,
		mLastPerfShellTraceStats.actorOverflowTraceOmittedCount);
	Printf("NRI PT scene texture cache: entries=%u highwater=%u misses=%u inserts=%u transitions=%u lookup_ms=%.3f realize_ms=%.3f descriptor_ms=%.3f transition_ms=%.3f\n",
		mSceneTextures.CacheStats().cacheEntriesLastBuild,
		mSceneTextures.CacheStats().cacheEntriesHighWater,
		mSceneTextures.CacheStats().lookupMissesLastBuild,
		mSceneTextures.CacheStats().insertCountLastBuild,
		mSceneTextures.CacheStats().transitionCountLastFrame,
		mSceneTextures.CacheStats().lookupMsLastBuild,
		mSceneTextures.CacheStats().realizeMsLastBuild,
		mSceneTextures.CacheStats().descriptorMsLastBuild,
		mSceneTextures.CacheStats().transitionMsLastFrame);
	Printf("NRI PT binding diag: label=%s materials=%u textures=%u actor_surfaces=%u actor_count=%u bridge_hash=0x%llx actor_hash=0x%llx scene_tex_updates=%llu scene_tex_hash=0x%llx scene_tex_reason=%s qframe=%u outstanding=%u scene_data_updates=%llu scene_data_hash=0x%llx scene_data_reason=%s qframe=%u outstanding=%u\n",
		mDescriptorCoherencyDebugStats.lastMaterialBuildLabel.empty() ? "none" : mDescriptorCoherencyDebugStats.lastMaterialBuildLabel.c_str(),
		mDescriptorCoherencyDebugStats.lastMaterialCount,
		mDescriptorCoherencyDebugStats.lastTextureCount,
		mDescriptorCoherencyDebugStats.lastActorSpriteSurfaceCount,
		mDescriptorCoherencyDebugStats.lastActorSpriteActorCount,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastMaterialBridgeHash,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastActorSpriteMaterialHash,
		(unsigned long long)mDescriptorCoherencyDebugStats.sceneTextureSetUpdates,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastSceneTextureDescriptorHash,
		mDescriptorCoherencyDebugStats.lastSceneTextureReason.empty() ? "none" : mDescriptorCoherencyDebugStats.lastSceneTextureReason.c_str(),
		mDescriptorCoherencyDebugStats.lastSceneTextureQueuedFrameIndex,
		mDescriptorCoherencyDebugStats.lastSceneTextureOutstandingQueuedFrames,
		(unsigned long long)mDescriptorCoherencyDebugStats.sceneDataSetUpdates,
		(unsigned long long)mDescriptorCoherencyDebugStats.lastSceneDataDescriptorHash,
		mDescriptorCoherencyDebugStats.lastSceneDataReason.empty() ? "none" : mDescriptorCoherencyDebugStats.lastSceneDataReason.c_str(),
		mDescriptorCoherencyDebugStats.lastSceneDataQueuedFrameIndex,
		mDescriptorCoherencyDebugStats.lastSceneDataOutstandingQueuedFrames);
	Printf("NRI PT material builds: calls=%u override_builds=%u override_ms=%.3f material_ms=%.3f\n",
		mLastPerfShellTraceStats.materialBuildCalls,
		mLastPerfShellTraceStats.actorOverrideMapBuildCalls,
		mLastPerfShellTraceStats.actorOverrideMapBuildMs,
		mLastPerfShellTraceStats.materialBuildMs);
	Printf("NRI PT mutation detail: structural_ms=%.3f material_refresh_ms=%.3f structural=%u material_refresh=%u refresh_delta=%u refresh_delta_mask=0x%x refresh_hwcanvas=%u refresh_animated=%u struct_delta=%u struct_delta_mask=0x%x struct_view=%u struct_static_anim_flip=%u struct_excl_static_flip=%u struct_force_topology=%u struct_invalid=%u hwcanvas_chunks=%u\n",
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildMs,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshMs,
		mLastPerfShellTraceStats.runtimeMutationStructuralRebuildChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReplacementDeltaChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshReasonMaskOr,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshHardwareCanvasChunks,
		mLastPerfShellTraceStats.runtimeMutationMaterialRefreshAnimatedChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementDeltaReasonMaskOr,
		mLastPerfShellTraceStats.runtimeMutationStructuralReplacementViewChangedChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralStaticAnimatedModeFlipChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralExcludeStaticFlipChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralForcedTopologyChunks,
		mLastPerfShellTraceStats.runtimeMutationStructuralInvalidChunks,
		mLastPerfShellTraceStats.runtimeMutationHardwareCanvasChunkCount);
	for (size_t index = 0; index < NRIRenderer::MaterialBuildTraceSlotCount; ++index)
	{
		const auto& entry = mLastPerfShellTraceStats.materialBuildByLabel[index];
		if (entry.calls == 0 && entry.overrideBuildCalls == 0)
		{
			continue;
		}

		Printf("NRI PT material detail: label=%s calls=%u override_builds=%u override_ms=%.3f material_ms=%.3f\n",
			GetMaterialBuildTraceSlotName((MaterialBuildTraceSlot)index),
			entry.calls,
			entry.overrideBuildCalls,
			entry.overrideBuildMs,
			entry.materialBuildMs);
		if (entry.textureAttributionCalls != entry.calls)
		{
			Printf("NRI PT material textures: label=%s availability=not-collected coverage=%u/%u; enable perf_looptraceframes or an audit census\n",
				GetMaterialBuildTraceSlotName((MaterialBuildTraceSlot)index), entry.textureAttributionCalls, entry.calls);
			continue;
		}
		Printf("NRI PT material textures: label=%s materials=%u actor_materials=%u textures=%u base=%u glow=%u normal=%u metallic=%u roughness=%u emissive=%u\n",
			GetMaterialBuildTraceSlotName((MaterialBuildTraceSlot)index),
			entry.materialCount,
			entry.actorMaterialCount,
			entry.textureCount,
			entry.baseTextureCount,
			entry.glowTextureCount,
			entry.normalTextureCount,
			entry.metallicTextureCount,
			entry.roughnessTextureCount,
			entry.emissiveTextureCount);
	}
}
