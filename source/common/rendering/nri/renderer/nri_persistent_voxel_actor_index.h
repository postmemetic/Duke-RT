#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Membership only: actors/resources and their authority remain with the batch.
// Slots survive vector reserve/append; a batch move/reset moves/resets this owner.
class NRIPersistentVoxelActorIndex
{
public:
	static constexpr uint32_t Missing = UINT32_MAX;

	uint32_t Find(uint64_t identity, uint32_t before = UINT32_MAX) const
	{
		const auto found = mSlots.find(identity);
		return found != mSlots.end() && found->second < before ? found->second : Missing;
	}

	void Appended(uint64_t identity, uint32_t slot)
	{
		// Last historical duplicate wins. Lookup's 'before' preserves the old
		// start-of-update map: a new identity stays absent during that update.
		mSlots[identity] = slot;
		assert(slot == mBindingPending.size());
		mBindingPending.push_back(0);
		Touched(slot);
	}

	void BeginUpdate()
	{
		mTouched = mActive;
	}

	void Touched(uint32_t slot)
	{
		assert(slot < mBindingPending.size());
		mTouched.push_back(slot);
		if (!mBindingPending[slot])
		{
			mBindingPending[slot] = 1;
			mBindings.push_back(slot);
			mBindingsSorted = false;
		}
	}

	template<class Actors> void FinishUpdate(const Actors& actors)
	{
		std::sort(mTouched.begin(), mTouched.end());
		mTouched.erase(std::unique(mTouched.begin(), mTouched.end()), mTouched.end());
		mActive.clear();
		for (uint32_t slot : mTouched)
			if (actors[slot].active) mActive.push_back(slot);
		mTouched.clear();
	}

	const std::vector<uint32_t>& Active() const { return mActive; }
	std::size_t PendingUpdateRows() const { return mTouched.size(); }
	const std::vector<uint32_t>& PendingMaterialBindings() const { return mBindings; }
	const std::unordered_map<uint64_t, uint32_t>& Slots() const { return mSlots; }
	const std::vector<uint32_t>& MaterialBindings()
	{
		if (!mBindingsSorted)
		{
			std::sort(mBindings.begin(), mBindings.end());
			mBindingsSorted = true;
		}
		return mBindings;
	}

	template<class Actors> void FinishMaterialRebind(const Actors& actors)
	{
		auto next = mBindings.begin();
		for (uint32_t slot : mBindings)
		{
			const auto& actor = actors[slot];
			if (actor.active || actor.materialOffset != 0 || actor.materialSlotGeneration != 0)
				*next++ = slot;
			else mBindingPending[slot] = 0;
		}
		mBindings.erase(next, mBindings.end());
	}

private:
	std::unordered_map<uint64_t, uint32_t> mSlots;
	std::vector<uint32_t> mActive;
	std::vector<uint32_t> mTouched;
	// Contains active rows plus inactive binding-clear obligations. It may keep
	// cleared rows until the next bridge rebuild, but never duplicates a slot.
	std::vector<uint32_t> mBindings;
	std::vector<uint8_t> mBindingPending;
	bool mBindingsSorted = true;
};
