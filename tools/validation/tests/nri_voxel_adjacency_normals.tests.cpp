#include "voxel_surface_emission.h"

namespace
{
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
								cullMask, runOffset, runLength, slabLength, faceCullBit, spans);
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
									if (VoxelSurfaceNormalKey(cullMask, voxel, slabLength, faceCullBit) != span.normalKey)
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
			CountVoxelAdjacencyNormalSpans(1u, 0u, 8u, 8u, 1u) == 1u &&
			CountVoxelAdjacencyNormalSpans(1u | 16u | 32u, 0u, 8u, 8u, 1u) == 3u &&
			CountVoxelAdjacencyNormalSpans(1u | 2u | 16u | 32u, 0u, 8u, 8u, 1u) == 1u &&
			CountVoxelAdjacencyNormalSpans(1u | 16u | 32u, 0u, 1u, 1u, 1u) == 1u &&
			CountVoxelAdjacencyNormalSpans(1u | 16u | 32u, 0u, 2u, 2u, 1u) == 2u;
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
					cullMask, runOffsets[run], runLengths[run], slabLength, faceCullBit);
			}
		}
		const uint32_t legacyColorRunFaces = 2u + 3u * 3u;
		const uint32_t unitFaces = CountVoxelUnitSurfaceFaces(cullMask, slabLength);
		return legacyColorRunFaces <= adjacencyFaces && adjacencyFaces <= unitFaces;
	}

	bool RejectsInvalidRanges()
	{
		return
			CountVoxelAdjacencyNormalSpans(1u, 0u, 0u, 4u, 1u) == 0u &&
			CountVoxelAdjacencyNormalSpans(1u, 5u, 1u, 4u, 1u) == 0u &&
			CountVoxelAdjacencyNormalSpans(1u, 3u, 2u, 4u, 1u) == 0u;
	}
}

int main()
{
	return ValidateEverySpanContract() &&
		HandlesNormalTransitionsAndFallbacks() &&
		PreservesGeometryCountBounds() &&
		RejectsInvalidRanges() ? 0 : 1;
}
