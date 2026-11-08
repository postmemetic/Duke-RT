#pragma once

#include <vector>

struct tspritetype;
class HWWall;
struct walltype;

namespace nri_scene
{
// Find the nearest backing walls, including contiguous coplanar receivers
// reached through ordinary sector seams inside the projected artwork.
void FindWallDecalReceivers(const tspritetype& sprite, const HWWall& quad,
	std::vector<walltype*>& receivers);
}
