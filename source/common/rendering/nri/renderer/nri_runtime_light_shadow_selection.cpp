#include "nri_runtime_light_shadow_selection.h"

#include <cstddef>
#include <utility>

bool NRIRuntimeLightShadowSelectionSnapshot::IsCompatible(
	uint64_t candidateFrameSerial,
	uint32_t candidateRenderWidth,
	uint32_t candidateRenderHeight,
	uint32_t candidateTileSize,
	uint32_t candidateTileCountX,
	uint32_t candidateTileCountY,
	uint32_t candidateShadowBudget,
	uint64_t candidatePolicyFingerprint) const
{
	return valid &&
		frameSerial + 1u == candidateFrameSerial &&
		renderWidth == candidateRenderWidth &&
		renderHeight == candidateRenderHeight &&
		tileSize == candidateTileSize &&
		tileCountX == candidateTileCountX &&
		tileCountY == candidateTileCountY &&
		shadowBudget == candidateShadowBudget &&
		policyFingerprint == candidatePolicyFingerprint &&
		tileKeyOffsets.size() == (std::size_t)tileCountX * tileCountY + 1u;
}

void NRIRuntimeLightShadowSelectionHistory::BeginFrame(uint64_t frameSerial, bool authoritative)
{
	mCurrentFrameSerial = frameSerial;
	mAuthoritative = authoritative;
	mPending = {};
}

const NRIRuntimeLightShadowSelectionSnapshot* NRIRuntimeLightShadowSelectionHistory::GetCommitted(
	uint64_t frameSerial,
	uint32_t renderWidth,
	uint32_t renderHeight,
	uint32_t tileSize,
	uint32_t tileCountX,
	uint32_t tileCountY,
	uint32_t shadowBudget,
	uint64_t policyFingerprint) const
{
	return mCommitted.IsCompatible(
		frameSerial,
		renderWidth,
		renderHeight,
		tileSize,
		tileCountX,
		tileCountY,
		shadowBudget,
		policyFingerprint) ? &mCommitted : nullptr;
}

void NRIRuntimeLightShadowSelectionHistory::Stage(const NRIRuntimeLightShadowSelectionSnapshot& snapshot)
{
	if (!mAuthoritative || !snapshot.valid || snapshot.frameSerial != mCurrentFrameSerial)
	{
		return;
	}
	mPending = snapshot;
}

void NRIRuntimeLightShadowSelectionHistory::Commit(uint64_t frameSerial)
{
	if (mAuthoritative && frameSerial == mCurrentFrameSerial &&
		mPending.valid && mPending.frameSerial == frameSerial)
	{
		mCommitted = std::move(mPending);
	}
	mPending = {};
	mAuthoritative = false;
}

void NRIRuntimeLightShadowSelectionHistory::Discard(uint64_t frameSerial)
{
	if (frameSerial == mCurrentFrameSerial)
	{
		mPending = {};
		mAuthoritative = false;
	}
}

void NRIRuntimeLightShadowSelectionHistory::InvalidateHistory()
{
	mCommitted = {};
	mPending = {};
}

void NRIRuntimeLightShadowSelectionHistory::Reset()
{
	InvalidateHistory();
	mCurrentFrameSerial = 0;
	mAuthoritative = false;
}
