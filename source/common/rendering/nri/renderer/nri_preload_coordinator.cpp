#include "nri_preload_coordinator.h"
#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_frame_resources.h"
#include "nri_renderer_settings.h"
#include "nri_sky_environment.h"
#include "nri_voxel_compute_meshing.h"
#include "nri_voxel_compute_preload.h"
#include "../system/nri_renderdevice.h"

#include "mapinfo.h"
#include "c_dispatch.h"
#include "printf.h"

#include <cstring>

namespace
{
	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	class ScopedPreloadPerfTimer
	{
	public:
		explicit ScopedPreloadPerfTimer(double& targetMs)
			: mTarget(&targetMs)
			, mStart(std::chrono::steady_clock::now())
		{
		}

		~ScopedPreloadPerfTimer()
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

	struct VoxelPreloadTimeline
	{
		uint64_t buildSerial = 0;
		std::chrono::steady_clock::time_point firstSeen = {};
		uint64_t peakTrackedBytes = 0;
		uint64_t lastLoggedPeakBytes = 0;
		uint64_t releaseCommandArmedBuildSerial = 0;
		uint64_t releaseCommandQueuedBuildSerial = 0;
		bool strictAbortLatched = false;
		bool strictTerminalLogged = false;
	};

	VoxelPreloadTimeline gVoxelPreloadTimeline;

	void UpdateVoxelPreloadTimeline(
		NRIRenderer& renderer,
		uint64_t buildSerial,
		const NRIAdapterMemoryTelemetry& adapterMemory,
		const char* stage)
	{
		const auto now = std::chrono::steady_clock::now();
		if (gVoxelPreloadTimeline.buildSerial != buildSerial)
		{
			gVoxelPreloadTimeline = {};
			gVoxelPreloadTimeline.buildSerial = buildSerial;
			gVoxelPreloadTimeline.firstSeen = now;
		}
		const NRIRenderer::MemoryTelemetry memory = renderer.GetMemoryTelemetry();
		static constexpr uint64_t PeakTraceStepBytes = 64ull * 1024ull * 1024ull;
		const bool newPeak =
			gVoxelPreloadTimeline.lastLoggedPeakBytes == 0 ||
			memory.totalTrackedBytes >= gVoxelPreloadTimeline.lastLoggedPeakBytes + PeakTraceStepBytes;
		gVoxelPreloadTimeline.peakTrackedBytes = std::max(gVoxelPreloadTimeline.peakTrackedBytes, memory.totalTrackedBytes);
		if ((int)nri_ptloadingtrace >= 1 && (newPeak || std::strcmp(stage, "gate-release") == 0))
		{
			gVoxelPreloadTimeline.lastLoggedPeakBytes = gVoxelPreloadTimeline.peakTrackedBytes;
			Printf("PERF pt voxel preload timeline NRI: stage=%s build_serial=%llu elapsed_ms=%.3f tracked_bytes=%llu peak_tracked_bytes=%llu local_usage_bytes=%llu local_budget_bytes=%llu nonlocal_usage_bytes=%llu nonlocal_budget_bytes=%llu live_usage_available=%u\n",
				stage,
				(unsigned long long)buildSerial,
				DurationMs(gVoxelPreloadTimeline.firstSeen, now),
				(unsigned long long)memory.totalTrackedBytes,
				(unsigned long long)gVoxelPreloadTimeline.peakTrackedBytes,
				(unsigned long long)adapterMemory.localUsageBytes,
				(unsigned long long)adapterMemory.localBudgetBytes,
				(unsigned long long)adapterMemory.nonLocalUsageBytes,
				(unsigned long long)adapterMemory.nonLocalBudgetBytes,
				adapterMemory.liveUsageAvailable ? 1u : 0u);
		}
	}

	void PrintVoxelPreloadClosure(
		const char* levelName,
		const NRIVoxelComputePreloadClosureStats& closure,
		const char* forcedOutcome = nullptr)
	{
		const char* outcome = forcedOutcome != nullptr ? forcedOutcome : closure.outcome;
		const bool final = forcedOutcome != nullptr || !closure.strictRequested || std::strcmp(outcome, "incomplete") != 0;
		Printf("PERF pt voxel preload closure NRI: level=%s build_serial=%llu manifest_hash=0x%llx sequence=%llu final=%u outcome=%s strict=%u dry_run=%u memory_guard_hit=%u selected_bindings=%u admitted_bindings=%u ready_bindings=%u reused_bindings=%u failed=%u cap_skipped=%u stale_cancelled=%u runtime_withheld=%u runtime_withheld_meshes=%u runtime_withheld_ready_meshes=%u runtime_withheld_ready_materials=%u pending=%u unique_sources=%u unique_meshes=%u ready_meshes=%u unique_materials=%u ready_materials=%u unique_textures=%u ready_textures=%u admission_queue=%u compute_inflight=%u blas_inflight=%u cpu_geometry_builds=%llu cpu_geometry_uploads=%llu cpu_geometry_upload_bytes=%llu cpu_fallback=%llu full_geometry_readback_bytes=%llu\n",
			levelName != nullptr ? levelName : "(none)",
			(unsigned long long)closure.buildSerial,
			(unsigned long long)closure.manifestHash,
			(unsigned long long)closure.sequence,
			final ? 1u : 0u,
			outcome,
			closure.strictRequested ? 1u : 0u,
			closure.dryRun ? 1u : 0u,
			closure.memoryGuardHit ? 1u : 0u,
			closure.selectedBindings,
			closure.admittedBindings,
			closure.readyBindings,
			closure.reusedBindings,
			closure.failedBindings,
			closure.capSkippedBindings,
			closure.staleCancelledBindings,
			closure.runtimeWithheldBindings,
			closure.runtimeWithheldUniqueMeshes,
			closure.runtimeWithheldReadyMeshes,
			closure.runtimeWithheldReadyMaterials,
			closure.pendingBindings,
			closure.selectedUniqueSources,
			closure.selectedUniqueMeshes,
			closure.readyUniqueMeshes,
			closure.selectedUniqueMaterials,
			closure.readyUniqueMaterials,
			closure.selectedUniqueTextures,
			closure.readyUniqueTextures,
			closure.admissionQueueCount,
			closure.computeInFlightCount,
			closure.blasInFlightCount,
			(unsigned long long)closure.cpuGeometryBuilds,
			(unsigned long long)closure.cpuGeometryUploads,
			(unsigned long long)closure.cpuGeometryUploadBytes,
			(unsigned long long)closure.cpuGeometryFallback,
			(unsigned long long)closure.fullGeometryReadbackBytes);
	}

	void PrintVoxelPreloadTerminal(
		const char* levelName,
		const NRIVoxelComputePreloadClosureStats& closure,
		const char* outcome,
		const char* result)
	{
		Printf("PERF pt voxel preload terminal NRI: level=%s build_serial=%llu manifest_hash=0x%llx sequence=%llu outcome=%s result=%s selected_bindings=%u ready_bindings=%u reused_bindings=%u failed=%u cap_skipped=%u stale_cancelled=%u pending=%u admission_queue=%u compute_inflight=%u blas_inflight=%u\n",
			levelName != nullptr ? levelName : "(none)",
			(unsigned long long)closure.buildSerial,
			(unsigned long long)closure.manifestHash,
			(unsigned long long)closure.sequence,
			outcome != nullptr ? outcome : closure.outcome,
			result != nullptr ? result : "unknown",
			closure.selectedBindings,
			closure.readyBindings,
			closure.reusedBindings,
			closure.failedBindings,
			closure.capSkippedBindings,
			closure.staleCancelledBindings,
			closure.pendingBindings,
			closure.admissionQueueCount,
			closure.computeInFlightCount,
			closure.blasInFlightCount);
	}

	void QueueStrictPreloadTerminalCommand()
	{
		const FString command((const char*)nri_ptvoxelcomputepreloadterminalcommand);
		if (command.IsEmpty())
		{
			return;
		}

		nri_ptvoxelcomputepreloadterminalcommand = "";
		Printf("PERF pt voxel preload terminal action NRI: result=queued command=\"%s\"\n", command.GetChars());
		AddCommandString(command.GetChars());
	}

	void ArmStrictPreloadFirstFrameReleaseCommand(const char* levelName, const NRIVoxelComputePreloadClosureStats& closure)
	{
		const FString command((const char*)nri_ptvoxelcomputepreloadreleasecommand);
		if (command.IsEmpty() || gVoxelPreloadTimeline.releaseCommandArmedBuildSerial == closure.buildSerial)
		{
			return;
		}

		gVoxelPreloadTimeline.releaseCommandArmedBuildSerial = closure.buildSerial;
		Printf("PERF pt voxel preload release action NRI: result=armed reason=strict-complete level=%s build_serial=%llu manifest_hash=0x%llx sequence=%llu command=\"%s\"\n",
			levelName != nullptr ? levelName : "(none)",
			(unsigned long long)closure.buildSerial,
			(unsigned long long)closure.manifestHash,
			(unsigned long long)closure.sequence,
			command.GetChars());
	}
}

void NRIPreloadCoordinator::QueueStrictPreloadFirstFrameReleaseCommand(NRIRenderer& renderer)
{
	const FString command((const char*)nri_ptvoxelcomputepreloadreleasecommand);
	if (command.IsEmpty())
	{
		return;
	}

	// This is a one-shot diagnostic command. Consume it before any possible
	// queue so a rejected or repeated release cannot leak into another level.
	nri_ptvoxelcomputepreloadreleasecommand = "";

	const uint64_t buildSerial = renderer.mMapWorld.buildSerial;
	const NRIVoxelComputePreloadClosureStats closure = BuildNRIVoxelComputePreloadClosure(
		renderer.mPersistentVoxels,
		buildSerial);
	const NRIPersistentVoxelPreloadStatus preloadStatus =
		renderer.mPersistentVoxels.BuildPreloadStatusSnapshot();
	const NRIPersistentVoxelStatusSnapshot voxelStatus =
		renderer.mPersistentVoxels.BuildStatusSnapshot();

	const char* reason = nullptr;
	if (buildSerial == 0)
	{
		reason = "build-serial-zero";
	}
	else if (gVoxelPreloadTimeline.releaseCommandQueuedBuildSerial == buildSerial)
	{
		reason = "already-queued";
	}
	else if (gVoxelPreloadTimeline.buildSerial != buildSerial)
	{
		reason = "timeline-build-mismatch";
	}
	else if (gVoxelPreloadTimeline.releaseCommandArmedBuildSerial != buildSerial)
	{
		reason = "command-build-mismatch";
	}
	else if (!closure.valid || closure.buildSerial != buildSerial)
	{
		reason = "closure-build-mismatch";
	}
	else if (voxelStatus.residencyBuildSerial != buildSerial)
	{
		reason = "residency-build-mismatch";
	}
	else if (!gVoxelPreloadTimeline.strictTerminalLogged ||
		gVoxelPreloadTimeline.strictAbortLatched ||
		!closure.strictRequested || closure.dryRun || closure.memoryGuardHit ||
		std::strcmp(closure.outcome, "complete") != 0)
	{
		reason = "strict-incomplete";
	}
	else if (!preloadStatus.gpuLoadingEnabled || !preloadStatus.hasCacheEntries)
	{
		reason = "actor-cache-empty";
	}
	else if (!preloadStatus.batchReady || preloadStatus.batchPendingActors != 0 ||
		preloadStatus.deferredTexturePrewarm != 0 || preloadStatus.deferredOnboarding != 0)
	{
		reason = "batch-incomplete";
	}
	else if (preloadStatus.requiredPending != 0 || preloadStatus.optionalPending != 0 ||
		preloadStatus.failed != 0 || voxelStatus.pendingInstanceCount != 0 ||
		voxelStatus.requiredAdmissionPendingCount != 0 ||
		voxelStatus.optionalAdmissionPendingCount != 0 ||
		voxelStatus.failedAdmissionCount != 0 || voxelStatus.computeInFlightCount != 0 ||
		voxelStatus.blasInFlightCount != 0)
	{
		reason = "active-work-pending";
	}
	else if (preloadStatus.batchReadyActors == 0 || voxelStatus.activeInstanceCount == 0)
	{
		reason = "active-batch-empty";
	}
	else if (preloadStatus.batchReadyActors != voxelStatus.activeInstanceCount)
	{
		reason = "active-batch-mismatch";
	}

	const char* levelName = renderer.mMapWorld.level != nullptr ?
		renderer.mMapWorld.level->labelName.GetChars() : "(none)";
	if (reason != nullptr)
	{
		Printf("PERF pt voxel preload release action NRI: result=rejected reason=%s level=%s build_serial=%llu timeline_build_serial=%llu command_build_serial=%llu closure_build_serial=%llu residency_build_serial=%llu manifest_hash=0x%llx sequence=%llu outcome=%s strict=%u terminal_complete=%u selected_bindings=%u ready_bindings=%u reused_bindings=%u closure_failed=%u cap_skipped=%u runtime_withheld=%u closure_pending=%u admission_queue=%u gpu_loading=%u has_cache_entries=%u batch_ready=%u batch_ready_actors=%u batch_pending=%u deferred_texture=%u deferred_onboarding=%u batch_actors=%u active_instances=%u instance_records=%u pending_instances=%u desired_actors=%u required_pending=%u optional_pending=%u failed=%u compute_inflight=%u blas_inflight=%u command=\"%s\"\n",
			reason,
			levelName,
			(unsigned long long)buildSerial,
			(unsigned long long)gVoxelPreloadTimeline.buildSerial,
			(unsigned long long)gVoxelPreloadTimeline.releaseCommandArmedBuildSerial,
			(unsigned long long)closure.buildSerial,
			(unsigned long long)voxelStatus.residencyBuildSerial,
			(unsigned long long)closure.manifestHash,
			(unsigned long long)closure.sequence,
			closure.outcome,
			closure.strictRequested ? 1u : 0u,
			gVoxelPreloadTimeline.strictTerminalLogged ? 1u : 0u,
			closure.selectedBindings,
			closure.readyBindings,
			closure.reusedBindings,
			closure.failedBindings,
			closure.capSkippedBindings,
			closure.runtimeWithheldBindings,
			closure.pendingBindings,
			closure.admissionQueueCount,
			preloadStatus.gpuLoadingEnabled ? 1u : 0u,
			preloadStatus.hasCacheEntries ? 1u : 0u,
			preloadStatus.batchReady ? 1u : 0u,
			preloadStatus.batchReadyActors,
			preloadStatus.batchPendingActors,
			preloadStatus.deferredTexturePrewarm,
			preloadStatus.deferredOnboarding,
			voxelStatus.batchActorCount,
			voxelStatus.activeInstanceCount,
			voxelStatus.instanceRecordCount,
			voxelStatus.pendingInstanceCount,
			voxelStatus.lastDesiredActorCount,
			preloadStatus.requiredPending,
			preloadStatus.optionalPending,
			preloadStatus.failed,
			voxelStatus.computeInFlightCount,
			voxelStatus.blasInFlightCount,
			command.GetChars());
		return;
	}

	gVoxelPreloadTimeline.releaseCommandQueuedBuildSerial = buildSerial;
	Printf("PERF pt voxel preload release action NRI: result=queued reason=complete level=%s build_serial=%llu manifest_hash=0x%llx sequence=%llu outcome=%s strict=1 terminal_complete=1 selected_bindings=%u ready_bindings=%u reused_bindings=%u closure_failed=0 cap_skipped=0 runtime_withheld=0 closure_pending=0 admission_queue=0 batch_ready=1 batch_ready_actors=%u batch_pending=0 batch_actors=%u active_instances=%u instance_records=%u pending_instances=0 desired_actors=%u required_pending=0 optional_pending=0 failed=0 compute_inflight=0 blas_inflight=0 command=\"%s\"\n",
		levelName,
		(unsigned long long)buildSerial,
		(unsigned long long)closure.manifestHash,
		(unsigned long long)closure.sequence,
		closure.outcome,
		closure.selectedBindings,
		closure.readyBindings,
		closure.reusedBindings,
		preloadStatus.batchReadyActors,
		voxelStatus.batchActorCount,
		voxelStatus.activeInstanceCount,
		voxelStatus.instanceRecordCount,
		voxelStatus.lastDesiredActorCount,
		command.GetChars());
	AddCommandString(command.GetChars());
}

bool NRIPreloadCoordinator::HasFrameTarget(NRIRenderer& renderer, const Context& context)
{
	const bool hasCommandBuffer = renderer.mFrameBuffer != nullptr && renderer.mFrameBuffer->HasCurrentCommandBuffer();
	const bool hasRequiredTarget = context.standaloneContextUsed || (renderer.mFrameBuffer != nullptr && renderer.mFrameBuffer->HasActiveTarget());
	if (!hasCommandBuffer || !hasRequiredTarget)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=wait reason=%s framebuffer=%u command_buffer=%u active_target=%u standalone_context=%u output=%ux%u target=%ux%u\n",
				!hasCommandBuffer ? "command-buffer-not-ready" : "frame-target-not-ready",
				renderer.mFrameBuffer != nullptr ? 1u : 0u,
				renderer.mFrameBuffer != nullptr && renderer.mFrameBuffer->HasCurrentCommandBuffer() ? 1u : 0u,
				renderer.mFrameBuffer != nullptr && renderer.mFrameBuffer->HasActiveTarget() ? 1u : 0u,
				context.standaloneContextUsed ? 1u : 0u,
				context.outputWidth,
				context.outputHeight,
				context.targetWidth,
				context.targetHeight);
		}
		return false;
	}
	return true;
}

bool NRIPreloadCoordinator::ShouldSkipForUnsupportedPathTracing(NRIRenderer& renderer, const Context& context)
{
	if (!renderer.RefreshPathTracingAvailability() || !renderer.mPathTracingSupported)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=pt-unsupported output=%ux%u target=%ux%u\n",
				context.outputWidth,
				context.outputHeight,
				context.targetWidth,
				context.targetHeight);
		}
		return true;
	}
	return false;
}

void NRIPreloadCoordinator::TraceBegin(NRIRenderer& renderer, const Context& context)
{
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=renderer-preload result=begin output=%ux%u target=%ux%u map_valid=%u static_valid=%u static_resident=%u\n",
			context.outputWidth,
			context.outputHeight,
			context.targetWidth,
			context.targetHeight,
			renderer.mMapWorld.valid ? 1u : 0u,
			renderer.mStaticMapScene.valid ? 1u : 0u,
			renderer.mStaticMapScene.valid && renderer.mStaticMapScene.texturesResident && renderer.mStaticMapScene.buffersResident && renderer.mStaticMapScene.accelerationResident ? 1u : 0u);
	}
}

bool NRIPreloadCoordinator::EnsureFrameResources(NRIRenderer& renderer, const Context& context)
{
	renderer.ResetPerfTraceStats();
	{
		ScopedPreloadPerfTimer initPerfTimer(renderer.mLastPerfShellTraceStats.initResourcesMs);
		if (!renderer.Initialize() || !NRIFrameResources::EnsureFrameResources(renderer, context.outputWidth, context.outputHeight, context.targetWidth, context.targetHeight))
		{
			renderer.LogFallback("PT preload frame resources or pipelines failed to initialize.");
			if ((int)nri_ptloadingtrace >= 1)
			{
				Printf("NRI PT loading gate: event=renderer-preload result=ready reason=init-failed ms=%.3f\n",
					DurationMs(context.start, std::chrono::steady_clock::now()));
			}
			return false;
		}
	}
	return true;
}

void NRIPreloadCoordinator::ResetSceneStats(NRIRenderer& renderer)
{
	renderer.ResetSceneBufferFrameStats();
	ResetRendererSkyPerfTraceStats();
	nri_scene::ResetAverageTextureColorCache();
	nri_scene::ResetSkyPerfStats();
}

NRIPreloadCoordinator::StepResult NRIPreloadCoordinator::PreloadStaticSceneAndStartupCorrection(NRIRenderer& renderer, const Context& context)
{
	renderer.RefreshMapWorld();
	if (!renderer.mMapWorld.valid)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=map-invalid ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}

	if (!renderer.PreloadStaticMapResources())
	{
		renderer.LogFallback("PT preload resident static scene build failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=static-map-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}

	if (!renderer.ApplyStartupMapWorldCorrectionIfNeeded("renderer-preload"))
	{
		renderer.LogFallback("PT preload startup map-world correction failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=startup-correction-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}
	if (renderer.mAllowStartupMapWorldCorrection)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=wait reason=startup-correction-pending ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Wait;
	}
	if (!renderer.mStaticMapScene.valid ||
		!renderer.mStaticMapScene.texturesResident ||
		!renderer.mStaticMapScene.buffersResident ||
		!renderer.mStaticMapScene.accelerationResident ||
		renderer.mStaticMapScene.buildSerial != renderer.mMapWorld.buildSerial)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=continue reason=startup-correction-rebuild static_valid=%u textures=%u buffers=%u acceleration=%u scene_build_serial=%llu map_build_serial=%llu ms=%.3f\n",
				renderer.mStaticMapScene.valid ? 1u : 0u,
				renderer.mStaticMapScene.texturesResident ? 1u : 0u,
				renderer.mStaticMapScene.buffersResident ? 1u : 0u,
				renderer.mStaticMapScene.accelerationResident ? 1u : 0u,
				(unsigned long long)renderer.mStaticMapScene.buildSerial,
				(unsigned long long)renderer.mMapWorld.buildSerial,
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		if (!renderer.PreloadStaticMapResources())
		{
			renderer.LogFallback("PT preload corrected resident static scene build failed.");
			if ((int)nri_ptloadingtrace >= 1)
			{
				Printf("NRI PT loading gate: event=renderer-preload result=ready reason=startup-correction-static-map-failed ms=%.3f\n",
					DurationMs(context.start, std::chrono::steady_clock::now()));
			}
			return StepResult::Ready;
		}
	}
	return StepResult::Continue;
}

void NRIPreloadCoordinator::RefreshStaticLighting(NRIRenderer& renderer, Context& context)
{
	renderer.RefreshSceneLightSystem(true, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr);
	if (renderer.mGpuSceneHasDynamicOverlay)
	{
		return;
	}

	const bool needsResidentStaticLightRefresh =
		!renderer.mSceneLights.GetAnalyticLights().activeLights.empty() ||
		renderer.mBoundRuntimeLightCount != 0 ||
		renderer.mSceneLights.GetSectorLighting().activeSectorCount > 0 ||
		renderer.mBoundSectorLightActiveCount != 0;
	if (needsResidentStaticLightRefresh && !renderer.RefreshResidentStaticSceneDataSet())
	{
		context.staticLightRefreshReady = false;
		renderer.LogFallback("PT preload static scene light refresh failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=continue reason=static-light-refresh-failed analytic=%u runtime_bound=%u sector_active=%u sector_bound=%u ms=%.3f\n",
				renderer.mSceneLights.GetAnalyticLights().activeLights.empty() ? 0u : 1u,
				renderer.mBoundRuntimeLightCount,
				renderer.mSceneLights.GetSectorLighting().activeSectorCount,
				renderer.mBoundSectorLightActiveCount,
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
	}
}

NRIPreloadCoordinator::StepResult NRIPreloadCoordinator::PreloadResidentSceneResources(NRIRenderer& renderer, const Context& context)
{
	if (!renderer.PreloadPersistentVoxelResources())
	{
		if (renderer.mPersistentVoxels.HasPreloadPending())
		{
			if ((int)nri_ptloadingtrace >= 1)
			{
				uint32_t requiredPending = 0;
				uint32_t requiredReady = 0;
				uint32_t optionalPending = 0;
				uint32_t failed = 0;
				renderer.mPersistentVoxels.CountAdmissionWork(requiredPending, requiredReady, optionalPending, failed);
				Printf("NRI PT loading gate: event=renderer-preload result=wait reason=persistent-voxel-pending required_pending=%u required_ready=%u optional_pending=%u failed=%u ms=%.3f\n",
					requiredPending,
					requiredReady,
					optionalPending,
					failed,
					DurationMs(context.start, std::chrono::steady_clock::now()));
			}
			return StepResult::Wait;
		}
		renderer.LogFallback("PT preload persistent voxel resource admission failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=persistent-voxel-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}
	if (!renderer.PreloadMaterialResources())
	{
		if (renderer.HasMaterialPreloadPending())
		{
			const NRIRenderer::PreloadMaterialStatus& materialStatus = renderer.GetPreloadMaterialStatus();
			if ((int)nri_ptloadingtrace >= 1)
			{
				Printf("NRI PT loading gate: event=renderer-preload result=wait reason=material-pending static_pending=%u voxel_pending=%u submit_budget_hit=%u ms_budget_hit=%u ms=%.3f\n",
					materialStatus.staticTexturesPending,
					materialStatus.voxelTexturesPending,
					materialStatus.submitBudgetHit ? 1u : 0u,
					materialStatus.msBudgetHit ? 1u : 0u,
					DurationMs(context.start, std::chrono::steady_clock::now()));
			}
			return StepResult::Wait;
		}
		renderer.LogFallback("PT preload material warmup failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=material-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}

	NRIRenderer::EmissiveSamplingBuildContext emissiveSamplingContext = {};
	emissiveSamplingContext.staticGeometry = &renderer.mStaticMapScene.geometry;
	const NRIPersistentVoxelOverlayStats persistentVoxelStats = renderer.mPersistentVoxels.BuildOverlayStats();
	auto deferStandaloneEmissiveTlas = [&](const char* reason) -> StepResult
	{
		renderer.mEmissiveTlasInstanceCount = 0;
		renderer.mEmissiveTlasStaticInstanceCount = 0;
		renderer.mEmissiveTlasDynamicInstanceCount = 0;
		renderer.mEmissiveTlasInstancePayloadCacheValid = false;
		renderer.mEmissiveTlasInstancePayloadHash = 0;
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=emissive-tlas result=defer reason=%s ms=%.3f\n",
				reason,
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		if (!renderer.PreGrowLevelSceneResourcesForLoading())
		{
			renderer.LogFallback("PT preload scene resource pre-grow failed.");
			if ((int)nri_ptloadingtrace >= 1)
			{
				Printf("NRI PT loading gate: event=renderer-preload result=ready reason=pre-grow-failed ms=%.3f\n",
					DurationMs(context.start, std::chrono::steady_clock::now()));
			}
			return StepResult::Ready;
		}
		return StepResult::Continue;
	};
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=emissive-sampling result=begin standalone_context=%u surfaces=%u persistent_actors=%u persistent_prims=%u ms=%.3f\n",
			context.standaloneContextUsed ? 1u : 0u,
			(uint32_t)renderer.mSceneLights.GetEmissiveSurfaces().activeSurfaces.size(),
			persistentVoxelStats.actorCount,
			persistentVoxelStats.primitiveCount,
			DurationMs(context.start, std::chrono::steady_clock::now()));
	}
	if (context.standaloneContextUsed && persistentVoxelStats.actorCount > 0)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=emissive-sampling result=defer reason=standalone-persistent-voxel-overlay persistent_actors=%u persistent_prims=%u ms=%.3f\n",
				persistentVoxelStats.actorCount,
				persistentVoxelStats.primitiveCount,
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return deferStandaloneEmissiveTlas("standalone-persistent-voxel-overlay");
	}
	if (!renderer.UpdateEmissiveSamplingBuffers(emissiveSamplingContext))
	{
		renderer.LogFallback("PT preload emissive primitive update failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=emissive-sampling-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=emissive-sampling result=ready primitives=%u ms=%.3f\n",
			renderer.mBoundEmissivePrimitiveCount,
			DurationMs(context.start, std::chrono::steady_clock::now()));
	}
	if (context.standaloneContextUsed)
	{
		return deferStandaloneEmissiveTlas("standalone-preload");
	}
	if (!renderer.BuildEmissiveTopLevelAccelerationStructure())
	{
		renderer.LogFallback("PT preload emissive TLAS update failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=emissive-tlas-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}
	if (!renderer.PreGrowLevelSceneResourcesForLoading())
	{
		renderer.LogFallback("PT preload scene resource pre-grow failed.");
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=ready reason=pre-grow-failed ms=%.3f\n",
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return StepResult::Ready;
	}
	return StepResult::Continue;
}

bool NRIPreloadCoordinator::Finish(NRIRenderer& renderer, const Context& context)
{
	renderer.PrepareSceneTextureInputsForCompute();
	const NRIVoxelComputePreloadClosureStats preliminaryClosure = BuildNRIVoxelComputePreloadClosure(
		renderer.mPersistentVoxels,
		renderer.mMapWorld.buildSerial);
	if (preliminaryClosure.valid && preliminaryClosure.strictRequested &&
		std::strcmp(preliminaryClosure.outcome, "complete") == 0 &&
		!renderer.PumpPersistentVoxelBlasCompaction(renderer.mMapWorld.buildSerial))
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("NRI PT loading gate: event=renderer-preload result=wait reason=voxel-blas-compaction build_serial=%llu ms=%.3f\n",
				(unsigned long long)renderer.mMapWorld.buildSerial,
				DurationMs(context.start, std::chrono::steady_clock::now()));
		}
		return false;
	}
	const bool staticReady =
		renderer.mStaticMapScene.valid &&
		renderer.mStaticMapScene.texturesResident &&
		renderer.mStaticMapScene.buffersResident &&
		renderer.mStaticMapScene.accelerationResident &&
		(!renderer.mMapWorld.valid || renderer.mStaticMapScene.buildSerial == renderer.mMapWorld.buildSerial);
	const NRIPersistentVoxelPreloadStatus voxelStatus = renderer.mPersistentVoxels.BuildPreloadStatusSnapshot();
	const NRIPersistentVoxelStatusSnapshot voxelSnapshot = renderer.mPersistentVoxels.BuildStatusSnapshot();
	const NRIPersistentVoxelMemoryUsage voxelMemory = renderer.mPersistentVoxels.GetMemoryUsage();
	const NRIVoxelComputeMemoryUsage computeMemory = GetNRIVoxelComputeMemoryUsage();
	const NRIRenderer::MemoryTelemetry rendererMemory = renderer.GetMemoryTelemetry();
	const NRIRenderer::PreloadMaterialStatus& materialStatus = renderer.GetPreloadMaterialStatus();
	const double preloadMs = DurationMs(context.start, std::chrono::steady_clock::now());
	const NRIAdapterMemoryTelemetry adapterMemory = renderer.mFrameBuffer != nullptr ? renderer.mFrameBuffer->GetAdapterMemoryTelemetry() : NRIAdapterMemoryTelemetry{};
	const uint64_t localBudgetBytes = adapterMemory.localBudgetBytes;
	UpdateVoxelPreloadTimeline(renderer, renderer.mMapWorld.buildSerial, adapterMemory, "gate-release");
	const NRIVoxelComputePreloadClosureStats closure = BuildNRIVoxelComputePreloadClosure(
		renderer.mPersistentVoxels,
		renderer.mMapWorld.buildSerial);
	if (closure.valid)
	{
		if ((int)nri_ptloadingtrace >= 1)
		{
			Printf("PERF pt voxel preload memory NRI: level=%s build_serial=%llu pv_scene_bytes=%llu pv_as_bytes=%llu arena_vertex_committed=%llu arena_index_committed=%llu arena_primitive_committed=%llu arena_material_committed=%llu arena_vertex_used=%llu arena_index_used=%llu arena_primitive_used=%llu arena_material_used=%llu private_vertex_bytes=%llu private_index_bytes=%llu direct_blas_bytes=%llu shared_blas_bytes=%llu material_logical_bytes=%llu admission_transient_buffer_bytes=%llu admission_transient_as_bytes=%llu admission_cpu_geometry_bytes=%llu raw_sources=%u raw_uploaded=%u raw_cpu_bytes=%llu raw_device_bytes=%llu raw_upload_bytes=%llu compute_input_device_bytes=%llu compute_input_upload_bytes=%llu compute_generated_bytes=%llu status_readback_buffer_bytes=%llu geometry_readback_buffer_bytes=%llu diagnostic_as_bytes=%llu renderer_tracked_bytes=%llu renderer_scene_texture_bytes=%llu renderer_buffer_bytes=%llu renderer_as_bytes=%llu local_usage_bytes=%llu local_budget_bytes=%llu nonlocal_usage_bytes=%llu nonlocal_budget_bytes=%llu live_usage_available=%u peak_tracked_bytes=%llu retired_voxel_attribution_available=0 texture_voxel_attribution_available=0\n",
			renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : "(none)",
			(unsigned long long)renderer.mMapWorld.buildSerial,
			(unsigned long long)voxelMemory.sceneBufferBytes,
			(unsigned long long)voxelMemory.accelerationStructureBytes,
			(unsigned long long)voxelMemory.arenaVertexCommittedBytes,
			(unsigned long long)voxelMemory.arenaIndexCommittedBytes,
			(unsigned long long)voxelMemory.arenaPrimitiveCommittedBytes,
			(unsigned long long)voxelMemory.arenaMaterialCommittedBytes,
			(unsigned long long)voxelMemory.arenaVertexUsedBytes,
			(unsigned long long)voxelMemory.arenaIndexUsedBytes,
			(unsigned long long)voxelMemory.arenaPrimitiveUsedBytes,
			(unsigned long long)voxelMemory.arenaMaterialUsedBytes,
			(unsigned long long)voxelMemory.privateVertexBytes,
			(unsigned long long)voxelMemory.privateIndexBytes,
			(unsigned long long)voxelMemory.directBlasBytes,
			(unsigned long long)voxelMemory.sharedBlasBytes,
			(unsigned long long)voxelMemory.materialLogicalBytes,
			(unsigned long long)voxelMemory.admissionTransientBufferBytes,
			(unsigned long long)voxelMemory.admissionTransientAsBytes,
			(unsigned long long)voxelMemory.admissionCpuGeometryBytes,
			computeMemory.rawSourceCount,
			computeMemory.rawSourceUploadedCount,
			(unsigned long long)computeMemory.rawCpuBytes,
			(unsigned long long)computeMemory.rawDeviceBytes,
			(unsigned long long)computeMemory.rawUploadBytes,
			(unsigned long long)computeMemory.transientInputDeviceBytes,
			(unsigned long long)computeMemory.transientInputUploadBytes,
			(unsigned long long)computeMemory.transientGeneratedBytes,
			(unsigned long long)computeMemory.statusReadbackBytes,
			(unsigned long long)computeMemory.geometryReadbackBufferBytes,
			(unsigned long long)computeMemory.diagnosticAsBytes,
			(unsigned long long)rendererMemory.totalTrackedBytes,
			(unsigned long long)rendererMemory.sceneTextureBytes,
			(unsigned long long)rendererMemory.sceneBufferBytes,
			(unsigned long long)rendererMemory.accelerationStructureBytes,
			(unsigned long long)adapterMemory.localUsageBytes,
			(unsigned long long)localBudgetBytes,
			(unsigned long long)adapterMemory.nonLocalUsageBytes,
			(unsigned long long)adapterMemory.nonLocalBudgetBytes,
			adapterMemory.liveUsageAvailable ? 1u : 0u,
			(unsigned long long)gVoxelPreloadTimeline.peakTrackedBytes);
			PrintVoxelPreloadClosure(
				renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : nullptr,
				closure);
		}
		if (closure.strictRequested && std::strcmp(closure.outcome, "incomplete") == 0)
		{
			if ((int)nri_ptloadingtrace >= 1)
			{
				Printf("NRI PT loading gate: event=renderer-preload result=wait reason=strict-voxel-incomplete sequence=%llu pending=%u admission_queue=%u compute_inflight=%u blas_inflight=%u ms=%.3f\n",
					(unsigned long long)closure.sequence,
					closure.pendingBindings,
					closure.admissionQueueCount,
					closure.computeInFlightCount,
					closure.blasInFlightCount,
					preloadMs);
			}
			return false;
		}
		if (closure.strictRequested && std::strcmp(closure.outcome, "complete") != 0)
		{
			gVoxelPreloadTimeline.strictAbortLatched = true;
			if (!gVoxelPreloadTimeline.strictTerminalLogged)
			{
				PrintVoxelPreloadTerminal(
					renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : nullptr,
					closure,
					closure.outcome,
					"aborted");
				gVoxelPreloadTimeline.strictTerminalLogged = true;
			}
			return false;
		}
	}
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading summary: static_ready=%u startup_correction_pending=%u required_voxel_pending=%u required_voxel_ready=%u optional_voxel_pending=%u voxel_batch_ready=%u voxel_batch_pending=%u deferred_texture_prewarm=%u deferred_onboarding=%u material_pending=%u material_static_ready=%u material_static_pending=%u material_static_realized=%u material_static_bytes=%llu material_voxel_ready=%u material_voxel_pending=%u material_voxel_realized=%u material_voxel_bytes=%llu preload_submits=%u preload_submit_limit=%u submit_budget_hit=%u ms_budget_hit=%u frame_target_used=%u standalone_context_used=%u gpu_voxel_loading=%u static_light_refresh=%u\n",
		staticReady ? 1u : 0u,
		renderer.mAllowStartupMapWorldCorrection ? 1u : 0u,
		voxelStatus.requiredPending,
		voxelStatus.requiredReady,
		voxelStatus.optionalPending,
		voxelStatus.batchReady ? 1u : 0u,
		voxelStatus.batchPendingActors,
		voxelStatus.deferredTexturePrewarm,
		voxelStatus.deferredOnboarding,
		materialStatus.pending ? 1u : 0u,
		materialStatus.staticReady ? 1u : 0u,
		materialStatus.staticTexturesPending,
		materialStatus.staticTexturesRealized,
		(unsigned long long)materialStatus.staticUploadBytes,
		materialStatus.voxelReady ? 1u : 0u,
		materialStatus.voxelTexturesPending,
		materialStatus.voxelTexturesRealized,
		(unsigned long long)materialStatus.voxelUploadBytes,
		materialStatus.preloadSubmits,
		materialStatus.preloadSubmitLimit,
		materialStatus.submitBudgetHit ? 1u : 0u,
		materialStatus.msBudgetHit ? 1u : 0u,
		context.frameTargetUsed ? 1u : 0u,
		context.standaloneContextUsed ? 1u : 0u,
		voxelStatus.gpuLoadingEnabled ? 1u : 0u,
			context.staticLightRefreshReady ? 1u : 0u);
		Printf("PERF pt preload ready NRI: level=%s build_serial=%llu preload_ms=%.3f static_chunks=%u static_tris=%u static_materials=%u required_voxel_pending=%u required_voxel_ready=%u optional_voxel_pending=%u voxel_batch_ready=%u voxel_batch_pending=%u mesh_resources=%u material_resources=%u admission_queue=%u active_instances=%u pv_scene_bytes=%llu pv_as_bytes=%llu pv_total_bytes=%llu material_static_bytes=%llu material_voxel_bytes=%llu preload_submits=%u submit_budget_hit=%u ms_budget_hit=%u gpu_voxel_loading=%u\n",
		renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)renderer.mMapWorld.buildSerial,
		preloadMs,
		(uint32_t)renderer.mStaticMapScene.chunks.size(),
		(uint32_t)renderer.mStaticMapScene.geometry.primitives.size(),
		(uint32_t)renderer.mStaticMapScene.gpuMaterials.size(),
		voxelStatus.requiredPending,
		voxelStatus.requiredReady,
		voxelStatus.optionalPending,
		voxelStatus.batchReady ? 1u : 0u,
		voxelStatus.batchPendingActors,
		voxelSnapshot.meshVariantResourceCount,
		voxelSnapshot.materialVariantResourceCount,
		voxelSnapshot.admissionQueueCount,
		voxelSnapshot.activeInstanceCount,
		(unsigned long long)voxelMemory.sceneBufferBytes,
		(unsigned long long)voxelMemory.accelerationStructureBytes,
		(unsigned long long)(voxelMemory.sceneBufferBytes + voxelMemory.accelerationStructureBytes),
		(unsigned long long)materialStatus.staticUploadBytes,
		(unsigned long long)materialStatus.voxelUploadBytes,
		materialStatus.preloadSubmits,
		materialStatus.submitBudgetHit ? 1u : 0u,
		materialStatus.msBudgetHit ? 1u : 0u,
			voxelStatus.gpuLoadingEnabled ? 1u : 0u);
		Printf("NRI PT preload ready: level=%s build_serial=%llu chunks=%u tris=%u materials=%u\n",
			renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : "(none)",
			(unsigned long long)renderer.mMapWorld.buildSerial,
			(uint32_t)renderer.mStaticMapScene.chunks.size(),
			(uint32_t)renderer.mStaticMapScene.geometry.primitives.size(),
			(uint32_t)renderer.mStaticMapScene.gpuMaterials.size());
	}
	{
		NRIPersistentVoxelSettings settings = BuildNRIPersistentVoxelSettingsFromCVars();
		settings.admissionGraceFrames = std::max(settings.admissionGraceFrames, settings.preloadReadyGraceFrames);
		renderer.mPersistentVoxels.ArmPostLoadAdmissionGrace(
			renderer.mFrameIndex,
			settings,
			(int)nri_ptloadingtrace);
	}
	if (closure.valid && closure.strictRequested && !gVoxelPreloadTimeline.strictTerminalLogged)
	{
		ArmStrictPreloadFirstFrameReleaseCommand(
			renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : nullptr,
			closure);
		PrintVoxelPreloadTerminal(
			renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : nullptr,
			closure,
			"complete",
			"complete");
		gVoxelPreloadTimeline.strictTerminalLogged = true;
		QueueStrictPreloadTerminalCommand();
	}
	if ((int)nri_ptloadingtrace >= 1)
	{
		Printf("NRI PT loading gate: event=renderer-preload result=ready reason=complete static_light_refresh=%u ms=%.3f\n",
			context.staticLightRefreshReady ? 1u : 0u,
			preloadMs);
	}
	return true;
}

bool NRIPreloadCoordinator::Run(NRIRenderer& renderer, const NRIPreloadLevelSceneInputs& inputs)
{
	Context context = {};
	context.outputWidth = inputs.outputWidth;
	context.outputHeight = inputs.outputHeight;
	context.targetWidth = inputs.targetWidth;
	context.targetHeight = inputs.targetHeight;
	context.frameTargetUsed = inputs.frameTargetUsed;
	context.standaloneContextUsed = inputs.standaloneContextUsed;
	context.start = std::chrono::steady_clock::now();

	if (!HasFrameTarget(renderer, context))
	{
		return false;
	}
	if (ShouldSkipForUnsupportedPathTracing(renderer, context))
	{
		return true;
	}

	TraceBegin(renderer, context);
	if (!EnsureFrameResources(renderer, context))
	{
		return true;
	}
	ResetSceneStats(renderer);

	const StepResult staticSceneResult = PreloadStaticSceneAndStartupCorrection(renderer, context);
	if (staticSceneResult != StepResult::Continue)
	{
		return staticSceneResult != StepResult::Wait;
	}
	const NRIVoxelComputePreloadSettings computePreloadSettings = BuildNRIVoxelComputePreloadSettingsFromCVars();
	NRIAdapterMemoryTelemetry adapterMemory = {};
	if (renderer.mFrameBuffer != nullptr)
	{
		adapterMemory.localBudgetBytes = renderer.mFrameBuffer->GetAdapterLocalBudgetBytes();
		if (computePreloadSettings.strict)
		{
			adapterMemory = renderer.mFrameBuffer->GetAdapterMemoryTelemetry();
		}
	}
	UpdateVoxelPreloadTimeline(renderer, renderer.mMapWorld.buildSerial, adapterMemory, "preload-tick");
	if (computePreloadSettings.strict && gVoxelPreloadTimeline.strictAbortLatched)
	{
		return false;
	}
	if (computePreloadSettings.strict &&
		computePreloadSettings.watchdogMilliseconds != 0 &&
		DurationMs(gVoxelPreloadTimeline.firstSeen, std::chrono::steady_clock::now()) >= computePreloadSettings.watchdogMilliseconds)
	{
		const NRIVoxelComputePreloadClosureStats closure = BuildNRIVoxelComputePreloadClosure(
			renderer.mPersistentVoxels,
			renderer.mMapWorld.buildSerial);
		if (closure.valid)
		{
			PrintVoxelPreloadClosure(
				renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : nullptr,
				closure,
				"watchdog");
			gVoxelPreloadTimeline.strictAbortLatched = true;
			if (!gVoxelPreloadTimeline.strictTerminalLogged)
			{
				PrintVoxelPreloadTerminal(
					renderer.mMapWorld.level != nullptr ? renderer.mMapWorld.level->labelName.GetChars() : nullptr,
					closure,
					"watchdog",
					"aborted");
				gVoxelPreloadTimeline.strictTerminalLogged = true;
			}
			Printf("NRI PT loading gate: event=renderer-preload result=aborted reason=strict-watchdog elapsed_ms=%.3f watchdog_ms=%u\n",
				DurationMs(gVoxelPreloadTimeline.firstSeen, std::chrono::steady_clock::now()),
				computePreloadSettings.watchdogMilliseconds);
			return false;
		}
	}

	if ((bool)nri_ptloadingmutationbaseline &&
		!renderer.mAllowStartupMutationRebaseline &&
		!renderer.mPendingStartupMutationRebaseline)
	{
		renderer.mAllowStartupMutationRebaseline = true;
		renderer.mStartupMutationRebaselineDeadlineFrame = renderer.mFrameIndex + 64u;
		renderer.TraceStartupMutationProbe("arm");
	}
	const bool traceStartupMutationConsume = renderer.mPendingStartupMutationRebaseline || (int)nri_ptloadingtrace >= 2;
	if (traceStartupMutationConsume)
	{
		renderer.TraceStartupMutationProbe("consume-before");
	}
	renderer.RebuildStartupMutationBaseline();
	if (traceStartupMutationConsume)
	{
		renderer.TraceStartupMutationProbe("consume-after");
	}
	RefreshStaticLighting(renderer, context);
	const StepResult resourcesResult = PreloadResidentSceneResources(renderer, context);
	if (resourcesResult != StepResult::Continue)
	{
		return resourcesResult != StepResult::Wait;
	}

	return Finish(renderer, context);
}
