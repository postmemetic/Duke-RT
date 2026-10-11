#include "nri_static_scene_animated_bindings.h"
#include "../scene/nri_map_builder.h"
#include "../scene/nri_map_world.h"
#include "texturemanager.h"
#include "texinfo.h"

#include <algorithm>

namespace nri_static_scene
{
	static FTextureID ResolveAuthoredTextureIdForStaticMapSurface(const nri_scene::PTMapSurface& surface)
	{
		const int32_t authoredTextureId = surface.surface.material.authoredTextureId;
		if (authoredTextureId >= 0)
		{
			return FSetTextureID(authoredTextureId);
		}
		return FNullTextureID();
	}

	static bool IsAnimatedStaticMapSurfaceCandidate(const nri_scene::PTMapSurface& surface)
	{
		const FTextureID textureId = ResolveAuthoredTextureIdForStaticMapSurface(surface);
		return textureId.isValid() && GetExtInfo(textureId).picanm.type() != 0;
	}

	bool ChunkHasAnimatedSurfaceCandidates(const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk)
	{
		const uint32_t endSurface = std::min<uint32_t>(chunk.firstSurface + chunk.surfaceCount, (uint32_t)mapWorld.surfaces.size());
		for (uint32_t surfaceIndex = chunk.firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
		{
			if (IsAnimatedStaticMapSurfaceCandidate(mapWorld.surfaces[surfaceIndex]))
			{
				return true;
			}
		}

		return false;
	}

	bool ResolveAnimatedBindings(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk,
		const nri_scene::SceneView& chunkView,
		std::vector<FGameTexture*>& outBindings)
	{
		outBindings.clear();
		outBindings.reserve(chunkView.opaqueWalls.size() + chunkView.opaqueFlats.size() + chunkView.opaqueSprites.size());
		for (const auto& surface : chunkView.opaqueWalls) outBindings.push_back(surface.material.texture);
		for (const auto& surface : chunkView.opaqueFlats) outBindings.push_back(surface.material.texture);
		for (const auto& surface : chunkView.opaqueSprites) outBindings.push_back(surface.material.texture);
		if (chunk.firstSurface > mapWorld.surfaces.size() || chunk.surfaceCount > mapWorld.surfaces.size() - chunk.firstSurface) return false;
		size_t wallIndex = 0;
		size_t flatIndex = 0;
		for (uint32_t index = chunk.firstSurface; index < chunk.firstSurface + chunk.surfaceCount; ++index)
		{
			const auto& source = mapWorld.surfaces[index];
			if ((source.surface.material.flags & nri_scene::MaterialFlag_Sky) != 0 && source.surface.material.texture != nullptr) continue;
			size_t target = 0;
			if (source.kind == nri_scene::PTMapSurfaceKind::Floor || source.kind == nri_scene::PTMapSurfaceKind::Ceiling)
			{
				if (flatIndex >= chunkView.opaqueFlats.size()) return false;
				target = chunkView.opaqueWalls.size() + flatIndex++;
			}
			else
			{
				if (wallIndex >= chunkView.opaqueWalls.size()) return false;
				target = wallIndex++;
			}
			if (!IsAnimatedStaticMapSurfaceCandidate(source)) continue;
			const FTextureID authored = ResolveAuthoredTextureIdForStaticMapSurface(source);
			outBindings[target] = authored.isValid() ? TexMan.GetGameTexture(authored, true) : nullptr;
		}
		return wallIndex == chunkView.opaqueWalls.size() && flatIndex == chunkView.opaqueFlats.size();
	}

	bool RefreshAnimatedBindings(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk,
		nri_scene::SceneView& ioChunkView)
	{
		uint32_t wallSurfaceIndex = 0;
		uint32_t flatSurfaceIndex = 0;
		const uint32_t endSurface = std::min<uint32_t>(chunk.firstSurface + chunk.surfaceCount, (uint32_t)mapWorld.surfaces.size());
		for (uint32_t surfaceIndex = chunk.firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
		{
			const auto& mapSurface = mapWorld.surfaces[surfaceIndex];
			if ((mapSurface.surface.material.flags & nri_scene::MaterialFlag_Sky) != 0 && mapSurface.surface.material.texture != nullptr)
			{
				continue;
			}

			nri_scene::SurfaceRef* targetSurface = nullptr;
			switch (mapSurface.kind)
			{
			case nri_scene::PTMapSurfaceKind::Floor:
			case nri_scene::PTMapSurfaceKind::Ceiling:
				if (flatSurfaceIndex >= ioChunkView.opaqueFlats.size())
				{
					return false;
				}
				targetSurface = &ioChunkView.opaqueFlats[flatSurfaceIndex++];
				break;
			default:
				if (wallSurfaceIndex >= ioChunkView.opaqueWalls.size())
				{
					return false;
				}
				targetSurface = &ioChunkView.opaqueWalls[wallSurfaceIndex++];
				break;
			}

			if (!IsAnimatedStaticMapSurfaceCandidate(mapSurface))
			{
				continue;
			}

			const FTextureID textureId = ResolveAuthoredTextureIdForStaticMapSurface(mapSurface);
			FGameTexture* liveTexture = textureId.isValid() ? TexMan.GetGameTexture(textureId, true) : nullptr;
			targetSurface->material.texture = liveTexture;
		}

		return wallSurfaceIndex == ioChunkView.opaqueWalls.size() && flatSurfaceIndex == ioChunkView.opaqueFlats.size();
	}
}
