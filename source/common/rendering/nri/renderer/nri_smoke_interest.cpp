#include "nri_smoke_interest.h"

#include "nri_smoke_admission.h"
#include "../scene/nri_map_world.h"

#include "lightoverlay.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace
{
	bool ChunkMarked(const std::vector<uint32_t>& words, uint32_t chunkIndex)
	{
		const size_t wordIndex = chunkIndex >> 5u;
		return wordIndex < words.size() && (words[wordIndex] & (1u << (chunkIndex & 31u))) != 0u;
	}

	bool MarkChunk(std::vector<uint32_t>& words, uint32_t chunkIndex)
	{
		const size_t wordIndex = chunkIndex >> 5u;
		if (wordIndex >= words.size())
			return false;
		const uint32_t mask = 1u << (chunkIndex & 31u);
		const bool changed = (words[wordIndex] & mask) == 0u;
		words[wordIndex] |= mask;
		return changed;
	}

	float DistanceSquared(const float left[3], const float right[3])
	{
		const float dx = left[0] - right[0];
		const float dy = left[1] - right[1];
		const float dz = left[2] - right[2];
		return dx * dx + dy * dy + dz * dz;
	}

	void MapEmitterCenter(const ResolvedLightOverlayMapSmokeEmitterRule& rule,
		float renderCenter[3], DVector3& worldCenter)
	{
		worldCenter = DVector3(
			(double)rule.position[0] + (double)rule.normal[0] * (double)rule.offset,
			(double)rule.position[1] + (double)rule.normal[1] * (double)rule.offset,
			(double)rule.position[2] + (double)rule.normal[2] * (double)rule.offset);
		renderCenter[0] = (float)worldCenter.X;
		renderCenter[1] = (float)-worldCenter.Z;
		renderCenter[2] = (float)-worldCenter.Y;
	}

	uint32_t ResolveEmitterChunk(const DVector3& worldCenter, const nri_scene::PTMapWorld& world)
	{
		sectortype* emitterSector = nullptr;
		updatesectorz(worldCenter, &emitterSector);
		if (emitterSector == nullptr)
			updatesector(worldCenter, &emitterSector);
		if (emitterSector == nullptr)
			return UINT32_MAX;
		const int32_t sectorIndex = sector.IndexOf(emitterSector);
		if (sectorIndex < 0 || (size_t)sectorIndex >= world.sectorChunkLookup.size())
			return UINT32_MAX;
		return world.sectorChunkLookup[(size_t)sectorIndex];
	}
}

NRISmokeInterestTier NRISmokeInterestSnapshot::Resolve(uint32_t sourceId) const
{
	const auto found = std::find_if(sources.begin(), sources.end(), [sourceId](const auto& source)
	{
		return source.sourceId == sourceId;
	});
	// Unknown sources include interactive effects and editor previews. They must
	// remain prompt rather than being suppressed by ambient-map policy.
	return found != sources.end() ? found->tier : NRISmokeInterestTier::Hot;
}

void NRISmokeInterestTracker::Reset()
{
	mActiveMapName.clear();
	mSourceStates.clear();
	mSnapshot = {};
	mJumpOrigin[0] = mJumpOrigin[1] = mJumpOrigin[2] = 0.0f;
	mJumpGraceUntil = 0u;
}

void NRISmokeInterestTracker::Update(const NRISmokeInterestFrameInput& input)
{
	mSnapshot = {};
	mSnapshot.rendererFrame = input.rendererFrame;
	if (input.cameraPosition == nullptr || input.mapWorld == nullptr ||
		input.visibleChunkWords == nullptr || input.overlays == nullptr)
		return;

	const std::string activeMapName = input.overlays->activeMapName.GetChars();
	if (mActiveMapName != activeMapName)
	{
		mActiveMapName = activeMapName;
		mSourceStates.clear();
		mJumpGraceUntil = 0u;
	}
	for (auto& entry : mSourceStates)
		entry.second.observed = false;

	std::vector<uint32_t> positiveChunks = *input.visibleChunkWords;
	positiveChunks.resize(std::max<size_t>(positiveChunks.size(),
		(input.mapWorld->chunks.size() + 31u) / 32u), 0u);
	bool changed = true;
	for (size_t iteration = 0; changed && iteration < input.mapWorld->portals.size(); ++iteration)
	{
		changed = false;
		for (const auto& portal : input.mapWorld->portals)
		{
			if (portal.sourceChunkIndex == UINT32_MAX || !ChunkMarked(positiveChunks, portal.sourceChunkIndex))
				continue;
			if (portal.runtimeBoundTarget)
				mSnapshot.runtimePortalUncertain = true;
			for (uint32_t targetOffset = 0; targetOffset < portal.targetCount; ++targetOffset)
			{
				const uint32_t targetIndex = portal.firstTarget + targetOffset;
				if (targetIndex >= input.mapWorld->portalTargets.size())
					break;
				const uint32_t chunkIndex = input.mapWorld->portalTargets[targetIndex].chunkIndex;
				if (chunkIndex != UINT32_MAX && MarkChunk(positiveChunks, chunkIndex))
				{
					mSnapshot.portalPromotedChunks++;
					changed = true;
				}
			}
		}
	}
	mSnapshot.positiveChunkWords = positiveChunks;
	mSnapshot.conservativeInterestComplete = input.mapWorld->valid &&
		!mSnapshot.runtimePortalUncertain;

	float predictedCamera[3] = { input.cameraPosition[0], input.cameraPosition[1], input.cameraPosition[2] };
	if (input.hasPreviousCamera && input.previousCameraPosition != nullptr)
	{
		const float cameraDeltaSquared = DistanceSquared(input.cameraPosition, input.previousCameraPosition);
		mSnapshot.cameraJump = cameraDeltaSquared >= CameraJumpDistance * CameraJumpDistance;
		if (mSnapshot.cameraJump)
		{
			std::copy(input.previousCameraPosition, input.previousCameraPosition + 3, mJumpOrigin);
			mJumpGraceUntil = input.rendererFrame + CameraJumpGraceFrames;
		}
		else if (cameraDeltaSquared > 0.0f)
		{
			const float deltaLength = std::sqrt(cameraDeltaSquared);
			const float scale = std::min(MaximumPrefetchDistance / deltaLength, 8.0f);
			for (uint32_t axis = 0; axis < 3u; ++axis)
				predictedCamera[axis] += (input.cameraPosition[axis] - input.previousCameraPosition[axis]) * scale;
		}
	}

	if (input.mapEmittersEnabled)
	{
		for (const auto& rule : input.overlays->mapSmokeEmitterRules)
		{
			if (!input.overlays->currentMapAvailable || !rule.styleResolved ||
				!rule.hasPosition || !rule.hasNormal || !rule.hasSize ||
				rule.mapName.CompareNoCase(input.overlays->activeMapName) != 0)
				continue;
			const uint32_t sourceId = NRIMakeSmokeSourceId("map", input.overlays->activeMapName.GetChars(), rule.id.GetChars());
			SourceState& state = mSourceStates[sourceId];
			state.observed = true;
			float center[3];
			DVector3 worldCenter;
			MapEmitterCenter(rule, center, worldCenter);
			const uint32_t chunkIndex = ResolveEmitterChunk(worldCenter, *input.mapWorld);
			const bool positive = chunkIndex != UINT32_MAX && ChunkMarked(positiveChunks, chunkIndex);
			if (positive)
				state.lastPositiveFrame = input.rendererFrame;
			const bool recentPositive = state.lastPositiveFrame != UINT32_MAX &&
				input.rendererFrame - state.lastPositiveFrame <= RecentVisibilityFrames;
			const bool hotDistance = DistanceSquared(center, input.cameraPosition) <=
				(state.tier == NRISmokeInterestTier::Hot ? HotLeaveDistance * HotLeaveDistance : HotEnterDistance * HotEnterDistance);
			const bool prefetch = DistanceSquared(center, predictedCamera) <= HotEnterDistance * HotEnterDistance;
			const bool warmDistance = DistanceSquared(center, input.cameraPosition) <=
				(state.tier != NRISmokeInterestTier::Dormant ? WarmLeaveDistance * WarmLeaveDistance : WarmEnterDistance * WarmEnterDistance);
			const bool teleportGrace = input.rendererFrame <= mJumpGraceUntil &&
				DistanceSquared(center, mJumpOrigin) <= WarmLeaveDistance * WarmLeaveDistance;

			if (positive || hotDistance)
				state.tier = NRISmokeInterestTier::Hot;
			else if (prefetch || warmDistance || recentPositive || teleportGrace || mSnapshot.runtimePortalUncertain)
				state.tier = NRISmokeInterestTier::Warm;
			else
				state.tier = NRISmokeInterestTier::Dormant;

			mSnapshot.sources.push_back({ sourceId, state.tier, chunkIndex, state.lastPositiveFrame,
				positive, prefetch, recentPositive, teleportGrace });
			if (state.tier == NRISmokeInterestTier::Hot) mSnapshot.hotCount++;
			else if (state.tier == NRISmokeInterestTier::Warm) mSnapshot.warmCount++;
			else mSnapshot.dormantCount++;
			if (positive) mSnapshot.positiveCount++;
		}
	}

	for (auto it = mSourceStates.begin(); it != mSourceStates.end(); )
		it = !it->second.observed ? mSourceStates.erase(it) : std::next(it);
	std::sort(mSnapshot.sources.begin(), mSnapshot.sources.end(), [](const auto& left, const auto& right)
	{
		return left.sourceId < right.sourceId;
	});
}
