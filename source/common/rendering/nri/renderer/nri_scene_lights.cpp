#include "nri_scene_lights.h"
#include "nri_cvars.h"

#include "nri_actor_sprite_diagnostics.h"
#include "nri_diagnostic_names.h"
#include "nri_renderer.h"
#include "nri_render_geometry_helpers.h"
#include "nri_runtime_mutation_trace.h"
#include "nri_sky_environment.h"
#include "nri_static_scene.h"
#include "../scene/nri_hash.h"
#include "../system/nri_renderdevice.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "coreactor.h"
#include "gamefuncs.h"
#include "hw_voxels.h"
#include "maptypes.h"
#include "palette.h"
#include "printf.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>


namespace
{
	constexpr uint32_t NRI_MAX_EMISSIVE_SURFACES_FOR_REFRESH = 4096u;

	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	float ResolveAnalyticEmitterRadius(const SceneLightSystem::SceneAnalyticLight& light)
	{
		const float authoredRadius = std::max(light.emitterRadius, 0.0f);
		const float fallbackRadius = std::max((float)nri_ptanalyticsoftshadowradius, 0.0f);
		const float resolvedRadius = authoredRadius > 0.0f ? authoredRadius : fallbackRadius;
		return std::min(resolvedRadius, std::max(light.radius, 0.0f));
	}
}

bool NRIRenderer::AddRuntimePointLight(const float position[3], const float color[3], float intensity, float radius, uint32_t& outId)
{
	if (!mSceneLights.AddRuntimePointLight(position, color, intensity, radius, NRI_MAX_RUNTIME_POINT_LIGHTS, outId))
	{
		return false;
	}
	InvalidateRuntimeLightSceneData();
	NoteLightHistoryChange("runtime-light-change");
	return true;
}

bool NRIRenderer::UpdateRuntimePointLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius)
{
	if (!mSceneLights.UpdateRuntimePointLight(id, position, color, intensity, radius))
	{
		return false;
	}

	InvalidateRuntimeLightSceneData();
	NoteLightHistoryChange("runtime-light-change");
	return true;
}

bool NRIRenderer::RemoveRuntimePointLight(uint32_t id)
{
	if (!mSceneLights.RemoveRuntimePointLight(id))
	{
		return false;
	}

	InvalidateRuntimeLightSceneData();
	NoteLightHistoryChange("runtime-light-change");
	return true;
}

void NRIRenderer::ClearRuntimePointLights()
{
	if (!mSceneLights.ClearRuntimePointLights())
	{
		return;
	}

	InvalidateRuntimeLightSceneData();
	NoteLightHistoryChange("runtime-light-change");
}

void NRIRenderer::PrintRuntimePointLights() const
{
	mSceneLights.PrintRuntimePointLights(NRI_MAX_RUNTIME_POINT_LIGHTS);
}

uint32_t NRIRenderer::GetRuntimePointLightCount() const
{
	return mSceneLights.GetManualAnalyticLightCount();
}

bool NRIRenderer::AddSpriteTileLightHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId)
{
	if (!mSceneLights.AddSpriteTileHeuristic(textureId, color, intensity, radius, flickerFrames, outRuleId))
	{
		return false;
	}

	NoteLightHistoryChange("analytic-light-heuristic-change");
	return true;
}

void NRIRenderer::ClearSpriteTileLightHeuristics()
{
	if (!mSceneLights.ClearSpriteTileHeuristics())
	{
		return;
	}

	NoteLightHistoryChange("analytic-light-heuristic-change");
}

void NRIRenderer::PrintSpriteTileLightHeuristics() const
{
	mSceneLights.PrintSpriteTileLightHeuristics();
}

bool NRIRenderer::AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId)
{
	if (!mSceneLights.AddTextureEmissiveHeuristic(textureId, emissiveMode, intensityScale, emissiveColor, hasExplicitColor, outRuleId))
	{
		return false;
	}

	QueueStaticMapSceneLightingInvalidation();
	mSceneLights.ConsumeEmissiveMaterialBindingChanged();
	mSceneLights.ConsumeEmissiveMaterialPropertiesChanged();
	NoteLightHistoryChange("emissive-heuristic-change");
	return true;
}

void NRIRenderer::ClearTextureEmissiveHeuristics()
{
	if (!mSceneLights.ClearTextureEmissiveHeuristics())
	{
		return;
	}

	QueueStaticMapSceneLightingInvalidation();
	mSceneLights.ConsumeEmissiveMaterialBindingChanged();
	mSceneLights.ConsumeEmissiveMaterialPropertiesChanged();
	NoteLightHistoryChange("emissive-heuristic-change");
}

void NRIRenderer::PrintTextureEmissiveHeuristics() const
{
	mSceneLights.PrintTextureEmissiveHeuristics();
}

void NRIRenderer::NotifyGlowControlChange()
{
	QueueStaticMapSceneLightingInvalidation();
	ResetPersistentDynamicEmissiveCache();
	NoteLightHistoryChange("glow-control-change");
}

void NRIRenderer::NotifyMaterialLightingCalibrationChange()
{
	QueueStaticMapSceneLightingInvalidation();
	ResetPersistentDynamicEmissiveCache();
	NoteLightHistoryChange("material-lighting-calibration-change");
}

void NRIRenderer::NotifyAnalyticLightSettingsChange()
{
	InvalidateRuntimeLightSceneData();
	NoteLightHistoryChange("analytic-light-settings-change");
}

void NRIRenderer::ResetPersistentDynamicEmissiveCache()
{
	mSceneLights.ResetPersistentDynamicEmissiveCache();
}

SceneLightSystem::PersistentDynamicEmissiveCacheBuildServices NRIRenderer::BuildPersistentDynamicEmissiveCacheServices()
{
	SceneLightSystem::PersistentDynamicEmissiveCacheBuildServices services = {};
	services.user = this;
	services.traceActorSpriteVerbose = nri_actor_sprite_diag::ShouldTraceVerbose((int)nri_ptactorspritetrace, (int)nri_pttraceframes);
	services.traceActorSpriteMismatch = nri_actor_sprite_diag::ShouldTraceMismatch((int)nri_ptactorspritetrace, (int)nri_pttraceframes);
	services.buildGeometry = [](void* user, const nri_scene::SceneView& sceneView, nri_scene::GeometryData& geometry, const char* label)
	{
		NRIRenderer* renderer = static_cast<NRIRenderer*>(user);
		Clocker clock(NriPTGeometryBuild);
		double* timer =
			label != nullptr && std::strcmp(label, "persistent_emissive_cache_prune") == 0 ?
			&renderer->mLastPerfShellTraceStats.geometryBuildPersistentEmissivePruneMs :
			&renderer->mLastPerfShellTraceStats.geometryBuildPersistentEmissiveRebuildMs;
		nri_runtime_mutation::ScopedPtPerfTimer perfTimer(*timer);
		nri_scene::BuildGeometry(sceneView, geometry);
		AssignGeometryPortalIndices(renderer->mMapWorld, geometry);
	};
	services.buildMaterials = [](void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label)
	{
		Clocker clock(NriPTMaterialBuild);
		static_cast<NRIRenderer*>(user)->BuildMaterialsWithActorOverrides(sceneView, materials, label);
	};
	return services;
}

namespace
{
	constexpr float TwoPi = 6.28318530717958647692f;
	constexpr uint32_t NriPtMuzzleFlashSlotCount = 8u;
	constexpr uint32_t NriMaxEmissivePrimitives = 16384u;

	DVector3 PathTracingToWorldPosition(const DVector3& source)
	{
		return { source.X, -source.Z, -source.Y };
	}

	DVector3 WorldToPathTracingPosition(const DVector3& source)
	{
		return { source.X, -source.Z, -source.Y };
	}

	void TraceSurfaceNudge(
		const SceneLightSystem::SurfaceRecord& record,
		const SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule& rule,
		const char* pathName,
		const DVector3& sourcePosition,
		const DVector3& nudgedPosition,
		float displacement)
	{
		if (nri_ptnudgetrace <= 0)
		{
			return;
		}

		const DVector3 delta = nudgedPosition - sourcePosition;
		Printf(
			"NRI PT surface nudge: rule=%u path=%s source=%s sector=%d wall=%d nextsector=%d cstat=0x%x nudge=%.3f disp=%.3f from=(%.2f, %.2f, %.2f) to=(%.2f, %.2f, %.2f) delta=(%.2f, %.2f, %.2f)\n",
			rule.ruleId,
			pathName != nullptr ? pathName : "unknown",
			nri_diag::GetSurfaceSourceTypeName(record.provenance.sourceType),
			record.provenance.sectorIndex,
			record.provenance.wallIndex,
			record.provenance.nextSectorIndex,
			record.provenance.cstat,
			rule.nudgeFromSurfaceDistance,
			displacement,
			sourcePosition.X,
			sourcePosition.Y,
			sourcePosition.Z,
			nudgedPosition.X,
			nudgedPosition.Y,
			nudgedPosition.Z,
			delta.X,
			delta.Y,
			delta.Z);
	}

	void Copy3f(const float* source, float* destination)
	{
		destination[0] = source[0];
		destination[1] = source[1];
		destination[2] = source[2];
	}

	const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}

	bool ContainsCaseInsensitive(const char* text, const char* needle)
	{
		if (text == nullptr || needle == nullptr || *needle == '\0')
		{
			return false;
		}

		const size_t needleLength = std::strlen(needle);
		for (const char* cursor = text; *cursor != '\0'; ++cursor)
		{
			size_t index = 0;
			while (index < needleLength &&
				cursor[index] != '\0' &&
				std::tolower((unsigned char)cursor[index]) == std::tolower((unsigned char)needle[index]))
			{
				index++;
			}
			if (index == needleLength)
			{
				return true;
			}
		}

		return false;
	}

	bool ShouldTraceActorOverlayRule(const SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule& rule)
	{
		if ((int)nri_ptactoroverlaylighttrace <= 0 || (int)nri_pttraceframes <= 0)
		{
			return false;
		}
		if ((int)nri_ptactoroverlaylighttrace >= 2)
		{
			return true;
		}

		return ContainsCaseInsensitive(rule.ruleName.c_str(), "explosion") ||
			ContainsCaseInsensitive(rule.actorClassName, "explosion");
	}

	const char* ActorOverlayActivationName(const SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule& rule)
	{
		return rule.activateImmediately ? "immediate" : "surface";
	}

	enum class ActorSpriteLiveMatchResult : uint32_t
	{
		Match = 0,
		NullLiveTexture,
		TextureMismatch,
		PaletteMismatch
	};

	struct ActorSpriteLiveMatchDetails
	{
		ActorSpriteLiveMatchResult result = ActorSpriteLiveMatchResult::Match;
		FGameTexture* liveTexture = nullptr;
		int32_t liveTextureId = -1;
		int32_t surfaceTextureId = -1;
		int32_t livePalette = 0;
		int32_t surfacePalette = 0;
	};

	const char* GetActorSpriteLiveMatchResultName(ActorSpriteLiveMatchResult result)
	{
		switch (result)
		{
		case ActorSpriteLiveMatchResult::Match: return "match";
		case ActorSpriteLiveMatchResult::NullLiveTexture: return "null_live_texture";
		case ActorSpriteLiveMatchResult::TextureMismatch: return "texture_mismatch";
		case ActorSpriteLiveMatchResult::PaletteMismatch: return "palette_mismatch";
		default: return "unknown";
		}
	}

	FTextureID GetLiveActorDisplayTextureId(const DCoreActor& actor)
	{
		return actor.dispictex.isValid() ? actor.dispictex : actor.spr.spritetexture();
	}

	FGameTexture* GetLiveActorSurfaceTexture(const DCoreActor& actor, nri_scene::SurfaceSourceType sourceType)
	{
		switch (sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall:
		case nri_scene::SurfaceSourceType::FacingSprite:
			return TexMan.GetGameTexture(GetLiveActorDisplayTextureId(actor));

		case nri_scene::SurfaceSourceType::VoxelProxySprite:
		{
			if (!r_voxels)
			{
				return nullptr;
			}

			const int voxelIndex = GetExtInfo(actor.spr.spritetexture()).tiletovox;
			if (voxelIndex < 0 || voxelIndex >= MAXVOXELS || voxmodels[voxelIndex] == nullptr || voxmodels[voxelIndex]->model == nullptr)
			{
				return nullptr;
			}

			return TexMan.GetGameTexture(voxmodels[voxelIndex]->model->GetPaletteTexture());
		}

		default:
			return nullptr;
		}
	}

	bool SurfaceUsesLiveActorTextureValidation(const nri_scene::SurfaceRef& surface)
	{
		if (surface.provenance.actorIndex < 0)
		{
			return false;
		}

		switch (surface.provenance.sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall:
			return (surface.material.flags & nri_scene::MaterialFlag_Sprite) != 0;
		case nri_scene::SurfaceSourceType::FacingSprite:
		case nri_scene::SurfaceSourceType::VoxelProxySprite:
			return true;
		default:
			return false;
		}
	}

	ActorSpriteLiveMatchDetails EvaluateCachedSurfaceMatchAgainstLiveActor(const nri_scene::SurfaceRef& surface, const DCoreActor& actor)
	{
		ActorSpriteLiveMatchDetails details = {};
		details.surfaceTextureId = surface.material.texture != nullptr ? surface.material.texture->GetID().GetIndex() : -1;
		details.surfacePalette = surface.material.palette;
		details.livePalette = actor.spr.pal;

		switch (surface.provenance.sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall:
		case nri_scene::SurfaceSourceType::FacingSprite:
		case nri_scene::SurfaceSourceType::VoxelProxySprite:
		{
			details.liveTexture = GetLiveActorSurfaceTexture(actor, surface.provenance.sourceType);
			details.liveTextureId = details.liveTexture != nullptr ? details.liveTexture->GetID().GetIndex() : -1;
			if (details.liveTexture == nullptr)
			{
				details.result = ActorSpriteLiveMatchResult::NullLiveTexture;
			}
			else if (surface.material.texture != details.liveTexture)
			{
				details.result = ActorSpriteLiveMatchResult::TextureMismatch;
			}
			else if (surface.material.palette != actor.spr.pal)
			{
				details.result = ActorSpriteLiveMatchResult::PaletteMismatch;
			}
			return details;
		}

		default:
			return details;
		}
	}

	bool CachedSurfaceMatchesLiveActor(const nri_scene::SurfaceRef& surface, const DCoreActor& actor)
	{
		return EvaluateCachedSurfaceMatchAgainstLiveActor(surface, actor).result == ActorSpriteLiveMatchResult::Match;
	}

	uint64_t HashPersistentSurfaceTaggedSignedValue(uint64_t hash, uint64_t tag, int32_t value)
	{
		hash = (hash ^ tag) * 1099511628211ull;
		hash = (hash ^ (uint64_t)(uint32_t)(value + 1)) * 1099511628211ull;
		return hash;
	}

	uint64_t QuantizePersistentSurfaceCenter(const nri_scene::SurfaceRef& surface)
	{
		if (surface.vertices.empty())
		{
			return 0ull;
		}

		double center[3] = {};
		for (const auto& vertex : surface.vertices)
		{
			center[0] += vertex.position[0];
			center[1] += vertex.position[1];
			center[2] += vertex.position[2];
		}

		const double invVertexCount = 1.0 / (double)surface.vertices.size();
		const int64_t x = (int64_t)std::llround(center[0] * invVertexCount * 16.0);
		const int64_t y = (int64_t)std::llround(center[1] * invVertexCount * 16.0);
		const int64_t z = (int64_t)std::llround(center[2] * invVertexCount * 16.0);

		uint64_t key = 1469598103934665603ull;
		key = (key ^ (uint64_t)x) * 1099511628211ull;
		key = (key ^ (uint64_t)y) * 1099511628211ull;
		key = (key ^ (uint64_t)z) * 1099511628211ull;
		return key;
	}

	uint64_t BuildPersistentEmissiveSurfaceIdentityKey(const nri_scene::SurfaceRef& surface)
	{
		uint64_t key = 1469598103934665603ull;
		key = (key ^ (uint64_t)(uint32_t)surface.provenance.sourceType) * 1099511628211ull;
		key = (key ^ (uint64_t)surface.provenance.drawListType) * 1099511628211ull;

		bool hasAuthoritativeOwnership = false;
		if (surface.provenance.actorIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0xA11C700000000001ull, surface.provenance.actorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (surface.provenance.sectorIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0x5EC70B5E00000001ull, surface.provenance.sectorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (surface.provenance.wallIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0xAA11000000000001ull, surface.provenance.wallIndex);
			hasAuthoritativeOwnership = true;
		}
		if (surface.provenance.sectionIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0x5EC7100000000001ull, surface.provenance.sectionIndex);
			hasAuthoritativeOwnership = true;
		}
		if (surface.provenance.mapChunkIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0xC4C0000000000001ull, surface.provenance.mapChunkIndex);
			hasAuthoritativeOwnership = true;
		}
		if (surface.provenance.nextSectorIndex >= 0)
		{
			key = HashPersistentSurfaceTaggedSignedValue(key, 0x9E57000000000001ull, surface.provenance.nextSectorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (!hasAuthoritativeOwnership)
		{
			key = (key ^ 0xCE173E0000000001ull) * 1099511628211ull;
			key = (key ^ QuantizePersistentSurfaceCenter(surface)) * 1099511628211ull;
		}

		key = (key ^ (uint64_t)(uintptr_t)surface.material.texture) * 1099511628211ull;
		key = (key ^ (uint64_t)(uint32_t)(surface.material.palette + 1)) * 1099511628211ull;
		key = (key ^ (uint64_t)surface.provenance.cstat) * 1099511628211ull;
		key = (key ^ (uint64_t)surface.provenance.materialFlags) * 1099511628211ull;
		return key;
	}

	template <typename SurfaceContainer>
	void AppendUniquePersistentEmissiveSurfaces(
		const SurfaceContainer& source,
		SurfaceContainer& destination,
		std::unordered_set<uint64_t>& inOutSeenKeys,
		uint32_t* appendedCount = nullptr,
		uint32_t* duplicateCount = nullptr)
	{
		for (const auto& surface : source)
		{
			const uint64_t identityKey = BuildPersistentEmissiveSurfaceIdentityKey(surface);
			if (!inOutSeenKeys.insert(identityKey).second)
			{
				if (duplicateCount != nullptr)
				{
					(*duplicateCount)++;
				}
				continue;
			}

			destination.push_back(surface);
			if (appendedCount != nullptr)
			{
				(*appendedCount)++;
			}
		}
	}

	bool HasAutoEmissiveSourceFlags(uint32_t sourceFlags)
	{
		return (sourceFlags & (
			SceneEmissiveSurfaceSourceFlag_AutoFullbright |
			SceneEmissiveSurfaceSourceFlag_AutoTextureGlow |
			SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0;
	}

	float GetSectorEmitterNeutralBrightness()
	{
		const float sectorClamp = std::max(0.0f, (float)nri_ptsectorclamp);
		const float ambientScale = std::max(0.0f, (float)nri_ptsectorambientscale);
		return std::min(sectorClamp, ambientScale * (0.10f + 0.75f * 0.55f));
	}

	const char* GetSceneLightRecordSourceName(SceneLightRecordSource source)
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

	void ComputeSurfaceBounds(const nri_scene::SurfaceRef& surface, float outCenter[3], float& outRadius)
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		outRadius = 0.0f;

		if (surface.vertices.empty())
		{
			return;
		}

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			outCenter[0] += vertex.position[0];
			outCenter[1] += vertex.position[1];
			outCenter[2] += vertex.position[2];
		}

		const float invCount = 1.0f / (float)surface.vertices.size();
		outCenter[0] *= invCount;
		outCenter[1] *= invCount;
		outCenter[2] *= invCount;

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			const float dx = vertex.position[0] - outCenter[0];
			const float dy = vertex.position[1] - outCenter[1];
			const float dz = vertex.position[2] - outCenter[2];
			outRadius = std::max(outRadius, std::sqrt(dx * dx + dy * dy + dz * dz));
		}
	}

	float ComputeTriangleArea(const nri_scene::CapturedVertex& a, const nri_scene::CapturedVertex& b, const nri_scene::CapturedVertex& c)
	{
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		return 0.5f * std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
	}

	float ComputeSurfaceArea(const nri_scene::SurfaceRef& surface)
	{
		if (surface.vertices.size() < 3)
		{
			return 0.0f;
		}

		float area = 0.0f;
		if ((surface.material.flags & nri_scene::MaterialFlag_Flat) != 0)
		{
			for (uint32_t i = 0; i + 2 < surface.vertices.size(); i += 3)
			{
				area += ComputeTriangleArea(surface.vertices[i], surface.vertices[i + 1], surface.vertices[i + 2]);
			}
		}
		else
		{
			const nri_scene::CapturedVertex& root = surface.vertices[0];
			for (uint32_t i = 1; i + 1 < surface.vertices.size(); ++i)
			{
				area += ComputeTriangleArea(root, surface.vertices[i], surface.vertices[i + 1]);
			}
		}

		return area;
	}

	uint64_t HashLightOverlayText(uint64_t hash, const char* text)
	{
		if (text == nullptr)
		{
			return hash;
		}

		for (const unsigned char* cursor = (const unsigned char*)text; *cursor != '\0'; ++cursor)
		{
			hash ^= (uint64_t)(*cursor);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	uint32_t BuildResolvedLightOverlayRuleId(const char* id, const char* classOrMapName, const LightOverlaySourceLocation& source)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashLightOverlayText(hash, id);
		hash = HashLightOverlayText(hash, classOrMapName);
		hash = HashLightOverlayText(hash, source.sourceName.GetChars());
		hash ^= (uint64_t)source.orderIndex + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		const uint32_t ruleId = (uint32_t)(hash ^ (hash >> 32));
		return ruleId != 0 ? ruleId : 1u;
	}

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	float ComputePrimitiveArea(const nri_scene::GeometryData& geometry, uint32_t primitiveIndex)
	{
		if (primitiveIndex >= geometry.primitives.size())
		{
			return 0.0f;
		}

		const auto& primitive = geometry.primitives[primitiveIndex];
		if (primitive.indices[0] >= geometry.vertices.size() ||
			primitive.indices[1] >= geometry.vertices.size() ||
			primitive.indices[2] >= geometry.vertices.size())
		{
			return 0.0f;
		}

		const auto& a = geometry.vertices[primitive.indices[0]];
		const auto& b = geometry.vertices[primitive.indices[1]];
		const auto& c = geometry.vertices[primitive.indices[2]];
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		return 0.5f * std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
	}

	void ComputePrimitiveBounds(const nri_scene::GeometryData& geometry, uint32_t primitiveIndex, float outCenter[3], float& outRadius)
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		outRadius = 0.0f;
		if (primitiveIndex >= geometry.primitives.size())
		{
			return;
		}

		const auto& primitive = geometry.primitives[primitiveIndex];
		if (primitive.indices[0] >= geometry.vertices.size() ||
			primitive.indices[1] >= geometry.vertices.size() ||
			primitive.indices[2] >= geometry.vertices.size())
		{
			return;
		}

		const auto& a = geometry.vertices[primitive.indices[0]];
		const auto& b = geometry.vertices[primitive.indices[1]];
		const auto& c = geometry.vertices[primitive.indices[2]];
		outCenter[0] = (a.position[0] + b.position[0] + c.position[0]) / 3.0f;
		outCenter[1] = (a.position[1] + b.position[1] + c.position[1]) / 3.0f;
		outCenter[2] = (a.position[2] + b.position[2] + c.position[2]) / 3.0f;
		for (const auto* vertex : { &a, &b, &c })
		{
			const float dx = vertex->position[0] - outCenter[0];
			const float dy = vertex->position[1] - outCenter[1];
			const float dz = vertex->position[2] - outCenter[2];
			outRadius = std::max(outRadius, std::sqrt(dx * dx + dy * dy + dz * dz));
		}
	}

	uint64_t HashGeometryForEmissiveSampling(const nri_scene::GeometryData* geometry)
	{
		uint64_t hash = 1469598103934665603ull;
		if (geometry == nullptr)
		{
			return nri_scene::HashCombine64(hash, 0ull);
		}

		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry->vertices.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry->primitives.size());
		for (const nri_scene::SceneVertex& vertex : geometry->vertices)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(vertex.position[0]));
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(vertex.position[1]));
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(vertex.position[2]));
		}

		for (const nri_scene::PrimitiveData& primitive : geometry->primitives)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)primitive.indices[0]);
			hash = nri_scene::HashCombine64(hash, (uint64_t)primitive.indices[1]);
			hash = nri_scene::HashCombine64(hash, (uint64_t)primitive.indices[2]);
			hash = nri_scene::HashCombine64(hash, (uint64_t)primitive.materialIndex);
		}

		return hash;
	}

	float Dot3(const float* a, const float* b)
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	}

	std::string BuildNormalizedMuzzleFlashEventKey(const FString& eventId)
	{
		if (eventId.IsEmpty())
		{
			return {};
		}

		const FString normalizedId = eventId.MakeLower();
		return std::string(normalizedId.GetChars());
	}

	void WorldToPathTracingPosition(const DVector3& worldPos, float out[3])
	{
		out[0] = (float)worldPos.X;
		out[1] = (float)-worldPos.Z;
		out[2] = (float)-worldPos.Y;
	}

	uint32_t BuildMuzzleFlashRuleId(const ResolvedLightOverlayMuzzleFlashRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), "", rule.source);
	}

	uint64_t BuildMuzzleFlashRandomSeed(const PathTracingWeaponLightEvent& event)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, event.serial);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(event.hasEmitterActorIndex ? event.emitterActorIndex + 1 : 0));
		hash = HashLightOverlayText(hash, BuildNormalizedMuzzleFlashEventKey(event.eventId).c_str());
		return hash;
	}

	uint64_t AdvanceMuzzleFlashRandomState(uint64_t& state)
	{
		state += 0x9e3779b97f4a7c15ull;
		uint64_t z = state;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return z ^ (z >> 31);
	}

	float NextMuzzleFlashUnitRandom(uint64_t& state)
	{
		const uint64_t bits = AdvanceMuzzleFlashRandomState(state);
		return (float)((bits >> 40) & 0xFFFFFFu) * (1.0f / 16777215.0f);
	}

	float ResolveMuzzleFlashRandomRange(uint64_t& randomState, float minValue, float maxValue)
	{
		if (!std::isfinite(minValue) || !std::isfinite(maxValue))
		{
			return 0.0f;
		}

		if (minValue > maxValue)
		{
			std::swap(minValue, maxValue);
		}

		if (minValue == maxValue)
		{
			return minValue;
		}

		return minValue + (maxValue - minValue) * NextMuzzleFlashUnitRandom(randomState);
	}

	float EvaluateMuzzleFlashFadeOut(double currentTimeSeconds, bool occupied, float peakIntensity, float radius, double activationTimeSeconds, double endTimeSeconds)
	{
		if (!occupied ||
			peakIntensity <= 0.0f ||
			radius <= 0.0f ||
			currentTimeSeconds < activationTimeSeconds)
		{
			return 0.0f;
		}

		const double durationSeconds = endTimeSeconds - activationTimeSeconds;
		if (durationSeconds <= 0.0 || currentTimeSeconds >= endTimeSeconds)
		{
			return 0.0f;
		}

		const double progress = std::clamp((currentTimeSeconds - activationTimeSeconds) / durationSeconds, 0.0, 1.0);
		const double fade = progress >= 1.0 ? 0.0 : std::pow(2.0, -10.0 * progress);
		return (float)(peakIntensity * fade);
	}

	uint64_t QuantizePositionKey(const float position[3])
	{
		const int64_t x = (int64_t)std::llround(position[0] * 16.0f);
		const int64_t y = (int64_t)std::llround(position[1] * 16.0f);
		const int64_t z = (int64_t)std::llround(position[2] * 16.0f);
		uint64_t key = 1469598103934665603ull;
		key = nri_scene::HashCombine64(key, (uint64_t)x);
		key = nri_scene::HashCombine64(key, (uint64_t)y);
		key = nri_scene::HashCombine64(key, (uint64_t)z);
		return key;
	}

	uint64_t HashTaggedSignedValue(uint64_t hash, uint64_t tag, int32_t value)
	{
		hash = nri_scene::HashCombine64(hash, tag);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)(value + 1));
		return hash;
	}

	uint64_t BuildSurfaceIdentityKey(const SceneLightSystem::SurfaceRecord& record)
	{
		uint64_t key = 1469598103934665603ull;
		key = nri_scene::HashCombine64(key, (uint64_t)(uint32_t)record.source);
		key = nri_scene::HashCombine64(key, (uint64_t)(uint32_t)record.provenance.sourceType);
		key = nri_scene::HashCombine64(key, (uint64_t)record.provenance.drawListType);

		bool hasAuthoritativeOwnership = false;
		if (record.provenance.actorIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0xA11C700000000001ull, record.provenance.actorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (record.provenance.sectorIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0x5EC70B5E00000001ull, record.provenance.sectorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (record.provenance.wallIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0xAA11000000000001ull, record.provenance.wallIndex);
			hasAuthoritativeOwnership = true;
		}
		if (record.provenance.sectionIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0x5EC7100000000001ull, record.provenance.sectionIndex);
			hasAuthoritativeOwnership = true;
		}
		if (record.provenance.mapChunkIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0xC4C0000000000001ull, record.provenance.mapChunkIndex);
			hasAuthoritativeOwnership = true;
		}
		if (record.provenance.nextSectorIndex >= 0)
		{
			key = HashTaggedSignedValue(key, 0x9E57000000000001ull, record.provenance.nextSectorIndex);
			hasAuthoritativeOwnership = true;
		}
		if (!hasAuthoritativeOwnership)
		{
			key = nri_scene::HashCombine64(key, 0xCE173E0000000001ull);
			key = nri_scene::HashCombine64(key, QuantizePositionKey(record.center));
		}

		return key;
	}

	uint64_t BuildAnalyticTopologyKey(uint32_t sourceFlags, uint32_t ruleId, const SceneLightSystem::SurfaceRecord& record)
	{
		uint64_t key = 1469598103934665603ull;
		key = nri_scene::HashCombine64(key, (uint64_t)sourceFlags);
		key = nri_scene::HashCombine64(key, (uint64_t)ruleId);
		key = nri_scene::HashCombine64(key, record.identityKey);
		return key;
	}

	uint64_t BuildActorOverlayTopologyKey(const SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule& rule)
	{
		uint64_t key = 1469598103934665603ull;
		key = nri_scene::HashCombine64(key, (uint64_t)SceneAnalyticLightSourceFlag_ActorOverlay);
		key = nri_scene::HashCombine64(key, (uint64_t)rule.ruleId);
		key = nri_scene::HashCombine64(key, (uint64_t)(uint32_t)(rule.actorIndex + 1));
		return key;
	}

	uint64_t HashQuantizedFloat(uint64_t hash, float value, float scale)
	{
		const int64_t quantized = (int64_t)std::llround((double)value * (double)scale);
		return nri_scene::HashCombine64(hash, (uint64_t)quantized);
	}

	uint64_t BuildAnalyticPropertyHash(const SceneLightSystem::SceneAnalyticLight& light)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)light.source);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)light.actorIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)light.textureId);
		hash = nri_scene::HashCombine64(hash, (uint64_t)light.flags);
		hash = HashQuantizedFloat(hash, light.position[0], 16.0f);
		hash = HashQuantizedFloat(hash, light.position[1], 16.0f);
		hash = HashQuantizedFloat(hash, light.position[2], 16.0f);
		hash = HashQuantizedFloat(hash, light.color[0], 4096.0f);
		hash = HashQuantizedFloat(hash, light.color[1], 4096.0f);
		hash = HashQuantizedFloat(hash, light.color[2], 4096.0f);
		hash = HashQuantizedFloat(hash, light.intensity, 4096.0f);
		hash = HashQuantizedFloat(hash, light.radius, 16.0f);
		return hash;
	}

	uint64_t BuildAnalyticBindingHash(const SceneLightSystem::SceneAnalyticLight& light)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)light.sourceFlags);
		hash = nri_scene::HashCombine64(hash, (uint64_t)light.sourceRuleId);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)light.source);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)light.actorIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)light.textureId);
		return hash;
	}

	float EvaluateFlickerScale(uint64_t stableKey, uint32_t frameIndex, uint32_t flickerFrames)
	{
		if (flickerFrames <= 1)
		{
			return 1.0f;
		}

		const uint32_t seed = (uint32_t)(stableKey ^ (stableKey >> 32));
		const uint32_t phaseFrame = (frameIndex + seed) % flickerFrames;
		const float phase = ((float)phaseFrame / (float)flickerFrames) * TwoPi;
		return 0.35f + 0.65f * (0.5f + 0.5f * std::cos(phase));
	}

	float EvaluatePulseScale(uint64_t stableKey, uint32_t frameIndex, uint32_t pulseFrames, float pulseAmount)
	{
		if (pulseFrames <= 1 || pulseAmount <= 0.0f)
		{
			return 1.0f;
		}

		const float clampedAmount = std::clamp(pulseAmount, 0.0f, 0.95f);
		const float baseScale = 1.0f - clampedAmount;
		return baseScale + clampedAmount * EvaluateFlickerScale(stableKey ^ 0x5EC70B5E00000000ull, frameIndex, pulseFrames);
	}

	uint64_t AdvanceOverlayRandomState(uint64_t& state)
	{
		state += 0x9e3779b97f4a7c15ull;
		uint64_t z = state;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return z ^ (z >> 31);
	}

	float NextOverlayUnitRandom(uint64_t& state)
	{
		const uint64_t bits = AdvanceOverlayRandomState(state);
		return (float)((bits >> 40) & 0xFFFFFFu) * (1.0f / 16777215.0f);
	}

	float EvaluateOverlayRandomIntensityOffset(uint64_t stableKey, uint32_t renderFrameIndex, float minValue, float maxValue)
	{
		if (!std::isfinite(minValue) || !std::isfinite(maxValue))
		{
			return 0.0f;
		}

		if (minValue > maxValue)
		{
			std::swap(minValue, maxValue);
		}

		if (minValue == maxValue)
		{
			return minValue;
		}

		uint64_t randomState = stableKey ^ 0xA17F4D6300000000ull ^ ((uint64_t)renderFrameIndex * 0x9e3779b97f4a7c15ull);
		return minValue + (maxValue - minValue) * NextOverlayUnitRandom(randomState);
	}

	float ResolveOverlayLightIntensity(
		float baseIntensity,
		uint64_t stableKey,
		uint32_t flickerTimeIndex,
		uint32_t renderFrameIndex,
		uint32_t flickerFrames,
		bool hasRandomIntensity,
		const float randomIntensityRange[2])
	{
		if (hasRandomIntensity)
		{
			const float intensityOffset = EvaluateOverlayRandomIntensityOffset(
				stableKey,
				renderFrameIndex,
				randomIntensityRange[0],
				randomIntensityRange[1]);
			return std::max(baseIntensity + intensityOffset, 0.0f);
		}

		return baseIntensity * EvaluateFlickerScale(stableKey, flickerTimeIndex, flickerFrames);
	}

	bool IsValidSurfaceNudgeDistance(float distance)
	{
		return std::isfinite(distance) && distance > 0.0f;
	}

	bool TryResolveSurfaceNudgeSector(
		const SceneLightSystem::SurfaceRecord& record,
		const DVector3& position,
		sectortype*& outSector)
	{
		outSector =
			record.provenance.sectorIndex >= 0 && (unsigned)record.provenance.sectorIndex < sector.Size() ?
			&sector[(unsigned)record.provenance.sectorIndex] :
			nullptr;

		sectortype* candidate = outSector;
		updatesectorz(position, &candidate);
		if (candidate != nullptr)
		{
			outSector = candidate;
			return true;
		}

		candidate = outSector;
		updatesector(position, &candidate);
		if (candidate != nullptr)
		{
			outSector = candidate;
			return true;
		}

		const int bestSectorIndex = FindBestSector(position);
		if (bestSectorIndex >= 0 && (unsigned)bestSectorIndex < sector.Size())
		{
			outSector = &sector[(unsigned)bestSectorIndex];
			return true;
		}

		outSector = nullptr;
		return false;
	}

	bool IsWallLikeSurfaceSource(nri_scene::SurfaceSourceType sourceType)
	{
		switch (sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall:
		case nri_scene::SurfaceSourceType::MirrorWall:
		case nri_scene::SurfaceSourceType::MapWallBand:
			return true;
		default:
			return false;
		}
	}

	uint64_t BuildActorTextureSurfaceIndexKey(int32_t actorIndex, uint32_t textureId)
	{
		return ((uint64_t)(uint32_t)actorIndex << 32) | (uint64_t)textureId;
	}

	DVector2 ComputeSectorCenter2D(const sectortype* sec)
	{
		if (sec == nullptr || sec->walls.Size() == 0)
		{
			return {};
		}

		DVector2 center = {};
		for (const auto& wal : sec->walls)
		{
			center += wal.pos;
		}
		return center / (double)sec->walls.Size();
	}

	bool TryProjectAwayFromWall(
		const DVector3& sourcePosition,
		const walltype& sourceWall,
		float nudgeDistance,
		const sectortype* preferredSideSector,
		DVector3& outPosition,
		float& outDisplacement)
	{
		outPosition = sourcePosition;
		outDisplacement = 0.0f;

		if (!IsValidSurfaceNudgeDistance(nudgeDistance))
		{
			return false;
		}

		DVector2 nearestPoint = {};
		const double maxDistanceSquared = (double)nudgeDistance * (double)nudgeDistance;
		const double distanceSquared = SquareDistToWall(sourcePosition.X, sourcePosition.Y, &sourceWall, &nearestPoint);
		if (distanceSquared > maxDistanceSquared)
		{
			return false;
		}

		DVector2 wallNormal = sourceWall.delta().Rotated90CCW().Unit();
		const DVector2 nearestToSource = sourcePosition.XY() - nearestPoint;
		if (nearestToSource.LengthSquared() > 1e-12)
		{
			if (nearestToSource.dot(wallNormal) < 0.0)
			{
				wallNormal = -wallNormal;
			}
		}
		else if (preferredSideSector != nullptr)
		{
			const DVector2 sectorDelta = ComputeSectorCenter2D(preferredSideSector) - nearestPoint;
			if (sectorDelta.dot(wallNormal) < 0.0)
			{
				wallNormal = -wallNormal;
			}
		}

		const DVector2 nudgedXY = nearestPoint + wallNormal * nudgeDistance;
		const DVector2 displacement = nudgedXY - sourcePosition.XY();
		const double displacementSquared = displacement.LengthSquared();
		if (displacementSquared <= 1e-12)
		{
			return false;
		}

		outPosition = sourcePosition;
		outPosition.X = nudgedXY.X;
		outPosition.Y = nudgedXY.Y;
		outDisplacement = (float)std::sqrt(displacementSquared);
		return true;
	}

	bool TryApplyProvenanceWallSurfaceNudge(
		const SceneLightSystem::SurfaceRecord& record,
		const DVector3& sourcePosition,
		float nudgeDistance,
		DVector3& outPosition,
		float& outDisplacement)
	{
		outPosition = sourcePosition;
		outDisplacement = 0.0f;

		if (!IsWallLikeSurfaceSource(record.provenance.sourceType) ||
			record.provenance.wallIndex < 0 ||
			(unsigned)record.provenance.wallIndex >= wall.Size())
		{
			return false;
		}

		const walltype& sourceWall = wall[(unsigned)record.provenance.wallIndex];
		const sectortype* preferredSideSector =
			record.provenance.sectorIndex >= 0 && (unsigned)record.provenance.sectorIndex < sector.Size() ?
			&sector[(unsigned)record.provenance.sectorIndex] :
			nullptr;
		return TryProjectAwayFromWall(sourcePosition, sourceWall, nudgeDistance, preferredSideSector, outPosition, outDisplacement);
	}

	bool TryApplyWallSurfaceNudge(
		const DVector3& sourcePosition,
		sectortype* startSector,
		float nudgeDistance,
		DVector3& outPosition,
		float& outDisplacement)
	{
		outPosition = sourcePosition;
		outDisplacement = 0.0f;

		if (startSector == nullptr || !IsValidSurfaceNudgeDistance(nudgeDistance))
		{
			return false;
		}

		walltype* bestWall = nullptr;
		double bestDistanceSquared = DBL_MAX;
		const double maxDistanceSquared = (double)nudgeDistance * (double)nudgeDistance;

		BFSSectorSearch search(startSector);
		while (auto sec = search.GetNext())
		{
			for (auto& wal : sec->walls)
			{
				DVector2 nearestPoint = {};
				const double distanceSquared = SquareDistToWall(sourcePosition.X, sourcePosition.Y, &wal, &nearestPoint);
				if (distanceSquared > maxDistanceSquared)
				{
					continue;
				}

				bool blocked = false;
				if (!wal.twoSided())
				{
					blocked = true;
				}
				else
				{
					const DVector2 nearestPoint = NearestPointOnWall(sourcePosition.X, sourcePosition.Y, &wal);
					blocked = checkOpening(nearestPoint, sourcePosition.Z, sec, wal.nextSector(), 0.0, 0.0);
					if (!blocked)
					{
						search.Add(wal.nextSector());
					}
				}

				if (!blocked || distanceSquared >= bestDistanceSquared)
				{
					continue;
				}

				bestWall = &wal;
				bestDistanceSquared = distanceSquared;
			}
		}

		if (bestWall == nullptr)
		{
			return false;
		}

		return TryProjectAwayFromWall(sourcePosition, *bestWall, nudgeDistance, nullptr, outPosition, outDisplacement);
	}

	bool TryApplyPlaneSurfaceNudge(
		const DVector3& sourcePosition,
		sectortype* startSector,
		float nudgeDistance,
		DVector3& outPosition,
		float& outDisplacement)
	{
		outPosition = sourcePosition;
		outDisplacement = 0.0f;

		if (startSector == nullptr || !IsValidSurfaceNudgeDistance(nudgeDistance))
		{
			return false;
		}

		double ceilingZ = -FLT_MAX;
		double floorZ = FLT_MAX;
		CollisionBase ceilingHit = {};
		CollisionBase floorHit = {};
		getzrange(sourcePosition, startSector, &ceilingZ, ceilingHit, &floorZ, floorHit, nudgeDistance, 0u);

		double bestTargetZ = sourcePosition.Z;
		double bestDisplacement = DBL_MAX;
		bool foundCandidate = false;

		const double ceilingDistance = sourcePosition.Z - ceilingZ;
		if (ceilingHit.type == kHitSector &&
			ceilingDistance >= 0.0 &&
			ceilingDistance < nudgeDistance)
		{
			const double targetZ = ceilingZ + nudgeDistance;
			const double displacement = targetZ - sourcePosition.Z;
			if (displacement > 0.0 && displacement < bestDisplacement)
			{
				bestTargetZ = targetZ;
				bestDisplacement = displacement;
				foundCandidate = true;
			}
		}

		const double floorDistance = floorZ - sourcePosition.Z;
		if (floorHit.type == kHitSector &&
			floorDistance >= 0.0 &&
			floorDistance < nudgeDistance)
		{
			const double targetZ = floorZ - nudgeDistance;
			const double displacement = sourcePosition.Z - targetZ;
			if (displacement > 0.0 && displacement < bestDisplacement)
			{
				bestTargetZ = targetZ;
				bestDisplacement = displacement;
				foundCandidate = true;
			}
		}

		if (!foundCandidate)
		{
			return false;
		}

		outPosition = sourcePosition;
		outPosition.Z = bestTargetZ;
		outDisplacement = (float)bestDisplacement;
		return true;
	}

	void ApplyActorOverlaySurfaceNudge(
		const SceneLightSystem::SurfaceRecord& record,
		const SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule& rule,
		float position[3])
	{
		if (!rule.hasNudgeFromSurface || !IsValidSurfaceNudgeDistance(rule.nudgeFromSurfaceDistance) || position == nullptr)
		{
			return;
		}

		const DVector3 sourceRenderPosition(position[0], position[1], position[2]);
		const DVector3 sourceWorldPosition = PathTracingToWorldPosition(sourceRenderPosition);
		sectortype* startSector = nullptr;
		if (!TryResolveSurfaceNudgeSector(record, sourceWorldPosition, startSector) || startSector == nullptr)
		{
			return;
		}

		DVector3 provenanceWallPosition = sourceWorldPosition;
		float provenanceWallDisplacement = 0.0f;
		if (TryApplyProvenanceWallSurfaceNudge(record, sourceWorldPosition, rule.nudgeFromSurfaceDistance, provenanceWallPosition, provenanceWallDisplacement))
		{
			const DVector3 provenanceWallRenderPosition = WorldToPathTracingPosition(provenanceWallPosition);
			TraceSurfaceNudge(record, rule, "source_wall", sourceRenderPosition, provenanceWallRenderPosition, provenanceWallDisplacement);
			position[0] = (float)provenanceWallRenderPosition.X;
			position[1] = (float)provenanceWallRenderPosition.Y;
			position[2] = (float)provenanceWallRenderPosition.Z;
			return;
		}

		DVector3 bestPosition = sourceWorldPosition;
		float bestDisplacement = FLT_MAX;

		DVector3 wallPosition = sourceWorldPosition;
		float wallDisplacement = 0.0f;
		if (TryApplyWallSurfaceNudge(sourceWorldPosition, startSector, rule.nudgeFromSurfaceDistance, wallPosition, wallDisplacement) &&
			wallDisplacement < bestDisplacement)
		{
			bestPosition = wallPosition;
			bestDisplacement = wallDisplacement;
		}

		DVector3 planePosition = sourceWorldPosition;
		float planeDisplacement = 0.0f;
		if (TryApplyPlaneSurfaceNudge(sourceWorldPosition, startSector, rule.nudgeFromSurfaceDistance, planePosition, planeDisplacement) &&
			planeDisplacement < bestDisplacement)
		{
			bestPosition = planePosition;
			bestDisplacement = planeDisplacement;
		}

		if (bestDisplacement == FLT_MAX)
		{
			return;
		}

		const DVector3 bestRenderPosition = WorldToPathTracingPosition(bestPosition);
		TraceSurfaceNudge(record, rule, bestPosition.Z != sourceWorldPosition.Z ? "plane" : "wall_fallback", sourceRenderPosition, bestRenderPosition, bestDisplacement);

		position[0] = (float)bestRenderPosition.X;
		position[1] = (float)bestRenderPosition.Y;
		position[2] = (float)bestRenderPosition.Z;
	}

	float ComputeColorLuminance(const float color[3])
	{
		return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
	}

	float ComputeBuildLightLevel(int shade, int paletteIndex)
	{
		const int clampedPalette = clamp(paletteIndex, 0, MAXPALOOKUPS - 1);
		const float shadeDiv = lookups.tables[clampedPalette].ShadeFactor;
		const bool fullbright = shadeDiv < 1.0f / 1000.0f || shade < -numshades;
		if (fullbright)
		{
			return 1.0f;
		}

		float inverseLight = (float)shade * 255.0f / (float)numshades;
		inverseLight /= shadeDiv;
		const float lightLevel = clamp(255.0f - inverseLight, 0.0f, 255.0f);
		return lightLevel / 255.0f;
	}

	float ComputeSectorEmitterResponseScale(float brightness, float neutralBrightness, float intensity, float minScale, float maxScale)
	{
		const float clampedMin = std::max(0.0f, minScale);
		const float clampedMax = std::max(clampedMin, maxScale);
		if (neutralBrightness <= 0.0001f || intensity <= 0.0f)
		{
			return clamp(1.0f, clampedMin, clampedMax);
		}

		const float normalizedDelta = (brightness - neutralBrightness) / neutralBrightness;
		return clamp(1.0f + normalizedDelta * intensity, clampedMin, clampedMax);
	}

	float ComputeSectorEmitterRangeResponseScale(float signal, float inputMin, float inputMax, float minScale, float maxScale)
	{
		const float clampedMin = std::max(0.0f, minScale);
		const float clampedMax = std::max(clampedMin, maxScale);
		const float inputRange = inputMax - inputMin;
		if (std::abs(inputRange) <= 0.0001f)
		{
			return clamp(1.0f, clampedMin, clampedMax);
		}

		const float t = clamp((signal - inputMin) / inputRange, 0.0f, 1.0f);
		return clampedMin + (clampedMax - clampedMin) * t;
	}

	bool IsGlowDrivenEmissive(uint32_t sourceFlags, uint32_t emissiveMode)
	{
		if (emissiveMode == nri_scene::MaterialEmissiveMode_UseGlowmapTexture)
		{
			return true;
		}

		return (sourceFlags & (SceneEmissiveSurfaceSourceFlag_AutoTextureGlow | SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0;
	}

	float ResolveGlowStrengthScale(uint32_t sourceFlags, uint32_t emissiveMode, const NRILightingSettings& settings)
	{
		return IsGlowDrivenEmissive(sourceFlags, emissiveMode) ? std::max(settings.glowScale, 0.0f) : 1.0f;
	}

	float ResolveGlowSamplingScale(uint32_t sourceFlags, uint32_t emissiveMode, const NRILightingSettings& settings)
	{
		return IsGlowDrivenEmissive(sourceFlags, emissiveMode) ? std::max(settings.glowReach, 0.0f) : 1.0f;
	}

	float ResolveGlowFalloffScale(uint32_t sourceFlags, uint32_t emissiveMode, const NRILightingSettings& settings)
	{
		return IsGlowDrivenEmissive(sourceFlags, emissiveMode) ? std::clamp(settings.glowFalloff, 0.25f, 4.0f) : 1.0f;
	}

	bool HasPalEntryColor(const PalEntry& color)
	{
		return color.r != 0 || color.g != 0 || color.b != 0;
	}

	void ResolveSectorTint(const sectortype& sec, int paletteIndex, float outTint[3], float& outFogStrength)
	{
		(void)paletteIndex;

		outTint[0] = 1.0f;
		outTint[1] = 1.0f;
		outTint[2] = 1.0f;
		outFogStrength = 0.0f;

		const float visibilityStrength = std::clamp((float)sec.visibility / 128.0f, 0.0f, 1.0f);
		const bool hasExplicitFogPalette = sec.fogpal > 0;
		outFogStrength = hasExplicitFogPalette ? std::max(visibilityStrength, 0.35f) : visibilityStrength;
		if (!hasExplicitFogPalette)
		{
			return;
		}

		PalEntry fade = {};
		fade = lookups.getFade(clamp((int)sec.fogpal, 0, MAXPALOOKUPS - 1));

		const bool hasFogTint = HasPalEntryColor(fade);
		if (!hasFogTint)
		{
			return;
		}

		const float tint[3] = {
			(float)fade.r / 255.0f,
			(float)fade.g / 255.0f,
			(float)fade.b / 255.0f,
		};
		const float tintWeight = std::clamp((hasExplicitFogPalette ? 0.20f : 0.10f) + outFogStrength * 0.35f, 0.0f, 0.65f);
		outTint[0] = 1.0f + (tint[0] - 1.0f) * tintWeight;
		outTint[1] = 1.0f + (tint[1] - 1.0f) * tintWeight;
		outTint[2] = 1.0f + (tint[2] - 1.0f) * tintWeight;
	}

	uint64_t BuildEmissiveTopologyKey(const SceneLightSystem::SurfaceRecord& record)
	{
		return record.identityKey;
	}

	uint64_t BuildEmissivePropertyHash(const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& emissive)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.sourceFlags);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.sourceRuleId);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.overrideRuleId);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)emissive.source);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)emissive.actorIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)emissive.sectorIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)emissive.authoredSectorIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)emissive.wallIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.textureId);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.emissiveTextureIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.emissiveMode);
		hash = HashQuantizedFloat(hash, emissive.center[0], 16.0f);
		hash = HashQuantizedFloat(hash, emissive.center[1], 16.0f);
		hash = HashQuantizedFloat(hash, emissive.center[2], 16.0f);
		hash = HashQuantizedFloat(hash, emissive.boundsRadius, 16.0f);
		hash = HashQuantizedFloat(hash, emissive.surfaceArea, 16.0f);
		hash = HashQuantizedFloat(hash, emissive.emissiveColor[0], 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.emissiveColor[1], 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.emissiveColor[2], 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.emissiveIntensity, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.reachScale, 4096.0f);
		hash = nri_scene::HashCombine64(hash, emissive.hasSectorResponseParams ? 1ull : 0ull);
		hash = HashQuantizedFloat(hash, emissive.sectorResponseIntensity, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.sectorResponseMin, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.sectorResponseMax, 4096.0f);
		hash = nri_scene::HashCombine64(hash, emissive.hasSectorResponseInputRange ? 1ull : 0ull);
		hash = HashQuantizedFloat(hash, emissive.sectorResponseInputMin, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.sectorResponseInputMax, 4096.0f);
		hash = nri_scene::HashCombine64(hash, emissive.materialResponseEnabled ? 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, emissive.materialResponseExplicit ? 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, emissive.hasMaterialResponseParams ? 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, emissive.hasMaterialResponseMin ? 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, emissive.hasMaterialResponseMax ? 1ull : 0ull);
		hash = HashQuantizedFloat(hash, emissive.materialResponseMin, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.materialResponseMax, 4096.0f);
		hash = HashQuantizedFloat(hash, emissive.powerEstimate, 256.0f);
		hash = nri_scene::HashCombine64(hash, emissive.sectorResponseEnabled ? 1ull : 0ull);
		return hash;
	}

	uint64_t BuildEmissiveBindingHash(const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& emissive)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.sourceFlags);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.sourceRuleId);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.overrideRuleId);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)emissive.source);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)emissive.actorIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)emissive.sectorIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)emissive.authoredSectorIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)emissive.wallIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.textureId);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.materialIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.emissiveMode);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.emissiveTextureIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.placedPrimitiveBase);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.placedPrimitiveCount);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.sceneInstanceIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.occurrenceKeyLo);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.occurrenceKeyHi);
		hash = nri_scene::HashCombine64(hash, (uint64_t)emissive.occurrenceGeneration);
		return hash;
	}

	bool EmissiveOverrideMatchesSurface(
		const SceneLightSystem::EmissiveOverrideRule& rule,
		const SceneLightSystem::SurfaceRecord& record)
	{
		if (rule.hasSectorFilter && record.provenance.sectorIndex != rule.sectorFilter)
		{
			return false;
		}
		if (rule.hasWallFilter && record.provenance.wallIndex != rule.wallFilter)
		{
			return false;
		}
		if (rule.hasTileFilter && record.material.textureId != rule.tileFilter)
		{
			return false;
		}
		return rule.hasSectorFilter || rule.hasWallFilter || rule.hasTileFilter;
	}

	bool SurfaceLightFixtureRuleMatchesSurface(
		const SceneLightSystem::EmissiveOverrideRule& rule,
		const SceneLightSystem::SurfaceRecord& record)
	{
		return
			record.source == SceneLightRecordSource::SurfaceLightOverlayScene &&
			record.provenance.cstat == rule.ruleId;
	}

	float ResolveSurfaceLightOverlayMinimumBrightnessScale(const SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface)
	{
		if (surface.source != SceneLightRecordSource::SurfaceLightOverlayScene)
		{
			return 0.0f;
		}

		const float minimumBrightness = std::max(0.0f, (float)nri_ptsurfacelightminbrightness);
		if (minimumBrightness <= 0.0f || surface.emissiveIntensity <= 0.0f)
		{
			return 0.0f;
		}

		return minimumBrightness / surface.emissiveIntensity;
	}

	std::string NormalizeMaterialTextureName(const FGameTexture* texture)
	{
		std::string normalized = texture != nullptr ? texture->GetName().GetChars() : "";
		for (char& c : normalized)
		{
			c = (char)std::tolower((unsigned char)c);
		}

		const size_t slash = normalized.find_last_of("/\\");
		const size_t dot = normalized.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		{
			normalized.erase(dot);
		}
		return normalized;
	}

	bool EmissiveMaterialResponseRuleMatchesSurface(
		const SceneLightSystem::EmissiveMaterialResponseRule& rule,
		const SceneLightSystem::SurfaceRecord& record)
	{
		for (uint32_t textureId : rule.textureIds)
		{
			if (record.material.textureId == textureId)
			{
				return true;
			}
		}

		for (const auto& range : rule.textureRanges)
		{
			if (record.material.textureId >= range.first && record.material.textureId <= range.second)
			{
				return true;
			}
		}

		if (!rule.textureNames.empty())
		{
			const std::string textureName = NormalizeMaterialTextureName(record.material.texture);
			for (const std::string& ruleTextureName : rule.textureNames)
			{
				if (textureName == ruleTextureName)
				{
					return true;
				}
			}
		}

		return false;
	}

	void ApplyEmissiveMaterialResponseRule(
		const SceneLightSystem::EmissiveMaterialResponseRule& rule,
		SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& emissive)
	{
		const bool hasMaterialResponseControl =
			rule.hasMaterialResponse ||
			rule.hasMaterialResponseMin ||
			rule.hasMaterialResponseMax;
		if (hasMaterialResponseControl || !rule.hasVisibleGlowBlend)
		{
			if (rule.hasMaterialResponse)
			{
				emissive.materialResponseEnabled = rule.materialResponse;
				emissive.materialResponseExplicit = true;
			}
			else
			{
				emissive.materialResponseEnabled = true;
			}
			if (rule.hasMaterialResponseMin || rule.hasMaterialResponseMax)
			{
				emissive.hasMaterialResponseParams = true;
				emissive.hasMaterialResponseMin = rule.hasMaterialResponseMin;
				emissive.hasMaterialResponseMax = rule.hasMaterialResponseMax;
				if (rule.hasMaterialResponseMin)
				{
					emissive.materialResponseMin = std::max(0.0f, rule.materialResponseMin);
				}
				if (rule.hasMaterialResponseMax)
				{
					emissive.materialResponseMax = std::max(0.0f, rule.materialResponseMax);
				}
			}
		}
	}

	void ApplyEmissiveOverrideRule(
		const SceneLightSystem::EmissiveOverrideRule& rule,
		SceneLightSystem::EmissiveSurfaceRegistry::EmissiveSurfaceRecord& emissive,
		const NRILightingSettings& settings)
	{
		emissive.sourceFlags |= SceneEmissiveSurfaceSourceFlag_LightOverlayOverride;
		emissive.overrideRuleId = rule.ruleId;
		if (rule.hasIntensityScale)
		{
			emissive.emissiveIntensity *= std::max(rule.intensityScale, 0.0f);
		}
		if (rule.hasReachScale)
		{
			emissive.reachScale *= std::max(rule.reachScale, 0.0f);
		}
		if (rule.hasSectorResponse)
		{
			emissive.sectorResponseEnabled = rule.sectorResponse;
		}
		if (rule.hasSignalSector)
		{
			emissive.sectorIndex = rule.signalSector;
			if (!rule.hasSectorResponse)
			{
				emissive.sectorResponseEnabled = true;
			}
		}
		if (rule.hasResponseIntensity || rule.hasResponseMin || rule.hasResponseMax || rule.hasResponseInputMin || rule.hasResponseInputMax)
		{
			emissive.hasSectorResponseParams = true;
			emissive.sectorResponseIntensity = rule.hasResponseIntensity ? rule.responseIntensity : std::max(0.0f, settings.sectorEmissionSignalStrength);
			emissive.sectorResponseMin = rule.hasResponseMin ? rule.responseMin : std::max(0.0f, settings.sectorEmissionResponseMin);
			emissive.sectorResponseMax = rule.hasResponseMax ? rule.responseMax : std::max(emissive.sectorResponseMin, settings.sectorEmissionResponseMax);
			emissive.hasSectorResponseInputRange = rule.hasResponseInputMin && rule.hasResponseInputMax;
			emissive.sectorResponseInputMin = rule.hasResponseInputMin ? rule.responseInputMin : 0.0f;
			emissive.sectorResponseInputMax = rule.hasResponseInputMax ? rule.responseInputMax : 1.0f;
		}
		if (rule.hasResponseIntensityMin)
		{
			emissive.hasSectorResponseIntensityMin = true;
			emissive.sectorResponseIntensityMin = rule.responseIntensityMin;
		}
		if (rule.hasResponseIntensityMax)
		{
			emissive.hasSectorResponseIntensityMax = true;
			emissive.sectorResponseIntensityMax = rule.responseIntensityMax;
		}
		if (rule.hasResponseReachMin)
		{
			emissive.hasSectorResponseReachMin = true;
			emissive.sectorResponseReachMin = rule.responseReachMin;
		}
		if (rule.hasResponseReachMax)
		{
			emissive.hasSectorResponseReachMax = true;
			emissive.sectorResponseReachMax = rule.responseReachMax;
		}
		if (rule.hasMaterialResponse)
		{
			emissive.materialResponseEnabled = rule.materialResponse;
			emissive.materialResponseExplicit = true;
		}
		if (rule.hasMaterialResponseMin || rule.hasMaterialResponseMax)
		{
			if (!rule.hasMaterialResponse)
			{
				emissive.materialResponseEnabled = true;
			}
			emissive.hasMaterialResponseParams = true;
			emissive.hasMaterialResponseMin = rule.hasMaterialResponseMin;
			emissive.hasMaterialResponseMax = rule.hasMaterialResponseMax;
			if (rule.hasMaterialResponseMin)
			{
				emissive.materialResponseMin = std::max(0.0f, rule.materialResponseMin);
			}
			if (rule.hasMaterialResponseMax)
			{
				emissive.materialResponseMax = std::max(0.0f, rule.materialResponseMax);
			}
		}
	}

	bool EvaluateEmissiveMaterial(
		const SceneLightSystem::EmissiveSurfaceRegistry& registry,
		const nri_scene::MaterialLightingMetadata& metadata,
		const NRILightingSettings& settings,
		uint32_t& outSourceFlags,
		uint32_t& outRuleId,
		float outColor[3],
		float& outIntensity,
		uint32_t& outMode,
		uint32_t& outTextureIndex,
		float& outSamplingScale,
		float& outFalloffScale)
	{
		outSourceFlags = SceneEmissiveSurfaceSourceFlag_None;
		outRuleId = 0;
		outColor[0] = 0.0f;
		outColor[1] = 0.0f;
		outColor[2] = 0.0f;
		outIntensity = 0.0f;
		outMode = nri_scene::MaterialEmissiveMode_None;
		outTextureIndex = UINT32_MAX;
		outSamplingScale = 1.0f;
		outFalloffScale = 1.0f;

		if ((metadata.lightingFlags & (nri_scene::MaterialLightingFlag_MaterialFullbright | nri_scene::MaterialLightingFlag_TextureFullbright)) != 0)
		{
			outSourceFlags |= SceneEmissiveSurfaceSourceFlag_AutoFullbright;
		}
		if ((metadata.lightingFlags & (nri_scene::MaterialLightingFlag_TextureGlowing | nri_scene::MaterialLightingFlag_TextureAutoGlowing)) != 0)
		{
			outSourceFlags |= SceneEmissiveSurfaceSourceFlag_AutoTextureGlow;
		}
		if ((metadata.lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0)
		{
			outSourceFlags |= SceneEmissiveSurfaceSourceFlag_AutoGlowmap;
		}

		if (metadata.emissiveMode != nri_scene::MaterialEmissiveMode_None && metadata.emissiveIntensity > 0.0f)
		{
			outMode = metadata.emissiveMode;
			outTextureIndex = metadata.emissiveTextureIndex;
			outIntensity = metadata.emissiveIntensity;
			Copy3f(metadata.emissiveColor, outColor);
		}

		for (const auto& rule : registry.textureRules)
		{
			if (metadata.textureId != rule.textureId)
			{
				continue;
			}

			outSourceFlags |= SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule;
			outRuleId = rule.ruleId;
			const float baseIntensity = outIntensity > 0.0f ? outIntensity : 1.0f;
			switch (rule.emissiveMode)
			{
			case nri_scene::MaterialEmissiveMode_UseBaseTexture:
				outMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
				outTextureIndex = metadata.textureIndex;
				outColor[0] = metadata.averageColor[0];
				outColor[1] = metadata.averageColor[1];
				outColor[2] = metadata.averageColor[2];
				break;
			case nri_scene::MaterialEmissiveMode_UseGlowmapTexture:
				if (metadata.glowmapTextureIndex != UINT32_MAX)
				{
					outMode = nri_scene::MaterialEmissiveMode_UseGlowmapTexture;
					outTextureIndex = metadata.glowmapTextureIndex;
				}
				else
				{
					outMode = nri_scene::MaterialEmissiveMode_UseConstantColor;
				}
				outColor[0] = metadata.glowColor[0] > 0.0f ? metadata.glowColor[0] : metadata.averageColor[0];
				outColor[1] = metadata.glowColor[1] > 0.0f ? metadata.glowColor[1] : metadata.averageColor[1];
				outColor[2] = metadata.glowColor[2] > 0.0f ? metadata.glowColor[2] : metadata.averageColor[2];
				break;
			case nri_scene::MaterialEmissiveMode_UseConstantColor:
				outMode = nri_scene::MaterialEmissiveMode_UseConstantColor;
				if (rule.hasExplicitColor)
				{
					Copy3f(rule.emissiveColor, outColor);
				}
				else
				{
					outColor[0] = metadata.glowColor[0] > 0.0f ? metadata.glowColor[0] : metadata.averageColor[0];
					outColor[1] = metadata.glowColor[1] > 0.0f ? metadata.glowColor[1] : metadata.averageColor[1];
					outColor[2] = metadata.glowColor[2] > 0.0f ? metadata.glowColor[2] : metadata.averageColor[2];
				}
				break;
			default:
				break;
			}
			outIntensity = baseIntensity * std::max(rule.intensityScale, 0.0f);
			break;
		}

		if (outMode == nri_scene::MaterialEmissiveMode_None || outIntensity <= 0.0f)
		{
			return false;
		}

		outIntensity *= ResolveGlowStrengthScale(outSourceFlags, outMode, settings);
		outSamplingScale = ResolveGlowSamplingScale(outSourceFlags, outMode, settings);
		outFalloffScale = ResolveGlowFalloffScale(outSourceFlags, outMode, settings);
		return outIntensity > 0.0f;
	}

	uint32_t GetGameplayLightTimeIndexForSceneLights()
	{
		return PlayClock > 0 ? (uint32_t)(PlayClock / 4) : 0u;
	}

	double GetCurrentGameplayTimeSecondsForSceneLights()
	{
		return PlayClock > 0 ? (double)PlayClock * (1.0 / 120.0) : 0.0;
	}

	uint64_t QuantizeLightOverlayPositionKey(const float position[3])
	{
		const int64_t x = (int64_t)std::llround(position[0] * 16.0f);
		const int64_t y = (int64_t)std::llround(position[1] * 16.0f);
		const int64_t z = (int64_t)std::llround(position[2] * 16.0f);
		uint64_t key = 1469598103934665603ull;
		key = nri_scene::HashCombine64(key, (uint64_t)x);
		key = nri_scene::HashCombine64(key, (uint64_t)y);
		key = nri_scene::HashCombine64(key, (uint64_t)z);
		return key;
	}

	void ComputeCapturedSurfaceCenter(const nri_scene::SurfaceRef& surface, float outCenter[3])
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		if (surface.vertices.empty())
		{
			return;
		}

		for (const nri_scene::CapturedVertex& vertex : surface.vertices)
		{
			outCenter[0] += vertex.position[0];
			outCenter[1] += vertex.position[1];
			outCenter[2] += vertex.position[2];
		}

		const float invCount = 1.0f / (float)surface.vertices.size();
		outCenter[0] *= invCount;
		outCenter[1] *= invCount;
		outCenter[2] *= invCount;
	}

	uint32_t BuildActorOverlayRuleId(const ResolvedLightOverlayActorRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.actorClassName.GetChars(), rule.source);
	}

	bool IsSupportedActorOverlayRule(const ResolvedLightOverlayActorRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0;
	}

	bool TryBuildActorAnalyticOverlayRule(
		const ResolvedLightOverlayActorRule& resolvedRule,
		SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule& actorRule)
	{
		if (!resolvedRule.actorClassResolved ||
			resolvedRule.actorClass == nullptr ||
			!IsSupportedActorOverlayRule(resolvedRule) ||
			resolvedRule.intensity <= 0.0f ||
			resolvedRule.radius <= 0.0f)
		{
			return false;
		}

		actorRule = {};
		actorRule.ruleId = BuildActorOverlayRuleId(resolvedRule);
		actorRule.ruleName = resolvedRule.id.GetChars();
		actorRule.hasTileFilter = resolvedRule.hasTileFilter;
		actorRule.tileFilter = resolvedRule.hasTileFilter && resolvedRule.tileFilter >= 0 ? (uint32_t)resolvedRule.tileFilter : 0u;
		const bool lightCastsShadow = resolvedRule.hasLightShadowCast
			? resolvedRule.lightShadowCast
			: (!resolvedRule.hasShadowCast || resolvedRule.shadowCast);
		actorRule.flags = lightCastsShadow ? SceneAnalyticLightFlag_CastsShadow : SceneAnalyticLightFlag_None;
		actorRule.materialNoShadowReceive = resolvedRule.hasShadowReceive && !resolvedRule.shadowReceive;
		actorRule.materialNoShadowCast = resolvedRule.hasShadowCast && !resolvedRule.shadowCast;
		actorRule.materialFullbright = resolvedRule.hasFullbright && resolvedRule.fullbright;
		actorRule.activateImmediately = resolvedRule.activationPolicy == LightOverlayActorActivationPolicy::Immediate;
		actorRule.color[0] = resolvedRule.color[0];
		actorRule.color[1] = resolvedRule.color[1];
		actorRule.color[2] = resolvedRule.color[2];
		actorRule.intensity = resolvedRule.intensity;
		actorRule.radius = resolvedRule.radius;
		actorRule.offset[0] = resolvedRule.offset[0];
		actorRule.offset[1] = resolvedRule.offset[1];
		actorRule.offset[2] = resolvedRule.offset[2];
		actorRule.hasNudgeFromSurface = resolvedRule.hasNudgeFromSurface && resolvedRule.nudgeFromSurfaceDistance > 0.0f;
		actorRule.nudgeFromSurfaceDistance = resolvedRule.nudgeFromSurfaceDistance;
		actorRule.flickerFrames = resolvedRule.flickerFrames;
		actorRule.hasRandomIntensity = resolvedRule.hasRandom;
		actorRule.randomIntensityRange[0] = resolvedRule.randomIntensityRange[0];
		actorRule.randomIntensityRange[1] = resolvedRule.randomIntensityRange[1];
		return true;
	}

	void BuildActorAnalyticOverlayRules(
		const ResolvedLightOverlaySet& resolved,
		std::unordered_map<int32_t, std::vector<SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule>>& outRules)
	{
		if (resolved.actorRules.Size() == 0)
		{
			return;
		}

		TSpriteIterator<DCoreActor> it;
		while (auto actor = it.Next())
		{
			if (actor == nullptr ||
				!actor->exists() ||
				(actor->ObjectFlags & OF_EuthanizeMe) != 0)
			{
				continue;
			}

			PClass* actorClass = actor->GetClass();
			if (actorClass == nullptr)
			{
				continue;
			}

			auto& actorRules = outRules[(int32_t)actor->GetIndex()];
			for (const auto& resolvedRule : resolved.actorRules)
			{
				if (!resolvedRule.actorClassResolved ||
					resolvedRule.actorClass == nullptr ||
					(actorClass != resolvedRule.actorClass && !actorClass->IsDescendantOf(resolvedRule.actorClass)))
				{
					continue;
				}

				SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule actorRule = {};
				if (TryBuildActorAnalyticOverlayRule(resolvedRule, actorRule))
				{
					actorRule.actorClassName = actorClass->TypeName.GetChars();
					actorRule.actorIndex = (int32_t)actor->GetIndex();
					actorRule.actorPalette = actor->spr.pal;
					WorldToPathTracingPosition(actor->spr.pos, actorRule.actorPosition);
					const FTextureID liveTextureId = actor->dispictex.isValid() ? actor->dispictex : actor->spr.spritetexture();
					actorRule.actorTextureId = liveTextureId.isValid() ? (uint32_t)liveTextureId.GetIndex() : 0u;
					actorRules.push_back(actorRule);
				}
			}

			if (actorRules.empty())
			{
				outRules.erase((int32_t)actor->GetIndex());
			}
		}
	}

	void BuildActorAnalyticOverlayRuleLookup(
		const ResolvedLightOverlaySet& resolved,
		std::unordered_map<uint32_t, SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule>& outRulesById)
	{
		for (const auto& resolvedRule : resolved.actorRules)
		{
			SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule actorRule = {};
			if (TryBuildActorAnalyticOverlayRule(resolvedRule, actorRule))
			{
				outRulesById[actorRule.ruleId] = actorRule;
			}
		}
	}

	bool IsSupportedMapOverlayRule(const ResolvedLightOverlayMapLightRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0;
	}

	bool IsSupportedSurfaceLightRule(const ResolvedLightOverlaySurfaceLightRule& rule)
	{
		return rule.lightType.IsEmpty() || rule.lightType.CompareNoCase("point") == 0 || rule.lightType.CompareNoCase("rect") == 0;
	}

	uint32_t BuildMapOverlayRuleId(const ResolvedLightOverlayMapLightRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	uint32_t BuildSurfaceLightRuleId(const ResolvedLightOverlaySurfaceLightRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	uint32_t BuildEmissiveOverrideRuleId(const ResolvedLightOverlayEmissiveOverrideRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	std::string NormalizeLightOverlayTextureSelector(const char* value)
	{
		std::string normalized = value != nullptr ? value : "";
		for (char& c : normalized)
		{
			c = (char)std::tolower((unsigned char)c);
		}

		const size_t slash = normalized.find_last_of("/\\");
		const size_t dot = normalized.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		{
			normalized.erase(dot);
		}
		return normalized;
	}

	uint64_t BuildMapOverlayStableKey(uint32_t ruleId, const float position[3])
	{
		uint64_t key = 1469598103934665603ull;
		key = nri_scene::HashCombine64(key, (uint64_t)ruleId);
		key = nri_scene::HashCombine64(key, QuantizeLightOverlayPositionKey(position));
		return key;
	}

	void ConvertMapOverlayWorldVectorToPathTracing(const float source[3], float destination[3])
	{
		destination[0] = source[0];
		destination[1] = -source[2];
		destination[2] = -source[1];
	}

	bool TryResolveSectorMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex, float outPosition[3])
	{
		const nri_scene::PTMapChunk* matchedChunk = nullptr;
		for (const auto& chunk : mapWorld.chunks)
		{
			if (chunk.sectorIndex == sectorIndex)
			{
				matchedChunk = &chunk;
				break;
			}
		}
		if (matchedChunk == nullptr)
		{
			return false;
		}

		float flatCenterSum[3] = {};
		int flatCenterCount = 0;
		float anyCenterSum[3] = {};
		int anyCenterCount = 0;
		const uint32_t endSurface = matchedChunk->firstSurface + matchedChunk->surfaceCount;
		for (uint32_t surfaceIndex = matchedChunk->firstSurface; surfaceIndex < endSurface && surfaceIndex < mapWorld.surfaces.size(); ++surfaceIndex)
		{
			const auto& surface = mapWorld.surfaces[surfaceIndex].surface;
			if (surface.provenance.sectorIndex != sectorIndex)
			{
				continue;
			}

			float center[3] = {};
			ComputeCapturedSurfaceCenter(surface, center);
			anyCenterSum[0] += center[0];
			anyCenterSum[1] += center[1];
			anyCenterSum[2] += center[2];
			anyCenterCount++;

			if (surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapFloorSection ||
				surface.provenance.sourceType == nri_scene::SurfaceSourceType::MapCeilingSection)
			{
				flatCenterSum[0] += center[0];
				flatCenterSum[1] += center[1];
				flatCenterSum[2] += center[2];
				flatCenterCount++;
			}
		}

		const float* sum = flatCenterCount > 0 ? flatCenterSum : anyCenterSum;
		const int count = flatCenterCount > 0 ? flatCenterCount : anyCenterCount;
		if (count <= 0)
		{
			return false;
		}

		const float invCount = 1.0f / (float)count;
		outPosition[0] = sum[0] * invCount;
		outPosition[1] = sum[1] * invCount;
		outPosition[2] = sum[2] * invCount;
		return true;
	}

	bool TryResolveWallMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, int32_t wallIndex, float outPosition[3])
	{
		float centerSum[3] = {};
		int centerCount = 0;
		for (const auto& mapSurface : mapWorld.surfaces)
		{
			if (mapSurface.surface.provenance.wallIndex != wallIndex)
			{
				continue;
			}

			float center[3] = {};
			ComputeCapturedSurfaceCenter(mapSurface.surface, center);
			centerSum[0] += center[0];
			centerSum[1] += center[1];
			centerSum[2] += center[2];
			centerCount++;
		}

		if (centerCount <= 0)
		{
			return false;
		}

		const float invCount = 1.0f / (float)centerCount;
		outPosition[0] = centerSum[0] * invCount;
		outPosition[1] = centerSum[1] * invCount;
		outPosition[2] = centerSum[2] * invCount;
		return true;
	}

	bool TryResolveMapOverlayAnchorPosition(const nri_scene::PTMapWorld& mapWorld, const ResolvedLightOverlayMapLightRule& rule, float outPosition[3])
	{
		switch (rule.anchorType)
		{
		case LightOverlayAnchorType::Position:
			if (!rule.hasAnchorPosition)
			{
				return false;
			}
			ConvertMapOverlayWorldVectorToPathTracing(rule.anchorPosition, outPosition);
			return true;

		case LightOverlayAnchorType::Sector:
			return rule.anchorIndex >= 0 && TryResolveSectorMapOverlayAnchorPosition(mapWorld, rule.anchorIndex, outPosition);

		case LightOverlayAnchorType::Wall:
			return rule.anchorIndex >= 0 && TryResolveWallMapOverlayAnchorPosition(mapWorld, rule.anchorIndex, outPosition);

		default:
			return false;
		}
	}

	void BuildStaticMapAnalyticOverlayRules(
		const ResolvedLightOverlaySet& resolved,
		const nri_scene::PTMapWorld& mapWorld,
		std::vector<SceneLightSystem::AnalyticLightRegistry::MapOverlayRule>& outRules)
	{
		for (const auto& resolvedRule : resolved.mapLightRules)
		{
			if (!IsSupportedMapOverlayRule(resolvedRule) ||
				resolvedRule.intensity <= 0.0f ||
				resolvedRule.radius <= 0.0f)
			{
				continue;
			}

			float anchorPosition[3] = {};
			if (!TryResolveMapOverlayAnchorPosition(mapWorld, resolvedRule, anchorPosition))
			{
				continue;
			}

			SceneLightSystem::AnalyticLightRegistry::MapOverlayRule overlayRule = {};
			float offset[3] = {};
			ConvertMapOverlayWorldVectorToPathTracing(resolvedRule.offset, offset);
			overlayRule.ruleId = BuildMapOverlayRuleId(resolvedRule);
			overlayRule.source = SceneLightRecordSource::StaticMapScene;
			overlayRule.position[0] = anchorPosition[0] + offset[0];
			overlayRule.position[1] = anchorPosition[1] + offset[1];
			overlayRule.position[2] = anchorPosition[2] + offset[2];
			overlayRule.stableKey = BuildMapOverlayStableKey(overlayRule.ruleId, overlayRule.position);
			overlayRule.color[0] = resolvedRule.color[0];
			overlayRule.color[1] = resolvedRule.color[1];
			overlayRule.color[2] = resolvedRule.color[2];
			overlayRule.intensity = resolvedRule.intensity;
			overlayRule.radius = resolvedRule.radius;
			overlayRule.flickerFrames = resolvedRule.flickerFrames;
			outRules.push_back(overlayRule);
		}

		for (const auto& resolvedRule : resolved.surfaceLightRules)
		{
			if (!IsSupportedSurfaceLightRule(resolvedRule) ||
				!resolvedRule.hasPosition ||
				!resolvedRule.hasNormal ||
				resolvedRule.intensity <= 0.0f ||
				resolvedRule.radius <= 0.0f)
			{
				continue;
			}

			SceneLightSystem::AnalyticLightRegistry::MapOverlayRule overlayRule = {};
			const float offset = resolvedRule.hasOffset ? resolvedRule.offset : 0.0f;
			overlayRule.ruleId = BuildSurfaceLightRuleId(resolvedRule);
			overlayRule.source = SceneLightRecordSource::DynamicScene;
			overlayRule.position[0] = resolvedRule.position[0] + resolvedRule.normal[0] * offset;
			overlayRule.position[1] = resolvedRule.position[1] + resolvedRule.normal[1] * offset;
			overlayRule.position[2] = resolvedRule.position[2] + resolvedRule.normal[2] * offset;
			overlayRule.stableKey = BuildMapOverlayStableKey(overlayRule.ruleId, overlayRule.position);
			overlayRule.color[0] = resolvedRule.color[0];
			overlayRule.color[1] = resolvedRule.color[1];
			overlayRule.color[2] = resolvedRule.color[2];
			overlayRule.intensity = resolvedRule.intensity;
			overlayRule.radius = resolvedRule.radius;
			overlayRule.hasSectorResponse = resolvedRule.hasSectorResponse;
			overlayRule.sectorResponse = resolvedRule.sectorResponse;
			overlayRule.hasSignalSector = resolvedRule.hasSignalSector;
			overlayRule.signalSector = resolvedRule.signalSector;
			overlayRule.hasResponseIntensity = resolvedRule.hasResponseIntensity;
			overlayRule.responseIntensity = resolvedRule.responseIntensity;
			overlayRule.hasResponseMin = resolvedRule.hasResponseMin;
			overlayRule.responseMin = resolvedRule.responseMin;
			overlayRule.hasResponseMax = resolvedRule.hasResponseMax;
			overlayRule.responseMax = resolvedRule.responseMax;
			overlayRule.hasResponseInputMin = resolvedRule.hasResponseInputMin;
			overlayRule.responseInputMin = resolvedRule.responseInputMin;
			overlayRule.hasResponseInputMax = resolvedRule.hasResponseInputMax;
			overlayRule.responseInputMax = resolvedRule.responseInputMax;
			outRules.push_back(overlayRule);
		}
	}

	void BuildEmissiveOverrideRules(
		const ResolvedLightOverlaySet& resolved,
		std::vector<SceneLightSystem::EmissiveOverrideRule>& outRules)
	{
		outRules.clear();
		outRules.reserve((size_t)resolved.emissiveOverrideRules.Size());
		for (const auto& resolvedRule : resolved.emissiveOverrideRules)
		{
			if (!resolvedRule.hasSectorFilter &&
				!resolvedRule.hasWallFilter &&
				!resolvedRule.hasTileFilter)
			{
				continue;
			}

			SceneLightSystem::EmissiveOverrideRule rule = {};
			rule.ruleId = BuildEmissiveOverrideRuleId(resolvedRule);
			rule.hasSectorFilter = resolvedRule.hasSectorFilter;
			rule.sectorFilter = resolvedRule.sectorFilter;
			rule.hasWallFilter = resolvedRule.hasWallFilter;
			rule.wallFilter = resolvedRule.wallFilter;
			rule.hasTileFilter = resolvedRule.hasTileFilter && resolvedRule.tileFilter >= 0;
			rule.tileFilter = rule.hasTileFilter ? (uint32_t)resolvedRule.tileFilter : 0u;
			rule.hasIntensityScale = resolvedRule.hasIntensityScale;
			rule.intensityScale = resolvedRule.intensityScale;
			rule.hasReachScale = resolvedRule.hasReachScale;
			rule.reachScale = resolvedRule.reachScale;
			rule.hasSectorResponse = resolvedRule.hasSectorResponse;
			rule.sectorResponse = resolvedRule.sectorResponse;
			rule.hasSignalSector = resolvedRule.hasSignalSector && resolvedRule.signalSector >= 0;
			rule.signalSector = rule.hasSignalSector ? resolvedRule.signalSector : -1;
			rule.hasResponseIntensity = resolvedRule.hasResponseIntensity;
			rule.responseIntensity = resolvedRule.responseIntensity;
			rule.hasResponseMin = resolvedRule.hasResponseMin;
			rule.responseMin = resolvedRule.responseMin;
			rule.hasResponseMax = resolvedRule.hasResponseMax;
			rule.responseMax = resolvedRule.responseMax;
			rule.hasResponseInputMin = resolvedRule.hasResponseInputMin;
			rule.responseInputMin = resolvedRule.responseInputMin;
			rule.hasResponseInputMax = resolvedRule.hasResponseInputMax;
			rule.responseInputMax = resolvedRule.responseInputMax;
			rule.hasResponseIntensityMin = resolvedRule.hasResponseIntensityMin;
			rule.responseIntensityMin = resolvedRule.responseIntensityMin;
			rule.hasResponseIntensityMax = resolvedRule.hasResponseIntensityMax;
			rule.responseIntensityMax = resolvedRule.responseIntensityMax;
			rule.hasResponseReachMin = resolvedRule.hasResponseReachMin;
			rule.responseReachMin = resolvedRule.responseReachMin;
			rule.hasResponseReachMax = resolvedRule.hasResponseReachMax;
			rule.responseReachMax = resolvedRule.responseReachMax;
			rule.hasMaterialResponse = resolvedRule.hasMaterialResponse;
			rule.materialResponse = resolvedRule.materialResponse;
			rule.hasMaterialResponseMin = resolvedRule.hasMaterialResponseMin;
			rule.materialResponseMin = resolvedRule.materialResponseMin;
			rule.hasMaterialResponseMax = resolvedRule.hasMaterialResponseMax;
			rule.materialResponseMax = resolvedRule.materialResponseMax;
			outRules.push_back(rule);
		}
	}

	void BuildSurfaceLightFixtureResponseRules(
		const ResolvedLightOverlaySet& resolved,
		std::vector<SceneLightSystem::EmissiveOverrideRule>& outRules)
	{
		outRules.clear();
		outRules.reserve((size_t)resolved.surfaceLightRules.Size());
		for (const auto& resolvedRule : resolved.surfaceLightRules)
		{
			if (!resolvedRule.hasPosition || !resolvedRule.hasNormal)
			{
				continue;
			}

			const bool sectorResponseEnabled = resolvedRule.hasSectorResponse && resolvedRule.sectorResponse;
			SceneLightSystem::EmissiveOverrideRule rule = {};
			rule.ruleId = BuildSurfaceLightRuleId(resolvedRule);
			rule.hasSectorResponse = true;
			rule.sectorResponse = sectorResponseEnabled;
			rule.hasSignalSector = resolvedRule.hasSignalSector && resolvedRule.signalSector >= 0;
			rule.signalSector = rule.hasSignalSector ? resolvedRule.signalSector : -1;
			rule.hasResponseIntensity = resolvedRule.hasResponseIntensity;
			rule.responseIntensity = resolvedRule.responseIntensity;
			rule.hasResponseMin = resolvedRule.hasResponseMin;
			rule.responseMin = resolvedRule.responseMin;
			rule.hasResponseMax = resolvedRule.hasResponseMax;
			rule.responseMax = resolvedRule.responseMax;
			rule.hasResponseInputMin = resolvedRule.hasResponseInputMin;
			rule.responseInputMin = resolvedRule.responseInputMin;
			rule.hasResponseInputMax = resolvedRule.hasResponseInputMax;
			rule.responseInputMax = resolvedRule.responseInputMax;
			if (resolvedRule.fixtureMaterialResponse && sectorResponseEnabled)
			{
				rule.hasMaterialResponse = true;
				rule.materialResponse = true;
				rule.hasMaterialResponseMin = resolvedRule.hasMaterialResponseMin;
				rule.materialResponseMin = resolvedRule.materialResponseMin;
				rule.hasMaterialResponseMax = resolvedRule.hasMaterialResponseMax;
				rule.materialResponseMax = resolvedRule.materialResponseMax;
			}
			outRules.push_back(rule);
		}
	}

	void BuildEmissiveMaterialResponseRules(
		const ResolvedLightOverlaySet& resolved,
		std::vector<SceneLightSystem::EmissiveMaterialResponseRule>& outRules)
	{
		outRules.clear();
		outRules.reserve((size_t)resolved.emissiveMaterialResponseRules.Size());
		for (const auto& resolvedRule : resolved.emissiveMaterialResponseRules)
		{
			SceneLightSystem::EmissiveMaterialResponseRule rule = {};
			rule.ruleId = BuildResolvedLightOverlayRuleId(resolvedRule.id.GetChars(), "", resolvedRule.source);
			rule.textureIds.reserve((size_t)resolvedRule.tileFilters.Size() + (size_t)resolvedRule.textureNames.Size());
			for (int tile : resolvedRule.tileFilters)
			{
				if (tile >= 0)
				{
					rule.textureIds.push_back((uint32_t)tile);
				}
			}
			rule.textureRanges.reserve((size_t)resolvedRule.tileRanges.Size());
			for (const auto& range : resolvedRule.tileRanges)
			{
				if (range.first >= 0 && range.last >= 0)
				{
					rule.textureRanges.emplace_back((uint32_t)range.first, (uint32_t)range.last);
				}
			}
			for (const auto& textureName : resolvedRule.textureNames)
			{
				rule.textureNames.push_back(NormalizeLightOverlayTextureSelector(textureName.GetChars()));
			}
			if (rule.textureIds.empty() && rule.textureRanges.empty() && rule.textureNames.empty())
			{
				continue;
			}
			rule.hasMaterialResponse = resolvedRule.hasMaterialResponse;
			rule.materialResponse = resolvedRule.materialResponse;
			rule.hasMaterialResponseMin = resolvedRule.hasMaterialResponseMin;
			rule.materialResponseMin = resolvedRule.materialResponseMin;
			rule.hasMaterialResponseMax = resolvedRule.hasMaterialResponseMax;
			rule.materialResponseMax = resolvedRule.materialResponseMax;
			rule.hasVisibleGlowBlend = resolvedRule.hasVisibleGlowBlend;
			rule.visibleGlowBlend = resolvedRule.visibleGlowBlend;
			outRules.push_back(rule);
		}
	}

	bool IsUsableDirectionalVector(const float direction[3])
	{
		if (!std::isfinite(direction[0]) || !std::isfinite(direction[1]) || !std::isfinite(direction[2]))
		{
			return false;
		}

		const float lengthSq = direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
		return lengthSq > 0.000001f;
	}

	float ClampDirectionalAngularSize(float angularSize)
	{
		if (!std::isfinite(angularSize))
		{
			return 0.03f;
		}

		return std::clamp(angularSize, 0.001f, 1.2f);
	}

	uint64_t QuantizeDirectionalLightScalar(float value, float scale)
	{
		return (uint64_t)(int64_t)std::llround((double)value * (double)scale);
	}

	uint64_t BuildDirectionalLightStateHash(const NRIDirectionalLightState& state)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = nri_scene::HashCombine64(hash, state.enabled ? 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, state.shadow ? 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, state.fromOverlay ? 1ull : 0ull);
		hash = nri_scene::HashCombine64(hash, (uint64_t)state.ruleId);
		hash = nri_scene::HashCombine64(hash, QuantizeDirectionalLightScalar(state.direction[0], 4096.0f));
		hash = nri_scene::HashCombine64(hash, QuantizeDirectionalLightScalar(state.direction[1], 4096.0f));
		hash = nri_scene::HashCombine64(hash, QuantizeDirectionalLightScalar(state.direction[2], 4096.0f));
		hash = nri_scene::HashCombine64(hash, QuantizeDirectionalLightScalar(state.color[0], 4096.0f));
		hash = nri_scene::HashCombine64(hash, QuantizeDirectionalLightScalar(state.color[1], 4096.0f));
		hash = nri_scene::HashCombine64(hash, QuantizeDirectionalLightScalar(state.color[2], 4096.0f));
		hash = nri_scene::HashCombine64(hash, QuantizeDirectionalLightScalar(state.angularSize, 4096.0f));
		return hash;
	}

	NRIDirectionalLightState BuildDirectionalLightState(const ResolvedLightOverlaySet& resolved, bool directionalLightEnabled)
	{
		NRIDirectionalLightState state = {};
		state.enabled = directionalLightEnabled;
		state.shadow = true;

		if (resolved.directionalRules.Size() > 0)
		{
			const ResolvedLightOverlayDirectionalRule& rule = resolved.directionalRules.Last();
			state.fromOverlay = true;
			state.ruleId = BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
			state.enabled = directionalLightEnabled;
			state.shadow = !rule.hasShadow || rule.shadow;

			if (rule.hasDirection && IsUsableDirectionalVector(rule.direction))
			{
				state.direction[0] = rule.direction[0];
				state.direction[1] = rule.direction[1];
				state.direction[2] = rule.direction[2];
				const float invLength = 1.0f / sqrtf(
					state.direction[0] * state.direction[0] +
					state.direction[1] * state.direction[1] +
					state.direction[2] * state.direction[2]);
				state.direction[0] *= invLength;
				state.direction[1] *= invLength;
				state.direction[2] *= invLength;
			}
			else
			{
				state.enabled = false;
				state.shadow = false;
			}

			if (rule.hasColor)
			{
				state.color[0] = std::max(rule.color[0], 0.0f);
				state.color[1] = std::max(rule.color[1], 0.0f);
				state.color[2] = std::max(rule.color[2], 0.0f);
			}

			const float intensity = rule.hasIntensity ? std::max(rule.intensity, 0.0f) : 1.0f;
			state.color[0] *= intensity;
			state.color[1] *= intensity;
			state.color[2] *= intensity;
			if (intensity <= 0.0f)
			{
				state.enabled = false;
				state.shadow = false;
			}

			if (rule.hasAngularSize)
			{
				state.angularSize = ClampDirectionalAngularSize(rule.angularSize);
			}
		}

		if (!state.enabled)
		{
			state.color[0] = 0.0f;
			state.color[1] = 0.0f;
			state.color[2] = 0.0f;
		}

		state.stateHash = BuildDirectionalLightStateHash(state);
		return state;
	}

	std::string FormatTopologyKeyListForSceneLightRefresh(const std::vector<uint64_t>& keys, size_t limit = 8)
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
}

void NRIRenderer::RefreshSceneLightSystem(
	bool usedStaticMapScene,
	const nri_scene::SceneView* capturedSceneView,
	const nri_scene::MaterialBridgeData* capturedMaterials,
	const nri_scene::SceneView* dynamicSceneView,
	const nri_scene::MaterialBridgeData* dynamicMaterials,
	const nri_scene::SceneView* surfaceLightSceneView,
	const nri_scene::MaterialBridgeData* surfaceLightMaterials,
	bool appendPersistentVoxelSceneLights,
	const TArray<PathTracingWeaponLightEvent>* weaponEvents)
{
	nri_runtime_mutation::ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.sceneLightsMs);
	SceneLightSystem::FrameAssemblyInput assemblyInput = {};
	assemblyInput.frameSerial = mFrameIndex;
	assemblyInput.frameIndex = mFrameIndex;
	assemblyInput.voxelStats = (bool)nri_voxelstats;
	assemblyInput.usedStaticMapScene = usedStaticMapScene;
	assemblyInput.staticScene = &mStaticMapScene;
	assemblyInput.capturedSceneView = capturedSceneView;
	assemblyInput.capturedMaterials = capturedMaterials;
	assemblyInput.dynamicSceneView = dynamicSceneView;
	assemblyInput.dynamicMaterials = dynamicMaterials;
	assemblyInput.surfaceLightSceneView = surfaceLightSceneView;
	assemblyInput.surfaceLightMaterials = surfaceLightMaterials;
	assemblyInput.appendPersistentVoxelSceneLights = appendPersistentVoxelSceneLights;
	assemblyInput.suppressedActorIndices = mPersistentVoxels.GetSuppressedActorIndices(mFrameIndex);

	SceneLightSystem::FrameAssemblyServices assemblyServices = {};
	assemblyServices.runtimeMutationUser = &mRuntimeMutation;
	assemblyServices.isRuntimeMutationReplacementActive = [](void* user, uint32_t mapChunkIndex) -> bool
	{
		return static_cast<NRIRuntimeMutationSystem*>(user)->IsReplacementActiveAndValid(mapChunkIndex);
	};
	assemblyServices.appendRuntimeMutationSceneLightRecords = [](void* user, SceneLightSystem& sceneLights)
	{
		static_cast<NRIRuntimeMutationSystem*>(user)->AppendSceneLightRecords(sceneLights);
	};
	assemblyServices.persistentVoxelUser = &mPersistentVoxels;
	assemblyServices.appendPersistentVoxelSceneLights = [](void* user, SceneLightSystem& sceneLights, uint32_t frameIndex, bool voxelStats)
	{
		static_cast<NRIPersistentVoxelResidency*>(user)->AppendSceneLights(sceneLights, frameIndex, voxelStats);
	};

	const SceneLightSystem::FrameAssemblyTimingStats assemblyTimings =
		mSceneLights.AssembleFrameSurfaceRecords(assemblyInput, assemblyServices);
	mLastPerfShellTraceStats.sceneLightStaticAppendMs += assemblyTimings.staticAppendMs;
	mLastPerfShellTraceStats.sceneLightRuntimeMutationAppendMs += assemblyTimings.runtimeMutationAppendMs;
	mLastPerfShellTraceStats.sceneLightCapturedAppendMs += assemblyTimings.capturedAppendMs;
	mLastPerfShellTraceStats.sceneLightDynamicAppendMs += assemblyTimings.dynamicAppendMs;
	mLastPerfShellTraceStats.sceneLightDynamicAppendMs += assemblyTimings.surfaceLightOverlayAppendMs;
	mLastPerfShellTraceStats.sceneLightPersistentVoxelAppendMs += assemblyTimings.persistentVoxelAppendMs;

	const ResolvedLightOverlaySet& resolvedLightOverlays = GetResolvedLightOverlaySet();
	const bool resolvedGenerationChanged =
		resolvedLightOverlays.resolvedGeneration != mLastResolvedLightOverlayGeneration;
	const bool muzzleFlashLookupNeedsRefresh =
		resolvedGenerationChanged ||
		mSceneLights.GetResolvedMuzzleFlashRuleCount() != (size_t)resolvedLightOverlays.muzzleFlashRules.Size();
	if (muzzleFlashLookupNeedsRefresh)
	{
		const bool hadPreviousGeneration = mLastResolvedLightOverlayGeneration != 0;
		if (resolvedGenerationChanged && hadPreviousGeneration)
		{
			ResetMuzzleFlashOverlayState("lightoverlay-resolve");
		}
		else if (resolvedLightOverlays.resolvedGeneration == 0)
		{
			ResetMuzzleFlashOverlayState("lightoverlay-empty");
		}

		mSceneLights.RefreshResolvedMuzzleFlashRuleLookup(resolvedLightOverlays);
		mLastResolvedLightOverlayGeneration = resolvedLightOverlays.resolvedGeneration;
		Printf("NRI PT muzzle-flash rules: generation=%u count=%u ids=%s\n",
			resolvedLightOverlays.resolvedGeneration,
			(unsigned)mSceneLights.GetResolvedMuzzleFlashRuleCount(),
			mSceneLights.FormatResolvedMuzzleFlashRuleIdList().c_str());

		if (resolvedGenerationChanged && hadPreviousGeneration)
		{
			NoteLightHistoryChange("lightoverlay-resolve");
		}
	}

	const NRIDirectionalLightState nextDirectionalLightState = BuildDirectionalLightState(resolvedLightOverlays, nri_ptdirectionallight);
	const bool directionalLightStateChanged =
		!mHasDirectionalLightState ||
		nextDirectionalLightState.stateHash != mDirectionalLightState.stateHash;
	const bool hadDirectionalLightState = mHasDirectionalLightState;
	mDirectionalLightState = nextDirectionalLightState;
	mHasDirectionalLightState = true;
	std::unordered_map<int32_t, std::vector<SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule>> actorOverlayRules;
	std::unordered_map<uint32_t, SceneLightSystem::AnalyticLightRegistry::ActorOverlayRule> actorOverlayRulesById;
	std::vector<SceneLightSystem::AnalyticLightRegistry::MapOverlayRule> mapOverlayRules;
	std::vector<SceneLightSystem::EmissiveOverrideRule> emissiveOverrideRules;
	std::vector<SceneLightSystem::EmissiveOverrideRule> surfaceLightFixtureRules;
	std::vector<SceneLightSystem::EmissiveMaterialResponseRule> emissiveMaterialResponseRules;
	BuildActorAnalyticOverlayRules(resolvedLightOverlays, actorOverlayRules);
	BuildActorAnalyticOverlayRuleLookup(resolvedLightOverlays, actorOverlayRulesById);
	BuildEmissiveOverrideRules(resolvedLightOverlays, emissiveOverrideRules);
	BuildSurfaceLightFixtureResponseRules(resolvedLightOverlays, surfaceLightFixtureRules);
	BuildEmissiveMaterialResponseRules(resolvedLightOverlays, emissiveMaterialResponseRules);
	if (mMapWorld.valid)
	{
		BuildStaticMapAnalyticOverlayRules(resolvedLightOverlays, mMapWorld, mapOverlayRules);
	}
	const uint32_t gameplayLightTimeIndex = GetGameplayLightTimeIndexForSceneLights();
	const double currentTimeSeconds = GetCurrentGameplayTimeSecondsForSceneLights();
	static const TArray<PathTracingWeaponLightEvent> emptyWeaponEvents;
	mSceneLights.RefreshTransientMuzzleFlashLights(
		currentTimeSeconds,
		weaponEvents != nullptr ? *weaponEvents : emptyWeaponEvents,
		nri_ptdebug > 0 || (int)nri_ptsmoketrace > 0);

	{
		nri_runtime_mutation::ScopedPtPerfTimer rebuildTimer(mLastPerfShellTraceStats.sceneLightAnalyticMs);
		mSceneLights.RebuildAnalyticLights(
			gameplayLightTimeIndex,
			mFrameIndex,
			NRI_MAX_RUNTIME_POINT_LIGHTS,
			mCurrentCameraPos,
			actorOverlayRules.empty() ? nullptr : &actorOverlayRules,
			actorOverlayRulesById.empty() ? nullptr : &actorOverlayRulesById,
			mapOverlayRules.empty() ? nullptr : &mapOverlayRules);
	}
	{
		nri_runtime_mutation::ScopedPtPerfTimer rebuildTimer(mLastPerfShellTraceStats.sceneLightEmissiveMs);
		mSceneLights.RebuildEmissiveSurfaces(
			NRI_MAX_EMISSIVE_SURFACES_FOR_REFRESH,
			emissiveOverrideRules.empty() ? nullptr : &emissiveOverrideRules,
			surfaceLightFixtureRules.empty() ? nullptr : &surfaceLightFixtureRules,
			emissiveMaterialResponseRules.empty() ? nullptr : &emissiveMaterialResponseRules);
	}
	{
		nri_runtime_mutation::ScopedPtPerfTimer rebuildTimer(mLastPerfShellTraceStats.sceneLightSectorMs);
		mSceneLights.RebuildSectorLighting(gameplayLightTimeIndex, (uint32_t)sector.Size());
	}
	TraceEmissiveSectorResponseChange();
	const auto& frameAppendStats = mSceneLights.GetFrameAppendStats();
	mLastPerfShellTraceStats.sceneLightSurfaceRecordCount = frameAppendStats.totalRecordCount;
	mLastPerfShellTraceStats.sceneLightStaticRecordCount = frameAppendStats.staticRecordCount;
	mLastPerfShellTraceStats.sceneLightRuntimeMutationRecordCount = frameAppendStats.runtimeMutationRecordCount;
	mLastPerfShellTraceStats.sceneLightDynamicRecordCount = frameAppendStats.dynamicRecordCount;
	mLastPerfShellTraceStats.sceneLightDynamicRecordCount += frameAppendStats.surfaceLightOverlayRecordCount;
	mLastPerfShellTraceStats.sceneLightCapturedRecordCount = frameAppendStats.capturedRecordCount;
	mLastPerfShellTraceStats.sceneLightPersistentVoxelRecordCount = frameAppendStats.persistentVoxelRecordCount;
	const auto& analyticStats = mSceneLights.GetAnalyticLights();
	mLastPerfShellTraceStats.sceneLightSpriteTileRuleCount = analyticStats.spriteTileRuleCount;
	mLastPerfShellTraceStats.sceneLightSpriteRecordCandidateScans = analyticStats.spriteRecordCandidateScans;
	mLastPerfShellTraceStats.sceneLightActorOverlayRuleCount = analyticStats.actorOverlayRuleCount;
	mLastPerfShellTraceStats.sceneLightActorOverlaySurfaceLookups = analyticStats.actorOverlaySurfaceLookups;
	mLastPerfShellTraceStats.sceneLightActorOverlayFullRecordScans = analyticStats.actorOverlayFullRecordScans;
	mLastPerfShellTraceStats.sceneLightActorOverlaySurfaceCandidateScans = analyticStats.actorOverlaySurfaceCandidateScans;
	mLastPerfShellTraceStats.sceneLightActorOverlayIndexedCandidateCount = analyticStats.actorOverlayIndexedCandidateCount;
	mLastPerfShellTraceStats.sceneLightTopologyKeyCount = analyticStats.topologyKeyCount;
	mLastPerfShellTraceStats.sceneLightTopologyRebuildCount = analyticStats.topologyRebuildCount;
	mLastPerfShellTraceStats.sceneLightPropertyOnlyUpdateCount = analyticStats.propertyOnlyUpdateCount;
	mLastPerfShellTraceStats.sceneLightTopologySortSkippedCount = analyticStats.topologySortSkippedCount;
	mLastPerfShellTraceStats.sceneLightTopologyAddedKeyCount = analyticStats.topologyAddedKeyCount;
	mLastPerfShellTraceStats.sceneLightTopologyRemovedKeyCount = analyticStats.topologyRemovedKeyCount;
	mLastPerfShellTraceStats.sceneLightTopologyReboundKeyCount = analyticStats.topologyReboundKeyCount;
	mLastPerfShellTraceStats.sceneLightSoftLightCount = analyticStats.softLightCount;
	mLastPerfShellTraceStats.sceneLightSurvivingIndexChangeCount = analyticStats.survivingKeyIndexChangeCount;
	mLastPerfShellTraceStats.sceneLightSurvivingSoftIndexChangeCount = analyticStats.survivingSoftLightIndexChangeCount;
	mLastPerfShellTraceStats.sceneLightOrderedStableKeyHash = analyticStats.orderedStableKeyHash;
	mLastPerfShellTraceStats.sceneLightTopologySortMs = analyticStats.topologySortMs;
	if (hadDirectionalLightState && directionalLightStateChanged)
	{
		NoteLightHistoryChange("directional-light-change");
	}
	const bool analyticLightTopologyChanged = mSceneLights.ConsumeAnalyticLightTopologyChanged();
	const bool analyticLightPropertiesChanged = mSceneLights.ConsumeAnalyticLightPropertiesChanged();
	const bool emissiveSurfaceTopologyChanged = mSceneLights.ConsumeEmissiveSurfaceTopologyChanged();
	const bool emissiveSurfacePropertiesChanged = mSceneLights.ConsumeEmissiveSurfacePropertiesChanged();
	const bool emissiveMaterialBindingChanged = mSceneLights.ConsumeEmissiveMaterialBindingChanged();
	const bool emissiveMaterialPropertiesChanged = mSceneLights.ConsumeEmissiveMaterialPropertiesChanged();
	const bool sectorLightingTopologyChanged = mSceneLights.ConsumeSectorLightingTopologyChanged();

	if (analyticLightTopologyChanged || analyticLightPropertiesChanged)
	{
		InvalidateRuntimeLightSceneData();
	}
	if (analyticLightTopologyChanged)
	{
		if (ShouldTraceSkyPerf())
		{
			const auto& analyticLights = mSceneLights.GetAnalyticLights();
			Printf("NRI PT light topology analytic: frame=%u added=%s removed=%s rebound=%s\n",
				mFrameIndex,
				FormatTopologyKeyListForSceneLightRefresh(analyticLights.addedTopologyKeys).c_str(),
				FormatTopologyKeyListForSceneLightRefresh(analyticLights.removedTopologyKeys).c_str(),
				FormatTopologyKeyListForSceneLightRefresh(analyticLights.reboundTopologyKeys).c_str());
		}
		NoteLightHistoryChange("analytic-light-topology");
	}
	if (emissiveSurfaceTopologyChanged)
	{
		if (ShouldTraceSkyPerf())
		{
			const auto& emissiveSurfaces = mSceneLights.GetEmissiveSurfaces();
			Printf("NRI PT light topology emissive: frame=%u added=%s removed=%s rebound=%s\n",
				mFrameIndex,
				FormatTopologyKeyListForSceneLightRefresh(emissiveSurfaces.addedTopologyKeys).c_str(),
				FormatTopologyKeyListForSceneLightRefresh(emissiveSurfaces.removedTopologyKeys).c_str(),
				FormatTopologyKeyListForSceneLightRefresh(emissiveSurfaces.reboundTopologyKeys).c_str());
		}
		NoteLightHistoryChange("emissive-surface-topology");
	}
	if (emissiveMaterialBindingChanged)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.emissiveMaterialDirtyEvents++;
		}

		// Binding changes still require resident static materials to be refreshed,
		// but they no longer need to tear down the whole static scene cache.
		QueueStaticMapSceneLightingInvalidation();
	}
	if (sectorLightingTopologyChanged)
	{
		NoteLightHistoryChange("sector-light-topology");
	}

	(void)analyticLightPropertiesChanged;
	(void)emissiveSurfacePropertiesChanged;
	(void)emissiveMaterialPropertiesChanged;
}

NRILightingSettings SceneLightSystem::CaptureSettings()
{
	NRILightingSettings settings = {};
	settings.emissiveMinPower = (float)nri_ptemissiveminpower;
	settings.emissiveMinSurface = (float)nri_ptemissiveminsurface;
	settings.glowScale = (float)nri_ptglowscale;
	settings.glowReach = (float)nri_ptglowreach;
	settings.glowFalloff = (float)nri_ptglowfalloff;
	settings.sectorLighting = !!nri_ptsectorlighting;
	settings.sectorAmbientScale = (float)nri_ptsectorambientscale;
	settings.sectorHemisphereScale = (float)nri_ptsectorhemiscale;
	settings.sectorFogScale = (float)nri_ptsectorfogscale;
	settings.sectorClamp = (float)nri_ptsectorclamp;
	settings.sectorFilterPalette = (int)nri_ptsectorfilterpal;
	settings.sectorFilterMinShade = (int)nri_ptsectorfilterminshade;
	settings.sectorFilterMaxShade = (int)nri_ptsectorfiltermaxshade;
	settings.sectorFilterLotag = (int)nri_ptsectorfilterlotag;
	settings.sectorPulseFrames = (int)nri_ptsectorpulseframes;
	settings.sectorPulseAmount = (float)nri_ptsectorpulseamount;
	settings.sectorEmissionSignalStrength = (float)nri_ptsectoremissionsignalstrength;
	settings.sectorEmissionResponseMin = (float)nri_ptsectoremissionresponsemin;
	settings.sectorEmissionResponseMax = (float)nri_ptsectoremissionresponsemax;
	return settings;
}

void SceneLightSystem::Reset()
{
	mEmissiveSamplingDistribution.Reset();
	mAnalyticLights = {};
	mEmissiveSurfaces = {};
	mSectorLighting = {};
	mEnvironmentLighting = {};
	mPersistentDynamicEmissiveCache = {};
	mPersistentDynamicEmissiveHighWaterStats = {};
	mActorSpriteDebugStats = {};
	mSurfaceRecords.clear();
	mFrameAppendStats = {};
	mFrameSerial = 0;
	mActivatedActorOverlayKeys.clear();
	mPublishedActorOverlayIndices.clear();
	mSuppressedActorIndices.clear();
	mEmissiveStableSurfaceStates.clear();
	mResolvedMuzzleFlashRuleLookup.clear();
	mTransientMuzzleFlashSlots.clear();
	mTransientMuzzleFlashLights.clear();
	mLastMuzzleFlashEventSerial = 0;
	ResetEmissiveSectorResponseCaches();
}

void SceneLightSystem::ResetLevelState()
{
	mEmissiveSamplingDistribution.Reset();
	mAnalyticLights.manualLights.clear();
	mAnalyticLights.transientLights.clear();
	mAnalyticLights.activeLights.clear();
	mAnalyticLights.activeTopologyKeys.clear();
	mAnalyticLights.activePropertyHashes.clear();
	mAnalyticLights.activeBindingHashes.clear();
	mAnalyticLights.activeDiagnosticFlags.clear();
	mAnalyticLights.addedTopologyKeys.clear();
	mAnalyticLights.removedTopologyKeys.clear();
	mAnalyticLights.reboundTopologyKeys.clear();
	mAnalyticLights.matchedSurfaceCount = 0;
	mAnalyticLights.actorOverlayRuleCount = 0;
	mAnalyticLights.actorOverlayMatchedSurfaceCount = 0;
	mAnalyticLights.actorOverlayPublishedActorCount = 0;
	mAnalyticLights.actorOverlayPublishedFallbackActivationCount = 0;
	mAnalyticLights.mapOverlayRuleCount = 0;
	mAnalyticLights.transientMuzzleSlotCount = 0;
	mAnalyticLights.transientMuzzleActiveCount = 0;
	mAnalyticLights.dedupedMatchCount = 0;
	mAnalyticLights.truncatedLightCount = 0;
	mAnalyticLights.topologyChanged = false;
	mAnalyticLights.propertiesChanged = false;
	mAnalyticLights.lastBuildTopologyChanged = false;
	mAnalyticLights.lastBuildPropertiesChanged = false;

	mEmissiveSurfaces.activeSurfaces.clear();
	mEmissiveSurfaces.activeTopologyKeys.clear();
	mEmissiveSurfaces.activePropertyHashes.clear();
	mEmissiveSurfaces.activeBindingHashes.clear();
	mEmissiveSurfaces.activeDiagnosticFlags.clear();
	mEmissiveSurfaces.addedTopologyKeys.clear();
	mEmissiveSurfaces.removedTopologyKeys.clear();
	mEmissiveSurfaces.reboundTopologyKeys.clear();
	mEmissiveSurfaces.totalPowerEstimate = 0.0f;
	mEmissiveSurfaces.autoTaggedCount = 0;
	mEmissiveSurfaces.explicitRuleMatchCount = 0;
	mEmissiveSurfaces.overrideRuleCount = 0;
	mEmissiveSurfaces.overrideMatchedSurfaceCount = 0;
	mEmissiveSurfaces.materialResponseRuleCount = 0;
	mEmissiveSurfaces.materialResponseMatchedSurfaceCount = 0;
	mEmissiveSurfaces.truncatedSurfaceCount = 0;
	mEmissiveSurfaces.topologyChanged = false;
	mEmissiveSurfaces.propertiesChanged = false;
	mEmissiveSurfaces.materialBindingChanged = false;
	mEmissiveSurfaces.materialPropertiesChanged = false;
	mEmissiveSurfaces.lastBuildTopologyChanged = false;
	mEmissiveSurfaces.lastBuildPropertiesChanged = false;

	mSectorLighting = {};
	mEnvironmentLighting = {};
	mPersistentDynamicEmissiveCache = {};
	mPersistentDynamicEmissiveHighWaterStats = {};
	mActorSpriteDebugStats = {};
	mSurfaceRecords.clear();
	mFrameAppendStats = {};
	mFrameSerial = 0;
	mActivatedActorOverlayKeys.clear();
	mPublishedActorOverlayIndices.clear();
	mEmissiveStableSurfaceStates.clear();
	mResolvedMuzzleFlashRuleLookup.clear();
	mTransientMuzzleFlashSlots.clear();
	mTransientMuzzleFlashLights.clear();
	mLastMuzzleFlashEventSerial = 0;
	ResetEmissiveSectorResponseCaches();
}

void SceneLightSystem::PersistentDynamicEmissiveCacheBuildServices::BuildGeometry(
	const nri_scene::SceneView& sceneView,
	nri_scene::GeometryData& geometry,
	const char* label) const
{
	if (buildGeometry != nullptr)
	{
		buildGeometry(user, sceneView, geometry, label);
		return;
	}

	nri_scene::BuildGeometry(sceneView, geometry);
}

void SceneLightSystem::PersistentDynamicEmissiveCacheBuildServices::BuildMaterials(
	nri_scene::SceneView& sceneView,
	nri_scene::MaterialBridgeData& materials,
	const char* label) const
{
	if (buildMaterials != nullptr)
	{
		buildMaterials(user, sceneView, materials, label);
		return;
	}

	nri_scene::BuildMaterials(sceneView, materials);
}

void SceneLightSystem::ResetPersistentDynamicEmissiveCache()
{
	mPersistentDynamicEmissiveCache = {};
	mActorSpriteDebugStats = {};
	mEmissiveStableSurfaceStates.clear();
}

bool SceneLightSystem::IsEmissiveStableForSampling(
	uint64_t stableKey,
	uint32_t requiredFrames,
	const float center[3],
	float boundsRadius,
	float surfaceArea)
{
	if (requiredFrames <= 1)
	{
		return true;
	}

	EmissiveStableSurfaceState& state = mEmissiveStableSurfaceStates[stableKey];
	bool comparable = false;
	if (state.consecutiveFrames > 0)
	{
		const float dx = center[0] - state.center[0];
		const float dy = center[1] - state.center[1];
		const float dz = center[2] - state.center[2];
		const float radiusBase = std::max(1.0f, std::max(std::fabs(boundsRadius), std::fabs(state.boundsRadius)));
		const float centerTolerance = std::max(0.5f, radiusBase * 0.10f);
		const float radiusTolerance = std::max(0.5f, radiusBase * 0.10f);
		const float areaBase = std::max(1.0f, std::max(std::fabs(surfaceArea), std::fabs(state.surfaceArea)));
		const float areaTolerance = std::max(1.0f, areaBase * 0.15f);

		comparable =
			dx * dx + dy * dy + dz * dz <= centerTolerance * centerTolerance &&
			std::fabs(boundsRadius - state.boundsRadius) <= radiusTolerance &&
			std::fabs(surfaceArea - state.surfaceArea) <= areaTolerance;
	}

	const bool sameFrame = state.lastFrameSerial == mFrameSerial;
	const bool consecutiveFrame = state.lastFrameSerial + 1 == mFrameSerial;
	if (state.consecutiveFrames == 0 || (!sameFrame && !consecutiveFrame) || !comparable)
	{
		state.consecutiveFrames = 1;
	}
	else if (!sameFrame)
	{
		state.consecutiveFrames = std::min(requiredFrames, state.consecutiveFrames + 1);
	}

	state.lastFrameSerial = mFrameSerial;
	Copy3f(center, state.center);
	state.boundsRadius = boundsRadius;
	state.surfaceArea = surfaceArea;
	return state.consecutiveFrames >= requiredFrames;
}

void SceneLightSystem::PruneEmissiveStableSurfaceStates()
{
	if (mEmissiveStableSurfaceStates.empty())
	{
		return;
	}

	constexpr uint64_t maxStaleFrames = 8;
	for (auto it = mEmissiveStableSurfaceStates.begin(); it != mEmissiveStableSurfaceStates.end(); )
	{
		if (it->second.lastFrameSerial + maxStaleFrames < mFrameSerial)
		{
			it = mEmissiveStableSurfaceStates.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void SceneLightSystem::ResetPersistentDynamicEmissiveHighWaterStats()
{
	mPersistentDynamicEmissiveHighWaterStats = {};
}

SceneLightSystem::PersistentDynamicSurfaceStats SceneLightSystem::GatherPersistentDynamicEmissiveSurfaceStats() const
{
	if (!mPersistentDynamicEmissiveCache.valid)
	{
		return {};
	}

	return mPersistentDynamicEmissiveCache.surfaceStats;
}

namespace
{
	SceneLightSystem::PersistentDynamicSurfaceStats BuildPersistentDynamicSurfaceStats(const nri_scene::SceneView& sceneView)
	{
		SceneLightSystem::PersistentDynamicSurfaceStats stats = {};
		stats.wallSurfaceCount = (uint32_t)sceneView.opaqueWalls.size();
		stats.flatSurfaceCount = (uint32_t)sceneView.opaqueFlats.size();
		stats.spriteSurfaceCount = (uint32_t)sceneView.opaqueSprites.size();
		auto accumulate = [&stats](const auto& surfaces)
		{
			for (const auto& surface : surfaces)
			{
				if (surface.provenance.actorIndex >= 0)
				{
					stats.actorSurfaceCount++;
				}
				else
				{
					stats.nonActorSurfaceCount++;
				}

				switch (surface.provenance.sourceType)
				{
				case nri_scene::SurfaceSourceType::FacingSprite:
					stats.actorFacingSpriteCount++;
					break;
				case nri_scene::SurfaceSourceType::VoxelProxySprite:
					stats.actorVoxelSpriteCount++;
					break;
				default:
					break;
				}
			}
		};

		accumulate(sceneView.opaqueWalls);
		accumulate(sceneView.opaqueFlats);
		accumulate(sceneView.opaqueSprites);
		return stats;
	}
}

void SceneLightSystem::UpdatePersistentDynamicEmissiveHighWaterStats(const PersistentDynamicSurfaceStats& currentStats)
{
	if (!mPersistentDynamicEmissiveCache.valid)
	{
		return;
	}

	mPersistentDynamicEmissiveHighWaterStats.surfaceCount = std::max(mPersistentDynamicEmissiveHighWaterStats.surfaceCount, mPersistentDynamicEmissiveCache.surfaceCount);
	mPersistentDynamicEmissiveHighWaterStats.primitiveCount = std::max(mPersistentDynamicEmissiveHighWaterStats.primitiveCount, mPersistentDynamicEmissiveCache.primitiveCount);
	mPersistentDynamicEmissiveHighWaterStats.materialCount = std::max(mPersistentDynamicEmissiveHighWaterStats.materialCount, mPersistentDynamicEmissiveCache.materialCount);
	mPersistentDynamicEmissiveHighWaterStats.surfaceStats.actorSurfaceCount = std::max(mPersistentDynamicEmissiveHighWaterStats.surfaceStats.actorSurfaceCount, currentStats.actorSurfaceCount);
	mPersistentDynamicEmissiveHighWaterStats.surfaceStats.nonActorSurfaceCount = std::max(mPersistentDynamicEmissiveHighWaterStats.surfaceStats.nonActorSurfaceCount, currentStats.nonActorSurfaceCount);
	mPersistentDynamicEmissiveHighWaterStats.surfaceStats.wallSurfaceCount = std::max(mPersistentDynamicEmissiveHighWaterStats.surfaceStats.wallSurfaceCount, currentStats.wallSurfaceCount);
	mPersistentDynamicEmissiveHighWaterStats.surfaceStats.flatSurfaceCount = std::max(mPersistentDynamicEmissiveHighWaterStats.surfaceStats.flatSurfaceCount, currentStats.flatSurfaceCount);
	mPersistentDynamicEmissiveHighWaterStats.surfaceStats.spriteSurfaceCount = std::max(mPersistentDynamicEmissiveHighWaterStats.surfaceStats.spriteSurfaceCount, currentStats.spriteSurfaceCount);
	mPersistentDynamicEmissiveHighWaterStats.surfaceStats.actorFacingSpriteCount = std::max(mPersistentDynamicEmissiveHighWaterStats.surfaceStats.actorFacingSpriteCount, currentStats.actorFacingSpriteCount);
	mPersistentDynamicEmissiveHighWaterStats.surfaceStats.actorVoxelSpriteCount = std::max(mPersistentDynamicEmissiveHighWaterStats.surfaceStats.actorVoxelSpriteCount, currentStats.actorVoxelSpriteCount);
}

SceneLightSystem::PersistentDynamicMergeStats SceneLightSystem::MergePersistentDynamicEmissiveCacheIntoSceneView(nri_scene::SceneView& inOutSceneView) const
{
	PersistentDynamicMergeStats stats = {};
	stats.liveWallSurfaceCount = (uint32_t)inOutSceneView.opaqueWalls.size();
	stats.liveFlatSurfaceCount = (uint32_t)inOutSceneView.opaqueFlats.size();
	stats.liveSpriteSurfaceCount = (uint32_t)inOutSceneView.opaqueSprites.size();
	if (!mPersistentDynamicEmissiveCache.valid)
	{
		return stats;
	}
	stats.cacheWallSurfaceCount = (uint32_t)mPersistentDynamicEmissiveCache.sceneView.opaqueWalls.size();
	stats.cacheFlatSurfaceCount = (uint32_t)mPersistentDynamicEmissiveCache.sceneView.opaqueFlats.size();
	stats.cacheSpriteSurfaceCount = (uint32_t)mPersistentDynamicEmissiveCache.sceneView.opaqueSprites.size();

	std::unordered_set<uint64_t> seenSurfaceKeys;
	seenSurfaceKeys.reserve(
		inOutSceneView.opaqueWalls.size() +
		inOutSceneView.opaqueFlats.size() +
		inOutSceneView.opaqueSprites.size() +
		mPersistentDynamicEmissiveCache.sceneView.opaqueWalls.size() +
		mPersistentDynamicEmissiveCache.sceneView.opaqueFlats.size() +
		mPersistentDynamicEmissiveCache.sceneView.opaqueSprites.size());
	for (const auto& surface : inOutSceneView.opaqueWalls)
	{
		seenSurfaceKeys.insert(BuildPersistentEmissiveSurfaceIdentityKey(surface));
	}
	for (const auto& surface : inOutSceneView.opaqueFlats)
	{
		seenSurfaceKeys.insert(BuildPersistentEmissiveSurfaceIdentityKey(surface));
	}
	for (const auto& surface : inOutSceneView.opaqueSprites)
	{
		seenSurfaceKeys.insert(BuildPersistentEmissiveSurfaceIdentityKey(surface));
	}
	AppendUniquePersistentEmissiveSurfaces(
		mPersistentDynamicEmissiveCache.sceneView.opaqueWalls,
		inOutSceneView.opaqueWalls,
		seenSurfaceKeys,
		&stats.appendedWallSurfaceCount,
		&stats.duplicateWallSurfaceCount);
	AppendUniquePersistentEmissiveSurfaces(
		mPersistentDynamicEmissiveCache.sceneView.opaqueFlats,
		inOutSceneView.opaqueFlats,
		seenSurfaceKeys,
		&stats.appendedFlatSurfaceCount,
		&stats.duplicateFlatSurfaceCount);
	AppendUniquePersistentEmissiveSurfaces(
		mPersistentDynamicEmissiveCache.sceneView.opaqueSprites,
		inOutSceneView.opaqueSprites,
		seenSurfaceKeys,
		&stats.appendedSpriteSurfaceCount,
		&stats.duplicateSpriteSurfaceCount);
	return stats;
}

void SceneLightSystem::PrunePersistentDynamicEmissiveCacheToLiveActors(const PersistentDynamicEmissiveCacheBuildServices& services)
{
	mActorSpriteDebugStats = {};
	if (!mPersistentDynamicEmissiveCache.valid)
	{
		return;
	}

	std::unordered_map<int32_t, bool> liveActorIndices;
	std::unordered_map<int32_t, DCoreActor*> liveActorsByIndex;
	liveActorIndices.reserve(256);
	liveActorsByIndex.reserve(256);

	TSpriteIterator<DCoreActor> it;
	while (auto actor = it.Next())
	{
		if (actor == nullptr ||
			!actor->exists() ||
			(actor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			continue;
		}

		liveActorIndices[(int32_t)actor->GetIndex()] = true;
		liveActorsByIndex[(int32_t)actor->GetIndex()] = actor;
	}

	bool needsPrune = false;
	auto detectStaleActorOwnership = [this, &services, &needsPrune, &liveActorIndices, &liveActorsByIndex](const auto& surfaces)
	{
		for (const auto& surface : surfaces)
		{
			if (SurfaceUsesLiveActorTextureValidation(surface))
			{
				mActorSpriteDebugStats.lastPruneChecks++;
				if (surface.provenance.actorIndex < 0)
				{
					mActorSpriteDebugStats.lastPruneDroppedMissingActorIndex++;
					if (services.traceActorSpriteMismatch)
					{
						Printf("NRI PT actor-sprite cache: action=drop reason=missing_actor_index source=%s actor=%d surface_tex=%d surface_ptr=%p surface_pal=%d\n",
							nri_diag::GetSurfaceSourceTypeName(surface.provenance.sourceType),
							surface.provenance.actorIndex,
							surface.material.texture != nullptr ? surface.material.texture->GetID().GetIndex() : -1,
							surface.material.texture,
							surface.material.palette);
					}
					needsPrune = true;
					continue;
				}

				auto liveActorIt = liveActorsByIndex.find(surface.provenance.actorIndex);
				if (liveActorIt == liveActorsByIndex.end())
				{
					mActorSpriteDebugStats.lastPruneDroppedMissingActor++;
					if (services.traceActorSpriteMismatch)
					{
						Printf("NRI PT actor-sprite cache: action=drop reason=missing_actor source=%s actor=%d surface_tex=%d surface_ptr=%p surface_pal=%d\n",
							nri_diag::GetSurfaceSourceTypeName(surface.provenance.sourceType),
							surface.provenance.actorIndex,
							surface.material.texture != nullptr ? surface.material.texture->GetID().GetIndex() : -1,
							surface.material.texture,
							surface.material.palette);
					}
					needsPrune = true;
					continue;
				}

				const ActorSpriteLiveMatchDetails match = EvaluateCachedSurfaceMatchAgainstLiveActor(surface, *liveActorIt->second);
				if (match.result == ActorSpriteLiveMatchResult::Match)
				{
					mActorSpriteDebugStats.lastPruneMatches++;
					if (services.traceActorSpriteVerbose)
					{
						Printf("NRI PT actor-sprite cache: action=keep reason=%s source=%s actor=%d surface_tex=%d surface_ptr=%p live_tex=%d live_ptr=%p surface_pal=%d live_pal=%d\n",
							GetActorSpriteLiveMatchResultName(match.result),
							nri_diag::GetSurfaceSourceTypeName(surface.provenance.sourceType),
							surface.provenance.actorIndex,
							match.surfaceTextureId,
							surface.material.texture,
							match.liveTextureId,
							match.liveTexture,
							match.surfacePalette,
							match.livePalette);
					}
					continue;
				}

				switch (match.result)
				{
				case ActorSpriteLiveMatchResult::NullLiveTexture: mActorSpriteDebugStats.lastPruneDroppedNullLiveTexture++; break;
				case ActorSpriteLiveMatchResult::TextureMismatch: mActorSpriteDebugStats.lastPruneDroppedTextureMismatch++; break;
				case ActorSpriteLiveMatchResult::PaletteMismatch: mActorSpriteDebugStats.lastPruneDroppedPaletteMismatch++; break;
				default: break;
				}
				if (services.traceActorSpriteMismatch)
				{
					Printf("NRI PT actor-sprite cache: action=drop reason=%s source=%s actor=%d surface_tex=%d surface_ptr=%p live_tex=%d live_ptr=%p surface_pal=%d live_pal=%d\n",
						GetActorSpriteLiveMatchResultName(match.result),
						nri_diag::GetSurfaceSourceTypeName(surface.provenance.sourceType),
						surface.provenance.actorIndex,
						match.surfaceTextureId,
						surface.material.texture,
						match.liveTextureId,
						match.liveTexture,
						match.surfacePalette,
						match.livePalette);
				}
				needsPrune = true;
			}
			else if (surface.provenance.actorIndex >= 0 &&
				liveActorIndices.find(surface.provenance.actorIndex) == liveActorIndices.end())
			{
				needsPrune = true;
			}
		}
	};

	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueWalls);
	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueFlats);
	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueSprites);
	if (!needsPrune)
	{
		return;
	}

	PersistentDynamicEmissiveCache next = {};
	next.sceneView.drawInfo = mPersistentDynamicEmissiveCache.sceneView.drawInfo;
	next.sceneView.sky = mPersistentDynamicEmissiveCache.sceneView.sky;
	Copy3f(mPersistentDynamicEmissiveCache.sceneView.skyColor, next.sceneView.skyColor);
	Copy3f(mPersistentDynamicEmissiveCache.sceneView.groundColor, next.sceneView.groundColor);

	auto appendLiveOwnedSurfaces = [&liveActorIndices, &liveActorsByIndex](const auto& source, auto& destination)
	{
		for (const auto& surface : source)
		{
			if (SurfaceUsesLiveActorTextureValidation(surface))
			{
				if (surface.provenance.actorIndex < 0)
				{
					continue;
				}

				auto liveActorIt = liveActorsByIndex.find(surface.provenance.actorIndex);
				if (liveActorIt == liveActorsByIndex.end() || !CachedSurfaceMatchesLiveActor(surface, *liveActorIt->second))
				{
					continue;
				}
			}
			else if (surface.provenance.actorIndex >= 0 &&
				liveActorIndices.find(surface.provenance.actorIndex) == liveActorIndices.end())
			{
				continue;
			}

			destination.push_back(surface);
		}
	};

	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueWalls, next.sceneView.opaqueWalls);
	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueFlats, next.sceneView.opaqueFlats);
	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueSprites, next.sceneView.opaqueSprites);

	next.surfaceCount =
		(uint32_t)next.sceneView.opaqueWalls.size() +
		(uint32_t)next.sceneView.opaqueFlats.size() +
		(uint32_t)next.sceneView.opaqueSprites.size();
	if (next.surfaceCount == 0)
	{
		mPersistentDynamicEmissiveCache = {};
		return;
	}

	services.BuildGeometry(next.sceneView, next.geometry, "persistent_emissive_cache_prune");
	services.BuildMaterials(next.sceneView, next.materialBridge, "persistent_emissive_cache_prune");

	next.primitiveCount = (uint32_t)next.geometry.primitives.size();
	next.materialCount = (uint32_t)next.materialBridge.materials.size();
	next.surfaceStats = BuildPersistentDynamicSurfaceStats(next.sceneView);
	next.sceneView.stats.totalDrawItems = next.surfaceCount;
	next.sceneView.stats.wallDrawItems = (uint32_t)next.sceneView.opaqueWalls.size();
	next.sceneView.stats.flatDrawItems = (uint32_t)next.sceneView.opaqueFlats.size();
	next.sceneView.stats.spriteDrawItems = (uint32_t)next.sceneView.opaqueSprites.size();
	next.sceneView.stats.triangleEstimate = next.primitiveCount;
	next.sceneView.stats.materialRefs = next.materialCount;
	next.valid = next.primitiveCount > 0 && next.materialCount > 0;
	if (!next.valid)
	{
		mPersistentDynamicEmissiveCache = {};
		return;
	}

	mPersistentDynamicEmissiveCache = std::move(next);
}

bool SceneLightSystem::RebuildPersistentDynamicEmissiveCache(
	const nri_scene::SceneView& sceneView,
	const nri_scene::MaterialBridgeData& materials,
	const PersistentDynamicEmissiveCacheBuildServices& services)
{
	PersistentDynamicEmissiveCache next = {};
	next.sceneView.drawInfo = sceneView.drawInfo;
	next.sceneView.sky = sceneView.sky;
	Copy3f(sceneView.skyColor, next.sceneView.skyColor);
	Copy3f(sceneView.groundColor, next.sceneView.groundColor);

	bool liveSceneHasEmissive = false;
	std::unordered_set<uint64_t> seenSurfaceKeys;
	seenSurfaceKeys.reserve(
		sceneView.opaqueWalls.size() +
		sceneView.opaqueFlats.size() +
		sceneView.opaqueSprites.size() +
		mPersistentDynamicEmissiveCache.sceneView.opaqueWalls.size() +
		mPersistentDynamicEmissiveCache.sceneView.opaqueFlats.size() +
		mPersistentDynamicEmissiveCache.sceneView.opaqueSprites.size());

	uint32_t materialIndex = 0;
	auto appendLiveSurfaceList = [this, &materials, &materialIndex, &liveSceneHasEmissive, &seenSurfaceKeys](const auto& source, auto& destination)
	{
		for (const auto& surface : source)
		{
			bool keepSurface = false;
			if (materialIndex < materials.lightMetadata.size() && MaterialWouldEmit(materials.lightMetadata[materialIndex]))
			{
				liveSceneHasEmissive = true;
				const nri_scene::MaterialLightingMetadata& metadata = materials.lightMetadata[materialIndex];
				keepSurface = true;
				if (metadata.emissiveStableFrames > 1)
				{
					float center[3] = {};
					float boundsRadius = 0.0f;
					ComputeSurfaceBounds(surface, center, boundsRadius);
					const float surfaceArea = ComputeSurfaceArea(surface);
					const uint64_t stableKey = nri_scene::HashCombine64(BuildPersistentEmissiveSurfaceIdentityKey(surface), 0x5053594E414D4943ull);
					keepSurface = IsEmissiveStableForSampling(stableKey, metadata.emissiveStableFrames, center, boundsRadius, surfaceArea);
				}
			}
			const bool keepSpriteCacheSurface =
				surface.provenance.sourceType != nri_scene::SurfaceSourceType::FacingSprite &&
				surface.provenance.sourceType != nri_scene::SurfaceSourceType::VoxelProxySprite;
			if (keepSurface && (keepSpriteCacheSurface || surface.provenance.actorIndex >= 0))
			{
				const uint64_t identityKey = BuildPersistentEmissiveSurfaceIdentityKey(surface);
				if (seenSurfaceKeys.insert(identityKey).second)
				{
					destination.push_back(surface);
				}
			}
			materialIndex++;
		}
	};

	appendLiveSurfaceList(sceneView.opaqueWalls, next.sceneView.opaqueWalls);
	appendLiveSurfaceList(sceneView.opaqueFlats, next.sceneView.opaqueFlats);
	appendLiveSurfaceList(sceneView.opaqueSprites, next.sceneView.opaqueSprites);

	if (mPersistentDynamicEmissiveCache.valid)
	{
		AppendUniquePersistentEmissiveSurfaces(
			mPersistentDynamicEmissiveCache.sceneView.opaqueWalls,
			next.sceneView.opaqueWalls,
			seenSurfaceKeys);
		AppendUniquePersistentEmissiveSurfaces(
			mPersistentDynamicEmissiveCache.sceneView.opaqueFlats,
			next.sceneView.opaqueFlats,
			seenSurfaceKeys);
		AppendUniquePersistentEmissiveSurfaces(
			mPersistentDynamicEmissiveCache.sceneView.opaqueSprites,
			next.sceneView.opaqueSprites,
			seenSurfaceKeys);
	}

	next.surfaceCount =
		(uint32_t)next.sceneView.opaqueWalls.size() +
		(uint32_t)next.sceneView.opaqueFlats.size() +
		(uint32_t)next.sceneView.opaqueSprites.size();
	if (next.surfaceCount == 0)
	{
		mPersistentDynamicEmissiveCache = {};
		return liveSceneHasEmissive;
	}

	services.BuildGeometry(next.sceneView, next.geometry, "persistent_emissive_cache_rebuild");
	services.BuildMaterials(next.sceneView, next.materialBridge, "persistent_emissive_cache_rebuild");

	next.primitiveCount = (uint32_t)next.geometry.primitives.size();
	next.materialCount = (uint32_t)next.materialBridge.materials.size();
	next.surfaceStats = BuildPersistentDynamicSurfaceStats(next.sceneView);
	next.sceneView.stats.totalDrawItems = next.surfaceCount;
	next.sceneView.stats.wallDrawItems = (uint32_t)next.sceneView.opaqueWalls.size();
	next.sceneView.stats.flatDrawItems = (uint32_t)next.sceneView.opaqueFlats.size();
	next.sceneView.stats.spriteDrawItems = (uint32_t)next.sceneView.opaqueSprites.size();
	next.sceneView.stats.triangleEstimate = next.primitiveCount;
	next.sceneView.stats.materialRefs = next.materialCount;
	next.valid = next.primitiveCount > 0 && next.materialCount > 0;
	if (!next.valid)
	{
		mPersistentDynamicEmissiveCache = {};
		return liveSceneHasEmissive;
	}

	mPersistentDynamicEmissiveCache = std::move(next);
	return liveSceneHasEmissive;
}

void SceneLightSystem::BeginFrame(uint64_t frameSerial)
{
	mFrameSerial = frameSerial;
	mSurfaceRecords.clear();
	mSurfaceRecordIndex.Clear();
	mPublishedActorOverlayIndices.clear();
	mSuppressedActorIndices.clear();
	mFrameAppendStats = {};
	mAnalyticLights.matchedSurfaceCount = 0;
	mAnalyticLights.actorOverlayRuleCount = 0;
	mAnalyticLights.actorOverlayMatchedSurfaceCount = 0;
	mAnalyticLights.actorOverlayPublishedActorCount = 0;
	mAnalyticLights.actorOverlayPublishedFallbackActivationCount = 0;
	mAnalyticLights.mapOverlayRuleCount = 0;
	mAnalyticLights.dedupedMatchCount = 0;
	mAnalyticLights.truncatedLightCount = 0;
	mAnalyticLights.topologyChanged = false;
	mAnalyticLights.propertiesChanged = false;
	mEmissiveSurfaces.totalPowerEstimate = 0.0f;
	mEmissiveSurfaces.autoTaggedCount = 0;
	mEmissiveSurfaces.explicitRuleMatchCount = 0;
	mEmissiveSurfaces.overrideRuleCount = 0;
	mEmissiveSurfaces.overrideMatchedSurfaceCount = 0;
	mEmissiveSurfaces.materialResponseRuleCount = 0;
	mEmissiveSurfaces.materialResponseMatchedSurfaceCount = 0;
	mEmissiveSurfaces.truncatedSurfaceCount = 0;
	mEmissiveSurfaces.topologyChanged = false;
	mEmissiveSurfaces.propertiesChanged = false;
	mSectorLighting.eligibleSectorCount = 0;
	mSectorLighting.activeSectorCount = 0;
	mSectorLighting.fogSectorCount = 0;
	mSectorLighting.pulsingSectorCount = 0;
	mSectorLighting.topologyChanged = false;
}

SceneLightSystem::FrameAssemblyTimingStats SceneLightSystem::AssembleFrameSurfaceRecords(
	const FrameAssemblyInput& input,
	const FrameAssemblyServices& services)
{
	FrameAssemblyTimingStats timings = {};
	BeginFrame(input.frameSerial);
	if (input.suppressedActorIndices != nullptr)
	{
		mSuppressedActorIndices = *input.suppressedActorIndices;
	}

	auto measure = [](double& target, auto&& work)
	{
		const auto start = std::chrono::steady_clock::now();
		work();
		const auto end = std::chrono::steady_clock::now();
		target += std::chrono::duration<double, std::milli>(end - start).count();
	};

	if (input.usedStaticMapScene && input.staticScene != nullptr && input.staticScene->valid)
	{
		const StaticMapSceneCache& staticScene = *input.staticScene;
		const size_t chunkCount = std::min(staticScene.lightChunkViews.size(), staticScene.chunks.size());
		measure(timings.staticAppendMs, [&]()
		{
			for (size_t chunkListIndex = 0; chunkListIndex < chunkCount; ++chunkListIndex)
			{
				const auto& staticChunk = staticScene.chunks[chunkListIndex];
				if (!staticChunk.active)
				{
					continue;
				}
				const uint32_t mapChunkIndex = staticChunk.chunkIndex;
				const bool useRuntimeMutationReplacement =
					services.isRuntimeMutationReplacementActive != nullptr &&
					services.isRuntimeMutationReplacementActive(services.runtimeMutationUser, mapChunkIndex);
				if (useRuntimeMutationReplacement)
				{
					continue;
				}

				AppendSceneView(
					staticScene.lightChunkViews[chunkListIndex],
					staticScene.materialBridge,
					SceneLightRecordSource::StaticMapScene,
					staticChunk.materialOffset,
					staticChunk.materialOffset);
			}
		});

		measure(timings.runtimeMutationAppendMs, [&]()
		{
			if (services.appendRuntimeMutationSceneLightRecords != nullptr)
			{
				services.appendRuntimeMutationSceneLightRecords(services.runtimeMutationUser, *this);
			}
		});
	}
	else if (input.capturedSceneView != nullptr && input.capturedMaterials != nullptr)
	{
		measure(timings.capturedAppendMs, [&]()
		{
			AppendSceneView(*input.capturedSceneView, *input.capturedMaterials, SceneLightRecordSource::CapturedScene);
		});
	}

	if (input.dynamicSceneView != nullptr && input.dynamicMaterials != nullptr)
	{
		measure(timings.dynamicAppendMs, [&]()
		{
			AppendSceneView(*input.dynamicSceneView, *input.dynamicMaterials, SceneLightRecordSource::DynamicScene);
		});
	}

	if (input.surfaceLightSceneView != nullptr && input.surfaceLightMaterials != nullptr)
	{
		measure(timings.surfaceLightOverlayAppendMs, [&]()
		{
			AppendSceneView(*input.surfaceLightSceneView, *input.surfaceLightMaterials, SceneLightRecordSource::SurfaceLightOverlayScene);
		});
	}

	if (input.appendPersistentVoxelSceneLights)
	{
		measure(timings.persistentVoxelAppendMs, [&]()
		{
			if (services.appendPersistentVoxelSceneLights != nullptr)
			{
				services.appendPersistentVoxelSceneLights(services.persistentVoxelUser, *this, input.frameIndex, input.voxelStats);
			}
		});
	}

	return timings;
}

void SceneLightSystem::AppendSceneView(
	const nri_scene::SceneView& sceneView,
	const nri_scene::MaterialBridgeData& materials,
	SceneLightRecordSource source,
	uint32_t materialIndexBase,
	uint32_t materialLookupIndexBase,
	const SurfaceIdentityOverrides* identityOverrides)
{
	uint32_t localMaterialIndex = 0;
	AppendSurfaceList(
		sceneView.opaqueWalls,
		materials,
		source,
		materialIndexBase,
		materialLookupIndexBase,
		localMaterialIndex,
		identityOverrides != nullptr ? &identityOverrides->opaqueWalls : nullptr);
	AppendSurfaceList(
		sceneView.opaqueFlats,
		materials,
		source,
		materialIndexBase,
		materialLookupIndexBase,
		localMaterialIndex,
		identityOverrides != nullptr ? &identityOverrides->opaqueFlats : nullptr);
	AppendSurfaceList(
		sceneView.opaqueSprites,
		materials,
		source,
		materialIndexBase,
		materialLookupIndexBase,
		localMaterialIndex,
		identityOverrides != nullptr ? &identityOverrides->opaqueSprites : nullptr);
}

void SceneLightSystem::AppendSpriteSurfaces(
	const std::vector<nri_scene::SurfaceRef>& surfaces,
	const nri_scene::MaterialBridgeData& materials,
	SceneLightRecordSource source,
	uint32_t materialIndexBase,
	uint32_t materialLookupIndexBase,
	const std::vector<uint64_t>* identityOverrides)
{
	uint32_t localMaterialIndex = 0;
	AppendSurfaceList(
		surfaces,
		materials,
		source,
		materialIndexBase,
		materialLookupIndexBase,
		localMaterialIndex,
		identityOverrides);
}

SceneLightSystem::SurfaceRecord SceneLightSystem::BuildSurfaceRecord(
	const nri_scene::SurfaceRef& surface,
	const nri_scene::MaterialBridgeData& materials,
	SceneLightRecordSource source,
	uint32_t materialIndex,
	uint32_t materialLookupIndex,
	uint64_t identityOverride) const
{
	SurfaceRecord record = {};
	record.source = source;
	record.materialIndex = materialIndex;
	record.provenance = surface.provenance;
	ComputeSurfaceBounds(surface, record.center, record.boundsRadius);
	record.surfaceArea = ComputeSurfaceArea(surface);

	if (materialLookupIndex < materials.lightMetadata.size())
	{
		record.material = materials.lightMetadata[materialLookupIndex];
	}
	else if (materialLookupIndex < materials.materials.size())
	{
		record.material.sectorIndex = materials.materials[materialLookupIndex].sectorIndex != UINT32_MAX ? (int32_t)materials.materials[materialLookupIndex].sectorIndex : -1;
		record.material.paletteIndex = materials.materials[materialLookupIndex].paletteIndex;
		record.material.materialFlags = materials.materials[materialLookupIndex].flags;
		record.material.alpha = materials.materials[materialLookupIndex].alpha;
		record.material.lightLevel = materials.materials[materialLookupIndex].lightLevel;
	}

	record.identityKey = identityOverride != 0ull ? identityOverride : BuildSurfaceIdentityKey(record);
	return record;
}

void SceneLightSystem::AppendSurfaceRecords(
	const std::vector<SurfaceRecord>& records,
	uint32_t materialIndexBase)
{
	for (SurfaceRecord record : records)
	{
		AppendSurfaceRecord(record, materialIndexBase);
	}
}

void SceneLightSystem::MarkActorPublishedForOverlayActivation(int32_t actorIndex)
{
	if (actorIndex >= 0 && !IsActorSuppressedForFrame(actorIndex))
	{
		mPublishedActorOverlayIndices.insert(actorIndex);
	}
}

bool SceneLightSystem::IsActorSuppressedForFrame(int32_t actorIndex) const
{
	return actorIndex >= 0 && mSuppressedActorIndices.find(actorIndex) != mSuppressedActorIndices.end();
}

bool SceneLightSystem::HasActorAppearanceEvidence(int32_t actorIndex) const
{
	if (actorIndex < 0)
	{
		return false;
	}

	const auto surfaceIt = mSurfaceRecordIndex.spriteRecordsByActorIndex.find(actorIndex);
	const bool hasSpriteSurface = surfaceIt != mSurfaceRecordIndex.spriteRecordsByActorIndex.end() && !surfaceIt->second.empty();
	return hasSpriteSurface || IsActorPublishedForOverlayActivation(actorIndex);
}

bool SceneLightSystem::IsActorPublishedForOverlayActivation(int32_t actorIndex) const
{
	return actorIndex >= 0 && mPublishedActorOverlayIndices.find(actorIndex) != mPublishedActorOverlayIndices.end();
}

uint64_t SceneLightSystem::ComputeSurfaceIdentityKey(
	SceneLightRecordSource source,
	const nri_scene::SurfaceProvenance& provenance,
	const float center[3])
{
	SurfaceRecord record = {};
	record.source = source;
	record.provenance = provenance;
	Copy3f(center, record.center);
	return BuildSurfaceIdentityKey(record);
}

void SceneLightSystem::RebuildAnalyticLights(
	uint32_t flickerTimeIndex,
	uint32_t renderFrameIndex,
	uint32_t maxActiveLights,
	const float* currentCameraPos,
	const std::unordered_map<int32_t, std::vector<AnalyticLightRegistry::ActorOverlayRule>>* actorOverlayRules,
	const std::unordered_map<uint32_t, AnalyticLightRegistry::ActorOverlayRule>* actorOverlayRulesById,
	const std::vector<AnalyticLightRegistry::MapOverlayRule>* mapOverlayRules)
{
	const NRILightingSettings settings = CaptureSettings();
	std::vector<SceneAnalyticLight> nextLights;
	size_t overlayRuleCount = 0;
	if (actorOverlayRules != nullptr)
	{
		for (const auto& entry : *actorOverlayRules)
		{
			overlayRuleCount += entry.second.size();
		}
	}
	else if (actorOverlayRulesById != nullptr)
	{
		overlayRuleCount = actorOverlayRulesById->size();
	}
	const size_t mapOverlayRuleCount = mapOverlayRules != nullptr ? mapOverlayRules->size() : 0u;
	mAnalyticLights.actorOverlayRuleCount = (uint32_t)overlayRuleCount;
	mAnalyticLights.mapOverlayRuleCount = (uint32_t)mapOverlayRuleCount;
	mAnalyticLights.spriteTileRuleCount = (uint32_t)mAnalyticLights.spriteTileRules.size();
	mAnalyticLights.spriteRecordCandidateScans = 0;
	mAnalyticLights.actorOverlaySurfaceLookups = 0;
	mAnalyticLights.actorOverlayFullRecordScans = 0;
	mAnalyticLights.actorOverlaySurfaceCandidateScans = 0;
	mAnalyticLights.actorOverlayIndexedCandidateCount = 0;
	mAnalyticLights.actorOverlayPublishedActorCount = (uint32_t)mPublishedActorOverlayIndices.size();
	mAnalyticLights.actorOverlayPublishedFallbackActivationCount = 0;
	mAnalyticLights.topologyKeyCount = 0;
	mAnalyticLights.topologyRebuildCount = 0;
	mAnalyticLights.propertyOnlyUpdateCount = 0;
	mAnalyticLights.topologySortSkippedCount = 0;
	mAnalyticLights.topologyAddedKeyCount = 0;
	mAnalyticLights.topologyRemovedKeyCount = 0;
	mAnalyticLights.topologyReboundKeyCount = 0;
	mAnalyticLights.softLightCount = 0;
	mAnalyticLights.survivingKeyIndexChangeCount = 0;
	mAnalyticLights.survivingSoftLightIndexChangeCount = 0;
	mAnalyticLights.orderedStableKeyHash = 0;
	mAnalyticLights.topologySortMs = 0.0;
	nextLights.reserve(mAnalyticLights.manualLights.size() + mAnalyticLights.transientLights.size() + mAnalyticLights.spriteTileRules.size() + overlayRuleCount + mapOverlayRuleCount);
	std::unordered_map<uint64_t, size_t> keyToLightIndex;
	keyToLightIndex.reserve(mAnalyticLights.manualLights.size() + mAnalyticLights.transientLights.size() + mAnalyticLights.spriteTileRules.size() * 4u + overlayRuleCount * 2u + mapOverlayRuleCount);

	auto tryAppendLight = [this, &nextLights, &keyToLightIndex](const SceneAnalyticLight& light)
	{
		// A certified wrong-locality actor is omitted as one frame-local
		// occurrence.  Apply that same answer at the final analytic-light
		// publication boundary so sprite heuristics, transient actor lights,
		// and overlay rules cannot leave a light-only apparition behind.
		if (IsActorSuppressedForFrame(light.actorIndex))
		{
			return;
		}
		if (keyToLightIndex.find(light.stableKey) != keyToLightIndex.end())
		{
			mAnalyticLights.dedupedMatchCount++;
			return;
		}

		keyToLightIndex.emplace(light.stableKey, nextLights.size());
		nextLights.push_back(light);
	};

	for (const SceneAnalyticLight& manualLight : mAnalyticLights.manualLights)
	{
		tryAppendLight(manualLight);
	}

	for (const SceneAnalyticLight& transientLight : mAnalyticLights.transientLights)
	{
		tryAppendLight(transientLight);
	}

	for (const AnalyticLightHeuristicRule& rule : mAnalyticLights.spriteTileRules)
	{
		const auto candidateIt = mSurfaceRecordIndex.spriteRecordsByTextureId.find(rule.textureId);
		if (candidateIt == mSurfaceRecordIndex.spriteRecordsByTextureId.end())
		{
			continue;
		}

		for (uint32_t recordIndex : candidateIt->second)
		{
			if (recordIndex >= mSurfaceRecords.size())
			{
				continue;
			}
			mAnalyticLights.spriteRecordCandidateScans++;
			const SurfaceRecord& record = mSurfaceRecords[recordIndex];

			mAnalyticLights.matchedSurfaceCount++;

			SceneAnalyticLight light = {};
			light.stableKey = BuildAnalyticTopologyKey(SceneAnalyticLightSourceFlag_SpriteTileHeuristic, rule.ruleId, record);
			light.id = 0;
			light.sourceFlags = SceneAnalyticLightSourceFlag_SpriteTileHeuristic;
			light.sourceRuleId = rule.ruleId;
			light.source = record.source;
			light.actorIndex = record.provenance.actorIndex;
			light.textureId = record.material.textureId;
			Copy3f(record.center, light.position);
			Copy3f(rule.color, light.color);
			light.intensity = rule.intensity * EvaluateFlickerScale(light.stableKey, flickerTimeIndex, rule.flickerFrames);
			light.radius = rule.radius;
			tryAppendLight(light);
		}
	}

	if ((actorOverlayRules != nullptr && !actorOverlayRules->empty()) ||
		(actorOverlayRulesById != nullptr && !actorOverlayRulesById->empty()))
	{
		auto surfaceMatchesActorRule = [](const SurfaceRecord& record, const AnalyticLightRegistry::ActorOverlayRule& rule)
		{
			if ((record.material.materialFlags & nri_scene::MaterialFlag_Sprite) == 0 ||
				record.provenance.actorIndex != rule.actorIndex)
			{
				return false;
			}
			if (rule.hasTileFilter && record.material.textureId != rule.tileFilter)
			{
				return false;
			}
			return true;
		};

		auto findActorOverlaySurface = [this, &surfaceMatchesActorRule](const AnalyticLightRegistry::ActorOverlayRule& rule) -> const SurfaceRecord*
		{
			mAnalyticLights.actorOverlaySurfaceLookups++;
			const std::vector<uint32_t>* indexedCandidates = nullptr;
			if (rule.actorIndex >= 0)
			{
				if (rule.hasTileFilter)
				{
					const auto candidateIt = mSurfaceRecordIndex.spriteRecordsByActorTexture.find(BuildActorTextureSurfaceIndexKey(rule.actorIndex, rule.tileFilter));
					if (candidateIt != mSurfaceRecordIndex.spriteRecordsByActorTexture.end())
					{
						indexedCandidates = &candidateIt->second;
					}
				}
				else
				{
					const auto candidateIt = mSurfaceRecordIndex.spriteRecordsByActorIndex.find(rule.actorIndex);
					if (candidateIt != mSurfaceRecordIndex.spriteRecordsByActorIndex.end())
					{
						indexedCandidates = &candidateIt->second;
					}
				}

				if (indexedCandidates != nullptr)
				{
					mAnalyticLights.actorOverlayIndexedCandidateCount += (uint32_t)indexedCandidates->size();
					for (uint32_t recordIndex : *indexedCandidates)
					{
						if (recordIndex >= mSurfaceRecords.size())
						{
							continue;
						}
						mAnalyticLights.actorOverlaySurfaceCandidateScans++;
						const SurfaceRecord& record = mSurfaceRecords[recordIndex];
						if (surfaceMatchesActorRule(record, rule))
						{
							return &record;
						}
					}
					return nullptr;
				}

				return nullptr;
			}

			mAnalyticLights.actorOverlayFullRecordScans++;
			for (const SurfaceRecord& record : mSurfaceRecords)
			{
				mAnalyticLights.actorOverlaySurfaceCandidateScans++;
				if (surfaceMatchesActorRule(record, rule))
				{
					return &record;
				}
			}
			return nullptr;
		};

		std::unordered_set<uint64_t> liveActorOverlayKeys;
		if (actorOverlayRules != nullptr)
		{
			liveActorOverlayKeys.reserve(overlayRuleCount);
		}

		auto appendActorOwnedOverlayLight = [this, &tryAppendLight, &liveActorOverlayKeys, flickerTimeIndex, renderFrameIndex](
			const AnalyticLightRegistry::ActorOverlayRule& rule,
			const SurfaceRecord* record)
		{
			if (rule.actorIndex < 0)
			{
				return;
			}
			if (IsActorSuppressedForFrame(rule.actorIndex))
			{
				return;
			}
			if (rule.hasTileFilter && rule.actorTextureId != rule.tileFilter)
			{
				if (ShouldTraceActorOverlayRule(rule))
				{
					Printf("NRI PT explosion actor: frame=%u actor=%d class=%s rule=%u rule_name=%s live=yes emitted=no reason=tile_filter live_tile=%u filter_tile=%u\n",
						renderFrameIndex,
						rule.actorIndex,
						rule.actorClassName != nullptr ? rule.actorClassName : "",
						rule.ruleId,
						rule.ruleName.c_str(),
						rule.actorTextureId,
						rule.tileFilter);
				}
				return;
			}

			const bool hasSurface = record != nullptr;
			const bool publishedActor = IsActorPublishedForOverlayActivation(rule.actorIndex);
			const uint64_t stableKey = BuildActorOverlayTopologyKey(rule);
			liveActorOverlayKeys.insert(stableKey);
			if (hasSurface || publishedActor || rule.activateImmediately)
			{
				if (hasSurface)
				{
					mAnalyticLights.matchedSurfaceCount++;
					mAnalyticLights.actorOverlayMatchedSurfaceCount++;
				}
				else if (publishedActor)
				{
					mAnalyticLights.actorOverlayPublishedFallbackActivationCount++;
				}
				mActivatedActorOverlayKeys.insert(stableKey);
			}
			const bool active = rule.activateImmediately || mActivatedActorOverlayKeys.find(stableKey) != mActivatedActorOverlayKeys.end();

			SceneAnalyticLight light = {};
			light.stableKey = stableKey;
			light.id = 0;
			light.sourceFlags = SceneAnalyticLightSourceFlag_ActorOverlay;
			light.flags = rule.flags;
			light.sourceRuleId = rule.ruleId;
			light.source = hasSurface ? record->source : SceneLightRecordSource::DynamicScene;
			light.actorIndex = rule.actorIndex;
			light.textureId = hasSurface ? record->material.textureId : rule.actorTextureId;
			const float* basePosition = hasSurface ? record->center : rule.actorPosition;
			light.position[0] = basePosition[0] + rule.offset[0];
			light.position[1] = basePosition[1] + rule.offset[1];
			light.position[2] = basePosition[2] + rule.offset[2];
			if (hasSurface)
			{
				ApplyActorOverlaySurfaceNudge(*record, rule, light.position);
			}
			Copy3f(rule.color, light.color);
			const float resolvedIntensity = ResolveOverlayLightIntensity(
				rule.intensity,
				light.stableKey,
				flickerTimeIndex,
				renderFrameIndex,
				rule.flickerFrames,
				rule.hasRandomIntensity,
				rule.randomIntensityRange);
			light.intensity = active ? resolvedIntensity : 0.0f;
			light.radius = rule.radius;

			if (ShouldTraceActorOverlayRule(rule))
			{
				Printf("NRI PT explosion actor: frame=%u actor=%d class=%s rule=%u rule_name=%s live=yes emitted=yes active=%s activation=%s ownership=actor live_tile=%u pal=%d has_surface=%s published_actor=%s surface_source=%s surface_tile=%u stable=0x%016llx pos=(%.3f, %.3f, %.3f)\n",
					renderFrameIndex,
					rule.actorIndex,
					rule.actorClassName != nullptr ? rule.actorClassName : "",
					rule.ruleId,
					rule.ruleName.c_str(),
					YesNo(active),
					ActorOverlayActivationName(rule),
					rule.actorTextureId,
					rule.actorPalette,
					YesNo(hasSurface),
					YesNo(publishedActor),
					hasSurface ? nri_diag::GetSurfaceSourceTypeName(record->provenance.sourceType) : "none",
					hasSurface ? record->material.textureId : 0u,
					(unsigned long long)light.stableKey,
					light.position[0],
					light.position[1],
					light.position[2]);
				if (hasSurface)
				{
					const uint32_t lightingFlags = record->material.lightingFlags;
					Printf("NRI PT explosion material: frame=%u actor=%d class=%s rule=%u rule_name=%s source=%s tile=%u mat_light=0x%x mat_flags=0x%x stamped_rules=%u no_shadow_receive=%s no_shadow_cast=%s fullbright=%s\n",
						renderFrameIndex,
						rule.actorIndex,
						rule.actorClassName != nullptr ? rule.actorClassName : "",
						rule.ruleId,
						rule.ruleName.c_str(),
						nri_diag::GetSurfaceSourceTypeName(record->provenance.sourceType),
						record->material.textureId,
						lightingFlags,
						record->material.materialFlags,
						record->material.actorOverlayRuleCount,
						YesNo((lightingFlags & nri_scene::MaterialLightingFlag_NoShadowReceive) != 0),
						YesNo((lightingFlags & nri_scene::MaterialLightingFlag_NoShadowCast) != 0),
						YesNo((lightingFlags & nri_scene::MaterialLightingFlag_MaterialFullbright) != 0));
				}
				Printf("NRI PT explosion light: frame=%u actor=%d class=%s rule=%u rule_name=%s emitted=yes active=%s activation=%s ownership=actor stable=0x%016llx source=%u tile=%u intensity=%.3f resolved_intensity=%.3f radius=%.3f shadow=%s\n",
					renderFrameIndex,
					rule.actorIndex,
					rule.actorClassName != nullptr ? rule.actorClassName : "",
					rule.ruleId,
					rule.ruleName.c_str(),
					YesNo(active),
					ActorOverlayActivationName(rule),
					(unsigned long long)light.stableKey,
					(uint32_t)light.source,
					light.textureId,
					light.intensity,
					resolvedIntensity,
					light.radius,
					YesNo((light.flags & SceneAnalyticLightFlag_CastsShadow) != 0));
			}

			tryAppendLight(light);
		};

		auto appendSurfaceOwnedOverlayLight = [this, &tryAppendLight, flickerTimeIndex, renderFrameIndex](
			const SurfaceRecord& record,
			const AnalyticLightRegistry::ActorOverlayRule& rule)
		{
			if (rule.hasTileFilter && record.material.textureId != rule.tileFilter)
			{
				return;
			}

			mAnalyticLights.matchedSurfaceCount++;
			mAnalyticLights.actorOverlayMatchedSurfaceCount++;

			SceneAnalyticLight light = {};
			light.stableKey = BuildAnalyticTopologyKey(SceneAnalyticLightSourceFlag_ActorOverlay, rule.ruleId, record);
			light.id = 0;
			light.sourceFlags = SceneAnalyticLightSourceFlag_ActorOverlay;
			light.flags = rule.flags;
			light.sourceRuleId = rule.ruleId;
			light.source = record.source;
			light.actorIndex = record.provenance.actorIndex;
			light.textureId = record.material.textureId;
			light.position[0] = record.center[0] + rule.offset[0];
			light.position[1] = record.center[1] + rule.offset[1];
			light.position[2] = record.center[2] + rule.offset[2];
			ApplyActorOverlaySurfaceNudge(record, rule, light.position);
			Copy3f(rule.color, light.color);
			light.intensity = ResolveOverlayLightIntensity(
				rule.intensity,
				light.stableKey,
				flickerTimeIndex,
				renderFrameIndex,
				rule.flickerFrames,
				rule.hasRandomIntensity,
				rule.randomIntensityRange);
			light.radius = rule.radius;
			tryAppendLight(light);
		};

		if (actorOverlayRules != nullptr)
		{
			for (const auto& entry : *actorOverlayRules)
			{
				for (const AnalyticLightRegistry::ActorOverlayRule& rule : entry.second)
				{
					appendActorOwnedOverlayLight(rule, findActorOverlaySurface(rule));
				}
			}

			for (auto it = mActivatedActorOverlayKeys.begin(); it != mActivatedActorOverlayKeys.end();)
			{
				if (liveActorOverlayKeys.find(*it) == liveActorOverlayKeys.end())
				{
					it = mActivatedActorOverlayKeys.erase(it);
				}
				else
				{
					++it;
				}
			}
		}
		else
		{
			mAnalyticLights.actorOverlayFullRecordScans++;
			for (const SurfaceRecord& record : mSurfaceRecords)
			{
				mAnalyticLights.actorOverlaySurfaceCandidateScans++;
				if ((record.material.materialFlags & nri_scene::MaterialFlag_Sprite) == 0 ||
					record.provenance.actorIndex < 0)
				{
					continue;
				}

				if (actorOverlayRulesById != nullptr && record.material.actorOverlayRuleCount > 0)
				{
					for (uint32_t ruleIndex = 0; ruleIndex < record.material.actorOverlayRuleCount; ++ruleIndex)
					{
						const uint32_t ruleId = record.material.actorOverlayRuleIds[ruleIndex];
						const auto ruleIt = actorOverlayRulesById->find(ruleId);
						if (ruleIt == actorOverlayRulesById->end())
						{
							continue;
						}

						appendSurfaceOwnedOverlayLight(record, ruleIt->second);
					}
				}
			}
		}
	}

	if (mapOverlayRules != nullptr)
	{
		for (const AnalyticLightRegistry::MapOverlayRule& rule : *mapOverlayRules)
		{
			SceneAnalyticLight light = {};
			light.stableKey = rule.stableKey;
			light.id = 0;
			light.sourceFlags = SceneAnalyticLightSourceFlag_MapOverlay;
			light.sourceRuleId = rule.ruleId;
			light.source = rule.source;
			light.actorIndex = -1;
			light.textureId = 0;
			Copy3f(rule.position, light.position);
			Copy3f(rule.color, light.color);
			float sectorScale = 1.0f;
			const int32_t signalSector = rule.hasSignalSector ? rule.signalSector : -1;
			const bool sectorResponseEnabled = rule.hasSectorResponse ? rule.sectorResponse : false;
			if (sectorResponseEnabled && signalSector >= 0 && (uint32_t)signalSector < mSectorLighting.sectors.size())
			{
				const auto& sectorRecord = mSectorLighting.sectors[(uint32_t)signalSector];
				if (rule.hasResponseInputMin && rule.hasResponseInputMax)
				{
					sectorScale = ComputeSectorEmitterRangeResponseScale(
						sectorRecord.rawResponseSignal,
						rule.responseInputMin,
						rule.responseInputMax,
						rule.hasResponseMin ? rule.responseMin : std::max(0.0f, settings.sectorEmissionResponseMin),
						rule.hasResponseMax ? rule.responseMax : std::max(std::max(0.0f, settings.sectorEmissionResponseMin), settings.sectorEmissionResponseMax));
				}
				else if (rule.hasResponseIntensity || rule.hasResponseMin || rule.hasResponseMax)
				{
					const float responseMin = rule.hasResponseMin ? rule.responseMin : std::max(0.0f, settings.sectorEmissionResponseMin);
					sectorScale = ComputeSectorEmitterResponseScale(
						sectorRecord.rawResponseBrightness,
						std::min(std::max(0.0f, settings.sectorClamp), std::max(0.0f, settings.sectorAmbientScale) * (0.10f + 0.75f * 0.55f)),
						rule.hasResponseIntensity ? rule.responseIntensity : std::max(0.0f, settings.sectorEmissionSignalStrength),
						responseMin,
						rule.hasResponseMax ? rule.responseMax : std::max(responseMin, settings.sectorEmissionResponseMax));
				}
				else
				{
					sectorScale = std::max(0.0f, sectorRecord.emitterResponseScale);
				}
			}
			light.intensity = rule.intensity * sectorScale * EvaluateFlickerScale(light.stableKey, flickerTimeIndex, rule.flickerFrames);
			light.radius = rule.radius;
			tryAppendLight(light);
		}
	}

	if (nextLights.size() > maxActiveLights)
	{
		mAnalyticLights.truncatedLightCount = (uint32_t)(nextLights.size() - maxActiveLights);
		if (currentCameraPos != nullptr)
		{
			auto distanceSqToCamera = [currentCameraPos](const SceneAnalyticLight& light)
			{
				const float dx = light.position[0] - currentCameraPos[0];
				const float dy = light.position[1] - currentCameraPos[1];
				const float dz = light.position[2] - currentCameraPos[2];
				return dx * dx + dy * dy + dz * dz;
			};
			std::stable_sort(nextLights.begin(), nextLights.end(), [&distanceSqToCamera](const SceneAnalyticLight& left, const SceneAnalyticLight& right)
			{
				return distanceSqToCamera(left) < distanceSqToCamera(right);
			});
		}
		nextLights.resize(maxActiveLights);
	}

	// Upload order is a transport contract: tile lists reference these indices, while
	// each light's stochastic sequence is identified by stableKey. Keep cap selection
	// distance-based, then publish the survivors in identity order.
	std::sort(nextLights.begin(), nextLights.end(), [](const SceneAnalyticLight& left, const SceneAnalyticLight& right)
	{
		return left.stableKey < right.stableKey;
	});

	std::unordered_map<uint64_t, uint32_t> previousIndices;
	previousIndices.reserve(mAnalyticLights.activeLights.size());
	for (uint32_t index = 0; index < (uint32_t)mAnalyticLights.activeLights.size(); ++index)
	{
		previousIndices.emplace(mAnalyticLights.activeLights[index].stableKey, index);
	}
	mAnalyticLights.orderedStableKeyHash = 1469598103934665603ull;
	for (uint32_t index = 0; index < (uint32_t)nextLights.size(); ++index)
	{
		const SceneAnalyticLight& light = nextLights[index];
		mAnalyticLights.orderedStableKeyHash = nri_scene::HashCombine64(mAnalyticLights.orderedStableKeyHash, light.stableKey);
		const bool softLight =
			light.intensity > 0.0f &&
			light.radius > 0.0f &&
			(light.flags & SceneAnalyticLightFlag_CastsShadow) != 0 &&
			ResolveAnalyticEmitterRadius(light) > 0.0f;
		if (softLight)
		{
			mAnalyticLights.softLightCount++;
		}
		const auto previousIt = previousIndices.find(light.stableKey);
		if (previousIt != previousIndices.end() && previousIt->second != index)
		{
			mAnalyticLights.survivingKeyIndexChangeCount++;
			if (softLight)
			{
				mAnalyticLights.survivingSoftLightIndexChangeCount++;
			}
		}
	}

	std::vector<uint64_t> nextTopologyKeys;
	nextTopologyKeys.reserve(nextLights.size());
	std::unordered_map<uint64_t, uint64_t> nextPropertyHashes;
	std::unordered_map<uint64_t, uint64_t> nextBindingHashes;
	std::unordered_map<uint64_t, uint32_t> nextDiagnosticFlags;
	nextPropertyHashes.reserve(nextLights.size());
	nextBindingHashes.reserve(nextLights.size());
	nextDiagnosticFlags.reserve(nextLights.size());
	for (const SceneAnalyticLight& light : nextLights)
	{
		nextTopologyKeys.push_back(light.stableKey);
		const uint64_t propertyHash = BuildAnalyticPropertyHash(light);
		const uint64_t bindingHash = BuildAnalyticBindingHash(light);
		uint32_t diagnosticFlags = SceneLightDiagnosticFlag_None;
		const auto previousPropertyIt = mAnalyticLights.activePropertyHashes.find(light.stableKey);
		if (previousPropertyIt != mAnalyticLights.activePropertyHashes.end())
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_PreviousMatch;
			if (previousPropertyIt->second != propertyHash)
			{
				diagnosticFlags |= SceneLightDiagnosticFlag_PropertyChanged;
			}
		}
		else
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_Added;
		}

		const auto previousBindingIt = mAnalyticLights.activeBindingHashes.find(light.stableKey);
		if (previousBindingIt != mAnalyticLights.activeBindingHashes.end() && previousBindingIt->second != bindingHash)
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_Rebound;
		}

		nextPropertyHashes.emplace(light.stableKey, propertyHash);
		nextBindingHashes.emplace(light.stableKey, bindingHash);
		nextDiagnosticFlags.emplace(light.stableKey, diagnosticFlags);
	}
	mAnalyticLights.topologyKeyCount = (uint32_t)nextTopologyKeys.size();
	if (std::is_sorted(nextTopologyKeys.begin(), nextTopologyKeys.end()))
	{
		mAnalyticLights.topologySortSkippedCount = 1;
		mAnalyticLights.topologySortMs = 0.0;
	}
	else
	{
		const auto topologySortStart = std::chrono::steady_clock::now();
		std::sort(nextTopologyKeys.begin(), nextTopologyKeys.end());
		mAnalyticLights.topologySortMs = DurationMs(topologySortStart, std::chrono::steady_clock::now());
	}
	mAnalyticLights.topologyChanged = nextTopologyKeys != mAnalyticLights.activeTopologyKeys;
	mAnalyticLights.propertiesChanged = false;
	mAnalyticLights.addedTopologyKeys.clear();
	mAnalyticLights.removedTopologyKeys.clear();
	mAnalyticLights.reboundTopologyKeys.clear();
	for (const auto& entry : nextPropertyHashes)
	{
		const auto previousIt = mAnalyticLights.activePropertyHashes.find(entry.first);
		if (previousIt != mAnalyticLights.activePropertyHashes.end() && previousIt->second != entry.second)
		{
			mAnalyticLights.propertiesChanged = true;
			break;
		}
	}
	for (const auto& key : nextTopologyKeys)
	{
		if (mAnalyticLights.activePropertyHashes.find(key) == mAnalyticLights.activePropertyHashes.end())
		{
			mAnalyticLights.addedTopologyKeys.push_back(key);
		}
	}
	for (const auto& key : mAnalyticLights.activeTopologyKeys)
	{
		if (nextPropertyHashes.find(key) == nextPropertyHashes.end())
		{
			mAnalyticLights.removedTopologyKeys.push_back(key);
		}
	}
	for (const auto& entry : nextDiagnosticFlags)
	{
		if ((entry.second & SceneLightDiagnosticFlag_Rebound) != 0)
		{
			mAnalyticLights.reboundTopologyKeys.push_back(entry.first);
		}
	}
	mAnalyticLights.topologyAddedKeyCount = (uint32_t)mAnalyticLights.addedTopologyKeys.size();
	mAnalyticLights.topologyRemovedKeyCount = (uint32_t)mAnalyticLights.removedTopologyKeys.size();
	mAnalyticLights.topologyReboundKeyCount = (uint32_t)mAnalyticLights.reboundTopologyKeys.size();
	mAnalyticLights.topologyRebuildCount = mAnalyticLights.topologyChanged ? 1u : 0u;
	mAnalyticLights.propertyOnlyUpdateCount =
		!mAnalyticLights.topologyChanged &&
		mAnalyticLights.propertiesChanged &&
		mAnalyticLights.topologyReboundKeyCount == 0 ? 1u : 0u;
	mAnalyticLights.lastBuildTopologyChanged = mAnalyticLights.topologyChanged;
	mAnalyticLights.lastBuildPropertiesChanged = mAnalyticLights.propertiesChanged;
	mAnalyticLights.activeTopologyKeys = std::move(nextTopologyKeys);
	mAnalyticLights.activePropertyHashes = std::move(nextPropertyHashes);
	mAnalyticLights.activeBindingHashes = std::move(nextBindingHashes);
	mAnalyticLights.activeDiagnosticFlags = std::move(nextDiagnosticFlags);
	mAnalyticLights.activeLights = std::move(nextLights);
}

void SceneLightSystem::RebuildEmissiveSurfaces(
	uint32_t maxActiveSurfaces,
	const std::vector<EmissiveOverrideRule>* overrideRules,
	const std::vector<EmissiveOverrideRule>* surfaceLightFixtureRules,
	const std::vector<EmissiveMaterialResponseRule>* materialResponseRules)
{
	const NRILightingSettings settings = CaptureSettings();
	mEmissiveSurfaces.totalPowerEstimate = 0.0f;
	mEmissiveSurfaces.autoTaggedCount = 0;
	mEmissiveSurfaces.explicitRuleMatchCount = 0;
	mEmissiveSurfaces.overrideRuleCount = overrideRules != nullptr ? (uint32_t)overrideRules->size() : 0u;
	mEmissiveSurfaces.overrideMatchedSurfaceCount = 0;
	mEmissiveSurfaces.materialResponseRuleCount = materialResponseRules != nullptr ? (uint32_t)materialResponseRules->size() : 0u;
	mEmissiveSurfaces.materialResponseMatchedSurfaceCount = 0;
	mEmissiveSurfaces.truncatedSurfaceCount = 0;

	std::vector<EmissiveSurfaceRegistry::EmissiveSurfaceRecord> nextSurfaces;
	nextSurfaces.reserve(std::min<uint32_t>((uint32_t)mSurfaceRecords.size(), maxActiveSurfaces));

	const float minSurfaceArea = std::max(settings.emissiveMinSurface, 0.0f);
	const float minPower = std::max(settings.emissiveMinPower, 0.0f);

	for (const SurfaceRecord& record : mSurfaceRecords)
	{
		uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
		uint32_t sourceRuleId = 0;
		float emissiveColor[3] = {};
		float emissiveIntensity = 0.0f;
		uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
		uint32_t emissiveTextureIndex = UINT32_MAX;
		float emissiveSamplingScale = 1.0f;
		float emissiveFalloffScale = 1.0f;
		if (!EvaluateEmissiveMaterial(mEmissiveSurfaces, record.material, settings, sourceFlags, sourceRuleId, emissiveColor, emissiveIntensity, emissiveMode, emissiveTextureIndex, emissiveSamplingScale, emissiveFalloffScale))
		{
			continue;
		}

		if (record.surfaceArea < minSurfaceArea)
		{
			continue;
		}

		const float resolvedLuminance = emissiveMode == nri_scene::MaterialEmissiveMode_UseBaseTexture ?
			ComputeColorLuminance(record.material.averageColor) :
			ComputeColorLuminance(emissiveColor);

		EmissiveSurfaceRegistry::EmissiveSurfaceRecord emissive = {};
		emissive.stableKey = BuildEmissiveTopologyKey(record);
		emissive.sourceFlags = sourceFlags;
		emissive.sourceRuleId = sourceRuleId;
		emissive.source = record.source;
		emissive.actorIndex = record.provenance.actorIndex;
		emissive.sectorIndex = record.provenance.sectorIndex;
		emissive.authoredSectorIndex = record.provenance.sectorIndex;
		emissive.wallIndex = record.provenance.wallIndex;
		emissive.textureId = record.material.textureId;
		emissive.emissiveTextureIndex = emissiveTextureIndex;
		emissive.materialIndex = record.materialIndex;
		emissive.emissiveMode = emissiveMode;
		emissive.surfaceArea = record.surfaceArea;
		emissive.boundsRadius = record.boundsRadius;
		Copy3f(record.center, emissive.center);
		Copy3f(emissiveColor, emissive.emissiveColor);
		emissive.emissiveIntensity = emissiveIntensity;
		bool matchedMaterialResponse = false;
		if (materialResponseRules != nullptr)
		{
			for (const EmissiveMaterialResponseRule& rule : *materialResponseRules)
			{
				if (EmissiveMaterialResponseRuleMatchesSurface(rule, record))
				{
					ApplyEmissiveMaterialResponseRule(rule, emissive);
					matchedMaterialResponse = true;
				}
			}
		}
		bool matchedOverride = false;
		if (surfaceLightFixtureRules != nullptr)
		{
			for (const EmissiveOverrideRule& rule : *surfaceLightFixtureRules)
			{
				if (SurfaceLightFixtureRuleMatchesSurface(rule, record))
				{
					ApplyEmissiveOverrideRule(rule, emissive, settings);
					matchedOverride = true;
					break;
				}
			}
		}
		if (overrideRules != nullptr)
		{
			for (const EmissiveOverrideRule& rule : *overrideRules)
			{
				if (EmissiveOverrideMatchesSurface(rule, record))
				{
					ApplyEmissiveOverrideRule(rule, emissive, settings);
					matchedOverride = true;
				}
			}
		}
		emissive.powerEstimate = record.surfaceArea * resolvedLuminance * emissive.emissiveIntensity;
		emissive.placedPrimitiveBase = record.placedPrimitiveBase;
		emissive.placedPrimitiveCount = record.placedPrimitiveCount;
		emissive.sceneInstanceIndex = record.sceneInstanceIndex;
		emissive.occurrenceKeyLo = record.occurrenceKeyLo;
		emissive.occurrenceKeyHi = record.occurrenceKeyHi;
		emissive.occurrenceGeneration = record.occurrenceGeneration;
		if (emissive.powerEstimate < minPower)
		{
			continue;
		}
		if (record.material.emissiveStableFrames > 1)
		{
			const uint64_t stableKey = nri_scene::HashCombine64(emissive.stableKey, 0x414354495645454Dull);
			if (!IsEmissiveStableForSampling(stableKey, record.material.emissiveStableFrames, record.center, record.boundsRadius, record.surfaceArea))
			{
				continue;
			}
		}

		if (nextSurfaces.size() >= maxActiveSurfaces)
		{
			mEmissiveSurfaces.truncatedSurfaceCount++;
			continue;
		}
		if (matchedOverride)
		{
			mEmissiveSurfaces.overrideMatchedSurfaceCount++;
		}
		if (matchedMaterialResponse)
		{
			mEmissiveSurfaces.materialResponseMatchedSurfaceCount++;
		}
		nextSurfaces.push_back(emissive);

		if ((sourceFlags & (SceneEmissiveSurfaceSourceFlag_AutoFullbright | SceneEmissiveSurfaceSourceFlag_AutoTextureGlow | SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0)
		{
			mEmissiveSurfaces.autoTaggedCount++;
		}
		if ((sourceFlags & SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule) != 0)
		{
			mEmissiveSurfaces.explicitRuleMatchCount++;
		}
		mEmissiveSurfaces.totalPowerEstimate += emissive.powerEstimate;
	}

	std::vector<uint64_t> nextTopologyKeys;
	nextTopologyKeys.reserve(nextSurfaces.size());
	std::unordered_map<uint64_t, uint64_t> nextPropertyHashes;
	std::unordered_map<uint64_t, uint64_t> nextBindingHashes;
	std::unordered_map<uint64_t, uint32_t> nextDiagnosticFlags;
	nextPropertyHashes.reserve(nextSurfaces.size());
	nextBindingHashes.reserve(nextSurfaces.size());
	nextDiagnosticFlags.reserve(nextSurfaces.size());
	for (const auto& emissive : nextSurfaces)
	{
		nextTopologyKeys.push_back(emissive.stableKey);
		const uint64_t propertyHash = BuildEmissivePropertyHash(emissive);
		const uint64_t bindingHash = BuildEmissiveBindingHash(emissive);
		uint32_t diagnosticFlags = SceneLightDiagnosticFlag_None;
		const auto previousPropertyIt = mEmissiveSurfaces.activePropertyHashes.find(emissive.stableKey);
		if (previousPropertyIt != mEmissiveSurfaces.activePropertyHashes.end())
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_PreviousMatch;
			if (previousPropertyIt->second != propertyHash)
			{
				diagnosticFlags |= SceneLightDiagnosticFlag_PropertyChanged;
			}
		}
		else
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_Added;
		}

		const auto previousBindingIt = mEmissiveSurfaces.activeBindingHashes.find(emissive.stableKey);
		if (previousBindingIt != mEmissiveSurfaces.activeBindingHashes.end() && previousBindingIt->second != bindingHash)
		{
			diagnosticFlags |= SceneLightDiagnosticFlag_Rebound;
		}

		nextPropertyHashes.emplace(emissive.stableKey, propertyHash);
		nextBindingHashes.emplace(emissive.stableKey, bindingHash);
		nextDiagnosticFlags.emplace(emissive.stableKey, diagnosticFlags);
	}
	std::sort(nextTopologyKeys.begin(), nextTopologyKeys.end());
	mEmissiveSurfaces.topologyChanged = nextTopologyKeys != mEmissiveSurfaces.activeTopologyKeys;
	mEmissiveSurfaces.propertiesChanged = false;
	mEmissiveSurfaces.addedTopologyKeys.clear();
	mEmissiveSurfaces.removedTopologyKeys.clear();
	mEmissiveSurfaces.reboundTopologyKeys.clear();
	for (const auto& entry : nextPropertyHashes)
	{
		const auto previousIt = mEmissiveSurfaces.activePropertyHashes.find(entry.first);
		if (previousIt != mEmissiveSurfaces.activePropertyHashes.end() && previousIt->second != entry.second)
		{
			mEmissiveSurfaces.propertiesChanged = true;
			break;
		}
	}
	for (const auto& key : nextTopologyKeys)
	{
		if (mEmissiveSurfaces.activePropertyHashes.find(key) == mEmissiveSurfaces.activePropertyHashes.end())
		{
			mEmissiveSurfaces.addedTopologyKeys.push_back(key);
		}
	}
	for (const auto& key : mEmissiveSurfaces.activeTopologyKeys)
	{
		if (nextPropertyHashes.find(key) == nextPropertyHashes.end())
		{
			mEmissiveSurfaces.removedTopologyKeys.push_back(key);
		}
	}
	for (const auto& entry : nextDiagnosticFlags)
	{
		if ((entry.second & SceneLightDiagnosticFlag_Rebound) != 0)
		{
			mEmissiveSurfaces.reboundTopologyKeys.push_back(entry.first);
		}
	}
	mEmissiveSurfaces.lastBuildTopologyChanged = mEmissiveSurfaces.topologyChanged;
	mEmissiveSurfaces.lastBuildPropertiesChanged = mEmissiveSurfaces.propertiesChanged;
	mEmissiveSurfaces.activeTopologyKeys = std::move(nextTopologyKeys);
	mEmissiveSurfaces.activePropertyHashes = std::move(nextPropertyHashes);
	mEmissiveSurfaces.activeBindingHashes = std::move(nextBindingHashes);
	mEmissiveSurfaces.activeDiagnosticFlags = std::move(nextDiagnosticFlags);
	mEmissiveSurfaces.activeSurfaces = std::move(nextSurfaces);
	PruneEmissiveStableSurfaceStates();
}

void SceneLightSystem::RebuildSectorLighting(uint32_t frameIndex, uint32_t sectorCount)
{
	const NRILightingSettings settings = CaptureSettings();
	mSectorLighting.sectorCount = sectorCount;
	mSectorLighting.eligibleSectorCount = 0;
	mSectorLighting.rawActiveSectorCount = 0;
	mSectorLighting.rawNonNeutralSectorCount = 0;
	mSectorLighting.responseBoostSectorCount = 0;
	mSectorLighting.responseDimSectorCount = 0;
	mSectorLighting.responseNeutralSectorCount = 0;
	mSectorLighting.activeSectorCount = 0;
	mSectorLighting.fogSectorCount = 0;
	mSectorLighting.pulsingSectorCount = 0;
	mSectorLighting.sectors.assign(sectorCount, {});

	std::vector<uint8_t> seenSectors(sectorCount, 0u);
	for (const SurfaceRecord& record : mSurfaceRecords)
	{
		if (record.provenance.sectorIndex < 0)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)record.provenance.sectorIndex;
		if (sectorIndex >= sectorCount)
		{
			continue;
		}

		seenSectors[sectorIndex] = 1u;
	}

	mSectorLighting.activeSectorIndices.clear();
	mSectorLighting.activeSectorIndices.reserve(sectorCount);
	mSectorLighting.rawActiveSectorIndices.clear();
	mSectorLighting.rawActiveSectorIndices.reserve(sectorCount);

	if (!settings.sectorLighting || sectorCount == 0)
	{
		mSectorLighting.topologyChanged = !mSectorLighting.activeTopologyKeys.empty();
		mSectorLighting.activeTopologyKeys.clear();
		return;
	}

	const int paletteFilter = settings.sectorFilterPalette;
	const int minShadeFilter = settings.sectorFilterMinShade;
	const int maxShadeFilter = std::max(minShadeFilter, settings.sectorFilterMaxShade);
		const int lotagFilter = settings.sectorFilterLotag;
		const uint32_t pulseFrames = std::max(0, settings.sectorPulseFrames);
		const float pulseAmount = std::max(0.0f, settings.sectorPulseAmount);
		const bool pulseSelectionFiltered =
			paletteFilter >= 0 ||
			lotagFilter >= 0 ||
			minShadeFilter > -128 ||
			maxShadeFilter < 127;
		const float ambientScale = std::max(0.0f, settings.sectorAmbientScale);
		const float hemisphereScale = std::max(0.0f, settings.sectorHemisphereScale);
		const float fogScale = std::max(0.0f, settings.sectorFogScale);
	const float sectorClamp = std::max(0.0f, settings.sectorClamp);
	const float responseIntensity = std::max(0.0f, settings.sectorEmissionSignalStrength);
	const float responseMin = std::max(0.0f, settings.sectorEmissionResponseMin);
	const float responseMax = std::max(responseMin, settings.sectorEmissionResponseMax);
	const float neutralAmbient = std::min(sectorClamp, ambientScale * (0.10f + 0.75f * 0.55f));

	for (uint32_t sectorIndex = 0; sectorIndex < sectorCount; ++sectorIndex)
	{
		if (sectorIndex >= seenSectors.size() || seenSectors[sectorIndex] == 0u)
		{
			continue;
		}

		mSectorLighting.eligibleSectorCount++;

		const auto& sec = sector[sectorIndex];
		const int resolvedPalette = sec.floorpal != 0 ? (int)sec.floorpal : (int)sec.ceilingpal;
		const int averageShade = ((int)sec.floorshade + (int)sec.ceilingshade) / 2;
		const int rawFloorShade = (int)sec.floorshade;
		const int rawCeilingShade = (int)sec.ceilingshade;
		const float rawLightLevel = ComputeBuildLightLevel(averageShade, resolvedPalette);
		const float rawFloorLight = ComputeBuildLightLevel(rawFloorShade, resolvedPalette);
		const float rawCeilingLight = ComputeBuildLightLevel(rawCeilingShade, resolvedPalette);
		const float rawHemisphereBias = clamp(rawCeilingLight - rawFloorLight, -1.0f, 1.0f);
		const int lightingAverageShade = 0;
		const int lightingFloorShade = 0;
		const int lightingCeilingShade = 0;
		float tint[3] = {};
		float fogStrength = 0.0f;
		ResolveSectorTint(sec, resolvedPalette, tint, fogStrength);
		const bool sectorPulseEnabled = pulseSelectionFiltered && pulseFrames > 1 && pulseAmount > 0.0f;
		const float pulseScale = sectorPulseEnabled ? EvaluatePulseScale(0x5EC70B5E00000000ull ^ (uint64_t)sectorIndex, frameIndex, pulseFrames, pulseAmount) : 1.0f;
		const float rawClampedAmbient = std::min(sectorClamp, ambientScale * (0.10f + rawLightLevel * 0.55f) * pulseScale);
		const float rawClampedHemisphere = std::min(sectorClamp, hemisphereScale * (0.08f + (0.5f + 0.5f * std::abs(rawHemisphereBias)) * 0.45f) * pulseScale);
		const float rawClampedFog = std::min(sectorClamp, fogScale * fogStrength * pulseScale);
		const float rawHemisphereAmount = rawHemisphereBias * rawClampedHemisphere;
		const float rawResponseBrightness = rawClampedAmbient + std::abs(rawHemisphereAmount);
		const float rawResponseSignal = clamp(rawLightLevel + std::abs(rawHemisphereBias), 0.0f, 1.0f);
		const float emitterResponseScale = ComputeSectorEmitterResponseScale(rawResponseBrightness, neutralAmbient, responseIntensity, responseMin, responseMax);

		SectorLightingRegistry::SectorLightRecord entry = {};
		entry.sectorIndex = sectorIndex;
		entry.paletteIndex = resolvedPalette;
		entry.lotag = sec.lotag;
		entry.hitag = sec.hitag;
		entry.averageShade = averageShade;
		entry.rawAverageShade = averageShade;
		entry.rawLightLevel = rawLightLevel;
		entry.rawFloorLight = rawFloorLight;
		entry.rawCeilingLight = rawCeilingLight;
		entry.rawAmbientIntensity = rawClampedAmbient;
		entry.rawHemisphereAmount = rawHemisphereAmount;
		entry.rawFogAmount = rawClampedFog;
		entry.rawResponseBrightness = rawResponseBrightness;
		entry.rawResponseSignal = rawResponseSignal;
		entry.emitterResponseScale = emitterResponseScale;
		entry.ambientColor[0] = tint[0];
		entry.ambientColor[1] = tint[1];
		entry.ambientColor[2] = tint[2];
		entry.pulseScale = pulseScale;

		const bool rawActive = rawClampedAmbient > 0.0f || rawClampedHemisphere > 0.0f || rawClampedFog > 0.0f;
		if (rawActive)
		{
			mSectorLighting.rawActiveSectorIndices.push_back(sectorIndex);
		}
		if (averageShade != 0 || rawFloorShade != 0 || rawCeilingShade != 0)
		{
			mSectorLighting.rawNonNeutralSectorCount++;
		}
		if (emitterResponseScale > 1.01f)
		{
			mSectorLighting.responseBoostSectorCount++;
		}
		else if (emitterResponseScale < 0.99f)
		{
			mSectorLighting.responseDimSectorCount++;
		}
		else
		{
			mSectorLighting.responseNeutralSectorCount++;
		}
		mSectorLighting.sectors[sectorIndex] = entry;

		if ((paletteFilter >= 0 && resolvedPalette != paletteFilter) ||
			(lightingAverageShade < minShadeFilter || lightingAverageShade > maxShadeFilter) ||
			(lotagFilter >= 0 && sec.lotag != lotagFilter))
		{
			continue;
		}

		const float lightLevel = ComputeBuildLightLevel(lightingAverageShade, resolvedPalette);
		const float floorLight = ComputeBuildLightLevel(lightingFloorShade, resolvedPalette);
		const float ceilingLight = ComputeBuildLightLevel(lightingCeilingShade, resolvedPalette);
		const float hemisphereBias = clamp(ceilingLight - floorLight, -1.0f, 1.0f);
		const float clampedAmbient = std::min(sectorClamp, ambientScale * (0.10f + lightLevel * 0.55f) * pulseScale);
		const float clampedHemisphere = std::min(sectorClamp, hemisphereScale * (0.08f + (0.5f + 0.5f * std::abs(hemisphereBias)) * 0.45f) * pulseScale);
		const float clampedFog = std::min(sectorClamp, fogScale * fogStrength * pulseScale);
		if (clampedAmbient <= 0.0f && clampedHemisphere <= 0.0f && clampedFog <= 0.0f)
		{
			continue;
		}

		entry.sourceFlags = SceneSectorLightSourceFlag_Heuristic;
		if (paletteFilter >= 0)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_PaletteFilter;
		}
		if (lotagFilter >= 0)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_LotagFilter;
		}
		if (fogStrength > 0.0f)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_FogPresent;
			mSectorLighting.fogSectorCount++;
		}
		if (sectorPulseEnabled)
		{
			entry.sourceFlags |= SceneSectorLightSourceFlag_Pulsing;
			mSectorLighting.pulsingSectorCount++;
		}

		entry.ambientIntensity = clampedAmbient;
		entry.hemisphereAmount = hemisphereBias * clampedHemisphere;
		entry.fogAmount = clampedFog;

		mSectorLighting.sectors[sectorIndex] = entry;
		mSectorLighting.activeSectorIndices.push_back(sectorIndex);
	}

	mSectorLighting.rawActiveSectorCount = (uint32_t)mSectorLighting.rawActiveSectorIndices.size();
	mSectorLighting.activeSectorCount = (uint32_t)mSectorLighting.activeSectorIndices.size();
	std::vector<uint32_t> nextTopologyKeys = mSectorLighting.activeSectorIndices;
	std::sort(nextTopologyKeys.begin(), nextTopologyKeys.end());
	mSectorLighting.topologyChanged = nextTopologyKeys != mSectorLighting.activeTopologyKeys;
	mSectorLighting.activeTopologyKeys = std::move(nextTopologyKeys);
}

void SceneLightSystem::BuildRuntimePointLightUpload(std::vector<NRIRuntimePointLightGpuData>& outLights) const
{
	const auto& activeLights = mAnalyticLights.activeLights;
	outLights.clear();
	outLights.reserve(activeLights.size());
	for (const SceneAnalyticLight& light : activeLights)
	{
		NRIRuntimePointLightGpuData gpuLight = {};
		Copy3f(light.position, gpuLight.position);
		gpuLight.radius = light.radius;
		Copy3f(light.color, gpuLight.color);
		gpuLight.intensity = light.intensity;
		gpuLight.flags = light.flags;
		gpuLight.emitterRadius = ResolveAnalyticEmitterRadius(light);
		gpuLight.stableKeyLo = (uint32_t)(light.stableKey & 0xffffffffu);
		gpuLight.stableKeyHi = (uint32_t)(light.stableKey >> 32u);
		outLights.push_back(gpuLight);
	}
}

uint64_t SceneLightSystem::BuildRuntimeLightPayloadHash() const
{
	const auto& activeLights = mAnalyticLights.activeLights;
	uint64_t hash = 1469598103934665603ull;
	hash = nri_scene::HashCombine64(hash, (uint64_t)activeLights.size());
	for (const SceneAnalyticLight& light : activeLights)
	{
		hash = nri_scene::HashCombine64(hash, light.stableKey);
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(light.position[0]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(light.position[1]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(light.position[2]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(light.color[0]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(light.color[1]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(light.color[2]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(light.intensity));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(light.radius));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(ResolveAnalyticEmitterRadius(light)));
		hash = nri_scene::HashCombine64(hash, (uint64_t)light.flags);
	}

	return hash;
}

uint64_t SceneLightSystem::BuildRuntimeLightClusterCameraHash(const RuntimeLightClusterBuildInput& input) const
{
	uint64_t hash = 1469598103934665603ull;
	hash = nri_scene::HashCombine64(hash, (uint64_t)input.renderWidth);
	hash = nri_scene::HashCombine64(hash, (uint64_t)input.renderHeight);
	hash = nri_scene::HashCombine64(hash, (uint64_t)input.tileSize);
	hash = nri_scene::HashCombine64(hash, (uint64_t)input.maxRuntimeLights);

	if (mAnalyticLights.activeLights.empty())
	{
		return hash;
	}

	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraPos[0]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraPos[1]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraPos[2]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraForward[0]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraForward[1]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraForward[2]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraRight[0]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraRight[1]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraRight[2]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraUp[0]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraUp[1]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.currentCameraUp[2]));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.tanHalfFovX));
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(input.tanHalfFovY));
	return hash;
}

void SceneLightSystem::BuildRuntimeLightClusterUpload(
	const RuntimeLightClusterBuildInput& input,
	std::vector<NRIRuntimeLightTileHeaderGpuData>& outHeaders,
	std::vector<uint32_t>& outIndices,
	uint32_t& outTileCountX,
	uint32_t& outTileCountY,
	uint32_t& outTileIndexCount,
	uint32_t& outMaxTileOccupancy) const
{
	const auto& activeLights = mAnalyticLights.activeLights;
	const uint32_t activeLightCount = (uint32_t)activeLights.size();
	const uint32_t tileSize = std::max(1u, input.tileSize);
	outTileCountX = std::max(1u, (input.renderWidth + tileSize - 1u) / tileSize);
	outTileCountY = std::max(1u, (input.renderHeight + tileSize - 1u) / tileSize);
	const uint32_t tileCount = outTileCountX * outTileCountY;
	const uint32_t maxIndexCapacity = tileCount * input.maxRuntimeLights;
	outTileIndexCount = 0;
	outMaxTileOccupancy = 0;
	outHeaders.assign(tileCount, {});
	outIndices.assign(maxIndexCapacity, 0u);

	if (tileCount == 0 || activeLightCount == 0 || input.renderWidth == 0 || input.renderHeight == 0)
	{
		return;
	}

	std::vector<std::vector<uint32_t>> tileLights(tileCount);
	for (uint32_t lightIndex = 0; lightIndex < activeLightCount; ++lightIndex)
	{
		const SceneAnalyticLight& light = activeLights[lightIndex];
		if (light.intensity <= 0.0f || light.radius <= 0.0f)
		{
			continue;
		}

		const float toLight[3] = {
			light.position[0] - input.currentCameraPos[0],
			light.position[1] - input.currentCameraPos[1],
			light.position[2] - input.currentCameraPos[2]
		};
		const float viewX = Dot3(toLight, input.currentCameraRight);
		const float viewY = Dot3(toLight, input.currentCameraUp);
		const float viewZ = Dot3(toLight, input.currentCameraForward);
		if (viewZ <= -light.radius)
		{
			continue;
		}

		int32_t minTileX = 0;
		int32_t minTileY = 0;
		int32_t maxTileX = (int32_t)outTileCountX - 1;
		int32_t maxTileY = (int32_t)outTileCountY - 1;

		if (viewZ > light.radius &&
			input.tanHalfFovX > 0.0f &&
			input.tanHalfFovY > 0.0f)
		{
			const float conservativeDepth = std::max(viewZ - light.radius, 1.0f);
			const float centerNdcX = viewX / (viewZ * input.tanHalfFovX);
			const float centerNdcY = viewY / (viewZ * input.tanHalfFovY);
			const float radiusNdcX = light.radius / (conservativeDepth * input.tanHalfFovX);
			const float radiusNdcY = light.radius / (conservativeDepth * input.tanHalfFovY);
			const float minPixelX = ((centerNdcX - radiusNdcX) * 0.5f + 0.5f) * (float)input.renderWidth;
			const float maxPixelX = ((centerNdcX + radiusNdcX) * 0.5f + 0.5f) * (float)input.renderWidth;
			const float minPixelY = (0.5f - (centerNdcY + radiusNdcY) * 0.5f) * (float)input.renderHeight;
			const float maxPixelY = (0.5f - (centerNdcY - radiusNdcY) * 0.5f) * (float)input.renderHeight;
			if (maxPixelX < 0.0f || minPixelX >= (float)input.renderWidth || maxPixelY < 0.0f || minPixelY >= (float)input.renderHeight)
			{
				continue;
			}

			minTileX = std::max(0, (int32_t)std::floor(minPixelX / (float)tileSize));
			minTileY = std::max(0, (int32_t)std::floor(minPixelY / (float)tileSize));
			maxTileX = std::min((int32_t)outTileCountX - 1, (int32_t)std::floor(std::max(maxPixelX - 1.0f, 0.0f) / (float)tileSize));
			maxTileY = std::min((int32_t)outTileCountY - 1, (int32_t)std::floor(std::max(maxPixelY - 1.0f, 0.0f) / (float)tileSize));
		}

		if (minTileX > maxTileX || minTileY > maxTileY)
		{
			continue;
		}

		for (int32_t tileY = minTileY; tileY <= maxTileY; ++tileY)
		{
			for (int32_t tileX = minTileX; tileX <= maxTileX; ++tileX)
			{
				tileLights[(size_t)tileY * outTileCountX + (size_t)tileX].push_back(lightIndex);
			}
		}
	}

	uint32_t indexCursor = 0;
	for (uint32_t tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		NRIRuntimeLightTileHeaderGpuData& header = outHeaders[tileIndex];
		const std::vector<uint32_t>& tileLightList = tileLights[tileIndex];
		header.indexOffset = indexCursor;
		header.indexCount = (uint32_t)tileLightList.size();
		outMaxTileOccupancy = std::max(outMaxTileOccupancy, header.indexCount);
		for (uint32_t lightIndex : tileLightList)
		{
			if (indexCursor < outIndices.size())
			{
				outIndices[indexCursor] = lightIndex;
				indexCursor++;
			}
		}
	}

	outTileIndexCount = indexCursor;
}

void SceneLightSystem::BuildEmissiveSamplingUpload(
	const EmissiveSamplingBuildContext& context,
	NRIEmissivePrimitiveHeaderGpuData& outHeader,
	std::vector<NRIEmissivePrimitiveGpuData>& outPrimitives,
	std::vector<float>& outCdf,
	std::vector<NRIEmissiveMaterialResponseGpuData>& outMaterialResponses,
	std::vector<NRIEmissivePrimitiveDebugRecord>& outDebugRecords,
	EmissiveSamplingUploadStats* outStats)
{
	EmissiveSamplingUploadStats localStats = {};
	outHeader = {};
	outHeader.dominantIndex = UINT32_MAX;
	outHeader.flags = 0u;
	outPrimitives.clear();
	outCdf.clear();
	outMaterialResponses.clear();
	outDebugRecords.clear();
	NRIEmissiveMaterialResponseGpuData materialResponseHeader = {};
	materialResponseHeader.primitiveIndex = UINT32_MAX;
	materialResponseHeader.materialScale = 1.0f;
	outMaterialResponses.push_back(materialResponseHeader);

	struct MaterialPrimitiveRange
	{
		uint32_t first = UINT32_MAX;
		uint32_t count = 0;
	};

	struct BuiltCandidate
	{
		NRIEmissivePrimitiveGpuData gpu = {};
		NRIEmissivePrimitiveDebugRecord debug = {};
		float referenceProposalWeight = 0.0f;
		bool hasReferenceProposalWeight = false;
	};

	auto buildRanges = [](const nri_scene::GeometryData* geometry, std::vector<MaterialPrimitiveRange>& outRanges)
	{
		outRanges.clear();
		if (geometry == nullptr)
		{
			return;
		}

		uint32_t maxMaterialIndex = 0;
		for (const auto& primitive : geometry->primitives)
		{
			maxMaterialIndex = std::max(maxMaterialIndex, primitive.materialIndex);
		}

		outRanges.assign((size_t)maxMaterialIndex + 1u, {});
		for (uint32_t primitiveIndex = 0; primitiveIndex < geometry->primitives.size(); ++primitiveIndex)
		{
			const uint32_t materialIndex = geometry->primitives[primitiveIndex].materialIndex;
			auto& range = outRanges[materialIndex];
			if (range.count == 0)
			{
				range.first = primitiveIndex;
			}
			range.count++;
		}
	};

	std::vector<MaterialPrimitiveRange> staticRanges;
	std::vector<MaterialPrimitiveRange> capturedRanges;
	std::vector<MaterialPrimitiveRange> runtimeMutationRanges;
	std::vector<MaterialPrimitiveRange> dynamicRanges;
	std::vector<MaterialPrimitiveRange> surfaceLightOverlayRanges;
	buildRanges(context.staticGeometry, staticRanges);
	buildRanges(context.capturedGeometry, capturedRanges);
	buildRanges(context.runtimeMutationGeometry, runtimeMutationRanges);
	buildRanges(context.dynamicGeometry, dynamicRanges);
	buildRanges(context.surfaceLightOverlayGeometry, surfaceLightOverlayRanges);

	const NRILightingSettings settings = CaptureSettings();
	std::vector<BuiltCandidate> candidates;
	std::unordered_map<uint64_t, uint32_t> materialResponseLookup;
	const auto& activeSurfaces = mEmissiveSurfaces.activeSurfaces;
	candidates.reserve(activeSurfaces.size());

	auto appendSurfacePrimitives = [&](const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, const nri_scene::GeometryData* geometry, const std::vector<MaterialPrimitiveRange>& ranges, uint32_t dataSource, uint32_t primitiveBase)
	{
		if (geometry == nullptr || surface.materialIndex == UINT32_MAX || surface.materialIndex >= ranges.size())
		{
			return;
		}

		const auto& range = ranges[surface.materialIndex];
		if (range.count == 0 || range.first == UINT32_MAX)
		{
			return;
		}

		float representativeLuminance = 0.0f;
		if (surface.surfaceArea > 0.0f && surface.emissiveIntensity > 0.0f)
		{
			representativeLuminance = std::max(surface.powerEstimate / (surface.surfaceArea * surface.emissiveIntensity), 0.0f);
		}
		const float samplingScale = ResolveGlowSamplingScale(surface.sourceFlags, surface.emissiveMode, settings) * std::max(surface.reachScale, 0.0f);
		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		bool sectorResponseApplied = false;
		const float sectorRawResponseScale = ResolveSectorEmissionScale(surface, sectorResponseApplied);
		const float sectorResponseScale = sectorResponseApplied ? ResolveSectorEmissionIntensityScale(surface, sectorRawResponseScale) : 1.0f;
		const float sectorReachScale = sectorResponseApplied ? ResolveSectorEmissionReachScale(surface, sectorRawResponseScale) : 1.0f;
		bool materialResponseApplied = false;
		const float materialResponseScale = ResolveEmissiveMaterialResponseScale(surface, materialResponseApplied);
		const bool materialResponseEligible = IsEmissiveSurfaceMaterialResponseEligible(surface);
		const float sectorReachBound = sectorResponseEligible ?
			(surface.hasSectorResponseReachMax ?
				std::max(std::max(0.0f, surface.sectorResponseReachMin), surface.sectorResponseReachMax) :
				std::max(std::max(0.0f, (float)nri_ptsectoremissionreachmin), (float)nri_ptsectoremissionreachmax)) :
			1.0f;
		uint64_t surfacePrimitiveKey = surface.stableKey;

		for (uint32_t localOffset = 0; localOffset < range.count; ++localOffset)
		{
			const uint32_t localPrimitiveIndex = range.first + localOffset;
			const uint32_t primitiveIndex = primitiveBase + localPrimitiveIndex;
			const float primitiveArea = ComputePrimitiveArea(*geometry, localPrimitiveIndex);
			if (primitiveArea <= 0.0f)
			{
				continue;
			}

			BuiltCandidate candidate = {};
			candidate.gpu.dataSource = dataSource;
			candidate.gpu.primitiveIndex = primitiveIndex;
			candidate.gpu.sourceFlags = surface.sourceFlags;
			candidate.gpu.textureId = surface.textureId;
			candidate.gpu.primitiveArea = primitiveArea;
			const float basePowerEstimate = std::max(primitiveArea * representativeLuminance * surface.emissiveIntensity, 0.0f);
			candidate.gpu.powerEstimate = basePowerEstimate * sectorResponseScale * materialResponseScale;
			candidate.gpu.selectionWeight = basePowerEstimate * samplingScale * sectorReachScale * materialResponseScale;
			candidate.gpu.emissionScale = sectorResponseScale * materialResponseScale;
			candidate.gpu.materialResponseScale = std::max(materialResponseScale, 0.0f);
			candidate.referenceProposalWeight = basePowerEstimate * samplingScale * sectorReachBound * materialResponseScale;
			candidate.hasReferenceProposalWeight = sectorResponseEligible;

			candidate.debug.stableKey = nri_scene::HashCombine64(surfacePrimitiveKey, ((uint64_t)dataSource << 32u) | localOffset);
			candidate.debug.surfaceStableKey = surface.stableKey;
			candidate.debug.dataSource = dataSource;
			candidate.debug.primitiveIndex = primitiveIndex;
			candidate.debug.materialIndex = surface.materialIndex;
			candidate.debug.sourceFlags = surface.sourceFlags;
			candidate.debug.sourceRuleId = surface.sourceRuleId;
			candidate.debug.overrideRuleId = surface.overrideRuleId;
			candidate.debug.textureId = surface.textureId;
			candidate.debug.emissiveMode = surface.emissiveMode;
			candidate.debug.emissiveTextureIndex = surface.emissiveTextureIndex;
			candidate.debug.actorIndex = surface.actorIndex;
			candidate.debug.sectorIndex = surface.sectorIndex;
			candidate.debug.primitiveArea = primitiveArea;
			candidate.debug.powerEstimate = candidate.gpu.powerEstimate;
			candidate.debug.selectionWeight = candidate.gpu.selectionWeight;
			candidate.debug.selectionPdf = 0.0f;
			candidate.debug.emissiveIntensity = surface.emissiveIntensity * sectorResponseScale;
			candidate.debug.sectorResponseScale = sectorResponseScale;
			candidate.debug.sectorReachScale = sectorReachScale;
			candidate.debug.materialResponseEnabled = materialResponseEligible;
			candidate.debug.materialResponseScale = materialResponseScale;
			candidate.debug.sectorResponseApplied = sectorResponseApplied;
			Copy3f(surface.emissiveColor, candidate.debug.emissiveColor);
			ComputePrimitiveBounds(*geometry, localPrimitiveIndex, candidate.gpu.boundsCenter, candidate.gpu.boundsRadius);
			Copy3f(candidate.gpu.boundsCenter, candidate.debug.center);
			candidate.debug.boundsRadius = candidate.gpu.boundsRadius;

			candidate.gpu.stableKeyLo = (uint32_t)(candidate.debug.stableKey & 0xffffffffu);
			candidate.gpu.stableKeyHi = (uint32_t)(candidate.debug.stableKey >> 32u);
			candidates.push_back(candidate);

			if (materialResponseEligible)
			{
				const uint64_t responseKey = ((uint64_t)dataSource << 32u) | primitiveIndex;
				if (materialResponseLookup.find(responseKey) == materialResponseLookup.end())
				{
					materialResponseLookup.emplace(responseKey, (uint32_t)outMaterialResponses.size());
					NRIEmissiveMaterialResponseGpuData response = {};
					response.dataSource = dataSource;
					response.primitiveIndex = primitiveIndex;
					response.materialScale = std::max(0.0f, materialResponseScale);
					outMaterialResponses.push_back(response);
				}
			}
		}
	};
	auto appendPlacedVoxelRange = [&](const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface)
	{
		if (surface.sceneInstanceIndex == UINT32_MAX || surface.placedPrimitiveCount == 0u)
		{
			localStats.skippedPersistentVoxelSurfaces++;
			return;
		}
		const float samplingScale = ResolveGlowSamplingScale(surface.sourceFlags, surface.emissiveMode, settings) * std::max(surface.reachScale, 0.0f);
		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		bool sectorResponseApplied = false;
		const float sectorRawResponseScale = ResolveSectorEmissionScale(surface, sectorResponseApplied);
		const float sectorResponseScale = sectorResponseApplied ? ResolveSectorEmissionIntensityScale(surface, sectorRawResponseScale) : 1.0f;
		const float sectorReachScale = sectorResponseApplied ? ResolveSectorEmissionReachScale(surface, sectorRawResponseScale) : 1.0f;
		bool materialResponseApplied = false;
		const float materialResponseScale = ResolveEmissiveMaterialResponseScale(surface, materialResponseApplied);
		const float sectorReachBound = sectorResponseEligible ?
			(surface.hasSectorResponseReachMax ?
				std::max(std::max(0.0f, surface.sectorResponseReachMin), surface.sectorResponseReachMax) :
				std::max(std::max(0.0f, (float)nri_ptsectoremissionreachmin), (float)nri_ptsectoremissionreachmax)) :
			1.0f;

		BuiltCandidate candidate = {};
		candidate.gpu.dataSource = nri_diag::SceneDataSourcePersistentVoxel;
		candidate.gpu.primitiveIndex = surface.placedPrimitiveBase;
		candidate.gpu.primitiveCount = surface.placedPrimitiveCount;
		candidate.gpu.sceneInstanceIndex = surface.sceneInstanceIndex;
		candidate.gpu.occurrenceKeyLo = surface.occurrenceKeyLo;
		candidate.gpu.occurrenceKeyHi = surface.occurrenceKeyHi;
		candidate.gpu.occurrenceGeneration = surface.occurrenceGeneration;
		Copy3f(surface.center, candidate.gpu.boundsCenter);
		candidate.gpu.boundsRadius = std::max(surface.boundsRadius, 0.0f);
		candidate.gpu.sourceFlags = surface.sourceFlags;
		candidate.gpu.textureId = surface.textureId;
		candidate.gpu.primitiveArea = std::max(surface.surfaceArea, 0.0f);
		candidate.gpu.powerEstimate = std::max(surface.powerEstimate, 0.0f) * sectorResponseScale * materialResponseScale;
		candidate.gpu.selectionWeight = std::max(surface.powerEstimate, 0.0f) * samplingScale * sectorReachScale * materialResponseScale;
		candidate.gpu.emissionScale = sectorResponseScale * materialResponseScale;
		candidate.gpu.materialResponseScale = std::max(materialResponseScale, 0.0f);
		candidate.referenceProposalWeight = std::max(surface.powerEstimate, 0.0f) * samplingScale * sectorReachBound * materialResponseScale;
		candidate.hasReferenceProposalWeight = sectorResponseEligible;

		candidate.debug.stableKey = nri_scene::HashCombine64(surface.stableKey, 0x504C41434544564Full);
		candidate.debug.surfaceStableKey = surface.stableKey;
		candidate.debug.dataSource = candidate.gpu.dataSource;
		candidate.debug.primitiveIndex = candidate.gpu.primitiveIndex;
		candidate.debug.primitiveCount = candidate.gpu.primitiveCount;
		candidate.debug.sceneInstanceIndex = candidate.gpu.sceneInstanceIndex;
		candidate.debug.occurrenceKeyLo = candidate.gpu.occurrenceKeyLo;
		candidate.debug.occurrenceKeyHi = candidate.gpu.occurrenceKeyHi;
		candidate.debug.occurrenceGeneration = candidate.gpu.occurrenceGeneration;
		candidate.debug.materialIndex = surface.materialIndex;
		candidate.debug.sourceFlags = surface.sourceFlags;
		candidate.debug.sourceRuleId = surface.sourceRuleId;
		candidate.debug.overrideRuleId = surface.overrideRuleId;
		candidate.debug.textureId = surface.textureId;
		candidate.debug.emissiveMode = surface.emissiveMode;
		candidate.debug.emissiveTextureIndex = surface.emissiveTextureIndex;
		candidate.debug.actorIndex = surface.actorIndex;
		candidate.debug.sectorIndex = surface.sectorIndex;
		candidate.debug.boundsRadius = candidate.gpu.boundsRadius;
		candidate.debug.primitiveArea = candidate.gpu.primitiveArea;
		candidate.debug.powerEstimate = candidate.gpu.powerEstimate;
		candidate.debug.selectionWeight = candidate.gpu.selectionWeight;
		candidate.debug.emissiveIntensity = surface.emissiveIntensity * sectorResponseScale * materialResponseScale;
		candidate.debug.sectorResponseScale = sectorResponseScale;
		candidate.debug.sectorReachScale = sectorReachScale;
		candidate.debug.materialResponseEnabled = IsEmissiveSurfaceMaterialResponseEligible(surface);
		candidate.debug.materialResponseScale = materialResponseScale;
		candidate.debug.sectorResponseApplied = sectorResponseApplied;
		Copy3f(surface.center, candidate.debug.center);
		Copy3f(surface.emissiveColor, candidate.debug.emissiveColor);
		candidate.gpu.stableKeyLo = (uint32_t)(candidate.debug.stableKey & 0xffffffffu);
		candidate.gpu.stableKeyHi = (uint32_t)(candidate.debug.stableKey >> 32u);
		candidates.push_back(candidate);
	};

	for (const auto& surface : activeSurfaces)
	{
		switch (surface.source)
		{
		case SceneLightRecordSource::StaticMapScene:
			localStats.surfaceStatic++;
			appendSurfacePrimitives(surface, context.staticGeometry, staticRanges, nri_diag::SceneDataSourceStatic, 0u);
			break;
		case SceneLightRecordSource::CapturedScene:
			localStats.surfaceCaptured++;
			appendSurfacePrimitives(surface, context.capturedGeometry, capturedRanges, nri_diag::SceneDataSourceDynamic, 0u);
			break;
		case SceneLightRecordSource::RuntimeMutationScene:
			localStats.surfaceRuntimeMutation++;
			appendSurfacePrimitives(surface, context.runtimeMutationGeometry, runtimeMutationRanges, nri_diag::SceneDataSourceDynamic, context.runtimeMutationPrimitiveBaseOffset);
			break;
		case SceneLightRecordSource::DynamicScene:
			localStats.surfaceDynamic++;
			appendSurfacePrimitives(surface, context.dynamicGeometry, dynamicRanges, nri_diag::SceneDataSourceDynamic, context.dynamicPrimitiveBaseOffset);
			break;
		case SceneLightRecordSource::SurfaceLightOverlayScene:
			localStats.surfaceLightOverlay++;
			appendSurfacePrimitives(surface, context.surfaceLightOverlayGeometry, surfaceLightOverlayRanges, nri_diag::SceneDataSourceDynamic, context.surfaceLightOverlayPrimitiveBaseOffset);
			break;
		case SceneLightRecordSource::PersistentVoxelScene:
			localStats.surfacePersistentVoxel++;
			appendPlacedVoxelRange(surface);
			break;
		default:
			break;
		}
	}

	std::vector<NRIEmissiveSamplingDistributionCandidate> distributionCandidates;
	distributionCandidates.reserve(candidates.size());
	for (const auto& candidate : candidates)
	{
		NRIEmissiveSamplingDistributionCandidate distributionCandidate = {};
		distributionCandidate.stableKey = candidate.debug.stableKey;
		distributionCandidate.bindingKey = nri_scene::HashCombine64(
			candidate.debug.stableKey,
			(uint64_t)candidate.debug.emissiveMode);
		distributionCandidate.tieBreakKey = nri_scene::HashCombine64(
			nri_scene::HashCombine64((uint64_t)candidate.gpu.dataSource, (uint64_t)candidate.gpu.primitiveIndex),
			(uint64_t)candidate.gpu.sceneInstanceIndex);
		distributionCandidate.proposalWeight = candidate.gpu.selectionWeight;
		distributionCandidate.referenceProposalWeight = candidate.referenceProposalWeight;
		distributionCandidate.hasReferenceProposalWeight = candidate.hasReferenceProposalWeight;
		distributionCandidate.live = candidate.gpu.powerEstimate > 0.0f && candidate.gpu.emissionScale > 0.0f;
		distributionCandidates.push_back(distributionCandidate);
	}
	std::vector<NRIEmissiveSamplingDistributionEntry> distributionEntries;
	NRIEmissiveSamplingDistributionStats distributionStats = {};
	mEmissiveSamplingDistribution.Build(
		distributionCandidates,
		mFrameSerial,
		NriMaxEmissivePrimitives,
		distributionEntries,
		outCdf,
		&distributionStats);
	localStats.proposalBoundGrowthCount = distributionStats.boundGrowthCount;
	localStats.lastProposalBoundGrowthStableKey = distributionStats.lastBoundGrowthStableKey;
	localStats.lastProposalBoundGrowthOldWeight = distributionStats.lastBoundGrowthOldWeight;
	localStats.lastProposalBoundGrowthNewWeight = distributionStats.lastBoundGrowthNewWeight;
	localStats.lastProposalBoundGrowthWasAuthored = distributionStats.lastBoundGrowthWasAuthored;
	localStats.proposalActiveCount = distributionStats.activeCount;
	localStats.proposalRetainedDarkCount = distributionStats.retainedDarkCount;
	localStats.proposalReactivatedCount = distributionStats.reactivatedCount;
	localStats.proposalRetiredMissingCount = distributionStats.retiredMissingCount;
	localStats.proposalRetiredReplacedCount = distributionStats.retiredReplacedCount;
	localStats.proposalRecordCount = distributionStats.recordCount;

	outPrimitives.reserve(candidates.size());
	outDebugRecords.reserve(candidates.size());

	float totalPower = 0.0f;
	float dominantPower = -1.0f;

	for (const auto& distributionEntry : distributionEntries)
	{
		BuiltCandidate candidate = candidates[distributionEntry.inputIndex];
		// The distribution resolves duplicate authored keys into unique,
		// deterministically ordered identities. Publish that resolved key to
		// GPU consumers so temporal reservoirs cannot confuse two candidates
		// that shared the pre-distribution key.
		candidate.gpu.stableKeyLo = (uint32_t)(distributionEntry.stableKey & 0xffffffffu);
		candidate.gpu.stableKeyHi = (uint32_t)(distributionEntry.stableKey >> 32u);
		candidate.debug.stableKey = distributionEntry.stableKey;
		candidate.gpu.selectionWeight = distributionEntry.proposalWeight;
		candidate.gpu.selectionPdf = distributionEntry.selectionPdf;
		candidate.debug.selectionWeight = distributionEntry.proposalWeight;
		candidate.debug.selectionPdf = distributionEntry.selectionPdf;
		outPrimitives.push_back(candidate.gpu);
		outDebugRecords.push_back(candidate.debug);
		if (candidate.debug.dataSource == nri_diag::SceneDataSourceStatic)
		{
			localStats.outputStaticRecords++;
		}
		else if (candidate.debug.dataSource == nri_diag::SceneDataSourceDynamic)
		{
			localStats.outputDynamicRecords++;
		}
		else if (candidate.debug.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
		{
			localStats.outputPersistentVoxelRecords++;
			localStats.outputPersistentVoxelPrimitivesRepresented += candidate.gpu.primitiveCount;
		}
		totalPower += candidate.gpu.powerEstimate;
		if (candidate.gpu.powerEstimate > dominantPower)
		{
			dominantPower = candidate.gpu.powerEstimate;
			outHeader.dominantIndex = (uint32_t)outPrimitives.size() - 1u;
		}
	}

	outHeader.activeCount = (uint32_t)outPrimitives.size();
	outHeader.totalPower = totalPower;
	outMaterialResponses[0].dataSource = (uint32_t)outMaterialResponses.size() - 1u;
	if (outStats != nullptr)
	{
		*outStats = localStats;
	}

}

uint64_t SceneLightSystem::BuildEmissiveSamplingPayloadHash(const EmissiveSamplingBuildContext& context) const
{
	uint64_t hash = 1469598103934665603ull;
	hash = nri_scene::HashCombine64(hash, HashGeometryForEmissiveSampling(context.staticGeometry));
	hash = nri_scene::HashCombine64(hash, HashGeometryForEmissiveSampling(context.capturedGeometry));
	hash = nri_scene::HashCombine64(hash, HashGeometryForEmissiveSampling(context.runtimeMutationGeometry));
	hash = nri_scene::HashCombine64(hash, (uint64_t)context.runtimeMutationPrimitiveBaseOffset);
	hash = nri_scene::HashCombine64(hash, HashGeometryForEmissiveSampling(context.dynamicGeometry));
	hash = nri_scene::HashCombine64(hash, (uint64_t)context.dynamicPrimitiveBaseOffset);
	hash = nri_scene::HashCombine64(hash, HashGeometryForEmissiveSampling(context.surfaceLightOverlayGeometry));
	hash = nri_scene::HashCombine64(hash, (uint64_t)context.surfaceLightOverlayPrimitiveBaseOffset);

	hash = nri_scene::HashCombine64(hash, (uint64_t)mEmissiveSurfaces.activeSurfaces.size());
	for (const auto& surface : mEmissiveSurfaces.activeSurfaces)
	{
		hash = nri_scene::HashCombine64(hash, surface.stableKey);

		const auto propertyIt = mEmissiveSurfaces.activePropertyHashes.find(surface.stableKey);
		hash = nri_scene::HashCombine64(hash, propertyIt != mEmissiveSurfaces.activePropertyHashes.end() ? propertyIt->second : 0ull);

		const auto bindingIt = mEmissiveSurfaces.activeBindingHashes.find(surface.stableKey);
		hash = nri_scene::HashCombine64(hash, bindingIt != mEmissiveSurfaces.activeBindingHashes.end() ? bindingIt->second : 0ull);

		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		if (sectorResponseEligible)
		{
			const uint32_t sectorIndex = (uint32_t)surface.sectorIndex;
			bool applied = false;
			const float responseScale = ResolveSectorEmissionScale(surface, applied);
			const float intensityScale = applied ? ResolveSectorEmissionIntensityScale(surface, responseScale) : 1.0f;
			const float reachScale = applied ? ResolveSectorEmissionReachScale(surface, responseScale) : 1.0f;
			hash = nri_scene::HashCombine64(hash, (uint64_t)sectorIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(responseScale));
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(intensityScale));
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(reachScale));
		}
		if (IsEmissiveSurfaceMaterialResponseEligible(surface))
		{
			bool applied = false;
			const float materialScale = ResolveEmissiveMaterialResponseScale(surface, applied);
			hash = nri_scene::HashCombine64(hash, 0x4d415452455350ull);
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)surface.sectorIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(materialScale));
		}
	}

	return hash;
}

void SceneLightSystem::BuildSectorLightingUpload(
	float sectorLightMultiplier,
	bool sectorLightingEnabled,
	NRISectorLightHeaderGpuData& outHeader,
	std::vector<NRISectorLightGpuData>& outSectors) const
{
	const auto& registry = mSectorLighting;
	outHeader = {};
	outHeader.sectorCount = registry.sectorCount;
	outHeader.activeCount = registry.activeSectorCount;
	outHeader.pulsingCount = registry.pulsingSectorCount;
	outHeader.flags = sectorLightingEnabled ? 1u : 0u;
	outSectors.assign(registry.sectorCount, {});

	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		if (sectorIndex >= registry.sectors.size() || sectorIndex >= outSectors.size())
		{
			continue;
		}

		const auto& source = registry.sectors[sectorIndex];
		auto& target = outSectors[sectorIndex];
		Copy3f(source.ambientColor, target.ambientColor);
		Copy3f(source.ambientColor, target.hemisphereColor);
		target.ambientIntensity = source.ambientIntensity * sectorLightMultiplier;
		target.hemisphereAmount = source.hemisphereAmount * sectorLightMultiplier;
		target.fogAmount = source.fogAmount * sectorLightMultiplier;
		target.pulseScale = source.pulseScale;
		target.sourceFlags = source.sourceFlags;
		target.paletteIndex = source.paletteIndex;
		target.lotag = source.lotag;
		target.hitag = source.hitag;
	}
}

NRISectorLightingBoundState SceneLightSystem::BuildSectorLightingBoundState(float sectorLightMultiplier) const
{
	NRISectorLightingBoundState state = {};
	const auto& registry = mSectorLighting;
	state.sectorCount = registry.sectorCount;
	state.activeCount = registry.activeSectorCount;
	state.pulsingCount = registry.pulsingSectorCount;
	state.dominantSector = UINT32_MAX;
	state.dominantContribution = 0.0f;

	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		if (sectorIndex >= registry.sectors.size())
		{
			continue;
		}

		const auto& sector = registry.sectors[sectorIndex];
		const float contribution = sectorLightMultiplier * (sector.ambientIntensity + std::abs(sector.hemisphereAmount) + sector.fogAmount);
		if (contribution > state.dominantContribution)
		{
			state.dominantContribution = contribution;
			state.dominantSector = sectorIndex;
		}
	}

	return state;
}

uint64_t SceneLightSystem::BuildSectorLightingPayloadHash(float sectorLightMultiplier, bool sectorLightingEnabled) const
{
	const auto& registry = mSectorLighting;
	uint64_t hash = 1469598103934665603ull;
	hash = nri_scene::HashCombine64(hash, sectorLightingEnabled ? 1ull : 0ull);
	hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(sectorLightMultiplier));
	hash = nri_scene::HashCombine64(hash, (uint64_t)registry.sectorCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)registry.activeSectorCount);
	hash = nri_scene::HashCombine64(hash, (uint64_t)registry.pulsingSectorCount);
	for (uint32_t sectorIndex : registry.activeSectorIndices)
	{
		hash = nri_scene::HashCombine64(hash, (uint64_t)sectorIndex);
		if (sectorIndex >= registry.sectors.size())
		{
			continue;
		}

		const auto& sector = registry.sectors[sectorIndex];
		hash = nri_scene::HashCombine64(hash, (uint64_t)sector.sourceFlags);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(int64_t)sector.paletteIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(int64_t)sector.lotag);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(int64_t)sector.hitag);
		hash = nri_scene::HashCombine64(hash, (uint64_t)(int64_t)sector.averageShade);
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(sector.ambientColor[0]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(sector.ambientColor[1]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(sector.ambientColor[2]));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(sector.ambientIntensity));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(sector.hemisphereAmount));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(sector.fogAmount));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(sector.pulseScale));
	}

	return hash;
}

bool SceneLightSystem::AddRuntimePointLight(const float position[3], const float color[3], float intensity, float radius, uint32_t maxLights, uint32_t& outId)
{
	if (position == nullptr || color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	if (GetManualAnalyticLightCount() >= maxLights)
	{
		return false;
	}

	const uint32_t id = mNextRuntimePointLightId++;
	if (!AddManualAnalyticLight(id, position, color, intensity, radius))
	{
		return false;
	}

	outId = id;
	return true;
}

bool SceneLightSystem::UpdateRuntimePointLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius)
{
	return UpdateManualAnalyticLight(id, position, color, intensity, radius);
}

bool SceneLightSystem::RemoveRuntimePointLight(uint32_t id)
{
	return RemoveManualAnalyticLight(id);
}

bool SceneLightSystem::ClearRuntimePointLights()
{
	if (GetManualAnalyticLightCount() == 0)
	{
		return false;
	}

	ClearManualAnalyticLights();
	return true;
}

void SceneLightSystem::ResetRuntimePointLights()
{
	ClearManualAnalyticLights();
	mNextRuntimePointLightId = 1;
}

void SceneLightSystem::PrintRuntimePointLights(uint32_t maxLights) const
{
	const auto& analyticLights = GetAnalyticLights();
	Printf("NRI PT analytic lights: active=%u manual=%u muzzle_slots=%u muzzle_active=%u rules=%u overlay_rules=%u map_rules=%u matched_surfaces=%u overlay_matches=%u published_actors=%u published_fallback_activations=%u deduped=%u truncated=%u topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u limit=%u\n",
		(uint32_t)analyticLights.activeLights.size(),
		(uint32_t)analyticLights.manualLights.size(),
		analyticLights.transientMuzzleSlotCount,
		analyticLights.transientMuzzleActiveCount,
		(uint32_t)analyticLights.spriteTileRules.size(),
		analyticLights.actorOverlayRuleCount,
		analyticLights.mapOverlayRuleCount,
		analyticLights.matchedSurfaceCount,
		analyticLights.actorOverlayMatchedSurfaceCount,
		analyticLights.actorOverlayPublishedActorCount,
		analyticLights.actorOverlayPublishedFallbackActivationCount,
		analyticLights.dedupedMatchCount,
		analyticLights.truncatedLightCount,
		YesNo(analyticLights.lastBuildTopologyChanged),
		YesNo(analyticLights.lastBuildPropertiesChanged),
		(uint32_t)analyticLights.addedTopologyKeys.size(),
		(uint32_t)analyticLights.removedTopologyKeys.size(),
		(uint32_t)analyticLights.reboundTopologyKeys.size(),
		maxLights);
	if (analyticLights.activeLights.empty())
	{
		return;
	}

	for (const SceneAnalyticLight& light : analyticLights.activeLights)
	{
		const char* sourceBase =
			(light.sourceFlags & SceneAnalyticLightSourceFlag_Manual) != 0 ? "manual" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MuzzleFlash) != 0 ? "transient" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_ActorOverlay) != 0 ? "overlay" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MapOverlay) != 0 ? "overlay" :
			"heuristic";
		const char* sourceSuffix =
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MuzzleFlash) != 0 ? ":muzzle" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_SpriteTileHeuristic) != 0 ? ":sprite_tile" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_ActorOverlay) != 0 ? ":actor" :
			(light.sourceFlags & SceneAnalyticLightSourceFlag_MapOverlay) != 0 ? ":map" :
			"";
		const auto diagnosticIt = analyticLights.activeDiagnosticFlags.find(light.stableKey);
		const uint32_t diagnosticFlags = diagnosticIt != analyticLights.activeDiagnosticFlags.end() ? diagnosticIt->second : SceneLightDiagnosticFlag_None;
		Printf("NRI PT analytic light %u: id=%u topology=0x%016llx prev_match=%s added=%s rebound=%s prop_changed=%s shadow=%s source=%s%s rule=%u actor=%d tile=%u render_pos=(%.3f, %.3f, %.3f) color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f emitter_radius=%.3f\n",
			light.id,
			light.id,
			(unsigned long long)light.stableKey,
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PreviousMatch) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Added) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Rebound) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PropertyChanged) != 0),
			YesNo((light.flags & SceneAnalyticLightFlag_CastsShadow) != 0),
			sourceBase,
			sourceSuffix,
			light.sourceRuleId,
			light.actorIndex,
			light.textureId,
			light.position[0],
			light.position[1],
			light.position[2],
			light.color[0],
			light.color[1],
			light.color[2],
			light.intensity,
			light.radius,
			ResolveAnalyticEmitterRadius(light));
	}
}

void SceneLightSystem::RefreshResolvedMuzzleFlashRuleLookup(const ResolvedLightOverlaySet& resolvedLightOverlays)
{
	mResolvedMuzzleFlashRuleLookup.clear();
	mResolvedMuzzleFlashRuleLookup.reserve((size_t)resolvedLightOverlays.muzzleFlashRules.Size());
	for (const auto& rule : resolvedLightOverlays.muzzleFlashRules)
	{
		const std::string key = BuildNormalizedMuzzleFlashEventKey(rule.id);
		if (key.empty())
		{
			continue;
		}

		mResolvedMuzzleFlashRuleLookup[key] = rule;
	}
}

void SceneLightSystem::ResetMuzzleFlashOverlayState(const char* reason, uint32_t discardedEventCount, bool debug)
{
	mResolvedMuzzleFlashRuleLookup.clear();
	mLastMuzzleFlashEventSerial = 0;
	for (TransientMuzzleFlashSlot& slot : mTransientMuzzleFlashSlots)
	{
		slot.ruleId = 0;
		slot.sourceEventSerial = 0;
		slot.emitterActorIndex = -1;
		slot.renderPosition[0] = 0.0f;
		slot.renderPosition[1] = 0.0f;
		slot.renderPosition[2] = 0.0f;
		slot.color[0] = 1.0f;
		slot.color[1] = 1.0f;
		slot.color[2] = 1.0f;
		slot.peakIntensity = 0.0f;
		slot.radius = 0.0f;
		slot.activationTimeSeconds = 0.0;
		slot.endTimeSeconds = 0.0;
		slot.occupied = false;
	}
	mTransientMuzzleFlashLights.clear();
	SetTransientAnalyticLights(mTransientMuzzleFlashLights);

	if (discardedEventCount > 0 && debug)
	{
		Printf("NRI PT muzzle-flash reset: reason=%s discarded_events=%u\n",
			reason != nullptr ? reason : "unknown",
			discardedEventCount);
	}
}

std::string SceneLightSystem::FormatResolvedMuzzleFlashRuleIdList(size_t limit) const
{
	if (mResolvedMuzzleFlashRuleLookup.empty())
	{
		return "none";
	}

	std::vector<std::string> ids;
	ids.reserve(mResolvedMuzzleFlashRuleLookup.size());
	for (const auto& entry : mResolvedMuzzleFlashRuleLookup)
	{
		ids.push_back(entry.second.id.GetChars());
	}

	std::sort(ids.begin(), ids.end());

	std::string result;
	const size_t printCount = std::min(ids.size(), limit);
	for (size_t i = 0; i < printCount; ++i)
	{
		if (!result.empty())
		{
			result += ",";
		}

		result += ids[i];
	}

	if (printCount < ids.size())
	{
		char buffer[32] = {};
		std::snprintf(buffer, sizeof(buffer), ",...(+%u)", (unsigned)(ids.size() - printCount));
		result += buffer;
	}

	return result;
}

void SceneLightSystem::RefreshTransientMuzzleFlashLights(double currentTimeSeconds, const TArray<PathTracingWeaponLightEvent>& pendingEvents, bool debug)
{
	if (mTransientMuzzleFlashSlots.empty())
	{
		mTransientMuzzleFlashSlots.resize(NriPtMuzzleFlashSlotCount);
		for (uint32_t slotIndex = 0; slotIndex < (uint32_t)mTransientMuzzleFlashSlots.size(); ++slotIndex)
		{
			TransientMuzzleFlashSlot& slot = mTransientMuzzleFlashSlots[slotIndex];
			slot.stableKey = 0x4d555a5a4c450000ull | (uint64_t)slotIndex;
			slot.slotIndex = slotIndex;
			slot.color[0] = 1.0f;
			slot.color[1] = 1.0f;
			slot.color[2] = 1.0f;
		}
	}

	for (const PathTracingWeaponLightEvent& event : pendingEvents)
	{
		if (event.serial != 0 && event.serial <= mLastMuzzleFlashEventSerial)
		{
			continue;
		}
		mLastMuzzleFlashEventSerial = std::max(mLastMuzzleFlashEventSerial, event.serial);
		const std::string key = BuildNormalizedMuzzleFlashEventKey(event.eventId);
		const auto ruleIt = key.empty() ? mResolvedMuzzleFlashRuleLookup.end() : mResolvedMuzzleFlashRuleLookup.find(key);
		if (ruleIt == mResolvedMuzzleFlashRuleLookup.end())
		{
			if (debug)
			{
				Printf("NRI PT muzzle-flash ignored: event=%s serial=%llu reason=no-rule\n",
					event.eventId.GetChars(),
					(unsigned long long)event.serial);
			}
			continue;
		}

		const ResolvedLightOverlayMuzzleFlashRule& rule = ruleIt->second;
		const float baseIntensity = rule.hasIntensity ? std::max(rule.intensity, 0.0f) : 0.0f;
		const float baseRadius = rule.hasRadius ? std::max(rule.radius, 0.0f) : 0.0f;
		const float baseDelaySeconds = rule.hasDelaySeconds ? std::max(rule.delaySeconds, 0.0f) : 0.0f;
		const float baseDurationSeconds = rule.hasDurationSeconds ? std::max(rule.durationSeconds, 0.0f) : 0.0f;
		if (baseIntensity <= 0.0f || baseRadius <= 0.0f || baseDurationSeconds <= 0.0f)
		{
			if (debug)
			{
				Printf("NRI PT muzzle-flash ignored: event=%s serial=%llu reason=invalid-rule intensity=%.3f radius=%.3f duration=%.4f\n",
					event.eventId.GetChars(),
					(unsigned long long)event.serial,
					baseIntensity,
					baseRadius,
					baseDurationSeconds);
			}
			continue;
		}

		uint64_t randomState = BuildMuzzleFlashRandomSeed(event);
		const float intensityScale = rule.hasIntensityRandom ? ResolveMuzzleFlashRandomRange(randomState, rule.intensityRandomRange[0], rule.intensityRandomRange[1]) : 1.0f;
		const float radiusScale = rule.hasRadiusRandom ? ResolveMuzzleFlashRandomRange(randomState, rule.radiusRandomRange[0], rule.radiusRandomRange[1]) : 1.0f;
		const float delayRandomSeconds = rule.hasDelayRandomSeconds ? ResolveMuzzleFlashRandomRange(randomState, rule.delayRandomSecondsRange[0], rule.delayRandomSecondsRange[1]) : 0.0f;
		const float durationRandomSeconds = rule.hasDurationRandomSeconds ? ResolveMuzzleFlashRandomRange(randomState, rule.durationRandomSecondsRange[0], rule.durationRandomSecondsRange[1]) : 0.0f;
		const float resolvedPeakIntensity = std::max(baseIntensity * intensityScale, 0.0f);
		const float resolvedRadius = std::max(baseRadius * radiusScale, 0.0f);
		const float resolvedDelaySeconds = std::max(baseDelaySeconds + delayRandomSeconds, 0.0f);
		const float resolvedDurationSeconds = std::max(baseDurationSeconds + durationRandomSeconds, 0.001f);
		if (resolvedPeakIntensity <= 0.0f || resolvedRadius <= 0.0f)
		{
			continue;
		}

		DVector3 resolvedWorldPosition = event.worldPosition;
		if (rule.hasOffset && event.hasBasis)
		{
			resolvedWorldPosition +=
				event.basisRight * rule.offset[0] +
				event.basisForward * rule.offset[1] +
				event.basisUp * rule.offset[2];
		}

		float renderPosition[3] = {};
		WorldToPathTracingPosition(resolvedWorldPosition, renderPosition);

		size_t selectedSlotIndex = 0;
		bool foundReusableSlot = false;
		double oldestEndTimeSeconds = std::numeric_limits<double>::infinity();
		for (size_t slotIndex = 0; slotIndex < mTransientMuzzleFlashSlots.size(); ++slotIndex)
		{
			const TransientMuzzleFlashSlot& slot = mTransientMuzzleFlashSlots[slotIndex];
			if (!slot.occupied || slot.endTimeSeconds <= currentTimeSeconds)
			{
				selectedSlotIndex = slotIndex;
				foundReusableSlot = true;
				break;
			}

			if (slot.endTimeSeconds < oldestEndTimeSeconds)
			{
				oldestEndTimeSeconds = slot.endTimeSeconds;
				selectedSlotIndex = slotIndex;
			}
		}

		TransientMuzzleFlashSlot& slot = mTransientMuzzleFlashSlots[selectedSlotIndex];
		slot.ruleId = BuildMuzzleFlashRuleId(rule);
		slot.sourceEventSerial = event.serial;
		slot.emitterActorIndex = event.hasEmitterActorIndex ? event.emitterActorIndex : -1;
		Copy3f(renderPosition, slot.renderPosition);
		slot.color[0] = rule.hasColor ? std::max(rule.color[0], 0.0f) : 1.0f;
		slot.color[1] = rule.hasColor ? std::max(rule.color[1], 0.0f) : 1.0f;
		slot.color[2] = rule.hasColor ? std::max(rule.color[2], 0.0f) : 1.0f;
		slot.peakIntensity = resolvedPeakIntensity;
		slot.radius = resolvedRadius;
		slot.activationTimeSeconds = std::max(event.absoluteTimeSeconds, 0.0) + (double)resolvedDelaySeconds;
		slot.endTimeSeconds = slot.activationTimeSeconds + (double)resolvedDurationSeconds;
		slot.occupied = true;

		if (debug)
		{
			Printf("NRI PT muzzle-flash spawn: slot=%u reused=%s event=%s serial=%llu rule=%u actor=%d delay=%.4f duration=%.4f peak=%.3f radius=%.3f render_pos=(%.3f, %.3f, %.3f)\n",
				slot.slotIndex,
				YesNo(foundReusableSlot),
				event.eventId.GetChars(),
				(unsigned long long)event.serial,
				slot.ruleId,
				slot.emitterActorIndex,
				resolvedDelaySeconds,
				resolvedDurationSeconds,
				slot.peakIntensity,
				slot.radius,
				slot.renderPosition[0],
				slot.renderPosition[1],
				slot.renderPosition[2]);
		}
	}

	mTransientMuzzleFlashLights.clear();
	mTransientMuzzleFlashLights.reserve(mTransientMuzzleFlashSlots.size());
	for (TransientMuzzleFlashSlot& slot : mTransientMuzzleFlashSlots)
	{
		SceneAnalyticLight light = {};
		light.id = 0x8000u + slot.slotIndex;
		light.stableKey = slot.stableKey;
		light.sourceFlags = SceneAnalyticLightSourceFlag_MuzzleFlash;
		light.sourceRuleId = slot.ruleId;
		light.source = SceneLightRecordSource::None;
		light.actorIndex = slot.emitterActorIndex;
		Copy3f(slot.renderPosition, light.position);
		Copy3f(slot.color, light.color);
		light.intensity = EvaluateMuzzleFlashFadeOut(
			currentTimeSeconds,
			slot.occupied,
			slot.peakIntensity,
			slot.radius,
			slot.activationTimeSeconds,
			slot.endTimeSeconds);
		light.radius = light.intensity > 0.0f ? slot.radius : 0.0f;
		mTransientMuzzleFlashLights.push_back(light);

		if (slot.occupied && currentTimeSeconds >= slot.endTimeSeconds)
		{
			slot.ruleId = 0;
			slot.sourceEventSerial = 0;
			slot.emitterActorIndex = -1;
			slot.peakIntensity = 0.0f;
			slot.radius = 0.0f;
			slot.activationTimeSeconds = 0.0;
			slot.endTimeSeconds = 0.0;
			slot.occupied = false;
		}
	}

	SetTransientAnalyticLights(mTransientMuzzleFlashLights);
}

bool SceneLightSystem::IsEmissiveSurfaceSectorResponseEligible(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface) const
{
	return
		surface.sectorResponseEnabled &&
		surface.sectorIndex >= 0 &&
		(((surface.sourceFlags & SceneEmissiveSurfaceSourceFlag_LightOverlayOverride) != 0) ||
			(HasAutoEmissiveSourceFlags(surface.sourceFlags) &&
				(surface.sourceFlags & SceneEmissiveSurfaceSourceFlag_ExplicitTextureRule) == 0));
}

bool SceneLightSystem::IsEmissiveSurfaceMaterialResponseEligible(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface) const
{
	return
		surface.materialResponseEnabled &&
		surface.sectorIndex >= 0 &&
		(surface.materialResponseExplicit || IsEmissiveSurfaceSectorResponseEligible(surface));
}

float SceneLightSystem::ResolveSectorEmissionScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, bool& outApplied) const
{
	outApplied = false;
	if (!IsEmissiveSurfaceSectorResponseEligible(surface))
	{
		return 1.0f;
	}

	const uint32_t sectorIndex = (uint32_t)surface.sectorIndex;
	if (sectorIndex >= mSectorLighting.sectors.size())
	{
		return 1.0f;
	}

	const auto& sector = mSectorLighting.sectors[sectorIndex];
	const float scale = surface.hasSectorResponseInputRange ?
		ComputeSectorEmitterRangeResponseScale(
			sector.rawResponseSignal,
			surface.sectorResponseInputMin,
			surface.sectorResponseInputMax,
			std::max(0.0f, surface.sectorResponseMin),
			std::max(surface.sectorResponseMin, surface.sectorResponseMax)) :
		surface.hasSectorResponseParams ?
		ComputeSectorEmitterResponseScale(
			sector.rawResponseBrightness,
			GetSectorEmitterNeutralBrightness(),
			std::max(0.0f, surface.sectorResponseIntensity),
			std::max(0.0f, surface.sectorResponseMin),
			std::max(surface.sectorResponseMin, surface.sectorResponseMax)) :
		std::max(0.0f, sector.emitterResponseScale);
	outApplied = scale != 1.0f;
	return scale;
}

float SceneLightSystem::ResolveSectorEmissionIntensityScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, float scale) const
{
	const float authoredMinScale = ResolveSurfaceLightOverlayMinimumBrightnessScale(surface);
	const float minScale = std::max(
		surface.hasSectorResponseIntensityMin ?
			std::max(0.0f, surface.sectorResponseIntensityMin) :
			std::max(0.0f, (float)nri_ptsectoremissionlightmin),
		authoredMinScale);
	const float maxScale = surface.hasSectorResponseIntensityMax ?
		std::max(minScale, surface.sectorResponseIntensityMax) :
		std::max(minScale, (float)nri_ptsectoremissionlightmax);
	return std::clamp(std::max(0.0f, scale), minScale, maxScale);
}

float SceneLightSystem::ResolveSectorEmissionReachScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, float scale) const
{
	const float minScale = surface.hasSectorResponseReachMin ?
		std::max(0.0f, surface.sectorResponseReachMin) :
		std::max(0.0f, (float)nri_ptsectoremissionreachmin);
	const float maxScale = surface.hasSectorResponseReachMax ?
		std::max(minScale, surface.sectorResponseReachMax) :
		std::max(minScale, (float)nri_ptsectoremissionreachmax);
	return std::clamp(std::max(0.0f, scale), minScale, maxScale);
}

float SceneLightSystem::ResolveEmissiveMaterialResponseScale(const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, bool& outApplied) const
{
	outApplied = false;
	if (!IsEmissiveSurfaceMaterialResponseEligible(surface))
	{
		return 1.0f;
	}

	const uint32_t sectorIndex = (uint32_t)surface.sectorIndex;
	if (sectorIndex >= mSectorLighting.sectors.size())
	{
		return 1.0f;
	}

	const auto& sector = mSectorLighting.sectors[sectorIndex];
	const float globalMinScale = std::max(0.0f, (float)nri_ptsectoremissionmaterialmin);
	const float globalMaxScale = std::max(globalMinScale, (float)nri_ptsectoremissionmaterialmax);
	const float authoredMinScale = ResolveSurfaceLightOverlayMinimumBrightnessScale(surface);
	const float minScale = std::max(
		surface.hasMaterialResponseMin ? std::max(0.0f, surface.materialResponseMin) : globalMinScale,
		authoredMinScale);
	const float maxScale = surface.hasMaterialResponseMax ? std::max(minScale, surface.materialResponseMax) : std::max(minScale, globalMaxScale);
	const float scale = surface.hasSectorResponseInputRange ?
		ComputeSectorEmitterRangeResponseScale(
			sector.rawResponseSignal,
			surface.sectorResponseInputMin,
			surface.sectorResponseInputMax,
			minScale,
			maxScale) :
		surface.hasSectorResponseParams ?
		ComputeSectorEmitterResponseScale(
			sector.rawResponseBrightness,
			GetSectorEmitterNeutralBrightness(),
			std::max(0.0f, surface.sectorResponseIntensity),
			minScale,
			maxScale) :
		std::clamp(std::max(0.0f, sector.emitterResponseScale), minScale, maxScale);

	outApplied = scale != 1.0f;
	return scale;
}

uint64_t SceneLightSystem::BuildEmissiveSectorResponsePayloadHash() const
{
	uint64_t hash = 1469598103934665603ull;
	for (const auto& surface : mEmissiveSurfaces.activeSurfaces)
	{
		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		if (!sectorResponseEligible)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)surface.sectorIndex;
		bool applied = false;
		const float responseScale = ResolveSectorEmissionScale(surface, applied);
		const float intensityScale = applied ? ResolveSectorEmissionIntensityScale(surface, responseScale) : 1.0f;
		const float reachScale = applied ? ResolveSectorEmissionReachScale(surface, responseScale) : 1.0f;
		hash = nri_scene::HashCombine64(hash, surface.stableKey);
		hash = nri_scene::HashCombine64(hash, (uint64_t)sectorIndex);
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(responseScale));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(intensityScale));
		hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(reachScale));
	}

	return hash;
}

void SceneLightSystem::ResetEmissiveSectorResponseCaches()
{
	mEmissiveSectorResponseTraceCacheValid = false;
	mEmissiveSectorResponseTraceHash = 0;
	mEmissiveSectorResponseNotifyCacheValid = false;
	mLastEmissiveSectorResponseNotifyFrame = 0;
	mEmissiveSectorResponseNotifyScales.clear();
	mSectorLightingEditNotifyCacheValid = false;
	mLastSectorLightingEditNotifyFrame = 0;
	mSectorLightingEditNotifyHashes.clear();
}

void SceneLightSystem::NotifyEmissiveSectorResponseEditModeChanges(uint32_t frameIndex, const float currentCameraPos[3])
{
	if (!nri_ptemissivelighteditmode || currentCameraPos == nullptr)
	{
		mEmissiveSectorResponseNotifyCacheValid = false;
		mSectorLightingEditNotifyCacheValid = false;
		return;
	}

	const float nearbyRadius = std::max(0.0f, (float)nri_ptemissivelighteditnotifyrange);
	const float nearbyRadiusSq = nearbyRadius * nearbyRadius;
	if (mSectorLightingEditNotifyHashes.size() != mSectorLighting.sectorCount)
	{
		mSectorLightingEditNotifyHashes.assign(mSectorLighting.sectorCount, 0u);
		mSectorLightingEditNotifyCacheValid = false;
	}

	struct SectorSurfaceEditAggregate
	{
		uint64_t hash = 1469598103934665603ull;
		float center[3] = {};
		int64_t shadeSum = 0;
		int32_t minShade = std::numeric_limits<int32_t>::max();
		int32_t maxShade = std::numeric_limits<int32_t>::min();
		uint32_t count = 0;
	};

	std::vector<SectorSurfaceEditAggregate> sectorSurfaceAggregates(mSectorLighting.sectorCount);
	for (const SurfaceRecord& record : mSurfaceRecords)
	{
		if (record.provenance.sectorIndex < 0)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)record.provenance.sectorIndex;
		if (sectorIndex >= sectorSurfaceAggregates.size())
		{
			continue;
		}

		auto& aggregate = sectorSurfaceAggregates[sectorIndex];
		aggregate.center[0] += record.center[0];
		aggregate.center[1] += record.center[1];
		aggregate.center[2] += record.center[2];
		aggregate.shadeSum += record.material.shade;
		aggregate.minShade = std::min(aggregate.minShade, record.material.shade);
		aggregate.maxShade = std::max(aggregate.maxShade, record.material.shade);
		aggregate.count++;
		aggregate.hash = nri_scene::HashCombine64(aggregate.hash, (uint64_t)(uint32_t)record.material.shade);
		aggregate.hash = nri_scene::HashCombine64(aggregate.hash, (uint64_t)record.material.paletteIndex);
		aggregate.hash = nri_scene::HashCombine64(aggregate.hash, (uint64_t)FloatBits(record.material.lightLevel));
	}

	std::vector<uint32_t> changedNearbySurfaceSectors;
	std::vector<uint64_t> nextSurfaceHashes(mSectorLighting.sectorCount, 0u);
	for (uint32_t sectorIndex = 0; sectorIndex < (uint32_t)sectorSurfaceAggregates.size(); ++sectorIndex)
	{
		const auto& aggregate = sectorSurfaceAggregates[sectorIndex];
		if (aggregate.count == 0)
		{
			continue;
		}

		uint64_t hash = aggregate.hash;
		if (sectorIndex < mSectorLighting.sectors.size())
		{
			const auto& sector = mSectorLighting.sectors[sectorIndex];
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)sector.averageShade);
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)sector.rawAverageShade);
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)sector.paletteIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(sector.rawFloorLight));
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(sector.rawCeilingLight));
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(sector.emitterResponseScale));
		}
		nextSurfaceHashes[sectorIndex] = hash;

		if (!mSectorLightingEditNotifyCacheValid ||
			sectorIndex >= mSectorLightingEditNotifyHashes.size() ||
			mSectorLightingEditNotifyHashes[sectorIndex] == 0u ||
			mSectorLightingEditNotifyHashes[sectorIndex] == hash)
		{
			continue;
		}

		const float invCount = 1.0f / (float)aggregate.count;
		const float centerX = aggregate.center[0] * invCount;
		const float centerY = aggregate.center[1] * invCount;
		const float centerZ = aggregate.center[2] * invCount;
		const float dx = centerX - currentCameraPos[0];
		const float dy = centerY - currentCameraPos[1];
		const float dz = centerZ - currentCameraPos[2];
		if (dx * dx + dy * dy + dz * dz <= nearbyRadiusSq)
		{
			changedNearbySurfaceSectors.push_back(sectorIndex);
		}
	}

	if (!changedNearbySurfaceSectors.empty() && frameIndex - mLastSectorLightingEditNotifyFrame >= 12)
	{
		const uint32_t printCount = std::min<uint32_t>((uint32_t)changedNearbySurfaceSectors.size(), 6u);
		for (uint32_t i = 0; i < printCount; ++i)
		{
			const uint32_t sectorIndex = changedNearbySurfaceSectors[i];
			const auto& aggregate = sectorSurfaceAggregates[sectorIndex];
			const int32_t avgShade = aggregate.count > 0 ? (int32_t)(aggregate.shadeSum / (int64_t)aggregate.count) : 0;
			const SectorLightingRegistry::SectorLightRecord* sector =
				sectorIndex < mSectorLighting.sectors.size() ? &mSectorLighting.sectors[sectorIndex] : nullptr;
			Printf(
				PRINT_LOW | PRINT_NOTIFY,
				"NRI PT sector %u surface light changed avg_shade=%d range=[%d,%d] sector_raw=(%.2f,%.2f) signal=%.2f response=%.2f\n",
				sectorIndex,
				avgShade,
				aggregate.minShade,
				aggregate.maxShade,
				sector != nullptr ? sector->rawFloorLight : 0.0f,
				sector != nullptr ? sector->rawCeilingLight : 0.0f,
				sector != nullptr ? sector->rawResponseSignal : 0.0f,
				sector != nullptr ? sector->emitterResponseScale : 1.0f);
		}
		if (changedNearbySurfaceSectors.size() > printCount)
		{
			Printf(PRINT_LOW | PRINT_NOTIFY, "NRI PT sector surface light changed: +%u more nearby sectors\n", (uint32_t)changedNearbySurfaceSectors.size() - printCount);
		}
		mLastSectorLightingEditNotifyFrame = frameIndex;
	}

	mSectorLightingEditNotifyHashes = std::move(nextSurfaceHashes);
	mSectorLightingEditNotifyCacheValid = true;

	if (mEmissiveSectorResponseNotifyScales.size() != mSectorLighting.sectors.size())
	{
		mEmissiveSectorResponseNotifyScales.assign(mSectorLighting.sectors.size(), -1.0f);
		mEmissiveSectorResponseNotifyCacheValid = false;
	}

	std::vector<float> nextScales(mSectorLighting.sectors.size(), -1.0f);
	std::vector<uint32_t> changedNearbySectors;

	for (const auto& surface : mEmissiveSurfaces.activeSurfaces)
	{
		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		if (!sectorResponseEligible)
		{
			continue;
		}

		const uint32_t sectorIndex = (uint32_t)surface.sectorIndex;
		if (sectorIndex >= nextScales.size())
		{
			continue;
		}

		bool applied = false;
		const float scale = ResolveSectorEmissionScale(surface, applied);
		nextScales[sectorIndex] = std::max(nextScales[sectorIndex], scale);
		const float previousScale = sectorIndex < mEmissiveSectorResponseNotifyScales.size() ? mEmissiveSectorResponseNotifyScales[sectorIndex] : -1.0f;
		if (!mEmissiveSectorResponseNotifyCacheValid || previousScale < 0.0f || std::abs(previousScale - scale) <= 0.02f)
		{
			continue;
		}

		const float dx = surface.center[0] - currentCameraPos[0];
		const float dy = surface.center[1] - currentCameraPos[1];
		const float dz = surface.center[2] - currentCameraPos[2];
		if (dx * dx + dy * dy + dz * dz > nearbyRadiusSq)
		{
			continue;
		}

		if (std::find(changedNearbySectors.begin(), changedNearbySectors.end(), sectorIndex) == changedNearbySectors.end())
		{
			changedNearbySectors.push_back(sectorIndex);
		}
	}

	if (!changedNearbySectors.empty() && frameIndex - mLastEmissiveSectorResponseNotifyFrame >= 12)
	{
		for (uint32_t sectorIndex : changedNearbySectors)
		{
			const float scale = sectorIndex < nextScales.size() && nextScales[sectorIndex] >= 0.0f ? nextScales[sectorIndex] : 1.0f;
			const char* state = scale > 1.01f ? "boosted" : (scale < 0.99f ? "dimmed" : "neutral");
			Printf(PRINT_LOW | PRINT_NOTIFY, "NRI PT sector %u emission %s %.2fx\n", sectorIndex, state, scale);
		}
		mLastEmissiveSectorResponseNotifyFrame = frameIndex;
	}

	mEmissiveSectorResponseNotifyScales = std::move(nextScales);
	mEmissiveSectorResponseNotifyCacheValid = true;
}

void SceneLightSystem::TraceEmissiveSectorResponseChange(uint32_t frameIndex, const float currentCameraPos[3], bool traceEnabled)
{
	NotifyEmissiveSectorResponseEditModeChanges(frameIndex, currentCameraPos);

	if (!traceEnabled)
	{
		mEmissiveSectorResponseTraceCacheValid = false;
		return;
	}

	const uint64_t sectorResponsePayloadHash = BuildEmissiveSectorResponsePayloadHash();
	if (!mEmissiveSectorResponseTraceCacheValid)
	{
		mEmissiveSectorResponseTraceCacheValid = true;
		mEmissiveSectorResponseTraceHash = sectorResponsePayloadHash;
		return;
	}

	if (mEmissiveSectorResponseTraceHash == sectorResponsePayloadHash)
	{
		return;
	}

	uint32_t affectedEmitterCount = 0;
	for (const auto& surface : mEmissiveSurfaces.activeSurfaces)
	{
		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		if (sectorResponseEligible)
		{
			affectedEmitterCount++;
		}
	}

	Printf("NRI PT sector response change: frame=%u affected_emitters=%u total_emitters=%u sector_response_hash=0x%016llx->0x%016llx response=boost:%u dim:%u neutral:%u\n",
		frameIndex,
		affectedEmitterCount,
		(uint32_t)mEmissiveSurfaces.activeSurfaces.size(),
		(unsigned long long)mEmissiveSectorResponseTraceHash,
		(unsigned long long)sectorResponsePayloadHash,
		mSectorLighting.responseBoostSectorCount,
		mSectorLighting.responseDimSectorCount,
		mSectorLighting.responseNeutralSectorCount);

	mEmissiveSectorResponseTraceHash = sectorResponsePayloadHash;
}

bool SceneLightSystem::AddManualAnalyticLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius)
{
	if (position == nullptr || color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	SceneAnalyticLight light = {};
	light.id = id;
	light.stableKey = 0x4d414e55414c0000ull | (uint64_t)id;
	light.sourceFlags = SceneAnalyticLightSourceFlag_Manual;
	light.textureId = 0;
	light.position[0] = position[0];
	light.position[1] = position[1];
	light.position[2] = position[2];
	light.color[0] = std::max(color[0], 0.0f);
	light.color[1] = std::max(color[1], 0.0f);
	light.color[2] = std::max(color[2], 0.0f);
	light.intensity = intensity;
	light.radius = radius;
	mAnalyticLights.manualLights.push_back(light);
	return true;
}

bool SceneLightSystem::UpdateManualAnalyticLight(uint32_t id, const float position[3], const float color[3], float intensity, float radius)
{
	if (position == nullptr || color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	const auto it = std::find_if(mAnalyticLights.manualLights.begin(), mAnalyticLights.manualLights.end(), [id](const SceneAnalyticLight& light)
	{
		return light.id == id;
	});
	if (it == mAnalyticLights.manualLights.end())
	{
		return false;
	}

	it->position[0] = position[0];
	it->position[1] = position[1];
	it->position[2] = position[2];
	it->color[0] = std::max(color[0], 0.0f);
	it->color[1] = std::max(color[1], 0.0f);
	it->color[2] = std::max(color[2], 0.0f);
	it->intensity = intensity;
	it->radius = radius;
	return true;
}

bool SceneLightSystem::RemoveManualAnalyticLight(uint32_t id)
{
	const auto it = std::find_if(mAnalyticLights.manualLights.begin(), mAnalyticLights.manualLights.end(), [id](const SceneAnalyticLight& light)
	{
		return light.id == id;
	});
	if (it == mAnalyticLights.manualLights.end())
	{
		return false;
	}

	mAnalyticLights.manualLights.erase(it);
	return true;
}

void SceneLightSystem::ClearManualAnalyticLights()
{
	mAnalyticLights.manualLights.clear();
}

void SceneLightSystem::SetTransientAnalyticLights(const std::vector<SceneAnalyticLight>& lights)
{
	mAnalyticLights.transientLights = lights;
	mAnalyticLights.transientMuzzleSlotCount = (uint32_t)lights.size();
	mAnalyticLights.transientMuzzleActiveCount = 0;
	for (const SceneAnalyticLight& light : lights)
	{
		if ((light.sourceFlags & SceneAnalyticLightSourceFlag_MuzzleFlash) != 0 &&
			light.intensity > 0.0f &&
			light.radius > 0.0f)
		{
			mAnalyticLights.transientMuzzleActiveCount++;
		}
	}
}

bool SceneLightSystem::AddSpriteTileHeuristic(uint32_t textureId, const float color[3], float intensity, float radius, uint32_t flickerFrames, uint32_t& outRuleId)
{
	if (color == nullptr || intensity <= 0.0f || radius <= 0.0f)
	{
		return false;
	}

	AnalyticLightHeuristicRule rule = {};
	rule.ruleId = mAnalyticLights.nextRuleId++;
	rule.textureId = textureId;
	rule.color[0] = std::max(color[0], 0.0f);
	rule.color[1] = std::max(color[1], 0.0f);
	rule.color[2] = std::max(color[2], 0.0f);
	rule.intensity = intensity;
	rule.radius = radius;
	rule.flickerFrames = flickerFrames;
	mAnalyticLights.spriteTileRules.push_back(rule);
	outRuleId = rule.ruleId;
	return true;
}

bool SceneLightSystem::ClearSpriteTileHeuristics()
{
	if (mAnalyticLights.spriteTileRules.empty())
	{
		return false;
	}

	mAnalyticLights.spriteTileRules.clear();
	return true;
}

void SceneLightSystem::PrintSpriteTileLightHeuristics() const
{
	const auto& analyticLights = GetAnalyticLights();
	Printf("NRI PT analytic sprite-tile heuristics: rules=%u matched_surfaces=%u deduped=%u truncated=%u\n",
		(uint32_t)analyticLights.spriteTileRules.size(),
		analyticLights.matchedSurfaceCount,
		analyticLights.dedupedMatchCount,
		analyticLights.truncatedLightCount);
	for (const auto& rule : analyticLights.spriteTileRules)
	{
		Printf("NRI PT analytic heuristic %u: tile=%u color=(%.3f, %.3f, %.3f) intensity=%.3f radius=%.3f flicker_frames=%u\n",
			rule.ruleId,
			rule.textureId,
			rule.color[0],
			rule.color[1],
			rule.color[2],
			rule.intensity,
			rule.radius,
			rule.flickerFrames);
	}
}

bool SceneLightSystem::AddTextureEmissiveHeuristic(uint32_t textureId, uint32_t emissiveMode, float intensityScale, const float* emissiveColor, bool hasExplicitColor, uint32_t& outRuleId)
{
	if (textureId == 0 || intensityScale <= 0.0f || emissiveMode == nri_scene::MaterialEmissiveMode_None)
	{
		return false;
	}

	EmissiveSurfaceRegistry::EmissiveHeuristicRule rule = {};
	rule.ruleId = mEmissiveSurfaces.nextRuleId++;
	rule.textureId = textureId;
	rule.emissiveMode = emissiveMode;
	rule.intensityScale = intensityScale;
	rule.hasExplicitColor = hasExplicitColor && emissiveColor != nullptr;
	if (rule.hasExplicitColor)
	{
		rule.emissiveColor[0] = std::max(emissiveColor[0], 0.0f);
		rule.emissiveColor[1] = std::max(emissiveColor[1], 0.0f);
		rule.emissiveColor[2] = std::max(emissiveColor[2], 0.0f);
	}
	mEmissiveSurfaces.textureRules.push_back(rule);
	mEmissiveSurfaces.materialBindingChanged = true;
	mEmissiveSurfaces.materialPropertiesChanged = true;
	outRuleId = rule.ruleId;
	return true;
}

bool SceneLightSystem::ClearTextureEmissiveHeuristics()
{
	if (mEmissiveSurfaces.textureRules.empty())
	{
		return false;
	}

	mEmissiveSurfaces.textureRules.clear();
	mEmissiveSurfaces.materialBindingChanged = true;
	mEmissiveSurfaces.materialPropertiesChanged = true;
	return true;
}

void SceneLightSystem::PrintTextureEmissiveHeuristics() const
{
	const auto& emissive = GetEmissiveSurfaces();
	Printf("NRI PT emissive heuristics: rules=%u auto_tagged=%u explicit_matches=%u overrides=%u override_matches=%u material_response_rules=%u material_response_matches=%u active=%u total_power=%.3f glow_scale=%.3f glow_reach=%.3f glow_falloff=%.3f glow_blend=%.3f truncated=%u\n",
		(uint32_t)emissive.textureRules.size(),
		emissive.autoTaggedCount,
		emissive.explicitRuleMatchCount,
		emissive.overrideRuleCount,
		emissive.overrideMatchedSurfaceCount,
		emissive.materialResponseRuleCount,
		emissive.materialResponseMatchedSurfaceCount,
		(uint32_t)emissive.activeSurfaces.size(),
		emissive.totalPowerEstimate,
		(float)nri_ptglowscale,
		(float)nri_ptglowreach,
		(float)nri_ptglowfalloff,
		(float)nri_ptglowblend,
		emissive.truncatedSurfaceCount);
	for (const auto& rule : emissive.textureRules)
	{
		Printf("NRI PT emissive heuristic %u: tile=%u mode=%s intensity_scale=%.3f explicit_color=%s color=(%.3f, %.3f, %.3f)\n",
			rule.ruleId,
			rule.textureId,
			nri_diag::GetMaterialEmissiveModeName(rule.emissiveMode),
			rule.intensityScale,
			rule.hasExplicitColor ? "yes" : "no",
			rule.emissiveColor[0],
			rule.emissiveColor[1],
			rule.emissiveColor[2]);
	}
}

void SceneLightSystem::PrintEmissiveSurfaceDump(
	const std::vector<NRIEmissivePrimitiveDebugRecord>& boundPrimitiveRecords,
	float boundTotalPower,
	const float currentCameraPos[3],
	float radius,
	uint32_t limit) const
{
	if (boundPrimitiveRecords.empty())
	{
		Printf("NRI PT emissive primitives: no emissive primitive candidates are bound.\n");
		return;
	}

	struct Candidate
	{
		const NRIEmissivePrimitiveDebugRecord* record = nullptr;
		float distanceSq = 0.0f;
		uint32_t gpuIndex = UINT32_MAX;
	};

	std::vector<Candidate> candidates;
	candidates.reserve(boundPrimitiveRecords.size());
	const float radiusSq = radius > 0.0f ? radius * radius : -1.0f;
	for (uint32_t gpuIndex = 0; gpuIndex < (uint32_t)boundPrimitiveRecords.size(); ++gpuIndex)
	{
		const auto& record = boundPrimitiveRecords[gpuIndex];
		const float dx = record.center[0] - currentCameraPos[0];
		const float dy = record.center[1] - currentCameraPos[1];
		const float dz = record.center[2] - currentCameraPos[2];
		const float distanceSq = dx * dx + dy * dy + dz * dz;
		if (radiusSq >= 0.0f && distanceSq > radiusSq)
		{
			continue;
		}
		candidates.push_back({ &record, distanceSq, gpuIndex });
	}

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
	{
		return a.distanceSq < b.distanceSq;
	});

	Printf("NRI PT emissive primitives: active=%u source_surfaces=%u auto=%u explicit=%u overrides=%u override_matches=%u material_response_rules=%u material_response_matches=%u total_power=%.3f topo_changed=%s prop_changed=%s added=%u removed=%u rebound=%u min_surface=%.3f min_power=%.3f\n",
		(uint32_t)boundPrimitiveRecords.size(),
		(uint32_t)mEmissiveSurfaces.activeSurfaces.size(),
		mEmissiveSurfaces.autoTaggedCount,
		mEmissiveSurfaces.explicitRuleMatchCount,
		mEmissiveSurfaces.overrideRuleCount,
		mEmissiveSurfaces.overrideMatchedSurfaceCount,
		mEmissiveSurfaces.materialResponseRuleCount,
		mEmissiveSurfaces.materialResponseMatchedSurfaceCount,
		boundTotalPower,
		YesNo(mEmissiveSurfaces.lastBuildTopologyChanged),
		YesNo(mEmissiveSurfaces.lastBuildPropertiesChanged),
		(uint32_t)mEmissiveSurfaces.addedTopologyKeys.size(),
		(uint32_t)mEmissiveSurfaces.removedTopologyKeys.size(),
		(uint32_t)mEmissiveSurfaces.reboundTopologyKeys.size(),
		(float)nri_ptemissiveminsurface,
		(float)nri_ptemissiveminpower);

	const uint32_t printCount = std::min<uint32_t>((uint32_t)candidates.size(), limit);
	for (uint32_t i = 0; i < printCount; ++i)
	{
		const auto& record = *candidates[i].record;
		const auto diagnosticIt = mEmissiveSurfaces.activeDiagnosticFlags.find(record.surfaceStableKey);
		const uint32_t diagnosticFlags = diagnosticIt != mEmissiveSurfaces.activeDiagnosticFlags.end() ? diagnosticIt->second : SceneLightDiagnosticFlag_None;
		Printf("NRI PT emissive gpu_index=%u near_rank=%u: primitive_key=0x%016llx surface_key=0x%016llx prev_match=%s added=%s rebound=%s prop_changed=%s source=%s primitive=%u primitive_count=%u scene_instance=%u occurrence=0x%08x%08x generation=%u material=%u flags=0x%x rule=%u override_rule=%u actor=%d sector=%d sector_scale=%.3f reach_scale=%.3f sector_applied=%s material_response=%s material_scale=%.3f tile=%u mode=%s emissive_tex=%u area=%.2f power=%.3f sample_weight=%.3f pdf=%.6f center=(%.2f, %.2f, %.2f) bounds_radius=%.2f color=(%.3f, %.3f, %.3f) intensity=%.3f\n",
			candidates[i].gpuIndex,
			i,
			(unsigned long long)record.stableKey,
			(unsigned long long)record.surfaceStableKey,
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PreviousMatch) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Added) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_Rebound) != 0),
			YesNo((diagnosticFlags & SceneLightDiagnosticFlag_PropertyChanged) != 0),
			nri_diag::GetSceneDataSourceName(record.dataSource),
			record.primitiveIndex,
			record.primitiveCount,
			record.sceneInstanceIndex,
			record.occurrenceKeyHi,
			record.occurrenceKeyLo,
			record.occurrenceGeneration,
			record.materialIndex,
			record.sourceFlags,
			record.sourceRuleId,
			record.overrideRuleId,
			record.actorIndex,
			record.sectorIndex,
			record.sectorResponseScale,
			record.sectorReachScale,
			YesNo(record.sectorResponseApplied),
			YesNo(record.materialResponseEnabled),
			record.materialResponseScale,
			record.textureId,
			nri_diag::GetMaterialEmissiveModeName(record.emissiveMode),
			record.emissiveTextureIndex != UINT32_MAX ? record.emissiveTextureIndex : 0u,
			record.primitiveArea,
			record.powerEstimate,
			record.selectionWeight,
			record.selectionPdf,
			record.center[0],
			record.center[1],
			record.center[2],
			record.boundsRadius,
			record.emissiveColor[0],
			record.emissiveColor[1],
			record.emissiveColor[2],
			record.emissiveIntensity);
	}
}

void SceneLightSystem::PrintSceneLightDump(
	const float currentCameraPos[3],
	const nri_scene::PTMapWorld& mapWorld,
	uint32_t frameIndex,
	float radius,
	uint32_t limit) const
{
	if (!HasRecords())
	{
		Printf("NRI PT scene lights: no cached scene-light identity is available yet.\n");
		return;
	}

	struct Candidate
	{
		const SurfaceRecord* record = nullptr;
		float distanceSq = 0.0f;
	};

	std::vector<Candidate> candidates;
	candidates.reserve(mSurfaceRecords.size());
	const float radiusSq = radius > 0.0f ? radius * radius : -1.0f;

	for (const SurfaceRecord& record : mSurfaceRecords)
	{
		const float dx = record.center[0] - currentCameraPos[0];
		const float dy = record.center[1] - currentCameraPos[1];
		const float dz = record.center[2] - currentCameraPos[2];
		const float distanceSq = dx * dx + dy * dy + dz * dz;
		if (radiusSq >= 0.0f && distanceSq > radiusSq)
		{
			continue;
		}

		candidates.push_back({ &record, distanceSq });
	}

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
	{
		if (a.distanceSq != b.distanceSq)
		{
			return a.distanceSq < b.distanceSq;
		}
		return a.record->materialIndex < b.record->materialIndex;
	});

	const uint32_t requestedLimit = limit == 0 ? 32u : limit;
	const uint32_t printCount = (uint32_t)std::min<size_t>(candidates.size(), requestedLimit);
	Printf("NRI PT scene lights: cached_surface_identities=%u near_camera=%u radius=%.2f frame=%u\n",
		(uint32_t)mSurfaceRecords.size(),
		(uint32_t)candidates.size(),
		radius,
		frameIndex);

	for (uint32_t i = 0; i < printCount; ++i)
	{
		const SurfaceRecord& record = *candidates[i].record;
		const uint32_t lightingFlags = record.material.lightingFlags;
		const int32_t localSpaceIndex = record.provenance.mapChunkIndex >= 0 ? nri_scene::FindMapWorldLocalSpaceIndex(mapWorld, (uint32_t)record.provenance.mapChunkIndex) : -1;
		const int32_t portalGraphIndex = nri_scene::FindMapWorldPortalIndex(mapWorld, record.provenance);
		const char* textureName = record.material.texture != nullptr ? record.material.texture->GetName().GetChars() : "(null)";
		Printf("NRI PT scene light %u: source=%s drawlist=%s dist=%.2f center=(%.2f, %.2f, %.2f) radius=%.2f material=%u material_key=0x%016llx texture_key=0x%016llx glowmap_key=0x%016llx tile=%u texture=%s sector=%d wall=%d chunk=%d local_space=%d portal_graph=%d actor=%d palette=%u shade=%d alpha=%.3f light=%.3f flags=0x%x fullbright=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s emissive_mode=%s emissive_tex=%u avg=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
			i,
			GetSceneLightRecordSourceName(record.source),
			nri_diag::GetDrawListTypeName(record.provenance.drawListType),
			std::sqrt(candidates[i].distanceSq),
			record.center[0],
			record.center[1],
			record.center[2],
			record.boundsRadius,
			record.materialIndex,
			(unsigned long long)record.material.materialKey,
			(unsigned long long)record.material.textureContentKey,
			(unsigned long long)record.material.glowmapContentKey,
			record.material.textureId,
			textureName,
			record.provenance.sectorIndex,
			record.provenance.wallIndex,
			record.provenance.mapChunkIndex,
			localSpaceIndex,
			portalGraphIndex,
			record.provenance.actorIndex,
			record.material.paletteIndex,
			record.material.shade,
			record.material.alpha,
			record.material.lightLevel,
			record.material.materialFlags,
			(lightingFlags & nri_scene::MaterialLightingFlag_MaterialFullbright) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_TextureFullbright) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_TextureGlowing) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_TextureAutoGlowing) != 0 ? "yes" : "no",
			(lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0 ? "yes" : "no",
			nri_diag::GetMaterialEmissiveModeName(record.material.emissiveMode),
			record.material.emissiveTextureIndex != UINT32_MAX ? record.material.emissiveTextureIndex : 0u,
			record.material.averageColor[0],
			record.material.averageColor[1],
			record.material.averageColor[2],
			record.material.glowColor[0],
			record.material.glowColor[1],
			record.material.glowColor[2]);
	}

	if (printCount == 0)
	{
		Printf("NRI PT scene lights: no cached surfaces matched the requested radius.\n");
	}
}

bool SceneLightSystem::MaterialWouldEmit(const nri_scene::MaterialLightingMetadata& metadata) const
{
	const NRILightingSettings settings = CaptureSettings();
	uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
	uint32_t sourceRuleId = 0;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	float emissiveSamplingScale = 1.0f;
	float emissiveFalloffScale = 1.0f;
	return EvaluateEmissiveMaterial(mEmissiveSurfaces, metadata, settings, sourceFlags, sourceRuleId, emissiveColor, emissiveIntensity, emissiveMode, emissiveTextureIndex, emissiveSamplingScale, emissiveFalloffScale);
}

bool SceneLightSystem::ApplyEmissiveMaterialSettings(const nri_scene::MaterialLightingMetadata& metadata, nri_scene::MaterialData& inOutMaterial) const
{
	const NRILightingSettings settings = CaptureSettings();
	uint32_t sourceFlags = SceneEmissiveSurfaceSourceFlag_None;
	uint32_t sourceRuleId = 0;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	uint32_t emissiveMode = nri_scene::MaterialEmissiveMode_None;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	float emissiveSamplingScale = 1.0f;
	float emissiveFalloffScale = 1.0f;
	if (!EvaluateEmissiveMaterial(mEmissiveSurfaces, metadata, settings, sourceFlags, sourceRuleId, emissiveColor, emissiveIntensity, emissiveMode, emissiveTextureIndex, emissiveSamplingScale, emissiveFalloffScale))
	{
		inOutMaterial.emissiveColor[0] = 0.0f;
		inOutMaterial.emissiveColor[1] = 0.0f;
		inOutMaterial.emissiveColor[2] = 0.0f;
		inOutMaterial.emissiveIntensity = 0.0f;
		inOutMaterial.emissiveMaskScale = 0.0f;
		inOutMaterial.emissiveMode = nri_scene::MaterialEmissiveMode_None;
		inOutMaterial.emissiveTextureIndex = UINT32_MAX;
		return false;
	}

	inOutMaterial.emissiveColor[0] = emissiveColor[0];
	inOutMaterial.emissiveColor[1] = emissiveColor[1];
	inOutMaterial.emissiveColor[2] = emissiveColor[2];
	inOutMaterial.emissiveIntensity = emissiveIntensity;
	inOutMaterial.emissiveMaskScale = std::max(emissiveFalloffScale, 0.0f);
	inOutMaterial.emissiveMode = emissiveMode;
	inOutMaterial.emissiveTextureIndex = emissiveTextureIndex;
	if (inOutMaterial.materialClass != 3u)
	{
		inOutMaterial.materialClass = 2u;
	}
	return true;
}

bool SceneLightSystem::ConsumeAnalyticLightTopologyChanged()
{
	const bool changed = mAnalyticLights.topologyChanged;
	mAnalyticLights.topologyChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeAnalyticLightPropertiesChanged()
{
	const bool changed = mAnalyticLights.propertiesChanged;
	mAnalyticLights.propertiesChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeEmissiveSurfaceTopologyChanged()
{
	const bool changed = mEmissiveSurfaces.topologyChanged;
	mEmissiveSurfaces.topologyChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeEmissiveSurfacePropertiesChanged()
{
	const bool changed = mEmissiveSurfaces.propertiesChanged;
	mEmissiveSurfaces.propertiesChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeEmissiveMaterialBindingChanged()
{
	const bool changed = mEmissiveSurfaces.materialBindingChanged;
	mEmissiveSurfaces.materialBindingChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeEmissiveMaterialPropertiesChanged()
{
	const bool changed = mEmissiveSurfaces.materialPropertiesChanged;
	mEmissiveSurfaces.materialPropertiesChanged = false;
	return changed;
}

bool SceneLightSystem::ConsumeSectorLightingTopologyChanged()
{
	const bool changed = mSectorLighting.topologyChanged;
	mSectorLighting.topologyChanged = false;
	return changed;
}

void SceneLightSystem::AppendSurfaceRecord(SurfaceRecord record, uint32_t materialIndexBase)
{
	if (IsActorSuppressedForFrame(record.provenance.actorIndex))
	{
		return;
	}
	if (record.materialIndex != UINT32_MAX)
	{
		record.materialIndex += materialIndexBase;
	}

	const uint32_t recordIndex = (uint32_t)mSurfaceRecords.size();
	mSurfaceRecords.push_back(record);
	if ((record.material.materialFlags & nri_scene::MaterialFlag_Sprite) != 0)
	{
		mSurfaceRecordIndex.spriteRecordsByTextureId[record.material.textureId].push_back(recordIndex);
		if (record.provenance.actorIndex >= 0)
		{
			mSurfaceRecordIndex.spriteRecordsByActorIndex[record.provenance.actorIndex].push_back(recordIndex);
			mSurfaceRecordIndex.spriteRecordsByActorTexture[BuildActorTextureSurfaceIndexKey(record.provenance.actorIndex, record.material.textureId)].push_back(recordIndex);
		}
	}
	mFrameAppendStats.totalRecordCount++;
	switch (record.source)
	{
	case SceneLightRecordSource::StaticMapScene:
		mFrameAppendStats.staticRecordCount++;
		break;
	case SceneLightRecordSource::RuntimeMutationScene:
		mFrameAppendStats.runtimeMutationRecordCount++;
		break;
	case SceneLightRecordSource::DynamicScene:
		mFrameAppendStats.dynamicRecordCount++;
		break;
	case SceneLightRecordSource::SurfaceLightOverlayScene:
		mFrameAppendStats.surfaceLightOverlayRecordCount++;
		break;
	case SceneLightRecordSource::CapturedScene:
		mFrameAppendStats.capturedRecordCount++;
		break;
	case SceneLightRecordSource::PersistentVoxelScene:
		mFrameAppendStats.persistentVoxelRecordCount++;
		break;
	default:
		break;
	}
}

void SceneLightSystem::AppendSurfaceList(
	const std::vector<nri_scene::SurfaceRef>& surfaces,
	const nri_scene::MaterialBridgeData& materials,
	SceneLightRecordSource source,
	uint32_t materialIndexBase,
	uint32_t materialLookupIndexBase,
	uint32_t& inOutLocalMaterialIndex,
	const std::vector<uint64_t>* identityOverrides)
{
	for (size_t surfaceIndex = 0; surfaceIndex < surfaces.size(); ++surfaceIndex)
	{
		const nri_scene::SurfaceRef& surface = surfaces[surfaceIndex];
		const uint32_t materialLookupIndex = materialLookupIndexBase + inOutLocalMaterialIndex;
		const uint64_t inheritedIdentityKey =
			identityOverrides != nullptr && surfaceIndex < identityOverrides->size() ?
			(*identityOverrides)[surfaceIndex] :
			0ull;
		const SurfaceRecord record = BuildSurfaceRecord(
			surface,
			materials,
			source,
			inOutLocalMaterialIndex,
			materialLookupIndex,
			inheritedIdentityKey);

		AppendSurfaceRecord(record, materialIndexBase);
		++inOutLocalMaterialIndex;
	}
}
