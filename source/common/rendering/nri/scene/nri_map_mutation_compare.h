#pragma once
#include "nri_map_builder.h"
#include <cstring>

// Exact semantic fields; never compare C++ struct padding.
namespace nri_scene::mutation_compare
{
template<class T> bool SameBits(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

inline bool SameBaseline(const nri_scene::PTMapChunkMutationBaseline& a, const nri_scene::PTMapChunkMutationBaseline& b)
{
#define MUTATION_FIELD(name) if (!SameBits(a.name, b.name)) return false
	MUTATION_FIELD(sectorIndex); MUTATION_FIELD(signature); MUTATION_FIELD(fluidNormalSignature);
	MUTATION_FIELD(floorz); MUTATION_FIELD(ceilingz); MUTATION_FIELD(floorstat); MUTATION_FIELD(ceilingstat);
	MUTATION_FIELD(floorheinum); MUTATION_FIELD(ceilingheinum); MUTATION_FIELD(portalflags);
	MUTATION_FIELD(floortexture); MUTATION_FIELD(ceilingtexture); MUTATION_FIELD(floorxpan); MUTATION_FIELD(floorypan);
	MUTATION_FIELD(ceilingxpan); MUTATION_FIELD(ceilingypan); MUTATION_FIELD(floorpal); MUTATION_FIELD(ceilingpal);
	MUTATION_FIELD(floorshade); MUTATION_FIELD(ceilingshade);
#undef MUTATION_FIELD
	if (a.sectionIndices != b.sectionIndices || a.walls.size() != b.walls.size()) return false;
	for (size_t i = 0; i < a.walls.size(); ++i)
	{
		const auto& x = a.walls[i]; const auto& y = b.walls[i];
#define MUTATION_FIELD(name) if (!SameBits(x.name, y.name)) return false
		MUTATION_FIELD(pos.X); MUTATION_FIELD(pos.Y); MUTATION_FIELD(point2); MUTATION_FIELD(nextwall); MUTATION_FIELD(nextsector);
		MUTATION_FIELD(cstat); MUTATION_FIELD(portalflags); MUTATION_FIELD(walltexture); MUTATION_FIELD(overtexture);
		MUTATION_FIELD(xpan); MUTATION_FIELD(ypan); MUTATION_FIELD(xrepeat); MUTATION_FIELD(yrepeat);
		MUTATION_FIELD(pal); MUTATION_FIELD(shade); MUTATION_FIELD(adjacentFloorz); MUTATION_FIELD(adjacentCeilingz);
		MUTATION_FIELD(adjacentFloorstat); MUTATION_FIELD(adjacentCeilingstat); MUTATION_FIELD(adjacentFloorheinum);
		MUTATION_FIELD(adjacentCeilingheinum); MUTATION_FIELD(adjacentPortalflags); MUTATION_FIELD(nextWallCstat);
		MUTATION_FIELD(nextWallTexture); MUTATION_FIELD(nextOverTexture); MUTATION_FIELD(nextWallPal); MUTATION_FIELD(nextWallShade);
#undef MUTATION_FIELD
	}
	return true;
}

inline bool SameAnalysis(const PTMapChunkMutationAnalysis& a, const PTMapChunkMutationAnalysis& b)
{
 return a.signature == b.signature && a.reasonMask == b.reasonMask &&
  a.sectionDirtyCount == b.sectionDirtyCount && a.sectorDirty == b.sectorDirty &&
  a.dragged == b.dragged && a.signatureChanged == b.signatureChanged;
}
}
