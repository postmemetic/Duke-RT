#pragma once

#include <cstdint>
#include <vector>

static constexpr float NRI_RUNTIME_LIGHT_SHADOW_REPLACEMENT_MARGIN = 0.125f;
static constexpr uint64_t NRI_RUNTIME_LIGHT_SHADOW_SELECTION_POLICY_FINGERPRINT = 0x4859535445524553ull;
static constexpr uint32_t NRI_RUNTIME_LIGHT_SHADOW_TRANSITION_SAMPLE_COUNT = 8u;

struct NRIRuntimeLightShadowSelectionSnapshot
{
	bool valid = false;
	uint64_t frameSerial = 0;
	uint64_t policyFingerprint = 0;
	uint64_t selectionHash = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	uint32_t tileSize = 0;
	uint32_t tileCountX = 0;
	uint32_t tileCountY = 0;
	uint32_t shadowBudget = 0;
	std::vector<uint32_t> tileKeyOffsets;
	std::vector<uint64_t> selectedStableKeys;

	bool IsCompatible(
		uint64_t candidateFrameSerial,
		uint32_t candidateRenderWidth,
		uint32_t candidateRenderHeight,
		uint32_t candidateTileSize,
		uint32_t candidateTileCountX,
		uint32_t candidateTileCountY,
		uint32_t candidateShadowBudget,
		uint64_t candidatePolicyFingerprint) const;
};

struct NRIRuntimeLightShadowTransitionSample
{
	enum Kind : uint32_t
	{
		Retained = 1u,
		Replaced = 2u,
		Expired = 3u,
	};

	uint32_t tileIndex = 0;
	uint32_t kind = 0;
	uint64_t previousStableKey = 0;
	uint64_t selectedStableKey = 0;
};

struct NRIRuntimeLightShadowTransitionTelemetry
{
	uint32_t retainedReferenceCount = 0;
	uint32_t replacedReferenceCount = 0;
	uint32_t expiredReferenceCount = 0;
	uint64_t retainedKeyHash = 0;
	uint64_t replacedKeyHash = 0;
	uint64_t expiredKeyHash = 0;
	uint32_t retainedSampleCount = 0;
	uint32_t replacedSampleCount = 0;
	uint32_t expiredSampleCount = 0;
	NRIRuntimeLightShadowTransitionSample retainedSamples[NRI_RUNTIME_LIGHT_SHADOW_TRANSITION_SAMPLE_COUNT] = {};
	NRIRuntimeLightShadowTransitionSample replacedSamples[NRI_RUNTIME_LIGHT_SHADOW_TRANSITION_SAMPLE_COUNT] = {};
	NRIRuntimeLightShadowTransitionSample expiredSamples[NRI_RUNTIME_LIGHT_SHADOW_TRANSITION_SAMPLE_COUNT] = {};
};

// Owns chronological shadow-selection history independently of rotating GPU
// frame-slot upload caches. Cluster builds stage a candidate snapshot; only a
// successful authoritative main-view render may commit it.
class NRIRuntimeLightShadowSelectionHistory
{
public:
	void BeginFrame(uint64_t frameSerial, bool authoritative);
	const NRIRuntimeLightShadowSelectionSnapshot* GetCommitted(
		uint64_t frameSerial,
		uint32_t renderWidth,
		uint32_t renderHeight,
		uint32_t tileSize,
		uint32_t tileCountX,
		uint32_t tileCountY,
		uint32_t shadowBudget,
		uint64_t policyFingerprint) const;
	void Stage(const NRIRuntimeLightShadowSelectionSnapshot& snapshot);
	void Commit(uint64_t frameSerial);
	void Discard(uint64_t frameSerial);
	void InvalidateHistory();
	void Reset();

	const NRIRuntimeLightShadowSelectionSnapshot& GetCommittedSnapshot() const { return mCommitted; }

private:
	NRIRuntimeLightShadowSelectionSnapshot mCommitted;
	NRIRuntimeLightShadowSelectionSnapshot mPending;
	uint64_t mCurrentFrameSerial = 0;
	bool mAuthoritative = false;
};
