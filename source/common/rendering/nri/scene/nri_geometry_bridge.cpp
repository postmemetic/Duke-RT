#include "nri_geometry_bridge.h"
#include "nri_voxel_material_slots.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace
{
	using namespace nri_scene;

	double DurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	void NoteCapacityGrowth(size_t requiredSize, size_t capacity, uint32_t& growthCount)
	{
		if (requiredSize > capacity)
		{
			growthCount++;
		}
	}

	SceneVertex MakeVertex(const FFlatVertex& source)
	{
		SceneVertex vertex = {};
		vertex.position[0] = source.x;
		vertex.position[1] = source.z;
		vertex.position[2] = source.y;
		vertex.prevPosition[0] = vertex.position[0];
		vertex.prevPosition[1] = vertex.position[1];
		vertex.prevPosition[2] = vertex.position[2];
		vertex.uv[0] = source.u;
		vertex.uv[1] = source.v;
		return vertex;
	}

	SceneVertex MakeVertex(const CapturedVertex& source)
	{
		SceneVertex vertex = {};
		vertex.position[0] = source.position[0];
		vertex.position[1] = source.position[1];
		vertex.position[2] = source.position[2];
		vertex.prevPosition[0] = source.prevPosition[0];
		vertex.prevPosition[1] = source.prevPosition[1];
		vertex.prevPosition[2] = source.prevPosition[2];
		vertex.uv[0] = source.uv[0];
		vertex.uv[1] = source.uv[1];
		return vertex;
	}

	void ComputeNormal(const SceneVertex& a, const SceneVertex& b, const SceneVertex& c, float outNormal[3])
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
		const float length = std::max(0.0001f, sqrtf(nx * nx + ny * ny + nz * nz));

		outNormal[0] = nx / length;
		outNormal[1] = ny / length;
		outNormal[2] = nz / length;
	}

	struct VoxelVertexPosition
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;

		bool operator==(const VoxelVertexPosition& other) const
		{
			return x == other.x && y == other.y && z == other.z;
		}
	};

	struct VoxelVertexPositionHash
	{
		size_t operator()(const VoxelVertexPosition& position) const
		{
			size_t hash = std::hash<float>{}(position.x);
			hash ^= std::hash<float>{}(position.y) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
			hash ^= std::hash<float>{}(position.z) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
			return hash;
		}
	};

	struct VoxelNormalSum
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	VoxelVertexPosition GetVoxelVertexPosition(const CapturedVertex& vertex)
	{
		return { vertex.position[0], vertex.position[1], vertex.position[2] };
	}

	uint32_t PackVoxelNormal(float x, float y, float z)
	{
		const float lengthSq = x * x + y * y + z * z;
		if (lengthSq <= 1.0e-12f)
		{
			return 0;
		}

		const float invL1Length = 1.0f / (std::abs(x) + std::abs(y) + std::abs(z));
		float octX = x * invL1Length;
		float octY = y * invL1Length;
		if (z < 0.0f)
		{
			const float oldX = octX;
			octX = (1.0f - std::abs(octY)) * (oldX < 0.0f ? -1.0f : 1.0f);
			octY = (1.0f - std::abs(oldX)) * (octY < 0.0f ? -1.0f : 1.0f);
		}
		const uint32_t packedX = (uint32_t)std::lround((std::clamp(octX, -1.0f, 1.0f) * 0.5f + 0.5f) * 255.0f);
		const uint32_t packedY = (uint32_t)std::lround((std::clamp(octY, -1.0f, 1.0f) * 0.5f + 0.5f) * 255.0f);
		return packedX | (packedY << 8u);
	}

	float VoxelCornerAngle(const CapturedVertex& center, const CapturedVertex& a, const CapturedVertex& b)
	{
		float ax = a.position[0] - center.position[0];
		float ay = a.position[1] - center.position[1];
		float az = a.position[2] - center.position[2];
		float bx = b.position[0] - center.position[0];
		float by = b.position[1] - center.position[1];
		float bz = b.position[2] - center.position[2];
		const float aLengthSq = ax * ax + ay * ay + az * az;
		const float bLengthSq = bx * bx + by * by + bz * bz;
		if (aLengthSq <= 1.0e-12f || bLengthSq <= 1.0e-12f)
		{
			return 0.0f;
		}
		const float invLengths = 1.0f / std::sqrt(aLengthSq * bLengthSq);
		return std::acos(std::clamp((ax * bx + ay * by + az * bz) * invLengths, -1.0f, 1.0f));
	}

	void BuildVoxelSmoothNormals(const SurfaceRef& surface, std::vector<uint32_t>& outPackedNormals)
	{
		outPackedNormals.assign(surface.vertices.size(), 0u);
		std::unordered_map<VoxelVertexPosition, VoxelNormalSum, VoxelVertexPositionHash> normalSums;
		normalSums.reserve(surface.vertices.size());
		for (uint32_t i = 0; i + 2u < surface.indices.size(); i += 3u)
		{
			const uint32_t indices[3] = { surface.indices[i], surface.indices[i + 1u], surface.indices[i + 2u] };
			if (indices[0] >= surface.vertices.size() || indices[1] >= surface.vertices.size() || indices[2] >= surface.vertices.size())
			{
				continue;
			}

			const CapturedVertex& v0 = surface.vertices[indices[0]];
			const CapturedVertex& v1 = surface.vertices[indices[1]];
			const CapturedVertex& v2 = surface.vertices[indices[2]];
			const float abx = v1.position[0] - v0.position[0];
			const float aby = v1.position[1] - v0.position[1];
			const float abz = v1.position[2] - v0.position[2];
			const float acx = v2.position[0] - v0.position[0];
			const float acy = v2.position[1] - v0.position[1];
			const float acz = v2.position[2] - v0.position[2];
			float nx = aby * acz - abz * acy;
			float ny = abz * acx - abx * acz;
			float nz = abx * acy - aby * acx;
			const float normalLengthSq = nx * nx + ny * ny + nz * nz;
			if (normalLengthSq <= 1.0e-12f)
			{
				continue;
			}
			const float invNormalLength = 1.0f / std::sqrt(normalLengthSq);
			nx *= invNormalLength;
			ny *= invNormalLength;
			nz *= invNormalLength;

			const float angles[3] = {
				VoxelCornerAngle(v0, v1, v2),
				VoxelCornerAngle(v1, v2, v0),
				VoxelCornerAngle(v2, v0, v1)
			};
			for (uint32_t corner = 0; corner < 3u; ++corner)
			{
				VoxelNormalSum& sum = normalSums[GetVoxelVertexPosition(surface.vertices[indices[corner]])];
				sum.x += nx * angles[corner];
				sum.y += ny * angles[corner];
				sum.z += nz * angles[corner];
			}
		}

		for (uint32_t i = 0; i < surface.vertices.size(); ++i)
		{
			const auto found = normalSums.find(GetVoxelVertexPosition(surface.vertices[i]));
			if (found != normalSums.end())
			{
				outPackedNormals[i] = PackVoxelNormal(found->second.x, found->second.y, found->second.z);
			}
		}
	}

	bool ShouldFlipFlatNormal(uint32_t flags, const SurfaceProvenance& provenance, const float normal[3])
	{
		if ((flags & MaterialFlag_Flat) == 0)
		{
			return false;
		}

		switch (provenance.sourceType)
		{
		case SurfaceSourceType::FloorFlat:
		case SurfaceSourceType::MapFloorSection:
			return normal[1] < 0.0f;

		case SurfaceSourceType::CeilingFlat:
		case SurfaceSourceType::MapCeilingSection:
			return normal[1] > 0.0f;

		default:
			return false;
		}
	}

	void AppendTriangle(const SceneVertex& v0, const SceneVertex& v1, const SceneVertex& v2, uint32_t materialIndex, uint32_t flags, const SurfaceProvenance& provenance, GeometryData& outGeometry, GeometryBuildTraceStats* traceStats)
	{
		const uint32_t vertexBase = (uint32_t)outGeometry.vertices.size();
		if (traceStats != nullptr)
		{
			NoteCapacityGrowth(outGeometry.vertices.size() + 3u, outGeometry.vertices.capacity(), traceStats->vertexCapacityGrowths);
			NoteCapacityGrowth(outGeometry.indices.size() + 3u, outGeometry.indices.capacity(), traceStats->indexCapacityGrowths);
			NoteCapacityGrowth(outGeometry.primitives.size() + 1u, outGeometry.primitives.capacity(), traceStats->primitiveCapacityGrowths);
			NoteCapacityGrowth(outGeometry.primitiveProvenance.size() + 1u, outGeometry.primitiveProvenance.capacity(), traceStats->provenanceCapacityGrowths);
		}
		outGeometry.vertices.push_back(v0);
		outGeometry.vertices.push_back(v1);
		outGeometry.vertices.push_back(v2);

		outGeometry.indices.push_back(vertexBase + 0);
		outGeometry.indices.push_back(vertexBase + 1);
		outGeometry.indices.push_back(vertexBase + 2);

		PrimitiveData primitive = {};
		primitive.indices[0] = vertexBase + 0;
		primitive.indices[1] = vertexBase + 1;
		primitive.indices[2] = vertexBase + 2;
		primitive.materialIndex = materialIndex;
		primitive.uv0[0] = v0.uv[0];
		primitive.uv0[1] = v0.uv[1];
		primitive.uv1[0] = v1.uv[0];
		primitive.uv1[1] = v1.uv[1];
		primitive.uv2[0] = v2.uv[0];
		primitive.uv2[1] = v2.uv[1];
		primitive.flags = flags;
		primitive.portalIndex = UINT32_MAX;
		ComputeNormal(v0, v1, v2, primitive.normal);
		if (ShouldFlipFlatNormal(flags, provenance, primitive.normal))
		{
			primitive.normal[0] = -primitive.normal[0];
			primitive.normal[1] = -primitive.normal[1];
			primitive.normal[2] = -primitive.normal[2];
		}
		outGeometry.primitives.push_back(primitive);
		outGeometry.primitiveProvenance.push_back(provenance);
	}

	bool AppendIndexedSurface(const SurfaceRef& surface, uint32_t materialBase, uint32_t flags, GeometryData& outGeometry, GeometryBuildTraceStats* traceStats)
	{
		if (surface.indices.size() < 3)
		{
			return false;
		}

		const uint32_t vertexBase = (uint32_t)outGeometry.vertices.size();
		if (traceStats != nullptr)
		{
			traceStats->indexedSurfaces++;
			NoteCapacityGrowth(outGeometry.vertices.size() + surface.vertices.size(), outGeometry.vertices.capacity(), traceStats->vertexCapacityGrowths);
		}
		outGeometry.vertices.reserve(outGeometry.vertices.size() + surface.vertices.size());
		for (const CapturedVertex& vertex : surface.vertices)
		{
			outGeometry.vertices.push_back(MakeVertex(vertex));
		}
		std::vector<uint32_t> voxelSmoothNormals;
		if (surface.provenance.sourceType == SurfaceSourceType::VoxelProxySprite)
		{
			BuildVoxelSmoothNormals(surface, voxelSmoothNormals);
		}

		if (traceStats != nullptr)
		{
			NoteCapacityGrowth(outGeometry.indices.size() + surface.indices.size(), outGeometry.indices.capacity(), traceStats->indexCapacityGrowths);
			NoteCapacityGrowth(outGeometry.primitives.size() + surface.indices.size() / 3u, outGeometry.primitives.capacity(), traceStats->primitiveCapacityGrowths);
			NoteCapacityGrowth(outGeometry.primitiveProvenance.size() + surface.indices.size() / 3u, outGeometry.primitiveProvenance.capacity(), traceStats->provenanceCapacityGrowths);
		}
		outGeometry.indices.reserve(outGeometry.indices.size() + surface.indices.size());
		outGeometry.primitives.reserve(outGeometry.primitives.size() + surface.indices.size() / 3u);
		outGeometry.primitiveProvenance.reserve(outGeometry.primitiveProvenance.size() + surface.indices.size() / 3u);
		for (uint32_t i = 0; i + 2 < surface.indices.size(); i += 3)
		{
			const uint32_t primitiveIndex = i / 3u;
			const uint32_t i0 = surface.indices[i + 0];
			const uint32_t i1 = surface.indices[i + 1];
			const uint32_t i2 = surface.indices[i + 2];
			if (i0 >= surface.vertices.size() || i1 >= surface.vertices.size() || i2 >= surface.vertices.size())
			{
				continue;
			}

			const uint32_t gi0 = vertexBase + i0;
			const uint32_t gi1 = vertexBase + i1;
			const uint32_t gi2 = vertexBase + i2;
			outGeometry.indices.push_back(gi0);
			outGeometry.indices.push_back(gi1);
			outGeometry.indices.push_back(gi2);

			const SceneVertex& v0 = outGeometry.vertices[gi0];
			const SceneVertex& v1 = outGeometry.vertices[gi1];
			const SceneVertex& v2 = outGeometry.vertices[gi2];

			PrimitiveData primitive = {};
			primitive.indices[0] = gi0;
			primitive.indices[1] = gi1;
			primitive.indices[2] = gi2;
			const uint32_t localMaterialSlot = primitiveIndex < surface.primitiveLocalMaterialSlots.size() ?
				surface.primitiveLocalMaterialSlots[primitiveIndex] : 0u;
			primitive.materialIndex = ResolveVoxelPrimitiveMaterialIndex(
				materialBase,
				surface.materialRowSpan,
				localMaterialSlot);
			primitive.uv0[0] = v0.uv[0];
			primitive.uv0[1] = v0.uv[1];
			primitive.uv1[0] = v1.uv[0];
			primitive.uv1[1] = v1.uv[1];
			primitive.uv2[0] = v2.uv[0];
			primitive.uv2[1] = v2.uv[1];
			primitive.flags = flags;
			primitive.portalIndex = UINT32_MAX;
			ComputeNormal(v0, v1, v2, primitive.normal);
			if (ShouldFlipFlatNormal(flags, surface.provenance, primitive.normal))
			{
				primitive.normal[0] = -primitive.normal[0];
				primitive.normal[1] = -primitive.normal[1];
				primitive.normal[2] = -primitive.normal[2];
			}
			if (!voxelSmoothNormals.empty())
			{
				const uint32_t faceNormal = PackVoxelNormal(primitive.normal[0], primitive.normal[1], primitive.normal[2]);
				const uint32_t n0 = voxelSmoothNormals[i0] != 0u ? voxelSmoothNormals[i0] : faceNormal;
				const uint32_t n1 = voxelSmoothNormals[i1] != 0u ? voxelSmoothNormals[i1] : faceNormal;
				const uint32_t n2 = voxelSmoothNormals[i2] != 0u ? voxelSmoothNormals[i2] : faceNormal;
				primitive.smoothNormals[0] = n0 | (n1 << 16u);
				primitive.smoothNormals[1] = n2 | 0x80000000u;
			}
			outGeometry.primitives.push_back(primitive);
			outGeometry.primitiveProvenance.push_back(surface.provenance);
		}
		return true;
	}
}

namespace nri_scene
{
void ClearGeometryRetainingCapacity(GeometryData& geometry)
{
	geometry.vertices.clear();
	geometry.indices.clear();
	geometry.primitives.clear();
	geometry.primitiveProvenance.clear();
}

void BuildGeometry(const SceneView& sceneView, GeometryData& outGeometry, GeometryBuildTraceStats* traceStats, bool retainOutputCapacity)
{
	if (retainOutputCapacity)
	{
		ClearGeometryRetainingCapacity(outGeometry);
	}
	else
	{
		outGeometry = {};
	}
	if (traceStats != nullptr)
	{
		*traceStats = {};
	}
	const uint32_t scenePrimitiveFlags = sceneView.primitiveFlags;

	uint32_t materialIndex = 0;
	{
		const auto phaseStart = traceStats != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
		for (const SurfaceRef& wall : sceneView.opaqueWalls)
		{
			if (traceStats != nullptr)
			{
				traceStats->wallSurfaces++;
				traceStats->sourceVertexCount += (uint32_t)wall.vertices.size();
				traceStats->sourceIndexCount += (uint32_t)wall.indices.size();
			}
			if (wall.vertices.size() < 3)
			{
				if (traceStats != nullptr)
				{
					traceStats->skippedSurfaces++;
				}
				materialIndex += NormalizeVoxelMaterialRowSpan(wall.materialRowSpan);
				continue;
			}

			if (AppendIndexedSurface(wall, materialIndex, wall.material.flags | scenePrimitiveFlags, outGeometry, traceStats))
			{
				materialIndex += NormalizeVoxelMaterialRowSpan(wall.materialRowSpan);
				continue;
			}

			if (traceStats != nullptr)
			{
				traceStats->triangleFanSurfaces++;
			}
			SceneVertex root = MakeVertex(wall.vertices[0]);
			for (uint32_t i = 1; i + 1 < wall.vertices.size(); ++i)
			{
				AppendTriangle(root, MakeVertex(wall.vertices[i]), MakeVertex(wall.vertices[i + 1]), materialIndex, wall.material.flags | scenePrimitiveFlags, wall.provenance, outGeometry, traceStats);
			}

			materialIndex += NormalizeVoxelMaterialRowSpan(wall.materialRowSpan);
		}
		if (traceStats != nullptr)
		{
			traceStats->wallMs += DurationMs(phaseStart, std::chrono::steady_clock::now());
		}
	}

	{
		const auto phaseStart = traceStats != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
		for (const SurfaceRef& flat : sceneView.opaqueFlats)
		{
			if (traceStats != nullptr)
			{
				traceStats->flatSurfaces++;
				traceStats->sourceVertexCount += (uint32_t)flat.vertices.size();
				traceStats->sourceIndexCount += (uint32_t)flat.indices.size();
			}
			if (flat.vertices.size() < 3)
			{
				if (traceStats != nullptr)
				{
					traceStats->skippedSurfaces++;
				}
				materialIndex += NormalizeVoxelMaterialRowSpan(flat.materialRowSpan);
				continue;
			}

			if (AppendIndexedSurface(flat, materialIndex, flat.material.flags | scenePrimitiveFlags, outGeometry, traceStats))
			{
				materialIndex += NormalizeVoxelMaterialRowSpan(flat.materialRowSpan);
				continue;
			}

			if (traceStats != nullptr)
			{
				traceStats->triangleFanSurfaces++;
			}
			for (uint32_t i = 0; i + 2 < flat.vertices.size(); i += 3)
			{
				AppendTriangle(MakeVertex(flat.vertices[i]), MakeVertex(flat.vertices[i + 1]), MakeVertex(flat.vertices[i + 2]), materialIndex, flat.material.flags | scenePrimitiveFlags, flat.provenance, outGeometry, traceStats);
			}

			materialIndex += NormalizeVoxelMaterialRowSpan(flat.materialRowSpan);
		}
		if (traceStats != nullptr)
		{
			traceStats->flatMs += DurationMs(phaseStart, std::chrono::steady_clock::now());
		}
	}

	{
		const auto phaseStart = traceStats != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
		for (const SurfaceRef& sprite : sceneView.opaqueSprites)
		{
			if (traceStats != nullptr)
			{
				traceStats->spriteSurfaces++;
				traceStats->sourceVertexCount += (uint32_t)sprite.vertices.size();
				traceStats->sourceIndexCount += (uint32_t)sprite.indices.size();
			}
			if (sprite.vertices.size() < 3)
			{
				if (traceStats != nullptr)
				{
					traceStats->skippedSurfaces++;
				}
				materialIndex += NormalizeVoxelMaterialRowSpan(sprite.materialRowSpan);
				continue;
			}

			if (AppendIndexedSurface(sprite, materialIndex, sprite.material.flags | scenePrimitiveFlags, outGeometry, traceStats))
			{
				materialIndex += NormalizeVoxelMaterialRowSpan(sprite.materialRowSpan);
				continue;
			}

			if (sprite.vertices.size() == 4)
			{
				if (traceStats != nullptr)
				{
					traceStats->spriteStripSurfaces++;
				}
				// Facing sprites come from HWSprite as a 4-vertex triangle strip.
				AppendTriangle(MakeVertex(sprite.vertices[0]), MakeVertex(sprite.vertices[1]), MakeVertex(sprite.vertices[2]), materialIndex, sprite.material.flags | scenePrimitiveFlags, sprite.provenance, outGeometry, traceStats);
				AppendTriangle(MakeVertex(sprite.vertices[2]), MakeVertex(sprite.vertices[1]), MakeVertex(sprite.vertices[3]), materialIndex, sprite.material.flags | scenePrimitiveFlags, sprite.provenance, outGeometry, traceStats);
				materialIndex += NormalizeVoxelMaterialRowSpan(sprite.materialRowSpan);
				continue;
			}

			if (traceStats != nullptr)
			{
				traceStats->triangleFanSurfaces++;
			}
			SceneVertex root = MakeVertex(sprite.vertices[0]);
			for (uint32_t i = 1; i + 1 < sprite.vertices.size(); ++i)
			{
				AppendTriangle(root, MakeVertex(sprite.vertices[i]), MakeVertex(sprite.vertices[i + 1]), materialIndex, sprite.material.flags | scenePrimitiveFlags, sprite.provenance, outGeometry, traceStats);
			}

			materialIndex += NormalizeVoxelMaterialRowSpan(sprite.materialRowSpan);
		}
		if (traceStats != nullptr)
		{
			traceStats->spriteMs += DurationMs(phaseStart, std::chrono::steady_clock::now());
		}
	}
	if (traceStats != nullptr)
	{
		traceStats->outputVertexCount = (uint32_t)outGeometry.vertices.size();
		traceStats->outputIndexCount = (uint32_t)outGeometry.indices.size();
		traceStats->outputPrimitiveCount = (uint32_t)outGeometry.primitives.size();
	}
}
}
