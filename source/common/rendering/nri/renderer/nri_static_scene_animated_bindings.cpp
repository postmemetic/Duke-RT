#include "nri_static_scene_animated_bindings.h"
#include "../scene/nri_scene_bridge.h"
#include "texturemanager.h"
#include "texinfo.h"

namespace nri_static_scene
{
	static FTextureID ResolveAuthoredTextureId(const nri_scene::SurfaceRef& surface)
	{
		const int32_t authoredTextureId = surface.material.authoredTextureId;
		if (authoredTextureId >= 0) return FSetTextureID(authoredTextureId);
		return FNullTextureID();
	}

	static bool IsAnimatedSurfaceCandidate(const nri_scene::SurfaceRef& surface)
	{
		// Match map-view construction: only nonnull sky carriers belong solely
		// to the scene environment. An unresolved sky row may remain opaque.
		if ((surface.material.flags & nri_scene::MaterialFlag_Sky) != 0 && surface.material.texture != nullptr)
			return false;
		const FTextureID textureId = ResolveAuthoredTextureId(surface);
		return textureId.isValid() && GetExtInfo(textureId).picanm.type() != 0;
	}

	bool ChunkHasAnimatedSurfaceCandidates(const nri_scene::SceneView& chunkView)
	{
		for (const auto& surface : chunkView.opaqueWalls)
			if (IsAnimatedSurfaceCandidate(surface)) return true;
		for (const auto& surface : chunkView.opaqueFlats)
			if (IsAnimatedSurfaceCandidate(surface)) return true;
		return false;
	}

	bool ResolveAnimatedBindings(const nri_scene::SceneView& chunkView, std::vector<FGameTexture*>& outBindings)
	{
		outBindings.clear();
		outBindings.reserve(chunkView.opaqueWalls.size() + chunkView.opaqueFlats.size() + chunkView.opaqueSprites.size());
		const auto append = [&](const nri_scene::SurfaceRef& surface)
		{
			outBindings.push_back(IsAnimatedSurfaceCandidate(surface) ?
				TexMan.GetGameTexture(ResolveAuthoredTextureId(surface), true) : surface.material.texture);
		};
		for (const auto& surface : chunkView.opaqueWalls) append(surface);
		for (const auto& surface : chunkView.opaqueFlats) append(surface);
		// Static map animation has never owned sprite bindings. Preserve their
		// suffix because the material-signature consumer uses all three lists.
		for (const auto& surface : chunkView.opaqueSprites) outBindings.push_back(surface.material.texture);
		return true;
	}

	bool RefreshAnimatedBindings(nri_scene::SceneView& chunkView)
	{
		const auto refresh = [](nri_scene::SurfaceRef& surface)
		{
			if (!IsAnimatedSurfaceCandidate(surface)) return;
			surface.material.texture = TexMan.GetGameTexture(ResolveAuthoredTextureId(surface), true);
		};
		for (auto& surface : chunkView.opaqueWalls) refresh(surface);
		for (auto& surface : chunkView.opaqueFlats) refresh(surface);
		return true;
	}
}
