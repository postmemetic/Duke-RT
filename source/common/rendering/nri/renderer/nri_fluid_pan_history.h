#pragma once

#include "nri_fluid_surface_gpu.h"

#include <cstdint>
#include <vector>

// Resource preparation may also occur for portal/auxiliary views. It must not
// advance main-frame history or overwrite a snapshot already awaiting submit.
class NRIFluidPanHistory
{
public:
	void Reset();
	void BeginPrepare(uint64_t mapEpoch, float gameplaySeconds, uint32_t planeCount);
	void PreparePlane(uint32_t planeIndex, double rawPanX, double rawPanY, bool hasMoved,
		double authoredPanX, double authoredPanY, uint32_t movedAxisMask);
	const std::vector<NRIFluidPanGpuData>& PreparedRows() const { return mPreparedRows; }
	float PreviousGameplaySeconds(float currentSeconds) const;
	bool StagePrepared(uint64_t serial);
	bool CommitSubmitted(uint64_t serial);
	void DiscardStaged();

private:
	struct PlaneState
	{
		float pan[2] = {};
		float phaseSeconds = 0.0f;
		double authoredPan[2] = {};
		float rateAnchorSeconds = 0.0f;
		float nominalRate[2] = {};
		uint32_t movedAxisMask = 0;
		bool active = false;
	};

	std::vector<PlaneState> mCommitted;
	std::vector<PlaneState> mPrepared;
	std::vector<PlaneState> mStaged;
	std::vector<NRIFluidPanGpuData> mPreparedRows;
	uint64_t mMapEpoch = 0;
	uint64_t mStagedSerial = 0;
	float mCommittedGameplaySeconds = 0.0f;
	float mPreparedGameplaySeconds = 0.0f;
	float mStagedGameplaySeconds = 0.0f;
	bool mHasMapEpoch = false;
	bool mHasCommitted = false;
	bool mHasPrepared = false;
	bool mHasStaged = false;
};
