#include "nri_spatial_absence_gpu_snapshot.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	constexpr uint64_t HashOffset = 1469598103934665603ull;
	constexpr uint64_t HashPrime = 1099511628211ull;
	constexpr uint32_t MaxSerializedFloatInteger = 1u << 24u;
	constexpr uint32_t MaxFootprintGridDimension = 32u;
	constexpr uint32_t InvalidIndex = UINT32_MAX;

	constexpr uint32_t RawRequiredBaseFlags =
		NRI_SPATIAL_ABSENCE_GPU_VALID | NRI_SPATIAL_ABSENCE_GPU_COMPLETE;
	constexpr uint32_t RawRequiredFootprintFlags =
		RawRequiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_FOOTPRINT;
	constexpr uint32_t RawRequiredGridFlags =
		RawRequiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_GRID;
	constexpr uint32_t RawRequiredNegativeFlags =
		RawRequiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_CERTIFIED |
		NRI_SPATIAL_ABSENCE_GPU_FOOTPRINT | NRI_SPATIAL_ABSENCE_GPU_GRID;
	constexpr uint32_t RawRequiredPairFlags =
		RawRequiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_CERTIFIED |
		NRI_SPATIAL_ABSENCE_GPU_PAIR;
	constexpr uint32_t RawRequiredCellFlags =
		RawRequiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_GRID |
		NRI_SPATIAL_ABSENCE_GPU_GRID_CELL;
	constexpr uint32_t RawRequiredReferenceFlags =
		RawRequiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_GRID |
		NRI_SPATIAL_ABSENCE_GPU_GRID_REFERENCE;

	uint64_t HashBytes(uint64_t hash, const void* data, size_t size)
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		for (size_t index = 0; index < size; ++index)
			hash = (hash ^ bytes[index]) * HashPrime;
		return hash;
	}

	uint64_t HashRawRecords(const NRISpatialAbsenceSnapshot& source)
	{
		if (source.gpuRecords.empty())
			return 0;
		return HashBytes(HashOffset, source.gpuRecords.data(),
			source.gpuRecords.size() * sizeof(source.gpuRecords[0]));
	}

	uint64_t HashTypedBlocks(const std::vector<NRISpatialAbsenceGpuBlock>& blocks)
	{
		if (blocks.empty())
			return 0;
		return HashBytes(HashOffset, blocks.data(), blocks.size() * sizeof(blocks[0]));
	}

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		static_assert(sizeof(bits) == sizeof(value));
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	float BitsFloat(uint32_t bits)
	{
		float value = 0.0f;
		static_assert(sizeof(bits) == sizeof(value));
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	}

	bool DecodeSerializedUint(float value, uint32_t& decoded)
	{
		decoded = 0;
		if (!std::isfinite(value) || value < 0.0f || value > (float)MaxSerializedFloatInteger)
			return false;
		decoded = (uint32_t)std::round(value);
		return value == (float)decoded;
	}

	bool RawRangeFits(uint32_t first, uint32_t count, size_t recordCount)
	{
		return (size_t)first <= recordCount && (size_t)count <= recordCount - (size_t)first;
	}

	bool IsFiniteOrderedBounds3(const float* boundsMin, const float* boundsMax)
	{
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			if (!std::isfinite(boundsMin[axis]) || !std::isfinite(boundsMax[axis]) ||
				boundsMin[axis] > boundsMax[axis])
			{
				return false;
			}
		}
		return true;
	}

	bool ComputeLayout(
		uint32_t chunkCount,
		uint32_t negativeCount,
		uint32_t pairCount,
		uint32_t footprintCount,
		uint32_t cellCount,
		uint32_t referenceCount,
		uint32_t triangleCount,
		NRISpatialAbsenceGpuSnapshotLayout& layout)
	{
		layout = {};
		const uint64_t referenceBlockCount = ((uint64_t)referenceCount + 3u) / 4u;
		uint64_t cursor = NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_HEADER_BLOCKS;
		auto appendSection = [&](uint32_t count, uint32_t stride, uint32_t& offset)
		{
			offset = (uint32_t)cursor;
			cursor += (uint64_t)count * stride;
			return cursor <= UINT32_MAX;
		};
		if (!appendSection(chunkCount, NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_BLOCKS,
				layout.chunkOffset) ||
			!appendSection(negativeCount, NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_NEGATIVE_BLOCKS,
				layout.negativeOffset) ||
			!appendSection(pairCount, NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_PAIR_BLOCKS,
				layout.pairOffset) ||
			!appendSection(footprintCount, NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FOOTPRINT_BLOCKS,
				layout.footprintOffset) ||
			!appendSection(cellCount, NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CELL_BLOCKS,
				layout.cellOffset))
		{
			return false;
		}
		if (referenceBlockCount > UINT32_MAX)
			return false;
		layout.referenceBlockCount = (uint32_t)referenceBlockCount;
		if (!appendSection(layout.referenceBlockCount, 1u, layout.referenceOffset) ||
			!appendSection(triangleCount, NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_TRIANGLE_BLOCKS,
				layout.triangleOffset))
		{
			return false;
		}
		layout.footerOffset = (uint32_t)cursor;
		cursor += NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FOOTER_BLOCKS;
		if (cursor > UINT32_MAX)
			return false;
		layout.totalBlockCount = (uint32_t)cursor;
		return true;
	}

	struct ParsedChunk
	{
		uint32_t flags = 0;
		uint32_t negativeIndex = InvalidIndex;
		uint32_t footprintIndex = InvalidIndex;
	};

	struct ParsedBounds3
	{
		float boundsMin[3] = {};
		float boundsMax[3] = {};
	};

	struct ParsedNegative : ParsedBounds3
	{
		uint32_t pairFirst = 0;
		uint32_t pairCount = 0;
	};

	struct ParsedPair : ParsedBounds3
	{
		uint32_t positiveChunk = InvalidIndex;
		uint32_t positiveFootprint = InvalidIndex;
	};

	struct ParsedFootprint
	{
		uint32_t triangleFirst = 0;
		uint32_t triangleCount = 0;
		uint32_t cellFirst = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		float boundsMin[2] = {};
		float boundsMax[2] = {};
	};

	struct ParsedCell
	{
		uint32_t referenceFirst = 0;
		uint32_t referenceCount = 0;
		uint32_t certificateTriangle = InvalidIndex;
		uint32_t flags = 0;
	};

	struct ParsedTriangle
	{
		float point[6] = {};
	};

	struct ParsedSnapshot
	{
		std::vector<ParsedChunk> chunks;
		std::vector<ParsedNegative> negatives;
		std::vector<ParsedPair> pairs;
		std::vector<ParsedFootprint> footprints;
		std::vector<ParsedCell> cells;
		std::vector<uint32_t> references;
		std::vector<ParsedTriangle> triangles;
	};

	enum RawRecordRole : uint8_t
	{
		RawRoleNone = 0,
		RawRoleLookup,
		RawRolePair,
		RawRoleTriangle,
		RawRoleGrid,
		RawRoleCell,
		RawRoleReference,
	};

	bool ParseRawSnapshot(const NRISpatialAbsenceSnapshot& source, ParsedSnapshot& parsed)
	{
		parsed = {};
		const std::vector<NRISpatialAbsenceGpuRecord>& records = source.gpuRecords;
		if (records.size() <= 1u || records.size() > MaxSerializedFloatInteger)
			return false;

		const NRISpatialAbsenceGpuRecord& header = records[0];
		uint32_t chunkCount = 0;
		const uint32_t certifiedCount = source.certifiedCount;
		uint32_t pairCount = 0;
		uint32_t triangleCount = 0;
		uint32_t cellCount = 0;
		if ((header.flags & (RawRequiredBaseFlags |
				NRI_SPATIAL_ABSENCE_GPU_RAY_QUERY_VALIDATED)) !=
				(RawRequiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_RAY_QUERY_VALIDATED) ||
			header.data0 != source.frameIndex ||
			((uint64_t)header.data1 | ((uint64_t)header.data2 << 32u)) != source.worldGeneration ||
			FloatBits(header.payload[0]) != FloatBits(source.center[0]) ||
			FloatBits(header.payload[1]) != FloatBits(source.center[1]) ||
			FloatBits(header.payload[2]) != FloatBits(source.center[2]) ||
			FloatBits(header.payload[3]) != FloatBits(source.guardRadius) ||
			!std::isfinite(header.payload[0]) || !std::isfinite(header.payload[1]) ||
			!std::isfinite(header.payload[2]) || !std::isfinite(header.payload[3]) ||
			header.payload[3] <= 0.0f ||
			!DecodeSerializedUint(header.payload[4], chunkCount) ||
			!std::isfinite(header.payload[5]) || header.payload[5] <= 0.0f ||
			FloatBits(header.payload[5]) != FloatBits(source.actorGuardRadius) ||
			!DecodeSerializedUint(header.payload[6], pairCount) ||
			!DecodeSerializedUint(header.payload[7], triangleCount) ||
			!DecodeSerializedUint(header.payload[8], cellCount) ||
			chunkCount == 0u || !RawRangeFits(1u, chunkCount, records.size()) ||
			pairCount != source.authorizedPairCount ||
			triangleCount != source.footprintTriangleCount ||
			cellCount != source.footprintGridCellCount)
		{
			return false;
		}

		const size_t expectedWordCount = std::max<size_t>(((uint64_t)chunkCount + 31u) / 32u, 1u);
		if (source.negativeChunkWords.size() != expectedWordCount ||
			source.reachedChunkWords.size() != expectedWordCount ||
			source.selections.size() != certifiedCount)
		{
			return false;
		}

		std::vector<uint8_t> selected(chunkCount, 0u);
		std::vector<uint8_t> reached(chunkCount, 0u);
		uint32_t selectedCount = 0;
		for (size_t wordIndex = 0; wordIndex < expectedWordCount; ++wordIndex)
		{
			uint32_t bits = source.negativeChunkWords[wordIndex];
			for (uint32_t bitIndex = 0; bitIndex < 32u; ++bitIndex)
			{
				if ((bits & (1u << bitIndex)) == 0u)
					continue;
				const uint64_t chunkIndex = (uint64_t)wordIndex * 32u + bitIndex;
				if (chunkIndex >= chunkCount)
					return false;
				selected[(size_t)chunkIndex] = 1u;
				selectedCount++;
			}
		}
		if (selectedCount != certifiedCount)
			return false;

		bool hasReached = false;
		for (size_t wordIndex = 0; wordIndex < expectedWordCount; ++wordIndex)
		{
			uint32_t bits = source.reachedChunkWords[wordIndex];
			for (uint32_t bitIndex = 0; bitIndex < 32u; ++bitIndex)
			{
				if ((bits & (1u << bitIndex)) == 0u)
					continue;
				const uint64_t chunkIndex = (uint64_t)wordIndex * 32u + bitIndex;
				if (chunkIndex >= chunkCount || selected[(size_t)chunkIndex] != 0u)
					return false;
				reached[(size_t)chunkIndex] = 1u;
				hasReached = true;
			}
		}
		if (!hasReached)
			return false;

		std::vector<uint8_t> selectionSeen(chunkCount, 0u);
		for (const NRISpatialAbsenceSelectionRecord& selection : source.selections)
		{
			if (selection.negativeChunk >= chunkCount ||
				selected[selection.negativeChunk] == 0u ||
				selectionSeen[selection.negativeChunk] != 0u)
			{
				return false;
			}
			selectionSeen[selection.negativeChunk] = 1u;
		}

		std::vector<uint8_t> roles(records.size(), RawRoleNone);
		for (uint32_t recordIndex = 0; recordIndex <= chunkCount; ++recordIndex)
			roles[recordIndex] = RawRoleLookup;
		auto claimRecord = [&](uint32_t recordIndex, RawRecordRole role)
		{
			if (recordIndex >= records.size() || roles[recordIndex] != RawRoleNone)
				return false;
			roles[recordIndex] = role;
			return true;
		};

		parsed.chunks.resize(chunkCount);
		std::vector<uint32_t> involvedChunks;
		for (uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
		{
			const NRISpatialAbsenceGpuRecord& lookup = records[(size_t)chunkIndex + 1u];
			const bool isSelected = selected[chunkIndex] != 0u;
			const bool isReached = reached[chunkIndex] != 0u;
			if (((lookup.flags & NRI_SPATIAL_ABSENCE_GPU_CERTIFIED) != 0u) != isSelected ||
				((lookup.flags & NRI_SPATIAL_ABSENCE_GPU_REACHED) != 0u) != isReached ||
				(isReached && (lookup.flags & RawRequiredBaseFlags) != RawRequiredBaseFlags))
			{
				return false;
			}
			if (isReached)
				parsed.chunks[chunkIndex].flags |= NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_REACHED;
			if (!isSelected)
				continue;

			uint32_t selectedPairCount = 0;
			if ((lookup.flags & RawRequiredNegativeFlags) != RawRequiredNegativeFlags ||
				!DecodeSerializedUint(lookup.payload[8], selectedPairCount) || selectedPairCount == 0u ||
				lookup.data2 <= chunkCount || !RawRangeFits(lookup.data2, selectedPairCount, records.size()) ||
				!IsFiniteOrderedBounds3(lookup.payload, lookup.payload + 4u))
			{
				return false;
			}
			if (parsed.negatives.size() >= UINT32_MAX || parsed.pairs.size() > UINT32_MAX)
				return false;
			ParsedNegative negative;
			negative.pairFirst = (uint32_t)parsed.pairs.size();
			negative.pairCount = selectedPairCount;
			std::memcpy(negative.boundsMin, lookup.payload, sizeof(negative.boundsMin));
			std::memcpy(negative.boundsMax, lookup.payload + 4u, sizeof(negative.boundsMax));
			parsed.chunks[chunkIndex].flags |= NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_NEGATIVE;
			parsed.chunks[chunkIndex].negativeIndex = (uint32_t)parsed.negatives.size();
			parsed.negatives.push_back(negative);
			involvedChunks.push_back(chunkIndex);

			for (uint32_t pairOffset = 0; pairOffset < selectedPairCount; ++pairOffset)
			{
				const uint32_t recordIndex = lookup.data2 + pairOffset;
				if (!claimRecord(recordIndex, RawRolePair))
					return false;
				const NRISpatialAbsenceGpuRecord& pair = records[recordIndex];
				if ((pair.flags & RawRequiredPairFlags) != RawRequiredPairFlags ||
					pair.data0 != chunkIndex || pair.data1 >= chunkCount ||
					!IsFiniteOrderedBounds3(pair.payload, pair.payload + 4u))
				{
					return false;
				}
				ParsedPair parsedPair;
				parsedPair.positiveChunk = pair.data1;
				std::memcpy(parsedPair.boundsMin, pair.payload, sizeof(parsedPair.boundsMin));
				std::memcpy(parsedPair.boundsMax, pair.payload + 4u, sizeof(parsedPair.boundsMax));
				parsed.pairs.push_back(parsedPair);
				involvedChunks.push_back(pair.data1);
			}
		}
		if (parsed.negatives.size() != certifiedCount || parsed.pairs.size() != pairCount)
			return false;

		std::sort(involvedChunks.begin(), involvedChunks.end());
		involvedChunks.erase(std::unique(involvedChunks.begin(), involvedChunks.end()), involvedChunks.end());
		for (uint32_t ownerChunk : involvedChunks)
		{
			const NRISpatialAbsenceGpuRecord& lookup = records[(size_t)ownerChunk + 1u];
			uint32_t gridIndex = 0;
			if ((lookup.flags & (RawRequiredFootprintFlags | NRI_SPATIAL_ABSENCE_GPU_GRID)) !=
					(RawRequiredFootprintFlags | NRI_SPATIAL_ABSENCE_GPU_GRID) ||
				lookup.data1 == 0u || lookup.data0 <= chunkCount ||
				!RawRangeFits(lookup.data0, lookup.data1, records.size()) ||
				!DecodeSerializedUint(lookup.payload[9], gridIndex) ||
				gridIndex <= chunkCount || gridIndex >= records.size())
			{
				return false;
			}

			if (parsed.footprints.size() >= UINT32_MAX || parsed.triangles.size() > UINT32_MAX ||
				parsed.cells.size() > UINT32_MAX || parsed.references.size() > UINT32_MAX)
			{
				return false;
			}
			ParsedFootprint footprint;
			footprint.triangleFirst = (uint32_t)parsed.triangles.size();
			footprint.triangleCount = lookup.data1;
			footprint.cellFirst = (uint32_t)parsed.cells.size();
			parsed.chunks[ownerChunk].flags |= NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_FOOTPRINT;
			parsed.chunks[ownerChunk].footprintIndex = (uint32_t)parsed.footprints.size();

			for (uint32_t triangleOffset = 0; triangleOffset < lookup.data1; ++triangleOffset)
			{
				const uint32_t recordIndex = lookup.data0 + triangleOffset;
				if (!claimRecord(recordIndex, RawRoleTriangle))
					return false;
				const NRISpatialAbsenceGpuRecord& triangle = records[recordIndex];
				if ((triangle.flags & RawRequiredFootprintFlags) != RawRequiredFootprintFlags ||
					triangle.data0 != ownerChunk)
				{
					return false;
				}
				ParsedTriangle parsedTriangle;
				for (uint32_t word = 0; word < 6u; ++word)
				{
					if (!std::isfinite(triangle.payload[word]))
						return false;
					parsedTriangle.point[word] = triangle.payload[word];
				}
				parsed.triangles.push_back(parsedTriangle);
			}

			if (!claimRecord(gridIndex, RawRoleGrid))
				return false;
			const NRISpatialAbsenceGpuRecord& grid = records[gridIndex];
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t gridCellCount = 0;
			uint32_t logicalReferenceCount = 0;
			uint32_t referenceRecordCount = 0;
			if ((grid.flags & RawRequiredGridFlags) != RawRequiredGridFlags || grid.data0 != ownerChunk ||
				!std::isfinite(grid.payload[0]) || !std::isfinite(grid.payload[1]) ||
				!std::isfinite(grid.payload[2]) || !std::isfinite(grid.payload[3]) ||
				grid.payload[0] >= grid.payload[2] || grid.payload[1] >= grid.payload[3] ||
				!DecodeSerializedUint(grid.payload[4], width) ||
				!DecodeSerializedUint(grid.payload[5], height) ||
				!DecodeSerializedUint(grid.payload[6], gridCellCount) ||
				!DecodeSerializedUint(grid.payload[7], logicalReferenceCount) ||
				!DecodeSerializedUint(grid.payload[8], referenceRecordCount) ||
				width == 0u || height == 0u || width > MaxFootprintGridDimension ||
				height > MaxFootprintGridDimension || gridCellCount != width * height ||
				grid.data1 != gridIndex + 1u || !RawRangeFits(grid.data1, gridCellCount, records.size()) ||
				grid.data2 != grid.data1 + gridCellCount ||
				!RawRangeFits(grid.data2, referenceRecordCount, records.size()))
			{
				return false;
			}
			footprint.width = width;
			footprint.height = height;
			footprint.boundsMin[0] = grid.payload[0];
			footprint.boundsMin[1] = grid.payload[1];
			footprint.boundsMax[0] = grid.payload[2];
			footprint.boundsMax[1] = grid.payload[3];

			uint64_t nextReferenceRecord = grid.data2;
			uint64_t accumulatedReferences = 0;
			for (uint32_t cellOffset = 0; cellOffset < gridCellCount; ++cellOffset)
			{
				const uint32_t recordIndex = grid.data1 + cellOffset;
				if (!claimRecord(recordIndex, RawRoleCell))
					return false;
				const NRISpatialAbsenceGpuRecord& cell = records[recordIndex];
				uint32_t storedCellOffset = 0;
				uint32_t cellReferenceRecordCount = 0;
				const uint32_t expectedReferenceRecordCount = (cell.data2 + 2u) / 3u;
				if ((cell.flags & RawRequiredCellFlags) != RawRequiredCellFlags || cell.data0 != ownerChunk ||
					!DecodeSerializedUint(cell.payload[0], storedCellOffset) || storedCellOffset != cellOffset ||
					!DecodeSerializedUint(cell.payload[1], cellReferenceRecordCount) ||
					cellReferenceRecordCount != expectedReferenceRecordCount ||
					cell.data2 > logicalReferenceCount || cell.data1 != nextReferenceRecord ||
					!RawRangeFits(cell.data1, cellReferenceRecordCount, records.size()) ||
					(uint64_t)cell.data1 + cellReferenceRecordCount >
						(uint64_t)grid.data2 + referenceRecordCount)
				{
					return false;
				}

				ParsedCell parsedCell;
				if (parsed.references.size() > UINT32_MAX)
					return false;
				parsedCell.referenceFirst = (uint32_t)parsed.references.size();
				parsedCell.referenceCount = cell.data2;
				const bool interior =
					(cell.flags & NRI_SPATIAL_ABSENCE_GPU_GRID_CELL_INTERIOR) != 0u;
				if (interior)
				{
					if (cell.data2 != 0u || !DecodeSerializedUint(
						cell.payload[2], parsedCell.certificateTriangle) ||
						parsedCell.certificateTriangle >= lookup.data1)
					{
						return false;
					}
					parsedCell.flags = NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CELL_INTERIOR;
				}

				for (uint32_t referenceOffset = 0;
					referenceOffset < cellReferenceRecordCount; ++referenceOffset)
				{
					const uint32_t referenceIndex = cell.data1 + referenceOffset;
					if (!claimRecord(referenceIndex, RawRoleReference))
						return false;
					const NRISpatialAbsenceGpuRecord& reference = records[referenceIndex];
					uint32_t referenceOwner = 0;
					uint32_t storedReferenceOffset = 0;
					const uint32_t consumed = referenceOffset * 3u;
					const uint32_t tailCount = std::min(cell.data2 - consumed, 3u);
					const bool tailValid = tailCount == 3u ||
						(tailCount == 2u && reference.data2 == InvalidIndex) ||
						(tailCount == 1u && reference.data1 == InvalidIndex &&
							reference.data2 == InvalidIndex);
					if ((reference.flags & RawRequiredReferenceFlags) != RawRequiredReferenceFlags ||
						!DecodeSerializedUint(reference.payload[0], referenceOwner) ||
						referenceOwner != ownerChunk ||
						!DecodeSerializedUint(reference.payload[1], storedReferenceOffset) ||
						storedReferenceOffset != referenceOffset || !tailValid)
					{
						return false;
					}
					const uint32_t triangleOffsets[3] = {
						reference.data0, reference.data1, reference.data2
					};
					for (uint32_t lane = 0; lane < tailCount; ++lane)
					{
						if (triangleOffsets[lane] >= lookup.data1 ||
							parsed.references.size() >= UINT32_MAX)
						{
							return false;
						}
						parsed.references.push_back(triangleOffsets[lane]);
					}
				}
				parsed.cells.push_back(parsedCell);
				nextReferenceRecord += cellReferenceRecordCount;
				accumulatedReferences += cell.data2;
			}
			if (nextReferenceRecord != (uint64_t)grid.data2 + referenceRecordCount ||
				accumulatedReferences != logicalReferenceCount)
			{
				return false;
			}
			parsed.footprints.push_back(footprint);
		}

		for (ParsedPair& pair : parsed.pairs)
		{
			pair.positiveFootprint = parsed.chunks[pair.positiveChunk].footprintIndex;
			if (pair.positiveFootprint == InvalidIndex)
				return false;
		}
		for (const ParsedChunk& chunk : parsed.chunks)
		{
			if ((chunk.flags & NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_NEGATIVE) != 0u &&
				chunk.footprintIndex == InvalidIndex)
			{
				return false;
			}
		}

		if (parsed.triangles.size() != triangleCount || parsed.cells.size() != cellCount)
			return false;
		for (size_t recordIndex = (size_t)chunkCount + 1u; recordIndex < roles.size(); ++recordIndex)
		{
			if (roles[recordIndex] == RawRoleNone)
				return false;
		}
		return true;
	}

	bool FitsUint32(size_t value)
	{
		return value <= UINT32_MAX;
	}

	void WriteHeaderAndFooter(
		NRISpatialAbsenceGpuSnapshot& snapshot,
		const NRISpatialAbsenceGpuSnapshotLayout& layout,
		uint32_t publicationFlags)
	{
		NRISpatialAbsenceGpuBlock& header0 = snapshot.blocks[0];
		header0.words[0] = NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_MAGIC;
		header0.words[1] = NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_VERSION;
		header0.words[2] = publicationFlags;
		header0.words[3] = layout.totalBlockCount;

		NRISpatialAbsenceGpuBlock& header1 = snapshot.blocks[1];
		header1.words[0] = snapshot.frameIndex;
		header1.words[1] = snapshot.chunkCount;
		header1.words[2] = snapshot.negativeCount;
		header1.words[3] = snapshot.pairCount;

		NRISpatialAbsenceGpuBlock& header2 = snapshot.blocks[2];
		header2.words[0] = snapshot.footprintCount;
		header2.words[1] = snapshot.cellCount;
		header2.words[2] = snapshot.referenceCount;
		header2.words[3] = snapshot.triangleCount;

		NRISpatialAbsenceGpuBlock& header3 = snapshot.blocks[3];
		header3.words[0] = (uint32_t)snapshot.worldGeneration;
		header3.words[1] = (uint32_t)(snapshot.worldGeneration >> 32u);
		header3.words[2] = (uint32_t)snapshot.captureSerial;
		header3.words[3] = (uint32_t)(snapshot.captureSerial >> 32u);

		NRISpatialAbsenceGpuBlock& header4 = snapshot.blocks[4];
		header4.words[0] = (uint32_t)snapshot.sourcePayloadHash;
		header4.words[1] = (uint32_t)(snapshot.sourcePayloadHash >> 32u);
		header4.words[2] = FloatBits(snapshot.actorGuardRadius);
		header4.words[3] = NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_HEADER_BLOCKS;

		NRISpatialAbsenceGpuBlock& header5 = snapshot.blocks[5];
		header5.words[0] = FloatBits(snapshot.center[0]);
		header5.words[1] = FloatBits(snapshot.center[1]);
		header5.words[2] = FloatBits(snapshot.center[2]);
		header5.words[3] = FloatBits(snapshot.guardRadius);

		NRISpatialAbsenceGpuBlock& footer0 = snapshot.blocks[layout.footerOffset];
		footer0 = header0;
		NRISpatialAbsenceGpuBlock& footer1 = snapshot.blocks[layout.footerOffset + 1u];
		footer1.words[0] = snapshot.frameIndex;
		footer1.words[1] = (uint32_t)snapshot.worldGeneration;
		footer1.words[2] = (uint32_t)(snapshot.worldGeneration >> 32u);
		footer1.words[3] = (uint32_t)snapshot.captureSerial;
		NRISpatialAbsenceGpuBlock& footer2 = snapshot.blocks[layout.footerOffset + 2u];
		footer2.words[0] = (uint32_t)(snapshot.captureSerial >> 32u);
		footer2.words[1] = (uint32_t)snapshot.sourcePayloadHash;
		footer2.words[2] = (uint32_t)(snapshot.sourcePayloadHash >> 32u);
		footer2.words[3] = layout.footerOffset;
	}

	bool PackTypedSnapshot(
		const NRISpatialAbsenceSnapshot& source,
		const ParsedSnapshot& parsed,
		NRISpatialAbsenceGpuSnapshot& destination)
	{
		if (!FitsUint32(parsed.chunks.size()) || !FitsUint32(parsed.negatives.size()) ||
			!FitsUint32(parsed.pairs.size()) || !FitsUint32(parsed.footprints.size()) ||
			!FitsUint32(parsed.cells.size()) || !FitsUint32(parsed.references.size()) ||
			!FitsUint32(parsed.triangles.size()))
		{
			return false;
		}

		destination.frameIndex = source.frameIndex;
		destination.worldGeneration = source.worldGeneration;
		destination.captureSerial = source.captureSerial;
		destination.sourcePayloadHash = source.payloadHash;
		std::memcpy(destination.center, source.center, sizeof(destination.center));
		destination.guardRadius = source.guardRadius;
		destination.actorGuardRadius = source.actorGuardRadius;
		destination.chunkCount = (uint32_t)parsed.chunks.size();
		destination.negativeCount = (uint32_t)parsed.negatives.size();
		destination.pairCount = (uint32_t)parsed.pairs.size();
		destination.footprintCount = (uint32_t)parsed.footprints.size();
		destination.cellCount = (uint32_t)parsed.cells.size();
		destination.referenceCount = (uint32_t)parsed.references.size();
		destination.triangleCount = (uint32_t)parsed.triangles.size();

		NRISpatialAbsenceGpuSnapshotLayout layout;
		if (!ComputeLayout(destination.chunkCount, destination.negativeCount,
			destination.pairCount, destination.footprintCount, destination.cellCount,
			destination.referenceCount, destination.triangleCount, layout) ||
			layout.totalBlockCount > destination.blocks.max_size())
		{
			return false;
		}
		destination.blocks.assign(layout.totalBlockCount, {});
		WriteHeaderAndFooter(destination, layout, NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_COMPLETE);

		for (uint32_t index = 0; index < destination.chunkCount; ++index)
		{
			const ParsedChunk& sourceChunk = parsed.chunks[index];
			NRISpatialAbsenceGpuBlock& block = destination.blocks[layout.chunkOffset + index];
			block.words[0] = sourceChunk.flags;
			block.words[1] = sourceChunk.negativeIndex;
			block.words[2] = sourceChunk.footprintIndex;
		}
		for (uint32_t index = 0; index < destination.negativeCount; ++index)
		{
			const ParsedNegative& negative = parsed.negatives[index];
			NRISpatialAbsenceGpuBlock& first = destination.blocks[
				layout.negativeOffset + index * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_NEGATIVE_BLOCKS];
			NRISpatialAbsenceGpuBlock& second = destination.blocks[
				layout.negativeOffset + index * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_NEGATIVE_BLOCKS + 1u];
			first.words[0] = negative.pairFirst;
			first.words[1] = negative.pairCount;
			first.words[2] = FloatBits(negative.boundsMin[0]);
			first.words[3] = FloatBits(negative.boundsMin[1]);
			second.words[0] = FloatBits(negative.boundsMin[2]);
			second.words[1] = FloatBits(negative.boundsMax[0]);
			second.words[2] = FloatBits(negative.boundsMax[1]);
			second.words[3] = FloatBits(negative.boundsMax[2]);
		}
		for (uint32_t index = 0; index < destination.pairCount; ++index)
		{
			const ParsedPair& pair = parsed.pairs[index];
			NRISpatialAbsenceGpuBlock& first = destination.blocks[
				layout.pairOffset + index * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_PAIR_BLOCKS];
			NRISpatialAbsenceGpuBlock& second = destination.blocks[
				layout.pairOffset + index * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_PAIR_BLOCKS + 1u];
			first.words[0] = pair.positiveChunk;
			first.words[1] = pair.positiveFootprint;
			first.words[2] = FloatBits(pair.boundsMin[0]);
			first.words[3] = FloatBits(pair.boundsMin[1]);
			second.words[0] = FloatBits(pair.boundsMin[2]);
			second.words[1] = FloatBits(pair.boundsMax[0]);
			second.words[2] = FloatBits(pair.boundsMax[1]);
			second.words[3] = FloatBits(pair.boundsMax[2]);
		}
		for (uint32_t index = 0; index < destination.footprintCount; ++index)
		{
			const ParsedFootprint& footprint = parsed.footprints[index];
			NRISpatialAbsenceGpuBlock& first = destination.blocks[
				layout.footprintOffset + index * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FOOTPRINT_BLOCKS];
			NRISpatialAbsenceGpuBlock& second = destination.blocks[
				layout.footprintOffset + index * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FOOTPRINT_BLOCKS + 1u];
			first.words[0] = footprint.triangleFirst;
			first.words[1] = footprint.triangleCount;
			first.words[2] = footprint.cellFirst;
			first.words[3] = footprint.width | (footprint.height << 16u);
			second.words[0] = FloatBits(footprint.boundsMin[0]);
			second.words[1] = FloatBits(footprint.boundsMin[1]);
			second.words[2] = FloatBits(footprint.boundsMax[0]);
			second.words[3] = FloatBits(footprint.boundsMax[1]);
		}
		for (uint32_t index = 0; index < destination.cellCount; ++index)
		{
			const ParsedCell& cell = parsed.cells[index];
			NRISpatialAbsenceGpuBlock& block = destination.blocks[layout.cellOffset + index];
			block.words[0] = cell.referenceFirst;
			block.words[1] = cell.referenceCount;
			block.words[2] = cell.certificateTriangle;
			block.words[3] = cell.flags;
		}
		for (uint32_t index = 0; index < layout.referenceBlockCount; ++index)
		{
			NRISpatialAbsenceGpuBlock& block = destination.blocks[layout.referenceOffset + index];
			std::fill(std::begin(block.words), std::end(block.words), InvalidIndex);
		}
		for (uint32_t index = 0; index < destination.referenceCount; ++index)
		{
			destination.blocks[layout.referenceOffset + index / 4u].words[index % 4u] =
				parsed.references[index];
		}
		for (uint32_t index = 0; index < destination.triangleCount; ++index)
		{
			const ParsedTriangle& triangle = parsed.triangles[index];
			NRISpatialAbsenceGpuBlock& first = destination.blocks[
				layout.triangleOffset + index * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_TRIANGLE_BLOCKS];
			NRISpatialAbsenceGpuBlock& second = destination.blocks[
				layout.triangleOffset + index * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_TRIANGLE_BLOCKS + 1u];
			for (uint32_t word = 0; word < 4u; ++word)
				first.words[word] = FloatBits(triangle.point[word]);
			second.words[0] = FloatBits(triangle.point[4]);
			second.words[1] = FloatBits(triangle.point[5]);
		}
		return true;
	}

	bool ValidateFiniteBounds3(const NRISpatialAbsenceGpuBlock& first,
		const NRISpatialAbsenceGpuBlock& second)
	{
		const float boundsMin[3] = {
			BitsFloat(first.words[2]), BitsFloat(first.words[3]), BitsFloat(second.words[0])
		};
		const float boundsMax[3] = {
			BitsFloat(second.words[1]), BitsFloat(second.words[2]), BitsFloat(second.words[3])
		};
		return IsFiniteOrderedBounds3(boundsMin, boundsMax);
	}

	bool ValidateTypedBlocks(const NRISpatialAbsenceGpuSnapshot& snapshot, bool requireSealed)
	{
		NRISpatialAbsenceGpuSnapshotLayout layout;
		if (!ComputeLayout(snapshot.chunkCount, snapshot.negativeCount, snapshot.pairCount,
			snapshot.footprintCount, snapshot.cellCount, snapshot.referenceCount,
			snapshot.triangleCount, layout) || snapshot.chunkCount == 0u ||
			snapshot.blocks.size() != layout.totalBlockCount)
		{
			return false;
		}

		const uint32_t expectedPublicationFlags = NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_COMPLETE |
			(requireSealed ? NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_SEALED : 0u);
		const NRISpatialAbsenceGpuBlock& header0 = snapshot.blocks[0];
		const NRISpatialAbsenceGpuBlock& header1 = snapshot.blocks[1];
		const NRISpatialAbsenceGpuBlock& header2 = snapshot.blocks[2];
		const NRISpatialAbsenceGpuBlock& header3 = snapshot.blocks[3];
		const NRISpatialAbsenceGpuBlock& header4 = snapshot.blocks[4];
		const NRISpatialAbsenceGpuBlock& header5 = snapshot.blocks[5];
		if (header0.words[0] != NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_MAGIC ||
			header0.words[1] != NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_VERSION ||
			header0.words[2] != expectedPublicationFlags ||
			header0.words[3] != layout.totalBlockCount ||
			header1.words[0] != snapshot.frameIndex ||
			header1.words[1] != snapshot.chunkCount ||
			header1.words[2] != snapshot.negativeCount ||
			header1.words[3] != snapshot.pairCount ||
			header2.words[0] != snapshot.footprintCount ||
			header2.words[1] != snapshot.cellCount ||
			header2.words[2] != snapshot.referenceCount ||
			header2.words[3] != snapshot.triangleCount ||
			header3.words[0] != (uint32_t)snapshot.worldGeneration ||
			header3.words[1] != (uint32_t)(snapshot.worldGeneration >> 32u) ||
			header3.words[2] != (uint32_t)snapshot.captureSerial ||
			header3.words[3] != (uint32_t)(snapshot.captureSerial >> 32u) ||
			header4.words[0] != (uint32_t)snapshot.sourcePayloadHash ||
			header4.words[1] != (uint32_t)(snapshot.sourcePayloadHash >> 32u) ||
			header4.words[2] != FloatBits(snapshot.actorGuardRadius) ||
			header4.words[3] != NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_HEADER_BLOCKS ||
			header5.words[0] != FloatBits(snapshot.center[0]) ||
			header5.words[1] != FloatBits(snapshot.center[1]) ||
			header5.words[2] != FloatBits(snapshot.center[2]) ||
			header5.words[3] != FloatBits(snapshot.guardRadius) ||
			snapshot.worldGeneration == 0u || snapshot.captureSerial == 0u ||
			snapshot.sourcePayloadHash == 0u ||
			!std::isfinite(snapshot.center[0]) || !std::isfinite(snapshot.center[1]) ||
			!std::isfinite(snapshot.center[2]) || !std::isfinite(snapshot.guardRadius) ||
			!std::isfinite(snapshot.actorGuardRadius) || snapshot.guardRadius <= 0.0f ||
			snapshot.actorGuardRadius <= 0.0f)
		{
			return false;
		}

		const NRISpatialAbsenceGpuBlock& footer0 = snapshot.blocks[layout.footerOffset];
		const NRISpatialAbsenceGpuBlock& footer1 = snapshot.blocks[layout.footerOffset + 1u];
		const NRISpatialAbsenceGpuBlock& footer2 = snapshot.blocks[layout.footerOffset + 2u];
		if (std::memcmp(&footer0, &header0, sizeof(footer0)) != 0 ||
			footer1.words[0] != snapshot.frameIndex ||
			footer1.words[1] != (uint32_t)snapshot.worldGeneration ||
			footer1.words[2] != (uint32_t)(snapshot.worldGeneration >> 32u) ||
			footer1.words[3] != (uint32_t)snapshot.captureSerial ||
			footer2.words[0] != (uint32_t)(snapshot.captureSerial >> 32u) ||
			footer2.words[1] != (uint32_t)snapshot.sourcePayloadHash ||
			footer2.words[2] != (uint32_t)(snapshot.sourcePayloadHash >> 32u) ||
			footer2.words[3] != layout.footerOffset)
		{
			return false;
		}

		std::vector<uint8_t> negativeOwners(snapshot.negativeCount, 0u);
		std::vector<uint8_t> footprintOwners(snapshot.footprintCount, 0u);
		bool hasReached = false;
		for (uint32_t chunkIndex = 0; chunkIndex < snapshot.chunkCount; ++chunkIndex)
		{
			const NRISpatialAbsenceGpuBlock& chunk = snapshot.blocks[layout.chunkOffset + chunkIndex];
			constexpr uint32_t allowedFlags =
				NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_REACHED |
				NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_NEGATIVE |
				NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_FOOTPRINT;
			const bool isReached =
				(chunk.words[0] & NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_REACHED) != 0u;
			const bool isNegative =
				(chunk.words[0] & NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_NEGATIVE) != 0u;
			const bool hasFootprint =
				(chunk.words[0] & NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_FOOTPRINT) != 0u;
			if ((chunk.words[0] & ~allowedFlags) != 0u || chunk.words[3] != 0u ||
				isNegative != (chunk.words[1] != InvalidIndex) ||
				hasFootprint != (chunk.words[2] != InvalidIndex) ||
				(isReached && isNegative))
			{
				return false;
			}
			hasReached = hasReached || isReached;
			if (isNegative)
			{
				if (chunk.words[1] >= snapshot.negativeCount ||
					negativeOwners[chunk.words[1]] != 0u || !hasFootprint)
				{
					return false;
				}
				negativeOwners[chunk.words[1]] = 1u;
			}
			if (hasFootprint)
			{
				if (chunk.words[2] >= snapshot.footprintCount ||
					footprintOwners[chunk.words[2]] != 0u)
				{
					return false;
				}
				footprintOwners[chunk.words[2]] = 1u;
			}
		}
		if (!hasReached || std::find(negativeOwners.begin(), negativeOwners.end(), 0u) != negativeOwners.end() ||
			std::find(footprintOwners.begin(), footprintOwners.end(), 0u) != footprintOwners.end())
		{
			return false;
		}

		uint32_t expectedPairFirst = 0;
		for (uint32_t negativeIndex = 0; negativeIndex < snapshot.negativeCount; ++negativeIndex)
		{
			const uint32_t blockIndex = layout.negativeOffset +
				negativeIndex * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_NEGATIVE_BLOCKS;
			const NRISpatialAbsenceGpuBlock& first = snapshot.blocks[blockIndex];
			const NRISpatialAbsenceGpuBlock& second = snapshot.blocks[blockIndex + 1u];
			if (first.words[0] != expectedPairFirst || first.words[1] == 0u ||
				first.words[0] > snapshot.pairCount ||
				first.words[1] > snapshot.pairCount - first.words[0] ||
				!ValidateFiniteBounds3(first, second))
			{
				return false;
			}
			expectedPairFirst += first.words[1];
		}
		if (expectedPairFirst != snapshot.pairCount)
			return false;

		for (uint32_t pairIndex = 0; pairIndex < snapshot.pairCount; ++pairIndex)
		{
			const uint32_t blockIndex = layout.pairOffset +
				pairIndex * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_PAIR_BLOCKS;
			const NRISpatialAbsenceGpuBlock& first = snapshot.blocks[blockIndex];
			const NRISpatialAbsenceGpuBlock& second = snapshot.blocks[blockIndex + 1u];
			if (first.words[0] >= snapshot.chunkCount || first.words[1] >= snapshot.footprintCount ||
				!ValidateFiniteBounds3(first, second))
			{
				return false;
			}
			const NRISpatialAbsenceGpuBlock& positiveChunk =
				snapshot.blocks[layout.chunkOffset + first.words[0]];
			if ((positiveChunk.words[0] & NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CHUNK_FOOTPRINT) == 0u ||
				positiveChunk.words[2] != first.words[1])
			{
				return false;
			}
		}

		uint32_t expectedTriangleFirst = 0;
		uint32_t expectedCellFirst = 0;
		uint32_t expectedReferenceFirst = 0;
		for (uint32_t footprintIndex = 0; footprintIndex < snapshot.footprintCount; ++footprintIndex)
		{
			const uint32_t blockIndex = layout.footprintOffset +
				footprintIndex * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FOOTPRINT_BLOCKS;
			const NRISpatialAbsenceGpuBlock& first = snapshot.blocks[blockIndex];
			const NRISpatialAbsenceGpuBlock& second = snapshot.blocks[blockIndex + 1u];
			const uint32_t width = first.words[3] & 0xffffu;
			const uint32_t height = first.words[3] >> 16u;
			const float boundsMin[2] = { BitsFloat(second.words[0]), BitsFloat(second.words[1]) };
			const float boundsMax[2] = { BitsFloat(second.words[2]), BitsFloat(second.words[3]) };
			if (first.words[0] != expectedTriangleFirst || first.words[1] == 0u ||
				first.words[0] > snapshot.triangleCount ||
				first.words[1] > snapshot.triangleCount - first.words[0] ||
				first.words[2] != expectedCellFirst || width == 0u || height == 0u ||
				width > MaxFootprintGridDimension || height > MaxFootprintGridDimension ||
				expectedCellFirst > snapshot.cellCount ||
				(uint64_t)width * height > snapshot.cellCount - expectedCellFirst ||
				!std::isfinite(boundsMin[0]) || !std::isfinite(boundsMin[1]) ||
				!std::isfinite(boundsMax[0]) || !std::isfinite(boundsMax[1]) ||
				boundsMin[0] >= boundsMax[0] || boundsMin[1] >= boundsMax[1])
			{
				return false;
			}

			const uint32_t footprintCellCount = width * height;
			for (uint32_t cellOffset = 0; cellOffset < footprintCellCount; ++cellOffset)
			{
				const NRISpatialAbsenceGpuBlock& cell =
					snapshot.blocks[layout.cellOffset + first.words[2] + cellOffset];
				const bool interior =
					(cell.words[3] & NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CELL_INTERIOR) != 0u;
				if ((cell.words[3] & ~NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_CELL_INTERIOR) != 0u ||
					cell.words[0] != expectedReferenceFirst ||
					cell.words[0] > snapshot.referenceCount ||
					cell.words[1] > snapshot.referenceCount - cell.words[0] ||
					interior != (cell.words[2] != InvalidIndex) ||
					(interior && (cell.words[1] != 0u || cell.words[2] >= first.words[1])))
				{
					return false;
				}
				for (uint32_t referenceOffset = 0; referenceOffset < cell.words[1]; ++referenceOffset)
				{
					const uint32_t referenceIndex = cell.words[0] + referenceOffset;
					const uint32_t triangleOffset = snapshot.blocks[
						layout.referenceOffset + referenceIndex / 4u].words[referenceIndex % 4u];
					if (triangleOffset >= first.words[1])
						return false;
				}
				expectedReferenceFirst += cell.words[1];
			}
			expectedTriangleFirst += first.words[1];
			expectedCellFirst += footprintCellCount;
		}
		if (expectedTriangleFirst != snapshot.triangleCount ||
			expectedCellFirst != snapshot.cellCount ||
			expectedReferenceFirst != snapshot.referenceCount)
		{
			return false;
		}

		for (uint64_t referenceIndex = snapshot.referenceCount;
			referenceIndex < (uint64_t)layout.referenceBlockCount * 4u; ++referenceIndex)
		{
			if (snapshot.blocks[layout.referenceOffset + (size_t)(referenceIndex / 4u)].words[referenceIndex % 4u] !=
				InvalidIndex)
			{
				return false;
			}
		}
		for (uint32_t triangleIndex = 0; triangleIndex < snapshot.triangleCount; ++triangleIndex)
		{
			const uint32_t blockIndex = layout.triangleOffset +
				triangleIndex * NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_TRIANGLE_BLOCKS;
			const NRISpatialAbsenceGpuBlock& first = snapshot.blocks[blockIndex];
			const NRISpatialAbsenceGpuBlock& second = snapshot.blocks[blockIndex + 1u];
			for (uint32_t word = 0; word < 4u; ++word)
			{
				if (!std::isfinite(BitsFloat(first.words[word])))
					return false;
			}
			if (!std::isfinite(BitsFloat(second.words[0])) ||
				!std::isfinite(BitsFloat(second.words[1])) ||
				second.words[2] != 0u || second.words[3] != 0u)
			{
				return false;
			}
		}

		return !requireSealed || snapshot.payloadHash == HashTypedBlocks(snapshot.blocks);
	}

	NRISpatialAbsenceSnapshot MakeSyntheticRawSnapshot(bool withNegative)
	{
		NRISpatialAbsenceSnapshot source;
		source.valid = true;
		source.captureSerial = 41u;
		source.worldGeneration = 29u;
		source.frameIndex = 17u;
		source.center[0] = 2.0f;
		source.center[1] = -3.0f;
		source.center[2] = 4.0f;
		source.guardRadius = 8.0f;
		source.actorGuardRadius = 4.0f;
		source.reachedSectorIndices = { 0u };
		source.negativeChunkWords = { withNegative ? 1u << 1u : 0u };
		source.reachedChunkWords = { 1u };
		source.gpuRecords.resize(withNegative ? 12u : 3u);

		NRISpatialAbsenceGpuRecord& header = source.gpuRecords[0];
		header.flags = RawRequiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_RAY_QUERY_VALIDATED;
		header.data0 = source.frameIndex;
		header.data1 = (uint32_t)source.worldGeneration;
		header.data2 = (uint32_t)(source.worldGeneration >> 32u);
		std::memcpy(header.payload, source.center, sizeof(source.center));
		header.payload[3] = source.guardRadius;
		header.payload[4] = 2.0f;
		header.payload[5] = source.actorGuardRadius;

		source.gpuRecords[1].flags = RawRequiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_REACHED;
		if (!withNegative)
		{
			source.payloadHash = HashRawRecords(source);
			return source;
		}

		source.certifiedCount = 1u;
		source.authorizedPairCount = 1u;
		source.footprintTriangleCount = 2u;
		source.footprintGridCellCount = 2u;
		source.footprintGridReferenceCount = 2u;
		header.payload[6] = 1.0f;
		header.payload[7] = 2.0f;
		header.payload[8] = 2.0f;
		NRISpatialAbsenceSelectionRecord selection;
		selection.negativeChunk = 1u;
		source.selections.push_back(selection);

		NRISpatialAbsenceGpuRecord& negative = source.gpuRecords[2];
		negative.flags = RawRequiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_CERTIFIED;
		negative.data2 = 3u;
		negative.payload[4] = negative.payload[5] = negative.payload[6] = 1.0f;
		negative.payload[8] = 1.0f;
		NRISpatialAbsenceGpuRecord& pair = source.gpuRecords[3];
		pair.flags = RawRequiredPairFlags;
		pair.data0 = 1u;
		pair.data1 = 0u;
		pair.payload[4] = pair.payload[5] = pair.payload[6] = 1.0f;

		auto setFootprint = [&](uint32_t owner, uint32_t triangleIndex, uint32_t gridIndex,
			uint32_t cellIndex, uint32_t referenceIndex)
		{
			NRISpatialAbsenceGpuRecord& lookup = source.gpuRecords[(size_t)owner + 1u];
			lookup.flags |= RawRequiredBaseFlags | NRI_SPATIAL_ABSENCE_GPU_FOOTPRINT |
				NRI_SPATIAL_ABSENCE_GPU_GRID;
			lookup.data0 = triangleIndex;
			lookup.data1 = 1u;
			lookup.payload[9] = (float)gridIndex;

			NRISpatialAbsenceGpuRecord& triangle = source.gpuRecords[triangleIndex];
			triangle.flags = RawRequiredFootprintFlags;
			triangle.data0 = owner;
			triangle.payload[0] = 0.0f;
			triangle.payload[1] = 0.0f;
			triangle.payload[2] = 1.0f;
			triangle.payload[3] = 0.0f;
			triangle.payload[4] = 0.0f;
			triangle.payload[5] = 1.0f;

			NRISpatialAbsenceGpuRecord& grid = source.gpuRecords[gridIndex];
			grid.flags = RawRequiredGridFlags;
			grid.data0 = owner;
			grid.data1 = cellIndex;
			grid.data2 = referenceIndex;
			grid.payload[2] = grid.payload[3] = 1.0f;
			grid.payload[4] = grid.payload[5] = grid.payload[6] = 1.0f;
			grid.payload[7] = grid.payload[8] = 1.0f;

			NRISpatialAbsenceGpuRecord& cell = source.gpuRecords[cellIndex];
			cell.flags = RawRequiredCellFlags;
			cell.data0 = owner;
			cell.data1 = referenceIndex;
			cell.data2 = 1u;
			cell.payload[1] = 1.0f;

			NRISpatialAbsenceGpuRecord& reference = source.gpuRecords[referenceIndex];
			reference.flags = RawRequiredReferenceFlags;
			reference.data0 = 0u;
			reference.data1 = InvalidIndex;
			reference.data2 = InvalidIndex;
			reference.payload[0] = (float)owner;
		};
		setFootprint(0u, 4u, 5u, 6u, 7u);
		setFootprint(1u, 8u, 9u, 10u, 11u);
		source.payloadHash = HashRawRecords(source);
		return source;
	}
}

bool GetNRISpatialAbsenceGpuSnapshotLayout(
	const NRISpatialAbsenceGpuSnapshot& snapshot,
	NRISpatialAbsenceGpuSnapshotLayout& layout)
{
	return ComputeLayout(snapshot.chunkCount, snapshot.negativeCount, snapshot.pairCount,
		snapshot.footprintCount, snapshot.cellCount, snapshot.referenceCount,
		snapshot.triangleCount, layout);
}

bool ValidateNRISpatialAbsenceGpuSnapshot(const NRISpatialAbsenceGpuSnapshot& snapshot)
{
	return snapshot.valid &&
		snapshot.failOpenFlags == NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_NONE &&
		ValidateTypedBlocks(snapshot, true);
}

bool BuildNRISpatialAbsenceGpuSnapshot(
	const NRISpatialAbsenceSnapshot& source,
	NRISpatialAbsenceGpuSnapshot& destination)
{
	const auto start = std::chrono::steady_clock::now();
	destination = {};
	auto fail = [&](uint32_t flag)
	{
		destination.valid = false;
		destination.failOpenFlags |= flag;
		destination.payloadHash = 0;
		destination.blocks.clear();
		destination.buildElapsedMilliseconds =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		return false;
	};

	if (source.gpuRecords.empty())
		return fail(NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_UNAVAILABLE);
	if ((source.gpuRecords[0].flags & NRI_SPATIAL_ABSENCE_GPU_RAY_QUERY_VALIDATED) == 0u)
		return fail(NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_UNSEALED);
	if (!source.HasCensusAuthority())
		return fail(NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_NOT_AUTHORITATIVE);
	if (source.payloadHash != HashRawRecords(source))
		return fail(NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_HASH_MISMATCH);

	ParsedSnapshot parsed;
	if (!ParseRawSnapshot(source, parsed))
		return fail(NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_INVALID);
	if (!PackTypedSnapshot(source, parsed, destination))
		return fail(NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_INDEX_OVERFLOW);
	if (!ValidateTypedBlocks(destination, false))
		return fail(NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_LAYOUT_INVALID);

	NRISpatialAbsenceGpuSnapshotLayout layout;
	if (!GetNRISpatialAbsenceGpuSnapshotLayout(destination, layout))
		return fail(NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_LAYOUT_INVALID);
	WriteHeaderAndFooter(destination, layout,
		NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_COMPLETE | NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_SEALED);
	destination.payloadHash = HashTypedBlocks(destination.blocks);
	destination.valid = true;
	if (!ValidateNRISpatialAbsenceGpuSnapshot(destination))
		return fail(NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SEAL_INVALID);

	destination.buildElapsedMilliseconds =
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	return true;
}

bool NRISpatialAbsenceGpuSnapshot::HasCensusAuthority(
	const NRISpatialAbsenceSnapshot& source) const
{
	return ValidateNRISpatialAbsenceGpuSnapshot(*this) && source.HasCensusAuthority() &&
		frameIndex == source.frameIndex && worldGeneration == source.worldGeneration &&
		captureSerial == source.captureSerial && sourcePayloadHash == source.payloadHash &&
		sourcePayloadHash == HashRawRecords(source) && chunkCount != 0u &&
		negativeCount == source.certifiedCount && pairCount == source.authorizedPairCount &&
		triangleCount == source.footprintTriangleCount &&
		cellCount == source.footprintGridCellCount &&
		FloatBits(center[0]) == FloatBits(source.center[0]) &&
		FloatBits(center[1]) == FloatBits(source.center[1]) &&
		FloatBits(center[2]) == FloatBits(source.center[2]) &&
		FloatBits(guardRadius) == FloatBits(source.guardRadius) &&
		FloatBits(actorGuardRadius) == FloatBits(source.actorGuardRadius);
}

bool NRISpatialAbsenceGpuSnapshot::HasNegativeAuthority(
	const NRISpatialAbsenceSnapshot& source) const
{
	return negativeCount != 0u && pairCount != 0u && source.HasNegativeAuthority() &&
		HasCensusAuthority(source);
}

bool RunNRISpatialAbsenceGpuSnapshotSelfTests(std::string* failureReason)
{
	auto fail = [&](const char* reason)
	{
		if (failureReason != nullptr)
			*failureReason = reason;
		return false;
	};
	if (failureReason != nullptr)
		failureReason->clear();

	NRISpatialAbsenceSnapshot source = MakeSyntheticRawSnapshot(true);
	NRISpatialAbsenceGpuSnapshot typed;
	if (!BuildNRISpatialAbsenceGpuSnapshot(source, typed) ||
		!ValidateNRISpatialAbsenceGpuSnapshot(typed) || !typed.HasCensusAuthority(source) ||
		!typed.HasNegativeAuthority(source))
	{
		return fail("complete sealed source did not publish typed negative authority");
	}
	NRISpatialAbsenceGpuSnapshotLayout layout;
	if (!GetNRISpatialAbsenceGpuSnapshotLayout(typed, layout) || typed.chunkCount != 2u ||
		typed.negativeCount != 1u || typed.pairCount != 1u || typed.footprintCount != 2u ||
		typed.cellCount != 2u || typed.referenceCount != 2u || typed.triangleCount != 2u ||
		layout.totalBlockCount != 26u || typed.blocks.size() * sizeof(typed.blocks[0]) != 416u)
	{
		return fail("typed source conversion produced the wrong fixed-order layout");
	}

	NRISpatialAbsenceGpuSnapshot corrupted = typed;
	corrupted.blocks[layout.triangleOffset].words[0] = 0x7fc00000u;
	if (ValidateNRISpatialAbsenceGpuSnapshot(corrupted))
		return fail("non-finite typed triangle corruption retained publication authority");
	corrupted = typed;
	corrupted.blocks[layout.footerOffset + 2u].words[3]++;
	if (ValidateNRISpatialAbsenceGpuSnapshot(corrupted))
		return fail("typed footer corruption retained publication authority");

	NRISpatialAbsenceSnapshot staleSource = source;
	staleSource.captureSerial++;
	if (typed.HasCensusAuthority(staleSource))
		return fail("typed authority ignored source capture identity");

	NRISpatialAbsenceSnapshot badHash = source;
	badHash.gpuRecords[3].data1 = 1u;
	NRISpatialAbsenceGpuSnapshot rejected;
	if (BuildNRISpatialAbsenceGpuSnapshot(badHash, rejected) ||
		(rejected.failOpenFlags & NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_HASH_MISMATCH) == 0u)
	{
		return fail("mutated raw bytes did not fail open on source hash mismatch");
	}
	badHash.payloadHash = HashRawRecords(badHash);
	if (BuildNRISpatialAbsenceGpuSnapshot(badHash, rejected) ||
		(rejected.failOpenFlags & NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_INVALID) == 0u)
	{
		return fail("invalid rehashed raw graph did not fail open during typed parsing");
	}

	NRISpatialAbsenceSnapshot unsealed = source;
	unsealed.gpuRecords[0].flags &= ~NRI_SPATIAL_ABSENCE_GPU_RAY_QUERY_VALIDATED;
	if (BuildNRISpatialAbsenceGpuSnapshot(unsealed, rejected) ||
		(rejected.failOpenFlags & NRI_SPATIAL_ABSENCE_GPU_SNAPSHOT_FAIL_SOURCE_UNSEALED) == 0u)
	{
		return fail("unsealed raw source did not fail open before typed conversion");
	}

	NRISpatialAbsenceSnapshot censusSource = MakeSyntheticRawSnapshot(false);
	NRISpatialAbsenceGpuSnapshot censusTyped;
	if (!BuildNRISpatialAbsenceGpuSnapshot(censusSource, censusTyped) ||
		!censusTyped.HasCensusAuthority(censusSource) ||
		censusTyped.HasNegativeAuthority(censusSource) || censusTyped.negativeCount != 0u ||
		censusTyped.pairCount != 0u || censusTyped.footprintCount != 0u)
	{
		return fail("census-only source did not preserve typed census authority");
	}
	return true;
}
