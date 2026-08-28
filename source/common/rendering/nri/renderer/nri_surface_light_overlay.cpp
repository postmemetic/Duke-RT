#include "nri_surface_light_overlay.h"

#include "nri_cvars.h"
#include "nri_renderer.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_scene_bridge.h"
#include "../scene/nri_texture_signature.h"
#include "lightoverlay.h"
#include "texinfo.h"
#include "texturemanager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace
{
	static uint64_t HashLightOverlayText(uint64_t hash, const char* text)
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

	static uint32_t BuildResolvedLightOverlayRuleId(const char* id, const char* classOrMapName, const LightOverlaySourceLocation& source)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashLightOverlayText(hash, id);
		hash = HashLightOverlayText(hash, classOrMapName);
		hash = HashLightOverlayText(hash, source.sourceName.GetChars());
		hash ^= (uint64_t)source.orderIndex + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		const uint32_t ruleId = (uint32_t)(hash ^ (hash >> 32));
		return ruleId != 0 ? ruleId : 1u;
	}

	static uint32_t BuildSurfaceLightRuleId(const ResolvedLightOverlaySurfaceLightRule& rule)
	{
		return BuildResolvedLightOverlayRuleId(rule.id.GetChars(), rule.mapName.GetChars(), rule.source);
	}

	static FGameTexture* ResolveSurfaceLightFixtureTexture(const ResolvedLightOverlaySurfaceLightRule& rule)
	{
		if (!rule.hasFixtureTexture || rule.fixtureTexture.IsEmpty())
		{
			return nullptr;
		}

		const FTextureID textureId = TexMan.CheckForTexture(
			rule.fixtureTexture.GetChars(),
			ETextureType::Any,
			FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ForceLookup);
		return textureId.isValid() ? TexMan.GetGameTexture(textureId, true) : nullptr;
	}

	static uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static bool AppendPersistentImageTextureDependency(uint64_t& key, FTexture* texture)
	{
		key = nri_scene::HashCombine64(key, (uint64_t)(uintptr_t)texture);
		if (texture == nullptr)
		{
			return true;
		}

		nri_scene::TextureSignature signature = {};
		nri_scene::TextureSignatureRequest request = {};
		if (!nri_scene::TryBuildImageTextureSignature(texture, request, signature) ||
			!signature.valid || !signature.persistentEligible || signature.key == 0)
		{
			return false;
		}

		key = nri_scene::HashCombine64(key, signature.key);
		key = nri_scene::HashCombine64(
			key,
			((uint64_t)signature.width << 32) | (uint64_t)signature.height);
		return true;
	}

	static bool AppendPersistentFixtureMaterialDependency(uint64_t& key, FGameTexture* texture)
	{
		key = nri_scene::HashCombine64(key, (uint64_t)(uintptr_t)texture);
		if (texture == nullptr)
		{
			return true;
		}

		nri_scene::TextureSignature signature = {};
		nri_scene::TextureSignatureRequest request = {};
		if (!nri_scene::TryBuildTextureSignature(texture, request, signature) ||
			!signature.valid || !signature.persistentEligible || signature.key == 0)
		{
			return false;
		}

		// BuildMaterials consumes both the resolved image and wrapper-level material
		// properties. Pointer identity protects retained raw texture references across
		// wrapper/resource replacement; persistent signatures reject canvases and other
		// mutable image sources.
		key = nri_scene::HashCombine64(key, (uint64_t)(uintptr_t)texture->GetTexture());
		key = nri_scene::HashCombine64(key, (uint64_t)(uint32_t)texture->GetID().GetIndex());
		key = nri_scene::HashCombine64(key, signature.key);
		key = nri_scene::HashCombine64(
			key,
			((uint64_t)signature.width << 32) | (uint64_t)signature.height);

		float glowColor[3] = {};
		if (texture->isGlowing())
		{
			texture->GetGlowColor(glowColor);
		}
		key = nri_scene::HashCombine64(key, texture->isFullbright() ? 1ull : 0ull);
		key = nri_scene::HashCombine64(key, texture->isGlowing() ? 1ull : 0ull);
		key = nri_scene::HashCombine64(key, texture->isAutoGlowing() ? 1ull : 0ull);
		key = nri_scene::HashCombine64(key, FloatBits(glowColor[0]));
		key = nri_scene::HashCombine64(key, FloatBits(glowColor[1]));
		key = nri_scene::HashCombine64(key, FloatBits(glowColor[2]));

		return AppendPersistentImageTextureDependency(key, texture->GetGlowmap()) &&
			AppendPersistentImageTextureDependency(key, texture->GetNormalmap()) &&
			AppendPersistentImageTextureDependency(key, texture->GetMetallic()) &&
			AppendPersistentImageTextureDependency(key, texture->GetRoughness());
	}

	static bool TryBuildSurfaceLightOverlayKey(const ResolvedLightOverlaySet& resolved, uint64_t& outKey)
	{
		outKey = 0;
		if (resolved.resolvedGeneration == 0)
		{
			return false;
		}

		uint64_t key = 1469598103934665603ull;
		key = nri_scene::HashCombine64(key, resolved.resolvedGeneration);
		key = nri_scene::HashCombine64(key, (uint64_t)resolved.surfaceLightRules.Size());
		key = HashLightOverlayText(key, resolved.activeMapName.GetChars());
		key = nri_scene::HashCombine64(key, FloatBits((float)nri_ptsurfacelightminbrightness));
		key = nri_scene::HashCombine64(key, FloatBits((float)nri_ptfullbrightboost));

		for (const auto& rule : resolved.surfaceLightRules)
		{
			FGameTexture* texture = ResolveSurfaceLightFixtureTexture(rule);
			if (texture == nullptr)
			{
				key = nri_scene::HashCombine64(key, 0ull);
				continue;
			}

			if (!AppendPersistentFixtureMaterialDependency(key, texture))
			{
				return false;
			}
		}

		outKey = key != 0 ? key : 1ull;
		return true;
	}

	template<typename T>
	static bool EqualPodVectors(const std::vector<T>& left, const std::vector<T>& right)
	{
		static_assert(std::is_trivially_copyable<T>::value, "validation comparison requires POD payloads");
		return left.size() == right.size() &&
			(left.empty() || std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0);
	}

	static bool EqualSurfaceLightProducts(
		const nri_scene::SceneView& leftSceneView,
		const nri_scene::GeometryData& leftGeometry,
		const nri_scene::MaterialBridgeData& leftMaterials,
		const nri_scene::SceneView& rightSceneView,
		const nri_scene::GeometryData& rightGeometry,
		const nri_scene::MaterialBridgeData& rightMaterials)
	{
		auto equalMaterialRef = [](const nri_scene::MaterialRef& left, const nri_scene::MaterialRef& right)
		{
			return left.texture == right.texture &&
				left.emissiveSourceTexture == right.emissiveSourceTexture &&
				left.authoredTextureId == right.authoredTextureId &&
				left.palette == right.palette && left.shade == right.shade &&
				left.alpha == right.alpha && left.flags == right.flags;
		};
		auto equalProvenance = [](const nri_scene::SurfaceProvenance& left, const nri_scene::SurfaceProvenance& right)
		{
			if (left.sourceType != right.sourceType || left.sectorIndex != right.sectorIndex ||
				left.wallIndex != right.wallIndex || left.sectionIndex != right.sectionIndex ||
				left.mapChunkIndex != right.mapChunkIndex || left.nextSectorIndex != right.nextSectorIndex ||
				left.actorIndex != right.actorIndex || left.drawListType != right.drawListType ||
				left.cstat != right.cstat || left.materialFlags != right.materialFlags ||
				left.actorOverlayRuleCount != right.actorOverlayRuleCount)
			{
				return false;
			}
			for (uint32_t i = 0; i < nri_scene::MaxActorOverlayRuleIdsPerSurface; ++i)
			{
				if (left.actorOverlayRuleIds[i] != right.actorOverlayRuleIds[i])
				{
					return false;
				}
			}
			return true;
		};
		auto equalSurfaces = [&](const std::vector<nri_scene::SurfaceRef>& left, const std::vector<nri_scene::SurfaceRef>& right)
		{
			if (left.size() != right.size())
			{
				return false;
			}
			for (size_t i = 0; i < left.size(); ++i)
			{
				if (!EqualPodVectors(left[i].vertices, right[i].vertices) ||
					left[i].indices != right[i].indices ||
					!equalMaterialRef(left[i].material, right[i].material) ||
					!equalProvenance(left[i].provenance, right[i].provenance))
				{
					return false;
				}
			}
			return true;
		};
		const auto& leftStats = leftSceneView.stats;
		const auto& rightStats = rightSceneView.stats;
		const bool sceneStatsEqual =
			leftStats.totalDrawItems == rightStats.totalDrawItems &&
			leftStats.wallDrawItems == rightStats.wallDrawItems &&
			leftStats.flatDrawItems == rightStats.flatDrawItems &&
			leftStats.spriteDrawItems == rightStats.spriteDrawItems &&
			leftStats.triangleEstimate == rightStats.triangleEstimate &&
			leftStats.materialRefs == rightStats.materialRefs;
		if (leftSceneView.drawInfo != rightSceneView.drawInfo ||
			leftSceneView.primitiveFlags != rightSceneView.primitiveFlags ||
			!sceneStatsEqual ||
			!equalSurfaces(leftSceneView.opaqueWalls, rightSceneView.opaqueWalls) ||
			!equalSurfaces(leftSceneView.opaqueFlats, rightSceneView.opaqueFlats) ||
			!equalSurfaces(leftSceneView.opaqueSprites, rightSceneView.opaqueSprites) ||
			!EqualPodVectors(leftGeometry.vertices, rightGeometry.vertices) ||
			!EqualPodVectors(leftGeometry.indices, rightGeometry.indices) ||
			!EqualPodVectors(leftGeometry.primitives, rightGeometry.primitives) ||
			!EqualPodVectors(leftGeometry.primitiveProvenance, rightGeometry.primitiveProvenance) ||
			!EqualPodVectors(leftMaterials.materials, rightMaterials.materials) ||
			!EqualPodVectors(leftMaterials.lightMetadata, rightMaterials.lightMetadata) ||
			leftMaterials.textures.size() != rightMaterials.textures.size() ||
			leftMaterials.paletteWidth != rightMaterials.paletteWidth ||
			leftMaterials.paletteHeight != rightMaterials.paletteHeight ||
			leftMaterials.paletteLookup != rightMaterials.paletteLookup)
		{
			return false;
		}

		for (size_t i = 0; i < leftMaterials.textures.size(); ++i)
		{
			const auto& left = leftMaterials.textures[i];
			const auto& right = rightMaterials.textures[i];
			if (left.key != right.key || left.width != right.width || left.height != right.height ||
				left.indexed != right.indexed || left.sourceTexture != right.sourceTexture ||
				left.pixels != right.pixels)
			{
				return false;
			}
		}
		return true;
	}

	static void RebuildSurfaceLightSceneViewStats(nri_scene::SceneView& sceneView)
	{
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
		sceneView.stats = stats;
	}
}

bool NRIRenderer::BuildSurfaceLightOverlay(
	nri_scene::SceneView& outSceneView,
	nri_scene::GeometryData& outGeometry,
	nri_scene::MaterialBridgeData& outMaterials,
	bool allowReuse,
	bool validateReuse)
{
	NRISurfaceLightOverlayCacheStats stats = {};
	const bool built = mSurfaceLightOverlayCache.Build(
		GetResolvedLightOverlaySet(),
		allowReuse,
		validateReuse,
		outSceneView,
		outGeometry,
		outMaterials,
		stats);
	mLastPerfShellTraceStats.sceneReuseSurfaceLightCalled = 1u;
	mLastPerfShellTraceStats.sceneReuseSurfaceLightHit = stats.hit ? 1u : 0u;
	mLastPerfShellTraceStats.sceneReuseSurfaceLightCandidateHit = stats.candidateHit ? 1u : 0u;
	mLastPerfShellTraceStats.sceneReuseSurfaceLightBuild = stats.built ? 1u : 0u;
	mLastPerfShellTraceStats.sceneReuseSurfaceLightReject = stats.rejected ? 1u : 0u;
	mLastPerfShellTraceStats.sceneReuseSurfaceLightValidationChecked = stats.validationChecked ? 1u : 0u;
	mLastPerfShellTraceStats.sceneReuseSurfaceLightValidationMismatch = stats.validationMismatch ? 1u : 0u;
	mLastPerfShellTraceStats.sceneReuseSurfaceLightKey = stats.key;
	return built;
}

bool NRISurfaceLightOverlayCache::Build(
	const ResolvedLightOverlaySet& resolved,
	bool allowReuse,
	bool validateReuse,
	nri_scene::SceneView& outSceneView,
	nri_scene::GeometryData& outGeometry,
	nri_scene::MaterialBridgeData& outMaterials,
	NRISurfaceLightOverlayCacheStats& outStats)
{
	outStats = {};
	uint64_t key = 0;
	const bool cacheable = TryBuildSurfaceLightOverlayKey(resolved, key);
	outStats.key = key;
	const bool candidateHit = allowReuse && cacheable && mValid && mKey == key;
	outStats.candidateHit = candidateHit;
	if (candidateHit && !validateReuse)
	{
		outSceneView = mSceneView;
		outGeometry = mGeometry;
		outMaterials = mMaterials;
		outStats.hit = true;
		return mBuilt;
	}

	nri_scene::SceneView rebuiltSceneView;
	nri_scene::GeometryData rebuiltGeometry;
	nri_scene::MaterialBridgeData rebuiltMaterials;
	const bool built = nri_surface_light_overlay::BuildSurfaceLightOverlay(
		resolved,
		rebuiltSceneView,
		rebuiltGeometry,
		rebuiltMaterials);
	outStats.built = true;
	outStats.rejected = allowReuse && !cacheable;
	if (candidateHit && validateReuse)
	{
		outStats.validationChecked = true;
		if (mBuilt != built || !EqualSurfaceLightProducts(
			mSceneView,
			mGeometry,
			mMaterials,
			rebuiltSceneView,
			rebuiltGeometry,
			rebuiltMaterials))
		{
			outStats.validationMismatch = true;
			mValid = false;
		}
	}

	outSceneView = rebuiltSceneView;
	outGeometry = rebuiltGeometry;
	outMaterials = rebuiltMaterials;
	if (cacheable)
	{
		mSceneView = std::move(rebuiltSceneView);
		mGeometry = std::move(rebuiltGeometry);
		mMaterials = std::move(rebuiltMaterials);
		mKey = key;
		mValid = true;
		mBuilt = built;
	}
	else
	{
		Reset();
	}
	return built;
}

void NRISurfaceLightOverlayCache::Reset()
{
	mSceneView = {};
	mGeometry = {};
	mMaterials = {};
	mKey = 0;
	mValid = false;
	mBuilt = false;
}

namespace nri_surface_light_overlay
{
bool BuildSurfaceLightOverlay(
	const ResolvedLightOverlaySet& resolved,
	nri_scene::SceneView& outSceneView,
	nri_scene::GeometryData& outGeometry,
	nri_scene::MaterialBridgeData& outMaterials)
{
	outSceneView = {};
	outGeometry = {};
	outMaterials = {};

	if (resolved.surfaceLightRules.Size() == 0)
	{
		return false;
	}

	auto normalize3 = [](float vector[3]) -> bool
	{
		const float lengthSq = vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2];
		if (!std::isfinite(lengthSq) || lengthSq <= 0.000001f)
		{
			return false;
		}
		const float invLength = 1.0f / std::sqrt(lengthSq);
		vector[0] *= invLength;
		vector[1] *= invLength;
		vector[2] *= invLength;
		return true;
	};

	auto cross3 = [](const float a[3], const float b[3], float out[3])
	{
		out[0] = a[1] * b[2] - a[2] * b[1];
		out[1] = a[2] * b[0] - a[0] * b[2];
		out[2] = a[0] * b[1] - a[1] * b[0];
	};

	std::vector<std::array<float, 3>> surfaceLightColors;
	std::vector<float> surfaceLightIntensities;
	std::vector<bool> surfaceLightTintEmission;
	for (const auto& rule : resolved.surfaceLightRules)
	{
		if (!rule.hasPosition || !rule.hasNormal)
		{
			continue;
		}

		float normal[3] = { rule.normal[0], rule.normal[1], rule.normal[2] };
		if (!normalize3(normal))
		{
			continue;
		}

		const float worldUp[3] = { 0.0f, 1.0f, 0.0f };
		const float worldRight[3] = { 1.0f, 0.0f, 0.0f };
		float tangent[3] = {};
		cross3(worldUp, normal, tangent);
		if (!normalize3(tangent))
		{
			cross3(worldRight, normal, tangent);
			if (!normalize3(tangent))
			{
				continue;
			}
		}

		float bitangent[3] = {};
		cross3(normal, tangent, bitangent);
		if (!normalize3(bitangent))
		{
			continue;
		}
		if (rule.hasRotation && std::isfinite(rule.rotation) && rule.rotation != 0.0f)
		{
			const float radians = rule.rotation * 0.017453292519943295f;
			const float c = std::cos(radians);
			const float s = std::sin(radians);
			const float rotatedTangent[3] =
			{
				tangent[0] * c + bitangent[0] * s,
				tangent[1] * c + bitangent[1] * s,
				tangent[2] * c + bitangent[2] * s,
			};
			const float rotatedBitangent[3] =
			{
				bitangent[0] * c - tangent[0] * s,
				bitangent[1] * c - tangent[1] * s,
				bitangent[2] * c - tangent[2] * s,
			};
			tangent[0] = rotatedTangent[0];
			tangent[1] = rotatedTangent[1];
			tangent[2] = rotatedTangent[2];
			bitangent[0] = rotatedBitangent[0];
			bitangent[1] = rotatedBitangent[1];
			bitangent[2] = rotatedBitangent[2];
		}

		const float offset = rule.hasOffset ? std::max(0.0f, rule.offset) : 0.5f;
		const float halfWidth = std::max(1.0f, rule.hasSize ? rule.size[0] : 32.0f) * 0.5f;
		const float halfHeight = std::max(1.0f, rule.hasSize ? rule.size[1] : 32.0f) * 0.5f;
		const float lightColor[3] =
		{
			rule.hasColor ? std::max(rule.color[0], 0.0f) : 1.0f,
			rule.hasColor ? std::max(rule.color[1], 0.0f) : 1.0f,
			rule.hasColor ? std::max(rule.color[2], 0.0f) : 1.0f,
		};
		const float lightIntensity = std::max(std::max(rule.intensity, 0.0f), (float)nri_ptsurfacelightminbrightness);
		const float center[3] =
		{
			rule.position[0] + normal[0] * offset,
			rule.position[1] + normal[1] * offset,
			rule.position[2] + normal[2] * offset,
		};

		auto makeVertex = [&](float tangentScale, float bitangentScale, float u, float v) -> nri_scene::CapturedVertex
		{
			nri_scene::CapturedVertex vertex = {};
			vertex.position[0] = center[0] + tangent[0] * tangentScale + bitangent[0] * bitangentScale;
			vertex.position[1] = center[1] + tangent[1] * tangentScale + bitangent[1] * bitangentScale;
			vertex.position[2] = center[2] + tangent[2] * tangentScale + bitangent[2] * bitangentScale;
			vertex.prevPosition[0] = vertex.position[0];
			vertex.prevPosition[1] = vertex.position[1];
			vertex.prevPosition[2] = vertex.position[2];
			vertex.uv[0] = u;
			vertex.uv[1] = v;
			return vertex;
		};

		nri_scene::SurfaceRef surface = {};
		surface.vertices.reserve(6u);
		const nri_scene::CapturedVertex v00 = makeVertex(-halfWidth, -halfHeight, 0.0f, 1.0f);
		const nri_scene::CapturedVertex v10 = makeVertex(halfWidth, -halfHeight, 1.0f, 1.0f);
		const nri_scene::CapturedVertex v11 = makeVertex(halfWidth, halfHeight, 1.0f, 0.0f);
		const nri_scene::CapturedVertex v01 = makeVertex(-halfWidth, halfHeight, 0.0f, 0.0f);
		surface.vertices.push_back(v00);
		surface.vertices.push_back(v10);
		surface.vertices.push_back(v11);
		surface.vertices.push_back(v00);
		surface.vertices.push_back(v11);
		surface.vertices.push_back(v01);
		surface.material.texture = ResolveSurfaceLightFixtureTexture(rule);
		surface.material.emissiveSourceTexture = surface.material.texture;
		surface.material.palette = 0;
		surface.material.shade = 0;
		surface.material.alpha = 1.0f;
		surface.material.flags = nri_scene::MaterialFlag_Fullbright | nri_scene::MaterialFlag_Flat;
		if (rule.fixtureMaterialResponse)
		{
			surface.material.flags |= nri_scene::MaterialFlag_TintEmission;
		}
		surface.provenance.sourceType = nri_scene::SurfaceSourceType::SurfaceLightOverlay;
		surface.provenance.sectorIndex = rule.hasSector ? rule.sector : -1;
		surface.provenance.wallIndex = rule.hasWall ? rule.wall : -1;
		surface.provenance.mapChunkIndex = -1;
		surface.provenance.cstat = BuildSurfaceLightRuleId(rule);
		outSceneView.opaqueFlats.push_back(std::move(surface));
		surfaceLightColors.push_back({ lightColor[0], lightColor[1], lightColor[2] });
		surfaceLightIntensities.push_back(lightIntensity);
		surfaceLightTintEmission.push_back(rule.fixtureMaterialResponse);
	}

	if (outSceneView.opaqueFlats.empty())
	{
		return false;
	}

	RebuildSurfaceLightSceneViewStats(outSceneView);
	nri_scene::BuildGeometry(outSceneView, outGeometry);
	nri_scene::BuildMaterials(outSceneView, outMaterials);
	for (size_t i = 0; i < outMaterials.materials.size(); ++i)
	{
		nri_scene::MaterialData& material = outMaterials.materials[i];
		const bool tintEmission = i < surfaceLightTintEmission.size() ? surfaceLightTintEmission[i] : false;
		material.flags |= nri_scene::MaterialFlag_Fullbright | nri_scene::MaterialFlag_Flat;
		if (tintEmission)
		{
			material.flags |= nri_scene::MaterialFlag_TintEmission;
		}
		else
		{
			material.flags &= ~nri_scene::MaterialFlag_TintEmission;
		}
		material.lightingFlags |=
			nri_scene::MaterialLightingFlag_MaterialFullbright |
			nri_scene::MaterialLightingFlag_NoShadowReceive |
			nri_scene::MaterialLightingFlag_NoShadowCast;
		material.lightLevel = 1.0f;
		material.emissiveMode = nri_scene::MaterialEmissiveMode_UseBaseTexture;
		material.emissiveTextureIndex = material.textureIndex;
		material.emissiveIntensity = i < surfaceLightIntensities.size() ? surfaceLightIntensities[i] : 1.0f;
		material.emissiveMaskScale = 1.0f;
		const std::array<float, 3> lightColor = i < surfaceLightColors.size() ? surfaceLightColors[i] : std::array<float, 3>{ 1.0f, 1.0f, 1.0f };
		material.emissiveColor[0] = lightColor[0];
		material.emissiveColor[1] = lightColor[1];
		material.emissiveColor[2] = lightColor[2];
		if (i < outMaterials.lightMetadata.size())
		{
			nri_scene::MaterialLightingMetadata& metadata = outMaterials.lightMetadata[i];
			metadata.materialFlags = material.flags;
			metadata.lightingFlags |=
				nri_scene::MaterialLightingFlag_MaterialFullbright |
				nri_scene::MaterialLightingFlag_NoShadowReceive |
				nri_scene::MaterialLightingFlag_NoShadowCast;
			metadata.lightLevel = 1.0f;
			metadata.emissiveMode = material.emissiveMode;
			metadata.emissiveTextureIndex = material.emissiveTextureIndex;
			metadata.emissiveIntensity = material.emissiveIntensity;
			metadata.emissiveMaskScale = material.emissiveMaskScale;
			metadata.emissiveColor[0] = material.emissiveColor[0];
			metadata.emissiveColor[1] = material.emissiveColor[1];
			metadata.emissiveColor[2] = material.emissiveColor[2];
		}
	}

	return !outGeometry.primitives.empty() && !outMaterials.materials.empty();
}
}
