#include "nri_map_builder.h"

#include "build.h"
#include "gamefuncs.h"
#include "hw_portal.h"
#include "hw_sections.h"
#include "mapinfo.h"
#include "render.h"
#include "sectorgeometry.h"
#include "texturemanager.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

void initSkyInfo(HWDrawInfo* di, HWSkyInfo* sky, sectortype* sector, int plane);

namespace
{
	using namespace nri_scene;

	uint64_t gPendingLevelGeometryBuildSerial = 0;
	uint64_t gMapWorldTopologyRevision = 0;

	enum class PTWallBandKind : uint32_t
	{
		OneSided = 0,
		Upper,
		Middle,
		Lower,
		Portal,
	};

	struct PTWallTexCoord
	{
		float u = 0.0f;
		float v = 0.0f;
	};

	struct PTWallQuad
	{
		float x1 = 0.0f;
		float y1 = 0.0f;
		float x2 = 0.0f;
		float y2 = 0.0f;
		float fracLeft = 0.0f;
		float fracRight = 1.0f;
		float zTop[2] = {};
		float zBottom[2] = {};
		PTWallTexCoord texcoords[4] = {};
	};

	enum PTWallTexCoordIndex
	{
		PTWallTexCoord_LowerLeft = 0,
		PTWallTexCoord_UpperLeft,
		PTWallTexCoord_UpperRight,
		PTWallTexCoord_LowerRight,
	};

	struct PTWallBandDesc
	{
		PTMapSurfaceKind surfaceKind = PTMapSurfaceKind::WallOneSided;
		PTWallBandKind bandKind = PTWallBandKind::OneSided;
		SurfaceSourceType sourceType = SurfaceSourceType::MapWallBand;
		walltype* wall = nullptr;
		walltype* refWall = nullptr;
		sectortype* frontSector = nullptr;
		sectortype* backSector = nullptr;
		FGameTexture* texture = nullptr;
		int shade = 0;
		int palette = 0;
		float alpha = 1.0f;
		uint32_t materialFlags = MaterialFlag_None;
		float topLeft = 0.0f;
		float topRight = 0.0f;
		float bottomLeft = 0.0f;
		float bottomRight = 0.0f;
		float referenceHeight = 0.0f;
	};

	uint32_t CountTriangles(const SurfaceRef& surface)
	{
		if (!surface.indices.empty())
		{
			return (uint32_t)(surface.indices.size() / 3u);
		}
		return surface.vertices.size() >= 3 ? (uint32_t)surface.vertices.size() - 2 : 0;
	}

	uint64_t HashMix(uint64_t hash, uint64_t value)
	{
		hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		return hash;
	}

	uint64_t HashFloatBits(uint64_t hash, float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return HashMix(hash, bits);
	}

	uint64_t HashDoubleBits(uint64_t hash, double value)
	{
		uint64_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return HashMix(hash, bits);
	}

	void IncludeChunkBoundsPoint(PTMapChunk& chunk, const float position[3])
	{
		if (!std::isfinite(position[0]) ||
			!std::isfinite(position[1]) ||
			!std::isfinite(position[2]))
		{
			return;
		}

		if (!chunk.bounds.valid)
		{
			chunk.bounds.valid = true;
			for (int axis = 0; axis < 3; ++axis)
			{
				chunk.bounds.min[axis] = position[axis];
				chunk.bounds.max[axis] = position[axis];
			}
			return;
		}

		for (int axis = 0; axis < 3; ++axis)
		{
			chunk.bounds.min[axis] = std::min(chunk.bounds.min[axis], position[axis]);
			chunk.bounds.max[axis] = std::max(chunk.bounds.max[axis], position[axis]);
		}
	}

	void IncludeChunkBoundsSurface(PTMapChunk& chunk, const SurfaceRef& surface)
	{
		for (const CapturedVertex& vertex : surface.vertices)
		{
			IncludeChunkBoundsPoint(chunk, vertex.position);
		}
	}

	void FinalizeChunkBounds(PTMapChunk& chunk)
	{
		if (!chunk.bounds.valid)
		{
			chunk.bounds = {};
			return;
		}

		float radiusSquared = 0.0f;
		for (int axis = 0; axis < 3; ++axis)
		{
			chunk.bounds.center[axis] = (chunk.bounds.min[axis] + chunk.bounds.max[axis]) * 0.5f;
			const float extent = chunk.bounds.max[axis] - chunk.bounds.center[axis];
			radiusSquared += extent * extent;
		}
		chunk.bounds.radius = sqrtf(radiusSquared);
	}

	void AppendSurface(PTMapWorld& outWorld, PTMapChunk& chunk, PTMapSurface&& surface)
	{
		IncludeChunkBoundsSurface(chunk, surface.surface);
		chunk.triangleCount += CountTriangles(surface.surface);
		outWorld.surfaces.push_back(std::move(surface));
	}

	bool IsPortalWall(const walltype* wal)
	{
		if (wal == nullptr)
		{
			return false;
		}

		switch (wal->portalflags)
		{
		case PORTAL_WALL_MIRROR:
		case PORTAL_WALL_VIEW:
		case PORTAL_WALL_TO_SPRITE:
			return true;
		default:
			return false;
		}
	}

	bool ShouldTranslateWallMirrorAsMaterial(const walltype* wal)
	{
		return wal != nullptr &&
			wal->portalflags == PORTAL_WALL_MIRROR;
	}

	bool IsPortalPlane(const sectortype* sec, int plane)
	{
		if (sec == nullptr)
		{
			return false;
		}

		if (plane == 0)
		{
			return sec->portalflags == PORTAL_SECTOR_FLOOR || sec->portalflags == PORTAL_SECTOR_FLOOR_REFLECT;
		}

		return sec->portalflags == PORTAL_SECTOR_CEILING || sec->portalflags == PORTAL_SECTOR_CEILING_REFLECT;
	}

	bool IsSkyPlane(const sectortype* sec, int plane)
	{
		if (sec == nullptr)
		{
			return false;
		}

		return plane == 0 ? (sec->floorstat & CSTAT_SECTOR_SKY) != 0 : (sec->ceilingstat & CSTAT_SECTOR_SKY) != 0;
	}

	FGameTexture* ResolvePlaneTexture(sectortype* sec, int plane)
	{
		if (sec == nullptr)
		{
			return nullptr;
		}

		if (IsSkyPlane(sec, plane))
		{
			HWSkyInfo skyinfo = {};
			initSkyInfo(nullptr, &skyinfo, sec, plane);
			if (skyinfo.texture != nullptr && skyinfo.texture->isValid())
			{
				return skyinfo.texture;
			}
		}

		return TexMan.GetGameTexture(plane == 0 ? sec->floortexture : sec->ceilingtexture, true);
	}

	FGameTexture* ResolveMirrorWallTexture(const walltype* wal)
	{
		if (wal == nullptr)
		{
			return nullptr;
		}

		if (wal->overtexture.isValid())
		{
			FGameTexture* texture = TexMan.GetGameTexture(wal->overtexture, true);
			if (texture != nullptr && texture->isValid())
			{
				return texture;
			}
		}

		return TexMan.GetGameTexture(wal->walltexture, true);
	}

	FGameTexture* ResolvePlainMirrorMaterialTexture(const walltype* wal)
	{
		if (TexMan.mirrorTexture.isValid())
		{
			FGameTexture* texture = TexMan.GetGameTexture(TexMan.mirrorTexture, true);
			if (texture != nullptr && texture->isValid())
			{
				return texture;
			}
		}

		return ResolveMirrorWallTexture(wal);
	}

	uint32_t GetPlaneMaterialFlags(const sectortype* sec, int plane)
	{
		uint32_t materialFlags = MaterialFlag_Flat;
		if (sec == nullptr)
		{
			return materialFlags;
		}

		if (!IsPortalPlane(sec, plane))
		{
			return materialFlags;
		}

		const bool reflective =
			(plane == 0 && sec->portalflags == PORTAL_SECTOR_FLOOR_REFLECT) ||
			(plane != 0 && sec->portalflags == PORTAL_SECTOR_CEILING_REFLECT);
		materialFlags |= reflective ? MaterialFlag_Mirror : MaterialFlag_Portal;
		return materialFlags;
	}

	void FillPlaneProvenance(SurfaceProvenance& provenance, const sectortype* sec, int plane, int sectionIndex, int chunkIndex, uint32_t materialFlags)
	{
		provenance.sourceType = plane == 0 ? SurfaceSourceType::MapFloorSection : SurfaceSourceType::MapCeilingSection;
		provenance.sectorIndex = sec != nullptr ? sector.IndexOf(sec) : -1;
		provenance.sectionIndex = sectionIndex;
		provenance.mapChunkIndex = chunkIndex;
		provenance.drawListType = UINT32_MAX;
		provenance.materialFlags = materialFlags;
		if (sec != nullptr)
		{
			provenance.cstat = plane == 0 ? (uint32_t)sec->floorstat : (uint32_t)sec->ceilingstat;
		}
	}

	void FillWallProvenance(SurfaceProvenance& provenance, const PTWallBandDesc& desc, int chunkIndex)
	{
		provenance.sourceType = desc.sourceType;
		provenance.sectorIndex = desc.frontSector != nullptr ? sector.IndexOf(desc.frontSector) : -1;
		provenance.wallIndex = desc.wall != nullptr ? wall.IndexOf(desc.wall) : -1;
		provenance.sectionIndex = -1;
		provenance.mapChunkIndex = chunkIndex;
		provenance.nextSectorIndex = desc.wall != nullptr ? desc.wall->nextsector : -1;
		provenance.drawListType = UINT32_MAX;
		provenance.materialFlags = desc.materialFlags;
		if (desc.wall != nullptr)
		{
			provenance.cstat = (uint32_t)desc.wall->cstat;
		}
	}

	bool SetWallCoordinates(float topLeft, float topRight, float bottomLeft, float bottomRight, PTWallQuad& outQuad)
	{
		if (topLeft <= bottomLeft && topRight <= bottomRight)
		{
			return false;
		}

		if (topLeft >= bottomLeft)
		{
			outQuad.zTop[0] = topLeft;
			outQuad.zBottom[0] = bottomLeft;
		}
		else
		{
			const float deltaTop = topRight - topLeft;
			const float deltaBottom = bottomRight - bottomLeft;
			const float intersectionX = (bottomLeft - topLeft) / (deltaTop - deltaBottom);
			const float intersectionY = topLeft + intersectionX * deltaTop;

			outQuad.x1 = outQuad.x1 + intersectionX * (outQuad.x2 - outQuad.x1);
			outQuad.y1 = outQuad.y1 + intersectionX * (outQuad.y2 - outQuad.y1);
			outQuad.fracLeft = intersectionX;
			outQuad.zTop[0] = intersectionY;
			outQuad.zBottom[0] = intersectionY;
		}

		if (topRight >= bottomRight)
		{
			outQuad.zTop[1] = topRight;
			outQuad.zBottom[1] = bottomRight;
		}
		else
		{
			const float deltaTop = topRight - topLeft;
			const float deltaBottom = bottomRight - bottomLeft;
			const float intersectionX = (bottomLeft - topLeft) / (deltaTop - deltaBottom);
			const float intersectionY = topLeft + intersectionX * deltaTop;

			outQuad.x2 = outQuad.x1 + intersectionX * (outQuad.x2 - outQuad.x1);
			outQuad.y2 = outQuad.y1 + intersectionX * (outQuad.y2 - outQuad.y1);
			outQuad.fracRight = intersectionX;
			outQuad.zTop[1] = intersectionY;
			outQuad.zBottom[1] = intersectionY;
		}

		return outQuad.zTop[0] > outQuad.zBottom[0] || outQuad.zTop[1] > outQuad.zBottom[1];
	}

	void ComputeWallTexcoords(const PTWallBandDesc& desc, PTWallQuad& quad)
	{
		if (desc.texture == nullptr || desc.wall == nullptr || desc.refWall == nullptr)
		{
			return;
		}

		const bool xFlipped = (desc.wall->cstat & CSTAT_WALL_XFLIP) != 0;
		const float leftDistance = xFlipped ? 1.0f - quad.fracLeft : quad.fracLeft;
		const float rightDistance = xFlipped ? 1.0f - quad.fracRight : quad.fracRight;

		float textureWidth = desc.texture->GetDisplayWidth();
		float textureHeight = desc.texture->GetDisplayHeight();
		if ((desc.wall->cstat & CSTAT_WALL_ROTATE_90) != 0)
		{
			std::swap(textureWidth, textureHeight);
		}

		int pow2Size = 1 << sizeToBits((int)textureHeight);
		if ((float)pow2Size < textureHeight)
		{
			pow2Size *= 2;
		}

		const float yPanning = desc.refWall->ypan_ != 0 ? pow2Size * desc.refWall->ypan_ / (256.0f * textureHeight) : 0.0f;
		quad.texcoords[PTWallTexCoord_LowerLeft].u = quad.texcoords[PTWallTexCoord_UpperLeft].u = ((leftDistance * 8.0f * desc.wall->xrepeat) + desc.refWall->xpan_) / textureWidth;
		quad.texcoords[PTWallTexCoord_LowerRight].u = quad.texcoords[PTWallTexCoord_UpperRight].u = ((rightDistance * 8.0f * desc.wall->xrepeat) + desc.refWall->xpan_) / textureWidth;

		const auto setV = [&](float heightLeft, float heightRight, float fraction)
		{
			float h = heightLeft + (heightRight - heightLeft) * fraction;
			h = (-(float)((desc.referenceHeight + h) * 256) / ((textureHeight * 2048.0f) / (float)std::max<int>((int)desc.wall->yrepeat, 1))) + yPanning;
			if ((desc.refWall->cstat & CSTAT_WALL_YFLIP) != 0)
			{
				h = -h;
			}
			return h;
		};

		quad.texcoords[PTWallTexCoord_UpperLeft].v = setV(desc.topLeft, desc.topRight, quad.fracLeft);
		quad.texcoords[PTWallTexCoord_LowerLeft].v = setV(desc.bottomLeft, desc.bottomRight, quad.fracLeft);
		quad.texcoords[PTWallTexCoord_UpperRight].v = setV(desc.topLeft, desc.topRight, quad.fracRight);
		quad.texcoords[PTWallTexCoord_LowerRight].v = setV(desc.bottomLeft, desc.bottomRight, quad.fracRight);
	}

	void AppendWallVertex(SurfaceRef& surface, float x, float z, float y, const PTWallTexCoord& texcoord, uint64_t cornerKey)
	{
		CapturedVertex vertex = {};
		vertex.position[0] = x;
		vertex.position[1] = z;
		vertex.position[2] = y;
		vertex.prevPosition[0] = x;
		vertex.prevPosition[1] = z;
		vertex.prevPosition[2] = y;
		vertex.uv[0] = texcoord.u;
		vertex.uv[1] = texcoord.v;
		vertex.temporalCornerKey = cornerKey;
		surface.vertices.push_back(vertex);
	}

	bool BuildWallSurface(const PTWallBandDesc& desc, int chunkIndex, PTMapSurface& outSurface)
	{
		if (desc.wall == nullptr)
		{
			return false;
		}

		PTWallQuad quad = {};
		quad.x1 = (float)desc.wall->pos.X;
		quad.y1 = (float)-desc.wall->pos.Y;
		quad.x2 = (float)desc.wall->point2Wall()->pos.X;
		quad.y2 = (float)-desc.wall->point2Wall()->pos.Y;
		if (!SetWallCoordinates(desc.topLeft, desc.topRight, desc.bottomLeft, desc.bottomRight, quad))
		{
			return false;
		}

		ComputeWallTexcoords(desc, quad);

		outSurface = {};
		outSurface.kind = desc.surfaceKind;
		outSurface.key.primary = desc.wall != nullptr ? (uint32_t)wall.IndexOf(desc.wall) : UINT32_MAX;
		outSurface.key.secondary = (uint32_t)desc.bandKind;
		outSurface.chunkIndex = (uint32_t)chunkIndex;
		outSurface.surface.material = MakeMaterialRef(desc.texture, desc.palette, desc.shade, desc.alpha, desc.materialFlags);
		FillWallProvenance(outSurface.surface.provenance, desc, chunkIndex);
		outSurface.surface.temporal.materialVerticalReference = desc.referenceHeight;
		outSurface.surface.temporal.materialVerticalReferenceValid =
			desc.texture != nullptr && desc.refWall != nullptr && std::isfinite(desc.referenceHeight);
		outSurface.surface.vertices.reserve(4);
		AppendWallVertex(outSurface.surface, quad.x1, quad.zBottom[0], quad.y1, quad.texcoords[PTWallTexCoord_LowerLeft], 0u);
		AppendWallVertex(outSurface.surface, quad.x1, quad.zTop[0], quad.y1, quad.texcoords[PTWallTexCoord_UpperLeft], 1u);
		AppendWallVertex(outSurface.surface, quad.x2, quad.zTop[1], quad.y2, quad.texcoords[PTWallTexCoord_UpperRight], 2u);
		AppendWallVertex(outSurface.surface, quad.x2, quad.zBottom[1], quad.y2, quad.texcoords[PTWallTexCoord_LowerRight], 3u);
		return true;
	}

	void FinalizeSurfaceStats(const PTMapSurface& surface, PTMapWorldStats& stats)
	{
		stats.surfaceCount++;
		stats.triangleCount += CountTriangles(surface.surface);
		const bool portalTagged = (surface.surface.material.flags & (MaterialFlag_Portal | MaterialFlag_Mirror)) != 0;
		switch (surface.kind)
		{
		case PTMapSurfaceKind::Floor:
		case PTMapSurfaceKind::Ceiling:
			stats.flatSurfaceCount++;
			if (portalTagged)
			{
				stats.portalSurfaceCount++;
			}
			break;
		case PTMapSurfaceKind::Portal:
			stats.portalSurfaceCount++;
			stats.wallSurfaceCount++;
			break;
		default:
			stats.wallSurfaceCount++;
			break;
		}

		if ((surface.surface.material.flags & MaterialFlag_Sky) != 0)
		{
			stats.skySurfaceCount++;
		}
	}

	void BuildPlaneSurface(PTMapWorld& outWorld, PTMapChunk& chunk, sectortype* sec, int sectionIndex, int plane)
	{
		if (sec == nullptr || sectionIndex < 0 || sectionIndex >= (int)sections.Size())
		{
			return;
		}

		FGameTexture* texture = ResolvePlaneTexture(sec, plane);
		if (texture == nullptr || !texture->isValid())
		{
			return;
		}

		TArray<int>* indices = nullptr;
		auto* mesh = sectionGeometry.get(&sections[sectionIndex], plane, { 0.0f, 0.0f }, &indices);
		if (mesh == nullptr || indices == nullptr || indices->Size() < 3)
		{
			return;
		}

		uint32_t materialFlags = GetPlaneMaterialFlags(sec, plane);
		if (IsSkyPlane(sec, plane))
		{
			materialFlags |= MaterialFlag_Sky;
		}

		PTMapSurface surface = {};
		surface.kind = plane == 0 ? PTMapSurfaceKind::Floor : PTMapSurfaceKind::Ceiling;
		surface.key.primary = (uint32_t)sectionIndex;
		surface.key.secondary = (uint32_t)plane;
		surface.chunkIndex = chunk.chunkIndex;
		surface.surface.material = MakeMaterialRef(texture, plane == 0 ? sec->floorpal : sec->ceilingpal, plane == 0 ? sec->floorshade : sec->ceilingshade, 1.0f, materialFlags);
		FillPlaneProvenance(surface.surface.provenance, sec, plane, sectionIndex, (int)chunk.chunkIndex, surface.surface.material.flags);
		surface.surface.vertices.reserve((uint32_t)indices->Size());

		const float base = -(plane == 0 ? sec->floorz : sec->ceilingz);
		for (unsigned i = 0; i < indices->Size(); ++i)
		{
			const int index = (*indices)[i];
			const auto& point = mesh->vertices[index];
			const auto& uv = mesh->texcoords[index];

			CapturedVertex vertex = {};
			vertex.position[0] = point.X;
			vertex.position[1] = base + point.Z;
			vertex.position[2] = point.Y;
			vertex.prevPosition[0] = vertex.position[0];
			vertex.prevPosition[1] = vertex.position[1];
			vertex.prevPosition[2] = vertex.position[2];
			vertex.uv[0] = uv.X;
			vertex.uv[1] = uv.Y;
			vertex.temporalCornerKey = (uint64_t)(uint32_t)index;
			surface.surface.vertices.push_back(vertex);
		}

		FinalizeSurfaceStats(surface, outWorld.stats);
		AppendSurface(outWorld, chunk, std::move(surface));
	}

	void TryAppendWallBand(PTMapWorld& outWorld, PTMapChunk& chunk, const PTWallBandDesc& desc)
	{
		PTMapSurface surface = {};
		if (!BuildWallSurface(desc, (int)chunk.chunkIndex, surface))
		{
			return;
		}

		FinalizeSurfaceStats(surface, outWorld.stats);
		AppendSurface(outWorld, chunk, std::move(surface));
	}

	void BuildWallGeometry(PTMapWorld& outWorld, PTMapChunk& chunk, sectortype* frontSector, walltype* wal, const PTMapBuildOptions& options)
	{
		if (frontSector == nullptr || wal == nullptr)
		{
			return;
		}

		walltype* backWall = wal->twoSided() ? wal->nextWall() : nullptr;
		sectortype* backSector = wal->twoSided() ? wal->nextSector() : nullptr;
		float frontCeilingLeft = 0.0f;
		float frontFloorLeft = 0.0f;
		float frontCeilingRight = 0.0f;
		float frontFloorRight = 0.0f;
		PlanesAtPoint(frontSector, wal->pos.X, wal->pos.Y, &frontCeilingLeft, &frontFloorLeft);
		PlanesAtPoint(frontSector, wal->point2Wall()->pos.X, wal->point2Wall()->pos.Y, &frontCeilingRight, &frontFloorRight);

		if (ShouldTranslateWallMirrorAsMaterial(wal))
		{
			FGameTexture* texture = ResolvePlainMirrorMaterialTexture(wal);
			if (texture == nullptr || !texture->isValid())
			{
				return;
			}

			PTWallBandDesc mirrorDesc = {};
			mirrorDesc.surfaceKind = PTMapSurfaceKind::WallOneSided;
			mirrorDesc.bandKind = PTWallBandKind::OneSided;
			mirrorDesc.wall = wal;
			mirrorDesc.refWall = wal;
			mirrorDesc.frontSector = frontSector;
			mirrorDesc.backSector = backSector;
			mirrorDesc.texture = texture;
			mirrorDesc.shade = wal->shade;
			mirrorDesc.palette = wal->pal;
			mirrorDesc.materialFlags = MaterialFlag_Mirror | MaterialFlag_PlainMirror;
			mirrorDesc.topLeft = frontCeilingLeft;
			mirrorDesc.topRight = frontCeilingRight;
			mirrorDesc.bottomLeft = frontFloorLeft;
			mirrorDesc.bottomRight = frontFloorRight;
			mirrorDesc.referenceHeight = (wal->cstat & CSTAT_WALL_ALIGN_BOTTOM) != 0 ? frontSector->floorz : frontSector->ceilingz;
			TryAppendWallBand(outWorld, chunk, mirrorDesc);
			return;
		}

		if (IsPortalWall(wal))
		{
			PTWallBandDesc portalDesc = {};
			portalDesc.surfaceKind = PTMapSurfaceKind::Portal;
			portalDesc.bandKind = PTWallBandKind::Portal;
			portalDesc.sourceType = SurfaceSourceType::MapPortalSurface;
			portalDesc.wall = wal;
			portalDesc.frontSector = frontSector;
			portalDesc.backSector = backSector;
			portalDesc.topLeft = frontCeilingLeft;
			portalDesc.topRight = frontCeilingRight;
			portalDesc.bottomLeft = frontFloorLeft;
			portalDesc.bottomRight = frontFloorRight;
			portalDesc.materialFlags = (wal->portalflags == PORTAL_WALL_MIRROR) ? MaterialFlag_Mirror : MaterialFlag_Portal;
			TryAppendWallBand(outWorld, chunk, portalDesc);
			return;
		}

		if (backSector == nullptr || backWall == nullptr)
		{
			const FTextureID tileNum = ((wal->cstat & CSTAT_WALL_1WAY) != 0 && wal->nextwall != -1) ? wal->overtexture : wal->walltexture;
			FGameTexture* texture = TexMan.GetGameTexture(tileNum, true);
			if (texture == nullptr || !texture->isValid())
			{
				return;
			}

			float referenceHeight = (wal->cstat & CSTAT_WALL_ALIGN_BOTTOM) != 0 ? frontSector->floorz : frontSector->ceilingz;
			PTWallBandDesc desc = {};
			desc.surfaceKind = PTMapSurfaceKind::WallOneSided;
			desc.bandKind = PTWallBandKind::OneSided;
			desc.wall = wal;
			desc.refWall = wal;
			desc.frontSector = frontSector;
			desc.texture = texture;
			desc.shade = wal->shade;
			desc.palette = wal->pal;
			desc.materialFlags = (wal->cstat & CSTAT_WALL_1WAY) != 0 ? (MaterialFlag_OneWay | MaterialFlag_AlphaClip) : MaterialFlag_None;
			desc.topLeft = frontCeilingLeft;
			desc.topRight = frontCeilingRight;
			desc.bottomLeft = frontFloorLeft;
			desc.bottomRight = frontFloorRight;
			desc.referenceHeight = referenceHeight;
			TryAppendWallBand(outWorld, chunk, desc);
			return;
		}

		float backFloorLeft = 0.0f;
		float backFloorRight = 0.0f;
		float backCeilingLeft = 0.0f;
		float backCeilingRight = 0.0f;
		PlanesAtPoint(backSector, wal->pos.X, wal->pos.Y, &backCeilingLeft, &backFloorLeft);
		PlanesAtPoint(backSector, wal->point2Wall()->pos.X, wal->point2Wall()->pos.Y, &backCeilingRight, &backFloorRight);

		if ((frontSector->ceilingstat & backSector->ceilingstat & CSTAT_SECTOR_SKY) == 0)
		{
			float adjustedBackCeilingLeft = backCeilingLeft;
			float adjustedBackCeilingRight = backCeilingRight;
			if (frontFloorLeft > backCeilingLeft || frontFloorRight > backCeilingRight)
			{
				if ((frontFloorLeft > backCeilingLeft && frontFloorRight > backCeilingRight) || frontSector->portalflags == PORTAL_SECTOR_FLOOR)
				{
					adjustedBackCeilingLeft = frontFloorLeft;
					adjustedBackCeilingRight = frontFloorRight;
				}
			}

			if (adjustedBackCeilingLeft < frontCeilingLeft || adjustedBackCeilingRight < frontCeilingRight)
			{
				FGameTexture* texture = TexMan.GetGameTexture(wal->walltexture, true);
				if (texture != nullptr && texture->isValid())
				{
					PTWallBandDesc desc = {};
					desc.surfaceKind = PTMapSurfaceKind::WallUpper;
					desc.bandKind = PTWallBandKind::Upper;
					desc.wall = wal;
					desc.refWall = wal;
					desc.frontSector = frontSector;
					desc.backSector = backSector;
					desc.texture = texture;
					desc.shade = wal->shade;
					desc.palette = wal->pal;
					desc.topLeft = frontCeilingLeft;
					desc.topRight = frontCeilingRight;
					desc.bottomLeft = adjustedBackCeilingLeft;
					desc.bottomRight = adjustedBackCeilingRight;
					desc.referenceHeight = (wal->cstat & CSTAT_WALL_ALIGN_BOTTOM) != 0 ? frontSector->ceilingz : backSector->ceilingz;
					TryAppendWallBand(outWorld, chunk, desc);
				}
			}
		}

		if ((wal->cstat & (CSTAT_WALL_MASKED | CSTAT_WALL_1WAY)) != 0)
		{
			FGameTexture* texture = TexMan.GetGameTexture(wal->overtexture, true);
			if (texture != nullptr && texture->isValid())
			{
				float topLeft = 0.0f;
				float topRight = 0.0f;
				float bottomLeft = 0.0f;
				float bottomRight = 0.0f;
				if ((backCeilingLeft - frontCeilingLeft) * (backCeilingRight - frontCeilingRight) >= 0)
				{
					topLeft = std::min(backCeilingLeft, frontCeilingLeft);
					topRight = std::min(backCeilingRight, frontCeilingRight);
				}
				else
				{
					topLeft = backCeilingLeft;
					topRight = backCeilingRight;
				}

				if ((backFloorLeft - frontFloorLeft) * (backFloorRight - frontFloorRight) >= 0)
				{
					bottomLeft = std::max(backFloorLeft, frontFloorLeft);
					bottomRight = std::max(backFloorRight, frontFloorRight);
				}
				else
				{
					bottomLeft = backFloorLeft;
					bottomRight = backFloorRight;
				}

				if (topLeft > bottomLeft || topRight > bottomRight)
				{
					PTWallBandDesc desc = {};
					desc.surfaceKind = PTMapSurfaceKind::WallMiddle;
					desc.bandKind = PTWallBandKind::Middle;
					desc.wall = wal;
					desc.refWall = wal;
					desc.frontSector = frontSector;
					desc.backSector = backSector;
					desc.texture = texture;
					desc.shade = wal->shade;
					desc.palette = wal->pal;
					desc.materialFlags = MaterialFlag_AlphaClip;
					if ((wal->cstat & CSTAT_WALL_1WAY) != 0)
					{
						desc.materialFlags |= MaterialFlag_OneWay;
					}
					desc.topLeft = topLeft;
					desc.topRight = topRight;
					desc.bottomLeft = bottomLeft;
					desc.bottomRight = bottomRight;
					if ((wal->cstat & CSTAT_WALL_1WAY) != 0)
					{
						desc.referenceHeight = (wal->cstat & CSTAT_WALL_ALIGN_BOTTOM) != 0 ? frontSector->ceilingz : backSector->ceilingz;
					}
					else
					{
						desc.referenceHeight = (wal->cstat & CSTAT_WALL_ALIGN_BOTTOM) != 0 ? std::min(frontSector->floorz, backSector->floorz) : std::max(frontSector->ceilingz, backSector->ceilingz);
					}
					TryAppendWallBand(outWorld, chunk, desc);
				}
			}
		}

		if ((frontSector->floorstat & backSector->floorstat & CSTAT_SECTOR_SKY) == 0)
		{
			float adjustedBackFloorLeft = backFloorLeft;
			float adjustedBackFloorRight = backFloorRight;
			if (frontCeilingLeft < backFloorLeft || frontCeilingRight < backFloorRight)
			{
				if ((frontCeilingLeft < backFloorLeft && frontCeilingRight < backFloorRight) || frontSector->portalflags == PORTAL_SECTOR_CEILING)
				{
					adjustedBackFloorLeft = frontCeilingLeft;
					adjustedBackFloorRight = frontCeilingRight;
				}
			}

			if (adjustedBackFloorLeft > frontFloorLeft || adjustedBackFloorRight > frontFloorRight)
			{
				walltype* referenceWall = (wal->cstat & CSTAT_WALL_BOTTOM_SWAP) != 0 ? backWall : wal;
				FGameTexture* texture = TexMan.GetGameTexture(referenceWall->walltexture, true);
				if (texture != nullptr && texture->isValid())
				{
					PTWallBandDesc desc = {};
					desc.surfaceKind = PTMapSurfaceKind::WallLower;
					desc.bandKind = PTWallBandKind::Lower;
					desc.wall = wal;
					desc.refWall = referenceWall;
					desc.frontSector = frontSector;
					desc.backSector = backSector;
					desc.texture = texture;
					desc.shade = referenceWall->shade;
					desc.palette = referenceWall->pal;
					desc.topLeft = adjustedBackFloorLeft;
					desc.topRight = adjustedBackFloorRight;
					desc.bottomLeft = frontFloorLeft;
					desc.bottomRight = frontFloorRight;
					desc.referenceHeight = (referenceWall->cstat & CSTAT_WALL_ALIGN_BOTTOM) != 0 ? frontSector->ceilingz : backSector->floorz;
					TryAppendWallBand(outWorld, chunk, desc);
				}
			}
		}
	}

	bool BuildSectorChunk(PTMapWorld& outWorld, uint32_t sectorIndex, uint32_t chunkIndex, PTMapChunk& outChunk, const PTMapBuildOptions& options)
	{
		if (sectorIndex >= sector.Size() || sectorIndex >= sectionsPerSector.Size())
		{
			return false;
		}

		PTMapChunk chunk = {};
		chunk.kind = PTMapChunkKind::Sector;
		chunk.chunkIndex = chunkIndex;
		chunk.sectorIndex = (int32_t)sectorIndex;
		chunk.updatePartitionIdentity = MakeMapChunkUpdatePartitionIdentity(chunk.chunkIndex, chunk.sectorIndex);
		chunk.currentGeometryIdentity = MakeMapChunkGeometryIdentity(chunk.chunkIndex);
		chunk.currentInstanceIdentity = MakeMapChunkInstanceIdentity(chunk.chunkIndex);
		chunk.firstSurface = (uint32_t)outWorld.surfaces.size();

		sectortype* sec = &sector[sectorIndex];
		for (int sectionIndex : sectionsPerSector[sectorIndex])
		{
			BuildPlaneSurface(outWorld, chunk, sec, sectionIndex, 0);
			BuildPlaneSurface(outWorld, chunk, sec, sectionIndex, 1);
		}

		for (auto& wal : sec->walls)
		{
			BuildWallGeometry(outWorld, chunk, sec, &wal, options);
		}

		chunk.surfaceCount = (uint32_t)outWorld.surfaces.size() - chunk.firstSurface;
		FinalizeChunkBounds(chunk);
		outChunk = chunk;
		return true;
	}

	void SetPortalDeltaPT(PTMapPortal& portal, double buildX, double buildY, double buildZ)
	{
		portal.delta[0] = buildX;
		portal.delta[1] = -buildZ;
		portal.delta[2] = -buildY;
	}

	std::vector<uint32_t> BuildSectorChunkLookup(const PTMapWorld& world)
	{
		std::vector<uint32_t> lookup(sector.Size(), UINT32_MAX);
		for (const PTMapChunk& chunk : world.chunks)
		{
			if (chunk.kind == PTMapChunkKind::Sector && chunk.sectorIndex >= 0 && (unsigned)chunk.sectorIndex < lookup.size())
			{
				lookup[(unsigned)chunk.sectorIndex] = chunk.chunkIndex;
			}
		}
		return lookup;
	}

	std::vector<uint32_t> BuildPortalWallSurfaceLookup(const PTMapWorld& world)
	{
		std::vector<uint32_t> lookup(wall.Size(), UINT32_MAX);
		for (uint32_t surfaceIndex = 0; surfaceIndex < world.surfaces.size(); ++surfaceIndex)
		{
			const PTMapSurface& surface = world.surfaces[surfaceIndex];
			if (surface.kind != PTMapSurfaceKind::Portal)
			{
				continue;
			}

			const int wallIndex = surface.surface.provenance.wallIndex;
			if (wallIndex >= 0 && (unsigned)wallIndex < lookup.size())
			{
				lookup[(unsigned)wallIndex] = surfaceIndex;
			}
		}
		return lookup;
	}

	std::vector<uint32_t> BuildPortalPlaneSurfaceLookup(const PTMapWorld& world)
	{
		std::vector<uint32_t> lookup(sector.Size() * 2u, UINT32_MAX);
		for (uint32_t surfaceIndex = 0; surfaceIndex < world.surfaces.size(); ++surfaceIndex)
		{
			const PTMapSurface& surface = world.surfaces[surfaceIndex];
			const SurfaceProvenance& provenance = surface.surface.provenance;
			const int sectorIndex = provenance.sectorIndex;
			if (sectorIndex < 0 || (unsigned)sectorIndex >= sector.Size())
			{
				continue;
			}

			int plane = -1;
			if (provenance.sourceType == SurfaceSourceType::MapFloorSection)
			{
				plane = 0;
			}
			else if (provenance.sourceType == SurfaceSourceType::MapCeilingSection)
			{
				plane = 1;
			}

			if (plane < 0 || (surface.surface.material.flags & (MaterialFlag_Portal | MaterialFlag_Mirror)) == 0)
			{
				continue;
			}

			const size_t lookupIndex = (size_t)sectorIndex * 2u + (size_t)plane;
			if (lookupIndex < lookup.size() && lookup[lookupIndex] == UINT32_MAX)
			{
				lookup[lookupIndex] = surfaceIndex;
			}
		}

		return lookup;
	}

	void BuildLocalSpaces(PTMapWorld& outWorld, const std::vector<uint32_t>& sectorChunkLookup)
	{
		std::vector<uint8_t> visited(sector.Size(), 0u);
		outWorld.localSpaces.clear();

		for (PTMapChunk& chunk : outWorld.chunks)
		{
			chunk.localSpaceIndex = UINT32_MAX;
		}

		for (const PTMapChunk& seedChunk : outWorld.chunks)
		{
			if (seedChunk.kind != PTMapChunkKind::Sector || seedChunk.sectorIndex < 0 || (unsigned)seedChunk.sectorIndex >= sector.Size())
			{
				continue;
			}

			const uint32_t seedSectorIndex = (uint32_t)seedChunk.sectorIndex;
			if (visited[seedSectorIndex] != 0u)
			{
				continue;
			}

			PTMapLocalSpace localSpace = {};
			localSpace.localSpaceIndex = (uint32_t)outWorld.localSpaces.size();
			localSpace.anchorSectorIndex = seedChunk.sectorIndex;

			std::vector<uint32_t> pendingSectors;
			pendingSectors.push_back(seedSectorIndex);
			visited[seedSectorIndex] = 1u;

			while (!pendingSectors.empty())
			{
				const uint32_t sectorIndex = pendingSectors.back();
				pendingSectors.pop_back();

				if (sectorIndex < sectorChunkLookup.size())
				{
					const uint32_t chunkIndex = sectorChunkLookup[sectorIndex];
					if (chunkIndex < outWorld.chunks.size() && outWorld.chunks[chunkIndex].localSpaceIndex == UINT32_MAX)
					{
						outWorld.chunks[chunkIndex].localSpaceIndex = localSpace.localSpaceIndex;
						localSpace.chunkCount++;
					}
				}

				const sectortype& sec = sector[sectorIndex];
				for (const walltype& wal : sec.walls)
				{
					if (IsPortalWall(&wal) || wal.nextsector < 0 || (unsigned)wal.nextsector >= sector.Size())
					{
						continue;
					}

					const uint32_t nextSectorIndex = (uint32_t)wal.nextsector;
					if (visited[nextSectorIndex] != 0u)
					{
						continue;
					}

					visited[nextSectorIndex] = 1u;
					pendingSectors.push_back(nextSectorIndex);
				}
			}

			outWorld.localSpaces.push_back(localSpace);
		}

		outWorld.stats.localSpaceCount = (uint32_t)outWorld.localSpaces.size();
	}

	void AppendPortalTarget(PTMapWorld& outWorld, PTMapPortal& portal, const std::vector<uint32_t>& sectorChunkLookup, int32_t sectorIndex, int32_t wallIndex)
	{
		PTMapPortalTarget target = {};
		target.sectorIndex = sectorIndex;
		target.wallIndex = wallIndex;
		if (sectorIndex >= 0 && (unsigned)sectorIndex < sectorChunkLookup.size())
		{
			target.chunkIndex = sectorChunkLookup[(unsigned)sectorIndex];
			if (target.chunkIndex < outWorld.chunks.size())
			{
				target.localSpaceIndex = outWorld.chunks[target.chunkIndex].localSpaceIndex;
			}
		}

		if (portal.targetCount == 0)
		{
			portal.firstTarget = (uint32_t)outWorld.portalTargets.size();
		}

		outWorld.portalTargets.push_back(target);
		portal.targetCount++;
	}

	void BuildPortalGraph(PTMapWorld& outWorld, const PTMapBuildOptions& options)
	{
		outWorld.portals.clear();
		outWorld.portalTargets.clear();
		outWorld.stats.portalCount = 0;
		outWorld.stats.portalTargetCount = 0;
		outWorld.stats.wallPortalCount = 0;
		outWorld.stats.sectorPortalCount = 0;
		outWorld.stats.mirrorPortalCount = 0;
		outWorld.stats.runtimePortalCount = 0;

		outWorld.sectorChunkLookup = BuildSectorChunkLookup(outWorld);
		BuildLocalSpaces(outWorld, outWorld.sectorChunkLookup);
		const std::vector<uint32_t> portalWallSurfaceLookup = BuildPortalWallSurfaceLookup(outWorld);
		const std::vector<uint32_t> portalPlaneSurfaceLookup = BuildPortalPlaneSurfaceLookup(outWorld);

		for (unsigned sectorIndex = 0; sectorIndex < sector.Size(); ++sectorIndex)
		{
			const sectortype& sec = sector[sectorIndex];
			const uint32_t sourceChunkIndex = sectorIndex < outWorld.sectorChunkLookup.size() ? outWorld.sectorChunkLookup[sectorIndex] : UINT32_MAX;
			const uint32_t sourceLocalSpaceIndex = sourceChunkIndex < outWorld.chunks.size() ? outWorld.chunks[sourceChunkIndex].localSpaceIndex : UINT32_MAX;

			for (const walltype& wal : sec.walls)
			{
				if (ShouldTranslateWallMirrorAsMaterial(&wal))
				{
					continue;
				}

				if (!IsPortalWall(&wal))
				{
					continue;
				}

				PTMapPortal portal = {};
				portal.portalIndex = (uint32_t)outWorld.portals.size();
				portal.sourceChunkIndex = sourceChunkIndex;
				portal.sourceLocalSpaceIndex = sourceLocalSpaceIndex;
				portal.sourceSectorIndex = (int32_t)sectorIndex;
				portal.sourceWallIndex = wall.IndexOf(&wal);
				portal.sourcePlane = -1;
				portal.portalNum = wal.portalnum;
				if (portal.sourceWallIndex >= 0 && (unsigned)portal.sourceWallIndex < portalWallSurfaceLookup.size())
				{
					portal.sourceSurfaceIndex = portalWallSurfaceLookup[(unsigned)portal.sourceWallIndex];
				}

				switch (wal.portalflags)
				{
				case PORTAL_WALL_MIRROR:
					portal.kind = PTPortalKind::WallMirror;
					outWorld.stats.wallPortalCount++;
					outWorld.stats.mirrorPortalCount++;
					break;

				case PORTAL_WALL_VIEW:
					portal.kind = PTPortalKind::WallView;
					outWorld.stats.wallPortalCount++;
					if (wal.portalnum >= 0 && (unsigned)wal.portalnum < wall.Size())
					{
						const walltype& destWall = wall[(unsigned)wal.portalnum];
						auto srcCenter = wal.center();
						auto dstCenter = destWall.center();
						SetPortalDeltaPT(portal, dstCenter.X - srcCenter.X, dstCenter.Y - srcCenter.Y, 0.0);
						AppendPortalTarget(outWorld, portal, outWorld.sectorChunkLookup, destWall.sector, wal.portalnum);
					}
					break;

				case PORTAL_WALL_TO_SPRITE:
					portal.kind = PTPortalKind::WallToSprite;
					portal.runtimeBoundTarget = true;
					outWorld.stats.wallPortalCount++;
					outWorld.stats.runtimePortalCount++;
					break;

				default:
					continue;
				}

				outWorld.portals.push_back(portal);
			}

			auto appendSectorPortal = [&](PTPortalKind kind, int plane)
			{
				PTMapPortal portal = {};
				portal.portalIndex = (uint32_t)outWorld.portals.size();
				portal.kind = kind;
				portal.sourceChunkIndex = sourceChunkIndex;
				portal.sourceLocalSpaceIndex = sourceLocalSpaceIndex;
				portal.sourceSectorIndex = (int32_t)sectorIndex;
				portal.sourcePlane = plane;
				portal.portalNum = sec.portalnum;
				const size_t planeLookupIndex = (size_t)sectorIndex * 2u + (size_t)std::max(plane, 0);
				if (planeLookupIndex < portalPlaneSurfaceLookup.size())
				{
					portal.sourceSurfaceIndex = portalPlaneSurfaceLookup[planeLookupIndex];
				}
				outWorld.stats.sectorPortalCount++;

				if (kind == PTPortalKind::SectorFloorMirror || kind == PTPortalKind::SectorCeilingMirror)
				{
					outWorld.stats.mirrorPortalCount++;
				}
				else if (sec.portalnum >= 0 && (unsigned)sec.portalnum < allPortals.Size())
				{
					const PortalDesc& desc = allPortals[(unsigned)sec.portalnum];
					SetPortalDeltaPT(portal, desc.delta.X, desc.delta.Y, desc.delta.Z);
					for (int targetSectorIndex : desc.targets)
					{
						AppendPortalTarget(outWorld, portal, outWorld.sectorChunkLookup, targetSectorIndex, -1);
					}
				}

				outWorld.portals.push_back(portal);
			};

			switch (sec.portalflags)
			{
			case PORTAL_SECTOR_FLOOR:
				appendSectorPortal(PTPortalKind::SectorFloorStack, 0);
				break;

			case PORTAL_SECTOR_CEILING:
				appendSectorPortal(PTPortalKind::SectorCeilingStack, 1);
				break;

			case PORTAL_SECTOR_FLOOR_REFLECT:
				appendSectorPortal(PTPortalKind::SectorFloorMirror, 0);
				break;

			case PORTAL_SECTOR_CEILING_REFLECT:
				appendSectorPortal(PTPortalKind::SectorCeilingMirror, 1);
				break;

			default:
				break;
			}
		}

		outWorld.stats.portalCount = (uint32_t)outWorld.portals.size();
		outWorld.stats.portalTargetCount = (uint32_t)outWorld.portalTargets.size();
	}

	void CaptureWallMutationSnapshot(const walltype& wal, PTMapWallMutationSnapshot& outSnapshot)
	{
		outSnapshot.pos = wal.pos;
		outSnapshot.point2 = wal.point2;
		outSnapshot.nextwall = wal.nextwall;
		outSnapshot.nextsector = wal.nextsector;
		outSnapshot.cstat = (uint16_t)wal.cstat;
		outSnapshot.portalflags = wal.portalflags;
		outSnapshot.walltexture = wal.walltexture.GetIndex();
		outSnapshot.overtexture = wal.overtexture.GetIndex();
		outSnapshot.xpan = wal.xpan_;
		outSnapshot.ypan = wal.ypan_;
		outSnapshot.xrepeat = wal.xrepeat;
		outSnapshot.yrepeat = wal.yrepeat;
		outSnapshot.pal = wal.pal;
		outSnapshot.shade = wal.shade;
		if (wal.nextsector >= 0 && (unsigned)wal.nextsector < sector.Size())
		{
			const sectortype& adjacent = sector[(unsigned)wal.nextsector];
			outSnapshot.adjacentFloorz = adjacent.floorz;
			outSnapshot.adjacentCeilingz = adjacent.ceilingz;
			outSnapshot.adjacentFloorstat = (uint16_t)adjacent.floorstat;
			outSnapshot.adjacentCeilingstat = (uint16_t)adjacent.ceilingstat;
			outSnapshot.adjacentFloorheinum = adjacent.floorheinum;
			outSnapshot.adjacentCeilingheinum = adjacent.ceilingheinum;
			outSnapshot.adjacentPortalflags = adjacent.portalflags;
		}

		if (wal.nextwall >= 0 && (unsigned)wal.nextwall < wall.Size())
		{
			const walltype& nextWall = wall[(unsigned)wal.nextwall];
			outSnapshot.nextWallCstat = (uint16_t)nextWall.cstat;
			outSnapshot.nextWallTexture = nextWall.walltexture.GetIndex();
			outSnapshot.nextOverTexture = nextWall.overtexture.GetIndex();
			outSnapshot.nextWallPal = nextWall.pal;
			outSnapshot.nextWallShade = nextWall.shade;
		}
	}

	bool CaptureChunkMutationBaselineInternal(const PTMapChunk& chunk, PTMapChunkMutationBaseline& outBaseline)
	{
		outBaseline = {};
		if (chunk.kind != PTMapChunkKind::Sector || chunk.sectorIndex < 0 || (unsigned)chunk.sectorIndex >= sector.Size())
		{
			return false;
		}

		const sectortype& sec = sector[(unsigned)chunk.sectorIndex];
		outBaseline.sectorIndex = chunk.sectorIndex;
		outBaseline.signature = ComputeMapChunkGeometrySignature(chunk);
		outBaseline.floorz = sec.floorz;
		outBaseline.ceilingz = sec.ceilingz;
		outBaseline.floorstat = (uint16_t)sec.floorstat;
		outBaseline.ceilingstat = (uint16_t)sec.ceilingstat;
		outBaseline.floorheinum = sec.floorheinum;
		outBaseline.ceilingheinum = sec.ceilingheinum;
		outBaseline.portalflags = sec.portalflags;
		outBaseline.floortexture = sec.floortexture.GetIndex();
		outBaseline.ceilingtexture = sec.ceilingtexture.GetIndex();
		outBaseline.floorxpan = sec.floorxpan_;
		outBaseline.floorypan = sec.floorypan_;
		outBaseline.ceilingxpan = sec.ceilingxpan_;
		outBaseline.ceilingypan = sec.ceilingypan_;
		outBaseline.floorpal = sec.floorpal;
		outBaseline.ceilingpal = sec.ceilingpal;
		outBaseline.floorshade = sec.floorshade;
		outBaseline.ceilingshade = sec.ceilingshade;

		for (int sectionIndex : sectionsPerSector[(unsigned)chunk.sectorIndex])
		{
			outBaseline.sectionIndices.push_back(sectionIndex);
		}

		outBaseline.walls.reserve(sec.walls.Size());
		for (const walltype& wal : sec.walls)
		{
			PTMapWallMutationSnapshot snapshot = {};
			CaptureWallMutationSnapshot(wal, snapshot);
			outBaseline.walls.push_back(snapshot);
		}

		return true;
	}

	bool AnalyzeChunkMutationInternal(const PTMapChunk& chunk, const PTMapChunkMutationBaseline& baseline, PTMapChunkMutationAnalysis& outAnalysis)
	{
		outAnalysis = {};
		if (chunk.kind != PTMapChunkKind::Sector || chunk.sectorIndex < 0 || (unsigned)chunk.sectorIndex >= sector.Size())
		{
			return false;
		}

		const sectortype& sec = sector[(unsigned)chunk.sectorIndex];
		outAnalysis.signature = ComputeMapChunkGeometrySignature(chunk);

		if (baseline.sectorIndex != chunk.sectorIndex ||
			baseline.floorz != sec.floorz ||
			baseline.ceilingz != sec.ceilingz ||
			baseline.floorheinum != sec.floorheinum ||
			baseline.ceilingheinum != sec.ceilingheinum)
		{
			outAnalysis.reasonMask |= PTMapChunkMutationReason_SectorGeometry;
		}

		if (baseline.floorstat != (uint16_t)sec.floorstat ||
			baseline.ceilingstat != (uint16_t)sec.ceilingstat ||
			baseline.portalflags != sec.portalflags ||
			baseline.floortexture != sec.floortexture.GetIndex() ||
			baseline.ceilingtexture != sec.ceilingtexture.GetIndex() ||
			baseline.floorxpan != sec.floorxpan_ ||
			baseline.floorypan != sec.floorypan_ ||
			baseline.ceilingxpan != sec.ceilingxpan_ ||
			baseline.ceilingypan != sec.ceilingypan_ ||
			baseline.floorpal != sec.floorpal ||
			baseline.ceilingpal != sec.ceilingpal ||
			baseline.floorshade != sec.floorshade ||
			baseline.ceilingshade != sec.ceilingshade)
		{
			outAnalysis.reasonMask |= PTMapChunkMutationReason_SectorMaterial;
		}

		outAnalysis.sectorDirty = sec.dirty != 0;
		if (outAnalysis.sectorDirty)
		{
			outAnalysis.reasonMask |= PTMapChunkMutationReason_SectorDirty;
		}

		outAnalysis.dragged = (sec.exflags & SECTOREX_DRAGGED) != 0;
		if (outAnalysis.dragged)
		{
			outAnalysis.reasonMask |= PTMapChunkMutationReason_Dragged;
		}

		for (int sectionIndex : baseline.sectionIndices)
		{
			if ((unsigned)sectionIndex < sections.Size() && sections[sectionIndex].dirty != 0)
			{
				outAnalysis.sectionDirtyCount++;
			}
		}
		if (outAnalysis.sectionDirtyCount > 0)
		{
			outAnalysis.reasonMask |= PTMapChunkMutationReason_SectionDirty;
		}

		if (baseline.walls.size() != sec.walls.Size())
		{
			outAnalysis.reasonMask |= PTMapChunkMutationReason_WallGeometry;
		}

		const size_t wallCount = std::min<size_t>(baseline.walls.size(), sec.walls.Size());
		for (size_t wallIndex = 0; wallIndex < wallCount; ++wallIndex)
		{
			const PTMapWallMutationSnapshot& baselineWall = baseline.walls[wallIndex];
			const walltype& liveWall = sec.walls[(unsigned)wallIndex];
			const uint16_t baselineCstat = baselineWall.cstat;
			const uint16_t liveCstat = (uint16_t)liveWall.cstat;
			static constexpr uint16_t kWallTopologyMutationCstatMask =
				CSTAT_WALL_MASKED |
				CSTAT_WALL_1WAY;

			if (baselineWall.pos != liveWall.pos ||
				baselineWall.point2 != liveWall.point2 ||
				baselineWall.nextwall != liveWall.nextwall ||
				baselineWall.nextsector != liveWall.nextsector)
			{
				outAnalysis.reasonMask |= PTMapChunkMutationReason_WallGeometry;
			}

			if (((baselineCstat ^ liveCstat) & kWallTopologyMutationCstatMask) != 0 ||
				baselineWall.portalflags != liveWall.portalflags)
			{
				outAnalysis.reasonMask |= PTMapChunkMutationReason_WallGeometry;
			}

			if (baselineCstat != liveCstat ||
				baselineWall.portalflags != liveWall.portalflags ||
				baselineWall.walltexture != liveWall.walltexture.GetIndex() ||
				baselineWall.overtexture != liveWall.overtexture.GetIndex() ||
				baselineWall.xpan != liveWall.xpan_ ||
				baselineWall.ypan != liveWall.ypan_ ||
				baselineWall.xrepeat != liveWall.xrepeat ||
				baselineWall.yrepeat != liveWall.yrepeat ||
				baselineWall.pal != liveWall.pal ||
				baselineWall.shade != liveWall.shade)
			{
				outAnalysis.reasonMask |= PTMapChunkMutationReason_WallMaterial;
			}

			if (baselineWall.adjacentFloorz != ((liveWall.nextsector >= 0 && (unsigned)liveWall.nextsector < sector.Size()) ? sector[(unsigned)liveWall.nextsector].floorz : 0.0) ||
				baselineWall.adjacentCeilingz != ((liveWall.nextsector >= 0 && (unsigned)liveWall.nextsector < sector.Size()) ? sector[(unsigned)liveWall.nextsector].ceilingz : 0.0) ||
				baselineWall.adjacentFloorheinum != ((liveWall.nextsector >= 0 && (unsigned)liveWall.nextsector < sector.Size()) ? sector[(unsigned)liveWall.nextsector].floorheinum : 0) ||
				baselineWall.adjacentCeilingheinum != ((liveWall.nextsector >= 0 && (unsigned)liveWall.nextsector < sector.Size()) ? sector[(unsigned)liveWall.nextsector].ceilingheinum : 0) ||
				baselineWall.adjacentFloorstat != ((liveWall.nextsector >= 0 && (unsigned)liveWall.nextsector < sector.Size()) ? (uint16_t)sector[(unsigned)liveWall.nextsector].floorstat : 0) ||
				baselineWall.adjacentCeilingstat != ((liveWall.nextsector >= 0 && (unsigned)liveWall.nextsector < sector.Size()) ? (uint16_t)sector[(unsigned)liveWall.nextsector].ceilingstat : 0) ||
				baselineWall.adjacentPortalflags != ((liveWall.nextsector >= 0 && (unsigned)liveWall.nextsector < sector.Size()) ? sector[(unsigned)liveWall.nextsector].portalflags : 0))
			{
				outAnalysis.reasonMask |= PTMapChunkMutationReason_WallGeometry;
			}

			if (baselineWall.nextWallCstat != ((liveWall.nextwall >= 0 && (unsigned)liveWall.nextwall < wall.Size()) ? (uint16_t)wall[(unsigned)liveWall.nextwall].cstat : 0) ||
				baselineWall.nextWallTexture != ((liveWall.nextwall >= 0 && (unsigned)liveWall.nextwall < wall.Size()) ? wall[(unsigned)liveWall.nextwall].walltexture.GetIndex() : -1) ||
				baselineWall.nextOverTexture != ((liveWall.nextwall >= 0 && (unsigned)liveWall.nextwall < wall.Size()) ? wall[(unsigned)liveWall.nextwall].overtexture.GetIndex() : -1) ||
				baselineWall.nextWallPal != ((liveWall.nextwall >= 0 && (unsigned)liveWall.nextwall < wall.Size()) ? wall[(unsigned)liveWall.nextwall].pal : 0) ||
				baselineWall.nextWallShade != ((liveWall.nextwall >= 0 && (unsigned)liveWall.nextwall < wall.Size()) ? wall[(unsigned)liveWall.nextwall].shade : 0))
			{
				outAnalysis.reasonMask |= PTMapChunkMutationReason_WallMaterial;
			}
		}

		outAnalysis.signatureChanged = outAnalysis.signature != baseline.signature;
		return true;
	}
}

namespace nri_scene
{
void NotifyLevelGeometryReady()
{
	++gPendingLevelGeometryBuildSerial;
}

uint64_t GetPendingLevelGeometryBuildSerial()
{
	return gPendingLevelGeometryBuildSerial;
}

bool BuildMapWorld(PTMapWorld& outWorld, const PTMapBuildOptions& options)
{
	outWorld.Reset();
	outWorld.level = currentLevel;
	outWorld.buildSerial = gPendingLevelGeometryBuildSerial;
	outWorld.topologyRevision = ++gMapWorldTopologyRevision;

	if (sector.Size() == 0 || sections.Size() == 0 || sectionsPerSector.Size() == 0)
	{
		return false;
	}

	outWorld.stats.sectorCount = (uint32_t)sector.Size();
	outWorld.stats.sectionCount = (uint32_t)sections.Size();
	outWorld.chunks.reserve(sector.Size());
	outWorld.surfaces.reserve((size_t)sections.Size() * 2u + (size_t)wall.Size() * 3u);

	for (unsigned sectorIndex = 0; sectorIndex < sector.Size(); ++sectorIndex)
	{
		PTMapChunk chunk = {};
		if (!BuildSectorChunk(outWorld, sectorIndex, (uint32_t)outWorld.chunks.size(), chunk, options))
		{
			return false;
		}

		outWorld.chunks.push_back(std::move(chunk));
	}

	outWorld.stats.chunkCount = (uint32_t)outWorld.chunks.size();
	BuildPortalGraph(outWorld, options);
	outWorld.valid = true;
	return true;
}

bool BuildLiveMapChunkWorld(const PTMapChunk& chunk, PTMapWorld& outWorld, PTMapWorldStats* outStats, const PTMapBuildOptions& options)
{
	outWorld = {};
	if (outStats != nullptr)
	{
		*outStats = {};
	}

	if (chunk.kind != PTMapChunkKind::Sector || chunk.sectorIndex < 0)
	{
		return false;
	}

	outWorld.level = currentLevel;
	outWorld.buildSerial = gPendingLevelGeometryBuildSerial;
	outWorld.topologyRevision = ++gMapWorldTopologyRevision;
	outWorld.stats.sectorCount = (uint32_t)sector.Size();
	outWorld.stats.sectionCount = (uint32_t)sections.Size();

	PTMapChunk liveChunk = {};
	if (!BuildSectorChunk(outWorld, (uint32_t)chunk.sectorIndex, chunk.chunkIndex, liveChunk, options))
	{
		return false;
	}

	outWorld.chunks.push_back(liveChunk);
	outWorld.sectorChunkLookup = BuildSectorChunkLookup(outWorld);
	outWorld.stats.chunkCount = 1;
	outWorld.valid = true;
	if (outStats != nullptr)
	{
		*outStats = outWorld.stats;
	}

	return true;
}

bool BuildLiveMapChunkSceneView(const PTMapChunk& chunk, SceneView& outView, PTMapWorldStats* outStats, const PTMapBuildOptions& options)
{
	outView = {};
	PTMapWorld liveWorld = {};
	if (!BuildLiveMapChunkWorld(chunk, liveWorld, outStats, options))
	{
		return false;
	}

	BuildMapChunkSceneView(liveWorld, liveWorld.chunks[0], outView);
	return true;
}

uint64_t ComputeMapChunkGeometrySignature(const PTMapChunk& chunk)
{
	if (chunk.kind != PTMapChunkKind::Sector || chunk.sectorIndex < 0 || (unsigned)chunk.sectorIndex >= sector.Size())
	{
		return 0;
	}

	const sectortype& sec = sector[(unsigned)chunk.sectorIndex];
	uint64_t hash = 1469598103934665603ull;
	hash = HashMix(hash, chunk.chunkIndex);
	hash = HashMix(hash, (uint64_t)(uint32_t)chunk.sectorIndex);
	hash = HashDoubleBits(hash, sec.floorz);
	hash = HashDoubleBits(hash, sec.ceilingz);
	hash = HashMix(hash, (uint16_t)sec.floorstat);
	hash = HashMix(hash, (uint16_t)sec.ceilingstat);
	hash = HashMix(hash, (uint16_t)sec.floorheinum);
	hash = HashMix(hash, (uint16_t)sec.ceilingheinum);
	hash = HashMix(hash, sec.portalflags);
	hash = HashMix(hash, sec.exflags);
	hash = HashMix(hash, sec.floortexture.GetIndex());
	hash = HashMix(hash, sec.ceilingtexture.GetIndex());
	hash = HashFloatBits(hash, sec.floorxpan_);
	hash = HashFloatBits(hash, sec.floorypan_);
	hash = HashFloatBits(hash, sec.ceilingxpan_);
	hash = HashFloatBits(hash, sec.ceilingypan_);
	hash = HashMix(hash, sec.floorpal);
	hash = HashMix(hash, sec.ceilingpal);
	hash = HashMix(hash, (uint8_t)sec.floorshade);
	hash = HashMix(hash, (uint8_t)sec.ceilingshade);

	for (const walltype& wal : sec.walls)
	{
		hash = HashDoubleBits(hash, wal.pos.X);
		hash = HashDoubleBits(hash, wal.pos.Y);
		hash = HashMix(hash, (uint32_t)wal.point2);
		hash = HashMix(hash, (uint32_t)wal.nextwall);
		hash = HashMix(hash, (uint32_t)wal.nextsector);
		hash = HashMix(hash, (uint16_t)wal.cstat);
		hash = HashMix(hash, wal.portalflags);
		hash = HashMix(hash, wal.walltexture.GetIndex());
		hash = HashMix(hash, wal.overtexture.GetIndex());
		hash = HashFloatBits(hash, wal.xpan_);
		hash = HashFloatBits(hash, wal.ypan_);
		hash = HashMix(hash, wal.xrepeat);
		hash = HashMix(hash, wal.yrepeat);
		hash = HashMix(hash, wal.pal);
		hash = HashMix(hash, (uint8_t)wal.shade);
		if (wal.nextsector >= 0 && (unsigned)wal.nextsector < sector.Size())
		{
			const sectortype& adjacent = sector[(unsigned)wal.nextsector];
			hash = HashDoubleBits(hash, adjacent.floorz);
			hash = HashDoubleBits(hash, adjacent.ceilingz);
			hash = HashMix(hash, (uint16_t)adjacent.floorstat);
			hash = HashMix(hash, (uint16_t)adjacent.ceilingstat);
			hash = HashMix(hash, (uint16_t)adjacent.floorheinum);
			hash = HashMix(hash, (uint16_t)adjacent.ceilingheinum);
			hash = HashMix(hash, adjacent.portalflags);
		}
		if (wal.nextwall >= 0 && (unsigned)wal.nextwall < wall.Size())
		{
			const walltype& nextWall = wall[(unsigned)wal.nextwall];
			hash = HashMix(hash, (uint16_t)nextWall.cstat);
			hash = HashMix(hash, nextWall.walltexture.GetIndex());
			hash = HashMix(hash, nextWall.overtexture.GetIndex());
			hash = HashMix(hash, nextWall.pal);
			hash = HashMix(hash, (uint8_t)nextWall.shade);
		}
	}

	return hash;
}

bool CaptureMapChunkMutationBaseline(const PTMapChunk& chunk, PTMapChunkMutationBaseline& outBaseline)
{
	return CaptureChunkMutationBaselineInternal(chunk, outBaseline);
}

bool AnalyzeMapChunkMutation(const PTMapChunk& chunk, const PTMapChunkMutationBaseline& baseline, PTMapChunkMutationAnalysis& outAnalysis)
{
	if (!AnalyzeChunkMutationInternal(chunk, baseline, outAnalysis))
	{
		return false;
	}

	return true;
}
}
