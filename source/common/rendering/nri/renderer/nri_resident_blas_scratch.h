#pragma once

#include "nri_resources.h"
#include <vector>

// Resident BLAS scratch has command-stream lifetime, independent of renderer
// history indices and resident upload frame recycling. Active storage remains
// in the renderer; this owner holds only allocations displaced by growth.
class NRIResidentBlasScratchRetirement
{
public:
	enum class Result { Unchanged, Grown, NoRecordingOwner, AllocationFailed };

	template<class Allocate, class Destroy>
	Result Ensure(NRIBufferResource& active, uint64_t requiredBytes, uint64_t recordingFence,
		Allocate&& allocate, Destroy&& destroy)
	{
		if (active.buffer != nullptr && active.size >= requiredBytes) return Result::Unchanged;
		// Fence zero means no open command stream, not completion of old uses.
		if (recordingFence == 0) return Result::NoRecordingOwner;
		if (active.buffer != nullptr) mRetired.reserve(mRetired.size() + 1);
		NRIBufferResource replacement = {};
		if (!allocate(replacement, requiredBytes))
		{
			destroy(replacement); // APIs may leave a partial allocation on failure.
			return Result::AllocationFailed;
		}
		if (active.buffer != nullptr)
		{
			mRetired.push_back({ active, recordingFence });
			mRetainedBytes += active.memorySize;
			if (mRetainedBytes > mPeakRetainedBytes) mPeakRetainedBytes = mRetainedBytes;
		}
		active = replacement;
		const uint64_t liveBytes = active.memorySize + mRetainedBytes;
		if (liveBytes > mPeakLiveBytes) mPeakLiveBytes = liveBytes;
		return Result::Grown;
	}

	template<class IsComplete, class Destroy>
	void Collect(IsComplete&& isComplete, Destroy&& destroy)
	{
		// An abandoned fence is deliberately not complete. Its allocations stay
		// owned until the device has waited AND discarded recordings at cleanup.
		size_t kept = 0;
		for (auto& entry : mRetired)
		{
			if (isComplete(entry.commandFence))
			{
				mRetainedBytes -= entry.resource.memorySize;
				destroy(entry.resource);
			}
			else mRetired[kept++] = entry;
		}
		mRetired.resize(kept);
	}

	// Caller must have completed submitted work and discarded recordings that
	// could reference these resources. Device-idle alone is insufficient.
	template<class Destroy>
	void ReleaseAfterWaitAndDiscard(Destroy&& destroy)
	{
		for (auto& entry : mRetired) destroy(entry.resource);
		mRetired.clear();
		mRetainedBytes = 0;
	}

	size_t RetainedCount() const { return mRetired.size(); }
	uint64_t RetainedBytes() const { return mRetainedBytes; }
	uint64_t PeakRetainedBytes() const { return mPeakRetainedBytes; }
	uint64_t PeakLiveBytes() const { return mPeakLiveBytes; }

private:
	struct Entry { NRIBufferResource resource; uint64_t commandFence; };
	std::vector<Entry> mRetired;
	uint64_t mRetainedBytes = 0;
	uint64_t mPeakRetainedBytes = 0;
	uint64_t mPeakLiveBytes = 0;
};
