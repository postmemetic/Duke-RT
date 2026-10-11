#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nri_scene
{
	struct PTMapWorld;
}

struct ResolvedLightOverlaySet;

enum class NRISmokeInterestTier : uint8_t
{
	Dormant = 0,
	Warm,
	Hot,
};

struct NRISmokeSourceInterest
{
	uint32_t sourceId = 0;
	NRISmokeInterestTier tier = NRISmokeInterestTier::Hot;
	uint32_t chunkIndex = UINT32_MAX;
	uint32_t lastPositiveFrame = UINT32_MAX;
	bool positiveVisibility = false;
	bool movementPrefetch = false;
	bool recentVisibility = false;
	bool teleportGrace = false;
};

struct NRISmokeInterestSnapshot
{
	uint32_t rendererFrame = UINT32_MAX;
	uint32_t hotCount = 0;
	uint32_t warmCount = 0;
	uint32_t dormantCount = 0;
	uint32_t positiveCount = 0;
	uint32_t portalPromotedChunks = 0;
	bool runtimePortalUncertain = false;
	bool cameraJump = false;
	bool conservativeInterestComplete = false;
	std::vector<uint32_t> positiveChunkWords;
	std::vector<NRISmokeSourceInterest> sources;

	NRISmokeInterestTier Resolve(uint32_t sourceId) const;
};

struct NRISmokeInterestFrameInput
{
	uint32_t rendererFrame = 0;
	const float* cameraPosition = nullptr;
	const float* previousCameraPosition = nullptr;
	bool hasPreviousCamera = false;
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	const std::vector<uint32_t>* visibleChunkWords = nullptr;
	const ResolvedLightOverlaySet* overlays = nullptr;
	bool mapEmittersEnabled = true;
};

class NRISmokeInterestTracker
{
public:
	void Update(const NRISmokeInterestFrameInput& input);
	void Reset();
	const NRISmokeInterestSnapshot& GetSnapshot() const { return mSnapshot; }

	static constexpr float HotEnterDistance = 1024.0f;
	static constexpr float HotLeaveDistance = 1280.0f;
	static constexpr float WarmEnterDistance = 2048.0f;
	static constexpr float WarmLeaveDistance = 2560.0f;
	static constexpr float MaximumPrefetchDistance = 512.0f;
	static constexpr float CameraJumpDistance = 768.0f;
	static constexpr uint32_t RecentVisibilityFrames = 240u;
	static constexpr uint32_t CameraJumpGraceFrames = 120u;

private:
	struct SourceState
	{
		NRISmokeInterestTier tier = NRISmokeInterestTier::Dormant;
		uint32_t lastPositiveFrame = UINT32_MAX;
		bool observed = false;
	};

	std::string mActiveMapName;
	std::unordered_map<uint32_t, SourceState> mSourceStates;
	NRISmokeInterestSnapshot mSnapshot;
	float mJumpOrigin[3] = {};
	uint32_t mJumpGraceUntil = 0;
};
