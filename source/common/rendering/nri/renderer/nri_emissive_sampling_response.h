#pragma once

#include "../scene/nri_sector_color_policy.h"
#include "perf_audit.h"

#include <cstdint>
#include <vector>

// Scalar results shared only between identification and construction in one
// synchronous update. Geometry, row membership and proposal history remain
// independent authorities; retained vector capacity is not a retained result.
struct NRIEmissiveSamplingResponse
{
	float samplingScale = 1.0f;
	float sectorRawScale = 1.0f;
	float sectorIntensityScale = 1.0f;
	float sectorReachScale = 1.0f;
	float referenceReachBound = 1.0f;
	float materialScale = 1.0f;
	nri_scene::SectorEmissionHue hue;
	bool sectorEligible = false;
	bool sectorApplied = false;
	bool materialEligible = false;
};

class NRIEmissiveSamplingResponseScope;
class NRIEmissiveSamplingResponseStorage
{
public:
	NRIEmissiveSamplingResponseStorage() = default;
	NRIEmissiveSamplingResponseStorage(const NRIEmissiveSamplingResponseStorage&) = delete;
	NRIEmissiveSamplingResponseStorage& operator=(const NRIEmissiveSamplingResponseStorage&) = delete;
	void Reset()
	{
		AdvanceEpoch();
		std::vector<NRIEmissiveSamplingResponse>{}.swap(mRows);
		// An existing lease still owns its scope exit. Reset only invalidates it.
	}
	uint64_t CapacityBytes() const { return mRows.capacity() * sizeof(NRIEmissiveSamplingResponse); }

private:
	friend class NRIEmissiveSamplingResponseScope;
	void AdvanceEpoch() { if (mEpoch != 0) ++mEpoch; }
	std::vector<NRIEmissiveSamplingResponse> mRows;
	uint64_t mEpoch = 1; // Wrap remains uncertified until owner destruction.
	bool mLeased = false;
};

class NRIEmissiveSamplingResponseScope
{
public:
	NRIEmissiveSamplingResponseScope(NRIEmissiveSamplingResponseStorage& storage, bool enabled)
		: mStorage(storage), mEnabled(enabled)
	{
		if (storage.mLeased)
		{
			// Even a disabled nested update invalidates its enclosing product.
			storage.AdvanceEpoch();
			mNestedFallbacks = enabled ? 1u : 0u;
			return;
		}
		if (!enabled) return;
		storage.AdvanceEpoch();
		if (storage.mEpoch == 0) return;
		mEpoch = storage.mEpoch;
		mAcquired = true;
		storage.mLeased = true;
		storage.mRows.clear();
	}
	NRIEmissiveSamplingResponseScope(const NRIEmissiveSamplingResponseScope&) = delete;
	NRIEmissiveSamplingResponseScope& operator=(const NRIEmissiveSamplingResponseScope&) = delete;
	~NRIEmissiveSamplingResponseScope()
	{
		if (mAcquired)
		{
			mStorage.mRows.clear();
			mStorage.mLeased = false;
		}
		if (!PerfAuditActive()) return;
#define RESPONSE_AUDIT(name, value) PerfAuditAdd(PerfAuditMetric::EmissiveResponseProduct##name, value)
		RESPONSE_AUDIT(Scopes, mAcquired);
		RESPONSE_AUDIT(PreparedRows, mPreparedRows);
		RESPONSE_AUDIT(ConsumedRows, mConsumedRows);
		RESPONSE_AUDIT(NestedFallbacks, mNestedFallbacks);
		RESPONSE_AUDIT(InvalidFallbacks, mInvalidFallbacks);
		RESPONSE_AUDIT(CapacityGrowths, mCapacityGrowths);
		RESPONSE_AUDIT(CapacityGrowthBytes, mCapacityGrowthBytes);
#undef RESPONSE_AUDIT
		PerfAuditSet(PerfAuditMetric::EmissiveResponseProductCapacityBytes, mStorage.CapacityBytes());
	}

	bool BeginRows(const void* owner, const void* source, size_t count)
	{
		mReady = false;
		if (!ValidLease()) return false;
		mOwner = owner;
		mSource = source;
		mCount = count;
		mStorage.mRows.clear();
		const size_t capacity = mStorage.mRows.capacity();
		mStorage.mRows.reserve(count);
		if (mStorage.mRows.capacity() > capacity)
		{
			++mCapacityGrowths;
			mCapacityGrowthBytes += (mStorage.mRows.capacity() - capacity) * sizeof(NRIEmissiveSamplingResponse);
		}
		return true;
	}
	void Append(const NRIEmissiveSamplingResponse& row)
	{
		if (!ValidLease() || mStorage.mRows.size() >= mCount) return;
		mStorage.mRows.push_back(row);
		++mPreparedRows;
	}
	void Complete()
	{
		mReady = ValidLease() && mStorage.mRows.size() == mCount;
	}
	const NRIEmissiveSamplingResponse* Rows(const void* owner, const void* source, size_t count)
	{
		if (!mEnabled) return nullptr;
		if (!mReady || !ValidLease() || owner != mOwner || source != mSource || count != mCount)
		{
			++mInvalidFallbacks;
			return nullptr;
		}
		return mStorage.mRows.data();
	}
	void RecordConsumed(uint64_t rows) { mConsumedRows += rows; }

private:
	bool ValidLease() const { return mAcquired && mEpoch != 0 && mEpoch == mStorage.mEpoch; }
	NRIEmissiveSamplingResponseStorage& mStorage;
	const void* mOwner = nullptr;
	const void* mSource = nullptr;
	size_t mCount = 0;
	uint64_t mEpoch = 0;
	uint64_t mPreparedRows = 0, mConsumedRows = 0;
	uint64_t mNestedFallbacks = 0, mInvalidFallbacks = 0;
	uint64_t mCapacityGrowths = 0, mCapacityGrowthBytes = 0;
	bool mEnabled = false, mAcquired = false, mReady = false;
};
