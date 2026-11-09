#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_emissive_tail_audit.h"
#include "nri_emissive_tail_composition.h"
#include "nri_render_geometry_helpers.h"
#include "nri_runtime_mutation_trace.h"
#include "nri_scene_frame_coordinator_types.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "perf_audit.h"
#include "printf.h"

#include <iterator>

bool NRIRenderer::BuildRetainedDynamicEmissiveProduct(RenderSceneFrameBuildResult& frame,
	const nri_scene::GeometryProductLayout& liveGeometryLayout,
	const nri_scene::MaterialProductLayout& liveMaterialLayout,
	void (*rebuildViewStats)(nri_scene::SceneView&))
{
	using nri_runtime_mutation::ScopedPtPerfTimer;
	auto& liveView = frame.dynamicSceneView;
	auto& mergedView = frame.mergedDynamicSceneView;
	auto& mergedGeometry = frame.mergedDynamicGeometry;
	auto& mergedMaterials = frame.mergedDynamicMaterialBridge;
	auto& scratch = frame.emissiveTailScratch;
	const auto& cache = mSceneLights.GetPersistentDynamicEmissiveCache();
	SceneLightSystem::PersistentDynamicMergeStats mergeStats = {};
	bool composed = false;
	const bool enabled = (bool)nri_ptemissivetailappend;

	const auto copyLive = [&]()
	{
		ScopedPtPerfTimer timer(mLastPerfShellTraceStats.sceneSelectDynamicMergeCopyMs);
		PerfAuditScope audit(PerfAuditMetric::EmissiveTailViewCopyNs);
		mergedView = liveView;
		NRIAuditEmissiveTailViewCopy(liveView);
	};
	const auto buildProduct = [&](nri_scene::SceneView& view, nri_scene::GeometryData& geometry,
		nri_scene::MaterialBridgeData& materials, nri_scene::GeometryProductLayout* geometryLayout,
		nri_scene::MaterialProductLayout* materialLayout)
	{
		{
			ScopedPtPerfTimer timer(mLastPerfShellTraceStats.sceneSelectDynamicMergeStatsMs);
			PerfAuditScope audit(PerfAuditMetric::EmissiveTailStatsNs);
			rebuildViewStats(view);
		}
		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer timer(mLastPerfShellTraceStats.sceneSelectDynamicMergeGeometryMs);
			ScopedPtPerfTimer legacy(mLastPerfShellTraceStats.geometryBuildMergedDynamicMs);
			PerfAuditScope audit(PerfAuditMetric::EmissiveTailGeometryBuildNs);
			nri_scene::BuildGeometry(view, geometry, nullptr, true, geometryLayout);
		}
		{
			ScopedPtPerfTimer timer(mLastPerfShellTraceStats.sceneSelectDynamicMergePortalAssignMs);
			PerfAuditScope audit(PerfAuditMetric::EmissiveTailPortalAssignNs);
			AssignGeometryPortalIndices(mMapWorld, geometry);
		}
		{
			Clocker clock(NriPTMaterialBuild);
			ScopedPtPerfTimer timer(mLastPerfShellTraceStats.sceneSelectDynamicMergeMaterialMs);
			PerfAuditScope audit(PerfAuditMetric::EmissiveTailMaterialBuildNs);
			// Preserve this trace slot: it now measures only the rows actually built.
			BuildMaterialsWithActorOverrides(view, materials, "dynamic_with_persistent_emissive", materialLayout);
		}
		NRIAuditEmissiveTailBuildProduct(geometry, materials);
	};
	bool useMerged = [&]()
	{
		ScopedPtPerfTimer timer(mLastPerfShellTraceStats.sceneSelectDynamicMergeMs);
		PerfAuditScope audit(PerfAuditMetric::EmissiveTailMergeNs);
		PerfAuditAdd(PerfAuditMetric::EmissiveTailMergeOperations);
		PerfAuditAdd(PerfAuditMetric::EmissiveTailLiveMaterialRows, frame.dynamicMaterialBridge.materials.size());
		PerfAuditAdd(PerfAuditMetric::EmissiveTailCacheMaterialRows, cache.materialBridge.materials.size());
		if (!enabled) copyLive();
		{
			ScopedPtPerfTimer appendTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergeAppendMs);
			PerfAuditScope appendAudit(PerfAuditMetric::EmissiveTailDedupAndAppendNs);
			mergeStats = enabled ? mSceneLights.CollectPersistentDynamicEmissiveTail(liveView, scratch.view) :
				mSceneLights.MergePersistentDynamicEmissiveCacheIntoSceneView(mergedView);
		}
		PerfAuditAdd(PerfAuditMetric::EmissiveTailLiveSurfaces, mergeStats.LiveSurfaceCount());
		PerfAuditAdd(PerfAuditMetric::EmissiveTailCacheSurfaces, mergeStats.CacheSurfaceCount());
		PerfAuditAdd(PerfAuditMetric::EmissiveTailAppendedSurfaces, mergeStats.AppendedSurfaceCount());
		PerfAuditAdd(PerfAuditMetric::EmissiveTailDuplicateSurfaces, mergeStats.DuplicateSurfaceCount());
		mLastPerfShellTraceStats.dynamicMergeLiveSurfaceCount = mergeStats.LiveSurfaceCount();
		mLastPerfShellTraceStats.dynamicMergePersistentCacheSurfaceCount = mergeStats.CacheSurfaceCount();
		mLastPerfShellTraceStats.dynamicMergeAppendedPersistentSurfaceCount = mergeStats.AppendedSurfaceCount();
		mLastPerfShellTraceStats.dynamicMergeDuplicatePersistentSurfaceCount = mergeStats.DuplicateSurfaceCount();
		mLastPerfShellTraceStats.dynamicMergeAppendedPersistentPrimitiveCount = cache.primitiveCount;
		mLastPerfShellTraceStats.dynamicMergeAppendedPersistentMaterialCount = cache.materialCount;
		mLastPerfShellTraceStats.dynamicMergeDeltaAppendAttempts++;
		if (mergeStats.AppendedSurfaceCount() == 0)
		{
			PerfAuditAdd(PerfAuditMetric::EmissiveTailZeroTailReuses);
			mLastPerfShellTraceStats.dynamicMergeDeltaAppendUsed++;
			return false;
		}
		if (enabled)
		{
			nri_scene::GeometryProductLayout tailGeometryLayout;
			auto& tailMaterialLayout = scratch.materialLayout;
			buildProduct(scratch.view, scratch.geometry, scratch.materials, &tailGeometryLayout, &tailMaterialLayout);
			PerfAuditAdd(PerfAuditMetric::EmissiveTailTailMaterialRowsBuilt, scratch.materials.materials.size());
			{
				PerfAuditScope compositionAudit(PerfAuditMetric::EmissiveTailCompositionNs);
				composed = ComposeNRIEmissiveTailProducts(liveView, frame.dynamicGeometry, liveGeometryLayout,
					frame.dynamicMaterialBridge, liveMaterialLayout, scratch.view, scratch.geometry, tailGeometryLayout,
					scratch.materials, tailMaterialLayout, scratch, mergedGeometry, mergedMaterials);
			}
			if (composed)
			{
				copyLive();
				const auto moveTail = [](auto& target, auto& tail)
				{
					target.insert(target.end(), std::make_move_iterator(tail.begin()), std::make_move_iterator(tail.end()));
				};
				moveTail(mergedView.opaqueWalls, scratch.view.opaqueWalls);
				moveTail(mergedView.opaqueFlats, scratch.view.opaqueFlats);
				moveTail(mergedView.opaqueSprites, scratch.view.opaqueSprites);
				{
					ScopedPtPerfTimer statsTimer(mLastPerfShellTraceStats.sceneSelectDynamicMergeStatsMs);
					PerfAuditScope statsAudit(PerfAuditMetric::EmissiveTailStatsNs);
					rebuildViewStats(mergedView);
				}
				PerfAuditAdd(PerfAuditMetric::EmissiveTailCompositions);
				PerfAuditAdd(PerfAuditMetric::EmissiveTailLiveMaterialRowsReused, frame.dynamicMaterialBridge.materials.size());
				mLastPerfShellTraceStats.dynamicMergeDeltaAppendUsed++;
				return !mergedGeometry.primitives.empty();
			}
			PerfAuditAdd(PerfAuditMetric::EmissiveTailUnsupportedFallbacks);
			copyLive();
			mSceneLights.MergePersistentDynamicEmissiveCacheIntoSceneView(mergedView);
		}
		mLastPerfShellTraceStats.dynamicMergeDeltaAppendFallbacks++;
		mLastPerfShellTraceStats.dynamicMergeDeltaAppendFallbackNonZeroSurfaces++;
		PerfAuditAdd(PerfAuditMetric::EmissiveMergeRebuilds);
		PerfAuditScope rebuildAudit(PerfAuditMetric::EmissiveRebuildNs);
		PerfAuditAdd(PerfAuditMetric::EmissiveTailFullBuilds);
		buildProduct(mergedView, mergedGeometry, mergedMaterials, nullptr, nullptr);
		return !mergedGeometry.primitives.empty();
	}();

	// This independent full-build oracle is deliberately outside normal owner
	// timing. Its wrapper/engine work belongs only to the validation population.
	if (enabled && (bool)nri_ptemissivetailvalidate && (composed || mergeStats.AppendedSurfaceCount() == 0))
	{
		PerfAuditScope validationAudit(PerfAuditMetric::EmissiveTailValidationNs);
		nri_scene::SceneView referenceView = liveView;
		nri_scene::GeometryData referenceGeometry;
		nri_scene::MaterialBridgeData referenceMaterials;
		const auto referenceStats = mSceneLights.MergePersistentDynamicEmissiveCacheIntoSceneView(referenceView);
		const bool nonzero = referenceStats.AppendedSurfaceCount() != 0;
		if (nonzero)
		{
			rebuildViewStats(referenceView);
			nri_scene::BuildGeometry(referenceView, referenceGeometry);
			AssignGeometryPortalIndices(mMapWorld, referenceGeometry);
			BuildMaterialsWithActorOverrides(referenceView, referenceMaterials, "emissive_tail_reference");
		}
		const bool referenceSelected = nonzero && !referenceGeometry.primitives.empty();
		const auto& selectedView = useMerged ? mergedView : liveView;
		const auto& selectedGeometry = useMerged ? mergedGeometry : frame.dynamicGeometry;
		const auto& selectedMaterials = useMerged ? mergedMaterials : frame.dynamicMaterialBridge;
		// Compare the built products even when both are geometry-empty and the
		// selection therefore falls back to live. Selection alone is insufficient.
		const bool viewEqual = NRIEmissiveTailViewsEqual(selectedView, referenceSelected ? referenceView : liveView) &&
			(!composed || NRIEmissiveTailViewsEqual(mergedView, referenceView));
		const bool geometryEqual = NRIEmissiveTailGeometryEqual(selectedGeometry, referenceSelected ? referenceGeometry : frame.dynamicGeometry) &&
			(!composed || NRIEmissiveTailGeometryEqual(mergedGeometry, referenceGeometry));
		const bool materialsEqual = NRIEmissiveTailMaterialsEqual(selectedMaterials, referenceSelected ? referenceMaterials : frame.dynamicMaterialBridge) &&
			(!composed || NRIEmissiveTailMaterialsEqual(mergedMaterials, referenceMaterials));
		const bool tailEqual = mergeStats.AppendedSurfaceCount() == referenceStats.AppendedSurfaceCount();
		PerfAuditAdd(PerfAuditMetric::EmissiveTailValidationChecks);
		if (composed && nonzero) PerfAuditAdd(PerfAuditMetric::EmissiveTailNonzeroValidationChecks);
		if (!viewEqual || !geometryEqual || !materialsEqual || !tailEqual || useMerged != referenceSelected)
		{
			PerfAuditAdd(PerfAuditMetric::EmissiveTailValidationMismatches);
			PerfAuditAdd(PerfAuditMetric::EmissiveTailQuarantines);
			Printf("NRI emissive tail validation mismatch: frame=%llu view=%u geometry=%u materials=%u tail=%u selected=%u reference_selected=%u\n",
				(unsigned long long)mFrameIndex, viewEqual, geometryEqual, materialsEqual, tailEqual, useMerged, referenceSelected);
			// Quarantine this operation: publish only the independently built product.
			if (referenceSelected)
			{
				mergedView = std::move(referenceView); mergedGeometry = std::move(referenceGeometry);
				mergedMaterials = std::move(referenceMaterials);
			}
			useMerged = referenceSelected;
		}
	}
	if (PerfAuditActive())
	{
		PerfAuditSet(PerfAuditMetric::EmissiveTailCapacityRendererFrame, mFrameIndex);
		PerfAuditAdd(PerfAuditMetric::EmissiveTailCapacityPublications);
		// Published on every operation, including zero-tail reuse. These are
		// owned CPU vector capacities; immutable payloads and allocator overhead
		// are excluded. Hash buckets are reported separately from dense remaps.
		PerfAuditSet(PerfAuditMetric::EmissiveTailStagingGeometryCapacityBytes,
			scratch.geometry.vertices.capacity() * sizeof(nri_scene::SceneVertex) +
			scratch.geometry.indices.capacity() * sizeof(uint32_t) +
			scratch.geometry.primitives.capacity() * sizeof(nri_scene::PrimitiveData) +
			scratch.geometry.primitiveProvenance.capacity() * sizeof(nri_scene::SurfaceProvenance));
		PerfAuditSet(PerfAuditMetric::EmissiveTailStagingMaterialCapacityBytes,
			scratch.materials.materials.capacity() * sizeof(nri_scene::MaterialData) +
			scratch.materials.lightMetadata.capacity() * sizeof(nri_scene::MaterialLightingMetadata) +
			scratch.materials.textures.capacity() * sizeof(nri_scene::TextureUpload));
		PerfAuditSet(PerfAuditMetric::EmissiveTailComposedGeometryCapacityBytes,
			mergedGeometry.vertices.capacity() * sizeof(nri_scene::SceneVertex) +
			mergedGeometry.indices.capacity() * sizeof(uint32_t) +
			mergedGeometry.primitives.capacity() * sizeof(nri_scene::PrimitiveData) +
			mergedGeometry.primitiveProvenance.capacity() * sizeof(nri_scene::SurfaceProvenance));
		PerfAuditSet(PerfAuditMetric::EmissiveTailComposedMaterialCapacityBytes,
			mergedMaterials.materials.capacity() * sizeof(nri_scene::MaterialData) +
			mergedMaterials.lightMetadata.capacity() * sizeof(nri_scene::MaterialLightingMetadata) +
			mergedMaterials.textures.capacity() * sizeof(nri_scene::TextureUpload));
		PerfAuditSet(PerfAuditMetric::EmissiveTailRemapCapacityBytes,
			(scratch.liveTextureRemap.capacity() + scratch.tailTextureRemap.capacity()) * sizeof(uint32_t));
		PerfAuditSet(PerfAuditMetric::EmissiveTailLookupKeyCapacityBytes,
			(liveMaterialLayout.textureLookupKeys.capacity() + scratch.materialLayout.textureLookupKeys.capacity()) * sizeof(uint64_t));
		PerfAuditSet(PerfAuditMetric::EmissiveTailTextureLookupBuckets, scratch.textureLookup.bucket_count());
		PerfAuditSet(PerfAuditMetric::EmissiveTailTextureLookupEntries, scratch.textureLookup.size());
	}
	return useMerged;
}
