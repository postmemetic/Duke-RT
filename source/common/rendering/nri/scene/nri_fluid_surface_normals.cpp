#include "nri_fluid_surface_normals.h"
#include "nri_fluid_normal_math.h"
#include "nri_fluid_material_binding.h"
#include "nri_scene_surface_types.h"

#include "build.h"
#include "gamefuncs.h"
#include "fluidoverlay.h"

#include <array>

namespace nri_scene
{
namespace
{
	constexpr size_t MaxCornerFan = 32;
	struct Floor
	{
		int32_t sectorIndex = -1;
		uint32_t binding = UINT32_MAX;
		uint32_t palette = 0;
		std::array<double, 3> normal = {};
	};

	double Height(const sectortype& sec, double x, double z)
	{
		double floor = 0.0;
		calcSlope(&sec, x, z, nullptr, &floor);
		return -floor;
	}

	bool ResolveFloor(int32_t index, Floor& result)
	{
		if (index < 0 || (unsigned)index >= sector.Size()) return false;
		const auto& sec = sector[(unsigned)index];
		if (sec.portalflags != 0 || (sec.floorstat & CSTAT_SECTOR_SKY) != 0 || sec.walls.Size() == 0) return false;
		MaterialLightingMetadata metadata;
		metadata.authoredTextureId = sec.floortexture.isValid() ? (uint32_t)sec.floortexture.GetIndex() : UINT32_MAX;
		metadata.sourceType = SurfaceSourceType::MapFloorSection;
		metadata.materialFlags = MaterialFlag_Flat;
		metadata.paletteIndex = sec.floorpal;
		result.binding = ResolveFluidMaterialBindingIndex(GetResolvedFluidOverlaySet(), metadata);
		if (result.binding == UINT32_MAX) return false;
		result.sectorIndex = index;
		result.palette = sec.floorpal;
		const auto& origin = sec.walls[0].pos;
		const double height = Height(sec, origin.X, origin.Y);
		const double dx = (Height(sec, origin.X + 16.0, origin.Y) - height) / 16.0;
		const double dy = (Height(sec, origin.X, origin.Y + 16.0) - height) / 16.0;
		result.normal = FluidCarrierNormalFromBuildHeightDerivatives(dx, dy);
		return std::isfinite(dx) && std::isfinite(dy) && result.normal[1] >= 0.5;
	}

	bool PointOnEdge(double x, double z, const walltype& edge)
	{
		const auto& a = edge.pos;
		const auto& b = edge.point2Wall()->pos;
		const double dx = b.X - a.X, dz = b.Y - a.Y;
		const double lengthSq = dx * dx + dz * dz;
		if (lengthSq <= 1.0e-12) return false;
		const double t = ((x - a.X) * dx + (z - a.Y) * dz) / lengthSq;
		if (t < -1.0e-6 || t > 1.000001) return false;
		const double ex = x - (a.X + t * dx), ez = z - (a.Y + t * dz);
		return ex * ex + ez * ez <= 1.0e-6;
	}

	uint32_t CornerNormal(const Floor& root, double x, double z)
	{
		std::array<Floor, MaxCornerFan> fan = {};
		fan[0] = root;
		size_t fanSize = 1;
		for (size_t cursor = 0; cursor < fanSize; ++cursor)
		{
			const Floor current = fan[cursor];
			const auto& sec = sector[(unsigned)current.sectorIndex];
			for (const auto& edge : sec.walls)
			{
				if (!edge.twoSided() || edge.portalflags != 0 || !PointOnEdge(x, z, edge)) continue;
				if (std::any_of(fan.begin(), fan.begin() + fanSize, [&](const Floor& floor) { return floor.sectorIndex == edge.nextsector; })) continue;
				Floor neighbor;
				if (!ResolveFloor(edge.nextsector, neighbor) || neighbor.binding != root.binding || neighbor.palette != root.palette) continue;
				const auto& adjacent = sector[(unsigned)edge.nextsector];
				const auto& a = edge.pos;
				const auto& b = edge.point2Wall()->pos;
				const double normalDot = current.normal[0] * neighbor.normal[0] +
					current.normal[1] * neighbor.normal[1] + current.normal[2] * neighbor.normal[2];
				if (!FluidFloorEdgeIsContinuous(Height(sec, a.X, a.Y), Height(sec, b.X, b.Y),
					Height(adjacent, a.X, a.Y), Height(adjacent, b.X, b.Y), normalDot)) continue;
				if (fanSize == MaxCornerFan) return UINT32_MAX;
				fan[fanSize++] = neighbor;
			}
		}
		// Sector order makes shared-corner packing deterministic whichever side
		// built the chunk. Count each connected plane once, independent of tessellation.
		std::sort(fan.begin(), fan.begin() + fanSize, [](const Floor& a, const Floor& b) { return a.sectorIndex < b.sectorIndex; });
		std::array<double, 3> sum = {};
		for (size_t i = 0; i < fanSize; ++i)
			for (unsigned axis = 0; axis < 3; ++axis) sum[axis] += fan[i].normal[axis];
		return PackFluidCarrierNormal(sum[0], sum[1], sum[2]);
	}
}

void BuildFluidFloorCarrierNormals(SurfaceRef& surface)
{
	if (surface.provenance.sourceType != SurfaceSourceType::MapFloorSection ||
		(surface.material.flags & (MaterialFlag_Portal | MaterialFlag_Mirror | MaterialFlag_Sky | MaterialFlag_Sprite)) != 0) return;
	Floor floor;
	if (!ResolveFloor(surface.provenance.sectorIndex, floor)) return;
	for (auto& vertex : surface.vertices)
		vertex.fluidCarrierNormal = CornerNormal(floor, vertex.position[0], -vertex.position[2]);
}

uint64_t ComputeFluidFloorNormalSignature(int32_t sectorIndex)
{
	Floor floor;
	if (!ResolveFloor(sectorIndex, floor)) return 0;
	uint64_t signature = 1469598103934665603ull;
	for (const auto& edge : sector[(unsigned)sectorIndex].walls)
	{
		signature = (signature ^ CornerNormal(floor, edge.pos.X, edge.pos.Y)) * 1099511628211ull;
	}
	return signature;
}
}
