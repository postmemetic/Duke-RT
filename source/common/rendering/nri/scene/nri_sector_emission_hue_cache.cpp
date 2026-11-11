#include "nri_sector_emission_hue_cache.h"
#include "perf_audit.h"
#include "printf.h"

#include <cassert>
#include <cstring>

namespace nri_scene
{
namespace
{
	thread_local ScopedSectorEmissionHueCache* CurrentScope = nullptr;
	thread_local size_t BypassDepth = 0;

	bool EqualHue(const SectorEmissionHue& lhs, const SectorEmissionHue& rhs)
	{
		return std::memcmp(lhs.rgb, rhs.rgb, sizeof(lhs.rgb)) == 0 &&
			std::memcmp(&lhs.strength, &rhs.strength, sizeof(lhs.strength)) == 0 &&
			std::memcmp(&lhs.gain, &rhs.gain, sizeof(lhs.gain)) == 0 &&
			lhs.sectorIndex == rhs.sectorIndex && lhs.palette == rhs.palette && lhs.portalHops == rhs.portalHops;
	}
}

void SectorEmissionHueCache::Reset()
{
	std::vector<Entry>{}.swap(mEntries);
	NewEpoch();
}

uint64_t SectorEmissionHueCache::CapacityBytes() const
{
	return mEntries.capacity() * sizeof(Entry);
}

uint64_t SectorEmissionHueCache::NewEpoch()
{
	if (++mEpochSerial == 0)
	{
		for (auto& entry : mEntries) entry.epoch = 0;
		++mEpochSerial;
	}
	return mEpochSerial;
}

ScopedSectorEmissionHueCache::ScopedSectorEmissionHueCache(
	SectorEmissionHueCache& cache, size_t sectorCount, bool enabled, bool validate)
	: mCache(cache), mPrevious(CurrentScope), mEnabled(enabled), mValidate(validate), mCollectTiming(PerfAuditActive())
{
	if (mCollectTiming) mStartNs = PerfAuditClockNs();
	// Two keys per sector: explicit sector only versus local portal search.
	// A captured sector count is a bound only; out-of-range requests stay raw.
	if (enabled)
	{
		if (sectorCount <= mCache.mEntries.max_size() / 2)
			mCache.mEntries.resize(sectorCount * 2);
		else mEnabled = false;
	}
	mEpoch = mCache.NewEpoch();
	CurrentScope = this;
}

ScopedSectorEmissionHueCache::~ScopedSectorEmissionHueCache()
{
	assert(CurrentScope == this);
	CurrentScope = mPrevious;
	if (mPrevious != nullptr)
	{
		mPrevious->mEpoch = mPrevious->mCache.NewEpoch();
		++mPrevious->mStats.nestedInvalidations;
	}
	if (!PerfAuditActive()) return;
#define HUE_AUDIT(name, value) PerfAuditAdd(PerfAuditMetric::EmissiveSamplingHue##name, value)
	HUE_AUDIT(Requests, mStats.requests);
	HUE_AUDIT(RawResolutions, mStats.rawResolutions);
	HUE_AUDIT(Hits, mStats.hits);
	HUE_AUDIT(ValidationChecks, mStats.validationChecks);
	HUE_AUDIT(ValidationMismatches, mStats.validationMismatches);
	HUE_AUDIT(OperationQuarantines, mStats.operationQuarantines);
	HUE_AUDIT(NestedInvalidations, mStats.nestedInvalidations);
	HUE_AUDIT(ResolveNs, mStats.resolveNs);
	HUE_AUDIT(RawResolveNs, mStats.rawResolveNs);
	HUE_AUDIT(ScopeNs, mStartNs != 0 ? PerfAuditClockNs() - mStartNs : 0);
#undef HUE_AUDIT
	PerfAuditSet(PerfAuditMetric::EmissiveSamplingHueCapacityBytes, mCache.CapacityBytes());
}

SectorEmissionHue ScopedSectorEmissionHueCache::Resolve(int sectorIndex, bool neighbors, SectorEmissionHueResolver raw)
{
	const uint64_t start = mCollectTiming ? PerfAuditClockNs() : 0;
	++mStats.requests;
	const auto finish = [&](const SectorEmissionHue& value)
	{
		if (mCollectTiming) mStats.resolveNs += PerfAuditClockNs() - start;
		return value;
	};
	const auto resolveRaw = [&]()
	{
		const uint64_t rawStart = mCollectTiming ? PerfAuditClockNs() : 0;
		++mStats.rawResolutions;
		const auto value = raw(sectorIndex, neighbors);
		if (mCollectTiming) mStats.rawResolveNs += PerfAuditClockNs() - rawStart;
		return value;
	};
	const size_t index = sectorIndex >= 0 ? (size_t)sectorIndex * 2 + (neighbors ? 1u : 0u) : mCache.mEntries.size();
	if (!mEnabled || mQuarantined || BypassDepth != 0 || index >= mCache.mEntries.size())
		return finish(resolveRaw());
	if (mCache.mEntries[index].epoch == mEpoch)
	{
		++mStats.hits;
		const auto cached = mCache.mEntries[index].hue;
		if (mValidate)
		{
			++mStats.validationChecks;
			const auto epochBefore = mEpoch;
			ScopedSectorEmissionHueCacheBypass referenceBypass(true);
			const auto reference = resolveRaw();
			// A nested operation can invalidate the parent while resolving raw.
			if (mEpoch != epochBefore) return finish(reference);
			if (!EqualHue(cached, reference))
			{
				++mStats.validationMismatches;
				++mStats.operationQuarantines;
				mQuarantined = true;
				Printf("NRI PT emissive hue cache validation: mismatch=1 action=operation-quarantine sector=%d neighbors=%u\n", sectorIndex, neighbors ? 1u : 0u);
				return finish(reference);
			}
		}
		return finish(cached);
	}
	const auto epochBefore = mEpoch;
	const auto value = resolveRaw();
	// Recheck after raw resolution: a nested operation can resize the scratch.
	// Its returned value may predate nested mutation; never publish it under the
	// new parent epoch even though returning the raw result preserves semantics.
	if (mEpoch == epochBefore && index < mCache.mEntries.size())
	{
		mCache.mEntries[index].hue = value;
		mCache.mEntries[index].epoch = mEpoch;
	}
	return finish(value);
}

ScopedSectorEmissionHueCacheBypass::ScopedSectorEmissionHueCacheBypass(bool bypass) : mBypass(bypass)
{
	if (mBypass) ++BypassDepth;
}

ScopedSectorEmissionHueCacheBypass::~ScopedSectorEmissionHueCacheBypass()
{
	if (mBypass)
	{
		assert(BypassDepth != 0);
		--BypassDepth;
	}
}

SectorEmissionHue ResolveCachedSectorEmissionHue(int sectorIndex, bool neighbors, SectorEmissionHueResolver raw)
{
	return CurrentScope != nullptr ? CurrentScope->Resolve(sectorIndex, neighbors, raw) : raw(sectorIndex, neighbors);
}
bool IsSectorEmissionHueCacheBypassed() { return BypassDepth != 0; }
}
