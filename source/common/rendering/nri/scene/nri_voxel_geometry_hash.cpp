#include "nri_voxel_geometry_hash.h"

#include "nri_hash.h"
#include "nri_scene_surface_types.h"
#include "nri_voxel_material_slots.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nri_scene
{
namespace
{
	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	uint32_t CountVoxelSurfacePrimitives(const SurfaceRef& surface)
	{
		if (!surface.indices.empty())
		{
			return (uint32_t)(surface.indices.size() / 3u);
		}
		return surface.vertices.size() >= 3 ? (uint32_t)surface.vertices.size() - 2u : 0u;
	}

	uint64_t HashCapturedPosition(uint64_t hash, const CapturedVertex& vertex)
	{
		hash = HashCombine64(hash, (uint64_t)FloatBits(vertex.position[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(vertex.position[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(vertex.position[2]));
		return hash;
	}

	uint64_t HashCapturedVertexPayload(uint64_t hash, const CapturedVertex& vertex)
	{
		hash = HashCapturedPosition(hash, vertex);
		hash = HashCombine64(hash, (uint64_t)FloatBits(vertex.prevPosition[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(vertex.prevPosition[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(vertex.prevPosition[2]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(vertex.uv[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(vertex.uv[1]));
		return hash;
	}

	void ComputeSurfaceNormal(const CapturedVertex& a, const CapturedVertex& b, const CapturedVertex& c, float outNormal[3])
	{
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];

		const float nx = aby * acz - abz * acy;
		const float ny = abz * acx - abx * acz;
		const float nz = abx * acy - aby * acx;
		const float length = (std::max)(0.0001f, std::sqrt(nx * nx + ny * ny + nz * nz));
		outNormal[0] = nx / length;
		outNormal[1] = ny / length;
		outNormal[2] = nz / length;
	}

	bool GetSurfaceTriangleIndices(const SurfaceRef& surface, uint32_t primitiveIndex, uint32_t& outI0, uint32_t& outI1, uint32_t& outI2)
	{
		if (!surface.indices.empty())
		{
			const uint32_t indexOffset = primitiveIndex * 3u;
			if (indexOffset + 2u >= surface.indices.size())
			{
				return false;
			}
			outI0 = surface.indices[indexOffset + 0u];
			outI1 = surface.indices[indexOffset + 1u];
			outI2 = surface.indices[indexOffset + 2u];
		}
		else
		{
			if (primitiveIndex + 2u >= surface.vertices.size())
			{
				return false;
			}
			outI0 = 0u;
			outI1 = primitiveIndex + 1u;
			outI2 = primitiveIndex + 2u;
		}
		return outI0 < surface.vertices.size() &&
			outI1 < surface.vertices.size() &&
			outI2 < surface.vertices.size();
	}
}

uint64_t BuildVoxelGeometryContentHash(const SurfaceRef& surface)
{
	const uint32_t primitiveCount = CountVoxelSurfacePrimitives(surface);
	if (surface.vertices.empty() || primitiveCount == 0)
	{
		return 0ull;
	}

	uint64_t hash = NRIHashFnv1a64OffsetBasis;
	hash = HashCombine64(hash, 0x564f58474f4d3031ull); // VOXGOM01
	hash = HashCombine64(hash, (uint64_t)surface.vertices.size());
	hash = HashCombine64(hash, (uint64_t)primitiveCount);
	hash = HashCombine64(hash, surface.indices.empty() ? 0ull : (uint64_t)surface.indices.size());
	for (const CapturedVertex& vertex : surface.vertices)
	{
		hash = HashCapturedPosition(hash, vertex);
	}

	for (uint32_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		uint32_t i0 = 0;
		uint32_t i1 = 0;
		uint32_t i2 = 0;
		if (!GetSurfaceTriangleIndices(surface, primitiveIndex, i0, i1, i2))
		{
			return 0ull;
		}
		hash = HashCombine64(hash, (uint64_t)i0);
		hash = HashCombine64(hash, (uint64_t)i1);
		hash = HashCombine64(hash, (uint64_t)i2);
	}
	return hash != 0ull ? hash : 1ull;
}

uint64_t BuildVoxelRenderPrimitiveHash(const SurfaceRef& surface)
{
	const uint32_t primitiveCount = CountVoxelSurfacePrimitives(surface);
	if (surface.vertices.empty() || primitiveCount == 0)
	{
		return 0ull;
	}

	uint64_t hash = NRIHashFnv1a64OffsetBasis;
	hash = HashCombine64(hash, 0x564f585052494d32ull); // VOXPRIM2
	hash = HashCombine64(hash, (uint64_t)surface.vertices.size());
	hash = HashCombine64(hash, (uint64_t)primitiveCount);
	hash = HashCombine64(hash, surface.indices.empty() ? 0ull : (uint64_t)surface.indices.size());
	hash = HashCombine64(hash, (uint64_t)surface.material.flags);
	for (const CapturedVertex& vertex : surface.vertices)
	{
		hash = HashCapturedVertexPayload(hash, vertex);
	}

	for (uint32_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		uint32_t i0 = 0;
		uint32_t i1 = 0;
		uint32_t i2 = 0;
		if (!GetSurfaceTriangleIndices(surface, primitiveIndex, i0, i1, i2))
		{
			return 0ull;
		}
		const CapturedVertex& v0 = surface.vertices[i0];
		const CapturedVertex& v1 = surface.vertices[i1];
		const CapturedVertex& v2 = surface.vertices[i2];
		float normal[3] = {};
		ComputeSurfaceNormal(v0, v1, v2, normal);

		hash = HashCombine64(hash, (uint64_t)i0);
		hash = HashCombine64(hash, (uint64_t)i1);
		hash = HashCombine64(hash, (uint64_t)i2);
		const uint32_t localMaterialSlot = primitiveIndex < surface.primitiveLocalMaterialSlots.size() ?
			surface.primitiveLocalMaterialSlots[primitiveIndex] : 0u;
		hash = HashCombine64(hash, (uint64_t)localMaterialSlot);
		hash = HashCombine64(hash, (uint64_t)FloatBits(v0.uv[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(v0.uv[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(v1.uv[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(v1.uv[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(v2.uv[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(v2.uv[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(normal[0]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(normal[1]));
		hash = HashCombine64(hash, (uint64_t)FloatBits(normal[2]));
	}
	return hash != 0ull ? hash : 1ull;
}

VoxelGeometryContentHashes BuildVoxelGeometryContentHashes(const SurfaceRef& surface)
{
	VoxelGeometryContentHashes hashes = {};
	hashes.geometryContentHash = BuildVoxelGeometryContentHash(surface);
	hashes.renderPrimitiveHash = BuildVoxelRenderPrimitiveHash(surface);
	return hashes;
}
}
