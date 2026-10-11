#include "nri_smoke_transient_motion.h"

#include <algorithm>
#include <cmath>

const std::vector<NRISmokeTransientPreviousLobeGpu>& NRISmokeTransientMotion::Prepare(
	uint64_t rendererFrame, const std::vector<NRISmokeTransientLobeGpu>& lobes, bool reset)
{
	const bool consecutive = !reset && mPreviousFrame != UINT64_MAX &&
		rendererFrame == mPreviousFrame + 1u;
	mPendingFrame = rendererFrame;
	mPending.clear();
	mOutput.assign(lobes.size(), {});
	const auto valid = [](const NRISmokeTransientLobeGpu& lobe)
	{
		return lobe.shape == 0u && (lobe.flags & 1u) != 0u && lobe.groupSlot != UINT32_MAX &&
			std::isfinite(lobe.radius) && lobe.radius > 0.0f &&
			std::isfinite(lobe.densityScale) && lobe.densityScale > 0.0f &&
			std::all_of(std::begin(lobe.position), std::end(lobe.position),
				[](float value) { return std::isfinite(value); });
	};
	for (const auto& lobe : lobes)
	{
		// The current tracking transform is spherical. Unsupported shapes keep
		// current-frame opacity instead of receiving incorrect rigid motion.
		if (!valid(lobe)) continue;
		Record record;
		record.key = { lobe.epoch, lobe.groupSlot, lobe.groupGeneration,
			lobe.deterministicSeed, lobe.styleIndex, lobe.transientClass };
		std::copy_n(lobe.position, 3, record.transform.position);
		record.transform.radius = lobe.radius;
		mPending.push_back(record);
	}
	const auto less = [](const Record& a, const Record& b) { return a.key < b.key; };
	std::sort(mPending.begin(), mPending.end(), less);
	// Never guess between ambiguous seeds; invalidate every duplicate, not
	// merely whichever record happens to sort second.
	for (size_t begin = 0u; begin < mPending.size();)
	{
		size_t end = begin + 1u;
		while (end < mPending.size() && mPending[end].key == mPending[begin].key) ++end;
		if (end - begin > 1u)
			for (size_t i = begin; i < end; ++i) mPending[i].transform.radius = 0.0f;
		begin = end;
	}
	if (!consecutive) return mOutput;
	for (size_t index = 0; index < lobes.size(); ++index)
	{
		const auto& lobe = lobes[index];
		if (!valid(lobe)) continue;
		Record probe;
		probe.key = { lobe.epoch, lobe.groupSlot, lobe.groupGeneration,
			lobe.deterministicSeed, lobe.styleIndex, lobe.transientClass };
		const auto current = std::lower_bound(mPending.begin(), mPending.end(), probe, less);
		const auto previous = std::lower_bound(mPrevious.begin(), mPrevious.end(), probe, less);
		if (current != mPending.end() && current->key == probe.key && current->transform.radius > 0.0f &&
			previous != mPrevious.end() && previous->key == probe.key && previous->transform.radius > 0.0f)
			mOutput[index] = previous->transform;
	}
	return mOutput;
}

void NRISmokeTransientMotion::Commit()
{
	mPrevious.swap(mPending);
	mPreviousFrame = mPendingFrame;
	mPendingFrame = UINT64_MAX;
}

void NRISmokeTransientMotion::Reset()
{
	mPrevious.clear();
	mPending.clear();
	mOutput.clear();
	mPreviousFrame = mPendingFrame = UINT64_MAX;
}
