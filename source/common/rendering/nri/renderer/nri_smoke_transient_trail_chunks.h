#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct NRISmokeTransientTrailChunkRange
{
	uint32_t firstLogical = 0u;
	uint32_t lastLogical = 0u;
};

// Converts the authored spatial cadence into a much smaller transient-only
// group cadence. Four lobes are the Low-profile admission ceiling. Under pool
// pressure the owner may reduce a group to two; the lobe builder then expands
// the endpoint supports conservatively across this same span. Planning from the
// ceiling avoids multiplying normal-tick groups solely because of that fallback.
// The small shoulder allowance avoids splitting the normal 40.25-unit Duke RPG
// game-tick displacement when 4-unit cadence produces eleven crossings.
inline uint32_t NRIPlanSmokeTransientTrailChunkCount(uint32_t logicalCrossings,
	uint32_t maximumChunks, float spacing, float initialRadius,
	float minimumLobeRadiusScale, uint32_t requestedLobes)
{
	if (logicalCrossings == 0u || maximumChunks == 0u) return 0u;
	if (!std::isfinite(spacing) || spacing <= 0.0f ||
		!std::isfinite(initialRadius) || initialRadius <= 0.0f ||
		!std::isfinite(minimumLobeRadiusScale) || minimumLobeRadiusScale <= 0.0f)
		return std::min(logicalCrossings, maximumChunks);

	constexpr uint32_t LowProfileLobeCeiling = 4u;
	constexpr float ShoulderOverlap = 0.9f;
	constexpr float NormalTickShoulderAllowance = 1.1f;
	const uint32_t supportLobes = std::max(2u,
		std::min(std::max(requestedLobes, 1u), LowProfileLobeCeiling));
	const float minimumRadius = initialRadius * minimumLobeRadiusScale;
	const float preferredSpan = 2.0f * ShoulderOverlap * minimumRadius *
		static_cast<float>(supportLobes - 1u) * NormalTickShoulderAllowance;
	const uint32_t logicalPerChunk = std::max(1u,
		static_cast<uint32_t>(std::floor(preferredSpan / spacing)));
	const uint32_t requiredChunks = static_cast<uint32_t>(
		(static_cast<uint64_t>(logicalCrossings) + logicalPerChunk - 1u) /
		logicalPerChunk);
	return std::min({ logicalCrossings, maximumChunks,
		std::max(requiredChunks, 1u) });
}

// Partitions every logical crossing exactly once. If an extreme hitch needs
// more than maximumChunks, the lobe builder conservatively grows support for
// these larger final ranges instead of dropping the old prefix.
inline NRISmokeTransientTrailChunkRange NRIGetSmokeTransientTrailChunkRange(
	uint32_t logicalCrossings, uint32_t chunkCount, uint32_t chunkIndex)
{
	NRISmokeTransientTrailChunkRange range = {};
	if (logicalCrossings == 0u || chunkCount == 0u || chunkIndex >= chunkCount)
		return range;
	range.firstLogical = static_cast<uint32_t>(
		(static_cast<uint64_t>(chunkIndex) * logicalCrossings) / chunkCount);
	range.lastLogical = static_cast<uint32_t>(
		(static_cast<uint64_t>(chunkIndex + 1u) * logicalCrossings) / chunkCount) - 1u;
	return range;
}
