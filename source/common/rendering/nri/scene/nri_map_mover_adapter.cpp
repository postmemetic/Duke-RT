#include "nri_map_mover_adapter.h"

#ifndef NRI_MAP_MOVER_ADAPTER_VIEW_ONLY
#include "nri_map_world.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <tuple>
#include <utility>

namespace
{
	using namespace nri_scene;

	constexpr uint64_t HashOffset = 1469598103934665603ull;
	constexpr uint64_t HashPrime = 1099511628211ull;
	constexpr uint64_t PlaneCornerHashDomain = 0x504c414e45505431ull;

	enum : uint32_t
	{
		MapSurfaceFloor = 0,
		MapSurfaceCeiling = 1,
		MapSurfaceWallOneSided = 2,
		MapSurfaceWallUpper = 3,
		MapSurfaceWallMiddle = 4,
		MapSurfaceWallLower = 5,
		MapSurfacePortal = 6,
	};

	void SetFailure(
		PTMapMoverAdapterValidation& validation,
		PTMapMoverAdapterFailure failure,
		uint32_t surfaceIndex = UINT32_MAX,
		uint32_t vertexIndex = UINT32_MAX,
		uint32_t primitiveIndex = UINT32_MAX,
		double nearestAuthoredCornerDistance = std::numeric_limits<double>::infinity())
	{
		validation = {};
		validation.failure = failure;
		validation.surfaceIndex = surfaceIndex;
		validation.vertexIndex = vertexIndex;
		validation.primitiveIndex = primitiveIndex;
		validation.nearestAuthoredCornerDistance = nearestAuthoredCornerDistance;
	}

	template<class T>
	uint64_t HashValue(uint64_t hash, const T& value)
	{
		const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
		for (size_t i = 0; i < sizeof(value); ++i)
		{
			hash = (hash ^ bytes[i]) * HashPrime;
		}
		return hash;
	}

	bool IsFiniteVertex(const CapturedVertex& vertex)
	{
		return std::isfinite(vertex.position[0]) &&
			std::isfinite(vertex.position[1]) &&
			std::isfinite(vertex.position[2]) &&
			std::isfinite(vertex.uv[0]) &&
			std::isfinite(vertex.uv[1]);
	}

	bool Near(double a, double b, double tolerance)
	{
		return std::abs(a - b) <= tolerance;
	}

	double HorizontalDistance(const CapturedVertex& vertex, const double position[2])
	{
		return std::hypot(
			(double)vertex.position[0] - position[0],
			(double)vertex.position[2] - position[1]);
	}

	bool HorizontalNear(
		const CapturedVertex& vertex,
		const double position[2],
		const PTMapMoverAdapterOptions& options)
	{
		return Near(
			(double)vertex.position[0],
			position[0],
			ComputePTMapMoverAuthoredPositionTolerance(vertex.position[0], position[0], options)) &&
			Near(
				(double)vertex.position[2],
				position[1],
				ComputePTMapMoverAuthoredPositionTolerance(vertex.position[2], position[1], options));
	}

	bool SameVertexAttributes(
		const WorldMapMoverVertex& a,
		const WorldMapMoverVertex& b,
		double tolerance)
	{
		return Near(a.position.value[0], b.position.value[0], tolerance) &&
			Near(a.position.value[1], b.position.value[1], tolerance) &&
			Near(a.position.value[2], b.position.value[2], tolerance) &&
			Near(a.uv[0], b.uv[0], tolerance) &&
			Near(a.uv[1], b.uv[1], tolerance);
	}

	WorldMapMoverVertex MakeWorldVertex(const CapturedVertex& source, uint64_t stableCornerId)
	{
		WorldMapMoverVertex result = {};
		result.stableCornerId = stableCornerId;
		result.position.value[0] = source.position[0];
		result.position.value[1] = source.position[1];
		result.position.value[2] = source.position[2];
		result.uv[0] = source.uv[0];
		result.uv[1] = source.uv[1];
		return result;
	}

	uint64_t BuildMaterialSlotKey(const MapMoverSurfaceProvenance& provenance)
	{
		uint64_t hash = HashOffset;
		hash = HashValue(hash, provenance.sourceType);
		hash = HashValue(hash, provenance.sectorIndex);
		hash = HashValue(hash, provenance.wallIndex);
		hash = HashValue(hash, provenance.sectionIndex);
		hash = HashValue(hash, provenance.surfaceKind);
		return HashValue(hash, provenance.stableSubSurfaceId);
	}

	bool BuildMaterialStateKey(
		const MaterialRef& material,
		const PTMapMoverAdapterOptions& options,
		uint64_t& outKey)
	{
		if (!std::isfinite(material.alpha))
		{
			return false;
		}

		auto resolveTexture = [&](const FGameTexture* texture, uint64_t& outIdentity)
		{
			if (texture == nullptr)
			{
				outIdentity = 0;
				return true;
			}
			return options.resolveTextureIdentity != nullptr &&
				options.resolveTextureIdentity(texture, outIdentity, options.textureIdentityUserData);
		};

		uint64_t textureIdentity = 0;
		uint64_t emissiveIdentity = 0;
		if (!resolveTexture(material.texture, textureIdentity) ||
			!resolveTexture(material.emissiveSourceTexture, emissiveIdentity))
		{
			return false;
		}

		uint32_t alphaBits = 0;
		const float canonicalAlpha = material.alpha == 0.0f ? 0.0f : material.alpha;
		std::memcpy(&alphaBits, &canonicalAlpha, sizeof(alphaBits));

		uint64_t hash = HashOffset;
		hash = HashValue(hash, textureIdentity);
		hash = HashValue(hash, emissiveIdentity);
		hash = HashValue(hash, material.authoredTextureId);
		hash = HashValue(hash, material.palette);
		hash = HashValue(hash, material.shade);
		hash = HashValue(hash, alphaBits);
		hash = HashValue(hash, material.flags);
		outKey = hash;
		return true;
	}

	bool IsPlaneKind(uint32_t kind)
	{
		return kind == MapSurfaceFloor || kind == MapSurfaceCeiling;
	}

	bool IsWallKind(uint32_t kind)
	{
		return kind >= MapSurfaceWallOneSided && kind <= MapSurfacePortal;
	}

	bool BuildSurfaceProvenance(
		const PTMapMoverChunkView& chunkView,
		const PTMapMoverSurfaceView& source,
		MapMoverSurfaceProvenance& outProvenance)
	{
		if (source.surface == nullptr || source.chunkIndex != chunkView.chunkIndex)
		{
			return false;
		}

		const SurfaceProvenance& provenance = source.surface->provenance;
		if ((provenance.mapChunkIndex >= 0 && (uint32_t)provenance.mapChunkIndex != chunkView.chunkIndex) ||
			(provenance.sectorIndex >= 0 && chunkView.sectorIndex >= 0 && provenance.sectorIndex != chunkView.sectorIndex))
		{
			return false;
		}

		if (IsPlaneKind(source.surfaceKind))
		{
			const uint32_t expectedPlane = source.surfaceKind == MapSurfaceFloor ? 0u : 1u;
			if (provenance.sectionIndex < 0 ||
				source.keyPrimary != (uint32_t)provenance.sectionIndex ||
				source.keySecondary != expectedPlane)
			{
				return false;
			}
		}
		else if (IsWallKind(source.surfaceKind))
		{
			if (provenance.wallIndex < 0 || source.keyPrimary != (uint32_t)provenance.wallIndex)
			{
				return false;
			}
		}
		else
		{
			return false;
		}

		outProvenance.sourceType = (uint32_t)provenance.sourceType;
		outProvenance.sectorIndex = provenance.sectorIndex;
		outProvenance.wallIndex = provenance.wallIndex;
		outProvenance.sectionIndex = provenance.sectionIndex;
		outProvenance.surfaceKind = source.surfaceKind;
		outProvenance.stableSubSurfaceId = source.keySecondary;
		return true;
	}

	uint64_t BuildPlaneCornerId(int32_t sectionIndex, const std::vector<int32_t>& wallPointIds)
	{
		uint64_t hash = HashOffset;
		hash = HashValue(hash, PlaneCornerHashDomain);
		hash = HashValue(hash, sectionIndex);
		const uint32_t count = (uint32_t)wallPointIds.size();
		hash = HashValue(hash, count);
		for (int32_t wallPointId : wallPointIds)
		{
			hash = HashValue(hash, wallPointId);
		}
		return hash == InvalidMapMoverStableId ? hash - 1 : hash;
	}

	bool ResolvePlaneCornerId(
		const CapturedVertex& vertex,
		int32_t sectionIndex,
		const PTMapMoverAuthoredTopology& topology,
		const PTMapMoverAdapterOptions& options,
		uint64_t& outStableId,
		PTMapMoverAdapterFailure& outFailure,
		double& outNearestDistance)
	{
		std::vector<int32_t> matchingPointIds;
		outNearestDistance = std::numeric_limits<double>::infinity();
		for (const PTMapMoverAuthoredPlanePoint& point : topology.planePoints)
		{
			if (point.sectionIndex != sectionIndex)
			{
				continue;
			}
			if (point.wallPointId < 0 || !std::isfinite(point.position[0]) || !std::isfinite(point.position[1]))
			{
				outFailure = PTMapMoverAdapterFailure::AmbiguousAuthoredCorner;
				return false;
			}
			outNearestDistance = std::min(outNearestDistance, HorizontalDistance(vertex, point.position));
			if (HorizontalNear(vertex, point.position, options))
			{
				matchingPointIds.push_back(point.wallPointId);
			}
		}

		if (matchingPointIds.empty())
		{
			outFailure = PTMapMoverAdapterFailure::MissingAuthoredCorner;
			return false;
		}
		std::sort(matchingPointIds.begin(), matchingPointIds.end());
		matchingPointIds.erase(std::unique(matchingPointIds.begin(), matchingPointIds.end()), matchingPointIds.end());
		outStableId = BuildPlaneCornerId(sectionIndex, matchingPointIds);
		outFailure = PTMapMoverAdapterFailure::None;
		return true;
	}

	bool BuildPlaneSurface(
		const PTMapMoverSurfaceView& sourceView,
		const PTMapMoverAuthoredTopology& topology,
		const PTMapMoverAdapterOptions& options,
		WorldMapMoverSurface& outSurface,
		PTMapMoverAdapterValidation& validation,
		uint32_t surfaceIndex)
	{
		const SurfaceRef& source = *sourceView.surface;
		if (source.vertices.empty())
		{
			SetFailure(validation, PTMapMoverAdapterFailure::InvalidVertexLayout, surfaceIndex);
			return false;
		}

		std::vector<uint32_t> sourceToOutput(source.vertices.size(), UINT32_MAX);
		std::vector<uint64_t> sourceStableIds(source.vertices.size(), InvalidMapMoverStableId);
		for (uint32_t vertexIndex = 0; vertexIndex < source.vertices.size(); ++vertexIndex)
		{
			const CapturedVertex& captured = source.vertices[vertexIndex];
			if (!IsFiniteVertex(captured))
			{
				SetFailure(validation, PTMapMoverAdapterFailure::NonFiniteValue, surfaceIndex, vertexIndex);
				return false;
			}

			PTMapMoverAdapterFailure cornerFailure = PTMapMoverAdapterFailure::None;
			uint64_t stableId = InvalidMapMoverStableId;
			double nearestAuthoredCornerDistance = std::numeric_limits<double>::infinity();
			if (!ResolvePlaneCornerId(
				captured,
				source.provenance.sectionIndex,
				topology,
				options,
				stableId,
				cornerFailure,
				nearestAuthoredCornerDistance))
			{
				SetFailure(
					validation,
					cornerFailure,
					surfaceIndex,
					vertexIndex,
					UINT32_MAX,
					nearestAuthoredCornerDistance);
				return false;
			}
			sourceStableIds[vertexIndex] = stableId;

			const WorldMapMoverVertex candidate = MakeWorldVertex(captured, stableId);
			auto existing = std::find_if(
				outSurface.vertices.begin(),
				outSurface.vertices.end(),
				[stableId](const WorldMapMoverVertex& vertex) { return vertex.stableCornerId == stableId; });
			if (existing == outSurface.vertices.end())
			{
				sourceToOutput[vertexIndex] = (uint32_t)outSurface.vertices.size();
				outSurface.vertices.push_back(candidate);
			}
			else
			{
				if (!SameVertexAttributes(*existing, candidate, options.duplicateAttributeTolerance))
				{
					SetFailure(validation, PTMapMoverAdapterFailure::AmbiguousAuthoredCorner, surfaceIndex, vertexIndex);
					return false;
				}
				sourceToOutput[vertexIndex] = (uint32_t)std::distance(outSurface.vertices.begin(), existing);
			}
		}

		auto appendPrimitive = [&](uint32_t primitiveIndex, uint32_t a, uint32_t b, uint32_t c)
		{
			if (a >= sourceToOutput.size() || b >= sourceToOutput.size() || c >= sourceToOutput.size())
			{
				SetFailure(validation, PTMapMoverAdapterFailure::InvalidPrimitiveLayout, surfaceIndex, UINT32_MAX, primitiveIndex);
				return false;
			}
			const uint32_t mappedA = sourceToOutput[a];
			const uint32_t mappedB = sourceToOutput[b];
			const uint32_t mappedC = sourceToOutput[c];
			if (mappedA == mappedB || mappedB == mappedC || mappedC == mappedA)
			{
				SetFailure(validation, PTMapMoverAdapterFailure::InvalidPrimitiveLayout, surfaceIndex, UINT32_MAX, primitiveIndex);
				return false;
			}
			outSurface.triangles.push_back({ { mappedA, mappedB, mappedC } });
			return true;
		};

		if (!source.indices.empty())
		{
			if ((source.indices.size() % 3) != 0)
			{
				SetFailure(validation, PTMapMoverAdapterFailure::InvalidPrimitiveLayout, surfaceIndex);
				return false;
			}
			for (uint32_t i = 0; i < source.indices.size(); i += 3)
			{
				if (!appendPrimitive(i / 3, source.indices[i], source.indices[i + 1], source.indices[i + 2]))
				{
					return false;
				}
			}
		}
		else
		{
			if ((source.vertices.size() % 3) != 0)
			{
				SetFailure(validation, PTMapMoverAdapterFailure::InvalidPrimitiveLayout, surfaceIndex);
				return false;
			}
			for (uint32_t i = 0; i < source.vertices.size(); i += 3)
			{
				if (!appendPrimitive(i / 3, i, i + 1, i + 2))
				{
					return false;
				}
			}
		}

		return !outSurface.triangles.empty();
	}

	std::array<uint64_t, 3> NormalizeOrientedTriangle(std::array<uint64_t, 3> triangle)
	{
		std::array<uint64_t, 3> rotation1 = { triangle[1], triangle[2], triangle[0] };
		std::array<uint64_t, 3> rotation2 = { triangle[2], triangle[0], triangle[1] };
		return std::min(triangle, std::min(rotation1, rotation2));
	}

	bool BuildWallSurface(
		const PTMapMoverSurfaceView& sourceView,
		const PTMapMoverAuthoredTopology& topology,
		const PTMapMoverAdapterOptions& options,
		WorldMapMoverSurface& outSurface,
		PTMapMoverAdapterValidation& validation,
		uint32_t surfaceIndex)
	{
		const SurfaceRef& source = *sourceView.surface;
		if (source.vertices.size() != 4)
		{
			SetFailure(validation, PTMapMoverAdapterFailure::InvalidVertexLayout, surfaceIndex);
			return false;
		}

		const int32_t wallIndex = source.provenance.wallIndex;
		const PTMapMoverAuthoredWall* authoredWall = nullptr;
		for (const PTMapMoverAuthoredWall& candidate : topology.walls)
		{
			if (candidate.wallIndex != wallIndex)
			{
				continue;
			}
			if (authoredWall != nullptr)
			{
				SetFailure(validation, PTMapMoverAdapterFailure::AmbiguousAuthoredCorner, surfaceIndex);
				return false;
			}
			authoredWall = &candidate;
		}
		if (authoredWall == nullptr)
		{
			SetFailure(validation, PTMapMoverAdapterFailure::MissingAuthoredWall, surfaceIndex);
			return false;
		}
		if (authoredWall->startPointId < 0 || authoredWall->endPointId < 0 ||
			authoredWall->startPointId == authoredWall->endPointId ||
			!std::isfinite(authoredWall->startPosition[0]) || !std::isfinite(authoredWall->startPosition[1]) ||
			!std::isfinite(authoredWall->endPosition[0]) || !std::isfinite(authoredWall->endPosition[1]) ||
			(Near(authoredWall->startPosition[0], authoredWall->endPosition[0], options.authoredPositionTolerance) &&
				Near(authoredWall->startPosition[1], authoredWall->endPosition[1], options.authoredPositionTolerance)))
		{
			SetFailure(validation, PTMapMoverAdapterFailure::AmbiguousAuthoredCorner, surfaceIndex);
			return false;
		}

		std::vector<uint32_t> startVertices;
		std::vector<uint32_t> endVertices;
		for (uint32_t vertexIndex = 0; vertexIndex < source.vertices.size(); ++vertexIndex)
		{
			const CapturedVertex& vertex = source.vertices[vertexIndex];
			if (!IsFiniteVertex(vertex))
			{
				SetFailure(validation, PTMapMoverAdapterFailure::NonFiniteValue, surfaceIndex, vertexIndex);
				return false;
			}
			const double startDistance = HorizontalDistance(vertex, authoredWall->startPosition);
			const double endDistance = HorizontalDistance(vertex, authoredWall->endPosition);
			const double nearestAuthoredCornerDistance = std::min(startDistance, endDistance);
			const bool matchesStart = HorizontalNear(vertex, authoredWall->startPosition, options);
			const bool matchesEnd = HorizontalNear(vertex, authoredWall->endPosition, options);
			if (matchesStart == matchesEnd)
			{
				SetFailure(
					validation,
					matchesStart ? PTMapMoverAdapterFailure::AmbiguousAuthoredCorner : PTMapMoverAdapterFailure::MissingAuthoredCorner,
					surfaceIndex,
					vertexIndex,
					UINT32_MAX,
					nearestAuthoredCornerDistance);
				return false;
			}
			(matchesStart ? startVertices : endVertices).push_back(vertexIndex);
		}
		if (startVertices.size() != 2 || endVertices.size() != 2)
		{
			SetFailure(validation, PTMapMoverAdapterFailure::InvalidVertexLayout, surfaceIndex);
			return false;
		}

		auto sortBottomTop = [&](std::vector<uint32_t>& endpointVertices)
		{
			std::sort(
				endpointVertices.begin(),
				endpointVertices.end(),
				[&](uint32_t a, uint32_t b) { return source.vertices[a].position[1] < source.vertices[b].position[1]; });
			return !Near(
				(double)source.vertices[endpointVertices[0]].position[1],
				(double)source.vertices[endpointVertices[1]].position[1],
				options.authoredPositionTolerance);
		};
		if (!sortBottomTop(startVertices) || !sortBottomTop(endVertices))
		{
			SetFailure(validation, PTMapMoverAdapterFailure::AmbiguousAuthoredCorner, surfaceIndex);
			return false;
		}

		const uint64_t startBottomId = (uint64_t)(uint32_t)authoredWall->startPointId << 1;
		const uint64_t startTopId = startBottomId | 1ull;
		const uint64_t endBottomId = (uint64_t)(uint32_t)authoredWall->endPointId << 1;
		const uint64_t endTopId = endBottomId | 1ull;
		std::vector<uint64_t> sourceStableIds(source.vertices.size(), InvalidMapMoverStableId);
		sourceStableIds[startVertices[0]] = startBottomId;
		sourceStableIds[startVertices[1]] = startTopId;
		sourceStableIds[endVertices[0]] = endBottomId;
		sourceStableIds[endVertices[1]] = endTopId;
		for (uint32_t vertexIndex = 0; vertexIndex < source.vertices.size(); ++vertexIndex)
		{
			outSurface.vertices.push_back(MakeWorldVertex(source.vertices[vertexIndex], sourceStableIds[vertexIndex]));
		}

		const std::array<std::array<uint64_t, 3>, 2> expected =
		{
			NormalizeOrientedTriangle({ startBottomId, startTopId, endTopId }),
			NormalizeOrientedTriangle({ startBottomId, endTopId, endBottomId }),
		};

		if (source.indices.empty())
		{
			outSurface.triangles.push_back({ { startVertices[0], startVertices[1], endVertices[1] } });
			outSurface.triangles.push_back({ { startVertices[0], endVertices[1], endVertices[0] } });
			return true;
		}
		if (source.indices.size() != 6)
		{
			SetFailure(validation, PTMapMoverAdapterFailure::InvalidPrimitiveLayout, surfaceIndex);
			return false;
		}

		std::vector<std::array<uint64_t, 3>> actual;
		for (uint32_t i = 0; i < source.indices.size(); i += 3)
		{
			const uint32_t a = source.indices[i];
			const uint32_t b = source.indices[i + 1];
			const uint32_t c = source.indices[i + 2];
			if (a >= source.vertices.size() || b >= source.vertices.size() || c >= source.vertices.size())
			{
				SetFailure(validation, PTMapMoverAdapterFailure::InvalidPrimitiveLayout, surfaceIndex, UINT32_MAX, i / 3);
				return false;
			}
			actual.push_back(NormalizeOrientedTriangle({ sourceStableIds[a], sourceStableIds[b], sourceStableIds[c] }));
			outSurface.triangles.push_back({ { a, b, c } });
		}
		auto expectedSorted = expected;
		std::sort(actual.begin(), actual.end());
		std::sort(expectedSorted.begin(), expectedSorted.end());
		if (!std::equal(actual.begin(), actual.end(), expectedSorted.begin(), expectedSorted.end()))
		{
			SetFailure(validation, PTMapMoverAdapterFailure::InvalidPrimitiveLayout, surfaceIndex);
			return false;
		}
		return true;
	}
}

namespace nri_scene
{
double ComputePTMapMoverAuthoredPositionTolerance(
	double capturedPosition,
	double authoredPosition,
	const PTMapMoverAdapterOptions& options)
{
	if (!std::isfinite(capturedPosition) || !std::isfinite(authoredPosition) ||
		!std::isfinite(options.authoredPositionTolerance) || options.authoredPositionTolerance < 0.0 ||
		!std::isfinite(options.authoredPositionFloatUlpScale) || options.authoredPositionFloatUlpScale < 0.0)
	{
		return std::numeric_limits<double>::quiet_NaN();
	}

	const double magnitude = std::max(std::abs(capturedPosition), std::abs(authoredPosition));
	const float floatMagnitude = (float)magnitude;
	double floatUlp = 0.0;
	if (std::isfinite(floatMagnitude))
	{
		const float nextMagnitude = std::nextafter(floatMagnitude, std::numeric_limits<float>::infinity());
		if (std::isfinite(nextMagnitude))
		{
			floatUlp = (double)nextMagnitude - (double)floatMagnitude;
		}
	}
	return options.authoredPositionTolerance + options.authoredPositionFloatUlpScale * floatUlp;
}

bool BuildWorldMapMoverGeometryFromPTMapChunkView(
	const PTMapMoverChunkView& chunkView,
	const PTMapMoverAuthoredTopology& authoredTopology,
	WorldMapMoverGeometry& outGeometry,
	PTMapMoverAdapterValidation& outValidation,
	const PTMapMoverAdapterOptions& options)
{
	outGeometry = {};
	outValidation = {};
	if (chunkView.surfaces.empty())
	{
		SetFailure(outValidation, PTMapMoverAdapterFailure::EmptyChunk);
		return false;
	}
	if (!std::isfinite(options.authoredPositionTolerance) || options.authoredPositionTolerance < 0.0 ||
		!std::isfinite(options.authoredPositionFloatUlpScale) || options.authoredPositionFloatUlpScale < 0.0 ||
		!std::isfinite(options.duplicateAttributeTolerance) || options.duplicateAttributeTolerance < 0.0)
	{
		SetFailure(outValidation, PTMapMoverAdapterFailure::NonFiniteValue);
		return false;
	}

	outGeometry.surfaces.reserve(chunkView.surfaces.size());
	for (uint32_t surfaceIndex = 0; surfaceIndex < chunkView.surfaces.size(); ++surfaceIndex)
	{
		const PTMapMoverSurfaceView& sourceView = chunkView.surfaces[surfaceIndex];
		if (sourceView.surface == nullptr)
		{
			SetFailure(outValidation, PTMapMoverAdapterFailure::InvalidSurfaceReference, surfaceIndex);
			outGeometry = {};
			return false;
		}
		if (!IsPlaneKind(sourceView.surfaceKind) && !IsWallKind(sourceView.surfaceKind))
		{
			SetFailure(outValidation, PTMapMoverAdapterFailure::UnsupportedSurfaceKind, surfaceIndex);
			outGeometry = {};
			return false;
		}

		WorldMapMoverSurface converted = {};
		if (!BuildSurfaceProvenance(chunkView, sourceView, converted.provenance))
		{
			SetFailure(outValidation, PTMapMoverAdapterFailure::InvalidSurfaceIdentity, surfaceIndex);
			outGeometry = {};
			return false;
		}
		for (const WorldMapMoverSurface& existing : outGeometry.surfaces)
		{
			if (existing.provenance == converted.provenance)
			{
				SetFailure(outValidation, PTMapMoverAdapterFailure::DuplicateSurfaceIdentity, surfaceIndex);
				outGeometry = {};
				return false;
			}
		}
		converted.materialSlotKey = BuildMaterialSlotKey(converted.provenance);
		if (!BuildMaterialStateKey(sourceView.surface->material, options, converted.materialStateKey))
		{
			SetFailure(
				outValidation,
				std::isfinite(sourceView.surface->material.alpha)
					? PTMapMoverAdapterFailure::MissingMaterialIdentity
					: PTMapMoverAdapterFailure::NonFiniteValue,
				surfaceIndex);
			outGeometry = {};
			return false;
		}

		const bool built = IsPlaneKind(sourceView.surfaceKind)
			? BuildPlaneSurface(sourceView, authoredTopology, options, converted, outValidation, surfaceIndex)
			: BuildWallSurface(sourceView, authoredTopology, options, converted, outValidation, surfaceIndex);
		if (!built)
		{
			if (outValidation.failure == PTMapMoverAdapterFailure::None)
			{
				SetFailure(outValidation, PTMapMoverAdapterFailure::InvalidPrimitiveLayout, surfaceIndex);
			}
			outGeometry = {};
			return false;
		}
		outGeometry.surfaces.push_back(std::move(converted));
	}

	outValidation.valid = true;
	outValidation.failure = PTMapMoverAdapterFailure::None;
	return true;
}

#ifndef NRI_MAP_MOVER_ADAPTER_VIEW_ONLY
bool BuildWorldMapMoverGeometryFromPTMapChunk(
	const PTMapWorld& mapWorld,
	const PTMapChunk& chunk,
	const PTMapMoverAuthoredTopology& authoredTopology,
	WorldMapMoverGeometry& outGeometry,
	PTMapMoverAdapterValidation& outValidation,
	const PTMapMoverAdapterOptions& options)
{
	if (chunk.firstSurface > mapWorld.surfaces.size() ||
		chunk.surfaceCount > mapWorld.surfaces.size() - chunk.firstSurface)
	{
		outGeometry = {};
		SetFailure(outValidation, PTMapMoverAdapterFailure::InvalidChunkRange);
		return false;
	}

	PTMapMoverChunkView view = {};
	view.chunkIndex = chunk.chunkIndex;
	view.sectorIndex = chunk.sectorIndex;
	view.surfaces.reserve(chunk.surfaceCount);
	for (uint32_t offset = 0; offset < chunk.surfaceCount; ++offset)
	{
		const PTMapSurface& mapSurface = mapWorld.surfaces[chunk.firstSurface + offset];
		PTMapMoverSurfaceView surfaceView = {};
		surfaceView.surface = &mapSurface.surface;
		surfaceView.surfaceKind = (uint32_t)mapSurface.kind;
		surfaceView.keyPrimary = mapSurface.key.primary;
		surfaceView.keySecondary = mapSurface.key.secondary;
		surfaceView.chunkIndex = mapSurface.chunkIndex;
		view.surfaces.push_back(surfaceView);
	}
	return BuildWorldMapMoverGeometryFromPTMapChunkView(
		view,
		authoredTopology,
		outGeometry,
		outValidation,
		options);
}
#endif

const char* GetPTMapMoverAdapterFailureName(PTMapMoverAdapterFailure failure)
{
	switch (failure)
	{
	case PTMapMoverAdapterFailure::None: return "none";
	case PTMapMoverAdapterFailure::EmptyChunk: return "empty_chunk";
	case PTMapMoverAdapterFailure::InvalidChunkRange: return "invalid_chunk_range";
	case PTMapMoverAdapterFailure::InvalidSurfaceReference: return "invalid_surface_reference";
	case PTMapMoverAdapterFailure::InvalidSurfaceIdentity: return "invalid_surface_identity";
	case PTMapMoverAdapterFailure::DuplicateSurfaceIdentity: return "duplicate_surface_identity";
	case PTMapMoverAdapterFailure::UnsupportedSurfaceKind: return "unsupported_surface_kind";
	case PTMapMoverAdapterFailure::MissingAuthoredWall: return "missing_authored_wall";
	case PTMapMoverAdapterFailure::MissingAuthoredCorner: return "missing_authored_corner";
	case PTMapMoverAdapterFailure::AmbiguousAuthoredCorner: return "ambiguous_authored_corner";
	case PTMapMoverAdapterFailure::InvalidVertexLayout: return "invalid_vertex_layout";
	case PTMapMoverAdapterFailure::InvalidPrimitiveLayout: return "invalid_primitive_layout";
	case PTMapMoverAdapterFailure::NonFiniteValue: return "non_finite_value";
	case PTMapMoverAdapterFailure::MissingMaterialIdentity: return "missing_material_identity";
	}
	return "unknown";
}
}
