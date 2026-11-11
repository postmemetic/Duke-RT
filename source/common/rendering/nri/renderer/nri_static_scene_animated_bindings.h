#pragma once

#include <vector>

class FGameTexture;
namespace nri_scene { struct SceneView; }

namespace nri_static_scene
{
// Current retained surfaces own animation identity. Runtime topology can reveal,
// replace or reorder bands that were absent from the initial map-world snapshot.
bool ChunkHasAnimatedSurfaceCandidates(const nri_scene::SceneView& chunkView);
bool ResolveAnimatedBindings(const nri_scene::SceneView& chunkView, std::vector<FGameTexture*>& outBindings);
// Independent scalar refresh for opt-out and shadow validation. Sprites and
// scene-level sky carriers retain their existing bindings in both paths.
bool RefreshAnimatedBindings(nri_scene::SceneView& chunkView);
}
