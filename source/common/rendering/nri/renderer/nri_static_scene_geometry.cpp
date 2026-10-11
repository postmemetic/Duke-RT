#include "nri_static_scene_geometry.h"

#include "../scene/nri_hash.h"
#include "nri_resources.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace nri_static_scene_geometry
{
namespace
{
	uint64_t Combine(uint64_t hash, uint64_t value)
	{
		return nri_scene::HashCombine64(hash, value);
	}

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	uint64_t HashResidentGeometryVertexPayload(uint64_t hash, const nri_scene::SceneVertex& vertex)
	{
		for (float component : vertex.position)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		for (float component : vertex.prevPosition)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		for (float component : vertex.uv)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		return hash;
	}

	uint64_t HashResidentGeometryProvenancePayload(
		uint64_t hash,
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::GeometryData& geometry,
		const nri_scene::PrimitiveData& primitive,
		uint32_t primitiveIndex)
	{
		const bool hasProvenance = primitiveIndex < geometry.primitiveProvenance.size();
		const uint32_t visibilityChunk =
			hasProvenance ?
			(uint32_t)ResolveVisibilityChunkIndexForProvenance(mapWorld, geometry.primitiveProvenance[primitiveIndex]) :
			primitive.reserved0;
		hash = Combine(hash, (uint64_t)visibilityChunk);
		if (hasProvenance)
		{
			const auto& provenance = geometry.primitiveProvenance[primitiveIndex];
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.sourceType);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.sectorIndex);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.wallIndex);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.sectionIndex);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.mapChunkIndex);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.nextSectorIndex);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.actorIndex);
			hash = Combine(hash, (uint64_t)provenance.drawListType);
			hash = Combine(hash, (uint64_t)provenance.cstat);
			hash = Combine(hash, (uint64_t)provenance.materialFlags);
		}
		else
		{
			hash = Combine(hash, UINT64_MAX);
		}
		return hash;
	}
}

int32_t FindMapChunkIndexForSector(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex)
{
	if (!mapWorld.valid || sectorIndex < 0)
	{
		return -1;
	}
	if ((size_t)sectorIndex < mapWorld.sectorChunkLookup.size())
	{
		const uint32_t chunkIndex = mapWorld.sectorChunkLookup[(size_t)sectorIndex];
		if (chunkIndex != UINT32_MAX)
		{
			return (int32_t)chunkIndex;
		}
	}

	for (const auto& chunk : mapWorld.chunks)
	{
		if (chunk.kind == nri_scene::PTMapChunkKind::Sector && chunk.sectorIndex == sectorIndex)
		{
			return (int32_t)chunk.chunkIndex;
		}
	}

	return -1;
}

int32_t ResolveVisibilityChunkIndexForProvenance(const nri_scene::PTMapWorld& mapWorld, const nri_scene::SurfaceProvenance& provenance)
{
	if (provenance.mapChunkIndex >= 0)
	{
		return provenance.mapChunkIndex;
	}
	return FindMapChunkIndexForSector(mapWorld, provenance.sectorIndex);
}

uint32_t NormalizeResidentAtlasIndex(uint32_t value, uint32_t base)
{
	return value >= base ? value - base : UINT32_MAX;
}

uint64_t HashResidentGeometryPayload(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::GeometryData& geometry,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t materialOffset,
	uint32_t materialCount)
{
	if (vertexOffset + vertexCount > geometry.vertices.size() ||
		indexOffset + indexCount > geometry.indices.size() ||
		primitiveOffset + primitiveCount > geometry.primitives.size())
	{
		return 0;
	}

	uint64_t hash = 1469598103934665603ull;
	hash = Combine(hash, (uint64_t)vertexCount);
	hash = Combine(hash, (uint64_t)indexCount);
	hash = Combine(hash, (uint64_t)primitiveCount);
	hash = Combine(hash, (uint64_t)materialCount);
	for (uint32_t i = 0; i < vertexCount; ++i)
	{
		const auto& vertex = geometry.vertices[vertexOffset + i];
		hash = HashResidentGeometryVertexPayload(hash, vertex);
	}

	for (uint32_t i = 0; i < indexCount; ++i)
	{
		hash = Combine(
			hash,
			(uint64_t)NormalizeResidentAtlasIndex(geometry.indices[indexOffset + i], vertexOffset));
	}

	for (uint32_t i = 0; i < primitiveCount; ++i)
	{
		const uint32_t primitiveIndex = primitiveOffset + i;
		const auto& primitive = geometry.primitives[primitiveIndex];
		hash = Combine(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.indices[0], vertexOffset));
		hash = Combine(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.indices[1], vertexOffset));
		hash = Combine(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.indices[2], vertexOffset));
		hash = Combine(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.materialIndex, materialOffset));
		for (float component : primitive.uv0)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		for (float component : primitive.uv1)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		for (float component : primitive.uv2)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		for (float component : primitive.normal)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		hash = Combine(hash, (uint64_t)primitive.flags);
		hash = Combine(hash, (uint64_t)primitive.portalIndex);
		hash = HashResidentGeometryProvenancePayload(hash, mapWorld, geometry, primitive, primitiveIndex);
	}

	return hash != 0 ? hash : 1;
}

uint64_t HashResidentGeometryPayloadOrderIndependent(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::GeometryData& geometry,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t materialOffset,
	uint32_t materialCount)
{
	if (vertexOffset + vertexCount > geometry.vertices.size() ||
		primitiveOffset + primitiveCount > geometry.primitives.size())
	{
		return 0;
	}

	std::vector<uint64_t> primitiveHashes;
	primitiveHashes.reserve(primitiveCount);
	for (uint32_t i = 0; i < primitiveCount; ++i)
	{
		const uint32_t primitiveIndex = primitiveOffset + i;
		const auto& primitive = geometry.primitives[primitiveIndex];
		uint64_t primitiveHash = 1469598103934665603ull;
		primitiveHash = Combine(primitiveHash, (uint64_t)NormalizeResidentAtlasIndex(primitive.materialIndex, materialOffset));
		for (float component : primitive.normal)
		{
			primitiveHash = Combine(primitiveHash, (uint64_t)FloatBits(component));
		}
		primitiveHash = Combine(primitiveHash, (uint64_t)primitive.flags);
		primitiveHash = Combine(primitiveHash, (uint64_t)primitive.portalIndex);
		primitiveHash = HashResidentGeometryProvenancePayload(primitiveHash, mapWorld, geometry, primitive, primitiveIndex);

		const float* primitiveUvs[3] = { primitive.uv0, primitive.uv1, primitive.uv2 };
		for (uint32_t corner = 0; corner < 3; ++corner)
		{
			const uint32_t vertexIndex = primitive.indices[corner];
			if (vertexIndex < vertexOffset || vertexIndex >= vertexOffset + vertexCount)
			{
				return 0;
			}
			primitiveHash = HashResidentGeometryVertexPayload(primitiveHash, geometry.vertices[vertexIndex]);
			for (uint32_t component = 0; component < 2; ++component)
			{
				primitiveHash = Combine(primitiveHash, (uint64_t)FloatBits(primitiveUvs[corner][component]));
			}
		}
		primitiveHashes.push_back(primitiveHash);
	}

	std::sort(primitiveHashes.begin(), primitiveHashes.end());
	uint64_t hash = 1469598103934665603ull;
	hash = Combine(hash, (uint64_t)vertexCount);
	hash = Combine(hash, (uint64_t)primitiveCount);
	hash = Combine(hash, (uint64_t)materialCount);
	for (uint64_t primitiveHash : primitiveHashes)
	{
		hash = Combine(hash, primitiveHash);
	}
	return hash != 0 ? hash : 1;
}

uint64_t ComputeGeometryTopologySignature(const nri_scene::GeometryData& geometry)
{
	uint64_t hash = 1469598103934665603ull;
	hash = Combine(hash, (uint64_t)geometry.vertices.size());
	hash = Combine(hash, (uint64_t)geometry.indices.size());
	hash = Combine(hash, (uint64_t)geometry.primitives.size());
	for (uint32_t index : geometry.indices)
	{
		hash = Combine(hash, (uint64_t)index);
	}
	return hash;
}

uint64_t ComputePrimitiveLayoutSignature(const nri_scene::GeometryData& geometry)
{
	uint64_t hash = 1469598103934665603ull;
	hash = Combine(hash, (uint64_t)geometry.primitives.size());
	hash = Combine(hash, (uint64_t)geometry.primitiveProvenance.size());
	for (size_t i = 0; i < geometry.primitives.size(); ++i)
	{
		const nri_scene::PrimitiveData& primitive = geometry.primitives[i];
		hash = Combine(hash, (uint64_t)primitive.indices[0]);
		hash = Combine(hash, (uint64_t)primitive.indices[1]);
		hash = Combine(hash, (uint64_t)primitive.indices[2]);
		hash = Combine(hash, (uint64_t)primitive.materialIndex);
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv0[0]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv0[1]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv1[0]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv1[1]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv2[0]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv2[1]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.normal[0]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.normal[1]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.normal[2]));
		hash = Combine(hash, (uint64_t)primitive.flags);
		hash = Combine(hash, (uint64_t)primitive.portalIndex);
		if (i < geometry.primitiveProvenance.size())
		{
			const nri_scene::SurfaceProvenance& provenance = geometry.primitiveProvenance[i];
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.sourceType);
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.sectorIndex + 1));
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.wallIndex + 1));
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.sectionIndex + 1));
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.mapChunkIndex + 1));
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.nextSectorIndex + 1));
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.actorIndex + 1));
			hash = Combine(hash, (uint64_t)provenance.drawListType);
			hash = Combine(hash, (uint64_t)provenance.cstat);
			hash = Combine(hash, (uint64_t)provenance.materialFlags);
		}
	}
	return hash;
}

void ResetStaticMapChunkAtlas(StaticMapChunkAtlas& atlas)
{
	atlas = {};
}

uint32_t GetChunkAtlasCapacity(uint32_t usedCount)
{
	if (usedCount == 0)
	{
		return 0;
	}

	const uint64_t reserveFloor = std::max<uint64_t>(usedCount + 64u, usedCount * 2ull);
	const uint64_t grown = GetNRIGrownBufferSize((uint64_t)usedCount, reserveFloor, 1u);
	return (uint32_t)std::min<uint64_t>(grown, (uint64_t)UINT32_MAX);
}

uint32_t AllocateChunkAtlasSlice(uint32_t count, uint32_t alignment, uint32_t& cursor)
{
	if (alignment == 0)
	{
		alignment = 1;
	}

	const uint32_t alignedCursor =
		cursor % alignment == 0 ?
		cursor :
		cursor + (alignment - cursor % alignment);
	cursor = alignedCursor + count;
	return alignedCursor;
}

uint32_t AllocateChunkAtlasRange(uint32_t count, uint32_t capacity, std::vector<StaticMapChunkAtlas::FreeRange>& freeRanges, uint32_t& cursor)
{
	if (count == 0)
	{
		return 0;
	}

	for (size_t i = 0; i < freeRanges.size(); ++i)
	{
		auto& range = freeRanges[i];
		if (range.count < count)
		{
			continue;
		}

		const uint32_t offset = range.offset;
		range.offset += count;
		range.count -= count;
		if (range.count == 0)
		{
			freeRanges.erase(freeRanges.begin() + i);
		}
		return offset;
	}

	if (cursor > capacity || count > capacity - cursor)
	{
		return UINT32_MAX;
	}

	const uint32_t offset = cursor;
	cursor += count;
	return offset;
}

void ReleaseChunkAtlasRange(std::vector<StaticMapChunkAtlas::FreeRange>& freeRanges, uint32_t offset, uint32_t count)
{
	if (count == 0)
	{
		return;
	}

	freeRanges.push_back({ offset, count });
	std::sort(
		freeRanges.begin(),
		freeRanges.end(),
		[](const StaticMapChunkAtlas::FreeRange& a, const StaticMapChunkAtlas::FreeRange& b)
		{
			return a.offset < b.offset;
		});

	size_t writeIndex = 0;
	for (size_t i = 0; i < freeRanges.size(); ++i)
	{
		if (writeIndex == 0)
		{
			freeRanges[writeIndex++] = freeRanges[i];
			continue;
		}

		auto& previous = freeRanges[writeIndex - 1];
		const auto& current = freeRanges[i];
		if (previous.offset + previous.count >= current.offset)
		{
			const uint32_t end = std::max(previous.offset + previous.count, current.offset + current.count);
			previous.count = end - previous.offset;
			continue;
		}

		freeRanges[writeIndex++] = current;
	}

	freeRanges.resize(writeIndex);
}

bool BuildStaticMapChunkAtlasLayout(const StaticMapSceneCache& staticScene, StaticMapChunkAtlas& outAtlas)
{
	ResetStaticMapChunkAtlas(outAtlas);
	if (staticScene.chunks.empty())
	{
		return false;
	}

	outAtlas.valid = true;
	outAtlas.buildSerial = staticScene.buildSerial;
	outAtlas.chunkCount = (uint32_t)staticScene.chunks.size();
	outAtlas.chunks.resize(staticScene.chunks.size());

	uint32_t vertexCursor = 0;
	uint32_t indexCursor = 0;
	uint32_t primitiveCursor = 0;
	uint32_t materialCursor = 0;
	for (uint32_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& sourceChunk = staticScene.chunks[chunkListIndex];
		auto& atlasChunk = outAtlas.chunks[chunkListIndex];
		atlasChunk.valid = true;
		atlasChunk.chunkIndex = sourceChunk.chunkIndex;
		atlasChunk.staticSceneChunkListIndex = chunkListIndex;
		atlasChunk.vertexOffset = AllocateChunkAtlasSlice(sourceChunk.vertexCount, 1u, vertexCursor);
		atlasChunk.vertexCount = sourceChunk.vertexCount;
		atlasChunk.indexOffset = AllocateChunkAtlasSlice(sourceChunk.indexCount, 1u, indexCursor);
		atlasChunk.indexCount = sourceChunk.indexCount;
		atlasChunk.primitiveOffset = AllocateChunkAtlasSlice(sourceChunk.primitiveCount, 1u, primitiveCursor);
		atlasChunk.primitiveCount = sourceChunk.primitiveCount;
		atlasChunk.materialOffset = AllocateChunkAtlasSlice(sourceChunk.materialCount, 1u, materialCursor);
		atlasChunk.materialCount = sourceChunk.materialCount;
	}

	outAtlas.vertexCount = vertexCursor;
	outAtlas.indexCount = indexCursor;
	outAtlas.primitiveCount = primitiveCursor;
	outAtlas.materialCount = materialCursor;
	outAtlas.vertexCapacity = GetChunkAtlasCapacity(vertexCursor);
	outAtlas.indexCapacity = GetChunkAtlasCapacity(indexCursor);
	outAtlas.primitiveCapacity = GetChunkAtlasCapacity(primitiveCursor);
	outAtlas.materialCapacity = GetChunkAtlasCapacity(materialCursor);
	return true;
}

void CopyChunkVertexDataToAtlas(
	const nri_scene::GeometryData& sourceGeometry,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::SceneVertex>& outVertices)
{
	if (!atlasChunk.valid)
	{
		return;
	}

	if (sourceChunk.vertexOffset + sourceChunk.vertexCount <= sourceGeometry.vertices.size() &&
		atlasChunk.vertexOffset + atlasChunk.vertexCount <= outVertices.size())
	{
		std::copy_n(
			sourceGeometry.vertices.data() + sourceChunk.vertexOffset,
			sourceChunk.vertexCount,
			outVertices.data() + atlasChunk.vertexOffset);
	}
}

void CopyChunkIndexDataToAtlas(
	const nri_scene::GeometryData& sourceGeometry,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<uint32_t>& outIndices)
{
	if (!atlasChunk.valid)
	{
		return;
	}

	if (sourceChunk.indexOffset + sourceChunk.indexCount <= sourceGeometry.indices.size() &&
		atlasChunk.indexOffset + atlasChunk.indexCount <= outIndices.size())
	{
		for (uint32_t i = 0; i < sourceChunk.indexCount; ++i)
		{
			const uint32_t sourceIndex = sourceGeometry.indices[sourceChunk.indexOffset + i];
			outIndices[atlasChunk.indexOffset + i] = atlasChunk.vertexOffset + sourceIndex - sourceChunk.vertexOffset;
		}
	}
}

void CopyChunkVertexAndIndexDataToAtlas(
	const nri_scene::GeometryData& sourceGeometry,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::SceneVertex>& outVertices,
	std::vector<uint32_t>& outIndices)
{
	if (!atlasChunk.valid)
	{
		return;
	}

	CopyChunkVertexDataToAtlas(
		sourceGeometry,
		sourceChunk,
		atlasChunk,
		outVertices);

	CopyChunkIndexDataToAtlas(
		sourceGeometry,
		sourceChunk,
		atlasChunk,
		outIndices);
}

void CopyChunkPrimitiveDataToAtlas(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::GeometryData& sourceGeometry,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::PrimitiveData>& outPrimitives)
{
	if (!atlasChunk.valid)
	{
		return;
	}

	if (sourceChunk.primitiveOffset + sourceChunk.primitiveCount <= sourceGeometry.primitives.size() &&
		atlasChunk.primitiveOffset + atlasChunk.primitiveCount <= outPrimitives.size())
	{
		for (uint32_t i = 0; i < sourceChunk.primitiveCount; ++i)
		{
			nri_scene::PrimitiveData primitive = sourceGeometry.primitives[sourceChunk.primitiveOffset + i];
			primitive.indices[0] = atlasChunk.vertexOffset + primitive.indices[0] - sourceChunk.vertexOffset;
			primitive.indices[1] = atlasChunk.vertexOffset + primitive.indices[1] - sourceChunk.vertexOffset;
			primitive.indices[2] = atlasChunk.vertexOffset + primitive.indices[2] - sourceChunk.vertexOffset;
			primitive.materialIndex = atlasChunk.materialOffset + primitive.materialIndex - sourceChunk.materialOffset;
			const uint32_t provenanceIndex = sourceChunk.primitiveOffset + i;
			const int32_t visibilityChunk =
				provenanceIndex < sourceGeometry.primitiveProvenance.size() ?
				ResolveVisibilityChunkIndexForProvenance(mapWorld, sourceGeometry.primitiveProvenance[provenanceIndex]) :
				-1;
			primitive.reserved0 = visibilityChunk >= 0 ? (uint32_t)visibilityChunk : UINT32_MAX;
			outPrimitives[atlasChunk.primitiveOffset + i] = primitive;
		}
	}
}

void CopyChunkGeometryToAtlas(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::GeometryData& sourceGeometry,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::SceneVertex>& outVertices,
	std::vector<uint32_t>& outIndices,
	std::vector<nri_scene::PrimitiveData>& outPrimitives)
{
	CopyChunkVertexAndIndexDataToAtlas(
		sourceGeometry,
		sourceChunk,
		atlasChunk,
		outVertices,
		outIndices);
	CopyChunkPrimitiveDataToAtlas(
		mapWorld,
		sourceGeometry,
		sourceChunk,
		atlasChunk,
		outPrimitives);
}

void CopyChunkMaterialsToAtlas(
	const std::vector<nri_scene::MaterialData>& sourceMaterials,
	const StaticMapSceneCache::ChunkCache& sourceChunk,
	const StaticMapChunkAtlas::ChunkEntry& atlasChunk,
	std::vector<nri_scene::MaterialData>& outMaterials)
{
	if (!atlasChunk.valid)
	{
		return;
	}

	if (sourceChunk.materialOffset + sourceChunk.materialCount <= sourceMaterials.size() &&
		atlasChunk.materialOffset + atlasChunk.materialCount <= outMaterials.size())
	{
		std::copy_n(
			sourceMaterials.data() + sourceChunk.materialOffset,
			sourceChunk.materialCount,
			outMaterials.data() + atlasChunk.materialOffset);
	}
}

bool RebuildResidentStaticCpuAtlasMirror(
	const nri_scene::PTMapWorld& mapWorld,
	StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas)
{
	if (!atlas.valid || atlas.chunks.size() != staticScene.chunks.size())
	{
		return false;
	}

	const nri_scene::GeometryData sourceGeometry = staticScene.geometry;
	const std::vector<nri_scene::MaterialData> sourceGpuMaterials = staticScene.gpuMaterials;
	nri_scene::GeometryData atlasGeometry = {};
	atlasGeometry.vertices.resize(atlas.vertexCount);
	atlasGeometry.indices.resize(atlas.indexCount);
	atlasGeometry.primitives.resize(atlas.primitiveCount);
	atlasGeometry.primitiveProvenance.resize(atlas.primitiveCount);
	std::vector<nri_scene::MaterialData> atlasGpuMaterials(atlas.materialCount);

	for (size_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		auto& chunkCache = staticScene.chunks[chunkListIndex];
		const auto& atlasChunk = atlas.chunks[chunkListIndex];
		if (!atlasChunk.valid)
		{
			chunkCache.active = false;
			chunkCache.vertexCount = 0;
			chunkCache.indexCount = 0;
			chunkCache.primitiveCount = 0;
			chunkCache.materialCount = 0;
			chunkCache.geometryPayloadHash = 0;
			nri_static_scene::UpdateAnimatedMaterialCandidate(staticScene, (uint32_t)chunkListIndex);
			continue;
		}

		CopyChunkGeometryToAtlas(
			mapWorld,
			sourceGeometry,
			chunkCache,
			atlasChunk,
			atlasGeometry.vertices,
			atlasGeometry.indices,
			atlasGeometry.primitives);
		CopyChunkMaterialsToAtlas(
			sourceGpuMaterials,
			chunkCache,
			atlasChunk,
			atlasGpuMaterials);

		if (chunkCache.primitiveOffset + chunkCache.primitiveCount <= sourceGeometry.primitiveProvenance.size() &&
			atlasChunk.primitiveOffset + atlasChunk.primitiveCount <= atlasGeometry.primitiveProvenance.size())
		{
			std::copy_n(
				sourceGeometry.primitiveProvenance.data() + chunkCache.primitiveOffset,
				chunkCache.primitiveCount,
				atlasGeometry.primitiveProvenance.data() + atlasChunk.primitiveOffset);
		}

		chunkCache.active = true;
		chunkCache.vertexOffset = atlasChunk.vertexOffset;
		chunkCache.vertexCount = atlasChunk.vertexCount;
		chunkCache.indexOffset = atlasChunk.indexOffset;
		chunkCache.indexCount = atlasChunk.indexCount;
		chunkCache.primitiveOffset = atlasChunk.primitiveOffset;
		chunkCache.primitiveCount = atlasChunk.primitiveCount;
		chunkCache.materialOffset = atlasChunk.materialOffset;
		chunkCache.materialCount = atlasChunk.materialCount;
		nri_static_scene::UpdateAnimatedMaterialCandidate(staticScene, (uint32_t)chunkListIndex);
		chunkCache.geometryPayloadHash = HashResidentGeometryPayload(
			mapWorld,
			atlasGeometry,
			chunkCache.vertexOffset,
			chunkCache.vertexCount,
			chunkCache.indexOffset,
			chunkCache.indexCount,
			chunkCache.primitiveOffset,
			chunkCache.primitiveCount,
			chunkCache.materialOffset,
			chunkCache.materialCount);
	}

	staticScene.geometry = std::move(atlasGeometry);
	++staticScene.geometryGeneration;
	staticScene.gpuMaterials = std::move(atlasGpuMaterials);
	return true;
}
}
