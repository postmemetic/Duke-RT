#include "nri_fluid_pan_history.h"

#include <algorithm>
#include <cmath>

void NRIFluidPanHistory::Reset()
{
	mCommitted.clear();
	mPrepared.clear();
	mStaged.clear();
	mPreparedRows.clear();
	mMapEpoch = 0;
	mStagedSerial = 0;
	mCommittedGameplaySeconds = 0.0f;
	mPreparedGameplaySeconds = 0.0f;
	mStagedGameplaySeconds = 0.0f;
	mHasMapEpoch = false;
	mHasCommitted = false;
	mHasPrepared = false;
	mHasStaged = false;
}

void NRIFluidPanHistory::BeginPrepare(uint64_t mapEpoch, float gameplaySeconds, uint32_t planeCount)
{
	const float currentSeconds = std::isfinite(gameplaySeconds) ? gameplaySeconds : 0.0f;
	if (!mHasMapEpoch || mapEpoch != mMapEpoch ||
		(mHasCommitted && currentSeconds < mCommittedGameplaySeconds))
	{
		Reset();
		mMapEpoch = mapEpoch;
		mHasMapEpoch = true;
	}
	mPreparedGameplaySeconds = currentSeconds;
	mPrepared.assign(planeCount, {});
	// A valid descriptor is mandatory even when no map planes exist.
	mPreparedRows.assign(std::max(planeCount, 1u), {});
	mHasPrepared = true;
}

void NRIFluidPanHistory::PreparePlane(uint32_t planeIndex, double rawPanX, double rawPanY, bool hasMoved,
	double authoredPanX, double authoredPanY, uint32_t movedAxisMask)
{
	if (!mHasPrepared || planeIndex >= mPrepared.size())
		return;
	const PlaneState previous = planeIndex < mCommitted.size() ? mCommitted[planeIndex] : PlaneState{};
	PlaneState& current = mPrepared[planeIndex];
	const float panX = (float)(rawPanX / 256.0);
	const float panY = (float)(rawPanY / 256.0);
	current.active = hasMoved && std::isfinite(panX) && std::isfinite(panY) &&
		std::isfinite(authoredPanX) && std::isfinite(authoredPanY);
	if (current.active)
	{
		current.pan[0] = panX;
		current.pan[1] = panY;
		// Retain the carrier's procedural phase once authored transport starts,
		// including stopped/pause frames. A reset/never-moved plane opts out.
		current.phaseSeconds = previous.active ? previous.phaseSeconds : mPreparedGameplaySeconds;
		current.authoredPan[0] = authoredPanX;
		current.authoredPan[1] = authoredPanY;
		current.rateAnchorSeconds = mPreparedGameplaySeconds;
		current.movedAxisMask = movedAxisMask & 3u;
		if (previous.active && previous.movedAxisMask == current.movedAxisMask)
		{
			current.nominalRate[0] = previous.nominalRate[0];
			current.nominalRate[1] = previous.nominalRate[1];
			const double elapsed = (double)mPreparedGameplaySeconds - previous.rateAnchorSeconds;
			if (elapsed > 0.0)
			{
				// Only authoritative simulation movement defines nominal foam age.
				// Presented/interpolated pan is still used directly for displacement.
				const float rateX = (float)((authoredPanX - previous.authoredPan[0]) / (256.0 * elapsed));
				const float rateY = (float)((authoredPanY - previous.authoredPan[1]) / (256.0 * elapsed));
				if (std::isfinite(rateX) && std::isfinite(rateY) && (rateX != 0.0f || rateY != 0.0f))
				{
					current.nominalRate[0] = rateX;
					current.nominalRate[1] = rateY;
				}
			}
			else
			{
				current.authoredPan[0] = previous.authoredPan[0];
				current.authoredPan[1] = previous.authoredPan[1];
				current.rateAnchorSeconds = previous.rateAnchorSeconds;
			}
			// Zero movement and paused/sub-tick frames keep the nominal rate.
			// This is a bounded analytic-trail heuristic, never fluid simulation.
		}
		// First activation or an authored axis reset seeds rate zero, avoiding
		// huge inferred velocities from restored accumulated displacement.
	}
	NRIFluidPanGpuData& row = mPreparedRows[planeIndex];
	row.currentPan[0] = current.pan[0];
	row.currentPan[1] = current.pan[1];
	row.previousPan[0] = previous.pan[0];
	row.previousPan[1] = previous.pan[1];
	row.currentPhaseSeconds = current.phaseSeconds;
	row.previousPhaseSeconds = previous.phaseSeconds;
	row.flags = (current.active ? NRI_FLUID_PAN_CURRENT_ACTIVE : 0u) |
		(previous.active ? NRI_FLUID_PAN_PREVIOUS_ACTIVE : 0u);
	row.currentPanRate[0] = current.nominalRate[0];
	row.currentPanRate[1] = current.nominalRate[1];
	row.previousPanRate[0] = previous.nominalRate[0];
	row.previousPanRate[1] = previous.nominalRate[1];
}

float NRIFluidPanHistory::PreviousGameplaySeconds(float currentSeconds) const
{
	return mHasCommitted ? mCommittedGameplaySeconds : currentSeconds;
}

bool NRIFluidPanHistory::StagePrepared(uint64_t serial)
{
	if (!mHasPrepared || serial == 0)
		return false;
	mStaged = mPrepared;
	mStagedGameplaySeconds = mPreparedGameplaySeconds;
	mStagedSerial = serial;
	mHasStaged = true;
	mHasPrepared = false;
	return true;
}

bool NRIFluidPanHistory::CommitSubmitted(uint64_t serial)
{
	if (!mHasStaged || mStagedSerial != serial)
		return false;
	mCommitted.swap(mStaged);
	mCommittedGameplaySeconds = mStagedGameplaySeconds;
	mHasCommitted = true;
	DiscardStaged();
	return true;
}

void NRIFluidPanHistory::DiscardStaged()
{
	mStaged.clear();
	mStagedSerial = 0;
	mHasStaged = false;
}
