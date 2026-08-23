#include "nri_renderer.h"

#include "nri_cvars.h"
#include "nri_diagnostic_names.h"
#include "nri_runtime_mutation.h"

#include "../scene/nri_map_world.h"
#include "lightoverlay.h"
#include "printf.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>

namespace
{
	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTargetMs(targetMs)
			, mStart(std::chrono::steady_clock::now())
		{
		}

		~ScopedPtPerfTimer()
		{
			const auto end = std::chrono::steady_clock::now();
			mTargetMs += std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - mStart).count();
		}

	private:
		double& mTargetMs;
		std::chrono::steady_clock::time_point mStart;
	};

	static const char* YesNo(bool value)
	{
		return value ? "yes" : "no";
	}

	static void Copy3(const float* src, float* dst)
	{
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
	}

	static void Normalize3(float v[3])
	{
		const float length = std::max(0.0001f, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}

	static bool ResolveSurfaceProbeTextureDebugInfo(uint32_t textureId, FString& outTextureName, int32_t& outLegacyTile)
	{
		outTextureName = "(none)";
		outLegacyTile = -1;
		if (textureId == 0)
		{
			return false;
		}

		auto texture = TexMan.GameByIndex((int)textureId);
		if (texture == nullptr)
		{
			return false;
		}

		outTextureName = texture->GetName();
		if (textureId >= (uint32_t)firstarttile && textureId <= (uint32_t)(firstarttile + maxarttile))
		{
			outLegacyTile = legacyTileNum(FSetTextureID((int)textureId));
		}
		return true;
	}

	static bool IsChunkMarkedVisible(const std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const uint32_t wordIndex = chunkIndex / 32u;
		const uint32_t bitIndex = chunkIndex % 32u;
		return wordIndex < visibleChunkWords.size() && (visibleChunkWords[wordIndex] & (1u << bitIndex)) != 0;
	}

	static bool IsFlatPlaneMarkedVisible(const std::vector<uint32_t>& visibleFlatPlaneWords, int32_t sectorIndex, bool ceiling)
	{
		if (sectorIndex < 0)
		{
			return false;
		}

		const uint32_t planeIndex = (uint32_t)sectorIndex * 2u + (ceiling ? 1u : 0u);
		const uint32_t wordIndex = planeIndex / 32u;
		const uint32_t bitIndex = planeIndex % 32u;
		return wordIndex < visibleFlatPlaneWords.size() && (visibleFlatPlaneWords[wordIndex] & (1u << bitIndex)) != 0;
	}

	static bool IntersectProbeTriangle(const nri_scene::SceneVertex& v0, const nri_scene::SceneVertex& v1, const nri_scene::SceneVertex& v2, const float origin[3], const float direction[3], float& outT)
	{
		outT = 0.0f;
		const float edge1[3] = {
			v1.position[0] - v0.position[0],
			v1.position[1] - v0.position[1],
			v1.position[2] - v0.position[2]
		};
		const float edge2[3] = {
			v2.position[0] - v0.position[0],
			v2.position[1] - v0.position[1],
			v2.position[2] - v0.position[2]
		};
		const float p[3] = {
			direction[1] * edge2[2] - direction[2] * edge2[1],
			direction[2] * edge2[0] - direction[0] * edge2[2],
			direction[0] * edge2[1] - direction[1] * edge2[0]
		};
		const float det = edge1[0] * p[0] + edge1[1] * p[1] + edge1[2] * p[2];
		if (fabsf(det) < 1e-5f)
		{
			return false;
		}

		const float invDet = 1.0f / det;
		const float t[3] = {
			origin[0] - v0.position[0],
			origin[1] - v0.position[1],
			origin[2] - v0.position[2]
		};
		const float u = (t[0] * p[0] + t[1] * p[1] + t[2] * p[2]) * invDet;
		if (u < 0.0f || u > 1.0f)
		{
			return false;
		}

		const float q[3] = {
			t[1] * edge1[2] - t[2] * edge1[1],
			t[2] * edge1[0] - t[0] * edge1[2],
			t[0] * edge1[1] - t[1] * edge1[0]
		};
		const float v = (direction[0] * q[0] + direction[1] * q[1] + direction[2] * q[2]) * invDet;
		if (v < 0.0f || (u + v) > 1.0f)
		{
			return false;
		}

		const float hitT = (edge2[0] * q[0] + edge2[1] * q[1] + edge2[2] * q[2]) * invDet;
		if (hitT <= 0.001f)
		{
			return false;
		}

		outT = hitT;
		return true;
	}

	static bool SameSurfaceProbeIdentity(const NRISurfaceProbeResult& a, const NRISurfaceProbeResult& b)
	{
		if (a.valid != b.valid || a.hit != b.hit)
		{
			return false;
		}
		if (!a.valid || !a.hit)
		{
			return true;
		}

		return
			a.provenance.sourceType == b.provenance.sourceType &&
			a.provenance.sectorIndex == b.provenance.sectorIndex &&
			a.provenance.wallIndex == b.provenance.wallIndex &&
			a.provenance.nextSectorIndex == b.provenance.nextSectorIndex &&
			a.provenance.actorIndex == b.provenance.actorIndex &&
			a.provenance.drawListType == b.provenance.drawListType &&
			a.provenance.cstat == b.provenance.cstat &&
			a.textureId == b.textureId &&
			a.baseTextureId == b.baseTextureId &&
			a.materialLightingFlags == b.materialLightingFlags &&
			a.semanticTextureIndex == b.semanticTextureIndex &&
			a.semanticPaletteIndex == b.semanticPaletteIndex &&
			a.metadataTextureIndex == b.metadataTextureIndex &&
			a.metadataPaletteIndex == b.metadataPaletteIndex &&
			a.gpuTextureIndex == b.gpuTextureIndex &&
			a.gpuPaletteIndex == b.gpuPaletteIndex &&
			a.expectedGpuTextureIndex == b.expectedGpuTextureIndex &&
			a.gpuMaterialValid == b.gpuMaterialValid &&
			a.expectedGpuTextureValid == b.expectedGpuTextureValid &&
			a.semanticMetadataMatch == b.semanticMetadataMatch &&
			a.gpuPaletteMatch == b.gpuPaletteMatch &&
			a.gpuTextureMatch == b.gpuTextureMatch &&
			a.textureSlotRevision == b.textureSlotRevision &&
			a.materialGeneration == b.materialGeneration &&
			a.shaderMaterialValid == b.shaderMaterialValid &&
			a.shaderHitMatch == b.shaderHitMatch &&
			a.shaderDataSource == b.shaderDataSource &&
			a.shaderPrimitiveIndex == b.shaderPrimitiveIndex &&
			a.shaderMaterialIndex == b.shaderMaterialIndex &&
			a.shaderTextureIndex == b.shaderTextureIndex &&
			a.shaderPaletteIndex == b.shaderPaletteIndex &&
			a.primitiveFlags == b.primitiveFlags &&
			a.sceneDataSource == b.sceneDataSource &&
			a.sceneOwner == b.sceneOwner &&
			a.materialIndex == b.materialIndex &&
			(a.provenance.sourceType != nri_scene::SurfaceSourceType::Unknown || a.primitiveIndex == b.primitiveIndex);
	}

	static bool SurfaceProvenanceMatches(const nri_scene::SurfaceProvenance& a, const nri_scene::SurfaceProvenance& b)
	{
		return
			a.sourceType == b.sourceType &&
			a.drawListType == b.drawListType &&
			a.mapChunkIndex == b.mapChunkIndex &&
			a.sectionIndex == b.sectionIndex &&
			a.sectorIndex == b.sectorIndex &&
			a.wallIndex == b.wallIndex &&
			a.nextSectorIndex == b.nextSectorIndex &&
			a.actorIndex == b.actorIndex &&
			a.cstat == b.cstat;
	}

	static SceneLightRecordSource MapSurfaceProbeOwnerToLightSource(uint32_t owner)
	{
		switch (owner)
		{
		case nri_diag::SurfaceProbeOwnerStaticMap: return SceneLightRecordSource::StaticMapScene;
		case nri_diag::SurfaceProbeOwnerCapturedScene: return SceneLightRecordSource::CapturedScene;
		case nri_diag::SurfaceProbeOwnerRuntimeMutation: return SceneLightRecordSource::RuntimeMutationScene;
		case nri_diag::SurfaceProbeOwnerDynamicOverlay: return SceneLightRecordSource::DynamicScene;
		default: return SceneLightRecordSource::CapturedScene;
		}
	}
}

void NRIRenderer::UpdateSurfaceProbe(
	const nri_scene::GeometryData& geometry,
	const nri_scene::MaterialBridgeData* materials,
	const std::vector<nri_scene::MaterialData>* gpuMaterials,
	bool allowLogging)
{
	ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.surfaceProbeMs);
	const bool logSurfaceProbe = allowLogging && nri_ptsurfaceprobe > 0;
	if (geometry.primitives.empty())
	{
		NRISurfaceProbeResult miss = {};
		miss.valid = true;
		mSurfaceProbe.SetLast(miss);
		if (!logSurfaceProbe)
		{
			return;
		}

		const bool logOnChangeOnly = nri_ptsurfaceprobe >= 2;
		if (logOnChangeOnly && mSurfaceProbe.LastLogged().valid && !mSurfaceProbe.LastLogged().hit)
		{
			return;
		}

		Printf("NRI PT surface probe: miss\n");
		mSurfaceProbe.SetLastLogged(miss);
		return;
	}

	NRISurfaceProbeResult result = {};
	result.valid = true;

	float direction[3] = { mCurrentCameraForward[0], mCurrentCameraForward[1], mCurrentCameraForward[2] };
	Normalize3(direction);

	float bestDistance = std::numeric_limits<float>::infinity();
	for (uint32_t primitiveIndex = 0; primitiveIndex < geometry.primitives.size(); ++primitiveIndex)
	{
		const auto& primitive = geometry.primitives[primitiveIndex];
		if ((primitive.flags & nri_scene::PrimitiveFlag_ReflectionOnly) != 0)
		{
			continue;
		}

		const auto& v0 = geometry.vertices[primitive.indices[0]];
		const auto& v1 = geometry.vertices[primitive.indices[1]];
		const auto& v2 = geometry.vertices[primitive.indices[2]];
		float hitT = 0.0f;
		if (!IntersectProbeTriangle(v0, v1, v2, mCurrentCameraPos, direction, hitT) || hitT >= bestDistance)
		{
			continue;
		}

		bestDistance = hitT;
		result.hit = true;
		result.primitiveIndex = primitiveIndex;
		result.materialIndex = primitive.materialIndex;
		result.primitiveFlags = primitive.flags;
		result.distance = hitT;
		result.position[0] = mCurrentCameraPos[0] + direction[0] * hitT;
		result.position[1] = mCurrentCameraPos[1] + direction[1] * hitT;
		result.position[2] = mCurrentCameraPos[2] + direction[2] * hitT;
		result.normal[0] = primitive.normal[0];
		result.normal[1] = primitive.normal[1];
		result.normal[2] = primitive.normal[2];
		if (primitiveIndex < geometry.primitiveProvenance.size())
		{
			result.provenance = geometry.primitiveProvenance[primitiveIndex];
		}
	}

	if (result.hit &&
		materials != nullptr &&
		result.materialIndex < materials->lightMetadata.size() &&
		result.materialIndex < materials->materials.size())
	{
		const auto& metadata = materials->lightMetadata[result.materialIndex];
		const auto& materialData = materials->materials[result.materialIndex];
		result.materialLightingFlags = metadata.lightingFlags;
		result.semanticTextureIndex = materialData.textureIndex;
		result.semanticPaletteIndex = materialData.paletteIndex;
		result.metadataTextureIndex = metadata.textureIndex;
		result.metadataPaletteIndex = metadata.paletteIndex;
		result.semanticMetadataMatch =
			materialData.textureIndex == metadata.textureIndex &&
			materialData.paletteIndex == metadata.paletteIndex;
		result.textureId = metadata.textureId;
		result.baseTextureId = metadata.baseTextureId != 0 ? metadata.baseTextureId : metadata.textureId;
		result.materialClass = metadata.materialClass;
		result.lightLevel = metadata.lightLevel;
		result.alpha = metadata.alpha;
		result.normalTextureIndex = metadata.normalTextureIndex;
		result.metallicTextureIndex = metadata.metallicTextureIndex;
		result.roughnessTextureIndex = metadata.roughnessTextureIndex;
		result.metalnessHint = materialData.metalnessHint;
		result.roughnessHint = materialData.roughnessHint;
		Copy3(metadata.averageColor, result.averageColor);
		Copy3(metadata.emissiveColor, result.emissiveColor);
		Copy3(metadata.glowColor, result.glowColor);

		nri_scene::MaterialData effectiveMaterial = {};
		effectiveMaterial.textureIndex = metadata.textureIndex;
		effectiveMaterial.paletteIndex = metadata.paletteIndex;
		effectiveMaterial.flags = metadata.materialFlags;
		effectiveMaterial.materialClass = metadata.materialClass;
		effectiveMaterial.lightLevel = metadata.lightLevel;
		effectiveMaterial.alpha = metadata.alpha;
		effectiveMaterial.normalTextureIndex = materialData.normalTextureIndex;
		effectiveMaterial.metallicTextureIndex = materialData.metallicTextureIndex;
		effectiveMaterial.roughnessTextureIndex = materialData.roughnessTextureIndex;
		effectiveMaterial.metalnessHint = materialData.metalnessHint;
		effectiveMaterial.roughnessHint = materialData.roughnessHint;
		effectiveMaterial.emissiveTextureIndex = metadata.emissiveTextureIndex;
		mSceneLights.ApplyEmissiveMaterialSettings(metadata, effectiveMaterial);
		result.emissiveMode = effectiveMaterial.emissiveMode;
		result.emissiveTextureIndex = effectiveMaterial.emissiveTextureIndex;
	}
	if (result.hit && gpuMaterials != nullptr && result.materialIndex < gpuMaterials->size())
	{
		const auto& gpuMaterial = (*gpuMaterials)[result.materialIndex];
		result.gpuMaterialValid = true;
		result.gpuTextureIndex = gpuMaterial.textureIndex;
		result.gpuPaletteIndex = gpuMaterial.paletteIndex;
		result.gpuPaletteMatch =
			result.semanticPaletteIndex != UINT32_MAX &&
			gpuMaterial.paletteIndex == result.semanticPaletteIndex;
	}

	if (result.hit)
	{
		if (!mSurfaceProbeFrame.valid)
		{
			result.sceneDataSource = UINT32_MAX;
			result.sceneOwner = nri_diag::SurfaceProbeOwnerUnknown;
		}
		else if (!mSurfaceProbeFrame.usesStaticMapScene)
		{
			result.sceneDataSource = nri_diag::SceneDataSourceDynamic;
			result.sceneOwner = nri_diag::SurfaceProbeOwnerCapturedScene;
		}
		else if (result.primitiveIndex < mSurfaceProbeFrame.staticPrimitiveCount)
		{
			result.sceneDataSource = nri_diag::SceneDataSourceStatic;
			result.sceneOwner = nri_diag::SurfaceProbeOwnerStaticMap;
		}
		else
		{
			uint32_t overlayPrimitiveIndex = result.primitiveIndex - mSurfaceProbeFrame.staticPrimitiveCount;
			result.sceneDataSource = nri_diag::SceneDataSourceDynamic;
			if (overlayPrimitiveIndex < mSurfaceProbeFrame.runtimeSpaceLinkPrimitiveCount)
			{
				result.sceneOwner = nri_diag::SurfaceProbeOwnerRuntimeLink;
			}
			else
			{
				overlayPrimitiveIndex -= std::min(overlayPrimitiveIndex, mSurfaceProbeFrame.runtimeSpaceLinkPrimitiveCount);
				if (overlayPrimitiveIndex < mSurfaceProbeFrame.runtimeMutationPrimitiveCount)
				{
					result.sceneOwner = nri_diag::SurfaceProbeOwnerRuntimeMutation;
				}
				else
				{
					overlayPrimitiveIndex -= std::min(overlayPrimitiveIndex, mSurfaceProbeFrame.runtimeMutationPrimitiveCount);
					result.sceneOwner = overlayPrimitiveIndex < mSurfaceProbeFrame.dynamicPrimitiveCount ?
						nri_diag::SurfaceProbeOwnerDynamicOverlay :
						nri_diag::SurfaceProbeOwnerUnknown;
				}
			}
		}
	}
	if (result.hit && result.sceneOwner == nri_diag::SurfaceProbeOwnerStaticMap)
	{
		result.gpuMaterialsUseStableTextureSlots = mStaticMapScene.gpuMaterialsUseStableTextureSlots;
		result.textureSlotRevision = mSceneTextures.SlotTable().MappingRevision();
		result.materialGeneration = mStaticMapScene.materialGeneration;
		result.materialBufferPayloadHash = mStaticMaterialBuffer.payloadHash;
		if (materials != nullptr &&
			result.semanticTextureIndex < materials->textures.size() &&
			result.gpuMaterialsUseStableTextureSlots)
		{
			const NRISceneTextureSlotHandle slot =
				mSceneTextures.SlotTable().Lookup(materials->textures[result.semanticTextureIndex].key);
			if (slot)
			{
				result.expectedGpuTextureValid = true;
				result.expectedGpuTextureIndex = slot.slot;
			}
		}
		else if (!result.gpuMaterialsUseStableTextureSlots && result.semanticTextureIndex != UINT32_MAX)
		{
			result.expectedGpuTextureValid = true;
			result.expectedGpuTextureIndex = result.semanticTextureIndex;
		}
	}
	if (result.gpuMaterialValid && result.expectedGpuTextureValid)
	{
		result.gpuTextureMatch = result.gpuTextureIndex == result.expectedGpuTextureIndex;
	}
	if (mLastPerfTraceShaderStats.valid && mLastPerfTraceShaderStats.surfaceProbe.valid)
	{
		const NRITraceShaderSurfaceProbe& shaderProbe = mLastPerfTraceShaderStats.surfaceProbe;
		result.shaderMaterialValid = true;
		result.shaderFrameNumber = mLastPerfTraceShaderStats.frameNumber;
		result.shaderDataSource = shaderProbe.dataSource;
		result.shaderInstanceIndex = shaderProbe.instanceId;
		result.shaderPrimitiveIndex = shaderProbe.primitiveIndex;
		result.shaderMaterialIndex = shaderProbe.materialIndex;
		result.shaderTextureIndex = shaderProbe.textureIndex;
		result.shaderPaletteIndex = shaderProbe.paletteIndex;
		result.shaderMaterialFlags = shaderProbe.flags;
		result.shaderLightingFlags = shaderProbe.lightingFlags;
		result.shaderHitMatch =
			shaderProbe.dataSource == result.sceneDataSource &&
			shaderProbe.primitiveIndex == result.primitiveIndex &&
			shaderProbe.materialIndex == result.materialIndex;
	}

	mSurfaceProbe.SetLast(result);
	if (!logSurfaceProbe)
	{
		return;
	}

	const bool logOnChangeOnly = nri_ptsurfaceprobe >= 2;
	if (logOnChangeOnly && SameSurfaceProbeIdentity(mSurfaceProbe.LastLogged(), result))
	{
		return;
	}

	if (!result.hit)
	{
		Printf("NRI PT surface probe: miss\n");
		mSurfaceProbe.SetLastLogged(result);
		return;
	}

	const NRISurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(result);

	const uint32_t flags = result.primitiveFlags;
	const uint32_t lightingFlags = result.materialLightingFlags;
	const int32_t localSpaceIndex = result.provenance.mapChunkIndex >= 0 ? nri_scene::FindMapWorldLocalSpaceIndex(mMapWorld, (uint32_t)result.provenance.mapChunkIndex) : -1;
	const int32_t portalGraphIndex = nri_scene::FindMapWorldPortalIndex(mMapWorld, result.provenance);
	bool chunkResidentStatic = false;
	bool chunkStaticTlasInstanced = false;
	bool chunkStaticProbeIncluded = false;
	bool chunkVisibleGate = false;
	bool flatPlaneVisibilityRelevant = false;
	bool flatPlaneVisible = false;
	bool chunkReplaced = false;
	bool chunkSectorDirty = false;
	bool chunkDragged = false;
	bool chunkBlindSpot = false;
	uint32_t chunkReasonMask = 0;
	uint32_t chunkSectionDirtyCount = 0;
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	if (result.provenance.mapChunkIndex >= 0)
	{
		const uint32_t chunkIndex = (uint32_t)result.provenance.mapChunkIndex;
		chunkVisibleGate = IsChunkMarkedVisible(mCurrentVisibleChunkWords, chunkIndex);
		const uint32_t preferredChunkListIndex = NRIStaticSceneResidency::FindPreferredStaticSceneChunkListIndex(
			mStaticMapScene,
			mStaticMapChunkAtlas,
			chunkIndex);
		if (preferredChunkListIndex != UINT32_MAX)
		{
			chunkResidentStatic = true;
			const auto& chunkCache = mStaticMapScene.chunks[preferredChunkListIndex];
			chunkStaticTlasInstanced =
				chunkCache.active &&
				chunkCache.accelerationStructure.accelerationStructure != nullptr;
			chunkStaticProbeIncluded = chunkCache.active;
		}
		if (const auto* replacement = mRuntimeMutation.FindReplacement(chunkIndex))
		{
			chunkReplaced = replacement->active;
			chunkSectorDirty = replacement->sectorDirty;
			chunkDragged = replacement->dragged;
			chunkBlindSpot = replacement->blindSpot;
			chunkReasonMask = replacement->reasonMask;
			chunkSectionDirtyCount = replacement->sectionDirtyCount;
			replacementSurfaceCount = replacement->surfaceCount;
			replacementTriangleCount = replacement->triangleCount;
		}
	}
	if ((flags & nri_scene::MaterialFlag_Flat) != 0 &&
		(flags & (nri_scene::MaterialFlag_Sprite | nri_scene::MaterialFlag_Mirror | nri_scene::MaterialFlag_Sky | nri_scene::MaterialFlag_Portal)) == 0 &&
		result.provenance.sectorIndex >= 0)
	{
		flatPlaneVisibilityRelevant = true;
		flatPlaneVisible = IsFlatPlaneMarkedVisible(mCurrentVisibleFlatPlaneWords, result.provenance.sectorIndex, result.normal[1] < 0.0f);
	}
	const std::string chunkReasons = GetRuntimeMapMutationReasonSummary(chunkReasonMask);
	FString textureName;
	int32_t legacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(result.textureId, textureName, legacyTile);
	FString materialTextureName;
	int32_t materialLegacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(result.baseTextureId, materialTextureName, materialLegacyTile);
	Printf("NRI PT surface probe: hit source=%s drawlist=%s owner=%s data_source=%s chunk=%d gate_visible=%s flat_drawlist_visible=%s static_resident=%s static_tlas_instanced=%s static_probe_included=%s chunk_replaced=%s chunk_reasons=%s section_dirty=%u sector_dirty=%s dragged=%s blind_spot=%s replacement_surfaces=%u replacement_tris=%u local_space=%d portal_graph=%d sector=%d wall=%d nextsector=%d actor=%d cstat=0x%x primitive=%u material=%u cpu_tex=%u metadata_tex=%u gpu_tex=%u expected_gpu_tex=%u cpu_palette=%u metadata_palette=%u gpu_palette=%u gpu_material_valid=%s cpu_metadata_match=%s gpu_palette_match=%s gpu_texture_match=%s stable_texture_slots=%s texture_slot_revision=%llu material_generation=%llu material_buffer_hash=%llu shader_valid=%s shader_hit_match=%s shader_frame=%llu shader_source=%u shader_instance=%u shader_primitive=%u shader_material=%u shader_tex=%u shader_palette=%u shader_flags=0x%x shader_lighting=0x%x texid=%u legacy_tile=%d texture_name=%s material_texid=%u material_legacy_tile=%d material_texture_name=%s distance=%.2f pos=(%.2f, %.2f, %.2f) normal=(%.3f, %.3f, %.3f) flags=0x%x indexed=%s fullbright=%s flat=%s sprite=%s mirror=%s sky=%s portal=%s facing_billboard=%s point_sampled=%s tex_fullbright=%s glowing=%s auto_glow=%s glowmap=%s normalmap=%s metallic=%s roughness=%s normal_tex=%u metallic_tex=%u roughness_tex=%u metalness_hint=%.3f roughness_hint=%.3f material_class=%u emissive_mode=%s emissive_tex=%u light_surface=%s light_mat=%u emissive_surface=%s emissive_prims=%u emissive_hit=%s emissive_flags=0x%x emissive_rule=%u emissive_override=%u emissive_sector=%d sector_scale=%.3f sector_reach=%.3f sector_applied=%s emissive_area=%.2f emissive_power=%.3f emissive_sample_weight=%.3f emissive_pdf=%.6f emissive_intensity=%.3f material_response=%s material_scale=%.3f light=%.3f alpha=%.3f avg=(%.2f, %.2f, %.2f) emissive=(%.2f, %.2f, %.2f) glow=(%.2f, %.2f, %.2f)\n",
		nri_diag::GetSurfaceSourceTypeName(result.provenance.sourceType),
		nri_diag::GetDrawListTypeName(result.provenance.drawListType),
		nri_diag::GetSurfaceProbeSceneOwnerName(result.sceneOwner),
		nri_diag::GetSceneDataSourceName(result.sceneDataSource),
		result.provenance.mapChunkIndex,
		YesNo(chunkVisibleGate),
		flatPlaneVisibilityRelevant ? YesNo(flatPlaneVisible) : "n/a",
		YesNo(chunkResidentStatic),
		YesNo(chunkStaticTlasInstanced),
		YesNo(chunkStaticProbeIncluded),
		YesNo(chunkReplaced),
		chunkReasons.c_str(),
		chunkSectionDirtyCount,
		YesNo(chunkSectorDirty),
		YesNo(chunkDragged),
		YesNo(chunkBlindSpot),
		replacementSurfaceCount,
		replacementTriangleCount,
		localSpaceIndex,
		portalGraphIndex,
		result.provenance.sectorIndex,
		result.provenance.wallIndex,
		result.provenance.nextSectorIndex,
		result.provenance.actorIndex,
		result.provenance.cstat,
		result.primitiveIndex,
		result.materialIndex,
		result.semanticTextureIndex != UINT32_MAX ? result.semanticTextureIndex : 0u,
		result.metadataTextureIndex != UINT32_MAX ? result.metadataTextureIndex : 0u,
		result.gpuTextureIndex != UINT32_MAX ? result.gpuTextureIndex : 0u,
		result.expectedGpuTextureIndex != UINT32_MAX ? result.expectedGpuTextureIndex : 0u,
		result.semanticPaletteIndex != UINT32_MAX ? result.semanticPaletteIndex : 0u,
		result.metadataPaletteIndex != UINT32_MAX ? result.metadataPaletteIndex : 0u,
		result.gpuPaletteIndex != UINT32_MAX ? result.gpuPaletteIndex : 0u,
		YesNo(result.gpuMaterialValid),
		YesNo(result.semanticMetadataMatch),
		YesNo(result.gpuPaletteMatch),
		result.expectedGpuTextureValid ? YesNo(result.gpuTextureMatch) : "n/a",
		YesNo(result.gpuMaterialsUseStableTextureSlots),
		(unsigned long long)result.textureSlotRevision,
		(unsigned long long)result.materialGeneration,
		(unsigned long long)result.materialBufferPayloadHash,
		YesNo(result.shaderMaterialValid),
		YesNo(result.shaderHitMatch),
		(unsigned long long)result.shaderFrameNumber,
		result.shaderDataSource != UINT32_MAX ? result.shaderDataSource : 0u,
		result.shaderInstanceIndex != UINT32_MAX ? result.shaderInstanceIndex : 0u,
		result.shaderPrimitiveIndex != UINT32_MAX ? result.shaderPrimitiveIndex : 0u,
		result.shaderMaterialIndex != UINT32_MAX ? result.shaderMaterialIndex : 0u,
		result.shaderTextureIndex != UINT32_MAX ? result.shaderTextureIndex : 0u,
		result.shaderPaletteIndex != UINT32_MAX ? result.shaderPaletteIndex : 0u,
		result.shaderMaterialFlags,
		result.shaderLightingFlags,
		result.textureId,
		legacyTile,
		textureName.GetChars(),
		result.baseTextureId,
		materialLegacyTile,
		materialTextureName.GetChars(),
		result.distance,
		result.position[0], result.position[1], result.position[2],
		result.normal[0], result.normal[1], result.normal[2],
		flags,
		YesNo((flags & nri_scene::MaterialFlag_Indexed) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Fullbright) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Flat) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sprite) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Mirror) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Sky) != 0),
		YesNo((flags & nri_scene::MaterialFlag_Portal) != 0),
		YesNo((flags & nri_scene::MaterialFlag_FacingBillboard) != 0),
		YesNo((flags & nri_scene::MaterialFlag_PointSampled) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureFullbright) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_TextureAutoGlowing) != 0),
		YesNo((lightingFlags & nri_scene::MaterialLightingFlag_HasGlowmap) != 0),
		YesNo(result.normalTextureIndex != UINT32_MAX),
		YesNo(result.metallicTextureIndex != UINT32_MAX),
		YesNo(result.roughnessTextureIndex != UINT32_MAX),
		result.normalTextureIndex != UINT32_MAX ? result.normalTextureIndex : 0u,
		result.metallicTextureIndex != UINT32_MAX ? result.metallicTextureIndex : 0u,
		result.roughnessTextureIndex != UINT32_MAX ? result.roughnessTextureIndex : 0u,
		result.metalnessHint,
		result.roughnessHint,
		result.materialClass,
		nri_diag::GetMaterialEmissiveModeName(result.emissiveMode),
		result.emissiveTextureIndex != UINT32_MAX ? result.emissiveTextureIndex : 0u,
		YesNo(emissiveDiagnostics.sceneLightSurfaceMatch),
		emissiveDiagnostics.sceneLightMaterialIndex != UINT32_MAX ? emissiveDiagnostics.sceneLightMaterialIndex : 0u,
		YesNo(emissiveDiagnostics.activeEmissiveSurfaceMatch),
		emissiveDiagnostics.emissivePrimitiveMatchCount,
		YesNo(emissiveDiagnostics.exactEmissivePrimitiveMatch),
		emissiveDiagnostics.emissiveSourceFlags,
		emissiveDiagnostics.emissiveSourceRuleId,
		emissiveDiagnostics.emissiveOverrideRuleId,
		emissiveDiagnostics.emissiveSectorIndex,
		emissiveDiagnostics.sectorResponseScale,
		emissiveDiagnostics.sectorReachScale,
		YesNo(emissiveDiagnostics.sectorResponseApplied),
		emissiveDiagnostics.emissivePrimitiveArea,
		emissiveDiagnostics.emissivePowerEstimate,
		emissiveDiagnostics.emissiveSelectionWeight,
		emissiveDiagnostics.emissiveSelectionPdf,
		emissiveDiagnostics.emissiveIntensity,
		YesNo(emissiveDiagnostics.materialResponseEnabled),
		emissiveDiagnostics.materialResponseScale,
		result.lightLevel,
		result.alpha,
		result.averageColor[0], result.averageColor[1], result.averageColor[2],
		result.emissiveColor[0], result.emissiveColor[1], result.emissiveColor[2],
		result.glowColor[0], result.glowColor[1], result.glowColor[2]);
	mSurfaceProbe.SetLastLogged(result);
}

NRISurfaceProbeEmissiveDiagnostics NRIRenderer::BuildSurfaceProbeEmissiveDiagnostics(const NRISurfaceProbeResult& probe) const
{
	NRISurfaceProbeEmissiveDiagnostics diagnostics = {};
	if (!probe.valid || !probe.hit)
	{
		return diagnostics;
	}

	const SceneLightRecordSource expectedSource = MapSurfaceProbeOwnerToLightSource(probe.sceneOwner);
	const SceneLightSystem::SurfaceRecord* matchedSurface = nullptr;
	for (const auto& record : mSceneLights.GetSurfaceRecords())
	{
		if (!SurfaceProvenanceMatches(record.provenance, probe.provenance))
		{
			continue;
		}
		if (record.material.textureId != probe.textureId)
		{
			continue;
		}
		if (record.source == expectedSource)
		{
			matchedSurface = &record;
			break;
		}
		if (matchedSurface == nullptr)
		{
			matchedSurface = &record;
		}
	}

	if (matchedSurface == nullptr)
	{
		return diagnostics;
	}

	diagnostics.sceneLightSurfaceMatch = true;
	diagnostics.sceneLightMaterialIndex = matchedSurface->materialIndex;

	for (const auto& surface : mSceneLights.GetEmissiveSurfaces().activeSurfaces)
	{
		if (surface.source == matchedSurface->source &&
			surface.materialIndex == matchedSurface->materialIndex &&
			surface.textureId == matchedSurface->material.textureId &&
			surface.actorIndex == matchedSurface->provenance.actorIndex)
		{
			diagnostics.activeEmissiveSurfaceMatch = true;
			break;
		}
	}

	const uint32_t expectedDataSource =
		matchedSurface->source == SceneLightRecordSource::StaticMapScene ?
			nri_diag::SceneDataSourceStatic :
			nri_diag::SceneDataSourceDynamic;
	for (const auto& record : mBoundEmissivePrimitiveRecords)
	{
		if (record.dataSource == expectedDataSource &&
			record.materialIndex == matchedSurface->materialIndex &&
			record.textureId == matchedSurface->material.textureId &&
			record.actorIndex == matchedSurface->provenance.actorIndex)
		{
			diagnostics.emissivePrimitiveMatchCount++;
		}
		if (record.dataSource == probe.sceneDataSource &&
			record.primitiveIndex == probe.primitiveIndex)
		{
			diagnostics.exactEmissivePrimitiveMatch = true;
			diagnostics.emissiveSourceFlags = record.sourceFlags;
			diagnostics.emissiveSourceRuleId = record.sourceRuleId;
			diagnostics.emissiveOverrideRuleId = record.overrideRuleId;
			diagnostics.emissiveSectorIndex = record.sectorIndex;
			diagnostics.emissivePrimitiveArea = record.primitiveArea;
			diagnostics.emissivePowerEstimate = record.powerEstimate;
			diagnostics.emissiveSelectionWeight = record.selectionWeight;
			diagnostics.emissiveSelectionPdf = record.selectionPdf;
			diagnostics.emissiveIntensity = record.emissiveIntensity;
			diagnostics.sectorResponseScale = record.sectorResponseScale;
			diagnostics.sectorReachScale = record.sectorReachScale;
			diagnostics.sectorResponseApplied = record.sectorResponseApplied;
			diagnostics.materialResponseEnabled = record.materialResponseEnabled;
			diagnostics.materialResponseScale = record.materialResponseScale;
		}
	}

	return diagnostics;
}

bool NRIRenderer::BuildEmissiveLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const
{
	outTarget = {};
	const NRISurfaceProbeResult& lastProbe = mSurfaceProbe.Last();
	if (!lastProbe.valid)
	{
		outTarget.failureReason = "no sampled center hit has been recorded yet";
		return false;
	}

	if (!lastProbe.hit)
	{
		outTarget.valid = true;
		outTarget.failureReason = "last sampled center ray missed translated PT geometry";
		return false;
	}

	const NRISurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(lastProbe);
	outTarget.valid = true;
	outTarget.hit = true;
	outTarget.emissive = emissiveDiagnostics.activeEmissiveSurfaceMatch;
	outTarget.sectorIndex = lastProbe.provenance.sectorIndex;
	outTarget.wallIndex = lastProbe.provenance.wallIndex;
	outTarget.textureId = (int)lastProbe.textureId;
	outTarget.baseTextureId = (int)lastProbe.baseTextureId;
	outTarget.materialIndex = (int)lastProbe.materialIndex;
	outTarget.surfaceLightOverlay = lastProbe.provenance.sourceType == nri_scene::SurfaceSourceType::SurfaceLightOverlay;
	outTarget.surfaceLightRuleId = outTarget.surfaceLightOverlay ? lastProbe.provenance.cstat : 0u;
	outTarget.position[0] = lastProbe.position[0];
	outTarget.position[1] = lastProbe.position[1];
	outTarget.position[2] = lastProbe.position[2];
	outTarget.normal[0] = lastProbe.normal[0];
	outTarget.normal[1] = lastProbe.normal[1];
	outTarget.normal[2] = lastProbe.normal[2];
	int32_t legacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(lastProbe.textureId, outTarget.textureName, legacyTile);
	ResolveSurfaceProbeTextureDebugInfo(lastProbe.baseTextureId, outTarget.materialTextureName, legacyTile);
	outTarget.sectorResponseIntensity = std::max(0.0f, (float)nri_ptsectoremissionsignalstrength);
	outTarget.sectorResponseMin = std::max(0.0f, (float)nri_ptsectoremissionresponsemin);
	outTarget.sectorResponseMax = std::max(outTarget.sectorResponseMin, (float)nri_ptsectoremissionresponsemax);
	if (!outTarget.emissive)
	{
		outTarget.failureReason = emissiveDiagnostics.sceneLightSurfaceMatch ?
			"aimed surface is not currently an active emissive surface" :
			"aimed surface is not present in the scene-light surface registry";
		return false;
	}

	return true;
}

bool NRIRenderer::BuildSurfaceLightEditTarget(PathTracingEmissiveLightEditTarget& outTarget) const
{
	outTarget = {};
	const NRISurfaceProbeResult& lastProbe = mSurfaceProbe.Last();
	if (!lastProbe.valid)
	{
		outTarget.failureReason = "no sampled center hit has been recorded yet";
		return false;
	}

	if (!lastProbe.hit)
	{
		outTarget.valid = true;
		outTarget.failureReason = "last sampled center ray missed translated PT geometry";
		return false;
	}

	const NRISurfaceProbeEmissiveDiagnostics emissiveDiagnostics = BuildSurfaceProbeEmissiveDiagnostics(lastProbe);
	outTarget.valid = true;
	outTarget.hit = true;
	outTarget.emissive = emissiveDiagnostics.activeEmissiveSurfaceMatch;
	outTarget.sectorIndex = lastProbe.provenance.sectorIndex;
	outTarget.wallIndex = lastProbe.provenance.wallIndex;
	outTarget.textureId = (int)lastProbe.textureId;
	outTarget.baseTextureId = (int)lastProbe.baseTextureId;
	outTarget.materialIndex = (int)lastProbe.materialIndex;
	outTarget.position[0] = lastProbe.position[0];
	outTarget.position[1] = lastProbe.position[1];
	outTarget.position[2] = lastProbe.position[2];
	outTarget.normal[0] = lastProbe.normal[0];
	outTarget.normal[1] = lastProbe.normal[1];
	outTarget.normal[2] = lastProbe.normal[2];
	float viewDirection[3] = { mCurrentCameraForward[0], mCurrentCameraForward[1], mCurrentCameraForward[2] };
	Normalize3(viewDirection);
	if (outTarget.normal[0] * viewDirection[0] + outTarget.normal[1] * viewDirection[1] + outTarget.normal[2] * viewDirection[2] > 0.0f)
	{
		outTarget.normal[0] = -outTarget.normal[0];
		outTarget.normal[1] = -outTarget.normal[1];
		outTarget.normal[2] = -outTarget.normal[2];
	}
	int32_t legacyTile = -1;
	ResolveSurfaceProbeTextureDebugInfo(lastProbe.textureId, outTarget.textureName, legacyTile);
	ResolveSurfaceProbeTextureDebugInfo(lastProbe.baseTextureId, outTarget.materialTextureName, legacyTile);
	outTarget.sectorResponseIntensity = std::max(0.0f, (float)nri_ptsectoremissionsignalstrength);
	outTarget.sectorResponseMin = std::max(0.0f, (float)nri_ptsectoremissionresponsemin);
	outTarget.sectorResponseMax = std::max(outTarget.sectorResponseMin, (float)nri_ptsectoremissionresponsemax);
	return true;
}
