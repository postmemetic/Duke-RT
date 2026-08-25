#include "nri_map_deformer_layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	using namespace nri_scene;

	struct HorizontalCornerKey
	{
		uint32_t x = 0;
		uint32_t z = 0;

		bool operator==(const HorizontalCornerKey& other) const
		{
			return x == other.x && z == other.z;
		}

		bool operator<(const HorizontalCornerKey& other) const
		{
			return x < other.x || (x == other.x && z < other.z);
		}
	};

	struct HorizontalUvCornerKey
	{
		HorizontalCornerKey horizontal;
		uint32_t u = 0;
		uint32_t v = 0;

		bool operator==(const HorizontalUvCornerKey& other) const
		{
			return horizontal == other.horizontal && u == other.u && v == other.v;
		}

		bool operator<(const HorizontalUvCornerKey& other) const
		{
			if (horizontal < other.horizontal)
				return true;
			if (other.horizontal < horizontal)
				return false;
			return u < other.u || (u == other.u && v < other.v);
		}
	};

	struct TriangleKey
	{
		std::array<HorizontalCornerKey, 3> corners = {};
	};

	struct TriangleUvKey
	{
		std::array<HorizontalUvCornerKey, 3> corners = {};
	};

	struct TriangleShape
	{
		std::array<double, 3> horizontalEdgeLengths = {};
	};

	constexpr double HorizontalShapeAbsoluteTolerance = 1.0e-4;
	constexpr double HorizontalShapeRelativeTolerance = 1.0e-5;

	uint32_t FloatKey(float value)
	{
		if (value == 0.0f)
			return 0;
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	bool SameFloat(float a, float b)
	{
		return FloatKey(a) == FloatKey(b);
	}

	bool ProvenanceEquals(const SurfaceProvenance& a, const SurfaceProvenance& b)
	{
		if (a.sourceType != b.sourceType ||
			a.sectorIndex != b.sectorIndex ||
			a.wallIndex != b.wallIndex ||
			a.sectionIndex != b.sectionIndex ||
			a.mapChunkIndex != b.mapChunkIndex ||
			a.nextSectorIndex != b.nextSectorIndex ||
			a.actorIndex != b.actorIndex ||
			a.drawListType != b.drawListType ||
			a.cstat != b.cstat ||
			a.materialFlags != b.materialFlags ||
			a.actorOverlayRuleCount != b.actorOverlayRuleCount)
		{
			return false;
		}
		for (uint32_t i = 0; i < MaxActorOverlayRuleIdsPerSurface; ++i)
		{
			if (a.actorOverlayRuleIds[i] != b.actorOverlayRuleIds[i])
				return false;
		}
		return true;
	}

	bool TriangleKeyEquals(const TriangleKey& a, const TriangleKey& b)
	{
		return a.corners == b.corners;
	}

	bool TriangleUvKeyEquals(const TriangleUvKey& a, const TriangleUvKey& b)
	{
		return a.corners == b.corners;
	}

	bool HorizontalLengthEquals(double a, double b)
	{
		const double scale = std::max({ 1.0, std::abs(a), std::abs(b) });
		return std::abs(a - b) <=
			HorizontalShapeAbsoluteTolerance + HorizontalShapeRelativeTolerance * scale;
	}

	bool TriangleShapeEquals(const TriangleShape& a, const TriangleShape& b)
	{
		for (uint32_t i = 0; i < 3; ++i)
		{
			if (!HorizontalLengthEquals(a.horizontalEdgeLengths[i], b.horizontalEdgeLengths[i]))
				return false;
		}
		return true;
	}

	bool IsFiniteVertexPayload(const SceneVertex& vertex)
	{
		for (float value : vertex.position)
		{
			if (!std::isfinite(value))
				return false;
		}
		for (float value : vertex.prevPosition)
		{
			if (!std::isfinite(value))
				return false;
		}
		return std::isfinite(vertex.uv[0]) && std::isfinite(vertex.uv[1]);
	}

	bool IsFinitePrimitivePayload(const PrimitiveData& primitive)
	{
		for (float value : primitive.uv0)
		{
			if (!std::isfinite(value))
				return false;
		}
		for (float value : primitive.uv1)
		{
			if (!std::isfinite(value))
				return false;
		}
		for (float value : primitive.uv2)
		{
			if (!std::isfinite(value))
				return false;
		}
		for (float value : primitive.normal)
		{
			if (!std::isfinite(value))
				return false;
		}
		return true;
	}

	bool RangeFits(uint32_t offset, uint32_t count, size_t size)
	{
		return (uint64_t)offset + (uint64_t)count <= (uint64_t)size;
	}

	const float* PrimitiveUv(const PrimitiveData& primitive, uint32_t corner)
	{
		switch (corner)
		{
		case 0: return primitive.uv0;
		case 1: return primitive.uv1;
		default: return primitive.uv2;
		}
	}

	float* PrimitiveUv(PrimitiveData& primitive, uint32_t corner)
	{
		switch (corner)
		{
		case 0: return primitive.uv0;
		case 1: return primitive.uv1;
		default: return primitive.uv2;
		}
	}

	bool BuildTriangleKey(
		const GeometryData& geometry,
		const PrimitiveData& primitive,
		TriangleKey& outKey,
		TriangleUvKey& outUvKey,
		TriangleShape& outShape)
	{
		for (uint32_t corner = 0; corner < 3; ++corner)
		{
			const uint32_t vertexIndex = primitive.indices[corner];
			if (vertexIndex >= geometry.vertices.size())
				return false;
			const SceneVertex& vertex = geometry.vertices[vertexIndex];
			if (!std::isfinite(vertex.position[0]) || !std::isfinite(vertex.position[2]))
				return false;
			const float* primitiveUv = PrimitiveUv(primitive, corner);
			if (!std::isfinite(primitiveUv[0]) || !std::isfinite(primitiveUv[1]))
				return false;
			outKey.corners[corner] = { FloatKey(vertex.position[0]), FloatKey(vertex.position[2]) };
			outUvKey.corners[corner] =
			{
				outKey.corners[corner],
				FloatKey(primitiveUv[0]),
				FloatKey(primitiveUv[1])
			};
		}
		std::sort(outKey.corners.begin(), outKey.corners.end());
		std::sort(outUvKey.corners.begin(), outUvKey.corners.end());
		for (uint32_t edge = 0; edge < 3; ++edge)
		{
			const SceneVertex& a = geometry.vertices[primitive.indices[edge]];
			const SceneVertex& b = geometry.vertices[primitive.indices[(edge + 1u) % 3u]];
			const double dx = (double)a.position[0] - (double)b.position[0];
			const double dz = (double)a.position[2] - (double)b.position[2];
			outShape.horizontalEdgeLengths[edge] = std::sqrt(dx * dx + dz * dz);
		}
		std::sort(outShape.horizontalEdgeLengths.begin(), outShape.horizontalEdgeLengths.end());
		return true;
	}

	double HorizontalEdgeLength(const SceneVertex& a, const SceneVertex& b)
	{
		const double dx = (double)a.position[0] - (double)b.position[0];
		const double dz = (double)a.position[2] - (double)b.position[2];
		return std::sqrt(dx * dx + dz * dz);
	}

	double HorizontalSignedArea(
		const GeometryData& geometry,
		const PrimitiveData& primitive,
		const std::array<uint32_t, 3>& cornerOrder)
	{
		const SceneVertex& a = geometry.vertices[primitive.indices[cornerOrder[0]]];
		const SceneVertex& b = geometry.vertices[primitive.indices[cornerOrder[1]]];
		const SceneVertex& c = geometry.vertices[primitive.indices[cornerOrder[2]]];
		return
			((double)b.position[0] - a.position[0]) * ((double)c.position[2] - a.position[2]) -
			((double)b.position[2] - a.position[2]) * ((double)c.position[0] - a.position[0]);
	}

	bool CornerPermutationPreservesHorizontalShape(
		const GeometryData& retained,
		const PrimitiveData& retainedPrimitive,
		const GeometryData& current,
		const PrimitiveData& currentPrimitive,
		const std::array<uint32_t, 3>& currentCornerForRetained)
	{
		for (uint32_t first = 0; first < 3; ++first)
		{
			for (uint32_t second = first + 1; second < 3; ++second)
			{
				const SceneVertex& retainedFirst = retained.vertices[retainedPrimitive.indices[first]];
				const SceneVertex& retainedSecond = retained.vertices[retainedPrimitive.indices[second]];
				const SceneVertex& currentFirst = current.vertices[
					currentPrimitive.indices[currentCornerForRetained[first]]];
				const SceneVertex& currentSecond = current.vertices[
					currentPrimitive.indices[currentCornerForRetained[second]]];
				if (!HorizontalLengthEquals(
						HorizontalEdgeLength(retainedFirst, retainedSecond),
						HorizontalEdgeLength(currentFirst, currentSecond)))
				{
					return false;
				}
			}
		}

		const std::array<uint32_t, 3> retainedOrder = { 0, 1, 2 };
		const double retainedArea = HorizontalSignedArea(retained, retainedPrimitive, retainedOrder);
		const double currentArea = HorizontalSignedArea(current, currentPrimitive, currentCornerForRetained);
		const double areaScale = std::max({ 1.0, std::abs(retainedArea), std::abs(currentArea) });
		const double areaTolerance =
			HorizontalShapeAbsoluteTolerance + HorizontalShapeRelativeTolerance * areaScale;
		if (std::abs(retainedArea) > areaTolerance && std::abs(currentArea) > areaTolerance &&
			(retainedArea < 0.0) != (currentArea < 0.0))
		{
			return false;
		}
		return true;
	}

	bool TryGetVerticalRank(
		const GeometryData& geometry,
		const PrimitiveData& primitive,
		uint32_t corner,
		uint32_t& outRank)
	{
		const SceneVertex& target = geometry.vertices[primitive.indices[corner]];
		outRank = 0;
		for (uint32_t otherCorner = 0; otherCorner < 3; ++otherCorner)
		{
			if (otherCorner == corner)
				continue;
			const SceneVertex& other = geometry.vertices[primitive.indices[otherCorner]];
			if (!SameFloat(target.position[0], other.position[0]) ||
				!SameFloat(target.position[2], other.position[2]))
			{
				continue;
			}
			if (!std::isfinite(target.position[1]) || !std::isfinite(other.position[1]) ||
				SameFloat(target.position[1], other.position[1]))
			{
				return false;
			}
			if (other.position[1] < target.position[1])
				++outRank;
		}
		return true;
	}

	struct CornerMappingScore
	{
		uint32_t exactYMatches = 0;
		uint32_t verticalRankMatches = 0;
		uint32_t uvMatches = 0;
	};

	bool BetterCornerMappingScore(const CornerMappingScore& a, const CornerMappingScore& b)
	{
		if (a.verticalRankMatches != b.verticalRankMatches)
			return a.verticalRankMatches > b.verticalRankMatches;
		if (a.exactYMatches != b.exactYMatches)
			return a.exactYMatches > b.exactYMatches;
		return a.uvMatches > b.uvMatches;
	}

	bool EqualCornerMappingScore(const CornerMappingScore& a, const CornerMappingScore& b)
	{
		return a.exactYMatches == b.exactYMatches &&
			a.verticalRankMatches == b.verticalRankMatches &&
			a.uvMatches == b.uvMatches;
	}

	bool ResolveCornerMapping(
		const GeometryData& retained,
		const PrimitiveData& retainedPrimitive,
		const GeometryData& current,
		const PrimitiveData& currentPrimitive,
		const std::vector<uint32_t>& retainedToCurrentVertex,
		const std::vector<uint32_t>& currentToRetainedVertex,
		std::array<uint32_t, 3>& outCurrentCornerForRetained,
		CornerMappingScore& outScore)
	{
		std::array<uint32_t, 3> permutation = { 0, 1, 2 };
		std::array<uint32_t, 3> best = {};
		CornerMappingScore bestScore = {};
		bool found = false;
		bool ambiguous = false;
		do
		{
			CornerMappingScore score = {};
			bool valid = CornerPermutationPreservesHorizontalShape(
				retained,
				retainedPrimitive,
				current,
				currentPrimitive,
				permutation);
			for (uint32_t retainedCorner = 0; retainedCorner < 3; ++retainedCorner)
			{
				if (!valid)
					break;
				const uint32_t currentCorner = permutation[retainedCorner];
				const uint32_t retainedVertexIndex = retainedPrimitive.indices[retainedCorner];
				const uint32_t currentVertexIndex = currentPrimitive.indices[currentCorner];
				const SceneVertex& retainedVertex = retained.vertices[retainedVertexIndex];
				const SceneVertex& currentVertex = current.vertices[currentVertexIndex];
				if ((retainedToCurrentVertex[retainedVertexIndex] != UINT32_MAX &&
						retainedToCurrentVertex[retainedVertexIndex] != currentVertexIndex) ||
					(currentToRetainedVertex[currentVertexIndex] != UINT32_MAX &&
						currentToRetainedVertex[currentVertexIndex] != retainedVertexIndex))
				{
					valid = false;
					break;
				}

				if (SameFloat(retainedVertex.position[1], currentVertex.position[1]))
					++score.exactYMatches;
				uint32_t retainedRank = 0;
				uint32_t currentRank = 0;
				if (TryGetVerticalRank(retained, retainedPrimitive, retainedCorner, retainedRank) &&
					TryGetVerticalRank(current, currentPrimitive, currentCorner, currentRank) &&
					retainedRank == currentRank)
				{
					++score.verticalRankMatches;
				}
				const float* retainedUv = PrimitiveUv(retainedPrimitive, retainedCorner);
				const float* currentUv = PrimitiveUv(currentPrimitive, currentCorner);
				if (SameFloat(retainedUv[0], currentUv[0]) && SameFloat(retainedUv[1], currentUv[1]))
					++score.uvMatches;
			}

			if (!valid)
				continue;
			if (!found || BetterCornerMappingScore(score, bestScore))
			{
				best = permutation;
				bestScore = score;
				found = true;
				ambiguous = false;
			}
			else if (EqualCornerMappingScore(score, bestScore))
			{
				ambiguous = true;
			}
		}
		while (std::next_permutation(permutation.begin(), permutation.end()));

		if (!found || ambiguous)
			return false;
		outCurrentCornerForRetained = best;
		outScore = bestScore;
		return true;
	}

	bool ValidatePrimitiveIndexLayout(
		const GeometryData& geometry,
		uint32_t vertexOffset,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t primitiveOffset,
		uint32_t primitiveCount)
	{
		const uint64_t vertexEnd = (uint64_t)vertexOffset + vertexCount;
		for (uint32_t i = 0; i < primitiveCount; ++i)
		{
			const PrimitiveData& primitive = geometry.primitives[primitiveOffset + i];
			for (uint32_t corner = 0; corner < 3; ++corner)
			{
				const uint32_t index = geometry.indices[indexOffset + i * 3u + corner];
				if (primitive.indices[corner] != index ||
					index < vertexOffset || (uint64_t)index >= vertexEnd)
				{
					return false;
				}
			}
		}
		return true;
	}

	GeometryData BuildNormalizedRetainedGeometry(
		const GeometryData& retainedGeometry,
		const MapDeformerGeometrySlice& slice)
	{
		GeometryData result;
		result.vertices.assign(
			retainedGeometry.vertices.begin() + slice.vertexOffset,
			retainedGeometry.vertices.begin() + slice.vertexOffset + slice.vertexCount);
		result.indices.reserve(slice.indexCount);
		for (uint32_t i = 0; i < slice.indexCount; ++i)
		{
			result.indices.push_back(retainedGeometry.indices[slice.indexOffset + i] - slice.vertexOffset);
		}
		result.primitives.reserve(slice.primitiveCount);
		result.primitiveProvenance.reserve(slice.primitiveCount);
		for (uint32_t i = 0; i < slice.primitiveCount; ++i)
		{
			PrimitiveData primitive = retainedGeometry.primitives[slice.primitiveOffset + i];
			for (uint32_t corner = 0; corner < 3; ++corner)
				primitive.indices[corner] -= slice.vertexOffset;
			primitive.materialIndex -= slice.materialOffset;
			result.primitives.push_back(primitive);
			result.primitiveProvenance.push_back(retainedGeometry.primitiveProvenance[slice.primitiveOffset + i]);
		}
		return result;
	}

	template<typename T>
	std::vector<MapDeformerChangedSpan> BuildChangedSpans(
		const std::vector<T>& retained,
		const std::vector<T>& current,
		uint32_t destinationElementOffset,
		uint64_t& outBytes)
	{
		std::vector<MapDeformerChangedSpan> spans;
		outBytes = 0;
		size_t begin = current.size();
		for (size_t i = 0; i <= current.size(); ++i)
		{
			const bool changed =
				i < current.size() && std::memcmp(&retained[i], &current[i], sizeof(T)) != 0;
			if (changed && begin == current.size())
			{
				begin = i;
			}
			else if (!changed && begin != current.size())
			{
				const uint32_t sourceOffset = (uint32_t)begin;
				const uint32_t count = (uint32_t)(i - begin);
				MapDeformerChangedSpan span;
				span.sourceElementOffset = sourceOffset;
				span.destinationElementOffset = destinationElementOffset + sourceOffset;
				span.elementCount = count;
				span.destinationByteOffset = (uint64_t)span.destinationElementOffset * sizeof(T);
				span.byteCount = (uint64_t)count * sizeof(T);
				outBytes += span.byteCount;
				spans.push_back(span);
				begin = current.size();
			}
		}
		return spans;
	}

	void Reject(MapDeformerLayoutMapping& mapping, uint32_t reason)
	{
		mapping.compatible = false;
		mapping.rejectMask |= reason;
	}

	constexpr uint32_t TopologyFallbackMaxPrimitives = 512;
	constexpr uint32_t TopologyFallbackMaxVertices = 1536;
	constexpr uint32_t TopologyFallbackMaxSearchStates = 4096;

	struct TopologyMappingScore
	{
		CornerMappingScore corner;
		uint32_t exactHorizontalMatches = 0;
	};
	constexpr uint32_t TopologyHorizontalTieBreakMinimumMatches = 6;

	uint32_t EffectiveTopologyHorizontalMatches(const TopologyMappingScore& score)
	{
		return score.exactHorizontalMatches >= TopologyHorizontalTieBreakMinimumMatches ?
			score.exactHorizontalMatches : 0u;
	}

	bool BetterTopologyMappingScore(const TopologyMappingScore& a, const TopologyMappingScore& b)
	{
		const uint32_t aHorizontal = EffectiveTopologyHorizontalMatches(a);
		const uint32_t bHorizontal = EffectiveTopologyHorizontalMatches(b);
		if (aHorizontal != bHorizontal)
			return aHorizontal > bHorizontal;
		return BetterCornerMappingScore(a.corner, b.corner);
	}

	bool EqualTopologyMappingScore(const TopologyMappingScore& a, const TopologyMappingScore& b)
	{
		return EffectiveTopologyHorizontalMatches(a) == EffectiveTopologyHorizontalMatches(b) &&
			EqualCornerMappingScore(a.corner, b.corner);
	}

	bool GeometryPayloadEquals(const GeometryData& a, const GeometryData& b)
	{
		return a.vertices.size() == b.vertices.size() &&
			a.indices == b.indices &&
			a.primitives.size() == b.primitives.size() &&
			(a.vertices.empty() || std::memcmp(
				a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(SceneVertex)) == 0) &&
			(a.primitives.empty() || std::memcmp(
				a.primitives.data(), b.primitives.data(), a.primitives.size() * sizeof(PrimitiveData)) == 0);
	}

	TopologyMappingScore ScoreTopologyCornerPermutation(
		const GeometryData& retained,
		const PrimitiveData& retainedPrimitive,
		const GeometryData& current,
		const PrimitiveData& currentPrimitive,
		const std::array<uint32_t, 3>& currentCornerForRetained)
	{
		TopologyMappingScore score = {};
		for (uint32_t retainedCorner = 0; retainedCorner < 3; ++retainedCorner)
		{
			const uint32_t currentCorner = currentCornerForRetained[retainedCorner];
			const SceneVertex& retainedVertex = retained.vertices[retainedPrimitive.indices[retainedCorner]];
			const SceneVertex& currentVertex = current.vertices[currentPrimitive.indices[currentCorner]];
			if (SameFloat(retainedVertex.position[0], currentVertex.position[0]) &&
				SameFloat(retainedVertex.position[2], currentVertex.position[2]))
			{
				++score.exactHorizontalMatches;
			}
			if (SameFloat(retainedVertex.position[1], currentVertex.position[1]))
				++score.corner.exactYMatches;
			uint32_t retainedRank = 0;
			uint32_t currentRank = 0;
			if (TryGetVerticalRank(retained, retainedPrimitive, retainedCorner, retainedRank) &&
				TryGetVerticalRank(current, currentPrimitive, currentCorner, currentRank) &&
				retainedRank == currentRank)
			{
				++score.corner.verticalRankMatches;
			}
			const float* retainedUv = PrimitiveUv(retainedPrimitive, retainedCorner);
			const float* currentUv = PrimitiveUv(currentPrimitive, currentCorner);
			if (SameFloat(retainedUv[0], currentUv[0]) && SameFloat(retainedUv[1], currentUv[1]))
				++score.corner.uvMatches;
		}
		return score;
	}

	TopologyMappingScore AddTopologyMappingScores(
		const TopologyMappingScore& a,
		const TopologyMappingScore& b)
	{
		TopologyMappingScore result;
		result.exactHorizontalMatches = a.exactHorizontalMatches + b.exactHorizontalMatches;
		result.corner.exactYMatches = a.corner.exactYMatches + b.corner.exactYMatches;
		result.corner.verticalRankMatches = a.corner.verticalRankMatches + b.corner.verticalRankMatches;
		result.corner.uvMatches = a.corner.uvMatches + b.corner.uvMatches;
		return result;
	}

	class TopologyFallbackSearch
	{
	public:
		TopologyFallbackSearch(
			const GeometryData& retained,
			const GeometryData& current,
			uint32_t materialCount)
			: m_retained(retained)
			, m_current(current)
			, m_materialCount(materialCount)
		{
		}

		bool Initialize()
		{
			const uint32_t primitiveCount = (uint32_t)m_retained.primitives.size();
			const uint32_t vertexCount = (uint32_t)m_retained.vertices.size();
			if (primitiveCount == 0 || primitiveCount > TopologyFallbackMaxPrimitives ||
				vertexCount == 0 || vertexCount > TopologyFallbackMaxVertices ||
				m_current.primitives.size() != primitiveCount ||
				m_current.vertices.size() != vertexCount)
			{
				return false;
			}

			m_retainedVertexDegree.assign(vertexCount, 0);
			m_currentVertexDegree.assign(vertexCount, 0);
			for (uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
			{
				if (!IsFiniteVertexPayload(m_retained.vertices[vertexIndex]) ||
					!IsFiniteVertexPayload(m_current.vertices[vertexIndex]))
				{
					return false;
				}
			}
			for (uint32_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
			{
				const PrimitiveData& retainedPrimitive = m_retained.primitives[primitiveIndex];
				const PrimitiveData& currentPrimitive = m_current.primitives[primitiveIndex];
				for (uint32_t corner = 0; corner < 3; ++corner)
				{
					const uint32_t retainedVertex = retainedPrimitive.indices[corner];
					const uint32_t currentVertex = currentPrimitive.indices[corner];
					if (retainedVertex >= vertexCount || currentVertex >= vertexCount)
						return false;
					for (uint32_t previousCorner = 0; previousCorner < corner; ++previousCorner)
					{
						if (retainedPrimitive.indices[previousCorner] == retainedVertex ||
							currentPrimitive.indices[previousCorner] == currentVertex)
						{
							return false;
						}
					}
					++m_retainedVertexDegree[retainedVertex];
					++m_currentVertexDegree[currentVertex];
				}
			}
			if (std::find(m_retainedVertexDegree.begin(), m_retainedVertexDegree.end(), 0) !=
					m_retainedVertexDegree.end() ||
				std::find(m_currentVertexDegree.begin(), m_currentVertexDegree.end(), 0) !=
					m_currentVertexDegree.end())
			{
				return false;
			}

			std::vector<TriangleShape> retainedShapes(primitiveCount);
			std::vector<TriangleShape> currentShapes(primitiveCount);
			for (uint32_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
			{
				TriangleKey retainedKey = {};
				TriangleUvKey retainedUvKey = {};
				TriangleKey currentKey = {};
				TriangleUvKey currentUvKey = {};
				if (!BuildTriangleKey(
						m_retained,
						m_retained.primitives[primitiveIndex],
						retainedKey,
						retainedUvKey,
						retainedShapes[primitiveIndex]) ||
					!BuildTriangleKey(
						m_current,
						m_current.primitives[primitiveIndex],
						currentKey,
						currentUvKey,
						currentShapes[primitiveIndex]))
				{
					return false;
				}
			}

			m_candidates.resize(primitiveCount);
			m_requiresShapePermutation.assign(primitiveCount, 0);
			for (uint32_t retainedPrimitiveIndex = 0;
				retainedPrimitiveIndex < primitiveCount;
				++retainedPrimitiveIndex)
			{
				std::vector<uint32_t> metadataCandidates;
				std::vector<uint32_t> shapeCandidates;
				for (uint32_t currentPrimitiveIndex = 0;
					currentPrimitiveIndex < primitiveCount;
					++currentPrimitiveIndex)
				{
					if (PrimitiveMetadataMatches(retainedPrimitiveIndex, currentPrimitiveIndex))
					{
						metadataCandidates.push_back(currentPrimitiveIndex);
						if (TriangleShapeEquals(
								retainedShapes[retainedPrimitiveIndex],
								currentShapes[currentPrimitiveIndex]))
						{
							shapeCandidates.push_back(currentPrimitiveIndex);
						}
					}
				}
				if (metadataCandidates.empty())
					return false;
				if (!shapeCandidates.empty())
				{
					m_candidates[retainedPrimitiveIndex] = std::move(shapeCandidates);
					m_requiresShapePermutation[retainedPrimitiveIndex] = 1;
				}
				else
				{
					m_candidates[retainedPrimitiveIndex] = std::move(metadataCandidates);
				}
			}

			m_retainedToCurrentVertex.assign(vertexCount, UINT32_MAX);
			m_currentToRetainedVertex.assign(vertexCount, UINT32_MAX);
			m_currentPrimitiveForRetained.assign(primitiveCount, UINT32_MAX);
			m_currentPrimitiveUsed.assign(primitiveCount, 0);
			m_currentCornersForRetained.resize(primitiveCount);
			return true;
		}

		void Run()
		{
			Search(0, {});
		}

		bool HasUniqueResult() const
		{
			return m_found && !m_ambiguous && !m_exhausted;
		}

		bool Exhausted() const
		{
			return m_exhausted;
		}

		const GeometryData& Result() const
		{
			return m_bestCanonical;
		}

	private:
		bool PrimitiveMetadataMatches(uint32_t retainedIndex, uint32_t currentIndex) const
		{
			const PrimitiveData& retained = m_retained.primitives[retainedIndex];
			const PrimitiveData& current = m_current.primitives[currentIndex];
			return
				ProvenanceEquals(
					m_retained.primitiveProvenance[retainedIndex],
					m_current.primitiveProvenance[currentIndex]) &&
				retained.materialIndex < m_materialCount &&
				current.materialIndex < m_materialCount &&
				retained.materialIndex == current.materialIndex &&
				retained.flags == current.flags &&
				retained.portalIndex == current.portalIndex &&
				(current.reserved0 == UINT32_MAX || retained.reserved0 == current.reserved0) &&
				retained.smoothNormals[0] == 0 && retained.smoothNormals[1] == 0 &&
				current.smoothNormals[0] == 0 && current.smoothNormals[1] == 0 &&
				IsFinitePrimitivePayload(current);
		}

		uint32_t SelectNextRetainedPrimitive() const
		{
			uint32_t selected = UINT32_MAX;
			uint32_t selectedMappedVertices = 0;
			uint32_t selectedAvailableCandidates = UINT32_MAX;
			for (uint32_t retainedPrimitiveIndex = 0;
				retainedPrimitiveIndex < m_retained.primitives.size();
				++retainedPrimitiveIndex)
			{
				if (m_currentPrimitiveForRetained[retainedPrimitiveIndex] != UINT32_MAX)
					continue;
				uint32_t mappedVertices = 0;
				for (uint32_t corner = 0; corner < 3; ++corner)
				{
					const uint32_t vertex = m_retained.primitives[retainedPrimitiveIndex].indices[corner];
					if (m_retainedToCurrentVertex[vertex] != UINT32_MAX)
						++mappedVertices;
				}
				uint32_t availableCandidates = 0;
				for (uint32_t candidate : m_candidates[retainedPrimitiveIndex])
				{
					if (m_currentPrimitiveUsed[candidate] == 0)
						++availableCandidates;
				}
				if (selected == UINT32_MAX ||
					mappedVertices > selectedMappedVertices ||
					(mappedVertices == selectedMappedVertices &&
						availableCandidates < selectedAvailableCandidates))
				{
					selected = retainedPrimitiveIndex;
					selectedMappedVertices = mappedVertices;
					selectedAvailableCandidates = availableCandidates;
				}
			}
			return selected;
		}

		bool PermutationFits(
			uint32_t retainedPrimitiveIndex,
			uint32_t currentPrimitiveIndex,
			const std::array<uint32_t, 3>& permutation) const
		{
			const PrimitiveData& retained = m_retained.primitives[retainedPrimitiveIndex];
			const PrimitiveData& current = m_current.primitives[currentPrimitiveIndex];
			if (m_requiresShapePermutation[retainedPrimitiveIndex] != 0 &&
				!CornerPermutationPreservesHorizontalShape(
					m_retained,
					retained,
					m_current,
					current,
					permutation))
			{
				return false;
			}
			for (uint32_t retainedCorner = 0; retainedCorner < 3; ++retainedCorner)
			{
				const uint32_t retainedVertex = retained.indices[retainedCorner];
				const uint32_t currentVertex = current.indices[permutation[retainedCorner]];
				if (m_retainedVertexDegree[retainedVertex] != m_currentVertexDegree[currentVertex] ||
					(m_retainedToCurrentVertex[retainedVertex] != UINT32_MAX &&
						m_retainedToCurrentVertex[retainedVertex] != currentVertex) ||
					(m_currentToRetainedVertex[currentVertex] != UINT32_MAX &&
						m_currentToRetainedVertex[currentVertex] != retainedVertex))
				{
					return false;
				}
			}
			return true;
		}

		GeometryData BuildCanonical() const
		{
			GeometryData canonical = m_retained;
			for (uint32_t retainedVertex = 0;
				retainedVertex < m_retainedToCurrentVertex.size();
				++retainedVertex)
			{
				canonical.vertices[retainedVertex] =
					m_current.vertices[m_retainedToCurrentVertex[retainedVertex]];
			}
			for (uint32_t retainedPrimitiveIndex = 0;
				retainedPrimitiveIndex < canonical.primitives.size();
				++retainedPrimitiveIndex)
			{
				PrimitiveData& canonicalPrimitive = canonical.primitives[retainedPrimitiveIndex];
				const PrimitiveData& currentPrimitive =
					m_current.primitives[m_currentPrimitiveForRetained[retainedPrimitiveIndex]];
				for (uint32_t retainedCorner = 0; retainedCorner < 3; ++retainedCorner)
				{
					const uint32_t currentCorner =
						m_currentCornersForRetained[retainedPrimitiveIndex][retainedCorner];
					const float* currentUv = PrimitiveUv(currentPrimitive, currentCorner);
					float* canonicalUv = PrimitiveUv(canonicalPrimitive, retainedCorner);
					canonicalUv[0] = currentUv[0];
					canonicalUv[1] = currentUv[1];
				}
				std::copy(
					std::begin(currentPrimitive.normal),
					std::end(currentPrimitive.normal),
					canonicalPrimitive.normal);
				canonicalPrimitive.temporalSurfaceId[0] = currentPrimitive.temporalSurfaceId[0];
				canonicalPrimitive.temporalSurfaceId[1] = currentPrimitive.temporalSurfaceId[1];
				canonicalPrimitive.temporalGeneration = currentPrimitive.temporalGeneration;
				canonicalPrimitive.temporalFlags = currentPrimitive.temporalFlags;
			}
			return canonical;
		}

		void NoteCompleteSolution(const TopologyMappingScore& score)
		{
			GeometryData canonical = BuildCanonical();
			if (!m_found || BetterTopologyMappingScore(score, m_bestScore))
			{
				m_found = true;
				m_ambiguous = false;
				m_bestScore = score;
				m_bestCanonical = std::move(canonical);
			}
			else if (EqualTopologyMappingScore(score, m_bestScore) &&
				!GeometryPayloadEquals(canonical, m_bestCanonical))
			{
				m_ambiguous = true;
			}
		}

		void Search(uint32_t assignedPrimitiveCount, const TopologyMappingScore& score)
		{
			if (m_exhausted)
				return;
			if (m_found &&
				EffectiveTopologyHorizontalMatches(m_bestScore) != 0 &&
				score.exactHorizontalMatches +
					(m_retained.primitives.size() - assignedPrimitiveCount) * 3u <
					m_bestScore.exactHorizontalMatches)
			{
				return;
			}
			if (assignedPrimitiveCount == m_retained.primitives.size())
			{
				if (std::find(
						m_retainedToCurrentVertex.begin(),
						m_retainedToCurrentVertex.end(),
						UINT32_MAX) == m_retainedToCurrentVertex.end())
				{
					NoteCompleteSolution(score);
				}
				return;
			}

			const uint32_t retainedPrimitiveIndex = SelectNextRetainedPrimitive();
			if (retainedPrimitiveIndex == UINT32_MAX)
				return;
			const PrimitiveData& retainedPrimitive = m_retained.primitives[retainedPrimitiveIndex];
			for (uint32_t currentPrimitiveIndex : m_candidates[retainedPrimitiveIndex])
			{
				if (m_currentPrimitiveUsed[currentPrimitiveIndex] != 0)
					continue;
				const PrimitiveData& currentPrimitive = m_current.primitives[currentPrimitiveIndex];
				std::array<uint32_t, 3> permutation = { 0, 1, 2 };
				do
				{
					if (++m_searchStates > TopologyFallbackMaxSearchStates)
					{
						m_exhausted = true;
						return;
					}
					if (!PermutationFits(retainedPrimitiveIndex, currentPrimitiveIndex, permutation))
						continue;

					std::array<uint8_t, 3> addedVertexMapping = {};
					for (uint32_t retainedCorner = 0; retainedCorner < 3; ++retainedCorner)
					{
						const uint32_t retainedVertex = retainedPrimitive.indices[retainedCorner];
						const uint32_t currentVertex = currentPrimitive.indices[permutation[retainedCorner]];
						if (m_retainedToCurrentVertex[retainedVertex] == UINT32_MAX)
						{
							m_retainedToCurrentVertex[retainedVertex] = currentVertex;
							m_currentToRetainedVertex[currentVertex] = retainedVertex;
							addedVertexMapping[retainedCorner] = 1;
						}
					}
					m_currentPrimitiveForRetained[retainedPrimitiveIndex] = currentPrimitiveIndex;
					m_currentCornersForRetained[retainedPrimitiveIndex] = permutation;
					m_currentPrimitiveUsed[currentPrimitiveIndex] = 1;
					const TopologyMappingScore nextScore = AddTopologyMappingScores(
						score,
						ScoreTopologyCornerPermutation(
							m_retained,
							retainedPrimitive,
							m_current,
							currentPrimitive,
							permutation));
					Search(assignedPrimitiveCount + 1, nextScore);
					m_currentPrimitiveUsed[currentPrimitiveIndex] = 0;
					m_currentPrimitiveForRetained[retainedPrimitiveIndex] = UINT32_MAX;
					for (uint32_t retainedCorner = 0; retainedCorner < 3; ++retainedCorner)
					{
						if (addedVertexMapping[retainedCorner] == 0)
							continue;
						const uint32_t retainedVertex = retainedPrimitive.indices[retainedCorner];
						const uint32_t currentVertex = m_retainedToCurrentVertex[retainedVertex];
						m_retainedToCurrentVertex[retainedVertex] = UINT32_MAX;
						m_currentToRetainedVertex[currentVertex] = UINT32_MAX;
					}
				}
				while (std::next_permutation(permutation.begin(), permutation.end()));
			}
		}

		const GeometryData& m_retained;
		const GeometryData& m_current;
		uint32_t m_materialCount = 0;
		std::vector<uint32_t> m_retainedVertexDegree;
		std::vector<uint32_t> m_currentVertexDegree;
		std::vector<std::vector<uint32_t>> m_candidates;
		std::vector<uint8_t> m_requiresShapePermutation;
		std::vector<uint32_t> m_retainedToCurrentVertex;
		std::vector<uint32_t> m_currentToRetainedVertex;
		std::vector<uint32_t> m_currentPrimitiveForRetained;
		std::vector<uint8_t> m_currentPrimitiveUsed;
		std::vector<std::array<uint32_t, 3>> m_currentCornersForRetained;
		uint32_t m_searchStates = 0;
		bool m_exhausted = false;
		bool m_found = false;
		bool m_ambiguous = false;
		TopologyMappingScore m_bestScore = {};
		GeometryData m_bestCanonical;
	};

	MapDeformerLayoutMapping MapCurrentGeometryByTopologyFallback(
		const GeometryData& retainedGeometry,
		const MapDeformerGeometrySlice& retainedSlice,
		const GeometryData& exactCurrentGeometry)
	{
		MapDeformerLayoutMapping result;
		GeometryData retained = BuildNormalizedRetainedGeometry(retainedGeometry, retainedSlice);
		TopologyFallbackSearch search(retained, exactCurrentGeometry, retainedSlice.materialCount);
		if (!search.Initialize())
		{
			Reject(result, MapDeformerLayoutReject_UnmatchedTriangle);
			return result;
		}
		search.Run();
		if (!search.HasUniqueResult())
		{
			Reject(result, MapDeformerLayoutReject_AmbiguousKey);
			return result;
		}

		result.canonicalCurrent = search.Result();
		result.changedVertexSpans = BuildChangedSpans(
			retained.vertices,
			result.canonicalCurrent.vertices,
			retainedSlice.vertexOffset,
			result.changedVertexBytes);
		result.changedPrimitiveSpans = BuildChangedSpans(
			retained.primitives,
			result.canonicalCurrent.primitives,
			retainedSlice.primitiveOffset,
			result.changedPrimitiveBytes);
		result.changedBytes = result.changedVertexBytes + result.changedPrimitiveBytes;
		result.compatible = true;
		return result;
	}
}

namespace nri_scene
{
MapDeformerLayoutMapping MapCurrentGeometryToRetainedDeformerLayoutFast(
	const GeometryData& retainedGeometry,
	const MapDeformerGeometrySlice& retainedSlice,
	const GeometryData& exactCurrentGeometry)
{
	MapDeformerLayoutMapping result;
	const bool validRetainedRanges =
		retainedSlice.vertexCount != 0 &&
		retainedSlice.primitiveCount != 0 &&
		retainedSlice.materialCount != 0 &&
		retainedSlice.indexCount == (uint64_t)retainedSlice.primitiveCount * 3u &&
		RangeFits(retainedSlice.vertexOffset, retainedSlice.vertexCount, retainedGeometry.vertices.size()) &&
		RangeFits(retainedSlice.indexOffset, retainedSlice.indexCount, retainedGeometry.indices.size()) &&
		RangeFits(retainedSlice.primitiveOffset, retainedSlice.primitiveCount, retainedGeometry.primitives.size()) &&
		RangeFits(retainedSlice.primitiveOffset, retainedSlice.primitiveCount, retainedGeometry.primitiveProvenance.size());
	if (!validRetainedRanges)
	{
		Reject(result, MapDeformerLayoutReject_InvalidSlice);
		return result;
	}

	if (exactCurrentGeometry.vertices.size() != retainedSlice.vertexCount ||
		exactCurrentGeometry.indices.size() != retainedSlice.indexCount ||
		exactCurrentGeometry.primitives.size() != retainedSlice.primitiveCount ||
		exactCurrentGeometry.primitiveProvenance.size() != retainedSlice.primitiveCount)
	{
		Reject(result, MapDeformerLayoutReject_CountMismatch);
		return result;
	}

	if (!ValidatePrimitiveIndexLayout(
			retainedGeometry,
			retainedSlice.vertexOffset,
			retainedSlice.vertexCount,
			retainedSlice.indexOffset,
			retainedSlice.primitiveOffset,
			retainedSlice.primitiveCount) ||
		!ValidatePrimitiveIndexLayout(
			exactCurrentGeometry,
			0,
			(uint32_t)exactCurrentGeometry.vertices.size(),
			0,
			0,
			(uint32_t)exactCurrentGeometry.primitives.size()))
	{
		Reject(result, MapDeformerLayoutReject_IndexLayout);
		return result;
	}

	GeometryData retained = BuildNormalizedRetainedGeometry(retainedGeometry, retainedSlice);
	std::vector<TriangleKey> retainedKeys(retainedSlice.primitiveCount);
	std::vector<TriangleUvKey> retainedUvKeys(retainedSlice.primitiveCount);
	std::vector<TriangleShape> retainedShapes(retainedSlice.primitiveCount);
	std::vector<TriangleKey> currentKeys(retainedSlice.primitiveCount);
	std::vector<TriangleUvKey> currentUvKeys(retainedSlice.primitiveCount);
	std::vector<TriangleShape> currentShapes(retainedSlice.primitiveCount);

	for (uint32_t i = 0; i < retainedSlice.primitiveCount; ++i)
	{
		if (!BuildTriangleKey(
				retained,
				retained.primitives[i],
				retainedKeys[i],
				retainedUvKeys[i],
				retainedShapes[i]) ||
			!BuildTriangleKey(
				exactCurrentGeometry,
				exactCurrentGeometry.primitives[i],
				currentKeys[i],
				currentUvKeys[i],
				currentShapes[i]))
		{
			Reject(result, MapDeformerLayoutReject_NonFiniteKey);
			return result;
		}
	}

	for (uint32_t i = 0; i < retainedSlice.primitiveCount; ++i)
	{
		for (uint32_t j = i + 1; j < retainedSlice.primitiveCount; ++j)
		{
			const bool duplicateRetained =
				ProvenanceEquals(retained.primitiveProvenance[i], retained.primitiveProvenance[j]) &&
				TriangleKeyEquals(retainedKeys[i], retainedKeys[j]) &&
				TriangleUvKeyEquals(retainedUvKeys[i], retainedUvKeys[j]);
			const bool duplicateCurrent =
				ProvenanceEquals(exactCurrentGeometry.primitiveProvenance[i], exactCurrentGeometry.primitiveProvenance[j]) &&
				TriangleKeyEquals(currentKeys[i], currentKeys[j]) &&
				TriangleUvKeyEquals(currentUvKeys[i], currentUvKeys[j]);
			if (duplicateRetained || duplicateCurrent)
			{
				Reject(result, MapDeformerLayoutReject_DuplicateTriangle);
				return result;
			}
		}
	}

	GeometryData canonical = retained;
	std::vector<uint8_t> currentPrimitiveUsed(retainedSlice.primitiveCount, 0);
	std::vector<uint32_t> retainedToCurrentVertex(retainedSlice.vertexCount, UINT32_MAX);
	std::vector<uint32_t> currentToRetainedVertex(retainedSlice.vertexCount, UINT32_MAX);

	for (uint32_t retainedPrimitiveIndex = 0;
		retainedPrimitiveIndex < retainedSlice.primitiveCount;
		++retainedPrimitiveIndex)
	{
		std::vector<uint32_t> shapeCandidates;
		std::vector<uint32_t> provenanceCandidates;
		std::vector<uint32_t> materialCandidates;
		std::vector<uint32_t> flagsCandidates;
		std::vector<uint32_t> portalCandidates;
		std::vector<uint32_t> reservedCandidates;
		for (uint32_t currentPrimitiveIndex = 0;
			currentPrimitiveIndex < retainedSlice.primitiveCount;
			++currentPrimitiveIndex)
		{
			if (currentPrimitiveUsed[currentPrimitiveIndex] != 0 ||
				!TriangleShapeEquals(retainedShapes[retainedPrimitiveIndex], currentShapes[currentPrimitiveIndex]))
			{
				continue;
			}
			shapeCandidates.push_back(currentPrimitiveIndex);
			if (!ProvenanceEquals(
					retained.primitiveProvenance[retainedPrimitiveIndex],
					exactCurrentGeometry.primitiveProvenance[currentPrimitiveIndex]))
				continue;
			provenanceCandidates.push_back(currentPrimitiveIndex);
			const PrimitiveData& retainedPrimitive = retained.primitives[retainedPrimitiveIndex];
			const PrimitiveData& currentPrimitive = exactCurrentGeometry.primitives[currentPrimitiveIndex];
			if (retainedPrimitive.materialIndex >= retainedSlice.materialCount ||
				currentPrimitive.materialIndex >= retainedSlice.materialCount ||
				retainedPrimitive.materialIndex != currentPrimitive.materialIndex)
			{
				continue;
			}
			materialCandidates.push_back(currentPrimitiveIndex);
			if (retainedPrimitive.flags != currentPrimitive.flags)
				continue;
			flagsCandidates.push_back(currentPrimitiveIndex);
			if (retainedPrimitive.portalIndex != currentPrimitive.portalIndex)
				continue;
			portalCandidates.push_back(currentPrimitiveIndex);
			if (currentPrimitive.reserved0 != UINT32_MAX &&
				retainedPrimitive.reserved0 != currentPrimitive.reserved0)
			{
				continue;
			}
			reservedCandidates.push_back(currentPrimitiveIndex);
		}

		if (shapeCandidates.empty())
		{
			Reject(result, MapDeformerLayoutReject_UnmatchedTriangle);
			return result;
		}
		if (provenanceCandidates.empty())
		{
			Reject(result, MapDeformerLayoutReject_Provenance);
			return result;
		}
		if (materialCandidates.empty())
		{
			Reject(result, MapDeformerLayoutReject_MaterialSlot);
			return result;
		}
		if (flagsCandidates.empty())
		{
			Reject(result, MapDeformerLayoutReject_Flags);
			return result;
		}
		if (portalCandidates.empty())
		{
			Reject(result, MapDeformerLayoutReject_Portal);
			return result;
		}
		if (reservedCandidates.empty())
		{
			Reject(result, MapDeformerLayoutReject_Reserved);
			return result;
		}

		uint32_t currentPrimitiveIndex = UINT32_MAX;
		std::array<uint32_t, 3> currentCornerForRetained = {};
		CornerMappingScore selectedScore = {};
		bool candidateAmbiguous = false;
		for (uint32_t candidate : reservedCandidates)
		{
			std::array<uint32_t, 3> candidateCorners = {};
			CornerMappingScore candidateScore = {};
			if (!ResolveCornerMapping(
					retained,
					retained.primitives[retainedPrimitiveIndex],
					exactCurrentGeometry,
					exactCurrentGeometry.primitives[candidate],
					retainedToCurrentVertex,
					currentToRetainedVertex,
					candidateCorners,
					candidateScore))
			{
				continue;
			}
			if (currentPrimitiveIndex == UINT32_MAX ||
				BetterCornerMappingScore(candidateScore, selectedScore))
			{
				currentPrimitiveIndex = candidate;
				currentCornerForRetained = candidateCorners;
				selectedScore = candidateScore;
				candidateAmbiguous = false;
			}
			else if (EqualCornerMappingScore(candidateScore, selectedScore))
			{
				candidateAmbiguous = true;
			}
		}
		if (currentPrimitiveIndex == UINT32_MAX || candidateAmbiguous)
		{
			Reject(result, MapDeformerLayoutReject_AmbiguousKey);
			return result;
		}

		const PrimitiveData& retainedPrimitive = retained.primitives[retainedPrimitiveIndex];
		const PrimitiveData& currentPrimitive = exactCurrentGeometry.primitives[currentPrimitiveIndex];
		if (retainedPrimitive.smoothNormals[0] != 0 || retainedPrimitive.smoothNormals[1] != 0 ||
			currentPrimitive.smoothNormals[0] != 0 || currentPrimitive.smoothNormals[1] != 0)
		{
			Reject(result, MapDeformerLayoutReject_SmoothNormals);
			return result;
		}
		if (!IsFinitePrimitivePayload(currentPrimitive))
		{
			Reject(result, MapDeformerLayoutReject_NonFinitePayload);
			return result;
		}

		PrimitiveData& canonicalPrimitive = canonical.primitives[retainedPrimitiveIndex];
		for (uint32_t retainedCorner = 0; retainedCorner < 3; ++retainedCorner)
		{
			const uint32_t retainedVertexIndex = retainedPrimitive.indices[retainedCorner];
			const uint32_t currentCorner = currentCornerForRetained[retainedCorner];
			const uint32_t currentVertexIndex = currentPrimitive.indices[currentCorner];
			const SceneVertex& currentVertex = exactCurrentGeometry.vertices[currentVertexIndex];
			if (!IsFiniteVertexPayload(currentVertex))
			{
				Reject(result, MapDeformerLayoutReject_NonFinitePayload);
				return result;
			}
			if ((retainedToCurrentVertex[retainedVertexIndex] != UINT32_MAX &&
					retainedToCurrentVertex[retainedVertexIndex] != currentVertexIndex) ||
				(currentToRetainedVertex[currentVertexIndex] != UINT32_MAX &&
					currentToRetainedVertex[currentVertexIndex] != retainedVertexIndex))
			{
				Reject(result, MapDeformerLayoutReject_IndexLayout);
				return result;
			}
			retainedToCurrentVertex[retainedVertexIndex] = currentVertexIndex;
			currentToRetainedVertex[currentVertexIndex] = retainedVertexIndex;

			canonical.vertices[retainedVertexIndex] = currentVertex;

			const float* currentUv = PrimitiveUv(currentPrimitive, currentCorner);
			float* canonicalUv = PrimitiveUv(canonicalPrimitive, retainedCorner);
			canonicalUv[0] = currentUv[0];
			canonicalUv[1] = currentUv[1];
		}
		std::copy(std::begin(currentPrimitive.normal), std::end(currentPrimitive.normal), canonicalPrimitive.normal);
		canonicalPrimitive.temporalSurfaceId[0] = currentPrimitive.temporalSurfaceId[0];
		canonicalPrimitive.temporalSurfaceId[1] = currentPrimitive.temporalSurfaceId[1];
		canonicalPrimitive.temporalGeneration = currentPrimitive.temporalGeneration;
		canonicalPrimitive.temporalFlags = currentPrimitive.temporalFlags;
		currentPrimitiveUsed[currentPrimitiveIndex] = 1;
	}

	if (std::find(currentPrimitiveUsed.begin(), currentPrimitiveUsed.end(), 0) != currentPrimitiveUsed.end() ||
		std::find(retainedToCurrentVertex.begin(), retainedToCurrentVertex.end(), UINT32_MAX) != retainedToCurrentVertex.end() ||
		std::find(currentToRetainedVertex.begin(), currentToRetainedVertex.end(), UINT32_MAX) != currentToRetainedVertex.end())
	{
		Reject(result, MapDeformerLayoutReject_IndexLayout);
		return result;
	}

	result.changedVertexSpans = BuildChangedSpans(
		retained.vertices,
		canonical.vertices,
		retainedSlice.vertexOffset,
		result.changedVertexBytes);
	result.changedPrimitiveSpans = BuildChangedSpans(
		retained.primitives,
		canonical.primitives,
		retainedSlice.primitiveOffset,
		result.changedPrimitiveBytes);
	result.changedBytes = result.changedVertexBytes + result.changedPrimitiveBytes;
	result.canonicalCurrent = std::move(canonical);
	result.compatible = true;
	return result;
}

MapDeformerLayoutMapping MapCurrentGeometryToRetainedDeformerLayout(
	const GeometryData& retainedGeometry,
	const MapDeformerGeometrySlice& retainedSlice,
	const GeometryData& exactCurrentGeometry)
{
	MapDeformerLayoutMapping fast = MapCurrentGeometryToRetainedDeformerLayoutFast(
		retainedGeometry,
		retainedSlice,
		exactCurrentGeometry);
	if (fast.compatible ||
		(fast.rejectMask &
			(MapDeformerLayoutReject_UnmatchedTriangle | MapDeformerLayoutReject_AmbiguousKey)) == 0)
	{
		return fast;
	}
	return MapCurrentGeometryByTopologyFallback(
		retainedGeometry,
		retainedSlice,
		exactCurrentGeometry);
}
}
