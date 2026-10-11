#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

// Scratch is leased for a synchronous call. Reentrant callers get a distinct
// slot; heap ownership keeps an outer reference stable when the slot list grows.
// The number of retained slots is bounded by peak active call depth.
template<class T>
class NRIRetainedScratch
{
public:
	class Lease
	{
	public:
		explicit Lease(NRIRetainedScratch& owner) : mOwner(owner), mIndex(owner.mDepth)
		{
			if (mIndex == owner.mSlots.size())
				owner.mSlots.emplace_back(std::make_unique<T>());
			++owner.mDepth;
		}
		~Lease()
		{
			assert(mOwner.mDepth == mIndex + 1);
			--mOwner.mDepth;
		}
		Lease(const Lease&) = delete;
		Lease& operator=(const Lease&) = delete;
		T& Get() const { return *mOwner.mSlots[mIndex]; }
	private:
		NRIRetainedScratch& mOwner;
		size_t mIndex;
	};

	Lease Acquire() { return Lease(*this); }
	size_t SlotCount() const { return mSlots.size(); }
private:
	std::vector<std::unique_ptr<T>> mSlots;
	size_t mDepth = 0;
};
