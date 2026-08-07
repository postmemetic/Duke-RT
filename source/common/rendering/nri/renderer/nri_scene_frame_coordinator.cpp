#include "nri_renderer.h"
#include "nri_cvars.h"

#include "../framegen/nri_framegen.h"
#include "nri_acceleration.h"
#include "nri_diagnostic_names.h"
#include "nri_frame_resources.h"
#include "nri_frame_diagnostics_policy.h"
#include "nri_material_policy.h"
#include "nri_pass_dispatch.h"
#include "nri_persistent_voxel_services.h"
#include "nri_render_geometry_helpers.h"
#include "nri_renderer_settings.h"
#include "nri_scene_frame_builder.h"
#include "nri_scene_frame_diagnostics.h"
#include "nri_scene_frame_coordinator_types.h"
#include "gameupdate.h"
#include "nri_scene_frame_mirrors.h"
#include "nri_scene_frame_overlay.h"
#include "nri_scene_frame_selection.h"
#include "nri_scene_frame_state.h"
#include "nri_scene_upload.h"
#include "nri_static_scene_geometry.h"
#include "nri_surface_light_overlay.h"
#include "nri_runtime_mutation_shared.h"
#include "nri_sky_environment.h"
#include "nri_voxel_compute_meshing.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_scene_stats.h"
#include "../system/nri_renderdevice.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "perf_capture.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "hw_voxels.h"
#include "gamecontrol.h"
#include "gamestate.h"
#include "hw_sections.h"
#include "lightoverlay.h"
#include "mapinfo.h"
#include "printf.h"
#include "gamestruct.h"
#include "hw_portal.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>

namespace
{
	static uint32_t GetBootstrapMode()
	{
		return (uint32_t)std::max(0, std::min((int)nri_ptbootstrapmode, 13));
	}


	static bool TryComputeCapturedSurfaceNormal(const nri_scene::SurfaceRef& surface, float outNormal[3])
	{
		if (surface.vertices.size() < 3)
		{
			return false;
		}

		const nri_scene::CapturedVertex& a = surface.vertices[0];
		const nri_scene::CapturedVertex& b = surface.vertices[1];
		const nri_scene::CapturedVertex& c = surface.vertices[2];
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float lengthSq = nx * nx + ny * ny + nz * nz;
		if (lengthSq <= 1.0e-8f)
		{
			return false;
		}

		const float invLength = 1.0f / std::sqrt(lengthSq);
		outNormal[0] = nx * invLength;
		outNormal[1] = ny * invLength;
		outNormal[2] = nz * invLength;
		return true;
	}

	static void NudgeBlindSpotReplacementFlats(nri_scene::SceneView& sceneView)
	{
		static constexpr float kBlindSpotFlatDepthNudge = 0.01f;

		for (nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			if (surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapFloorSection &&
				surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapCeilingSection)
			{
				continue;
			}

			float normal[3] = {};
			if (!TryComputeCapturedSurfaceNormal(surface, normal))
			{
				continue;
			}

			for (nri_scene::CapturedVertex& vertex : surface.vertices)
			{
				vertex.position[0] += normal[0] * kBlindSpotFlatDepthNudge;
				vertex.position[1] += normal[1] * kBlindSpotFlatDepthNudge;
				vertex.position[2] += normal[2] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[0] += normal[0] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[1] += normal[1] * kBlindSpotFlatDepthNudge;
				vertex.prevPosition[2] += normal[2] * kBlindSpotFlatDepthNudge;
			}
		}
	}

	static void NudgeCapturedSurface(nri_scene::SurfaceRef& surface, float depthNudge)
	{
		float normal[3] = {};
		if (!TryComputeCapturedSurfaceNormal(surface, normal))
		{
			return;
		}

		for (nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			vertex.position[0] += normal[0] * depthNudge;
			vertex.position[1] += normal[1] * depthNudge;
			vertex.position[2] += normal[2] * depthNudge;
			vertex.prevPosition[0] += normal[0] * depthNudge;
			vertex.prevPosition[1] += normal[1] * depthNudge;
			vertex.prevPosition[2] += normal[2] * depthNudge;
		}
	}


	static void RebuildSceneViewStats(nri_scene::SceneView& sceneView)
	{
		const nri_scene::SceneDebugStats preservedStats = sceneView.stats;
		nri_scene::SceneDebugStats stats = {};
		stats.wallDrawItems = (uint32_t)sceneView.opaqueWalls.size();
		stats.flatDrawItems = (uint32_t)sceneView.opaqueFlats.size();
		stats.spriteDrawItems = (uint32_t)sceneView.opaqueSprites.size();

		for (const nri_scene::SurfaceRef& wall : sceneView.opaqueWalls)
		{
			stats.triangleEstimate += !wall.indices.empty() ? (uint32_t)(wall.indices.size() / 3u) : (wall.vertices.size() >= 3 ? (uint32_t)wall.vertices.size() - 2u : 0u);
			stats.materialRefs++;
			if (wall.provenance.sourceType == nri_scene::SurfaceSourceType::MirrorWall)
			{
				stats.mirrorSurfaces++;
			}
		}

		for (const nri_scene::SurfaceRef& flat : sceneView.opaqueFlats)
		{
			stats.triangleEstimate += !flat.indices.empty() ? (uint32_t)(flat.indices.size() / 3u) : (uint32_t)(flat.vertices.size() / 3u);
			stats.materialRefs++;
		}

		for (const nri_scene::SurfaceRef& sprite : sceneView.opaqueSprites)
		{
			stats.triangleEstimate += !sprite.indices.empty() ? (uint32_t)(sprite.indices.size() / 3u) : (sprite.vertices.size() >= 3 ? (uint32_t)sprite.vertices.size() - 2u : 0u);
			stats.materialRefs++;
			if (sprite.provenance.sourceType == nri_scene::SurfaceSourceType::VoxelProxySprite)
			{
				stats.modelDrawItems++;
				stats.voxelProxyDrawItems++;
			}
			else
			{
				stats.translucentDrawItems++;
			}
		}

		stats.totalDrawItems = stats.wallDrawItems + stats.flatDrawItems + stats.spriteDrawItems;
		stats.voxelStableCandidates = preservedStats.voxelStableCandidates;
		stats.voxelStableUncacheable = preservedStats.voxelStableUncacheable;
		stats.voxelStableSignatureHits = preservedStats.voxelStableSignatureHits;
		stats.voxelStableSignatureMisses = preservedStats.voxelStableSignatureMisses;
		stats.voxelStableSignatureChanges = preservedStats.voxelStableSignatureChanges;
		stats.voxelStableSplitStable = preservedStats.voxelStableSplitStable;
		stats.voxelStableSplitLive = preservedStats.voxelStableSplitLive;
		stats.voxelCacheEntries = preservedStats.voxelCacheEntries;
		stats.voxelCacheSurfaceHits = preservedStats.voxelCacheSurfaceHits;
		stats.voxelCacheSurfaceStores = preservedStats.voxelCacheSurfaceStores;
		stats.voxelCacheSurfaceRebuilds = preservedStats.voxelCacheSurfaceRebuilds;
		stats.voxelCacheTransformRebakes = preservedStats.voxelCacheTransformRebakes;
		stats.voxelCacheSurfaceRemoves = preservedStats.voxelCacheSurfaceRemoves;
		stats.voxelCacheNotCaptured = preservedStats.voxelCacheNotCaptured;
		stats.voxelCachePrimitives = preservedStats.voxelCachePrimitives;
		sceneView.stats = stats;
	}


	struct MaterialTextureAttributionCounts
	{
		uint32_t materialCount = 0;
		uint32_t actorMaterialCount = 0;
		uint32_t textureCount = 0;
		uint32_t baseTextureCount = 0;
		uint32_t glowTextureCount = 0;
		uint32_t normalTextureCount = 0;
		uint32_t metallicTextureCount = 0;
		uint32_t roughnessTextureCount = 0;
		uint32_t emissiveTextureCount = 0;
	};

	static bool ShouldTraceActorOverflow()
	{
		return (int)perf_looptraceframes > 0;
	}

	static bool ShouldTraceResidentGeometryOrderHash()
	{
		return (int)perf_looptraceframes > 0;
	}

	static bool ShouldTraceSceneBufferDirtyRanges()
	{
		return (int)perf_looptraceframes > 0;
	}


	static constexpr uint32_t NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES = 16;

	static MaterialTextureAttributionCounts GatherMaterialTextureAttribution(
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<nri_scene::MaterialLightingMetadata>& lightMetadata,
		size_t textureCount)
	{
		MaterialTextureAttributionCounts counts = {};
		counts.materialCount = (uint32_t)materials.size();
		counts.textureCount = (uint32_t)textureCount;

		std::unordered_set<uint32_t> baseTextures;
		std::unordered_set<uint32_t> glowTextures;
		std::unordered_set<uint32_t> normalTextures;
		std::unordered_set<uint32_t> metallicTextures;
		std::unordered_set<uint32_t> roughnessTextures;
		std::unordered_set<uint32_t> emissiveTextures;
		baseTextures.reserve(materials.size());
		glowTextures.reserve(lightMetadata.size());
		normalTextures.reserve(materials.size());
		metallicTextures.reserve(materials.size());
		roughnessTextures.reserve(materials.size());
		emissiveTextures.reserve(materials.size());

		const auto addTextureIndex = [textureCount](std::unordered_set<uint32_t>& destination, uint32_t textureIndex)
		{
			if (textureIndex != UINT32_MAX && (size_t)textureIndex < textureCount)
			{
				destination.insert(textureIndex);
			}
		};

		for (uint32_t materialIndex = 0; materialIndex < (uint32_t)materials.size(); ++materialIndex)
		{
			const auto& material = materials[materialIndex];
			addTextureIndex(baseTextures, material.textureIndex);
			addTextureIndex(normalTextures, material.normalTextureIndex);
			addTextureIndex(metallicTextures, material.metallicTextureIndex);
			addTextureIndex(roughnessTextures, material.roughnessTextureIndex);
			addTextureIndex(emissiveTextures, material.emissiveTextureIndex);
			if (materialIndex < lightMetadata.size())
			{
				const auto& metadata = lightMetadata[materialIndex];
				addTextureIndex(glowTextures, metadata.glowmapTextureIndex);
				if (metadata.actorIndex >= 0)
				{
					counts.actorMaterialCount++;
				}
			}
		}

		counts.baseTextureCount = (uint32_t)baseTextures.size();
		counts.glowTextureCount = (uint32_t)glowTextures.size();
		counts.normalTextureCount = (uint32_t)normalTextures.size();
		counts.metallicTextureCount = (uint32_t)metallicTextures.size();
		counts.roughnessTextureCount = (uint32_t)roughnessTextures.size();
		counts.emissiveTextureCount = (uint32_t)emissiveTextures.size();
		return counts;
	}

	static void AccumulateMaterialTextureAttribution(NRIRenderer::MaterialBuildTraceEntry& entry, const MaterialTextureAttributionCounts& counts)
	{
		entry.materialCount += counts.materialCount;
		entry.actorMaterialCount += counts.actorMaterialCount;
		entry.textureCount += counts.textureCount;
		entry.baseTextureCount += counts.baseTextureCount;
		entry.glowTextureCount += counts.glowTextureCount;
		entry.normalTextureCount += counts.normalTextureCount;
		entry.metallicTextureCount += counts.metallicTextureCount;
		entry.roughnessTextureCount += counts.roughnessTextureCount;
		entry.emissiveTextureCount += counts.emissiveTextureCount;
	}

static void Copy3(const float* src, float* dst)
{
	std::memcpy(dst, src, sizeof(float) * 3);
}

static void Copy2(const float* src, float* dst)
{
	std::memcpy(dst, src, sizeof(float) * 2);
}

static const char* YesNo(bool value)
{
	return value ? "yes" : "no";
}

static double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
{
	return std::chrono::duration<double, std::milli>(end - start).count();
}

static bool ShouldTracePtPerf()
{
	return PerfLoopTraceActive() || ShouldEmitRendererTemporalTraceLogs();
}

static bool ShouldCollectPtPerfTiming()
{
	return ShouldTracePtPerf() || (bool)nri_ptslowdowntrace || PerfCompactCaptureTimingActive();
}

class ScopedPtPerfTimer
{
public:
	explicit ScopedPtPerfTimer(double& targetMs)
		: mTarget(ShouldCollectPtPerfTiming() ? &targetMs : nullptr)
	{
		if (mTarget != nullptr)
		{
			mStart = std::chrono::steady_clock::now();
		}
	}

	~ScopedPtPerfTimer()
	{
		if (mTarget != nullptr)
		{
			*mTarget += DurationMs(mStart, std::chrono::steady_clock::now());
		}
	}

private:
	double* mTarget = nullptr;
	std::chrono::steady_clock::time_point mStart = {};
};

class ScopedPtPerfAttributionFinalizer
{
public:
	explicit ScopedPtPerfAttributionFinalizer(NRIRenderer::PerfShellTraceStats& stats)
		: mStats(stats)
	{
	}

	~ScopedPtPerfAttributionFinalizer()
	{
		const double coreStagesMs =
			mStats.initResourcesMs +
			mStats.mapWorldMs +
			mStats.updateStateMs +
			mStats.sceneSelectMs +
			mStats.sceneLightsMs +
			mStats.residentLightRefreshMs +
			mStats.emissiveUpdateMs +
			mStats.emissiveTlasMs +
			mStats.surfaceProbeMs +
			mStats.frameGraphMs;
		mStats.unattributedMs = CalculateNRIFrameUnattributedMs(
			mStats.totalMs,
			coreStagesMs,
			mStats.postFrameDiagnosticsMs);
		mStats.otherMs = mStats.unattributedMs;
	}

private:
	NRIRenderer::PerfShellTraceStats& mStats;
};

static NRIFrameDiagnosticPolicy GetFrameDiagnosticPolicy()
{
	NRIFrameDiagnosticPolicyInput input = {};
	input.perfLoopTraceActive = PerfLoopTraceActive();
	input.compactCaptureActive = PerfCompactCaptureTimingActive();
	input.selfTestEnabled = nri_ptselftest;
	input.slowdownTraceEnabled = nri_ptslowdowntrace;
	input.sceneStatsEnabled = nri_ptscenestats;
	input.voxelStatsEnabled = nri_voxelstats;
	return EvaluateNRIFrameDiagnosticPolicy(input);
}

}


RenderSceneHistorySnapshot NRIRenderer::CaptureRenderSceneHistorySnapshot(bool preserveHistory) const
{
	RenderSceneHistorySnapshot snapshot = {};
	snapshot.frameIndex = mFrameIndex;
	snapshot.currentTanHalfFovX = mCurrentTanHalfFovX;
	snapshot.currentTanHalfFovY = mCurrentTanHalfFovY;
	snapshot.previousTanHalfFovX = mPreviousTanHalfFovX;
	snapshot.previousTanHalfFovY = mPreviousTanHalfFovY;
	snapshot.hasPreviousCameraState = mHasPreviousCameraState;
	snapshot.resetHistory = mResetHistory;
	if (preserveHistory)
	{
		Copy3(mCurrentCameraPos, snapshot.currentCameraPos);
		Copy3(mCurrentCameraForward, snapshot.currentCameraForward);
		Copy3(mCurrentCameraRight, snapshot.currentCameraRight);
		Copy3(mCurrentCameraUp, snapshot.currentCameraUp);
		Copy3(mPreviousCameraPos, snapshot.previousCameraPos);
		Copy3(mPreviousCameraForward, snapshot.previousCameraForward);
		Copy3(mPreviousCameraRight, snapshot.previousCameraRight);
		Copy3(mPreviousCameraUp, snapshot.previousCameraUp);
		Copy2(mCurrentJitter, snapshot.currentJitter);
		Copy2(mPreviousJitter, snapshot.previousJitter);
		std::memcpy(snapshot.currentViewToClip, mCurrentViewToClip, sizeof(snapshot.currentViewToClip));
		std::memcpy(snapshot.previousViewToClip, mPreviousViewToClip, sizeof(snapshot.previousViewToClip));
		std::memcpy(snapshot.currentWorldToView, mCurrentWorldToView, sizeof(snapshot.currentWorldToView));
		std::memcpy(snapshot.previousWorldToView, mPreviousWorldToView, sizeof(snapshot.previousWorldToView));
	}
	return snapshot;
}

uint32_t NRIRenderer::GetLastCompletedFrameIndex() const
{
	return mLastCompletedFrameIndex;
}

void NRIRenderer::RestoreRenderSceneHistorySnapshot(const RenderSceneHistorySnapshot& snapshot)
{
	mFrameIndex = snapshot.frameIndex;
	Copy3(snapshot.currentCameraPos, mCurrentCameraPos);
	Copy3(snapshot.currentCameraForward, mCurrentCameraForward);
	Copy3(snapshot.currentCameraRight, mCurrentCameraRight);
	Copy3(snapshot.currentCameraUp, mCurrentCameraUp);
	Copy3(snapshot.previousCameraPos, mPreviousCameraPos);
	Copy3(snapshot.previousCameraForward, mPreviousCameraForward);
	Copy3(snapshot.previousCameraRight, mPreviousCameraRight);
	Copy3(snapshot.previousCameraUp, mPreviousCameraUp);
	Copy2(snapshot.currentJitter, mCurrentJitter);
	Copy2(snapshot.previousJitter, mPreviousJitter);
	std::memcpy(mCurrentViewToClip, snapshot.currentViewToClip, sizeof(mCurrentViewToClip));
	std::memcpy(mPreviousViewToClip, snapshot.previousViewToClip, sizeof(mPreviousViewToClip));
	std::memcpy(mCurrentWorldToView, snapshot.currentWorldToView, sizeof(mCurrentWorldToView));
	std::memcpy(mPreviousWorldToView, snapshot.previousWorldToView, sizeof(mPreviousWorldToView));
	mCurrentTanHalfFovX = snapshot.currentTanHalfFovX;
	mCurrentTanHalfFovY = snapshot.currentTanHalfFovY;
	mPreviousTanHalfFovX = snapshot.previousTanHalfFovX;
	mPreviousTanHalfFovY = snapshot.previousTanHalfFovY;
	mHasPreviousCameraState = snapshot.hasPreviousCameraState;
	mResetHistory = snapshot.resetHistory;
}

bool NRIRenderer::EnsureRenderSceneFrameResources(const NRIRendererFrameContext& frameContext, bool preserveHistory, const RenderSceneHistorySnapshot& history)
{
	bool ready = false;
	{
		ScopedPtPerfTimer initPerfTimer(mLastPerfShellTraceStats.initResourcesMs);
		ready =
			Initialize() &&
			NRIFrameResources::EnsureFrameResources(
				*this,
				frameContext.outputWidth,
				frameContext.outputHeight,
				frameContext.targetWidth,
				frameContext.targetHeight);
	}
	if (!ready)
	{
		LogFallback("PT frame resources or pipelines failed to initialize.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}
	return true;
}

bool NRIRenderer::BeginRenderSceneFrame(HWDrawInfo& di, const NRIRendererFrameContext& frameContext, bool preserveHistory, const RenderSceneHistorySnapshot& history)
{
	ResetSceneBufferFrameStats();
	ResetRendererSkyPerfTraceStats();
	nri_scene::ResetAverageTextureColorCache();
	nri_scene::ResetSkyPerfStats();
	mUsedStaticMapSceneLastFrame = false;
	mUsedDynamicSceneLastFrame = false;
	mUploadedStaticMapSceneLastFrame = false;
	mBuiltStaticMapSceneASLastFrame = false;
	mBuiltDynamicSceneASLastFrame = false;
	mDynamicSceneLastFrame = {};
	mRuntimeMutation.BeginFrameState();
	mRuntimeSpaceLinkLastFrame = {};
	if (!preserveHistory)
	{
		mPendingFrameGenerationTimestamp = std::chrono::steady_clock::now();
		mHasPendingFrameGenerationRealFrameTime = false;
		mPendingFrameGenerationRealFrameTimeMs = 0.0f;
		if (mHasFrameGenerationTimestamp)
		{
			const auto elapsed = mPendingFrameGenerationTimestamp - mLastFrameGenerationTimestamp;
			mPendingFrameGenerationRealFrameTimeMs = (float)std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsed).count();
			mHasPendingFrameGenerationRealFrameTime = true;
			if (HasFrameGenerationCadenceBreak())
			{
				ArmTemporalTraceBudget("framegen-cadence-break");
				if (ShouldEmitRendererTemporalTraceLogs())
				{
					Printf("NRI PT frame generation reset: reason=cadence-break frame=%u gap_ms=%.3f renderer_history_reset=no\n",
						mFrameIndex,
						mPendingFrameGenerationRealFrameTimeMs);
				}
			}
		}
	}
	UpdateFrameGenerationHistoryPolicy(frameContext.debugMode, mFrameBuffer->mFrameGeneration.GetPolicy(), frameContext.preserveHistory);

	RefreshMapWorld();
	if (!ApplyStartupMapWorldCorrectionIfNeeded("render-frame-start"))
	{
		LogFallback("PT startup world correction failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}
	if (mPendingStaticMapLightingInvalidation)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.lightingInvalidationsApplied++;
		}
		InvalidateStaticMapSceneForMaterialLighting();
		mPendingStaticMapLightingInvalidation = false;
	}
	UpdatePerFrameState(di, frameContext.drawMode == DM_MAINVIEW &&
		!frameContext.portal && !preserveHistory);
	if (preserveHistory)
	{
		mResetHistory = true;
	}
	return true;
}

bool NRIRenderer::RenderSimpleBootstrapView(bool preserveHistory, const RenderSceneHistorySnapshot& history)
{
	mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
	mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
	mUpscaledInputSlot = FrameTextureSlot::Composed;
	mUseUpscaledInFinal = false;
	NRIPassDispatchContext passContext = BuildPassDispatchContext(false);
	if (!NRIPassDispatcher::DispatchBootstrapView(passContext))
	{
		LogFallback("PT bootstrap view dispatch failed.");
		if (preserveHistory)
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
		return false;
	}

	CopyFinalToActiveTarget();
	if (!preserveHistory)
	{
		NoteSuccessfulRealFrame();
		mLastCompletedFrameIndex = mFrameIndex;
		++mFrameIndex;
		mHasPreviousCameraState = true;
		mResetHistory = false;
	}
	else
	{
		RestoreRenderSceneHistorySnapshot(history);
	}
	return true;
}

bool NRIRenderer::DispatchSelectedRenderScene(const RenderSceneDispatchInputs& inputs)
{
	if (inputs.bootstrapCapturedView)
	{
		mHistoryInputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPing : FrameTextureSlot::TaaHistoryPong;
		mHistoryOutputSlot = (mFrameIndex & 1u) == 0 ? FrameTextureSlot::TaaHistoryPong : FrameTextureSlot::TaaHistoryPing;
		mUpscaledInputSlot = FrameTextureSlot::Composed;
		mUseUpscaledInFinal = false;
		NRIPassDispatchContext passContext = BuildPassDispatchContext(false);
		return inputs.buffersReady && NRIPassDispatcher::DispatchBootstrapView(passContext);
	}

	NRIPassDispatchContext passContext = BuildPassDispatchContext(inputs.mainViewEligible);
	return inputs.accelerationReady &&
		inputs.drawInfo != nullptr &&
		inputs.activeGeometry != nullptr &&
		inputs.activeGpuMaterials != nullptr &&
		NRIPassDispatcher::DispatchFrameGraph(passContext, *inputs.drawInfo, *inputs.activeGeometry, *inputs.activeGpuMaterials, inputs.drawmode);
}

void NRIRenderer::LogRenderSceneFailureReasons(bool paletteReady, bool texturesReady, bool buffersReady, bool accelerationReady, bool dispatched, bool bootstrapCapturedView)
{
	if (!paletteReady)
	{
		LogFallback("PT palette texture upload failed.");
	}
	else if (!texturesReady)
	{
		LogFallback("PT material texture upload failed.");
	}
	else if (!buffersReady)
	{
		LogFallback("PT scene buffer upload failed.");
	}
	else if (!accelerationReady)
	{
		LogFallback("PT acceleration structure build failed.");
	}
	else if (!dispatched)
	{
		LogFallback(bootstrapCapturedView ? "PT bootstrap captured-scene dispatch failed." : "PT frame graph dispatch failed.");
	}
}

void NRIRenderer::CommitRenderSceneResult(const RenderSceneCompletionInputs& inputs, const RenderSceneHistorySnapshot& history)
{
	if (inputs.success)
	{
		mHasLoggedFallback = false;
		if (inputs.bootstrapCapturedView)
		{
			CopyFinalToActiveTarget();
		}

		if (!inputs.preserveHistory)
		{
			NoteSuccessfulRealFrame();
			mLastCompletedFrameIndex = mFrameIndex;
			mFrameIndex++;
			mHasPreviousCameraState = true;
			mResetHistory = false;
		}
		else
		{
			RestoreRenderSceneHistorySnapshot(history);
		}
	}
	else if (inputs.preserveHistory)
	{
		RestoreRenderSceneHistorySnapshot(history);
	}

	{
		ScopedPtPerfTimer diagnosticsTimer(mLastPerfShellTraceStats.postFrameDiagnosticsMs);
		if (inputs.success)
		{
			RecordRenderSceneSuccessStats(inputs);
			EmitSelfTestSummary(inputs.traceFrameIndex, inputs.drawmode, inputs.portal);
		}
		EmitRenderSceneTemporalTrace(inputs.traceFrameIndex);
	}
}

void NRIRenderer::RecordRenderSceneSuccessStats(const RenderSceneCompletionInputs& inputs)
{
	if (inputs.activeGeometry == nullptr || inputs.activeGpuMaterials == nullptr)
	{
		return;
	}

	const NRIFrameDiagnosticPolicy policy = GetFrameDiagnosticPolicy();
	if (!policy.collectDeepSceneAudit)
	{
		mStaticSceneDiagnostics.Discard();
	}
	mLastPerfShellTraceStats.successDiagnosticsBasicCollected = policy.collectBasicSuccessStats;
	mLastPerfShellTraceStats.successDiagnosticsInstanceCompositionCollected = policy.collectInstanceComposition;
	mLastPerfShellTraceStats.successDiagnosticsPersistentVoxelStatusCollected = policy.collectPersistentVoxelStatus;
	mLastPerfShellTraceStats.successDiagnosticsAsSummaryCollected = policy.collectAsSummary;
	mLastPerfShellTraceStats.successDiagnosticsDeepSceneAuditCollected = policy.collectDeepSceneAudit;
	if (!policy.collectBasicSuccessStats &&
		!policy.collectInstanceComposition &&
		!policy.collectPersistentVoxelStatus &&
		!policy.collectAsSummary &&
		!policy.collectDeepSceneAudit)
	{
		return;
	}

	if (policy.collectBasicSuccessStats)
	{
		mLastPerfShellTraceStats.activePrimitiveCount = (uint32_t)inputs.activeGeometry->primitives.size();
		mLastPerfShellTraceStats.dynamicPrimitiveCount = inputs.activeDynamicGeometry != nullptr ? (uint32_t)inputs.activeDynamicGeometry->primitives.size() : 0u;
		mLastPerfShellTraceStats.activeMaterialCount = (uint32_t)inputs.activeGpuMaterials->size();
		mLastPerfShellTraceStats.sceneInstanceCount = (uint32_t)mBoundSceneInstances.size();
	}
	if (policy.collectDeepSceneAudit)
	{
		ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneInstanceStatsMs);
		for (const SceneInstanceData& instance : mBoundSceneInstances)
		{
			mLastPerfShellTraceStats.successDiagnosticsInstanceRowsScanned++;
			mLastPerfShellTraceStats.sceneRecordAuditRecords++;
			mLastPerfShellTraceStats.hitMetadataAuditRecords++;
			mLastPerfShellTraceStats.hitMetadataLegacyPrimitiveOffsetMatches++;
			mLastPerfShellTraceStats.sceneRecordAuditLegacyCompatible++;
			if (instance.visibilityChunk != UINT32_MAX)
			{
				mLastPerfShellTraceStats.sceneRecordAuditVisibilityChunked++;
			}
			if (instance.dataSource == nri_diag::SceneDataSourceStatic)
			{
				mLastPerfShellTraceStats.sceneInstanceStaticCount++;
				mLastPerfShellTraceStats.sceneRecordAuditStatic++;
			}
			else if (instance.dataSource == nri_diag::SceneDataSourceDynamic)
			{
				mLastPerfShellTraceStats.sceneInstanceDynamicCount++;
				mLastPerfShellTraceStats.sceneRecordAuditDynamic++;
			}
			else if (instance.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
			{
				mLastPerfShellTraceStats.sceneInstancePersistentVoxelCount++;
				mLastPerfShellTraceStats.sceneRecordAuditPersistentVoxel++;
				if (instance.materialCount != UINT32_MAX && instance.materialCount > 0)
				{
					mLastPerfShellTraceStats.sceneRecordAuditMaterialIndirection++;
					mLastPerfShellTraceStats.hitMetadataPersistentMaterialBaseRecords++;
				}
			}
			else
			{
				mLastPerfShellTraceStats.sceneRecordAuditInvalidSource++;
			}
		}
	}
	if (policy.collectPersistentVoxelStatus)
	{
		NRIPersistentVoxelStatusSnapshot persistentVoxelStatus = {};
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelResourceStatsMs);
			mPersistentVoxels.FillResourceStatusSnapshot(persistentVoxelStatus);
		}
		{
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.persistentVoxelBatchStatsMs);
			mPersistentVoxels.FillBatchStatusSnapshot(persistentVoxelStatus);
		}
		mLastPerfShellTraceStats.successDiagnosticsPersistentStatusCalls++;
		mLastPerfShellTraceStats.persistentVoxelMeshVariantResourceCount = persistentVoxelStatus.meshVariantResourceCount;
		mLastPerfShellTraceStats.persistentVoxelMaterialVariantResourceCount = persistentVoxelStatus.materialVariantResourceCount;
		mLastPerfShellTraceStats.persistentVoxelBatchActorCount = persistentVoxelStatus.batchActorCount;
		mLastPerfShellTraceStats.persistentVoxelInstanceRecordCount = persistentVoxelStatus.instanceRecordCount;
		mLastPerfShellTraceStats.persistentVoxelAdmissionQueueCount = persistentVoxelStatus.admissionQueueCount;
		mLastPerfShellTraceStats.persistentVoxelPendingInstanceCount = persistentVoxelStatus.pendingInstanceCount;
		mLastPerfShellTraceStats.persistentVoxelResidentResourceBytes = persistentVoxelStatus.residentResourceBytes;
		mLastPerfShellTraceStats.persistentVoxelZeroRefResourceBytes = persistentVoxelStatus.zeroRefResourceBytes;
		mLastPerfShellTraceStats.persistentVoxelZeroRefMeshResourceCount = persistentVoxelStatus.zeroRefMeshResourceCount;
		mLastPerfShellTraceStats.persistentVoxelZeroRefMaterialResourceCount = persistentVoxelStatus.zeroRefMaterialResourceCount;
		mLastPerfShellTraceStats.persistentVoxelInstanceActiveCount = persistentVoxelStatus.activeInstanceCount;
		mLastPerfShellTraceStats.persistentVoxelInstancePrimitiveCount = persistentVoxelStatus.instancePrimitiveCount;
		mLastPerfShellTraceStats.persistentVoxelInstanceMaterialCount = persistentVoxelStatus.instanceMaterialCount;
		mLastPerfShellTraceStats.persistentVoxelInstanceMinPrimitiveCount = persistentVoxelStatus.instanceMinPrimitiveCount;
		mLastPerfShellTraceStats.persistentVoxelInstanceMaxPrimitiveCount = persistentVoxelStatus.instanceMaxPrimitiveCount;
	}
	if (!policy.collectAsSummary)
	{
		mLastPerfShellTraceStats.usedStaticMapScene = mUsedStaticMapSceneLastFrame;
		mLastPerfShellTraceStats.usedDynamicOverlay = mGpuSceneHasDynamicOverlay;
		mLastPerfShellTraceStats.usedPersistentDynamicEmissiveCache = inputs.usingPersistentDynamicEmissiveCache;
		return;
	}
	const NRIPersistentVoxelSharedBlasFrameStats& sharedBlasStats = mPersistentVoxels.GetSharedBlasFrameStats();
	mLastPerfShellTraceStats.voxelSharedBlasActiveActors = sharedBlasStats.activeActors;
	mLastPerfShellTraceStats.voxelSharedBlasUniqueDesiredKeys = sharedBlasStats.uniqueDesiredKeys;
	mLastPerfShellTraceStats.voxelSharedBlasResidentAssets = sharedBlasStats.residentSharedAssets;
	mLastPerfShellTraceStats.voxelSharedBlasQueuedAssets = sharedBlasStats.queuedSharedAssets;
	mLastPerfShellTraceStats.voxelSharedBlasEligibleBuildKeys = sharedBlasStats.eligibleBuildKeys;
	mLastPerfShellTraceStats.voxelSharedBlasBuildAttempts = sharedBlasStats.buildAttempts;
	mLastPerfShellTraceStats.voxelSharedBlasBuildSuccesses = sharedBlasStats.buildSuccesses;
	mLastPerfShellTraceStats.voxelSharedBlasBuildFailures = sharedBlasStats.buildFailures;
	mLastPerfShellTraceStats.voxelSharedBlasCacheHits = sharedBlasStats.cacheHits;
	mLastPerfShellTraceStats.voxelSharedBlasCacheMisses = sharedBlasStats.cacheMisses;
	mLastPerfShellTraceStats.voxelSharedBlasActorRefs = sharedBlasStats.actorRefs;
	mLastPerfShellTraceStats.voxelSharedBlasRoutedLegacy = sharedBlasStats.routedLegacy;
	mLastPerfShellTraceStats.voxelSharedBlasRoutedShared = sharedBlasStats.routedShared;
	mLastPerfShellTraceStats.voxelSharedBlasFallbackLastValid = sharedBlasStats.fallbackLastValid;
	mLastPerfShellTraceStats.voxelSharedBlasActiveReferencedAssets = sharedBlasStats.activeReferencedAssets;
	mLastPerfShellTraceStats.voxelSharedBlasUnreferencedResidentAssets = sharedBlasStats.unreferencedResidentAssets;
	mLastPerfShellTraceStats.voxelSharedBlasResidentBytes = sharedBlasStats.residentBytes;
	mLastPerfShellTraceStats.voxelSharedBlasActiveReferencedBytes = sharedBlasStats.activeReferencedBytes;
	mLastPerfShellTraceStats.voxelSharedBlasUnreferencedResidentBytes = sharedBlasStats.unreferencedResidentBytes;
	mLastPerfShellTraceStats.persistentVoxelResidentResourceBytes += sharedBlasStats.residentBytes;
	mLastPerfShellTraceStats.voxelSharedBlasRouteEligibleActors = sharedBlasStats.routeEligibleActors;
	mLastPerfShellTraceStats.voxelSharedBlasRouteRejectMissingResident = sharedBlasStats.routeRejectMissingResident;
	mLastPerfShellTraceStats.voxelSharedBlasRouteRejectNonLocal = sharedBlasStats.routeRejectNonLocal;
	mLastPerfShellTraceStats.voxelSharedBlasRouteRejectTransformKeyed = sharedBlasStats.routeRejectTransformKeyed;
	mLastPerfShellTraceStats.voxelSharedBlasRouteRejectInvalidMaterial = sharedBlasStats.routeRejectInvalidMaterial;
	mLastPerfShellTraceStats.voxelSharedBlasRouteRejectInvalidTransform = sharedBlasStats.routeRejectInvalidTransform;
	mLastPerfShellTraceStats.voxelSharedBlasRouteRejectGeometryMismatch = sharedBlasStats.routeRejectGeometryMismatch;
	mLastPerfShellTraceStats.voxelSharedBlasRejectMissingKey = sharedBlasStats.rejectMissingKey;
	mLastPerfShellTraceStats.voxelSharedBlasRejectDisabled = sharedBlasStats.rejectDisabled;
	mLastPerfShellTraceStats.voxelSharedBlasRejectNonLocal = sharedBlasStats.rejectNonLocal;
	mLastPerfShellTraceStats.voxelSharedBlasRejectTransformKeyed = sharedBlasStats.rejectTransformKeyed;
	mLastPerfShellTraceStats.voxelSharedBlasRejectMissingBuffers = sharedBlasStats.rejectMissingBuffers;
	mLastPerfShellTraceStats.voxelSharedBlasRejectInvalidCounts = sharedBlasStats.rejectInvalidCounts;
	mLastPerfShellTraceStats.voxelSharedBlasRejectBuildBudget = sharedBlasStats.rejectBuildBudget;
	mLastPerfShellTraceStats.voxelSharedBlasRejectGeometryMismatch = sharedBlasStats.rejectGeometryMismatch;
	mLastPerfShellTraceStats.voxelLocalShareProfileActiveActors = sharedBlasStats.profileActiveActors;
	mLastPerfShellTraceStats.voxelLocalShareProfileLocalSpaceActors = sharedBlasStats.profileLocalSpaceActors;
	mLastPerfShellTraceStats.voxelLocalShareProfileBakedTransformActors = sharedBlasStats.profileBakedTransformActors;
	mLastPerfShellTraceStats.voxelLocalShareProfileUnknownSpaceActors = sharedBlasStats.profileUnknownSpaceActors;
	mLastPerfShellTraceStats.voxelLocalShareProfileTransformKeyedActors = sharedBlasStats.profileTransformKeyedActors;
	mLastPerfShellTraceStats.voxelLocalShareProfileLocalIdentityTransformActors = sharedBlasStats.profileLocalIdentityTransformActors;
	mLastPerfShellTraceStats.voxelLocalShareProfileLocalNonIdentityTransformActors = sharedBlasStats.profileLocalNonIdentityTransformActors;
	mLastPerfShellTraceStats.voxelLocalShareProfileShareableLocalActors = sharedBlasStats.profileShareableLocalActors;
	mLastPerfShellTraceStats.voxelLocalShareProfileShareableUniqueKeys = sharedBlasStats.profileShareableUniqueKeys;
	mLastPerfShellTraceStats.voxelLocalShareProfileShareableDuplicateActorRefs = sharedBlasStats.profileShareableDuplicateActorRefs;
	mLastPerfShellTraceStats.voxelLocalShareProfileShareableSingleActorKeys = sharedBlasStats.profileShareableSingleActorKeys;
	mLastPerfShellTraceStats.voxelLocalShareProfileShareableMultiActorKeys = sharedBlasStats.profileShareableMultiActorKeys;
	mLastPerfShellTraceStats.voxelLocalShareProfileResidentShareableKeys = sharedBlasStats.profileResidentShareableKeys;
	mLastPerfShellTraceStats.voxelLocalShareProfileEligibleNotResidentActors = sharedBlasStats.profileEligibleNotResidentActors;
	mLastPerfShellTraceStats.voxelLocalShareProfileRejectMissingMesh = sharedBlasStats.profileRejectMissingMesh;
	mLastPerfShellTraceStats.voxelLocalShareProfileRejectNonLocal = sharedBlasStats.profileRejectNonLocal;
	mLastPerfShellTraceStats.voxelLocalShareProfileRejectTransformKeyed = sharedBlasStats.profileRejectTransformKeyed;
	mLastPerfShellTraceStats.voxelLocalShareProfileRejectMissingBuffers = sharedBlasStats.profileRejectMissingBuffers;
	mLastPerfShellTraceStats.voxelLocalShareProfileRejectInvalidCounts = sharedBlasStats.profileRejectInvalidCounts;
	mLastPerfShellTraceStats.voxelLocalShareProfileRejectInvalidMaterial = sharedBlasStats.profileRejectInvalidMaterial;
	mLastPerfShellTraceStats.voxelLocalShareProfileRejectInvalidTransform = sharedBlasStats.profileRejectInvalidTransform;
	mLastPerfShellTraceStats.voxelLocalShareProfileRejectGeometryMismatch = sharedBlasStats.profileRejectGeometryMismatch;
	mLastPerfShellTraceStats.voxelSharedKeyAuditActors = sharedBlasStats.keyAuditActors;
	mLastPerfShellTraceStats.voxelSharedKeyAuditKeys = sharedBlasStats.keyAuditKeys;
	mLastPerfShellTraceStats.voxelSharedKeyAuditSafeKeys = sharedBlasStats.keyAuditSafeKeys;
	mLastPerfShellTraceStats.voxelSharedKeyAuditUnsafeKeys = sharedBlasStats.keyAuditUnsafeKeys;
	mLastPerfShellTraceStats.voxelSharedKeyAuditGeometryMismatchKeys = sharedBlasStats.keyAuditGeometryMismatchKeys;
	mLastPerfShellTraceStats.voxelSharedKeyAuditCountMismatchKeys = sharedBlasStats.keyAuditCountMismatchKeys;
	mLastPerfShellTraceStats.voxelSharedKeyAuditMaterialVariantKeys = sharedBlasStats.keyAuditMaterialVariantKeys;
	mLastPerfShellTraceStats.voxelSharedKeyAuditMaterialCountMismatchKeys = sharedBlasStats.keyAuditMaterialCountMismatchKeys;
	mLastPerfShellTraceStats.voxelSharedKeyAuditSourcePicnumAliasKeys = sharedBlasStats.keyAuditSourcePicnumAliasKeys;
	mLastPerfShellTraceStats.voxelSharedKeyAuditVoxelIndexAliasKeys = sharedBlasStats.keyAuditVoxelIndexAliasKeys;
	mLastPerfShellTraceStats.voxelSharedKeyAuditSourceStateAliasActorRefs = sharedBlasStats.keyAuditSourceStateAliasActorRefs;
	mLastPerfShellTraceStats.voxelSharedKeyAuditBakeSpaceMismatchKeys = sharedBlasStats.keyAuditBakeSpaceMismatchKeys;
	mLastPerfShellTraceStats.voxelSharedKeyAuditTransformBasisMismatchKeys = sharedBlasStats.keyAuditTransformBasisMismatchKeys;
	mLastPerfShellTraceStats.voxelSharedKeyAuditLocalShareableUnsafeKeys = sharedBlasStats.keyAuditLocalShareableUnsafeKeys;
	mLastPerfShellTraceStats.voxelLocalSpaceInvariantLocalActors = sharedBlasStats.invariantLocalActors;
	mLastPerfShellTraceStats.voxelLocalSpaceInvariantLocalIdentityTransformActors = sharedBlasStats.invariantLocalIdentityTransformActors;
	mLastPerfShellTraceStats.voxelLocalSpaceInvariantLocalNonIdentityTransformActors = sharedBlasStats.invariantLocalNonIdentityTransformActors;
	mLastPerfShellTraceStats.voxelLocalSpaceInvariantSuspiciousWorldBoundsActors = sharedBlasStats.invariantSuspiciousWorldBoundsActors;
	mLastPerfShellTraceStats.voxelLocalSpaceInvariantMissingBoundsActors = sharedBlasStats.invariantMissingBoundsActors;
	mLastPerfShellTraceStats.voxelLocalSpaceInvariantInvalidTransformActors = sharedBlasStats.invariantInvalidTransformActors;
	mLastPerfShellTraceStats.voxelLocalSpaceInvariantBakedFallbackActors = sharedBlasStats.invariantBakedFallbackActors;
	mLastPerfShellTraceStats.voxelLocalSpaceInvariantUnknownSpaceActors = sharedBlasStats.invariantUnknownSpaceActors;
	mLastPerfShellTraceStats.voxelLocalSpaceInvariantMaxBoundsCenterMagnitude = sharedBlasStats.invariantMaxBoundsCenterMagnitude;
	mLastPerfShellTraceStats.voxelLocalSpaceInvariantMaxBoundsAbs = sharedBlasStats.invariantMaxBoundsAbs;
	const NRIWorldTlasFrameSlot* worldTlasFrameSlot =
		static_cast<const NRIRenderer*>(this)->GetCurrentWorldTlasFrameSlot();
	mLastPerfShellTraceStats.asWorldTlasObjects =
		worldTlasFrameSlot != nullptr &&
		worldTlasFrameSlot->accelerationStructure.accelerationStructure != nullptr &&
		mActiveTlasInstanceCount > 0 ? 1u : 0u;
	mLastPerfShellTraceStats.asWorldTlasEntries = mActiveTlasInstanceCount;
	mLastPerfShellTraceStats.asWorldTlasMaskAllWorkloadsRefs = mLastPerfShellTraceStats.asWorldTlasEntries;
	mLastPerfShellTraceStats.asWorldTlasMaskOtherRefs = 0;
	mLastPerfShellTraceStats.asEmissiveTlasObjects = mEmissiveTopLevelAS.accelerationStructure != nullptr && mEmissiveTlasInstanceCount > 0 ? 1u : 0u;
	mLastPerfShellTraceStats.asEmissiveTlasEntries = mEmissiveTlasInstanceCount;
	mLastPerfShellTraceStats.asEntriesStatic = mLastPerfShellTraceStats.sceneInstanceStaticCount;
	mLastPerfShellTraceStats.asEntriesDynamic = mLastPerfShellTraceStats.sceneInstanceDynamicCount;
	mLastPerfShellTraceStats.asEntriesVoxel = mLastPerfShellTraceStats.sceneInstancePersistentVoxelCount;
	mLastPerfShellTraceStats.asSceneRecords = mLastPerfShellTraceStats.sceneInstanceCount;
	mLastPerfShellTraceStats.asBlasDynamic = HasAnyDynamicBottomLevelAS() ? 1u : 0u;
	mLastPerfShellTraceStats.asDynamicUniqueGeometrySignatures = mLastPerfShellTraceStats.asBlasDynamic;
	mLastPerfShellTraceStats.asMonolithicDynamicBlasBuilds = mLastPerfShellTraceStats.dynamicAsCreateCalls > 0 ? 1u : 0u;
	mLastPerfShellTraceStats.asBlasVoxelUnique = mLastPerfShellTraceStats.persistentVoxelSharedMeshResources;
	mLastPerfShellTraceStats.asBlasVoxelActor = mLastPerfShellTraceStats.persistentVoxelMeshVariantResourceCount;
	mLastPerfShellTraceStats.asVoxelUniqueGeometryKeys =
		mLastPerfShellTraceStats.persistentVoxelSharedMeshResources != 0 ?
		mLastPerfShellTraceStats.persistentVoxelSharedMeshResources :
		mLastPerfShellTraceStats.voxelCacheUniqueMeshKeys;
	mLastPerfShellTraceStats.asVoxelActorInstances = mLastPerfShellTraceStats.sceneInstancePersistentVoxelCount;
	mLastPerfShellTraceStats.asVoxelSharedBlasRefs =
		mLastPerfShellTraceStats.asVoxelActorInstances > mLastPerfShellTraceStats.asVoxelUniqueGeometryKeys ?
		mLastPerfShellTraceStats.asVoxelActorInstances - mLastPerfShellTraceStats.asVoxelUniqueGeometryKeys :
		0u;
	const ResidentMapChunkRegistry& staticRegistry = mStaticSceneResidency.Registry();
	if (staticRegistry.valid)
	{
		mLastPerfShellTraceStats.asBlasStatic = staticRegistry.accelerationResidentChunkCount;
		mLastPerfShellTraceStats.asStaticChunkOwnedBlas = staticRegistry.accelerationResidentChunkCount;
		mLastPerfShellTraceStats.asStaticUniqueGeometrySignatures = staticRegistry.accelerationResidentChunkCount;
	}
	if (policy.collectDeepSceneAudit)
	{
		NRIStaticSceneDiagnosticsInput diagnosticsInput = {};
		diagnosticsInput.mapWorld = &mMapWorld;
		diagnosticsInput.staticScene = &mStaticMapScene;
		diagnosticsInput.atlas = &mStaticMapChunkAtlas;
		diagnosticsInput.registry = &staticRegistry;
		diagnosticsInput.builtStaticMapSceneASLastFrame = mBuiltStaticMapSceneASLastFrame;
		const NRIStaticSceneDiagnosticsSnapshot diagnostics =
			mStaticSceneDiagnostics.Build(diagnosticsInput);
		mLastPerfShellTraceStats.successDiagnosticsDeepSceneAuditCacheHits += diagnostics.cacheHit ? 1u : 0u;
		mLastPerfShellTraceStats.successDiagnosticsDeepSceneAuditRebuilds += diagnostics.cacheHit ? 0u : 1u;
		mLastPerfShellTraceStats.successDiagnosticsStaticChunkRowsScanned += diagnostics.diagnosticChunkRowsScanned;
		mLastPerfShellTraceStats.successDiagnosticsStaticChunkRowsIncrementallyUpdated +=
			diagnostics.diagnosticChunkRowsIncrementallyUpdated;
		mLastPerfShellTraceStats.successDiagnosticsStaticSurfaceRowsScanned += diagnostics.diagnosticSurfaceRowsScanned;
		mLastPerfShellTraceStats.successDiagnosticsStaticSurfaceRowsIncrementallyUpdated +=
			diagnostics.diagnosticSurfaceRowsIncrementallyUpdated;
		mLastPerfShellTraceStats.successDiagnosticsRegistryRowsScanned += diagnostics.diagnosticRegistryRowsScanned;
		mLastPerfShellTraceStats.successDiagnosticsTemporaryContainersBuilt += diagnostics.diagnosticContainersBuilt;
		mLastPerfShellTraceStats.asBlasStatic = diagnostics.asBlasStatic;
		mLastPerfShellTraceStats.asStaticChunkOwnedBlas = diagnostics.asStaticChunkOwnedBlas;
		mLastPerfShellTraceStats.asStaticUniqueGeometrySignatures = diagnostics.asStaticUniqueGeometrySignatures;
		mLastPerfShellTraceStats.asStaticSegmentBlas = diagnostics.asStaticSegmentBlas;
		mLastPerfShellTraceStats.asStaticSegmentCandidateChunks = diagnostics.asStaticSegmentCandidateChunks;
		mLastPerfShellTraceStats.asStaticSegmentUniqueGeometrySignatures = diagnostics.asStaticSegmentUniqueGeometrySignatures;
		mLastPerfShellTraceStats.asStaticSegmentDuplicateKeys = diagnostics.asStaticSegmentDuplicateKeys;
		mLastPerfShellTraceStats.asStaticSegmentDuplicateRefs = diagnostics.asStaticSegmentDuplicateRefs;
		mLastPerfShellTraceStats.asStaticSegmentPortalChunks = diagnostics.asStaticSegmentPortalChunks;
		mLastPerfShellTraceStats.asStaticSegmentLocalSpaceChunks = diagnostics.asStaticSegmentLocalSpaceChunks;
		mLastPerfShellTraceStats.asStaticSegmentAnimatedChunks = diagnostics.asStaticSegmentAnimatedChunks;
		mLastPerfShellTraceStats.asStaticSegmentAtlasEligibleChunks = diagnostics.asStaticSegmentAtlasEligibleChunks;
		mLastPerfShellTraceStats.asStaticSegmentRegistryMappedChunks = diagnostics.asStaticSegmentRegistryMappedChunks;
		mLastPerfShellTraceStats.asStaticSegmentCandidateSurfaces = diagnostics.asStaticSegmentCandidateSurfaces;
		mLastPerfShellTraceStats.asStaticSegmentWallCandidates = diagnostics.asStaticSegmentWallCandidates;
		mLastPerfShellTraceStats.asStaticSegmentFloorCandidates = diagnostics.asStaticSegmentFloorCandidates;
		mLastPerfShellTraceStats.asStaticSegmentCeilingCandidates = diagnostics.asStaticSegmentCeilingCandidates;
		mLastPerfShellTraceStats.asStaticSegmentPortalCandidates = diagnostics.asStaticSegmentPortalCandidates;
		mLastPerfShellTraceStats.asStaticSegmentLocalSpaceSurfaces = diagnostics.asStaticSegmentLocalSpaceSurfaces;
		mLastPerfShellTraceStats.asStaticSegmentAnimatedSurfaces = diagnostics.asStaticSegmentAnimatedSurfaces;
		mLastPerfShellTraceStats.asStaticSegmentMaterialRiskSurfaces = diagnostics.asStaticSegmentMaterialRiskSurfaces;
		mLastPerfShellTraceStats.asStaticSegmentContiguousChunkSurfaces = diagnostics.asStaticSegmentContiguousChunkSurfaces;
		mLastPerfShellTraceStats.asStaticSegmentCacheCandidates = diagnostics.asStaticSegmentCacheCandidates;
		mLastPerfShellTraceStats.asStaticSegmentCacheEntries = diagnostics.asStaticSegmentCacheEntries;
		mLastPerfShellTraceStats.asStaticSegmentCacheHits = diagnostics.asStaticSegmentCacheHits;
		mLastPerfShellTraceStats.asStaticSegmentCacheMisses = diagnostics.asStaticSegmentCacheMisses;
		mLastPerfShellTraceStats.asStaticSegmentCacheDuplicateRefs = diagnostics.asStaticSegmentCacheDuplicateRefs;
		mLastPerfShellTraceStats.asStaticSegmentCacheResidentBlas = diagnostics.asStaticSegmentCacheResidentBlas;
		mLastPerfShellTraceStats.asStaticSegmentCacheBuildsThisFrame = diagnostics.asStaticSegmentCacheBuildsThisFrame;
		mLastPerfShellTraceStats.asStaticSegmentCacheBuildsLastRebuild = diagnostics.asStaticSegmentCacheBuildsLastRebuild;
		mLastPerfShellTraceStats.asStaticSegmentCacheInvalidations = diagnostics.asStaticSegmentCacheInvalidations;
		mLastPerfShellTraceStats.asStaticSegmentCacheResidentBytes = diagnostics.asStaticSegmentCacheResidentBytes;
		mLastPerfShellTraceStats.asStaticSegmentCacheBlasBuildEnabled = diagnostics.asStaticSegmentCacheBlasBuildEnabled;
		mLastPerfShellTraceStats.asStaticSegmentRouteRouted = diagnostics.asStaticSegmentRouteRouted;
		mLastPerfShellTraceStats.asStaticSegmentRouteChunkFallback = diagnostics.asStaticSegmentRouteChunkFallback;
		mLastPerfShellTraceStats.asStaticSegmentRouteRejectDisabled = diagnostics.asStaticSegmentRouteRejectDisabled;
		mLastPerfShellTraceStats.asStaticSegmentRouteRejectMissingCache = diagnostics.asStaticSegmentRouteRejectMissingCache;
		mLastPerfShellTraceStats.asStaticSegmentRouteRejectMissingBlas = diagnostics.asStaticSegmentRouteRejectMissingBlas;
		mLastPerfShellTraceStats.asStaticSegmentRouteSegmentBlasRefs = diagnostics.asStaticSegmentRouteSegmentBlasRefs;
		mLastPerfShellTraceStats.asStaticSegmentRouteChunkBlasRefs = diagnostics.asStaticSegmentRouteChunkBlasRefs;
	}
	mLastPerfShellTraceStats.asBlasBuiltThisFrame =
		mLastPerfShellTraceStats.dynamicAsCreateCalls +
		mLastPerfShellTraceStats.persistentVoxelAsBuilds +
		mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasRecreateCount +
		mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasUpdateCount;
	mLastPerfShellTraceStats.asBlasCacheHits =
		mLastPerfShellTraceStats.dynamicAsReuseCount +
		mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasReuseCount +
		mLastPerfShellTraceStats.runtimeMutationResidentApplyBlasScratchCacheHitCount;
	mLastPerfShellTraceStats.usedStaticMapScene = mUsedStaticMapSceneLastFrame;
	mLastPerfShellTraceStats.usedDynamicOverlay = mGpuSceneHasDynamicOverlay;
	mLastPerfShellTraceStats.usedPersistentDynamicEmissiveCache = inputs.usingPersistentDynamicEmissiveCache;
}

void NRIRenderer::EmitRenderSceneTemporalTrace(uint32_t traceFrameIndex)
{
	if (!ShouldEmitRendererTemporalTraceLogs())
	{
		return;
	}

	const auto& analyticLights = mSceneLights.GetAnalyticLights();
	const auto& emissiveSurfaces = mSceneLights.GetEmissiveSurfaces();
	Printf("NRI PT light trace: frame=%u analytic=%u topo=%s prop=%s added=%u removed=%u rebound=%u ordered_key_hash=0x%016llx soft=%u surviving_index_changed=%u surviving_soft_index_changed=%u emissive=%u topo=%s prop=%s added=%u removed=%u rebound=%u reset=%s reason=%s\n",
		traceFrameIndex,
		(uint32_t)analyticLights.activeLights.size(),
		YesNo(analyticLights.lastBuildTopologyChanged),
		YesNo(analyticLights.lastBuildPropertiesChanged),
		(uint32_t)analyticLights.addedTopologyKeys.size(),
		(uint32_t)analyticLights.removedTopologyKeys.size(),
		(uint32_t)analyticLights.reboundTopologyKeys.size(),
		(unsigned long long)analyticLights.orderedStableKeyHash,
		analyticLights.softLightCount,
		analyticLights.survivingKeyIndexChangeCount,
		analyticLights.survivingSoftLightIndexChangeCount,
		(uint32_t)emissiveSurfaces.activeSurfaces.size(),
		YesNo(emissiveSurfaces.lastBuildTopologyChanged),
		YesNo(emissiveSurfaces.lastBuildPropertiesChanged),
		(uint32_t)emissiveSurfaces.addedTopologyKeys.size(),
		(uint32_t)emissiveSurfaces.removedTopologyKeys.size(),
		(uint32_t)emissiveSurfaces.reboundTopologyKeys.size(),
		YesNo(mResetHistory),
		mResetHistory ? mLastHistoryResetReason.c_str() : "none");

	const nri_scene::SkyPerfStats sceneSkyPerf = nri_scene::ConsumeSkyPerfStats();
	Printf("NRI PT sky perf: frame=%u ensure_scene=%u preserve_scene=%u rebuild_scene=%u ensure_sky=%u preserve_hit=%u reuse_active=%u reuse_probe=%u probe=%u/%u face_probes=%u uploads=%u ensure_ms=%.3f probe_ms=%.3f face_ms=%.3f upload_ms=%.3f static_builds=%u overlay_builds=%u\n",
		traceFrameIndex,
		gRendererSkyPerfTraceStats.ensureSceneTexturesCalls,
		gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveTrueCalls,
		gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveFalseCalls,
		gRendererSkyPerfTraceStats.ensureSkyCalls,
		gRendererSkyPerfTraceStats.preserveExistingHits,
		gRendererSkyPerfTraceStats.reuseActiveCubemapHits + gRendererSkyPerfTraceStats.solidReuseHits,
		gRendererSkyPerfTraceStats.reuseActiveProbeHits,
		gRendererSkyPerfTraceStats.probeSuccesses,
		gRendererSkyPerfTraceStats.probeAttempts,
		gRendererSkyPerfTraceStats.probeFaceCalls,
		gRendererSkyPerfTraceStats.buildCubemapUploadCalls,
		(double)gRendererSkyPerfTraceStats.ensureSkyTimeUs / 1000.0,
		(double)gRendererSkyPerfTraceStats.probeCubemapTimeUs / 1000.0,
		(double)gRendererSkyPerfTraceStats.probeFaceTimeUs / 1000.0,
		(double)gRendererSkyPerfTraceStats.buildCubemapUploadTimeUs / 1000.0,
		gRendererSkyPerfTraceStats.residentStaticSceneTextureBuilds,
		gRendererSkyPerfTraceStats.combinedOverlayTextureBuilds);
	Printf("NRI PT sky scene: frame=%u updates=%u wall=%u flat=%u portal=%u inspects=%u cubemap_candidates=%u solid_candidates=%u inspect_faces=%u avg_base=%u avg_recursive=%u recursive_faces=%u avg_pixels=%llu update_ms=%.3f inspect_ms=%.3f avg_ms=%.3f\n",
		traceFrameIndex,
		sceneSkyPerf.updateCalls,
		sceneSkyPerf.wallUpdateCalls,
		sceneSkyPerf.flatUpdateCalls,
		sceneSkyPerf.portalUpdateCalls,
		sceneSkyPerf.inspectCalls,
		sceneSkyPerf.inspectCubemapCandidates,
		sceneSkyPerf.inspectSolidCandidates,
		sceneSkyPerf.inspectFaceWalks,
		sceneSkyPerf.averageColorBaseCalls,
		sceneSkyPerf.averageColorRecursiveCalls,
		sceneSkyPerf.recursiveSkyboxFaceSamples,
		(unsigned long long)sceneSkyPerf.averageColorPixels,
		(double)sceneSkyPerf.updateTimeUs / 1000.0,
		(double)sceneSkyPerf.inspectTimeUs / 1000.0,
		(double)sceneSkyPerf.averageColorTimeUs / 1000.0);
	Printf("NRI PT sky invalidation: frame=%u requests=%u applied=%u emissive_material_dirty=%u keep_last=%u hold_level=%u cached_cubemap=%u create_cubemap=%u cached_solid=%u create_solid=%u\n",
		traceFrameIndex,
		gRendererSkyPerfTraceStats.lightingInvalidationRequests,
		gRendererSkyPerfTraceStats.lightingInvalidationsApplied,
		gRendererSkyPerfTraceStats.emissiveMaterialDirtyEvents,
		gRendererSkyPerfTraceStats.keepLastCubemapHits,
		gRendererSkyPerfTraceStats.holdLevelCubemapHits,
		gRendererSkyPerfTraceStats.activateCachedCubemapHits,
		gRendererSkyPerfTraceStats.createCachedCubemapHits,
		gRendererSkyPerfTraceStats.solidActivateHits,
		gRendererSkyPerfTraceStats.solidCreateHits);
}

bool NRIRenderer::RenderScene(HWDrawInfo& di, int drawmode, bool portal)
{
	if ((drawmode != DM_MAINVIEW && drawmode != DM_OFFSCREEN) || portal || mFrameBuffer == nullptr ||
		mFrameBuffer->mCommandBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return false;
	}

	if (!mPathTracingSupported)
	{
		LogFallback(GetAvailabilityReason());
		return false;
	}

	ResetPerfTraceStats();
	ScopedPtPerfAttributionFinalizer attributionFinalizer(mLastPerfShellTraceStats);
	ScopedPtPerfTimer totalPerfTimer(mLastPerfShellTraceStats.totalMs);
	Clocker totalClock(NriPTAll);

	const uint32_t bootstrapMode = GetBootstrapMode();
	const bool bootstrapSimpleView = nri_ptbootstrap && bootstrapMode <= 3u;
	const bool bootstrapCapturedView = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 12u;
	const bool bootstrapCapturedDiagnostics = nri_ptbootstrap && bootstrapMode >= 4u && bootstrapMode <= 10u;
	const bool bootstrapCapturedFlat = nri_ptbootstrap && bootstrapMode == 11u;
	const bool bootstrapCapturedBaseColor = nri_ptbootstrap && bootstrapMode == 12u;
	const bool rawTraceDirectScene = !nri_ptbootstrap && nri_ptdirectscene;
	const int debugMode = (int)nri_ptdebug;

	const bool preserveHistory = drawmode != DM_MAINVIEW;
	const NRIRendererFrameContext frameContext = BuildFrameContext(drawmode, portal, debugMode, preserveHistory);
	const uint32_t traceFrameIndex = frameContext.frameIndex;
	const RenderSceneHistorySnapshot history = CaptureRenderSceneHistorySnapshot(preserveHistory);
	if (!EnsureRenderSceneFrameResources(frameContext, preserveHistory, history) ||
		!BeginRenderSceneFrame(di, frameContext, preserveHistory, history))
	{
		return false;
	}

	if (bootstrapSimpleView)
	{
		return RenderSimpleBootstrapView(preserveHistory, history);
	}

	RenderSceneFrameBuildInputs sceneFrameInputs = {};
	const GameUpdateSnapshot gameUpdate = GetGameUpdateSnapshot();
	sceneFrameInputs.simulationGeneration = gameUpdate.simulationGeneration;
	sceneFrameInputs.engineUpdateGeneration = gameUpdate.engineUpdateGeneration;
	sceneFrameInputs.presentationGeneration = gameUpdate.presentationGeneration;
	sceneFrameInputs.ticksExecutedThisPresentation = gameUpdate.ticksExecutedThisPresentation;
	sceneFrameInputs.bootstrapMode = bootstrapMode;
	sceneFrameInputs.bootstrapCapturedView = bootstrapCapturedView;
	sceneFrameInputs.bootstrapCapturedDiagnostics = bootstrapCapturedDiagnostics;
	sceneFrameInputs.bootstrapCapturedFlat = bootstrapCapturedFlat;
	sceneFrameInputs.bootstrapCapturedBaseColor = bootstrapCapturedBaseColor;
	sceneFrameInputs.rawTraceDirectScene = rawTraceDirectScene;
	sceneFrameInputs.preserveHistory = preserveHistory;
	RenderSceneFrameBuildResult sceneFrame;
	if (!BuildRenderSceneFrame(di, sceneFrameInputs, history, sceneFrame))
	{
		return false;
	}
	DispatchNRIVoxelComputeMeshingDiagnostics(*this, traceFrameIndex);
	RenderSceneDispatchInputs dispatchInputs = {};
	dispatchInputs.bootstrapCapturedView = bootstrapCapturedView;
	dispatchInputs.buffersReady = sceneFrame.buffersReady;
	dispatchInputs.accelerationReady = sceneFrame.accelerationReady;
	dispatchInputs.mainViewEligible = drawmode == DM_MAINVIEW && !portal && !preserveHistory;
	dispatchInputs.drawInfo = &di;
	dispatchInputs.activeGeometry = sceneFrame.activeGeometry;
	dispatchInputs.activeGpuMaterials = sceneFrame.activeGpuMaterials;
	dispatchInputs.drawmode = drawmode;
	const bool dispatched = DispatchSelectedRenderScene(dispatchInputs);
	const bool success = sceneFrame.paletteReady && sceneFrame.texturesReady && sceneFrame.buffersReady && sceneFrame.accelerationReady && dispatched;
	LogRenderSceneFailureReasons(sceneFrame.paletteReady, sceneFrame.texturesReady, sceneFrame.buffersReady, sceneFrame.accelerationReady, dispatched, bootstrapCapturedView);

	RenderSceneCompletionInputs completionInputs = {};
	completionInputs.success = success;
	completionInputs.preserveHistory = preserveHistory;
	completionInputs.bootstrapCapturedView = bootstrapCapturedView;
	completionInputs.traceFrameIndex = traceFrameIndex;
	completionInputs.drawmode = drawmode;
	completionInputs.portal = portal;
	completionInputs.activeGeometry = sceneFrame.activeGeometry;
	completionInputs.activeGpuMaterials = sceneFrame.activeGpuMaterials;
	completionInputs.activeDynamicGeometry = sceneFrame.activeDynamicGeometry;
	completionInputs.usingPersistentDynamicEmissiveCache = sceneFrame.usingPersistentDynamicEmissiveCache;
	CommitRenderSceneResult(completionInputs, history);

	return success;
}
