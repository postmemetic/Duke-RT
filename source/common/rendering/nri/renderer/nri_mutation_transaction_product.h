#pragma once

#include <cassert>
#include <utility>

namespace nri_runtime_mutation
{
// One synchronous resident-apply invocation. Borrowed fallback storage stays
// untouched while preparation and staging can fail. Only the successful
// terminal publication may take its selected source. Never retain Get() across
// ReplaceOwned(): recovery can select a different object.
template<class T> class InvocationProduct
{
	T mOwned;
	T* mSelected = &mOwned;
public:
	InvocationProduct() = default;
	InvocationProduct(const InvocationProduct&) = delete;
	InvocationProduct& operator=(const InvocationProduct&) = delete;
	InvocationProduct(InvocationProduct&&) = delete;
	InvocationProduct& operator=(InvocationProduct&&) = delete;

	const T& Get() const
	{
		assert(mSelected != nullptr);
		return *mSelected;
	}
	void Borrow(T& source) { mSelected = &source; }
	T& BuildOwned()
	{
		mSelected = &mOwned;
		return mOwned;
	}
	void ReplaceOwned(T&& product)
	{
		mOwned = std::move(product);
		mSelected = &mOwned;
	}
	bool IsBorrowing(const T& source) const { return mSelected == &source; }
	T&& TakeForPublication()
	{
		assert(mSelected != nullptr);
		T* selected = mSelected;
		mSelected = nullptr;
		return std::move(*selected);
	}
};
}
