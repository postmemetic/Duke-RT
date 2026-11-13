#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Dependency keys live only for one refresh. Small lists use bounded stack
// storage; unusual topology spills to an ordinary vector without truncation.
// Nothing is shared between calls, so nested refreshes cannot alias scratch.
class NRIRuntimeMutationDependencyScratch
{
public:
	bool NeedsHeapGrowth() const
	{
		return mSize >= InlineCount && (!mSpilled || mSize == mOverflow.capacity());
	}
	void push_back(uint64_t key)
	{
		if (!mSpilled && mSize < InlineCount) mInline[mSize] = key;
		else
		{
			if (!mSpilled)
			{
				mOverflow.reserve(InlineCount * 2);
				mOverflow.assign(mInline.begin(), mInline.end());
				mSpilled = true;
			}
			mOverflow.push_back(key);
		}
		++mSize;
	}
	void SortUnique()
	{
		auto* first = mSpilled ? mOverflow.data() : mInline.data();
		std::sort(first, first + mSize);
		mSize = std::unique(first, first + mSize) - first;
		if (mSpilled) mOverflow.resize(mSize);
	}
	bool Equals(const std::vector<uint64_t>& other) const
	{
		const auto* first = mSpilled ? mOverflow.data() : mInline.data();
		return other.size() == mSize && std::equal(first, first + mSize, other.begin());
	}
	void Store(std::vector<uint64_t>& destination)
	{
		if (mSpilled) destination = std::move(mOverflow);
		else destination.assign(mInline.data(), mInline.data() + mSize);
	}
	size_t size() const { return mSize; }
	size_t HeapCapacity() const { return mOverflow.capacity(); }

private:
	static constexpr size_t InlineCount = 64;
	std::array<uint64_t, InlineCount> mInline;
	std::vector<uint64_t> mOverflow;
	size_t mSize = 0;
	bool mSpilled = false;
};
