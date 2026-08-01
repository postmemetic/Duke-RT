#include "voxel_surface_emission.h"

namespace
{
	bool CountsUnitFacesInsteadOfColorSpans()
	{
		return
			CountVoxelUnitSurfaceFaces(1u, 5u) == 5u &&
			CountVoxelUnitSurfaceFaces(1u | 2u | 16u | 32u, 5u) == 12u &&
			CountVoxelUnitSurfaceFaces(1u | 4u | 8u, 3u) == 9u;
	}

	bool CountsCapsOnlyOncePerSlab()
	{
		return
			CountVoxelUnitSurfaceFaces(16u, 7u) == 1u &&
			CountVoxelUnitSurfaceFaces(32u, 7u) == 1u &&
			CountVoxelUnitSurfaceFaces(16u | 32u, 1u) == 2u;
	}

	bool HandlesEmptyExposure()
	{
		return CountVoxelUnitSurfaceFaces(0u, 12u) == 0u;
	}
}

int main()
{
	return CountsUnitFacesInsteadOfColorSpans() && CountsCapsOnlyOncePerSlab() && HandlesEmptyExposure() ? 0 : 1;
}
