#pragma once

#include "nri_smoke_transient_clouds.h"

// Shader t5 input: preceding rendered center/radius in current-lobe order.
// A zero radius means no trustworthy predecessor (birth, reentry, reset, etc.).
struct NRISmokeTransientPreviousLobeGpu
{
	float position[3] = {};
	float radius = 0.0f;
};
static_assert(sizeof(NRISmokeTransientPreviousLobeGpu) == 16u);

// Pure snapshot matching, separate from GPU resource lifetime and simulation.
// Prepare is transactional: only Commit makes an uploaded snapshot a predecessor.
class NRISmokeTransientMotion
{
public:
	const std::vector<NRISmokeTransientPreviousLobeGpu>& Prepare(uint64_t rendererFrame,
		const std::vector<NRISmokeTransientLobeGpu>& lobes, bool reset = false);
	void Commit();
	void Reset();

private:
	struct Record
	{
		std::array<uint32_t, 6> key = {};
		NRISmokeTransientPreviousLobeGpu transform;
	};
	std::vector<Record> mPrevious;
	std::vector<Record> mPending;
	std::vector<NRISmokeTransientPreviousLobeGpu> mOutput;
	uint64_t mPreviousFrame = UINT64_MAX;
	uint64_t mPendingFrame = UINT64_MAX;
};
