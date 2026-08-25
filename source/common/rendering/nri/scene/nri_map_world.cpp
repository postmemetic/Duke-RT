#include "nri_map_world.h"
#include "nri_map_motion_correspondence.h"

#include <algorithm>

namespace nri_scene
{
namespace
{
	PTSkySourceType GetSkySourceType(const SurfaceProvenance& provenance)
	{
		switch (provenance.sourceType)
		{
		case SurfaceSourceType::MapPortalSurface:
			return PTSkySourceType::Portal;
		case SurfaceSourceType::MapFloorSection:
		case SurfaceSourceType::MapCeilingSection:
			return PTSkySourceType::Flat;
		default:
			return PTSkySourceType::Wall;
		}
	}

	void ApplyPreservedSky(SceneView& outView, const SceneView* preservedSkyView)
	{
		if (preservedSkyView == nullptr)
		{
			return;
		}

		outView.sky = preservedSkyView->sky;
		Copy3(preservedSkyView->skyColor, outView.skyColor);
		Copy3(preservedSkyView->groundColor, outView.groundColor);
	}

	void AppendSurfaceToSceneView(const PTMapSurface& surface, uint64_t mapEpoch, SceneView& outView, const SceneView* preservedSkyView)
	{
		SurfaceRef copy = surface.surface;
		InitializeMapTemporalSurface(surface, mapEpoch, copy);
		if ((copy.material.flags & MaterialFlag_Sky) != 0 && copy.material.texture != nullptr)
		{
			if (preservedSkyView == nullptr)
			{
				UpdateSceneSky(outView, copy.material.texture, 0, GetSkySourceType(copy.provenance));
			}
			// Map-world sky carriers should feed the scene-level environment only.
			// Keeping them out of the opaque lists preserves the "sky via miss"
			// contract used by the dynamic scene-capture path.
			return;
		}

		switch (surface.kind)
		{
		case PTMapSurfaceKind::Floor:
		case PTMapSurfaceKind::Ceiling:
			outView.opaqueFlats.push_back(std::move(copy));
			break;
		default:
			outView.opaqueWalls.push_back(std::move(copy));
			break;
		}
	}
}

void PTMapWorld::Reset()
{
	level = nullptr;
	buildSerial = 0;
	topologyRevision = 0;
	valid = false;
	chunks.clear();
	sectorChunkLookup.clear();
	surfaces.clear();
	localSpaces.clear();
	portals.clear();
	portalTargets.clear();
	stats = {};
}

SceneDebugStats CollectMapWorldDebugStats(const PTMapWorld& mapWorld)
{
	SceneDebugStats stats = {};
	stats.wallDrawItems = mapWorld.stats.wallSurfaceCount;
	stats.flatDrawItems = mapWorld.stats.flatSurfaceCount;
	stats.spriteDrawItems = 0;
	stats.translucentDrawItems = 0;
	stats.triangleEstimate = mapWorld.stats.triangleCount;
	stats.materialRefs = mapWorld.stats.surfaceCount;
	stats.skySurfaces = mapWorld.stats.skySurfaceCount;
	stats.portalViews = 0;
	stats.portalCapturesSkipped = 0;

	for (const PTMapSurface& surface : mapWorld.surfaces)
	{
		if ((surface.surface.material.flags & MaterialFlag_Mirror) != 0)
		{
			stats.mirrorSurfaces++;
		}
	}

	return stats;
}

void BuildMapSceneView(const PTMapWorld& mapWorld, SceneView& outView, const SceneView* preservedSkyView)
{
	outView = {};
	outView.stats = CollectMapWorldDebugStats(mapWorld);
	outView.opaqueWalls.reserve(mapWorld.stats.wallSurfaceCount);
	outView.opaqueFlats.reserve(mapWorld.stats.flatSurfaceCount);

	for (const PTMapSurface& surface : mapWorld.surfaces)
	{
		AppendSurfaceToSceneView(surface, mapWorld.buildSerial, outView, preservedSkyView);
	}

	ApplyPreservedSky(outView, preservedSkyView);
}

void BuildMapChunkSceneView(const PTMapWorld& mapWorld, const PTMapChunk& chunk, SceneView& outView, const SceneView* preservedSkyView)
{
	outView = {};
	outView.drawInfo = nullptr;
	outView.stats = CollectMapWorldDebugStats(mapWorld);
	outView.opaqueWalls.reserve(chunk.surfaceCount);
	outView.opaqueFlats.reserve(chunk.surfaceCount);

	const uint32_t endSurface = std::min<uint32_t>(chunk.firstSurface + chunk.surfaceCount, (uint32_t)mapWorld.surfaces.size());
	for (uint32_t surfaceIndex = chunk.firstSurface; surfaceIndex < endSurface; ++surfaceIndex)
	{
		AppendSurfaceToSceneView(mapWorld.surfaces[surfaceIndex], mapWorld.buildSerial, outView, preservedSkyView);
	}

	ApplyPreservedSky(outView, preservedSkyView);
}

int32_t FindMapWorldLocalSpaceIndex(const PTMapWorld& mapWorld, uint32_t chunkIndex)
{
	if (chunkIndex >= mapWorld.chunks.size())
	{
		return -1;
	}

	const uint32_t localSpaceIndex = mapWorld.chunks[chunkIndex].localSpaceIndex;
	return localSpaceIndex < mapWorld.localSpaces.size() ? (int32_t)localSpaceIndex : -1;
}

int32_t FindMapWorldPortalIndex(const PTMapWorld& mapWorld, const SurfaceProvenance& provenance)
{
	const int sourcePlane =
		provenance.sourceType == SurfaceSourceType::MapFloorSection ? 0 :
		provenance.sourceType == SurfaceSourceType::MapCeilingSection ? 1 :
		-1;

	for (const PTMapPortal& portal : mapWorld.portals)
	{
		if (portal.sourceSurfaceIndex != UINT32_MAX && portal.sourceSurfaceIndex < mapWorld.surfaces.size())
		{
			const SurfaceProvenance& portalProvenance = mapWorld.surfaces[portal.sourceSurfaceIndex].surface.provenance;
			if (portalProvenance.sourceType == provenance.sourceType &&
				portalProvenance.sectorIndex == provenance.sectorIndex &&
				portalProvenance.wallIndex == provenance.wallIndex &&
				portalProvenance.sectionIndex == provenance.sectionIndex)
			{
				return (int32_t)portal.portalIndex;
			}
		}

		if (portal.sourceWallIndex >= 0 && portal.sourceWallIndex == provenance.wallIndex)
		{
			return (int32_t)portal.portalIndex;
		}

		if (sourcePlane >= 0 &&
			portal.sourcePlane == sourcePlane &&
			portal.sourceSectorIndex == provenance.sectorIndex)
		{
			return (int32_t)portal.portalIndex;
		}
	}

	return -1;
}
}
