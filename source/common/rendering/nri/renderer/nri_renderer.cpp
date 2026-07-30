#include "nri_renderer.h"
#include "nri_smoke.h"
#include "nri_cvars.h"

#include "../framegen/nri_framegen.h"
#include "nri_acceleration.h"
#include "nri_descriptor_sets.h"
#include "nri_diagnostic_names.h"
#include "nri_frame_graph.h"
#include "nri_material_policy.h"
#include "nri_pipeline_state.h"
#include "nri_preload_coordinator.h"
#include "nri_persistent_voxel_services.h"
#include "nri_renderstate.h"
#include "nri_render_geometry_helpers.h"
#include "nri_renderer_settings.h"
#include "nri_scene_frame_builder.h"
#include "nri_shader_contracts.h"
#include "nri_static_scene_geometry.h"
#include "nri_upload_hash.h"
#include "nri_voxel_compute_meshing.h"
#include "nri_voxel_compute_preload.h"
#include "nri_runtime_mutation_shared.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_map_builder.h"
#include "../scene/nri_scene_math.h"
#include "../scene/nri_scene_stats.h"
#include "../system/nri_hwtexture.h"
#include "../system/nri_renderdevice.h"
#include "skyboxtexture.h"
#include "image.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "hw_voxels.h"
#include "gamecontrol.h"
#include "gamestate.h"
#include "menustate.h"
#include "hw_sections.h"
#include "lightoverlay.h"
#include "mapinfo.h"
#include "printf.h"
#include "gamestruct.h"
#include "hw_portal.h"
#include "texinfo.h"
#include "texturemanager.h"
#include "d_eventbase.h"
#include "perf_capture.h"
#include "v_video.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

namespace
{
	static constexpr double BuildTickSeconds = 1.0 / 120.0;

	static const char* GetNightVisionModeName(NRIPTNightVisionMode mode)
	{
		switch (mode)
		{
		case NRIPTNightVisionMode::Duke: return "duke";
		default: return "none";
		}
	}

	static uint32_t PackNightVisionControls(float contrast, float saturation)
	{
		const uint32_t contrastBits = (uint32_t)std::lround(std::clamp(contrast, 0.0f, 2.0f) * (65535.0f / 2.0f));
		const uint32_t saturationBits = (uint32_t)std::lround(std::clamp(saturation, 0.0f, 2.0f) * (65535.0f / 2.0f));
		return contrastBits | (saturationBits << 16);
	}

	static uint32_t PackNightVisionModeAndTint(NRIPTNightVisionMode mode, float red, float green, float blue)
	{
		const uint32_t redBits = (uint32_t)std::lround(std::clamp(red, 0.0f, 2.0f) * (255.0f / 2.0f));
		const uint32_t greenBits = (uint32_t)std::lround(std::clamp(green, 0.0f, 2.0f) * (255.0f / 2.0f));
		const uint32_t blueBits = (uint32_t)std::lround(std::clamp(blue, 0.0f, 2.0f) * (255.0f / 2.0f));
		return (uint32_t)mode | (redBits << 8) | (greenBits << 16) | (blueBits << 24);
	}

	static uint64_t HashCombineLightOverlay(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	static uint64_t QuantizeLightOverlayPositionKey(const float position[3])
	{
		const int64_t x = (int64_t)std::llround(position[0] * 16.0f);
		const int64_t y = (int64_t)std::llround(position[1] * 16.0f);
		const int64_t z = (int64_t)std::llround(position[2] * 16.0f);
		uint64_t key = 1469598103934665603ull;
		key = HashCombineLightOverlay(key, (uint64_t)x);
		key = HashCombineLightOverlay(key, (uint64_t)y);
		key = HashCombineLightOverlay(key, (uint64_t)z);
		return key;
	}

	static uint32_t GetGameplayLightTimeIndex()
	{
		return PlayClock > 0 ? (uint32_t)(PlayClock / 4) : 0u;
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

	static uint64_t HashUploadPayloadBytes(const void* data, uint64_t size)
	{
		return NRIHashUploadPayloadBytes(data, size);
	}
	static void FilterMaterialOnlyReplacementSceneView(nri_scene::SceneView& sceneView, uint32_t reasonMask)
	{
		static constexpr float kMaterialOnlyReplacementDepthNudge = 0.01f;
		const bool keepWalls = (reasonMask & nri_scene::PTMapChunkMutationReason_WallMaterial) != 0;
		const bool keepFlats = (reasonMask & nri_scene::PTMapChunkMutationReason_SectorMaterial) != 0;

		if (!keepWalls)
		{
			sceneView.opaqueWalls.clear();
		}

		if (!keepFlats)
		{
			sceneView.opaqueFlats.clear();
		}

		for (nri_scene::SurfaceRef& surface : sceneView.opaqueWalls)
		{
			NudgeCapturedSurface(surface, kMaterialOnlyReplacementDepthNudge);
		}

		for (nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			NudgeCapturedSurface(surface, kMaterialOnlyReplacementDepthNudge);
		}
	}

	static bool SceneViewHasSectorDrivenWallBands(const nri_scene::SceneView& sceneView)
	{
		for (const nri_scene::SurfaceRef& surface : sceneView.opaqueWalls)
		{
			if (surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapWallBand)
			{
				return true;
			}
		}

		return false;
	}

	static float ClampDirectionalAngularSize(float angularSize)
	{
		if (!std::isfinite(angularSize))
		{
			return 0.03f;
		}

		return std::clamp(angularSize, 0.001f, 1.2f);
	}

	static uint32_t PackDirectionalLightColor24(const float color[3])
	{
		auto packChannel = [](float value) -> uint32_t
		{
			const float clamped = std::clamp(value, 0.0f, 8.0f);
			return (uint32_t)std::clamp((int)std::lround((double)(clamped * (255.0f / 8.0f))), 0, 255);
		};

		const uint32_t r = packChannel(color[0]);
		const uint32_t g = packChannel(color[1]);
		const uint32_t b = packChannel(color[2]);
		return r | (g << 8u) | (b << 16u);
	}

	static uint32_t PackDirectionalAngularSize16(float angularSize)
	{
		const float normalized = ClampDirectionalAngularSize(angularSize) / 1.2f;
		return (uint32_t)std::clamp((int)std::lround((double)(normalized * 65535.0f)), 0, 65535);
	}

	static const char* GetDirectionalLightSourceName(const NRIDirectionalLightState& state)
	{
		if (!state.enabled)
		{
			return "off";
		}

		return state.fromOverlay ? "overlay" : "default";
	}

}





namespace
{
	constexpr uint32_t NRI_TRACE_SHADER_STATS_COUNTER_COUNT = NRIRenderer::TraceShaderStatCount;
	constexpr uint32_t NRI_MAX_EMISSIVE_SURFACES = 4096;

	struct NriSceneTextureLimitValidation
	{
		uint32_t requiredSceneTextureCap = NRI_MAX_SCENE_TEXTURES;
		uint32_t requiredSceneTextureSetDescriptors = NRI_SCENE_DESCRIPTOR_NUM;
		uint32_t requiredStageTextureDescriptors = NRI_SCENE_DESCRIPTOR_NUM + NRI_INPUT_DESCRIPTOR_NUM;
		bool descriptorSetTextureLimitOk = false;
		bool descriptorSetUpdateAfterSetTextureLimitOk = false;
		bool shaderStageTextureLimitOk = false;
		bool shaderStageUpdateAfterSetTextureLimitOk = false;
	};

	static NriSceneTextureLimitValidation ValidateSceneTextureDescriptorLimits(const nri::DeviceDesc& deviceDesc)
	{
		NriSceneTextureLimitValidation validation = {};
		validation.descriptorSetTextureLimitOk =
			deviceDesc.descriptorSet.textureMaxNum >= validation.requiredSceneTextureSetDescriptors;
		validation.descriptorSetUpdateAfterSetTextureLimitOk =
			deviceDesc.descriptorSet.updateAfterSet.textureMaxNum >= validation.requiredSceneTextureSetDescriptors;
		validation.shaderStageTextureLimitOk =
			deviceDesc.shaderStage.descriptorTextureMaxNum >= validation.requiredStageTextureDescriptors;
		validation.shaderStageUpdateAfterSetTextureLimitOk =
			deviceDesc.shaderStage.updateAfterSet.descriptorTextureMaxNum >= validation.requiredStageTextureDescriptors;
		return validation;
	}

	static const char* GetSceneTextureDescriptorLimitFailureReason(const nri::DeviceDesc& deviceDesc)
	{
		const NriSceneTextureLimitValidation validation = ValidateSceneTextureDescriptorLimits(deviceDesc);
		if (!validation.descriptorSetTextureLimitOk)
		{
			return "descriptor-set texture limit is below the NRI PT 1024-scene-texture requirement";
		}
		if (!validation.descriptorSetUpdateAfterSetTextureLimitOk)
		{
			return "update-after-set texture limit is below the NRI PT 1024-scene-texture requirement";
		}
		if (!validation.shaderStageTextureLimitOk)
		{
			return "per-stage texture descriptor limit is below the NRI PT 1024-scene-texture requirement";
		}
		if (!validation.shaderStageUpdateAfterSetTextureLimitOk)
		{
			return "per-stage update-after-set texture descriptor limit is below the NRI PT 1024-scene-texture requirement";
		}
		return nullptr;
	}

	static void LogSceneTextureDescriptorLimits(const nri::DeviceDesc& deviceDesc)
	{
		const NriSceneTextureLimitValidation validation = ValidateSceneTextureDescriptorLimits(deviceDesc);
		Printf(
			"NRI PT scene texture cap: cap=%u scene_set=%u stage_textures=%u limits=set:%u set_uas:%u stage:%u stage_uas:%u supported=%s\n",
			validation.requiredSceneTextureCap,
			validation.requiredSceneTextureSetDescriptors,
			validation.requiredStageTextureDescriptors,
			deviceDesc.descriptorSet.textureMaxNum,
			deviceDesc.descriptorSet.updateAfterSet.textureMaxNum,
			deviceDesc.shaderStage.descriptorTextureMaxNum,
			deviceDesc.shaderStage.updateAfterSet.descriptorTextureMaxNum,
			GetSceneTextureDescriptorLimitFailureReason(deviceDesc) == nullptr ? "yes" : "no");
	}
	constexpr int NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT = 8;
	constexpr float NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS = 0.5f;
	constexpr uint32_t NRI_SECTOR_LIGHTING_FLAG_ENABLED = 0x1u;


	bool ShouldTracePtPerf()
	{
		return PerfLoopTraceActive() || ShouldEmitRendererTemporalTraceLogs();
	}

	bool ShouldCollectPtPerfTiming()
	{
		return ShouldTracePtPerf() || (bool)nri_ptslowdowntrace || PerfCompactCaptureTimingActive();
	}

	bool ShouldCollectTraceShaderStats()
	{
		return !!nri_ptshaderstats && ShouldTracePtPerf();
	}

	uint32_t ScoreRuntimeSectorDirtyTruthEntry(const NRIRenderer::RuntimeSectorDirtyTruthTraceEntry& entry)
	{
		uint32_t score = 0;
		if (entry.forceTopology)
		{
			score += 1u << 18;
		}
		if (entry.baselineChanged)
		{
			score += 1u << 17;
		}
		if (entry.geometryChanged)
		{
			score += 1u << 16;
		}
		if (entry.materialChanged)
		{
			score += 1u << 15;
		}
		score += entry.liveTriangleCount * 8u;
		score += entry.liveSurfaceCount * 4u;
		return score;
	}

	uint32_t ScoreRuntimeAnimatedChurnTraceEntry(const NRIRenderer::RuntimeAnimatedChurnTraceEntry& entry)
	{
		uint32_t score = entry.materialRefreshes * 96u +
			entry.runtimeAttempts * 64u +
			entry.residentApplies * 48u +
			entry.syncSkips * 32u +
			entry.suppressionEmits * 16u;
		if (entry.suppressed)
		{
			score += 1u << 20;
		}
		return score;
	}

	uint32_t ScoreRuntimeMaterialOnlyMismatchTraceEntry(const NRIRenderer::RuntimeMaterialOnlyMismatchTraceEntry& entry)
	{
		const uint32_t materialDelta =
			entry.residentMaterialCount > entry.filteredMaterialCount ?
			entry.residentMaterialCount - entry.filteredMaterialCount :
			entry.filteredMaterialCount - entry.residentMaterialCount;
		uint32_t score = materialDelta * 128u;
		score += entry.residentMaterialCount * 16u;
		score += entry.filteredSurfaceCount * 8u;
		score += (entry.filteredWallCount + entry.filteredFlatCount) * 4u;
		if (entry.filteredWallCount != 0 && entry.filteredFlatCount != 0)
		{
			score += 1u << 18;
		}
		if (entry.residentWallCount != 0 && entry.residentFlatCount != 0)
		{
			score += 1u << 17;
		}
		return score;
	}

	enum RuntimeResidentBlasRecreateFallbackBits : uint32_t
	{
		RuntimeResidentBlasRecreateFallback_NoPreviousAs = 1u << 0,
		RuntimeResidentBlasRecreateFallback_RecoveredEmpty = 1u << 1,
		RuntimeResidentBlasRecreateFallback_SliceMoved = 1u << 2,
		RuntimeResidentBlasRecreateFallback_TopologyChanged = 1u << 3,
		RuntimeResidentBlasRecreateFallback_ForceTopology = 1u << 4,
	};

	uint32_t ScoreRuntimeResidentBlasRecreateTraceEntry(const NRIRenderer::RuntimeResidentBlasRecreateTraceEntry& entry)
	{
		uint32_t score = entry.triangleCount * 8u;
		score += entry.surfaceCount * 4u;
		score += entry.materialCount * 2u;
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_TopologyChanged) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_ForceTopology) != 0)
		{
			score += 1u << 19;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_SliceMoved) != 0)
		{
			score += 1u << 18;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_NoPreviousAs) != 0)
		{
			score += 1u << 17;
		}
		if ((entry.fallbackMask & RuntimeResidentBlasRecreateFallback_RecoveredEmpty) != 0)
		{
			score += 1u << 16;
		}
		return score;
	}

	enum RuntimeResidentBlasRefitRejectBits : uint32_t
	{
		RuntimeResidentBlasRefitReject_NoPreviousAs = 1u << 0,
		RuntimeResidentBlasRefitReject_IndexCountMismatch = 1u << 1,
		RuntimeResidentBlasRefitReject_PrimitiveCountMismatch = 1u << 2,
		RuntimeResidentBlasRefitReject_ZeroIndexCount = 1u << 3,
		RuntimeResidentBlasRefitReject_ZeroPrimitiveCount = 1u << 4,
	};

	uint32_t ScoreRuntimeResidentBlasRefitRejectTraceEntry(const NRIRenderer::RuntimeResidentBlasRefitRejectTraceEntry& entry)
	{
		const uint32_t indexDelta =
			entry.previousIndexCount > entry.liveIndexCount ?
			entry.previousIndexCount - entry.liveIndexCount :
			entry.liveIndexCount - entry.previousIndexCount;
		const uint32_t primitiveDelta =
			entry.previousPrimitiveCount > entry.livePrimitiveCount ?
			entry.previousPrimitiveCount - entry.livePrimitiveCount :
			entry.livePrimitiveCount - entry.previousPrimitiveCount;
		uint32_t score = indexDelta * 16u;
		score += primitiveDelta * 16u;
		score += entry.livePrimitiveCount * 4u;
		score += entry.liveIndexCount * 2u;
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_IndexCountMismatch) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_PrimitiveCountMismatch) != 0)
		{
			score += 1u << 19;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_NoPreviousAs) != 0)
		{
			score += 1u << 18;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_ZeroIndexCount) != 0)
		{
			score += 1u << 17;
		}
		if ((entry.rejectMask & RuntimeResidentBlasRefitReject_ZeroPrimitiveCount) != 0)
		{
			score += 1u << 16;
		}
		return score;
	}

	enum RuntimeStructuralRebuildTriggerBits : uint32_t
	{
		RuntimeStructuralRebuildTrigger_ReplacementDelta = 1u << 0,
		RuntimeStructuralRebuildTrigger_ViewChanged = 1u << 1,
		RuntimeStructuralRebuildTrigger_StaticAnimatedFlip = 1u << 2,
		RuntimeStructuralRebuildTrigger_ExcludeStaticFlip = 1u << 3,
		RuntimeStructuralRebuildTrigger_ForceTopology = 1u << 4,
		RuntimeStructuralRebuildTrigger_Invalid = 1u << 5,
	};

	uint32_t ScoreRuntimeStructuralRebuildTraceEntry(const NRIRenderer::RuntimeStructuralRebuildTraceEntry& entry)
	{
		uint32_t score = entry.triangleCount * 8u;
		score += entry.surfaceCount * 4u;
		score += entry.materialCount * 2u;
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_ForceTopology) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_StaticAnimatedFlip) != 0)
		{
			score += 1u << 19;
		}
		if ((entry.triggerMask & RuntimeStructuralRebuildTrigger_ExcludeStaticFlip) != 0)
		{
			score += 1u << 18;
		}
		if (entry.mixedMaterialOnly)
		{
			score += 1u << 17;
		}
		if (entry.geometryOrDirty)
		{
			score += 1u << 16;
		}
		return score;
	}

	enum RuntimeGeometryDirtyFamilyBits : uint32_t
	{
		RuntimeGeometryDirtyFamily_SectorGeometryOnly = 1u << 0,
		RuntimeGeometryDirtyFamily_WallGeometryOnly = 1u << 1,
		RuntimeGeometryDirtyFamily_SectorWallGeometry = 1u << 2,
		RuntimeGeometryDirtyFamily_DirtyOnly = 1u << 3,
		RuntimeGeometryDirtyFamily_GeometryDirtyMixed = 1u << 4,
	};

	uint32_t ScoreRuntimeGeometryDirtyTraceEntry(const NRIRenderer::RuntimeGeometryDirtyTraceEntry& entry)
	{
		const uint32_t triangleDelta =
			entry.previousTriangleCount > entry.liveTriangleCount ?
			entry.previousTriangleCount - entry.liveTriangleCount :
			entry.liveTriangleCount - entry.previousTriangleCount;
		const uint32_t materialDelta =
			entry.previousMaterialCount > entry.liveMaterialCount ?
			entry.previousMaterialCount - entry.liveMaterialCount :
			entry.liveMaterialCount - entry.previousMaterialCount;
		uint32_t score = triangleDelta * 32u;
		score += materialDelta * 16u;
		score += entry.liveTriangleCount * 4u;
		score += entry.liveMaterialCount * 2u;
		if ((entry.familyMask & RuntimeGeometryDirtyFamily_GeometryDirtyMixed) != 0)
		{
			score += 1u << 20;
		}
		if ((entry.familyMask & RuntimeGeometryDirtyFamily_SectorWallGeometry) != 0)
		{
			score += 1u << 19;
		}
		if (entry.forceTopology)
		{
			score += 1u << 18;
		}
		if (entry.countChanged)
		{
			score += 1u << 17;
		}
		if (entry.wallsChanged && entry.flatsChanged)
		{
			score += 1u << 16;
		}
		return score;
	}

	uint32_t ScoreRuntimeRecurringChunkTraceEntry(const NRIRenderer::RuntimeRecurringChunkTraceEntry& entry)
	{
		uint32_t score = entry.repeatedStateHitCount * 256u;
		score += entry.abaRecurrenceCount * 192u;
		score += entry.transitionCount * 64u;
		score += entry.uniqueStateCount * 32u;
		score += entry.visitCount * 8u;
		score += entry.lastTriangleCount * 4u;
		score += entry.lastMaterialCount * 2u;
		return score;
	}

	template <typename Entry, size_t N, typename ScoreFn>
	void InsertRankedTraceEntry(std::array<Entry, N>& entries, Entry entry, ScoreFn scoreFn)
	{
		entry.score = scoreFn(entry);
		size_t insertIndex = N;
		for (size_t i = 0; i < N; ++i)
		{
			if (!entries[i].valid || entry.score > entries[i].score)
			{
				insertIndex = i;
				break;
			}
		}

		if (insertIndex >= N)
		{
			return;
		}

		for (size_t i = N - 1; i > insertIndex; --i)
		{
			entries[i] = entries[i - 1];
		}
		entries[insertIndex] = entry;
		entries[insertIndex].valid = true;
	}

	std::string FormatTopologyKeyList(const std::vector<uint64_t>& keys, size_t limit = 8)
	{
		if (keys.empty())
		{
			return "none";
		}

		std::string result;
		const size_t printCount = std::min(keys.size(), limit);
		char buffer[32] = {};
		for (size_t i = 0; i < printCount; ++i)
		{
			if (!result.empty())
			{
				result += ",";
			}

			std::snprintf(buffer, sizeof(buffer), "0x%016llx", (unsigned long long)keys[i]);
			result += buffer;
		}

		if (printCount < keys.size())
		{
			result += ",...";
		}

		return result;
	}

	double GetCurrentGameplayTimeSeconds()
	{
		return PlayClock > 0 ? (double)PlayClock * BuildTickSeconds : 0.0;
	}

	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
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

	struct ScenePortalData
	{
		uint32_t traversalClass = 0;
		uint32_t kind = 0;
		uint32_t targetLocalSpaceIndex = UINT32_MAX;
		uint32_t flags = 0;
		float delta[3] = {};
		uint32_t reserved0 = 0;
	};

	static nri::StageBits NRIComputeStage()
	{
		return nri::StageBits::COMPUTE_SHADER;
	}

	static const char* GetNrdHitDistanceReconstructionModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "area_3x3";
		case 2: return "area_5x5";
		default: return "off";
		}
	}

	static bool IsSupportedPtDebugMode(uint32_t debugMode)
	{
		return IsNRIFrameGraphSupportedDebugMode(debugMode);
	}

	static uint32_t GetEffectivePtDebugMode()
	{
		if (nri_ptdebug < 0 || nri_ptdebug > (int)nri_diag::PtDebugIndirectLobeSelection)
		{
			return 0u;
		}

		const uint32_t debugMode = (uint32_t)nri_ptdebug;
		return IsSupportedPtDebugMode(debugMode) ? debugMode : 0u;
	}

	static float GetTemporalExposure(const NRIPTOutputPolicy& outputPolicy)
	{
		return std::max(outputPolicy.exposure, 0.125f);
	}

	static float GetFullbrightBoostScale()
	{
		return std::clamp((float)nri_ptfullbrightboost, 0.50f, 8.00f);
	}

	static float GetGlowmapVisibleBlendScale()
	{
		return std::clamp((float)nri_ptglowblend, 0.0f, 3.0f);
	}

	static float GetExposureDeltaStops(float previousExposure, float currentExposure)
	{
		const float safePrevious = std::max(previousExposure, 0.125f);
		const float safeCurrent = std::max(currentExposure, 0.125f);
		return std::abs(std::log2(safeCurrent) - std::log2(safePrevious));
	}

	static uint32_t GetBootstrapMode();

	static NRIPresentRouteInfo ResolvePresentRouteInfo(uint32_t debugMode, bool bootstrap)
	{
		NRIFrameRouteRequest request = {};
		request.debugMode = debugMode;
		request.bootstrap = bootstrap;
		request.bootstrapMode = bootstrap ? GetBootstrapMode() : 0u;
		return ResolveNRIFrameRoute(request);
	}

	static NRINrdDenoiserMode GetSelectedNrdDenoiserMode()
	{
		return (NRINrdDenoiserMode)std::clamp((int)nri_nrddenoiser, 0, 1);
	}

	static const char* GetNrdDenoiserModeName(NRINrdDenoiserMode mode)
	{
		switch (mode)
		{
		case NRINrdDenoiserMode::Relax: return "RELAX_DIFFUSE_SPECULAR";
		default: return "REBLUR_DIFFUSE_SPECULAR";
		}
	}

	static const char* GetNrdInputSplitModeName(uint32_t mode)
	{
		switch (mode)
		{
		case 1: return "raw_left_denoised_right";
		case 2: return "denoised_left_raw_right";
		default: return "off";
		}
	}

	static uint32_t GetDispatchSize(uint32_t value)
	{
		return (value + 7u) / 8u;
	}

	static uint64_t HashPrimitiveRewriteProvenancePayload(const std::vector<nri_scene::SurfaceProvenance>& provenanceList)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)provenanceList.size());
		for (const nri_scene::SurfaceProvenance& provenance : provenanceList)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)provenance.sourceType);
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.wallIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.sectionIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.mapChunkIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.nextSectorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(provenance.actorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.drawListType);
			hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.cstat);
			hash = nri_scene::HashCombine64(hash, (uint64_t)provenance.materialFlags);
		}
		return hash != 0 ? hash : 1;
	}

	static uint64_t HashPrimitiveRewriteVisibilityIdentity(const nri_scene::PTMapWorld& mapWorld)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, mapWorld.valid ? 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, mapWorld.buildSerial);
		hash = nri_scene::HashCombine64(hash, (uint64_t)mapWorld.chunks.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)mapWorld.stats.chunkCount);
		for (const nri_scene::PTMapChunk& chunk : mapWorld.chunks)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.chunkIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(chunk.sectorIndex + 1));
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.firstSurface);
			hash = nri_scene::HashCombine64(hash, (uint64_t)chunk.surfaceCount);
		}
		return hash != 0 ? hash : 1;
	}

	static void MarkChunkVisible(std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const size_t wordIndex = (size_t)(chunkIndex >> 5u);
		if (wordIndex >= visibleChunkWords.size())
		{
			return;
		}

		visibleChunkWords[wordIndex] |= 1u << (chunkIndex & 31u);
	}

	static bool IsChunkMarkedVisible(const std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const size_t wordIndex = (size_t)(chunkIndex >> 5u);
		if (wordIndex >= visibleChunkWords.size())
		{
			return false;
		}

		return (visibleChunkWords[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
	}

	static uint32_t GetFlatPlaneVisibilityIndex(int32_t sectorIndex, bool ceiling)
	{
		return (uint32_t)sectorIndex * 2u + (ceiling ? 1u : 0u);
	}

	static void MarkFlatPlaneVisible(std::vector<uint32_t>& visibleFlatPlaneWords, int32_t sectorIndex, bool ceiling)
	{
		if (sectorIndex < 0)
		{
			return;
		}

		const uint32_t flatPlaneIndex = GetFlatPlaneVisibilityIndex(sectorIndex, ceiling);
		const size_t wordIndex = (size_t)(flatPlaneIndex >> 5u);
		if (wordIndex >= visibleFlatPlaneWords.size())
		{
			return;
		}

		visibleFlatPlaneWords[wordIndex] |= 1u << (flatPlaneIndex & 31u);
	}

	static void MarkVisibleChunkForSector(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex, std::vector<uint32_t>& visibleChunkWords)
	{
		const int32_t chunkIndex = nri_static_scene_geometry::FindMapChunkIndexForSector(mapWorld, sectorIndex);
		if (chunkIndex >= 0)
		{
			MarkChunkVisible(visibleChunkWords, (uint32_t)chunkIndex);
		}
	}

	static void AccumulateVisibleChunksFromViewRoots(const HWDrawInfo& di, const nri_scene::PTMapWorld& mapWorld, std::vector<uint32_t>& visibleChunkWords)
	{
		if (di.Viewpoint.SectNums != nullptr)
		{
			for (int i = 0; i < di.Viewpoint.SectCount; ++i)
			{
				MarkVisibleChunkForSector(mapWorld, di.Viewpoint.SectNums[i], visibleChunkWords);
			}
		}
		else
		{
			MarkVisibleChunkForSector(mapWorld, di.Viewpoint.SectCount, visibleChunkWords);
		}
	}

	static void AccumulateVisibleChunksFromDrawLists(const HWDrawInfo& di, const nri_scene::PTMapWorld& mapWorld, std::vector<uint32_t>& visibleChunkWords)
	{
		for (int drawListType = 0; drawListType < GLDL_TYPES; ++drawListType)
		{
			const HWDrawList& drawList = di.drawlists[drawListType];

			for (const HWWall* wall : drawList.walls)
			{
				if (wall != nullptr && wall->seg != nullptr)
				{
					MarkVisibleChunkForSector(mapWorld, wall->seg->sector, visibleChunkWords);
				}
			}

			for (const HWFlat* flat : drawList.flats)
			{
				if (flat != nullptr && flat->sec != nullptr)
				{
					MarkVisibleChunkForSector(mapWorld, sector.IndexOf(flat->sec), visibleChunkWords);
				}
			}
		}
	}

	static void AccumulateVisibleFlatPlanesFromDrawLists(const HWDrawInfo& di, std::vector<uint32_t>& visibleFlatPlaneWords)
	{
		for (int drawListType = 0; drawListType < GLDL_TYPES; ++drawListType)
		{
			const HWDrawList& drawList = di.drawlists[drawListType];
			for (const HWFlat* flat : drawList.flats)
			{
				if (flat == nullptr || flat->sec == nullptr || flat->Sprite != nullptr)
				{
					continue;
				}

				MarkFlatPlaneVisible(
					visibleFlatPlaneWords,
					sector.IndexOf(flat->sec),
					flat->plane != 0);
			}
		}
	}

	static float Clamp01(float value)
	{
		return std::max(0.0f, std::min(value, 1.0f));
	}

	static uint32_t ClampTraceBounceCount(int value, uint32_t maxValue)
	{
		return (uint32_t)std::max(0, std::min(value, (int)maxValue));
	}

	static float GetBaseAmbient()
	{
		return std::max(0.0f, (float)nri_ptbaseambient);
	}

	static float GetMetalAmbient()
	{
		return std::max(0.0f, (float)nri_ptmetalambient);
	}

	static uint32_t PackAmbientMultiplier12(float value)
	{
		return (uint32_t)std::min(4095.0f, std::max(0.0f, value) * 1024.0f + 0.5f);
	}

	static uint32_t PackPortalDepthAndAmbientMultipliers(uint32_t portalDepth, float baseAmbient, float metalAmbient)
	{
		return
			(portalDepth & 0xffu) |
			(PackAmbientMultiplier12(baseAmbient) << 8u) |
			(PackAmbientMultiplier12(metalAmbient) << 20u);
	}

	static uint32_t PackTraceBounceCounts(uint32_t lightBounceCount, uint32_t mirrorBounceCount, const float directionalColor[3])
	{
		return
			(lightBounceCount & 0xfu) |
			((mirrorBounceCount & 0xfu) << 4u) |
			(PackDirectionalLightColor24(directionalColor) << 8u);
	}

	static uint32_t PackTraceAux1(uint32_t denoiserMode, uint32_t emissiveSampleCount, float directionalAngularSize)
	{
		return
			(denoiserMode & 0xffu) |
			((emissiveSampleCount & 0xffu) << 8u) |
			(PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static uint32_t PackDenoiserAux1(uint32_t denoiserMode, float directionalAngularSize)
	{
		return (denoiserMode & 0xffu) | (PackDirectionalAngularSize16(directionalAngularSize) << 16u);
	}

	static uint32_t PackUInt16Pair(uint32_t lo, uint32_t hi)
	{
		return (lo & 0xffffu) | ((hi & 0xffffu) << 16u);
	}

	static uint32_t GetPortalTraversalClass(nri_scene::PTPortalKind kind)
	{
		switch (kind)
		{
		case nri_scene::PTPortalKind::WallMirror:
		case nri_scene::PTPortalKind::SectorFloorMirror:
		case nri_scene::PTPortalKind::SectorCeilingMirror:
			return NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE;

		case nri_scene::PTPortalKind::WallView:
		case nri_scene::PTPortalKind::SectorFloorStack:
		case nri_scene::PTPortalKind::SectorCeilingStack:
			return NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER;

		case nri_scene::PTPortalKind::WallToSprite:
			return NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND;

		default:
			return NRI_PORTAL_TRAVERSAL_CLASS_NONE;
		}
	}

	static uint32_t CountPortalTraversalClass(const nri_scene::PTMapWorld& mapWorld, uint32_t traversalClass)
	{
		uint32_t count = 0;
		for (const auto& portal : mapWorld.portals)
		{
			if (GetPortalTraversalClass(portal.kind) == traversalClass)
			{
				count++;
			}
		}
		return count;
	}

	static uint32_t CountPendingPlanePortals(const nri_scene::PTMapWorld& mapWorld)
	{
		uint32_t count = 0;
		for (const auto& portal : mapWorld.portals)
		{
			switch (portal.kind)
			{
			case nri_scene::PTPortalKind::SectorFloorStack:
			case nri_scene::PTPortalKind::SectorCeilingStack:
			case nri_scene::PTPortalKind::SectorFloorMirror:
			case nri_scene::PTPortalKind::SectorCeilingMirror:
				if (portal.sourceSurfaceIndex == UINT32_MAX)
				{
					count++;
				}
				break;
			default:
				break;
			}
		}
		return count;
	}

	static std::vector<ScenePortalData> BuildScenePortalData(const nri_scene::PTMapWorld& mapWorld)
	{
		std::vector<ScenePortalData> portals;
		portals.reserve(std::max<size_t>(mapWorld.portals.size(), 1u));

		for (const auto& portal : mapWorld.portals)
		{
			ScenePortalData data = {};
			data.traversalClass = GetPortalTraversalClass(portal.kind);
			data.kind = (uint32_t)portal.kind;
			data.flags = portal.runtimeBoundTarget ? NRI_PORTAL_FLAG_RUNTIME_BOUND : 0u;
			if (portal.targetCount > 0 && portal.firstTarget < mapWorld.portalTargets.size())
			{
				data.targetLocalSpaceIndex = mapWorld.portalTargets[portal.firstTarget].localSpaceIndex;
			}
			data.delta[0] = (float)portal.delta[0];
			data.delta[1] = (float)portal.delta[1];
			data.delta[2] = (float)portal.delta[2];
			portals.push_back(data);
		}

		if (portals.empty())
		{
			portals.push_back({});
		}

		return portals;
	}

	static void AppendGeometryChunk(
		const nri_scene::GeometryData& source,
		uint32_t sourceVertexOffset,
		uint32_t sourceVertexCount,
		uint32_t sourceIndexOffset,
		uint32_t sourceIndexCount,
		uint32_t sourcePrimitiveOffset,
		uint32_t sourcePrimitiveCount,
		nri_scene::GeometryData& destination)
	{
		if (sourceVertexOffset >= source.vertices.size() ||
			sourcePrimitiveOffset >= source.primitives.size() ||
			sourceVertexCount == 0 ||
			sourcePrimitiveCount == 0)
		{
			return;
		}

		sourceVertexCount = std::min(sourceVertexCount, (uint32_t)source.vertices.size() - sourceVertexOffset);
		if (sourceIndexOffset >= source.indices.size())
		{
			sourceIndexCount = 0;
		}
		else
		{
			sourceIndexCount = std::min(sourceIndexCount, (uint32_t)source.indices.size() - sourceIndexOffset);
		}
		sourcePrimitiveCount = std::min(sourcePrimitiveCount, (uint32_t)source.primitives.size() - sourcePrimitiveOffset);
		const uint32_t sourcePrimitiveProvenanceCount =
			sourcePrimitiveOffset < source.primitiveProvenance.size() ?
			std::min(sourcePrimitiveCount, (uint32_t)source.primitiveProvenance.size() - sourcePrimitiveOffset) :
			0u;

		const uint32_t vertexBase = (uint32_t)destination.vertices.size();
		destination.vertices.insert(
			destination.vertices.end(),
			source.vertices.begin() + sourceVertexOffset,
			source.vertices.begin() + sourceVertexOffset + sourceVertexCount);

		if (sourceIndexCount > 0)
		{
			destination.indices.reserve(destination.indices.size() + sourceIndexCount);
			for (uint32_t i = 0; i < sourceIndexCount; ++i)
			{
				destination.indices.push_back(vertexBase + source.indices[sourceIndexOffset + i] - sourceVertexOffset);
			}
		}

		destination.primitives.reserve(destination.primitives.size() + sourcePrimitiveCount);
		for (uint32_t i = 0; i < sourcePrimitiveCount; ++i)
		{
			nri_scene::PrimitiveData copy = source.primitives[sourcePrimitiveOffset + i];
			copy.indices[0] = vertexBase + copy.indices[0] - sourceVertexOffset;
			copy.indices[1] = vertexBase + copy.indices[1] - sourceVertexOffset;
			copy.indices[2] = vertexBase + copy.indices[2] - sourceVertexOffset;
			destination.primitives.push_back(copy);
		}

		if (sourcePrimitiveProvenanceCount > 0)
		{
			destination.primitiveProvenance.insert(
				destination.primitiveProvenance.end(),
				source.primitiveProvenance.begin() + sourcePrimitiveOffset,
				source.primitiveProvenance.begin() + sourcePrimitiveOffset + sourcePrimitiveProvenanceCount);
		}
	}

	static void RemapMaterialBridgeAgainstTextureTable(
		const nri_scene::MaterialBridgeData& source,
		nri_scene::MaterialBridgeData& inOutTextureTable,
		nri_scene::MaterialBridgeData& outRemapped,
		bool* outTextureTableGrew = nullptr)
	{
		if (outTextureTableGrew != nullptr)
		{
			*outTextureTableGrew = false;
		}

		std::unordered_map<uint64_t, uint32_t> textureLookup;
		textureLookup.reserve(inOutTextureTable.textures.size() + source.textures.size());
		for (uint32_t i = 0; i < (uint32_t)inOutTextureTable.textures.size(); ++i)
		{
			textureLookup.emplace(inOutTextureTable.textures[i].key, i);
		}

		auto remapTextureIndex = [&source, &inOutTextureTable, &textureLookup, outTextureTableGrew](uint32_t textureIndex) -> uint32_t
		{
			if (textureIndex == UINT32_MAX)
			{
				return UINT32_MAX;
			}
			if (textureIndex >= source.textures.size())
			{
				return textureIndex;
			}

			const auto& texture = source.textures[textureIndex];
			auto it = textureLookup.find(texture.key);
			if (it != textureLookup.end())
			{
				return it->second;
			}

			const uint32_t newIndex = (uint32_t)inOutTextureTable.textures.size();
			textureLookup.emplace(texture.key, newIndex);
			inOutTextureTable.textures.push_back(texture);
			if (outTextureTableGrew != nullptr)
			{
				*outTextureTableGrew = true;
			}
			return newIndex;
		};

		outRemapped = {};
		outRemapped.materials.reserve(source.materials.size());
		outRemapped.lightMetadata.reserve(source.lightMetadata.size());

		for (size_t materialIndex = 0; materialIndex < source.materials.size(); ++materialIndex)
		{
			const auto& material = source.materials[materialIndex];
			nri_scene::MaterialData copy = material;
			copy.textureIndex = remapTextureIndex(material.textureIndex);
			copy.normalTextureIndex = remapTextureIndex(material.normalTextureIndex);
			copy.metallicTextureIndex = remapTextureIndex(material.metallicTextureIndex);
			copy.roughnessTextureIndex = remapTextureIndex(material.roughnessTextureIndex);
			copy.emissiveTextureIndex = remapTextureIndex(material.emissiveTextureIndex);
			outRemapped.materials.push_back(copy);

			if (materialIndex < source.lightMetadata.size())
			{
				nri_scene::MaterialLightingMetadata metadata = source.lightMetadata[materialIndex];
				metadata.textureIndex = remapTextureIndex(metadata.textureIndex);
				metadata.glowmapTextureIndex = remapTextureIndex(metadata.glowmapTextureIndex);
				metadata.normalTextureIndex = remapTextureIndex(metadata.normalTextureIndex);
				metadata.metallicTextureIndex = remapTextureIndex(metadata.metallicTextureIndex);
				metadata.roughnessTextureIndex = remapTextureIndex(metadata.roughnessTextureIndex);
				metadata.emissiveTextureIndex = remapTextureIndex(metadata.emissiveTextureIndex);
				outRemapped.lightMetadata.push_back(metadata);
			}
		}

		if (!source.paletteLookup.empty())
		{
			outRemapped.paletteLookup = source.paletteLookup;
			outRemapped.paletteWidth = source.paletteWidth;
			outRemapped.paletteHeight = source.paletteHeight;
			if (inOutTextureTable.paletteLookup.empty())
			{
				inOutTextureTable.paletteLookup = source.paletteLookup;
				inOutTextureTable.paletteWidth = source.paletteWidth;
				inOutTextureTable.paletteHeight = source.paletteHeight;
			}
		}
	}

	static bool TryBuildMergedSectorMaterialOnlyBridge(
		const nri_scene::SceneView& residentChunkView,
		const nri_scene::MaterialBridgeData& residentChunkMaterials,
		const nri_scene::SceneView& filteredLiveChunkView,
		const nri_scene::MaterialBridgeData& filteredLiveMaterials,
		nri_scene::MaterialBridgeData& outMergedMaterials)
	{
		if (!filteredLiveChunkView.opaqueWalls.empty())
		{
			return false;
		}

		const uint32_t residentWallCount = (uint32_t)residentChunkView.opaqueWalls.size();
		const uint32_t residentFlatCount = (uint32_t)residentChunkView.opaqueFlats.size();
		if (residentFlatCount == 0 ||
			filteredLiveChunkView.opaqueFlats.size() != residentFlatCount ||
			filteredLiveMaterials.materials.size() != residentFlatCount ||
			filteredLiveMaterials.lightMetadata.size() != residentFlatCount)
		{
			return false;
		}

		if (residentChunkMaterials.materials.size() != residentChunkMaterials.lightMetadata.size() ||
			residentWallCount + residentFlatCount > residentChunkMaterials.materials.size())
		{
			return false;
		}

		outMergedMaterials = residentChunkMaterials;
		nri_scene::MaterialBridgeData remappedFlatMaterials;
		RemapMaterialBridgeAgainstTextureTable(
			filteredLiveMaterials,
			outMergedMaterials,
			remappedFlatMaterials);
		if (remappedFlatMaterials.materials.size() != residentFlatCount ||
			remappedFlatMaterials.lightMetadata.size() != residentFlatCount)
		{
			return false;
		}

		std::copy_n(
			remappedFlatMaterials.materials.data(),
			residentFlatCount,
			outMergedMaterials.materials.begin() + residentWallCount);
		std::copy_n(
			remappedFlatMaterials.lightMetadata.data(),
			residentFlatCount,
			outMergedMaterials.lightMetadata.begin() + residentWallCount);
		return true;
	}

	static bool TryBuildMergedSectorMaterialOnlySceneView(
		const nri_scene::SceneView& residentChunkView,
		const nri_scene::SceneView& filteredLiveChunkView,
		nri_scene::SceneView& outMergedSceneView)
	{
		if (!filteredLiveChunkView.opaqueWalls.empty() ||
			residentChunkView.opaqueFlats.size() != filteredLiveChunkView.opaqueFlats.size())
		{
			return false;
		}

		outMergedSceneView = filteredLiveChunkView;
		outMergedSceneView.opaqueWalls = residentChunkView.opaqueWalls;
		outMergedSceneView.opaqueSprites = residentChunkView.opaqueSprites;
		outMergedSceneView.stats.totalDrawItems =
			(unsigned)(outMergedSceneView.opaqueWalls.size() +
				outMergedSceneView.opaqueFlats.size() +
				outMergedSceneView.opaqueSprites.size());
		outMergedSceneView.stats.wallDrawItems = (unsigned)outMergedSceneView.opaqueWalls.size();
		outMergedSceneView.stats.flatDrawItems = (unsigned)outMergedSceneView.opaqueFlats.size();
		outMergedSceneView.stats.spriteDrawItems = (unsigned)outMergedSceneView.opaqueSprites.size();
		outMergedSceneView.stats.materialRefs = outMergedSceneView.stats.totalDrawItems;
		return true;
	}

	static bool StructuredBufferUpdateNeedsWait(
		const NRIBufferResource& resource,
		const void* data,
		uint64_t size,
		uint32_t stride)
	{
		const uint64_t requiredSize = std::max<uint64_t>(size, stride);
		const bool needsGrowth =
			resource.buffer == nullptr ||
			resource.shaderView == nullptr ||
			resource.stride != stride ||
			resource.size < requiredSize;
		if (needsGrowth)
		{
			return resource.buffer != nullptr || resource.shaderView != nullptr;
		}

		return data != nullptr && size != 0;
	}

	static const char* GetUpscalerFamilyName(NRIMainUpscalerKind kind, bool runAppTaa)
	{
		switch (kind)
		{
		case NRIMainUpscalerKind::DLSR: return "vendor-sr";
		case NRIMainUpscalerKind::DLRR: return "vendor-rr";
		default: return runAppTaa ? "native-taa" : "native";
		}
	}

	static uint32_t PackPresentSceneOrigin(int sceneLeft, int sceneTop)
	{
		return (uint16_t)(int16_t)sceneLeft | ((uint32_t)(uint16_t)(int16_t)sceneTop << 16);
	}

	static void ApplyOutputPolicyToPresentConstants(const NRIPTOutputPolicy& policy, NRIPresentConstants& constants)
	{
		constants.OutputMode = (uint32_t)policy.resolvedMode;
		constants.TonemapMode = (uint32_t)policy.tonemapMode;
		constants.OutputFlags =
			(policy.displayInfoAvailable ? NRI_PRESENT_OUTPUT_FLAG_DISPLAY_INFO_AVAILABLE : 0u) |
			(policy.displayHdrSupported ? NRI_PRESENT_OUTPUT_FLAG_DISPLAY_HDR_SUPPORTED : 0u) |
			(policy.hdrSwapChainActive ? NRI_PRESENT_OUTPUT_FLAG_HDR_SWAPCHAIN_ACTIVE : 0u) |
			(policy.offscreenHdrTarget ? NRI_PRESENT_OUTPUT_FLAG_OFFSCREEN_HDR_TARGET : 0u);
		constants.Exposure = policy.exposure;
		constants.Contrast = policy.contrast;
		constants.Saturation = policy.saturation;
		constants.Shoulder = policy.shoulder;
		constants.Toe = policy.toe;
		constants.PaperWhiteNits = policy.paperWhiteNits;
		constants.DisplayMaxLuminance = policy.displayMaxLuminance;
		constants.DisplaySdrLuminance = policy.displaySdrLuminance;
	}

	static void ApplyNightVisionStateToPresentConstants(const NRIPTNightVisionState& state, NRIPresentConstants& constants)
	{
		constants.NightVisionPackedModeTint = PackNightVisionModeAndTint(
			state.mode,
			(float)nri_ptnightvisionred,
			(float)nri_ptnightvisiongreen,
			(float)nri_ptnightvisionblue);
		constants.NightVisionStrength = nri_ptnightvision ? state.strength01 : 0.0f;
		constants.NightVisionExposure = (float)nri_ptnightvisionexposure;
		constants.NightVisionPackedControls = PackNightVisionControls(
			(float)nri_ptnightvisioncontrast,
			(float)nri_ptnightvisionsaturation);
	}

	static float GetHaltonSample(uint32_t index, uint32_t base)
	{
		float inverseBase = 1.0f / (float)base;
		float fraction = inverseBase;
		float result = 0.0f;

		while (index > 0)
		{
			result += fraction * (float)(index % base);
			index /= base;
			fraction *= inverseBase;
		}

		return result;
	}

	static void ComputeTemporalJitter(uint32_t frameIndex, uint32_t jitterPhaseCount, float outJitter[2])
	{
		jitterPhaseCount = std::max(jitterPhaseCount, 1u);
		const uint32_t sampleIndex = (frameIndex % jitterPhaseCount) + 1u;
		outJitter[0] = GetHaltonSample(sampleIndex, 2u) - 0.5f;
		outJitter[1] = GetHaltonSample(sampleIndex, 3u) - 0.5f;
	}

	static void Normalize3(float v[3])
	{
		const float length = std::max(0.0001f, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}

	static void ApplyDirectionalLightStateToConstants(const NRIDirectionalLightState& state, NRITraceSceneConstants& constants)
	{
		constants.LightDirection[0] = state.direction[0];
		constants.LightDirection[1] = state.direction[1];
		constants.LightDirection[2] = state.direction[2];
		Normalize3(constants.LightDirection);
	}

	static void TransformPoint(const VSMatrix& matrix, float x, float y, float z, float out[4])
	{
		float point[4] = { x, y, z, 1.0f };
		VSMatrix copy = matrix;
		copy.multMatrixPoint(point, out);
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

	static const char* GetGraphicsApiName(nri::GraphicsAPI api)
	{
		switch (api)
		{
		case nri::GraphicsAPI::D3D12: return "d3d12";
		case nri::GraphicsAPI::VK: return "vulkan";
		default: return "unknown";
		}
	}

	static uint64_t HashBytes64(const uint8_t* data, size_t size)
	{
		uint64_t hash = 1469598103934665603ull;
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= (uint64_t)data[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	template <typename SurfaceContainer>
	static bool SurfaceContainerUsesHardwareCanvasTexture(const SurfaceContainer& surfaces)
	{
		for (const auto& surface : surfaces)
		{
			if (surface.material.texture != nullptr &&
				surface.material.texture->isHardwareCanvas())
			{
				return true;
			}
		}

		return false;
	}

	static bool SceneViewUsesHardwareCanvasTexture(const nri_scene::SceneView& sceneView)
	{
		return
			SurfaceContainerUsesHardwareCanvasTexture(sceneView.opaqueWalls) ||
			SurfaceContainerUsesHardwareCanvasTexture(sceneView.opaqueFlats) ||
			SurfaceContainerUsesHardwareCanvasTexture(sceneView.opaqueSprites);
	}

	static bool IsAuthoredTextureCurrentlyUnresolved(FTextureID textureId)
	{
		if (!textureId.isValid())
		{
			return false;
		}

		FGameTexture* texture = TexMan.GetGameTexture(textureId, true);
		return texture == nullptr || !texture->isValid();
	}

	static bool ChunkHasUnresolvedAuthoredTextures(const nri_scene::PTMapChunk& chunk)
	{
		if (chunk.kind != nri_scene::PTMapChunkKind::Sector ||
			chunk.sectorIndex < 0 ||
			(unsigned)chunk.sectorIndex >= sector.Size())
		{
			return false;
		}

		const sectortype& sec = sector[(unsigned)chunk.sectorIndex];
		if (IsAuthoredTextureCurrentlyUnresolved(sec.floortexture) ||
			IsAuthoredTextureCurrentlyUnresolved(sec.ceilingtexture))
		{
			return true;
		}

		for (const walltype& wal : sec.walls)
		{
			if (IsAuthoredTextureCurrentlyUnresolved(wal.walltexture) ||
				IsAuthoredTextureCurrentlyUnresolved(wal.overtexture))
			{
				return true;
			}

			if (wal.nextwall >= 0 && (unsigned)wal.nextwall < wall.Size())
			{
				const walltype& nextWall = wall[(unsigned)wal.nextwall];
				if (IsAuthoredTextureCurrentlyUnresolved(nextWall.walltexture) ||
					IsAuthoredTextureCurrentlyUnresolved(nextWall.overtexture))
				{
					return true;
				}
			}
		}

		return false;
	}

	static void RemapToPTSpace(const float* src, float* dst)
	{
		dst[0] = src[0];
		dst[1] = src[2];
		dst[2] = src[1];
	}

	static uint32_t GetBootstrapMode()
	{
		return (uint32_t)std::max(0, std::min((int)nri_ptbootstrapmode, 13));
	}

	static const char* GetSceneLightRecordSourceName(SceneLightRecordSource source)
	{
		switch (source)
		{
		case SceneLightRecordSource::CapturedScene: return "captured_scene";
		case SceneLightRecordSource::StaticMapScene: return "static_map_scene";
		case SceneLightRecordSource::RuntimeMutationScene: return "runtime_mutation_scene";
		case SceneLightRecordSource::DynamicScene: return "dynamic_scene";
		case SceneLightRecordSource::SurfaceLightOverlayScene: return "surface_light_overlay_scene";
		case SceneLightRecordSource::PersistentVoxelScene: return "persistent_voxel_scene";
		default: return "none";
	}
}

}

NRIRenderer::NRIRenderer(NRIRenderDevice* frameBuffer)
	: mFrameBuffer(frameBuffer),
	mSmoke(std::make_unique<NRISmokeSystem>())
{
}

NRIRenderer::~NRIRenderer()
{
	Shutdown();
}

void NRIRenderer::PrintSmokeStatus() const
{
	if (mSmoke != nullptr)
	{
		mSmoke->PrintStatus(*this);
	}
}

void NRIRenderer::ResetSmoke(const char* reason)
{
	if (mSmoke != nullptr)
	{
		mSmoke->Reset(reason);
	}
}

void NRIRenderer::QueueSyntheticSmoke()
{
	if (mSmoke != nullptr)
	{
		mSmoke->QueueSyntheticInjection();
	}
}

bool NRIRenderer::Initialize()
{
	Clocker clock(NriPTInitialize);

	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return false;
	}

	if (!mSceneTextures.LimitLogPrinted())
	{
		LogSceneTextureDescriptorLimits(mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice));
		mSceneTextures.LimitLogPrinted() = true;
	}

	if (!CheckPathTracingSupport())
	{
		return true;
	}

	if (mPipelineLayout != nullptr)
	{
		return true;
	}

	const bool rendererReady =
		NRIPipelineStateManager::CreatePipelineLayout(*this) &&
		NRIPipelineStateManager::CreateTaaPipelineLayout(*this) &&
		NRIPipelineStateManager::CreatePresentPipelineLayout(*this) &&
		NRIPipelineStateManager::CreateExposurePipelineLayout(*this) &&
		NRIPipelineStateManager::CreateBloomPipelineLayout(*this) &&
		NRIPipelineStateManager::CreateVoxelComputePipelineLayout(*this) &&
		NRIDescriptorSetManager::AllocateDescriptorSets(*this) &&
		NRIDescriptorSetManager::UpdateSamplerSet(*this) &&
		NRIPipelineStateManager::CreatePipelines(*this);
	return rendererReady && (mSmoke == nullptr || mSmoke->Initialize(*this));
}

void NRIRenderer::Shutdown()
{
	if (mSmoke != nullptr)
	{
		mSmoke->Shutdown(*this);
	}
	mWeaponEventBatch.Reset();
	ResetMuzzleFlashOverlayState("renderer-shutdown");
	mLastResolvedLightOverlayGeneration = 0;

	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return;
	}

	mNrd.Shutdown();
	mUpscaler.Shutdown(*mFrameBuffer);
	mIndirectRadianceCache.Destroy(BuildResourceServices());
	DestroyNRIVoxelComputeMeshingDiagnostics(*this);
	DestroyAccelerationStructures();
	ClearRuntimePointLights();
	DestroySceneBuffers();
	NRIFrameResources::DestroyFrameTextures(*this);
	mFrameBuffer->DestroyTextureResource(mSceneTextures.PaletteTexture());
	DestroyCachedTextures();
	mFrameGenerationFrameId = 0;
	mHasFrameGenerationRealFrameTime = false;
	mHasPendingFrameGenerationRealFrameTime = false;
	mHasFrameGenerationTimestamp = false;
	mHasFrameGenerationConfigState = false;
	mLastFrameGenerationRealFrameTimeMs = 0.0f;
	mPendingFrameGenerationRealFrameTimeMs = 0.0f;
	mLastFrameGenerationTimestamp = {};
	mPendingFrameGenerationTimestamp = {};
	mSceneTextures.LimitLogPrinted() = false;
	mLastFrameGenerationRequestedEnabled = false;
	mLastFrameGenerationRequestedProvider = NRIFrameGenerationProvider::Off;
	mLastFrameGenerationResolvedUiMode = NRIFrameGenerationUiMode::Auto;

	for (nri::Pipeline*& pipeline : mPipelines)
	{
		if (pipeline != nullptr)
		{
			mFrameBuffer->mCore.DestroyPipeline(pipeline);
			pipeline = nullptr;
		}
	}

	if (mPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mPipelineLayout);
		mPipelineLayout = nullptr;
	}
	if (mIndirectRadianceCachePipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mIndirectRadianceCachePipelineLayout);
		mIndirectRadianceCachePipelineLayout = nullptr;
	}
	if (mTaaPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mTaaPipelineLayout);
		mTaaPipelineLayout = nullptr;
	}
	if (mPresentPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mPresentPipelineLayout);
		mPresentPipelineLayout = nullptr;
	}
	if (mExposurePipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mExposurePipelineLayout);
		mExposurePipelineLayout = nullptr;
	}
	if (mBloomPipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mBloomPipelineLayout);
		mBloomPipelineLayout = nullptr;
	}
	if (mVoxelComputePipelineLayout != nullptr)
	{
		mFrameBuffer->mCore.DestroyPipelineLayout(mVoxelComputePipelineLayout);
		mVoxelComputePipelineLayout = nullptr;
	}

	mSamplerSet = nullptr;
	mSceneTextureSets.clear();
	mSceneTextureSetHashes.clear();
	mSceneTextureSetHashValid.clear();
	mSceneDataSets.clear();
	mSceneDataSnapshots.clear();
	mActiveSceneDataSet = nullptr;
	mActiveSceneDataSnapshot = nullptr;
	mActiveSceneDataSetFrameIndex = UINT64_MAX;
	mSceneDataSnapshotCursor = 0;
	mFrameTextureSet = nullptr;
	mOutputSet = nullptr;
	mCompositionFrameTextureSet = nullptr;
	mCompositionOutputSet = nullptr;
	mUpscalerPrepassFrameTextureSet = nullptr;
	mUpscalerPrepassOutputSet = nullptr;
	mTaaFrameTextureSet = nullptr;
	mTaaOutputSet = nullptr;
	mRawPresentFrameTextureSet = nullptr;
	mRawPresentOutputSet = nullptr;
	mFinalPresentFrameTextureSet = nullptr;
	mFinalPresentOutputSet = nullptr;
	mBloomInputSets = {};
	mBloomOutputSets = {};
	mExposureInputSets = {};
	mExposureOutputSets = {};
	mVoxelComputeInputSets = {};
	mVoxelComputeOutputSets = {};
	mAutoExposureInputSourceSlot = FrameTextureSlot::Count;
	mSceneDataDescriptorsInitialized.clear();
	mSceneDataDescriptorMapEpochs.clear();
	mSceneDataDescriptorBuildEpochs.clear();
}

void NRIRenderer::OnLevelUnloadBegin(const LevelTransitionInfo& info)
{
	WaitForCommandsTracked("level-unload");
	RequestHistoryReset("level-unload", true, true);
	ResetSmoke("level-unload");

	nri_scene::ResetPersistentVoxelActorCache("level-unload");
	mPersistentVoxels.ResetLevelSchedulingState(
		"level-unload",
		(int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats,
		BuildNRIPersistentVoxelResetServices(*this));
	mVoxelRepresentationPolicy.Reset();
	if (info.oldLevel != info.newLevel)
	{
		mPersistentVoxels.CompactMaterialRangesForQuiescentLevelTransition(
			"level-unload",
			(int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats);
	}

	// Static BLAS resources are about to be destroyed. Retire every TLAS that
	// can reference them and invalidate all scene-data publications from the old
	// level before any new queued-frame slot can be considered trace-ready.
	DestroyWorldTlasFrameSlots();
	for (uint8_t& initialized : mSceneDataDescriptorsInitialized)
	{
		initialized = 0u;
	}
	std::fill(mSceneDataDescriptorMapEpochs.begin(), mSceneDataDescriptorMapEpochs.end(), 0ull);
	std::fill(mSceneDataDescriptorBuildEpochs.begin(), mSceneDataDescriptorBuildEpochs.end(), 0ull);
	for (SceneDataDescriptorSnapshot& snapshot : mSceneDataSnapshots)
	{
		snapshot.descriptorsInitialized = false;
		snapshot.publishedMapEpoch = 0;
		snapshot.publishedBuildEpoch = 0;
	}
	mActiveSceneDataSet = nullptr;
	mActiveSceneDataSnapshot = nullptr;
	mActiveSceneDataSetFrameIndex = UINT64_MAX;
	mSceneDataDescriptors.fill(nullptr);
	mSceneInstancePayloadCacheValid = false;
	mSceneInstancePayloadHash = 0;
	mSceneInstancePayloadCount = 0;
	mPortalPayloadCacheValid = false;
	mPortalPayloadHash = 0;
	mPortalPayloadBuildSerial = 0;
	mPortalPayloadCount = 0;

	DestroyStaticMapSceneCache("level-unload");
	mStaticMapScene = {};
	mStaticAccelerationBuildSerial = 0;
	mSkyEnvironment.PreservedStaticMapSky() = {};

	mMapWorld = {};
	mObservedMapWorldBuildSerial = 0;
	mMapMoverShadow.Reset();
	mMapMoverRigidRoute.Reset();
	mSE29FloorDeformerRoute.Reset();
	mMapMaterialOnlyRoute.Reset();

	DestroyCachedTextures();
	ResetPersistentDynamicEmissiveCache();
	mWeaponEventBatch.Reset();
	ResetMuzzleFlashOverlayState("level-unload");
	mLastResolvedLightOverlayGeneration = 0;

	ClearRuntimePointLights();
	ClearRuntimeDebugSpheres();
	mSceneLights.ResetLevelState();

	mPendingStaticMapLightingInvalidation = false;
	mAllowStartupMapWorldCorrection = false;
	mAllowStartupMutationRebaseline = false;
	mPendingStartupMutationRebaseline = false;
	mStartupMutationProbe = {};
	mPendingStartupVisibleChunkValidation.clear();
	mRuntimeMutation.ResetLevelLifecycleState();
	mStartupMapWorldCorrectionDeadlineFrame = 0;
	mStartupMutationRebaselineDeadlineFrame = 0;

	mCurrentVisibleChunkWords.clear();
	mCurrentVisibleFlatPlaneWords.clear();
	mSurfaceProbe.Reset();
	mSurfaceProbeFrame = {};
	mDynamicSceneLastFrame = {};
	mRuntimeSpaceLinkLastFrame = {};
	mLastRuntimeLinkTraceState = {};
	mHasRuntimeLinkTraceState = false;
	mRuntimeChunkTranslationHistory.clear();
	mLastStats = {};
	mHasLoggedStats = false;

	mSceneTextures.CacheStats() = {};
	mSceneLights.ResetPersistentDynamicEmissiveHighWaterStats();

	mUsedStaticMapSceneLastFrame = false;
	mUsedDynamicSceneLastFrame = false;
	mGpuSceneHasDynamicOverlay = false;
	mUploadedStaticMapSceneLastFrame = false;
	mBuiltStaticMapSceneASLastFrame = false;
	mBuiltDynamicSceneASLastFrame = false;

	mActiveTlasInstanceCount = 0;
	mBoundStaticPrimitiveCount = 0;
	mBoundDynamicPrimitiveCount = 0;
	mBoundStaticMaterialCount = 0;
	mBoundDynamicMaterialCount = 0;
	mBoundPortalCount = 0;
	mBoundRuntimeLightCount = 0;
	mBoundRuntimeLightTileCountX = 0;
	mBoundRuntimeLightTileCountY = 0;
	mBoundRuntimeLightTileSize = 0;
	mBoundRuntimeLightTileIndexCount = 0;
	mBoundRuntimeLightMaxTileOccupancy = 0;
	mRuntimeLightPayloadCacheValid = false;
	mRuntimeLightPayloadHash = 0;
	mRuntimeLightClusterCacheValid = false;
	mRuntimeLightClusterPayloadHash = 0;
	mRuntimeLightClusterCameraHash = 0;
	mRuntimeLightSceneDataDirty = false;
	mBoundEmissivePrimitiveCount = 0;
	mBoundEmissiveDominantPrimitive = UINT32_MAX;
	mBoundEmissiveDominantTile = 0;
	mBoundEmissiveDominantFlags = 0;
	mBoundEmissiveDominantDataSource = 0;
	mEmissiveSamplingPayloadCacheValid = false;
	mEmissiveSamplingPayloadHash = 0;
	mEmissiveStabilityTraceValid = false;
	mEmissiveStabilityTopologyEpoch = 0;
	mEmissiveStabilityDistributionEpoch = 0;
	mEmissiveStabilityTopologyHash = 0;
	mEmissiveStabilityOrderedKeyHash = 0;
	mEmissiveStabilityLivePowerHash = 0;
	mEmissiveStabilityProposalWeightHash = 0;
	mEmissiveStabilityCdfHash = 0;
	mEmissiveStabilityOrderedKeys.clear();
	mEmissiveStabilityCdf.clear();
	mEmissiveSectorResponsePayloadCacheValid = false;
	mEmissiveSectorResponsePayloadHash = 0;
	mSceneLights.ResetEmissiveSectorResponseCaches();
	mEmissiveTlasInstanceCount = 0;
	mEmissiveTlasStaticInstanceCount = 0;
	mEmissiveTlasDynamicInstanceCount = 0;
	mEmissiveTlasBuildCount = 0;
	mEmissiveTlasInstancePayloadCacheValid = false;
	mEmissiveTlasInstancePayloadHash = 0;
	mBoundEmissiveTotalPower = 0.0f;
	mBoundEmissiveDominantPower = 0.0f;
	mBoundEmissivePrimitiveRecords.clear();
	mSectorLightingPayloadCacheValid = false;
	mSectorLightingPayloadHash = 0;
	mBoundSectorLightSectorCount = 0;
	mBoundSectorLightActiveCount = 0;
	mBoundSectorLightPulsingCount = 0;
	mBoundSectorLightDominantSector = UINT32_MAX;
	mBoundSectorLightDominantContribution = 0.0f;

	if (info.newLevel == nullptr)
	{
		mSceneLights.ResetRuntimePointLights();
		mDebugOverlays.ResetRuntimeDebugSphereIds();
	}
}

void NRIRenderer::OnLevelUnloadComplete(const LevelTransitionInfo& info)
{
	assert(!mMapWorld.valid);
	assert(!mStaticMapScene.valid);
	assert(!mStaticMapScene.texturesResident);
	assert(!mStaticMapScene.buffersResident);
	assert(!mStaticMapScene.accelerationResident);
	assert(mStaticMapScene.chunks.empty());
	assert(!mStaticMapChunkAtlas.valid);
	assert(mStaticMapChunkAtlas.chunks.empty());
	assert(!mStaticSceneResidency.Registry().valid);
	assert(mStaticSceneResidency.Registry().entries.empty());
	assert(mRuntimeMutation.IsCacheEmpty());
	assert(!mPendingStaticMapLightingInvalidation);
	assert(!mSurfaceProbe.Last().valid);
	assert(!mSurfaceProbe.LastLogged().valid);
	assert(mDebugOverlays.Empty());
	assert(mSceneLights.GetManualAnalyticLightCount() == 0);

	if (info.newLevel == nullptr)
	{
		mCurrentVisibleChunkWords.clear();
		mCurrentVisibleFlatPlaneWords.clear();
	}
}

void NRIRenderer::OnLevelLoadBegin(const LevelTransitionInfo& info)
{
	mWeaponEventBatch.Reset();
	nri_scene::ResetPersistentVoxelActorCache("level-load");
	mPersistentVoxels.ResetLevelSchedulingState(
		"level-load",
		(int)nri_ptloadingtrace >= 1 || (bool)nri_voxelstats,
		BuildNRIPersistentVoxelResetServices(*this));
	mVoxelRepresentationPolicy.Reset();

	mMapWorld = {};
	mObservedMapWorldBuildSerial = 0;
	mMapMoverShadow.Reset();
	mMapMoverRigidRoute.Reset();
	mSE29FloorDeformerRoute.Reset();
	mMapMaterialOnlyRoute.Reset();
	mAllowStartupMapWorldCorrection = false;
	mAllowStartupMutationRebaseline = false;
	mPendingStartupMutationRebaseline = false;
	mStartupMutationProbe = {};
	mPendingStartupVisibleChunkValidation.clear();
	mRuntimeMutation.ResetLevelLifecycleState();
	mStartupMapWorldCorrectionDeadlineFrame = 0;
	mStartupMutationRebaselineDeadlineFrame = 0;
	mSurfaceProbe.Reset();
	mSurfaceProbeFrame = {};
	mDynamicSceneLastFrame = {};
	mRuntimeSpaceLinkLastFrame = {};
	mRuntimeChunkTranslationHistory.clear();
	mSceneTextures.CacheStats() = {};
	mSceneLights.ResetPersistentDynamicEmissiveHighWaterStats();
	mLastStats = {};
	mHasLoggedStats = false;
	mLastRuntimeLinkTraceState = {};
	mHasRuntimeLinkTraceState = false;
	mSceneLights.ResetRuntimePointLights();
	mDebugOverlays.ResetRuntimeDebugSphereIds();

	if (info.newLevel == nullptr)
	{
		mCurrentVisibleChunkWords.clear();
		mCurrentVisibleFlatPlaneWords.clear();
	}
}

void NRIRenderer::OnLevelFirstFrameRelease()
{
	NotifyNRIVoxelComputePreloadRuntimeTailReleased(mMapWorld.buildSerial, mFrameIndex);
	const int runtimeCaptureFrames = std::clamp((int)nri_ptvoxelcomputepreloadruntimecaptureframes, 0, 4096);
	if (runtimeCaptureFrames > 0)
	{
		perf_looptraceframes = runtimeCaptureFrames;
		perf_compactframes = runtimeCaptureFrames;
		Printf("PERF pt voxel preload runtime tail capture NRI: build_serial=%llu frame=%u frames=%d compact=1\n",
			(unsigned long long)mMapWorld.buildSerial,
			mFrameIndex,
			runtimeCaptureFrames);
	}
	mPersistentVoxels.ArmPostLoadAdmissionGrace(
		mFrameIndex,
		BuildNRIPersistentVoxelSettingsFromCVars(),
		(int)nri_ptloadingtrace);
	NRIPreloadCoordinator::QueueStrictPreloadFirstFrameReleaseCommand(*this);
}

NRIRenderer::LevelTransitionSnapshot NRIRenderer::BuildLevelTransitionSnapshot() const
{
	LevelTransitionSnapshot snapshot = {};
	snapshot.mapWorldValid = mMapWorld.valid;
	snapshot.mapWorldBuildSerial = mMapWorld.buildSerial;
	snapshot.mapWorldChunkCount = (uint32_t)mMapWorld.chunks.size();
	snapshot.mapWorldSurfaceCount = (uint32_t)mMapWorld.surfaces.size();
	snapshot.staticSceneValid = mStaticMapScene.valid;
	snapshot.staticSceneTexturesResident = mStaticMapScene.texturesResident;
	snapshot.staticSceneBuffersResident = mStaticMapScene.buffersResident;
	snapshot.staticSceneAccelerationResident = mStaticMapScene.accelerationResident;
	snapshot.staticSceneBuildSerial = mStaticMapScene.buildSerial;
	snapshot.staticSceneChunkCount = (uint32_t)mStaticMapScene.chunks.size();
	snapshot.staticSceneMaterialCount = (uint32_t)mStaticMapScene.gpuMaterials.size();
	snapshot.textureCacheCount = mSceneTextures.CacheCount();
	snapshot.skyTextureCacheCount = (uint32_t)mSkyEnvironment.CachedTextures().size();
	const RuntimeMutationCacheStats runtimeMutationCacheStats = mRuntimeMutation.GatherCacheStats();
	snapshot.runtimeMutationChunkCount = mRuntimeMutation.GetCacheChunkCount();
	snapshot.runtimeMutationActiveChunkCount = runtimeMutationCacheStats.activeChunkCount;
	snapshot.runtimeMutationValidChunkCount = runtimeMutationCacheStats.validChunkCount;
	snapshot.residentChunkRegistryValid = mStaticSceneResidency.Registry().valid;
	snapshot.residentChunkRegistryEntryCount = (uint32_t)mStaticSceneResidency.Registry().entries.size();
	snapshot.residentChunkRegistryChunkCount = mStaticSceneResidency.Registry().chunkCount;
	snapshot.residentChunkRegistryActiveChunkCount = mStaticSceneResidency.Registry().activeChunkCount;
	snapshot.residentChunkRegistryMappedChunkCount = mStaticSceneResidency.Registry().mappedChunkCount;
	snapshot.residentChunkRegistryAccelerationResidentChunkCount = mStaticSceneResidency.Registry().accelerationResidentChunkCount;
	snapshot.pendingStaticMapLightingInvalidation = mPendingStaticMapLightingInvalidation;
	const NRISurfaceProbeResult& surfaceProbe = mSurfaceProbe.Last();
	snapshot.surfaceProbeValid = surfaceProbe.valid;
	snapshot.surfaceProbeHit = surfaceProbe.hit;
	snapshot.surfaceProbeWallIndex = surfaceProbe.provenance.wallIndex;
	snapshot.surfaceProbeMapChunkIndex = surfaceProbe.provenance.mapChunkIndex;
	snapshot.transientMuzzleFlashSlotCount = mSceneLights.GetAnalyticLights().transientMuzzleSlotCount;
	snapshot.transientMuzzleFlashActiveCount = mSceneLights.GetAnalyticLights().transientMuzzleActiveCount;
	snapshot.analyticLightCount = (uint32_t)mSceneLights.GetAnalyticLights().activeLights.size();
	snapshot.manualLightCount = mSceneLights.GetManualAnalyticLightCount();
	snapshot.emissiveSurfaceCount = (uint32_t)mSceneLights.GetEmissiveSurfaces().activeSurfaces.size();
	snapshot.activeSectorLightCount = mSceneLights.GetSectorLighting().activeSectorCount;
	snapshot.runtimeDebugSphereCount = mDebugOverlays.GetRuntimeDebugSphereCount();
	snapshot.runtimeTestLightCount = mSceneLights.GetManualAnalyticLightCount();
	const NRIPersistentVoxelStatusSnapshot persistentVoxels = mPersistentVoxels.BuildStatusSnapshot();
	snapshot.persistentVoxelMeshResources = persistentVoxels.meshVariantResourceCount;
	snapshot.persistentVoxelMaterialResources = persistentVoxels.materialVariantResourceCount;
	snapshot.persistentVoxelBatchActors = persistentVoxels.batchActorCount;
	snapshot.persistentVoxelActiveInstances = persistentVoxels.activeInstanceCount;
	snapshot.persistentVoxelInstanceRecords = persistentVoxels.instanceRecordCount;
	snapshot.persistentVoxelAdmissionQueue = persistentVoxels.admissionQueueCount;
	snapshot.persistentVoxelRequiredAdmissionPending = persistentVoxels.requiredAdmissionPendingCount;
	snapshot.persistentVoxelRequiredAdmissionReady = persistentVoxels.requiredAdmissionReadyCount;
	snapshot.persistentVoxelOptionalAdmissionPending = persistentVoxels.optionalAdmissionPendingCount;
	snapshot.persistentVoxelFailedAdmission = persistentVoxels.failedAdmissionCount;
	snapshot.persistentVoxelResidentBytes = persistentVoxels.residentResourceBytes;
	snapshot.persistentVoxelZeroRefBytes = persistentVoxels.zeroRefResourceBytes;
	snapshot.persistentVoxelColdMeshes = persistentVoxels.lastColdMeshCount;
	snapshot.persistentVoxelColdMaterials = persistentVoxels.lastColdMaterialCount;
	snapshot.persistentVoxelColdPrimitives = persistentVoxels.lastColdPrimitiveCount;
	snapshot.persistentVoxelResidencyGeneration = persistentVoxels.residencyGeneration;
	snapshot.persistentVoxelResidencyBuildSerial = persistentVoxels.residencyBuildSerial;
	snapshot.persistentVoxelLastDesired = persistentVoxels.lastDesiredResidencyCount;
	snapshot.persistentVoxelLastDesiredPreload = persistentVoxels.lastDesiredPreloadCount;
	snapshot.persistentVoxelLastDesiredActor = persistentVoxels.lastDesiredActorCount;
	snapshot.persistentVoxelLastGpuReady = persistentVoxels.lastGpuReadyCount;
	snapshot.persistentVoxelLastRetained = persistentVoxels.lastRetainedCount;
	snapshot.persistentVoxelLastQueued = persistentVoxels.lastQueuedCount;
	snapshot.persistentVoxelLastQueuedBytes = persistentVoxels.lastQueuedUploadBytes;
	snapshot.persistentVoxelLastMeshMissing = persistentVoxels.lastMeshMissingCount;
	snapshot.persistentVoxelLastMaterialOnly = persistentVoxels.lastMaterialOnlyCount;
	snapshot.persistentVoxelLastBlasOnly = persistentVoxels.lastBlasOnlyCount;
	snapshot.persistentVoxelLastForced = persistentVoxels.lastForcedCount;
	snapshot.persistentVoxelLastPreferred = persistentVoxels.lastPreferredCount;
	snapshot.sceneInstanceBufferBytes = mSceneInstanceBuffer.memorySize;
	snapshot.visibleChunkBufferBytes = mVisibleChunkBuffer.memorySize;
	snapshot.visibleFlatBufferBytes = mVisibleFlatPlaneBuffer.memorySize;
	snapshot.reprojectionBufferBytes = mReprojectionBuffer.memorySize;
	snapshot.dynamicScratchBufferBytes = mScratchBuffer.memorySize;
	snapshot.worldTlasScratchBufferBytes = mWorldTlasFrameSlots.GetMemoryUsage().scratchBufferBytes;
	return snapshot;
}

void NRIRenderer::TraceStartupMutationProbe(const char* event) const
{
	if ((int)nri_ptloadingtrace < 1 || !(bool)nri_ptloadingmutationbaseline)
	{
		return;
	}

	const StartupMutationProbeState& probe = mStartupMutationProbe;
	const uint32_t deadlineRemaining =
		mAllowStartupMutationRebaseline && mFrameIndex <= mStartupMutationRebaselineDeadlineFrame ?
		mStartupMutationRebaselineDeadlineFrame - mFrameIndex :
		0u;
	Printf(
		"NRI PT startup mutation probe: event=%s level=%s frame=%u allow=%u pending=%u deadline=%u deadline_remaining=%u probe_valid=%u probe_frame=%llu chunks=%u visible_chunks=%u candidates=%u active=%u visible_resident=%u startup_visible=%u unresolved_textures=%u static_animated=%u sector_dirty=%u section_dirty=%u dragged=%u signature_watch=%u background=%u deferred_material=%u deferred_structural=%u detected_material_only=%u dirty_chunks=%u startup_material_only_dirty=%u mutation_cache_chunks=%u\n",
		event != nullptr ? event : "unknown",
		currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)",
		mFrameIndex,
		mAllowStartupMutationRebaseline ? 1u : 0u,
		mPendingStartupMutationRebaseline ? 1u : 0u,
		mStartupMutationRebaselineDeadlineFrame,
		deadlineRemaining,
		probe.valid ? 1u : 0u,
		(unsigned long long)probe.frameIndex,
		probe.chunkCount,
		probe.visibleChunkCount,
		probe.candidateCount,
		probe.candidateActiveReplacementCount,
		probe.candidateVisibleResidentValidationCount,
		probe.candidateStartupVisibleValidationCount,
		probe.candidateUnresolvedAuthoredTextureCount,
		probe.candidateStaticAnimatedSuppressedCount,
		probe.candidateSectorDirtyCount,
		probe.candidateSectionDirtyCount,
		probe.candidateDraggedCount,
		probe.candidateSignatureWatchlistCount,
		probe.candidateBackgroundSweepCount,
		probe.candidateDeferredMaterialRefreshCount,
		probe.candidateDeferredStructuralRebuildCount,
		probe.detectedMaterialOnly ? 1u : 0u,
		probe.dirtyChunkCount,
		probe.startupMaterialOnlyDirtyChunkCount,
		mRuntimeMutation.GetCacheChunkCount());
}

void NRIRenderer::ResetMuzzleFlashOverlayState(const char* reason)
{
	mSceneLights.ResetMuzzleFlashOverlayState(reason, 0, nri_ptdebug > 0);
}

void NRIRenderer::ResetPerfTraceStats()
{
	mLastPerfShellTraceStats = {};
	mLastPerfResourceTraceStats = {};
}

void NRIRenderer::NotePerfBufferUpload(const SceneBufferDebugStats* stats, uint64_t size, bool growth, const char* reason, int uploadKind)
{
	if (!ShouldTracePtPerf() || stats == nullptr)
	{
		return;
	}

	auto& perf = mLastPerfResourceTraceStats;
	if (growth)
	{
		perf.growEvents++;
	}
	else
	{
		perf.overwriteEvents++;
	}

	auto noteBytes = [&](uint32_t& callCount, uint64_t& byteCount)
	{
		callCount++;
		byteCount += size;
	};

	if (stats == &mVertexBufferStats || stats == &mIndexBufferStats || stats == &mPrimitiveBufferStats || stats == &mMaterialBufferStats)
	{
		noteBytes(perf.sceneUploadCalls, perf.sceneUploadBytes);
		int effectiveUploadKind = uploadKind;
		if (effectiveUploadKind < 0)
		{
			if (stats == &mVertexBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Vertex;
			}
			else if (stats == &mIndexBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Index;
			}
			else if (stats == &mPrimitiveBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Primitive;
			}
			else if (stats == &mMaterialBufferStats)
			{
				effectiveUploadKind = ResidentUploadKind_Material;
			}
		}
		switch (effectiveUploadKind)
		{
		case ResidentUploadKind_Vertex: perf.sceneVertexUploadBytes += size; break;
		case ResidentUploadKind_Index: perf.sceneIndexUploadBytes += size; break;
		case ResidentUploadKind_Primitive: perf.scenePrimitiveUploadBytes += size; break;
		case ResidentUploadKind_Material: perf.sceneMaterialUploadBytes += size; break;
		default: break;
		}

		if (reason != nullptr && std::strcmp(reason, "scene_buffer_upload") == 0)
		{
			noteBytes(perf.sceneDynamicUploadCalls, perf.sceneDynamicUploadBytes);
		}
		else if (reason != nullptr && std::strcmp(reason, "resident_chunk_write") == 0)
		{
			noteBytes(perf.sceneResidentChunkUploadCalls, perf.sceneResidentChunkUploadBytes);
		}
		else if (reason != nullptr && std::strcmp(reason, "persistent_voxel_scene_upload") == 0)
		{
			noteBytes(perf.scenePersistentVoxelUploadCalls, perf.scenePersistentVoxelUploadBytes);
		}
		else if (reason != nullptr &&
			(std::strcmp(reason, "persistent_voxel_mesh_vertex") == 0 ||
				std::strcmp(reason, "persistent_voxel_mesh_index") == 0 ||
				std::strcmp(reason, "persistent_voxel_mesh_primitive") == 0 ||
				std::strcmp(reason, "persistent_voxel_material_variant") == 0))
		{
			noteBytes(perf.scenePersistentVoxelVariantUploadCalls, perf.scenePersistentVoxelVariantUploadBytes);
		}
		else if (reason == nullptr)
		{
			noteBytes(perf.sceneStaticRefreshUploadCalls, perf.sceneStaticRefreshUploadBytes);
		}
		else
		{
			noteBytes(perf.sceneOtherUploadCalls, perf.sceneOtherUploadBytes);
		}
	}
	else if (stats == &mEmissivePrimitiveHeaderBufferStats || stats == &mEmissivePrimitiveBufferStats || stats == &mEmissivePrimitiveCdfBufferStats || stats == &mEmissiveMaterialResponseBufferStats || stats == &mEmissiveTlasInstanceBufferStats)
	{
		noteBytes(perf.emissiveUploadCalls, perf.emissiveUploadBytes);
	}
	else
	{
		noteBytes(perf.sceneDataUploadCalls, perf.sceneDataUploadBytes);
	}
}

NRIRendererFrameContext NRIRenderer::BuildFrameContext(int drawmode, bool portal, int debugMode, bool preserveHistory) const
{
	NRIRendererFrameContext context = {};
	context.frameIndex = mFrameIndex;
	context.drawMode = drawmode;
	context.debugMode = debugMode;
	context.portal = portal;
	context.preserveHistory = preserveHistory;
	if (mFrameBuffer != nullptr)
	{
		context.outputWidth = std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.width, 1u);
		context.outputHeight = std::max<uint32_t>((uint32_t)mFrameBuffer->mSceneViewport.height, 1u);
		if (mFrameBuffer->mActiveTarget != nullptr)
		{
			context.targetWidth = mFrameBuffer->mActiveTarget->width;
			context.targetHeight = mFrameBuffer->mActiveTarget->height;
		}
	}
	return context;
}

bool NRIRenderer::PreloadLevelScene(uint32_t outputWidth, uint32_t outputHeight, uint32_t targetWidth, uint32_t targetHeight, bool frameTargetUsed, bool standaloneContextUsed)
{
	NRIPreloadLevelSceneInputs inputs = {};
	inputs.outputWidth = outputWidth;
	inputs.outputHeight = outputHeight;
	inputs.targetWidth = targetWidth;
	inputs.targetHeight = targetHeight;
	inputs.frameTargetUsed = frameTargetUsed;
	inputs.standaloneContextUsed = standaloneContextUsed;
	return NRIPreloadCoordinator::Run(*this, inputs);
}

void NRIRenderer::ResetHistory()
{
	RequestHistoryReset("history-reset", true, true);
}

void NRIRenderer::RequestAutoExposureReset(const char* reason)
{
	const char* safeReason = reason != nullptr && *reason != '\0' ? reason : "unspecified";
	mExposure.RequestReset(safeReason, (uint64_t)mFrameIndex);
	if (nri_ptautoexposurestats)
	{
		const NRIAutoExposureStatus& status = mExposure.GetStatus();
		Printf("NRI PT auto exposure reset: reason=%s frame=%u serial=%llu\n",
			safeReason,
			mFrameIndex,
			(unsigned long long)status.resetSerial);
	}
}

void NRIRenderer::NotifyCameraCut(const char* reason)
{
	RequestHistoryReset((reason != nullptr && *reason != '\0') ? reason : "camera-cut", true, false);
}

void NRIRenderer::SetGuiCaptureState(bool active)
{
	if (mGuiCaptureActive == active)
	{
		return;
	}

	mGuiCaptureActive = active;
	if (nri_ptscenestats)
	{
		const NRIMainUpscalerKind resolvedMain = ResolveMainUpscalerKind(false);
		const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMain, GetSelectedUpscalerMode());
		Printf("NRI PT gui capture: frame=%u active=%s jitter=%s phases=%u\n",
			mFrameIndex,
			mGuiCaptureActive ? "yes" : "no",
			NRIGetTemporalJitterModeName(resolvedMain, mGuiCaptureActive),
			NRIGetTemporalJitterPhaseCount(resolvedMain, resolvedUpscalerMode, mGuiCaptureActive));
	}
}

void NRIRenderer::RequestHistoryReset(const char* reason, bool clearPreviousCameraState, bool clearRuntimeChunkTranslationHistory)
{
	ArmTemporalTraceBudget(reason);
	mResetHistory = true;
	mLastHistoryResetReason = (reason != nullptr && *reason != '\0') ? reason : "unspecified";
	RequestAutoExposureReset(mLastHistoryResetReason.c_str());
	if (clearPreviousCameraState)
	{
		mHasPreviousCameraState = false;
	}
	if (clearRuntimeChunkTranslationHistory)
	{
		mRuntimeChunkTranslationHistory.clear();
	}
}

void NRIRenderer::NoteLightHistoryChange(const char* reason)
{
	ArmTemporalTraceBudget(reason);
	if (ShouldEmitRendererTemporalTraceLogs())
	{
		Printf("NRI PT light change: reason=%s frame=%u reset=no\n",
			(reason != nullptr && *reason != '\0') ? reason : "unspecified",
			mFrameIndex);
	}
}

void NRIRenderer::InvalidateRuntimeLightSceneData()
{
	mBoundRuntimeLightCount = 0;
	mBoundRuntimeLightTileCountX = 0;
	mBoundRuntimeLightTileCountY = 0;
	mBoundRuntimeLightTileSize = 0;
	mBoundRuntimeLightTileIndexCount = 0;
	mBoundRuntimeLightMaxTileOccupancy = 0;
	mRuntimeLightPayloadCacheValid = false;
	mRuntimeLightPayloadHash = 0;
	mRuntimeLightClusterCacheValid = false;
	mRuntimeLightClusterPayloadHash = 0;
	mRuntimeLightClusterCameraHash = 0;
	mRuntimeLightSceneDataDirty = true;
}

void NRIRenderer::PrintRuntimeLightClusterStatus() const
{
	const uint32_t tileCount = mBoundRuntimeLightTileCountX * mBoundRuntimeLightTileCountY;
	const uint32_t centerTileX = mBoundRuntimeLightTileCountX > 0 ? (mBoundRuntimeLightTileCountX - 1) / 2u : 0u;
	const uint32_t centerTileY = mBoundRuntimeLightTileCountY > 0 ? (mBoundRuntimeLightTileCountY - 1) / 2u : 0u;
	uint32_t centerTileCount = 0;
	if (mRuntimeLightTileHeaderBuffer.buffer != nullptr &&
		mBoundRuntimeLightTileCountX > 0 &&
		mBoundRuntimeLightTileCountY > 0)
	{
		void* mapped = mFrameBuffer->mCore.MapBuffer(*mRuntimeLightTileHeaderBuffer.buffer, 0, mRuntimeLightTileHeaderBuffer.usedSize);
		if (mapped != nullptr)
		{
			const auto* headers = reinterpret_cast<const RuntimeLightTileHeaderGpuData*>(mapped);
			const uint32_t centerIndex = centerTileY * mBoundRuntimeLightTileCountX + centerTileX;
			if ((uint64_t)(centerIndex + 1) * sizeof(RuntimeLightTileHeaderGpuData) <= mRuntimeLightTileHeaderBuffer.usedSize)
			{
				centerTileCount = headers[centerIndex].indexCount;
			}
			mFrameBuffer->mCore.UnmapBuffer(*mRuntimeLightTileHeaderBuffer.buffer);
		}
	}

	Printf("NRI PT light clusters: tile_size=%u grid=%ux%u tiles=%u active_lights=%u used_indices=%u max_occupancy=%u center_tile=(%u,%u) center_count=%u debug_mode=%u\n",
		mBoundRuntimeLightTileSize,
		mBoundRuntimeLightTileCountX,
		mBoundRuntimeLightTileCountY,
		tileCount,
		mBoundRuntimeLightCount,
		mBoundRuntimeLightTileIndexCount,
		mBoundRuntimeLightMaxTileOccupancy,
		centerTileX,
		centerTileY,
		centerTileCount,
		nri_diag::PtDebugAnalyticDirect);
}

void NRIRenderer::UpdateNightVisionState()
{
	mNightVisionState = {};

	if (gi == nullptr)
	{
		return;
	}

	RuntimeNightVisionState runtimeState = {};
	if (!gi->GetNightVisionState(&runtimeState) || !runtimeState.available)
	{
		return;
	}

	switch (runtimeState.mode)
	{
	case RuntimeNightVisionMode::Duke:
		mNightVisionState.mode = NRIPTNightVisionMode::Duke;
		break;
	default:
		mNightVisionState.mode = NRIPTNightVisionMode::None;
		break;
	}

	mNightVisionState.viewEligible = runtimeState.viewEligible;
	mNightVisionState.enabled = runtimeState.enabled;
	mNightVisionState.strength01 = runtimeState.strength01;
	mNightVisionState.remainingSeconds = runtimeState.remainingSeconds;
}

void NRIRenderer::PrintSectorLightDump(float radius, uint32_t limit) const
{
	mSceneLights.PrintSectorLightDump(mCurrentCameraPos, NRIGetSectorLightMultiplier(), radius, limit);
}

NRIRenderer::MemoryTelemetry NRIRenderer::GetMemoryTelemetry() const
{
	MemoryTelemetry telemetry = {};
	telemetry.renderWidth = mRenderWidth;
	telemetry.renderHeight = mRenderHeight;
	telemetry.outputWidth = mOutputWidth;
	telemetry.outputHeight = mOutputHeight;

	const auto accumulateTexture = [](const NRITextureResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};
	const auto accumulateBuffer = [](const NRIBufferResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};
	const auto accumulateAs = [](const NRIAccelerationStructureResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};

	for (const NRITextureResource& texture : mFrameTextures)
	{
		accumulateTexture(texture, telemetry.frameTextureBytes);
	}

	accumulateTexture(mSceneTextures.PaletteTexture(), telemetry.sceneTextureBytes);
	for (const NRISceneCachedTexture& texture : mSceneTextures.CachedTextures())
	{
		accumulateTexture(texture.resource, telemetry.sceneTextureBytes);
	}

	for (const NRICachedSkyTexture& texture : mSkyEnvironment.CachedTextures())
	{
		accumulateTexture(texture.resource, telemetry.skyTextureBytes);
	}

	accumulateBuffer(mVertexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mPrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mMaterialBuffer, telemetry.sceneBufferBytes);
	for (const SceneUploadBufferRingSlot& slot : mSceneUploadBufferRing)
	{
		accumulateBuffer(slot.vertexBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.indexBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.primitiveBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.materialBuffer, telemetry.sceneBufferBytes);
		accumulateAs(slot.dynamicBottomLevelAS, telemetry.accelerationStructureBytes);
	}
	accumulateBuffer(mStaticVertexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticPrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mStaticMaterialBuffer, telemetry.sceneBufferBytes);
	const NRIPersistentVoxelMemoryUsage persistentVoxelMemory = mPersistentVoxels.GetMemoryUsage();
	telemetry.sceneBufferBytes += persistentVoxelMemory.sceneBufferBytes;
	telemetry.accelerationStructureBytes += persistentVoxelMemory.accelerationStructureBytes;
	for (const NRIWorldTlasFrameSlot& frameSlot : mWorldTlasFrameSlots.Slots())
	{
		accumulateBuffer(frameSlot.instanceBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(frameSlot.scratchBuffer, telemetry.sceneBufferBytes);
		accumulateAs(frameSlot.accelerationStructure, telemetry.accelerationStructureBytes);
	}
	accumulateBuffer(mSceneInstanceBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mPortalBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightTileHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mRuntimeLightTileIndexBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissivePrimitiveCdfBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissiveMaterialResponseBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissiveTlasInstanceBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mSectorLightHeaderBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mSectorLightBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mReprojectionBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mVisibleChunkBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mVisibleFlatPlaneBuffer, telemetry.sceneBufferBytes);
	for (const SceneDataFrameSlot& slot : mSceneDataFrameRing)
	{
		accumulateBuffer(slot.reprojectionBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.visibleChunkBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.visibleFlatPlaneBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.sceneInstanceBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.portalBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.runtimeLightBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.runtimeLightTileHeaderBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.runtimeLightTileIndexBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.emissivePrimitiveHeaderBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.emissivePrimitiveBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.emissivePrimitiveCdfBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.emissiveMaterialResponseBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.sectorLightHeaderBuffer, telemetry.sceneBufferBytes);
		accumulateBuffer(slot.sectorLightBuffer, telemetry.sceneBufferBytes);
	}
	accumulateBuffer(mScratchBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mResidentStaticBlasScratchBuffer, telemetry.sceneBufferBytes);
	accumulateBuffer(mEmissiveTopLevelScratchBuffer, telemetry.sceneBufferBytes);

	accumulateAs(mEmissiveTopLevelAS, telemetry.accelerationStructureBytes);
	for (const auto& chunk : mStaticMapScene.chunks)
	{
		accumulateAs(chunk.accelerationStructure, telemetry.accelerationStructureBytes);
	}

	telemetry.totalTrackedBytes =
		telemetry.frameTextureBytes +
		telemetry.sceneTextureBytes +
		telemetry.skyTextureBytes +
		telemetry.sceneBufferBytes +
		telemetry.accelerationStructureBytes;
	return telemetry;
}

void NRIRenderer::PrintSwapChainRenderConfig() const
{
	NRISyncLegacyUpscalerConfig(false);
	const NRIMainUpscalerKind requestedMain = GetSelectedMainUpscalerKind();
	const NRIMainUpscalerKind resolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind requestedPost = GetSelectedPostSharpenKind();
	const NRIPostSharpenKind resolvedPost = GetResolvedPostSharpenKindForStatus();
	const nri::UpscalerMode requestedUpscalerMode = GetSelectedUpscalerMode();
	const nri::UpscalerMode resolvedUpscalerMode = NRIResolveUpscalerModeForMain(resolvedMain, requestedUpscalerMode);
	const bool runAppTaa = NRIShouldRunAppTaa(resolvedMain);
	const float requestedRenderScale = std::max(0.33f, std::min((float)nri_renderscale, 1.0f));
	const float resolvedRenderScale = NRIResolveRenderScaleForMain(resolvedMain, requestedUpscalerMode, requestedRenderScale);
	const bool beautyDenoiseActive = !!nri_denoise && resolvedMain != NRIMainUpscalerKind::DLRR;
	NRIPTOutputPolicy outputPolicy = {};
	if (mFrameBuffer != nullptr)
	{
		outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	}
	const bool nisSupported =
		mFrameBuffer != nullptr &&
		mFrameBuffer->mDevice != nullptr &&
		mFrameBuffer->mUpscaler.IsUpscalerSupported(*mFrameBuffer->mDevice, nri::UpscalerType::NIS);
	const bool dlsrSupported = IsMainUpscalerSupported(NRIMainUpscalerKind::DLSR);
	const bool dlrrSupported = IsMainUpscalerSupported(NRIMainUpscalerKind::DLRR);

	Printf("NRI swapchain render config: main_upscaler=%s->%s mode=%s->%s post_sharpen=%s->%s support=NIS:%s DLSS-SR:%s DLRR:%s app_taa=requested:%s active:%s denoise=requested:%s beauty_active:%s nrd=%s render_scale=%.3f->%.3f jitter=%s phases=%u output=%s->%s hdr_swapchain=%s display_hdr=%s tonemap=%s sharpness=%.3f\n",
		NRIGetMainUpscalerName(requestedMain),
		NRIGetMainUpscalerName(resolvedMain),
		NRIGetUpscalerModeName(requestedUpscalerMode),
		NRIGetUpscalerModeName(resolvedUpscalerMode),
		NRIGetPostSharpenName(requestedPost),
		NRIGetPostSharpenName(resolvedPost),
		nisSupported ? "yes" : "no",
		dlsrSupported ? "yes" : "no",
		dlrrSupported ? "yes" : "no",
		nri_pttaa ? "on" : "off",
		runAppTaa ? "on" : "off",
		nri_denoise ? "on" : "off",
		beautyDenoiseActive ? "on" : "off",
		GetNrdDenoiserModeName(GetSelectedNrdDenoiserMode()),
		requestedRenderScale,
		resolvedRenderScale,
		NRIGetTemporalJitterModeName(resolvedMain, mGuiCaptureActive),
		NRIGetTemporalJitterPhaseCount(resolvedMain, resolvedUpscalerMode, mGuiCaptureActive),
		GetNRIPTOutputModeName(outputPolicy.requestedMode),
		GetNRIPTOutputModeName(outputPolicy.resolvedMode),
		outputPolicy.hdrSwapChainActive ? "yes" : "no",
		!outputPolicy.displayInfoAvailable ? "unknown" : (outputPolicy.displayHdrSupported ? "yes" : "no"),
		GetNRIPTTonemapModeName(outputPolicy.tonemapMode),
		(float)nri_sharpness);
}

const char* NRIRenderer::GetExposureDomainName(ExposureDomain domain) const
{
	switch (domain)
	{
	case ExposureDomain::SceneHDR: return "scene_hdr";
	case ExposureDomain::PreExposedHDR: return "pre_exposed_hdr";
	case ExposureDomain::DisplayMappedOutput: return "display_mapped_output";
	default: return "unknown";
	}
}

NRIRenderer::ExposureDomain NRIRenderer::ResolveFrameTextureExposureDomain(FrameTextureSlot slot, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const
{
	switch (slot)
	{
	case FrameTextureSlot::Composed:
	case FrameTextureSlot::TraceTransparentOutput:
	case FrameTextureSlot::PostVolumeOutput:
	case FrameTextureSlot::SrInput:
	case FrameTextureSlot::RrInput:
	case FrameTextureSlot::RrVolumeInput:
		return ExposureDomain::SceneHDR;
	case FrameTextureSlot::TaaHistoryPing:
	case FrameTextureSlot::TaaHistoryPong:
		return NRIShouldRunAppTaa(mainKind) ? ExposureDomain::PreExposedHDR : ExposureDomain::SceneHDR;
	case FrameTextureSlot::VendorOutput:
		return ExposureDomain::SceneHDR;
	case FrameTextureSlot::BloomPyramid0:
	case FrameTextureSlot::BloomPyramid1:
	case FrameTextureSlot::BloomPyramid2:
	case FrameTextureSlot::BloomPyramid3:
	case FrameTextureSlot::BloomPyramid4:
	case FrameTextureSlot::BloomPyramid5:
	case FrameTextureSlot::BloomPyramid6:
	case FrameTextureSlot::BloomPyramid7:
	case FrameTextureSlot::PostBloomOutput:
		return mainKind == NRIMainUpscalerKind::Off && NRIShouldRunAppTaa(mainKind) ? ExposureDomain::PreExposedHDR : ExposureDomain::SceneHDR;
	case FrameTextureSlot::PostSharpenOutput:
		if (postSharpenKind == NRIPostSharpenKind::Off)
		{
			return ExposureDomain::SceneHDR;
		}
		if (mainKind != NRIMainUpscalerKind::Off)
		{
			return ExposureDomain::SceneHDR;
		}
		return NRIShouldRunAppTaa(mainKind) ? ExposureDomain::PreExposedHDR : ExposureDomain::SceneHDR;
	case FrameTextureSlot::Final:
		return ExposureDomain::DisplayMappedOutput;
	default:
		return ExposureDomain::SceneHDR;
	}
}

NRIRenderer::ExposureRoute NRIRenderer::ResolveExposureRoute(FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const
{
	ExposureRoute route = {};
	route.inputDomain = ResolveFrameTextureExposureDomain(inputSlot, mainKind, postSharpenKind);
	route.temporalExposure = GetTemporalExposure(outputPolicy);
	route.presentExposure =
		route.inputDomain == ExposureDomain::PreExposedHDR ?
		1.0f :
		outputPolicy.exposure;
	return route;
}

void NRIRenderer::EmitSelfTestSummary(uint32_t traceFrameIndex, int drawmode, bool portal) const
{
	if (!nri_ptselftest)
	{
		return;
	}

	const PerfShellTraceStats& shell = mLastPerfShellTraceStats;
	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIAutoExposureSettings& exposureSettings = mExposure.GetSettings();
	const NRIAutoExposureStatus& exposureStatus = mExposure.GetStatus();
	const NRITextureResource& final = GetFrameTexture(FrameTextureSlot::Final);
	const bool finalTextureValid = final.texture != nullptr && final.shaderView != nullptr;
	const bool worldActive = gamestate == GS_LEVEL && currentLevel != nullptr;
	const bool gameplayFrame = worldActive && drawmode == DM_MAINVIEW && !portal;
	const uint64_t sceneSignature = nri_scene::HashCombine64(
		nri_scene::HashCombine64(
			nri_scene::HashCombine64(mVertexBuffer.payloadHash, mIndexBuffer.payloadHash),
			mPrimitiveBuffer.payloadHash),
		mSceneInstanceBuffer.payloadHash);
	const uint64_t materialSignature = mMaterialBuffer.payloadHash;
	const uint64_t instanceSignature = mSceneInstanceBuffer.payloadHash;
	const uint64_t skySignature = nri_scene::HashCombine64(mSkyEnvironment.ActiveKey(), (uint64_t)mSkyEnvironment.ActiveState().faceMask);
	const NRIBufferResource& vertexBuffer = mVertexBuffer;
	const NRIBufferResource& indexBuffer = mIndexBuffer;
	const NRIBufferResource& primitiveBuffer = mPrimitiveBuffer;
	const NRIBufferResource& materialBuffer = mMaterialBuffer;
	const uint64_t vertexBytes = vertexBuffer.payloadSize != 0 ? vertexBuffer.payloadSize : vertexBuffer.usedSize;
	const uint64_t indexBytes = indexBuffer.payloadSize != 0 ? indexBuffer.payloadSize : indexBuffer.usedSize;
	const uint64_t primitiveBytes = primitiveBuffer.payloadSize != 0 ? primitiveBuffer.payloadSize : primitiveBuffer.usedSize;
	const uint64_t materialBytes = materialBuffer.payloadSize != 0 ? materialBuffer.payloadSize : materialBuffer.usedSize;

	NRISelfTestSummarySnapshot summary = {};
	summary.traceFrameIndex = traceFrameIndex;
	summary.engineFrameIndex = mFrameIndex;
	summary.mapName = currentLevel != nullptr ? currentLevel->labelName.GetChars() : "none";
	summary.levelName = mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "none";
	summary.graphicsApiName = GetGraphicsApiName(mFrameBuffer->GetLiveAPI());
	summary.worldActive = worldActive;
	summary.menuActive = menuactive != MENU_Off;
	summary.gameplayFrame = gameplayFrame;
	summary.portal = portal;
	summary.drawmode = drawmode;
	summary.route = mDiagnostics.GetSelfTestRouteSnapshot();
	summary.debugMode = (int)GetEffectivePtDebugMode();
	summary.presentKind = summary.route.presenterName;
	summary.renderWidth = mRenderWidth;
	summary.renderHeight = mRenderHeight;
	summary.outputWidth = mOutputWidth;
	summary.outputHeight = mOutputHeight;
	summary.swapchainFormat = (uint32_t)mFrameBuffer->mCreatedSwapChainFormat;
	summary.hdr = outputPolicy.hdrSwapChainActive;
	summary.primitiveCount = shell.activePrimitiveCount;
	summary.materialCount = shell.activeMaterialCount;
	summary.sceneInstanceCount = shell.sceneInstanceCount;
	summary.staticInstanceCount = shell.sceneInstanceStaticCount;
	summary.dynamicInstanceCount = shell.sceneInstanceDynamicCount;
	summary.persistentVoxelInstanceCount = shell.sceneInstancePersistentVoxelCount;
	summary.emissiveInstanceCount = mBoundEmissivePrimitiveCount;
	summary.staticSceneUploadThisFrame = mUploadedStaticMapSceneLastFrame ? 1u : 0u;
	summary.staticSceneAsBuildThisFrame = mBuiltStaticMapSceneASLastFrame ? 1u : 0u;
	summary.runtimeVoxelOnboardingAdmitted = shell.persistentVoxelOnboardingAdmittedCount;
	summary.runtimeVoxelTexturePrewarmDeferred = shell.persistentVoxelTexturePrewarmDeferredCount;
	summary.vertexCount = mVertexBuffer.stride != 0 ? (uint32_t)(vertexBytes / mVertexBuffer.stride) : 0u;
	summary.indexCount = mIndexBuffer.stride != 0 ? (uint32_t)(indexBytes / mIndexBuffer.stride) : 0u;
	summary.vertexBytes = vertexBytes;
	summary.indexBytes = indexBytes;
	summary.primitiveBytes = primitiveBytes;
	summary.materialBytes = materialBytes;
	summary.instanceBytes = mSceneInstanceBuffer.payloadSize != 0 ? mSceneInstanceBuffer.payloadSize : mSceneInstanceBuffer.usedSize;
	summary.sceneSignature = sceneSignature;
	summary.materialSignature = materialSignature;
	summary.instanceSignature = instanceSignature;
	summary.skySignature = skySignature;
	summary.skyMode = GetSkyModeName(mSkyEnvironment.ActiveState().mode);
	summary.skySource = GetSkySourceTypeName(mSkyEnvironment.ActiveState().sourceType);
	summary.skyKey = mSkyEnvironment.ActiveKey();
	summary.skyBrightness = mSkyEnvironment.ActiveState().brightness;
	summary.skyAction = mSkyEnvironment.HasTracedState() ? "traced" : "untraced";
	summary.autoExposure = exposureSettings.enabled;
	summary.exposureTexture = mExposure.HasExposureStateTextures();
	summary.exposure = outputPolicy.exposure;
	summary.targetExposure = exposureStatus.targetExposure;
	summary.adaptedExposure = exposureStatus.adaptedExposure;
	summary.meteredLogLuminance = exposureStatus.meteredLogLuminance;
	summary.exposureStatsValid = exposureStatus.debugValid;
	summary.exposureStatsFrame = exposureStatus.debugFrameIndex;
	summary.finalValid = finalTextureValid;
	summary.exposureReason = exposureSettings.enabled ? "ok" : "disabled";
	mDiagnostics.EmitSelfTestSummary(summary);
}

void NRIRenderer::PrintTemporalStatus() const
{
	NRISyncLegacyUpscalerConfig(false);
	const auto buildTextureSnapshot = [this](FrameTextureSlot slot)
	{
		const NRITextureResource& texture = GetFrameTexture(slot);
		NRITextureStatusSnapshot snapshot = {};
		snapshot.slotName = GetFrameTextureSlotName(slot);
		snapshot.width = texture.width;
		snapshot.height = texture.height;
		snapshot.access = (uint32_t)texture.state.access;
		snapshot.layout = (uint32_t)texture.state.layout;
		snapshot.stages = (uint32_t)texture.state.stages;
		return snapshot;
	};

	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const NRIMainUpscalerKind requestedMain = GetSelectedMainUpscalerKind();
	const NRIMainUpscalerKind resolvedMain = GetResolvedMainUpscalerKindForStatus();
	const NRIPostSharpenKind requestedPost = GetSelectedPostSharpenKind();
	const NRIPostSharpenKind resolvedPost = GetResolvedPostSharpenKindForStatus();
	const bool runAppTaa = NRIShouldRunAppTaa(resolvedMain);
	const float exposure = GetTemporalExposure(outputPolicy);
	const float exposureStops = std::log2(std::max(exposure, 0.125f));
	const FrameTextureSlot presentSlot = mUseUpscaledInFinal ? mUpscaledInputSlot : mHistoryOutputSlot;
	const ExposureRoute exposureRoute = ResolveExposureRoute(presentSlot, outputPolicy, resolvedMain, resolvedPost);
	const NRIAutoExposureSettings& autoExposureSettings = mExposure.GetSettings();
	const NRITextureResource* autoExposureStateTexture = mExposure.GetExposureStateTexture(mFrameIndex & 1u);
	const bool autoExposureTextureValid =
		autoExposureStateTexture != nullptr &&
		autoExposureStateTexture->shaderView != nullptr;
	const bool autoExposureTaaApply =
		runAppTaa &&
		autoExposureSettings.enabled &&
		autoExposureTextureValid;

	NRITemporalStatusSnapshot snapshot = {};
	snapshot.debugMode = (int)nri_ptdebug;
	snapshot.requestedMainUpscaler = NRIGetMainUpscalerName(requestedMain);
	snapshot.resolvedMainUpscaler = NRIGetMainUpscalerName(resolvedMain);
	snapshot.requestedPostSharpen = NRIGetPostSharpenName(requestedPost);
	snapshot.resolvedPostSharpen = NRIGetPostSharpenName(resolvedPost);
	snapshot.taa = !!nri_pttaa;
	snapshot.guiCapture = mGuiCaptureActive;
	snapshot.lastDebugMode = mLastDebugMode;
	snapshot.lastMainUpscaler = NRIGetMainUpscalerName(mLastTemporalHistoryMainUpscaler);
	snapshot.lastPostSharpen = NRIGetPostSharpenName(mLastTemporalPostSharpen);
	snapshot.resetHistory = mResetHistory;
	snapshot.previousCamera = mHasPreviousCameraState;
	snapshot.historyInput = buildTextureSnapshot(mHistoryInputSlot);
	snapshot.historyOutput = buildTextureSnapshot(mHistoryOutputSlot);
	snapshot.presentSlotName = GetFrameTextureSlotName(presentSlot);
	snapshot.upscaledSlotName = GetFrameTextureSlotName(mUpscaledInputSlot);
	snapshot.useUpscaled = mUseUpscaledInFinal;
	snapshot.historyDomain = GetExposureDomainName(ResolveFrameTextureExposureDomain(mHistoryOutputSlot, resolvedMain, resolvedPost));
	snapshot.presentDomain = GetExposureDomainName(exposureRoute.inputDomain);
	snapshot.temporalExposure = exposure;
	snapshot.presentExposure = exposureRoute.presentExposure;
	snapshot.exposureStops = exposureStops;
	snapshot.resetThresholdStops = NRI_TAA_EXPOSURE_RESET_THRESHOLD_STOPS;
	snapshot.autoExposure = autoExposureSettings.enabled;
	snapshot.exposureTexture = autoExposureTextureValid;
	snapshot.taaApply = autoExposureTaaApply;
	PrintNRITemporalStatusSnapshot(snapshot);
}

void NRIRenderer::ArmTemporalTraceBudget(const char* reason)
{
	if (!nri_pttemporaltrace)
	{
		return;
	}

	if ((int)nri_pttraceframes >= NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT)
	{
		return;
	}

	nri_pttraceframes = NRI_TEMPORAL_TRACE_REARM_FRAME_COUNT;
	const NRIMainUpscalerKind resolvedMain = ResolveMainUpscalerKind(false);
	const NRIPostSharpenKind resolvedPost = ResolvePostSharpenKind(false);
	Printf("NRI PT temporal trace: armed=%d reason=%s frame=%u debug=%d resolved_main=%s resolved_post=%s\n",
		(int)nri_pttraceframes,
		reason != nullptr ? reason : "unspecified",
		mFrameIndex,
		(int)nri_ptdebug,
		NRIGetMainUpscalerName(resolvedMain),
		NRIGetPostSharpenName(resolvedPost));
}

void NRIRenderer::TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const
{
	if (!ShouldEmitRendererTemporalTraceLogs())
	{
		return;
	}

	const auto buildTextureSnapshot = [this](FrameTextureSlot slot)
	{
		const NRITextureResource& texture = GetFrameTexture(slot);
		NRITextureStatusSnapshot snapshot = {};
		snapshot.slotName = GetFrameTextureSlotName(slot);
		snapshot.width = texture.width;
		snapshot.height = texture.height;
		snapshot.access = (uint32_t)texture.state.access;
		snapshot.layout = (uint32_t)texture.state.layout;
		snapshot.stages = (uint32_t)texture.state.stages;
		return snapshot;
	};

	const FrameTextureSlot resolvedSecondarySlot = secondarySlot == FrameTextureSlot::Count ? mHistoryOutputSlot : secondarySlot;
	const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
	const ExposureRoute primaryExposureRoute = ResolveExposureRoute(primarySlot, outputPolicy, resolvedMainUpscaler, resolvedPostSharpen);
	const ExposureRoute secondaryExposureRoute = ResolveExposureRoute(resolvedSecondarySlot, outputPolicy, resolvedMainUpscaler, resolvedPostSharpen);
	NRITemporalTraceSnapshot snapshot = {};
	snapshot.stage = stage != nullptr ? stage : "unknown";
	snapshot.frameIndex = mFrameIndex;
	snapshot.debugMode = (int)nri_ptdebug;
	snapshot.resolvedMainUpscaler = NRIGetMainUpscalerName(resolvedMainUpscaler);
	snapshot.resolvedPostSharpen = NRIGetPostSharpenName(resolvedPostSharpen);
	snapshot.runAppTaa = runAppTaa;
	snapshot.guiCapture = mGuiCaptureActive;
	snapshot.primaryDomain = GetExposureDomainName(primaryExposureRoute.inputDomain);
	snapshot.secondaryDomain = GetExposureDomainName(secondaryExposureRoute.inputDomain);
	snapshot.temporalExposure = primaryExposureRoute.temporalExposure;
	snapshot.primaryPresentExposure = primaryExposureRoute.presentExposure;
	snapshot.secondaryPresentExposure = secondaryExposureRoute.presentExposure;
	snapshot.resetHistory = mResetHistory;
	snapshot.resetReason = mLastHistoryResetReason.c_str();
	snapshot.previousCamera = mHasPreviousCameraState;
	snapshot.historyInput = buildTextureSnapshot(mHistoryInputSlot);
	snapshot.historyOutput = buildTextureSnapshot(mHistoryOutputSlot);
	snapshot.primary = buildTextureSnapshot(primarySlot);
	snapshot.secondary = buildTextureSnapshot(resolvedSecondarySlot);
	snapshot.useUpscaled = mUseUpscaledInFinal;
	PrintNRITemporalTraceSnapshot(snapshot);
}

void NRIRenderer::PrintPortalTraversalStatus() const
{
	NRIPortalTraversalStatusSnapshot snapshot = {};
	if (!mMapWorld.valid)
	{
		PrintNRIPortalTraversalStatusSnapshot(snapshot);
		return;
	}

	snapshot.available = true;
	snapshot.depth = ClampTraceBounceCount((int)nri_ptportaldepth, 8u);
	snapshot.reflective = CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_REFLECTIVE);
	snapshot.transfer = CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_SPACE_TRANSFER);
	snapshot.runtimeBound = CountPortalTraversalClass(mMapWorld, NRI_PORTAL_TRAVERSAL_CLASS_RUNTIME_BOUND);
	snapshot.hittableSurfaces = mMapWorld.stats.portalSurfaceCount;
	snapshot.pendingPlanePortals = CountPendingPlanePortals(mMapWorld);
	PrintNRIPortalTraversalStatusSnapshot(snapshot);
}

void NRIRenderer::PrintResidentMapChunkRegistryStatus() const
{
	NRIResidentMapChunkRegistryStatusSnapshot snapshot = {};
	if (!mStaticSceneResidency.Registry().valid)
	{
		PrintNRIResidentMapChunkRegistryStatusSnapshot(snapshot);
		return;
	}

	snapshot.available = true;
	snapshot.buildSerial = mStaticSceneResidency.Registry().buildSerial;
	snapshot.chunkCount = mStaticSceneResidency.Registry().chunkCount;
	snapshot.activeChunkCount = mStaticSceneResidency.Registry().activeChunkCount;
	snapshot.mappedChunkCount = mStaticSceneResidency.Registry().mappedChunkCount;
	snapshot.accelerationResidentChunkCount = mStaticSceneResidency.Registry().accelerationResidentChunkCount;
	snapshot.animatedCandidateChunkCount = mStaticSceneResidency.Registry().animatedCandidateChunkCount;
	snapshot.animatedRefreshSuppressedChunkCount = mStaticSceneResidency.Registry().animatedRefreshSuppressedChunkCount;

	const NRIRuntimeMutationSettings runtimeMutationSettings = BuildNRIRuntimeMutationSettingsFromCVars();
	const float nearDistance = runtimeMutationSettings.nearDistance;
	const float nearDistanceSquared = nearDistance * nearDistance;
	const nri_scene::PTMapChunk* sampleChunk = nullptr;
	snapshot.nearDistance = nearDistance;
	snapshot.mapWorldChunkCount = (uint32_t)mMapWorld.chunks.size();
	const auto computeChunkDistanceSquared = [&](const nri_scene::PTMapChunk& chunk, float& outDistanceSquared) -> bool
	{
		if (!chunk.bounds.valid)
		{
			outDistanceSquared = 0.0f;
			return false;
		}

		outDistanceSquared = 0.0f;
		for (int axis = 0; axis < 3; ++axis)
		{
			float distance = 0.0f;
			if (mCurrentCameraPos[axis] < chunk.bounds.min[axis])
			{
				distance = chunk.bounds.min[axis] - mCurrentCameraPos[axis];
			}
			else if (mCurrentCameraPos[axis] > chunk.bounds.max[axis])
			{
				distance = mCurrentCameraPos[axis] - chunk.bounds.max[axis];
			}
			outDistanceSquared += distance * distance;
		}
		return true;
	};
	for (const nri_scene::PTMapChunk& chunk : mMapWorld.chunks)
	{
		float distanceSquared = 0.0f;
		const bool boundsValid = computeChunkDistanceSquared(chunk, distanceSquared);
		if (boundsValid)
		{
			snapshot.boundsValidCount++;
		}
		else
		{
			snapshot.boundsInvalidCount++;
		}

		const bool visible = IsChunkMarkedVisible(mCurrentVisibleChunkWords, chunk.chunkIndex);
		if (visible)
		{
			snapshot.visibleCount++;
		}
		else if (!boundsValid)
		{
			snapshot.invisibleUnknownCount++;
		}
		else if (distanceSquared <= nearDistanceSquared)
		{
			snapshot.invisibleNearCount++;
		}
		else
		{
			snapshot.invisibleFarCount++;
		}

		if (sampleChunk == nullptr && boundsValid)
		{
			sampleChunk = &chunk;
			snapshot.sampleDistance = sqrtf(distanceSquared);
			snapshot.sampleTier =
				visible ? "visible" :
				(distanceSquared <= nearDistanceSquared ? "near" : "far");
		}
	}
	if (sampleChunk != nullptr)
	{
		snapshot.sampleChunkIndex = sampleChunk->chunkIndex;
		snapshot.sampleCenter[0] = sampleChunk->bounds.center[0];
		snapshot.sampleCenter[1] = sampleChunk->bounds.center[1];
		snapshot.sampleCenter[2] = sampleChunk->bounds.center[2];
		snapshot.sampleRadius = sampleChunk->bounds.radius;
	}
	PrintNRIResidentMapChunkRegistryStatusSnapshot(snapshot);
}

bool NRIRenderer::UploadPersistentVoxelArenaMaterialBuffers(
	const std::vector<nri_scene::MaterialData>& materials,
	bool validateActiveMaterialPayloads)
{
	if (!mPersistentVoxels.HasValidBatch())
	{
		return true;
	}
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialMs);

	NRIPersistentVoxelMaterialUploadServices services = {};
	services.user = this;
	services.ensureMaterialArenaBuffer = [](void* user, NRIBufferResource& resource, uint64_t sizeBytes) -> bool
	{
		return static_cast<NRIRenderer*>(user)->EnsureResidentArenaBuffer(
			resource,
			sizeBytes,
			sizeof(nri_scene::MaterialData),
			nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess());
	};
	services.stageMaterialRanges = [](
		void* user,
		const NRIBufferResource& targetBuffer,
		const std::vector<RuntimeMutationResidentUploadRange>& ranges,
		const uint8_t* data,
		uint64_t availableBytes) -> bool
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		return renderer->StageResidentMaterialUploadRanges(
			targetBuffer,
			ranges,
			data,
			availableBytes,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatches,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchRanges,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchBarrierCommands,
			renderer->mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchCopyCommands);
	};
	services.noteMaterialUpload = [](void* user, uint64_t sizeBytes)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		renderer->NotePerfBufferUpload(
			&renderer->mMaterialBufferStats,
			sizeBytes,
			false,
			"persistent_voxel_material_variant",
			ResidentUploadKind_Material);
	};

	NRIPersistentVoxelMaterialUploadStats uploadStats = {};
	const bool uploaded = mPersistentVoxels.UploadArenaMaterialBuffers(
		materials,
		services,
		mFrameIndex,
		validateActiveMaterialPayloads,
		(bool)nri_voxelstats,
		uploadStats);

	auto& persistentVoxelDomain =
		mLastPerfShellTraceStats.sceneSelectBufferUploadDomains[(size_t)SceneBufferUploadDomain::PersistentVoxelMaterial];
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialRequestedBytes += uploadStats.requestedBytes;
	if (uploadStats.layoutInvalidatedResources != 0 && ((int)nri_ptvoxelcomputetrace > 0 || (int)perf_looptraceframes > 0))
	{
		Printf("PERF pt voxel material layout NRI: frame=%u action=republish reason=texture-table-relayout resources=%u requested_bytes=%llu uploaded_bytes=%llu\n",
			mFrameIndex,
			uploadStats.layoutInvalidatedResources,
			(unsigned long long)uploadStats.requestedBytes,
			(unsigned long long)uploadStats.uploadedBytes);
	}
	if ((uploadStats.actorMaterialRebinds != 0 || uploadStats.activeHashMisses != 0) &&
		((int)nri_ptvoxelcomputetrace > 0 || (int)perf_looptraceframes > 0))
	{
		Printf("PERF pt voxel material coherence NRI: frame=%u active_validated=%u active_hash_misses=%u actor_rebinds=%u uploads=%u requested_bytes=%llu uploaded_bytes=%llu\n",
			mFrameIndex,
			uploadStats.activeValidatedResources,
			uploadStats.activeHashMisses,
			uploadStats.actorMaterialRebinds,
			uploadStats.uploads,
			(unsigned long long)uploadStats.requestedBytes,
			(unsigned long long)uploadStats.uploadedBytes);
	}
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialUploads += uploadStats.uploads;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialDirtyBytes += uploadStats.dirtyBytes;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchRejects += uploadStats.batchRejects;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialBatchGapBytes += uploadStats.batchGapBytes;
	mLastPerfShellTraceStats.sceneSelectBufferUploadPersistentVoxelMaterialUploadedBytes += uploadStats.uploadedBytes;
	persistentVoxelDomain.payloadBytes += uploadStats.domainPayloadBytes;
	persistentVoxelDomain.materialPayloadBytes += uploadStats.domainMaterialPayloadBytes;
	persistentVoxelDomain.hashChecks += uploadStats.domainHashChecks;
	persistentVoxelDomain.hashMisses += uploadStats.domainHashMisses;
	persistentVoxelDomain.uploadedBytes += uploadStats.domainUploadedBytes;
	persistentVoxelDomain.materialUploadedBytes += uploadStats.domainMaterialUploadedBytes;
	return uploaded;
}

const char* NRIRenderer::GetAvailabilityReason() const
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mDevice == nullptr)
	{
		return "renderer device is not initialized";
	}

	const nri::DeviceDesc& deviceDesc = mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice);
	if (deviceDesc.tiers.rayTracing == 0)
	{
		return "required ray tracing capability is unavailable on this device/API";
	}

	const size_t requiredRootConstantSize = std::max({ sizeof(NRITraceSceneConstants), sizeof(NRITemporalConstants), sizeof(NRIPresentConstants), sizeof(NRIExposureConstants) });
	if (deviceDesc.pipelineLayout.rootConstantMaxSize < requiredRootConstantSize ||
		deviceDesc.pipelineLayout.rootDescriptorMaxNum < 1 ||
		deviceDesc.pipelineLayout.descriptorSetMaxNum < 5)
	{
		return "device pipeline layout limits are below the NRI PT backend requirements";
	}

	if (const char* sceneTextureLimitReason = GetSceneTextureDescriptorLimitFailureReason(deviceDesc))
	{
		return sceneTextureLimitReason;
	}

	return "path tracing is unavailable";
}

bool NRIRenderer::RefreshPathTracingAvailability()
{
	return CheckPathTracingSupport();
}

bool NRIRenderer::CheckPathTracingSupport()
{
	mPathTracingSupported = mFrameBuffer != nullptr && mFrameBuffer->mDevice != nullptr;
	if (!mPathTracingSupported)
	{
		return false;
	}

	const nri::DeviceDesc& deviceDesc = mFrameBuffer->mCore.GetDeviceDesc(*mFrameBuffer->mDevice);
	const size_t requiredRootConstantSize = std::max({ sizeof(NRITraceSceneConstants), sizeof(NRITemporalConstants), sizeof(NRIPresentConstants), sizeof(NRIExposureConstants) });
	if (deviceDesc.tiers.rayTracing == 0 ||
		deviceDesc.pipelineLayout.rootConstantMaxSize < requiredRootConstantSize ||
		deviceDesc.pipelineLayout.rootDescriptorMaxNum < 1 ||
		deviceDesc.pipelineLayout.descriptorSetMaxNum < 5)
	{
		mPathTracingSupported = false;
		LogFallback(GetAvailabilityReason());
	}
	else if (const char* sceneTextureLimitReason = GetSceneTextureDescriptorLimitFailureReason(deviceDesc))
	{
		mPathTracingSupported = false;
		LogFallback(sceneTextureLimitReason);
	}

	return mPathTracingSupported;
}

void NRIRenderer::RefreshMapWorld()
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.mapWorldMs);
	const uint64_t pendingBuildSerial = nri_scene::GetPendingLevelGeometryBuildSerial();
	const bool levelChanged = mMapWorld.level != currentLevel;
	if (levelChanged)
	{
		RequestHistoryReset("map-load", true, true);
		mSceneTextures.CacheStats() = {};
		mSceneLights.ResetPersistentDynamicEmissiveHighWaterStats();
		mRuntimeMutation.ResetLevelHighWaterStats();
	}
	const bool needsBuild = !mMapWorld.valid || levelChanged || pendingBuildSerial != mObservedMapWorldBuildSerial;
	if (!needsBuild)
	{
		if (mAllowStartupMapWorldCorrection && mFrameIndex > mStartupMapWorldCorrectionDeadlineFrame)
		{
			mAllowStartupMapWorldCorrection = false;
		}
		return;
	}

	ResetPersistentDynamicEmissiveCache();
	mAllowStartupMapWorldCorrection = true;
	mStartupMapWorldCorrectionDeadlineFrame = mFrameIndex + 8u;
	mAllowStartupMutationRebaseline = false;
	mPendingStartupMutationRebaseline = false;
	mStartupMutationProbe = {};
	mStartupMutationRebaselineDeadlineFrame = 0;

	nri_scene::PTMapWorld world;
	nri_scene::PTMapBuildOptions mapBuildOptions = {};
	if (!nri_scene::BuildMapWorld(world, mapBuildOptions))
	{
		if (pendingBuildSerial != mObservedMapWorldBuildSerial || levelChanged)
		{
			Printf(TEXTCOLOR_RED "NRI PT map world: authoritative level-load build failed for %s.\n",
				currentLevel != nullptr ? currentLevel->labelName.GetChars() : "(none)");
		}
		mMapWorld.Reset();
		mMapWorld.level = currentLevel;
		mMapMoverShadow.Reset();
		mMapMoverRigidRoute.Reset();
		mSE29FloorDeformerRoute.Reset();
		mMapMaterialOnlyRoute.Reset();
		mObservedMapWorldBuildSerial = pendingBuildSerial;
		mPendingStartupVisibleChunkValidation.clear();
		mRuntimeMutation.ResetForMapWorldBuildFailure();
		mAllowStartupMapWorldCorrection = false;
		mStartupMapWorldCorrectionDeadlineFrame = 0;
		mAllowStartupMutationRebaseline = false;
		mPendingStartupMutationRebaseline = false;
		mStartupMutationProbe = {};
		mStartupMutationRebaselineDeadlineFrame = 0;
		return;
	}

	mMapWorld = std::move(world);
	mObservedMapWorldBuildSerial = pendingBuildSerial;
	mPendingStartupVisibleChunkValidation.clear();
	mPendingStartupVisibleChunkValidation.resize(mMapWorld.chunks.size(), 0u);
	mRuntimeMutation.PrepareStartupBaseline(mMapWorld.buildSerial, (uint32_t)mMapWorld.chunks.size());
	mAllowStartupMutationRebaseline = false;
	mPendingStartupMutationRebaseline = false;
	mStartupMutationProbe = {};
	mStartupMutationRebaselineDeadlineFrame = 0;
	const auto& stats = mMapWorld.stats;
	Printf("NRI PT map world built: level=%s build_serial=%llu chunks=%u surfaces=%u walls=%u flats=%u portals=%u skies=%u tris=%u\n",
		mMapWorld.level != nullptr ? mMapWorld.level->labelName.GetChars() : "(none)",
		(unsigned long long)mMapWorld.buildSerial,
		stats.chunkCount,
		stats.surfaceCount,
		stats.wallSurfaceCount,
		stats.flatSurfaceCount,
		stats.portalSurfaceCount,
		stats.skySurfaceCount,
		stats.triangleCount);
}

SceneLightSystem::RuntimeLightClusterBuildInput NRIRenderer::BuildRuntimeLightClusterInput() const
{
	SceneLightSystem::RuntimeLightClusterBuildInput input = {};
	input.renderWidth = mRenderWidth;
	input.renderHeight = mRenderHeight;
	input.tileSize = NRI_RUNTIME_LIGHT_TILE_SIZE;
	input.maxRuntimeLights = NRI_MAX_RUNTIME_POINT_LIGHTS;
	Copy3(mCurrentCameraPos, input.currentCameraPos);
	Copy3(mCurrentCameraForward, input.currentCameraForward);
	Copy3(mCurrentCameraRight, input.currentCameraRight);
	Copy3(mCurrentCameraUp, input.currentCameraUp);
	input.tanHalfFovX = mCurrentTanHalfFovX;
	input.tanHalfFovY = mCurrentTanHalfFovY;
	return input;
}











bool NRIRenderer::EnsureAutoExposureResources(const NRIAutoExposureSettings& settings)
{
	return EnsureNRIRendererAutoExposureResources(*this, settings);
}

void NRIRenderer::DestroyAutoExposureResources()
{
	DestroyNRIRendererAutoExposureResources(*this);
}

bool NRIRenderer::UpdateAutoExposureDescriptorSets(FrameTextureSlot sourceSlot)
{
	return UpdateNRIRendererAutoExposureDescriptorSets(*this, (uint32_t)sourceSlot);
}

bool NRIRenderer::DispatchAutoExposure(FrameTextureSlot sourceSlot)
{
	return DispatchNRIRendererAutoExposure(*this, (uint32_t)sourceSlot);
}

void NRIRenderer::CopyAutoExposureStatsForReadback(uint64_t frameNumber)
{
	CopyNRIRendererAutoExposureStatsForReadback(*this, frameNumber);
}

void NRIRenderer::ReadbackAutoExposureStats()
{
	ReadbackNRIRendererAutoExposureStats(*this);
}



bool NRIRenderer::BindSceneRootDescriptors()
{
	const NRIWorldTlasFrameSlot& frameSlot = GetCurrentWorldTlasFrameSlot();
	if (!frameSlot.publicationValid ||
		frameSlot.accelerationStructure.accelerationStructure == nullptr ||
		frameSlot.accelerationStructure.descriptor == nullptr ||
		(mMapWorld.valid && frameSlot.publishedMapEpoch != mMapWorld.buildSerial) ||
		(mStaticMapScene.valid && frameSlot.publishedBuildEpoch != mStaticMapScene.buildSerial))
	{
		return false;
	}

	mFrameBuffer->mCore.CmdSetRootDescriptor(*mFrameBuffer->mCommandBuffer, { 0, frameSlot.accelerationStructure.descriptor, 0, nri::BindPoint::COMPUTE });
	return true;
}

uint32_t NRIRenderer::CountPotentialOutstandingQueuedFrames() const
{
	if (mFrameBuffer == nullptr)
	{
		return 0;
	}

	uint32_t count = 0;
	for (uint32_t i = 0; i < (uint32_t)mFrameBuffer->mQueuedFrames.size(); ++i)
	{
		if (i == mFrameBuffer->mCurrentQueuedFrameIndex)
		{
			continue;
		}

		const auto& queuedFrame = mFrameBuffer->mQueuedFrames[i];
		if (queuedFrame.hasSubmittedWork && queuedFrame.lastSubmittedFenceValue != 0)
		{
			count++;
		}
	}

	return count;
}

uint32_t NRIRenderer::GetCurrentQueuedFrameIndex() const
{
	if (mFrameBuffer == nullptr || mFrameBuffer->mQueuedFrames.empty())
	{
		return 0;
	}

	return std::min<uint32_t>(mFrameBuffer->mCurrentQueuedFrameIndex, (uint32_t)mFrameBuffer->mQueuedFrames.size() - 1u);
}

NRISE29FloorDeformerRouteFrameStats NRIRenderer::GetSE29FloorDeformerRouteFrameStats() const
{
	return mSE29FloorDeformerRoute.GetFrameStats();
}

NRIMapMaterialOnlyRouteFrameStats NRIRenderer::GetMapMaterialOnlyRouteFrameStats() const
{
	return mMapMaterialOnlyRoute.GetFrameStats();
}

nri_scene::PTMapMaterialStateVariantStats NRIRenderer::GetMapMaterialVariantStats() const
{
	return mMapMaterialOnlyRoute.GetVariantStats();
}
