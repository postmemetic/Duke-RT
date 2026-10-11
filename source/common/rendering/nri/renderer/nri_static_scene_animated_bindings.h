#pragma once

#include <vector>

class FGameTexture;
namespace nri_scene { struct PTMapWorld; struct PTMapChunk; struct SceneView; }

namespace nri_static_scene
{
bool ChunkHasAnimatedSurfaceCandidates(const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk);
bool ResolveAnimatedBindings(const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk,
	const nri_scene::SceneView& chunkView, std::vector<FGameTexture*>& outBindings);
// Independent legacy refresh retained for opt-out and shadow validation.
bool RefreshAnimatedBindings(const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk,
	nri_scene::SceneView& chunkView);
}
