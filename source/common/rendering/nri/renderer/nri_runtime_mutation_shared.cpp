#include "nri_runtime_mutation_shared.h"

#include "../scene/nri_hash.h"
#include "hw_sections.h"
#include "texturemanager.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace nri_runtime_mutation
{
	uint64_t RuntimeMutationHashCombine64(uint64_t hash, uint64_t value)
	{
		return nri_scene::HashCombine64(hash, value);
	}

	uint32_t RuntimeMutationFloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
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

	void NudgeCapturedSurface(nri_scene::SurfaceRef& surface, float depthNudge)
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

	void NudgeBlindSpotReplacementFlats(nri_scene::SceneView& sceneView)
	{
		static constexpr float kBlindSpotFlatDepthNudge = 0.01f;
		for (nri_scene::SurfaceRef& surface : sceneView.opaqueFlats)
		{
			if (surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapFloorSection &&
				surface.provenance.sourceType != nri_scene::SurfaceSourceType::MapCeilingSection)
			{
				continue;
			}

			NudgeCapturedSurface(surface, kBlindSpotFlatDepthNudge);
		}
	}

	uint32_t CountSceneViewSurfaces(const nri_scene::SceneView& sceneView)
	{
		return (uint32_t)(sceneView.opaqueWalls.size() + sceneView.opaqueFlats.size() + sceneView.opaqueSprites.size());
	}

	uint64_t HashMaterialBridgeSummary(const nri_scene::MaterialBridgeData& materials)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)materials.materials.size());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)materials.lightMetadata.size());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)materials.textures.size());
		for (const auto& material : materials.materials)
		{
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.textureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.paletteIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.flags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.lightingFlags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.emissiveMode);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.emissiveTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.alpha));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.fluidBindingIndex);
		}

		for (const auto& metadata : materials.lightMetadata)
		{
			hash = RuntimeMutationHashCombine64(hash, metadata.materialKey);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.textureId);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.actorIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.textureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.paletteIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.emissiveMode);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.emissiveTextureIndex);
		}

		for (const auto& texture : materials.textures)
		{
			hash = RuntimeMutationHashCombine64(hash, texture.key);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)texture.width);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)texture.height);
			hash = RuntimeMutationHashCombine64(hash, texture.indexed ? 1ull : 0ull);
		}

		return hash;
	}

	uint64_t HashResidentMaterialPayload(const nri_scene::MaterialBridgeData& materials)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)materials.materials.size());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)materials.lightMetadata.size());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)materials.textures.size());
		for (const auto& material : materials.materials)
		{
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.textureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.paletteIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.flags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.materialClass);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.lightingFlags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.normalTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.metallicTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.roughnessTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.sectorIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.emissiveTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.lightLevel));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.alpha));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.roughnessHint));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.metalnessHint));
			for (float color : material.emissiveColor)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(color));
			}
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.emissiveIntensity));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.emissiveMaskScale));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.emissiveMode);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(material.emissiveReserved));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)material.fluidBindingIndex);
		}

		for (const auto& metadata : materials.lightMetadata)
		{
			hash = RuntimeMutationHashCombine64(hash, metadata.materialKey);
			hash = RuntimeMutationHashCombine64(hash, metadata.textureContentKey);
			hash = RuntimeMutationHashCombine64(hash, metadata.glowmapContentKey);
			hash = RuntimeMutationHashCombine64(hash, metadata.normalContentKey);
			hash = RuntimeMutationHashCombine64(hash, metadata.metallicContentKey);
			hash = RuntimeMutationHashCombine64(hash, metadata.roughnessContentKey);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.textureId);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.textureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.glowmapTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.normalTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.metallicTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.roughnessTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.emissiveTextureIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.paletteIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.materialFlags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.lightingFlags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.materialClass);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)metadata.emissiveMode);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)metadata.sourceType);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)metadata.sectorIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)metadata.actorIndex);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)metadata.shade);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(metadata.alpha));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(metadata.lightLevel));
			for (float color : metadata.averageColor)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(color));
			}
			for (float color : metadata.glowColor)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(color));
			}
			for (float color : metadata.emissiveColor)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(color));
			}
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(metadata.emissiveIntensity));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(metadata.emissiveMaskScale));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(metadata.visibleFullbrightBoost));
		}

		for (const auto& texture : materials.textures)
		{
			hash = RuntimeMutationHashCombine64(hash, texture.key);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)texture.width);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)texture.height);
			hash = RuntimeMutationHashCombine64(hash, texture.indexed ? 1ull : 0ull);
		}

		return hash != 0 ? hash : 1;
	}

	void FilterMaterialOnlyReplacementSceneView(nri_scene::SceneView& sceneView, uint32_t reasonMask)
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

	bool SceneViewHasSectorDrivenWallBands(const nri_scene::SceneView& sceneView)
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

	static void RemapMaterialBridgeAgainstTextureTable(
		const nri_scene::MaterialBridgeData& source,
		nri_scene::MaterialBridgeData& inOutTextureTable,
		nri_scene::MaterialBridgeData& outRemapped)
	{
		std::unordered_map<uint64_t, uint32_t> textureLookup;
		textureLookup.reserve(inOutTextureTable.textures.size() + source.textures.size());
		for (uint32_t i = 0; i < (uint32_t)inOutTextureTable.textures.size(); ++i)
		{
			textureLookup.emplace(inOutTextureTable.textures[i].key, i);
		}

		auto remapTextureIndex = [&source, &inOutTextureTable, &textureLookup](uint32_t textureIndex) -> uint32_t
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

	bool TryBuildMergedSectorMaterialOnlyBridge(
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
		RemapMaterialBridgeAgainstTextureTable(filteredLiveMaterials, outMergedMaterials, remappedFlatMaterials);
		if (remappedFlatMaterials.materials.size() != residentFlatCount ||
			remappedFlatMaterials.lightMetadata.size() != residentFlatCount)
		{
			return false;
		}

		std::copy_n(remappedFlatMaterials.materials.data(), residentFlatCount, outMergedMaterials.materials.begin() + residentWallCount);
		std::copy_n(remappedFlatMaterials.lightMetadata.data(), residentFlatCount, outMergedMaterials.lightMetadata.begin() + residentWallCount);
		return true;
	}

	bool TryBuildMergedSectorMaterialOnlySceneView(
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

	bool IsChunkMarkedVisible(const std::vector<uint32_t>& visibleChunkWords, uint32_t chunkIndex)
	{
		const size_t wordIndex = (size_t)(chunkIndex >> 5u);
		if (wordIndex >= visibleChunkWords.size())
		{
			return false;
		}

		return (visibleChunkWords[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
	}

	uint64_t ComputeRecurringChunkStateSignature(
		uint32_t reasonMask,
		uint32_t liveWallCount,
		uint32_t liveFlatCount,
		uint32_t liveTriangleCount,
		uint32_t liveMaterialCount)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)reasonMask);
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)liveWallCount);
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)liveFlatCount);
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)liveTriangleCount);
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)liveMaterialCount);
		return hash;
	}

	static uint32_t GetAnimatedTextureId(FGameTexture* texture)
	{
		return texture != nullptr ? (uint32_t)texture->GetID().GetIndex() : 0u;
	}

	static uint64_t HashAnimatedLayerTexture(FTexture* texture)
	{
		return texture != nullptr ? (uint64_t)(uintptr_t)texture : 0ull;
	}

	static uint64_t HashAnimatedTextureBindingSignature(FGameTexture* texture)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)GetAnimatedTextureId(texture));
		if (texture == nullptr)
		{
			return hash;
		}

		hash = RuntimeMutationHashCombine64(hash, HashAnimatedLayerTexture(texture->GetGlowmap()));
		hash = RuntimeMutationHashCombine64(hash, HashAnimatedLayerTexture(texture->GetNormalmap()));
		hash = RuntimeMutationHashCombine64(hash, HashAnimatedLayerTexture(texture->GetMetallic()));
		hash = RuntimeMutationHashCombine64(hash, HashAnimatedLayerTexture(texture->GetRoughness()));
		return hash;
	}

	static uint64_t HashAnimatedTextureDisplaySignature(FGameTexture* texture)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)GetAnimatedTextureId(texture));
		if (texture == nullptr)
		{
			return hash;
		}

		hash = RuntimeMutationHashCombine64(hash, (uint64_t)texture->GetDisplayWidth());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)texture->GetDisplayHeight());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)texture->GetDisplayLeftOffset());
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)texture->GetDisplayTopOffset());
		return hash;
	}

	template <typename SurfaceContainer>
	static void HashAnimatedSurfaces(const SurfaceContainer& surfaces, uint64_t& hash, bool includeDisplaySignature,
		FGameTexture* const* bindings = nullptr)
	{
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)surfaces.size());
		size_t bindingIndex = 0;
		for (const auto& surface : surfaces)
		{
			FGameTexture* texture = bindings != nullptr ? bindings[bindingIndex++] : surface.material.texture;
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)surface.provenance.sourceType);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.sectorIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.wallIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.sectionIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.actorIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)surface.provenance.cstat);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)surface.material.flags);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.material.authoredTextureId + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)surface.material.palette);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)surface.material.shade);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(surface.material.alpha));
			hash = RuntimeMutationHashCombine64(hash, HashAnimatedTextureBindingSignature(texture));
			if (includeDisplaySignature)
			{
				hash = RuntimeMutationHashCombine64(hash, HashAnimatedTextureDisplaySignature(texture));
			}
		}
	}

	template <typename SurfaceContainer>
	static void HashAnimatedGeometrySurfaces(const SurfaceContainer& surfaces, uint64_t& hash)
	{
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)surfaces.size());
		for (const auto& surface : surfaces)
		{
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)surface.provenance.sourceType);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.sectorIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.wallIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.sectionIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.actorIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)surface.provenance.cstat);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)surface.indices.size());
			hash = RuntimeMutationHashCombine64(hash, HashAnimatedTextureDisplaySignature(surface.material.texture));
		}
	}

	template <typename SurfaceContainer>
	static void HashExactGeometrySurfaces(const SurfaceContainer& surfaces, uint64_t& hash)
	{
		hash = RuntimeMutationHashCombine64(hash, (uint64_t)surfaces.size());
		for (const auto& surface : surfaces)
		{
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)surface.provenance.sourceType);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.sectorIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.wallIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.sectionIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)(uint32_t)(surface.provenance.actorIndex + 1));
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)surface.provenance.cstat);
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)surface.vertices.size());
			hash = RuntimeMutationHashCombine64(hash, (uint64_t)surface.indices.size());
			for (const auto& vertex : surface.vertices)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(vertex.position[0]));
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(vertex.position[1]));
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)RuntimeMutationFloatBits(vertex.position[2]));
			}
			for (uint32_t index : surface.indices)
			{
				hash = RuntimeMutationHashCombine64(hash, (uint64_t)index);
			}
		}
	}

	uint64_t ComputeAnimatedMaterialSignature(const nri_scene::SceneView& sceneView)
	{
		uint64_t hash = 1469598103934665603ull;
		HashAnimatedSurfaces(sceneView.opaqueWalls, hash, false);
		HashAnimatedSurfaces(sceneView.opaqueFlats, hash, false);
		HashAnimatedSurfaces(sceneView.opaqueSprites, hash, false);
		return hash;
	}

	bool ComputeAnimatedMaterialSignatureWithBindings(
		const nri_scene::SceneView& sceneView,
		const std::vector<FGameTexture*>& bindings,
		uint64_t& outSignature)
	{
		const size_t wallCount = sceneView.opaqueWalls.size();
		const size_t flatCount = sceneView.opaqueFlats.size();
		if (bindings.size() != wallCount + flatCount + sceneView.opaqueSprites.size()) return false;
		uint64_t hash = 1469598103934665603ull;
		FGameTexture* const* data = bindings.empty() ? nullptr : bindings.data();
		HashAnimatedSurfaces(sceneView.opaqueWalls, hash, false, data);
		HashAnimatedSurfaces(sceneView.opaqueFlats, hash, false, data != nullptr ? data + wallCount : nullptr);
		HashAnimatedSurfaces(sceneView.opaqueSprites, hash, false, data != nullptr ? data + wallCount + flatCount : nullptr);
		outSignature = hash;
		return true;
	}

	uint64_t ComputeAnimatedGeometrySignature(const nri_scene::SceneView& sceneView)
	{
		uint64_t hash = 1469598103934665603ull;
		HashAnimatedGeometrySurfaces(sceneView.opaqueWalls, hash);
		HashAnimatedGeometrySurfaces(sceneView.opaqueFlats, hash);
		HashAnimatedGeometrySurfaces(sceneView.opaqueSprites, hash);
		return hash;
	}

	uint64_t ComputeExactGeometrySignature(const nri_scene::SceneView& sceneView)
	{
		uint64_t hash = 1469598103934665603ull;
		HashExactGeometrySurfaces(sceneView.opaqueWalls, hash);
		HashExactGeometrySurfaces(sceneView.opaqueFlats, hash);
		HashExactGeometrySurfaces(sceneView.opaqueSprites, hash);
		return hash;
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

	bool SceneViewUsesHardwareCanvasTexture(const nri_scene::SceneView& sceneView)
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

	bool ChunkHasUnresolvedAuthoredTextures(const nri_scene::PTMapChunk& chunk)
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

}
