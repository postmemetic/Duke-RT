#include "voxel_surface_emission.h"

namespace
{
	uint32_t ResolveVoxelExposureMask(uint32_t occupiedNeighborMask)
	{
		return VoxelExposureMaskFromOccupiedNeighbors(occupiedNeighborMask);
	}

	uint32_t BuildOccupancyNormalSpans(
		const uint8_t* colors,
		const uint32_t* occupiedNeighborMasks,
		uint32_t voxelCount,
		uint32_t faceCullBit,
		FVoxelAdjacencySpan* outSpans)
	{
		uint32_t spanCount = 0u;
		uint32_t voxelOffset = 0u;
		while (voxelOffset < voxelCount)
		{
			const uint32_t exposureMask = ResolveVoxelExposureMask(occupiedNeighborMasks[voxelOffset]);
			const uint32_t lateralExposureMask = exposureMask & 15u;
			const uint32_t runLength = VoxelSurfaceRunLength(
				colors, voxelOffset, voxelCount, lateralExposureMask,
				[&](uint32_t candidateOffset)
				{
					return ResolveVoxelExposureMask(occupiedNeighborMasks[candidateOffset]) & 15u;
				});
			outSpans[spanCount] = {
				voxelOffset,
				runLength,
				VoxelSurfaceNormalKey(exposureMask, faceCullBit)
			};
			++spanCount;
			voxelOffset += runLength;
		}
		return spanCount;
	}

	bool OccupiedNeighborSuppressesFalseSideContribution()
	{
		const uint32_t frontCullBit = 4u;
		const uint32_t falseSideCullBit = 1u;
		const uint32_t coarseExposureMask = frontCullBit | falseSideCullBit;
		const uint32_t occupiedNeighborMask = (~frontCullBit) & 0x3fu;
		const uint32_t exposureMask = ResolveVoxelExposureMask(occupiedNeighborMask);
		const uint32_t flatFrontKey = VoxelSurfaceNormalKey(frontCullBit, frontCullBit);
		return
			exposureMask == frontCullBit &&
			VoxelSurfaceNormalKey(exposureMask, frontCullBit) == flatFrontKey &&
			VoxelSurfaceNormalKey(coarseExposureMask, frontCullBit) != flatFrontKey;
	}

	bool MissingNeighborPreservesOuterEdgeDiagonal()
	{
		const uint32_t frontCullBit = 4u;
		const uint32_t outerSideCullBit = 1u;
		const uint32_t outerExposureMask = frontCullBit | outerSideCullBit;
		const uint32_t occupiedNeighborMask = (~outerExposureMask) & 0x3fu;
		const uint32_t exposureMask = ResolveVoxelExposureMask(occupiedNeighborMask);
		const uint32_t edgeKey = VoxelSurfaceNormalKey(exposureMask, frontCullBit);
		return
			exposureMask == outerExposureMask &&
			edgeKey == VoxelSurfaceNormalKey(outerExposureMask, frontCullBit) &&
			edgeKey != VoxelSurfaceNormalKey(frontCullBit, frontCullBit) &&
			edgeKey != VoxelSurfaceNormalKey(outerSideCullBit, frontCullBit);
	}

	bool NormalKeySpansFollowRealOccupancyTransitions()
	{
		const uint32_t frontCullBit = 4u;
		const uint32_t sideCullBit = 1u;
		const uint32_t edgeExposureMask = frontCullBit | sideCullBit;
		const uint32_t flatOccupiedNeighbors = (~frontCullBit) & 0x3fu;
		const uint32_t edgeOccupiedNeighbors = (~edgeExposureMask) & 0x3fu;
		const uint32_t occupiedNeighborMasks[] = {
			edgeOccupiedNeighbors, edgeOccupiedNeighbors,
			flatOccupiedNeighbors, flatOccupiedNeighbors, flatOccupiedNeighbors,
			edgeOccupiedNeighbors
		};
		const uint8_t colors[] = { 7u, 7u, 7u, 7u, 7u, 7u };
		FVoxelAdjacencySpan occupancySpans[6] = {};
		const uint32_t occupancySpanCount = BuildOccupancyNormalSpans(
			colors, occupiedNeighborMasks, 6u, frontCullBit, occupancySpans);
		const uint32_t outerEdgeKey = VoxelSurfaceNormalKey(edgeExposureMask, frontCullBit);
		const uint32_t flatFrontKey = VoxelSurfaceNormalKey(frontCullBit, frontCullBit);

		FVoxelAdjacencySpan coarseSpans[3] = {};
		const uint32_t coarseSpanCount = BuildVoxelAdjacencyNormalSpans(
			edgeExposureMask, 0u, 0u, 6u, 6u, frontCullBit, coarseSpans);

		// The helper-agnostic occupancy oracle deliberately records the behavior
		// that a slab-wide cull mask cannot represent: missing, occupied, then
		// missing side neighbors must form three normal-key spans.
		return
			occupancySpanCount == 3u &&
			occupancySpans[0].zOffset == 0u && occupancySpans[0].zLength == 2u && occupancySpans[0].normalKey == outerEdgeKey &&
			occupancySpans[1].zOffset == 2u && occupancySpans[1].zLength == 3u && occupancySpans[1].normalKey == flatFrontKey &&
			occupancySpans[2].zOffset == 5u && occupancySpans[2].zLength == 1u && occupancySpans[2].normalKey == outerEdgeKey &&
			coarseSpanCount == 1u && coarseSpans[0].zLength == 6u && coarseSpans[0].normalKey == outerEdgeKey;
	}

	bool AppliesCapExposureOnlyAtSlabEndpoints()
	{
		const uint32_t lateralExposureMask = 1u | 4u;
		const uint32_t capExposureMask = 16u | 32u;
		return
			VoxelSurfaceExposureMask(lateralExposureMask, capExposureMask, 0u, 3u) == (lateralExposureMask | 16u) &&
			VoxelSurfaceExposureMask(lateralExposureMask, capExposureMask, 1u, 3u) == lateralExposureMask &&
			VoxelSurfaceExposureMask(lateralExposureMask, capExposureMask, 2u, 3u) == (lateralExposureMask | 32u) &&
			VoxelSurfaceExposureMask(lateralExposureMask, capExposureMask, 0u, 1u) == (lateralExposureMask | capExposureMask);
	}

	bool ValidateEverySpanContract()
	{
		const uint32_t sideCullBits[] = { 1u, 2u, 4u, 8u };
		for (uint32_t cullMask = 0u; cullMask < 64u; ++cullMask)
		{
			for (uint32_t slabLength = 1u; slabLength <= 8u; ++slabLength)
			{
				for (uint32_t runOffset = 0u; runOffset < slabLength; ++runOffset)
				{
					for (uint32_t runLength = 1u; runLength <= slabLength - runOffset; ++runLength)
					{
						for (const uint32_t faceCullBit : sideCullBits)
						{
							FVoxelAdjacencySpan spans[3] = {};
							const uint32_t spanCount = BuildVoxelAdjacencyNormalSpans(
								cullMask & 15u, cullMask & 48u, runOffset, runLength, slabLength, faceCullBit, spans);
							if (spanCount == 0u || spanCount > 3u || spanCount > runLength)
							{
								return false;
							}
							uint32_t cursor = runOffset;
							for (uint32_t spanIndex = 0u; spanIndex < spanCount; ++spanIndex)
							{
								const FVoxelAdjacencySpan& span = spans[spanIndex];
								if (span.zOffset != cursor || span.zLength == 0u ||
									(spanIndex != 0u && spans[spanIndex - 1u].normalKey == span.normalKey))
								{
									return false;
								}
								for (uint32_t voxel = span.zOffset; voxel < span.zOffset + span.zLength; ++voxel)
								{
									const uint32_t exposureMask = VoxelSurfaceExposureMask(
										cullMask & 15u, cullMask & 48u, voxel, slabLength);
									if (VoxelSurfaceNormalKey(exposureMask, faceCullBit) != span.normalKey)
									{
										return false;
									}
								}
								cursor += span.zLength;
							}
							if (cursor != runOffset + runLength)
							{
								return false;
							}
						}
					}
				}
			}
		}
		return true;
	}

	bool HandlesNormalTransitionsAndFallbacks()
	{
		return
			CountVoxelAdjacencyNormalSpans(1u, 0u, 0u, 8u, 8u, 1u) == 1u &&
			CountVoxelAdjacencyNormalSpans(1u, 16u | 32u, 0u, 8u, 8u, 1u) == 3u &&
			CountVoxelAdjacencyNormalSpans(1u | 2u, 16u | 32u, 0u, 8u, 8u, 1u) == 1u &&
			CountVoxelAdjacencyNormalSpans(1u, 16u | 32u, 0u, 1u, 1u, 1u) == 1u &&
			CountVoxelAdjacencyNormalSpans(1u, 16u | 32u, 0u, 2u, 2u, 1u) == 2u;
	}

	bool PreservesGeometryCountBounds()
	{
		const uint32_t cullMask = 1u | 2u | 4u | 16u | 32u;
		const uint32_t slabLength = 9u;
		const uint32_t runOffsets[] = { 0u, 2u, 7u };
		const uint32_t runLengths[] = { 2u, 5u, 2u };
		const uint32_t sideCullBits[] = { 1u, 2u, 4u };
		uint32_t adjacencyFaces = 2u;
		for (uint32_t run = 0u; run < 3u; ++run)
		{
			for (const uint32_t faceCullBit : sideCullBits)
			{
				adjacencyFaces += CountVoxelAdjacencyNormalSpans(
					cullMask & 15u, cullMask & 48u, runOffsets[run], runLengths[run], slabLength, faceCullBit);
			}
		}
		const uint32_t legacyColorRunFaces = 2u + 3u * 3u;
		const uint32_t unitFaces = CountVoxelUnitSurfaceFaces(cullMask, slabLength);
		return legacyColorRunFaces <= adjacencyFaces && adjacencyFaces <= unitFaces;
	}

	bool RejectsInvalidRanges()
	{
		return
			CountVoxelAdjacencyNormalSpans(1u, 0u, 0u, 0u, 4u, 1u) == 0u &&
			CountVoxelAdjacencyNormalSpans(1u, 0u, 5u, 1u, 4u, 1u) == 0u &&
			CountVoxelAdjacencyNormalSpans(1u, 0u, 3u, 2u, 4u, 1u) == 0u;
	}
}

int main()
{
	return OccupiedNeighborSuppressesFalseSideContribution() &&
		MissingNeighborPreservesOuterEdgeDiagonal() &&
		NormalKeySpansFollowRealOccupancyTransitions() &&
		AppliesCapExposureOnlyAtSlabEndpoints() &&
		ValidateEverySpanContract() &&
		HandlesNormalTransitionsAndFallbacks() &&
		PreservesGeometryCountBounds() &&
		RejectsInvalidRanges() ? 0 : 1;
}
