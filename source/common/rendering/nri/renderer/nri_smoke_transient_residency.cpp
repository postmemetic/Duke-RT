#include "nri_smoke_transient_residency.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace
{
bool IsFire(const NRISmokeTransientLobeRequest& request)
{
	return request.transientClass == NRISmokeTransientClass::FirePacket;
}

bool IsBorrowExplosion(const NRISmokeTransientLobeRequest& request)
{
	return request.burstBorrow && request.transientClass == NRISmokeTransientClass::Explosion;
}

float Length3(const float value[3])
{
	return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}
}

void NRISmokeTransientResidency::Reset(uint32_t epoch)
{
	mHistory.clear();
	mSnapshot = {};
	mSnapshot.epoch = epoch;
	mSnapshot.allocatedHistoryBytes = mHistory.capacity() * sizeof(Entry);
	mAdmissionOrdinal = 0u;
	mPrepared = false;
	mTime = 0.0;
	mLastExplosionAuthoredSeconds = -1.0e30;
}

void NRISmokeTransientResidency::BeginFrame(double gameplaySeconds, uint32_t epoch,
	const NRISmokeTransientProfile& profile, const NRISmokeTransientView& view,
	NRISmokeTransientClouds& clouds)
{
	if (epoch != mSnapshot.epoch || (mPrepared && std::isfinite(gameplaySeconds) &&
		gameplaySeconds < mTime))
	{
		Reset(epoch);
		clouds.Reset(epoch);
	}
	if (clouds.GetSnapshot().epoch != epoch) clouds.Reset(epoch);
	mPrepared = std::isfinite(gameplaySeconds);
	if (mPrepared) mTime = gameplaySeconds;
	mProfile = profile;
	mProfile.maximumActiveGroups = std::min(profile.maximumActiveGroups,
		NRISmokeTransientClouds::FixedGroupCapacity);
	mProfile.maximumActiveLobes = std::min(profile.maximumActiveLobes,
		NRISmokeTransientClouds::FixedLobeCapacity);
	mView = view;
	mView.valid = view.valid && view.planeCount > 0u && view.planeCount <= 6u;
	for (float position : mView.position) mView.valid = mView.valid && std::isfinite(position);
	mView.hotPadding = std::isfinite(view.hotPadding) ? std::max(0.0f, view.hotPadding) : 64.0f;
	mView.warmPadding = std::isfinite(view.warmPadding) ?
		std::max(mView.hotPadding, view.warmPadding) : 256.0f;
	mView.nearDistance = std::isfinite(view.nearDistance) ? std::max(0.0f, view.nearDistance) : 128.0f;
	for (uint32_t plane = 0u; mView.valid && plane < mView.planeCount; ++plane)
	{
		const float length = Length3(mView.planes[plane]);
		if (!std::isfinite(length) || length < 1.0e-6f ||
			!std::isfinite(mView.planes[plane][3])) { mView.valid = false; break; }
		for (float& component : mView.planes[plane]) component /= length;
	}
	// Medium/High retain four production fire streams (<=12 cohorts each),
	// with an independent burst share. Low retains the two-fire production pair.
	const bool low = mProfile.maximumActiveGroups <= 24u;
	mSnapshot.fireGroupBudget = std::min(mProfile.maximumActiveGroups,
		low ? 22u : 48u);
	mSnapshot.fireLobeBudget = std::min(mProfile.maximumActiveLobes,
		low ? 88u : 192u);
	mSnapshot.burstGroupBudget = mProfile.maximumActiveGroups - mSnapshot.fireGroupBudget;
	mSnapshot.burstLobeBudget = mProfile.maximumActiveLobes - mSnapshot.fireLobeBudget;
	mSnapshot.supportedFireSources = low ? 2u : 4u;
	clouds.BeginFrame(gameplaySeconds, mProfile.maximumActiveLobes, mProfile);
	if (!mPrepared) return;
	if (mHistory.capacity() < MaximumHistoryGroups) mHistory.reserve(MaximumHistoryGroups);
	for (Entry& entry : mHistory)
	{
		if (mTime - entry.requests[0].authoredGameplaySeconds >=
			entry.requests[0].groupLifetimeSeconds)
		{
			clouds.Release(entry.handle);
			++mSnapshot.expiredGroups;
		}
		else
		{
			if (!clouds.IsLive(entry.handle)) { entry.handle = {}; entry.residentLobes = 0u; }
			Classify(entry);
		}
	}
	// Compact a synchronized burst expiry once, not one multi-kilobyte vector
	// shift per descriptor. History maintenance stays linear in the fixed bound.
	mHistory.erase(std::remove_if(mHistory.begin(), mHistory.end(), [this](const Entry& entry)
	{
		return mTime - entry.requests[0].authoredGameplaySeconds >= entry.requests[0].groupLifetimeSeconds;
	}), mHistory.end());
}

void NRISmokeTransientResidency::BuildBounds(Entry& entry) const
{
	for (uint32_t axis = 0u; axis < 3u; ++axis)
	{
		entry.boundsMin[axis] = std::numeric_limits<float>::max();
		entry.boundsMax[axis] = -std::numeric_limits<float>::max();
	}
	for (uint32_t index = 0u; index < entry.count; ++index)
	{
		const auto& request = entry.requests[index];
		// Radius is monotonic for positive radiusExponent; endpoints bound either
		// growth or contraction. A reduced continuous cohort may re-time this
		// representative earlier and extend its local lifetime to the group end.
		// Bound that possible representation as well as the original request.
		const float span = std::max(request.lifetimeSeconds, request.groupLifetimeSeconds);
		const float radius = std::max({ request.initialRadius,
			request.initialRadius + request.expansionVelocity * span,
			0.001f });
		const float extent = radius + (request.shape == 1u ?
			Length3(request.halfAxisU) + Length3(request.halfAxisV) : 0.0f);
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			const float start = request.position[axis];
			const float end = start + request.velocity[axis] * span;
			entry.boundsMin[axis] = std::min(entry.boundsMin[axis], std::min(start, end) - extent);
			entry.boundsMax[axis] = std::max(entry.boundsMax[axis], std::max(start, end) + extent);
		}
	}
}

bool NRISmokeTransientResidency::Intersects(const Entry& entry, float padding) const
{
	for (uint32_t plane = 0u; plane < mView.planeCount; ++plane)
	{
		const float* p = mView.planes[plane];
		float furthest = p[3];
		for (uint32_t axis = 0u; axis < 3u; ++axis)
			furthest += p[axis] * (p[axis] >= 0.0f ? entry.boundsMax[axis] : entry.boundsMin[axis]);
		if (furthest < -padding) return false;
	}
	return true;
}

void NRISmokeTransientResidency::Classify(Entry& entry) const
{
	float nearSquared = 0.0f;
	bool finite = true;
	for (uint32_t axis = 0u; axis < 3u; ++axis)
	{
		finite = finite && std::isfinite(entry.boundsMin[axis]) && std::isfinite(entry.boundsMax[axis]);
		const float distance = mView.position[axis] -
			std::clamp(mView.position[axis], entry.boundsMin[axis], entry.boundsMax[axis]);
		nearSquared += distance * distance;
	}
	if (!mView.valid || !finite || nearSquared <= mView.nearDistance * mView.nearDistance ||
		Intersects(entry, mView.hotPadding))
	{
		entry.interest = NRISmokeTransientInterest::Hot;
		entry.lastHotSeconds = mTime;
	}
	else if (Intersects(entry, mView.warmPadding) || mTime - entry.lastHotSeconds < 0.75)
		entry.interest = NRISmokeTransientInterest::Warm;
	else entry.interest = NRISmokeTransientInterest::Dormant;
}

bool NRISmokeTransientResidency::SubmitBatch(
	const NRISmokeTransientLobeRequest* requests, uint32_t count)
{
	++mSnapshot.submittedGroups;
	if (!mPrepared || !mProfile.enabled || count >
		NRISmokeTransientClouds::FixedMaximumLobesPerGroup ||
		!NRISmokeTransientClouds::ValidateBatch(requests, count))
	{
		++mSnapshot.invalidGroups;
		return false;
	}
	const auto& first = requests[0];
	const double age = mTime - first.authoredGameplaySeconds;
	if (first.epoch != mSnapshot.epoch || age >= first.groupLifetimeSeconds ||
		(first.maximumLatencySeconds > 0.0f && age > first.maximumLatencySeconds))
	{
		++mSnapshot.staleGroups;
		return false;
	}
	for (const Entry& entry : mHistory)
	{
		const auto& previous = entry.requests[0];
		if (previous.sourceId == first.sourceId && previous.epoch == first.epoch &&
			previous.sourceEventSerial == first.sourceEventSerial &&
			previous.authoredGameplaySeconds == first.authoredGameplaySeconds &&
			previous.transientClass == first.transientClass)
		{
			++mSnapshot.duplicateGroups;
			return true;
		}
	}
	Entry entry = {};
	entry.count = count;
	entry.submittedGameplaySeconds = mTime;
	std::copy_n(requests, count, entry.requests.begin());
	BuildBounds(entry);
	Classify(entry);
	const uint32_t burstHistory = static_cast<uint32_t>(std::count_if(mHistory.begin(),
		mHistory.end(), [](const Entry& value) { return !IsFire(value.requests[0]); }));
	const bool burstPartitionFull = !IsFire(first) &&
		burstHistory >= MaximumHistoryGroups - ReservedFireHistoryGroups;
	if (mHistory.size() >= MaximumHistoryGroups || burstPartitionFull)
	{
		// Never sacrifice a resident event (especially onscreen smoke) to store a
		// new descriptor. Under pressure only proven-dormant nonresident history
		// may be replaced; burst traffic cannot consume the Fire history reserve.
		auto victim = mHistory.end();
		for (auto it = mHistory.begin(); it != mHistory.end(); ++it)
		{
			if (it->handle.slot != UINT32_MAX || it->interest != NRISmokeTransientInterest::Dormant ||
				(burstPartitionFull && IsFire(it->requests[0]))) continue;
			if (victim == mHistory.end() || it->requests[0].authoredGameplaySeconds <
				victim->requests[0].authoredGameplaySeconds) victim = it;
		}
		if (victim == mHistory.end()) { ++mSnapshot.historyRejectedGroups; return false; }
		*victim = entry;
		++mSnapshot.historyEvictedGroups;
	}
	else mHistory.push_back(entry);
	return true;
}

void NRISmokeTransientResidency::StampExplosionEpisodes()
{
	std::vector<size_t> pending;
	for (size_t index = 0u; index < mHistory.size(); ++index)
		if (IsBorrowExplosion(mHistory[index].requests[0]) && mHistory[index].borrowDetail == 0u)
			pending.push_back(index);
	std::sort(pending.begin(), pending.end(), [this](size_t a, size_t b)
	{
		const auto& ra = mHistory[a].requests[0];
		const auto& rb = mHistory[b].requests[0];
		if (ra.authoredGameplaySeconds != rb.authoredGameplaySeconds)
			return ra.authoredGameplaySeconds < rb.authoredGameplaySeconds;
		if (ra.sourceId != rb.sourceId) return ra.sourceId < rb.sourceId;
		return ra.sourceEventSerial < rb.sourceEventSerial;
	});
	for (size_t index : pending)
	{
		Entry& entry = mHistory[index];
		const double authored = entry.requests[0].authoredGameplaySeconds;
		// Authored time, never render cadence or view state, defines an episode.
		// Its first explosion keeps the original art; rapid followers trade
		// decorative lobes for timely event coverage, preserving optical amount.
		const bool follower = authored - mLastExplosionAuthoredSeconds <= 0.35;
		entry.borrowDetail = follower ? std::min(entry.count, 3u) : entry.count;
		if (entry.borrowDetail < entry.count) ++mSnapshot.compactExplosionEvents;
		mLastExplosionAuthoredSeconds = std::max(mLastExplosionAuthoredSeconds, authored);
	}
}

void NRISmokeTransientResidency::Resolve(NRISmokeTransientClouds& clouds)
{
	if (!mPrepared) return;
	StampExplosionEpisodes();
	struct Source
	{
		uint64_t lastAdmission = 0u;
		uint32_t residentGroups = 0u;
		uint32_t hotCohorts = 0u;
		bool hot = false;
	};
	std::map<uint64_t, Source> sources;
	auto sourceKey = [](const Entry& entry)
	{
		return (static_cast<uint64_t>(IsFire(entry.requests[0])) << 32u) |
			entry.requests[0].sourceId;
	};
	struct Occupancy { uint32_t groups = 0u; uint32_t lobes = 0u; };
	Occupancy fireUse, ordinaryUse, explosionUse;
	auto usage = [&](const Entry& entry) -> Occupancy&
	{
		return IsFire(entry.requests[0]) ? fireUse :
			(IsBorrowExplosion(entry.requests[0]) ? explosionUse : ordinaryUse);
	};
	for (Entry& entry : mHistory)
	{
		if (entry.interest == NRISmokeTransientInterest::Dormant || !mProfile.enabled)
		{
			if (clouds.Release(entry.handle)) ++mSnapshot.releasedGroups;
			entry.handle = {};
			entry.residentLobes = 0u;
		}
		auto& source = sources[sourceKey(entry)];
		source.lastAdmission = std::max(source.lastAdmission, entry.admissionOrdinal);
		source.hot = source.hot || entry.interest == NRISmokeTransientInterest::Hot;
		if (entry.interest == NRISmokeTransientInterest::Hot) ++source.hotCohorts;
		if (!clouds.IsLive(entry.handle)) continue;
		clouds.SetInterest(entry.handle, entry.interest);
		++source.residentGroups;
		auto& occupied = usage(entry);
		++occupied.groups; occupied.lobes += entry.residentLobes;
	}
	const uint32_t hotFireSources = static_cast<uint32_t>(std::count_if(sources.begin(), sources.end(),
		[](const auto& item) { return (item.first >> 32u) != 0u && item.second.hot; }));
	mSnapshot.unsupportedFireSources = static_cast<uint32_t>(std::count_if(sources.begin(), sources.end(),
		[](const auto& item) { return (item.first >> 32u) != 0u &&
			item.second.hotCohorts > MaximumFireCohortsPerSource; }));
	if (hotFireSources > mSnapshot.supportedFireSources || mSnapshot.unsupportedFireSources > 0u)
		++mSnapshot.unsupportedLoadFrames;
	// Count Warm residents as well: rotating between two Hot pairs must not
	// accumulate three separate five-lobe source histories before all four
	// become visible. Their retained detail still consumes the protected share.
	const uint32_t detailedFireSources = static_cast<uint32_t>(std::count_if(sources.begin(), sources.end(),
		[](const auto& item) { return (item.first >> 32u) != 0u &&
			(item.second.hot || item.second.residentGroups > 0u); }));
	const uint32_t fireDetail = mSnapshot.supportedFireSources == 2u ? 4u :
		(detailedFireSources <= 2u ? 5u : 3u);
	const bool allowLoans = mProfile.maximumActiveGroups >= 64u &&
		mProfile.maximumActiveLobes >= 256u;
	// With no detailed Fire sources, preserve one complete future window and
	// lend the actual remainder. Engine birth batching can exceed the ideal
	// 38-event cadence estimate. Once any Fire is relevant/resident, keep the
	// two-source floor plus all established three/four-source windows.
	const uint32_t protectedSources = detailedFireSources == 0u ? 1u :
		std::clamp(detailedFireSources, 2u, 4u);
	mSnapshot.protectedFireGroups = allowLoans ?
		std::max(fireUse.groups, protectedSources * MaximumFireCohortsPerSource) : mSnapshot.fireGroupBudget;
	mSnapshot.protectedFireLobes = allowLoans ? std::max(fireUse.lobes,
		protectedSources <= 2u ? protectedSources * 60u :
			120u + (protectedSources - 2u) * 36u) : mSnapshot.fireLobeBudget;
	auto remaining = [](uint32_t capacity, uint32_t used) { return capacity > used ? capacity - used : 0u; };
	auto budget = [&](const Entry& entry) -> Occupancy
	{
		if (IsFire(entry.requests[0])) return { mSnapshot.fireGroupBudget, mSnapshot.fireLobeBudget };
		if (IsBorrowExplosion(entry.requests[0]) && allowLoans)
			return { remaining(mProfile.maximumActiveGroups,
				mSnapshot.protectedFireGroups + std::max(ordinaryUse.groups, 2u)),
				remaining(mProfile.maximumActiveLobes,
				mSnapshot.protectedFireLobes + std::max(ordinaryUse.lobes, 8u)) };
		// Non-opted effects never borrow. A current loan cannot be silently
		// double-promised to future Fire or to another ordinary burst either.
		if (allowLoans && explosionUse.groups > 0u)
			return { std::min(mSnapshot.burstGroupBudget, remaining(mProfile.maximumActiveGroups,
				mSnapshot.protectedFireGroups + explosionUse.groups)),
				std::min(mSnapshot.burstLobeBudget, remaining(mProfile.maximumActiveLobes,
				mSnapshot.protectedFireLobes + explosionUse.lobes)) };
		const Occupancy& other = IsBorrowExplosion(entry.requests[0]) ? ordinaryUse : explosionUse;
		return { remaining(mSnapshot.burstGroupBudget, other.groups),
			remaining(mSnapshot.burstLobeBudget, other.lobes) };
	};
	auto newFireWindowBlocked = [&](const Entry& entry)
	{
		if (!allowLoans || explosionUse.groups == 0u || !IsFire(entry.requests[0]) ||
			sources[sourceKey(entry)].residentGroups > 0u) return false;
		const uint32_t nextSources = 1u + static_cast<uint32_t>(std::count_if(sources.begin(), sources.end(),
			[](const auto& item) { return (item.first >> 32u) != 0u && item.second.residentGroups > 0u; }));
		const uint32_t futureLobes = nextSources <= 2u ? nextSources * 60u :
			120u + (nextSources - 2u) * 36u;
		// A late extra source cannot occupy a few apparently-free slots that are
		// already promised to established Fire's next cadence. Admit its first
		// cohort only once its entire supported age window also fits naturally.
		return nextSources > 4u || nextSources * MaximumFireCohortsPerSource >
			remaining(mProfile.maximumActiveGroups, explosionUse.groups + ordinaryUse.groups) ||
			futureLobes > remaining(mProfile.maximumActiveLobes, explosionUse.lobes + ordinaryUse.lobes);
	};
	// Each pass admits at most one head cohort from each source before that
	// source's ordinal advances. Equal-time ties are stable source identities,
	// never actor traversal order. Existing visible groups are never rewritten.
	for (uint32_t tier = 0u; tier < 2u; ++tier)
	{
		std::vector<size_t> candidates;
		for (size_t index = 0u; index < mHistory.size(); ++index)
			if (mHistory[index].handle.slot == UINT32_MAX &&
				static_cast<uint32_t>(mHistory[index].interest) == tier) candidates.push_back(index);
		while (!candidates.empty() && mProfile.enabled)
		{
			bool warmFire = false, warmBurst = false;
			if (tier == 0u)
				for (const Entry& entry : mHistory)
					if (entry.interest == NRISmokeTransientInterest::Warm && clouds.IsLive(entry.handle))
						(IsFire(entry.requests[0]) ? warmFire : warmBurst) = true;
			// A full family or source is pruned in one bounded walk, rather than
			// repeatedly finding/removing individual heads of a thousand-entry
			// deferred queue. The remaining arbitration is bounded by pool slots.
			candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](size_t index)
			{
				const Entry& entry = mHistory[index];
				const auto occupied = usage(entry);
				const auto available = budget(entry);
				const bool full = occupied.groups >= available.groups || occupied.lobes >= available.lobes;
				return IsFire(entry.requests[0]) ? (full && !warmFire) ||
					sources[sourceKey(entry)].residentGroups >= MaximumFireCohortsPerSource ||
					newFireWindowBlocked(entry) : full && !warmBurst;
			}), candidates.end());
			if (candidates.empty()) break;
			auto best = std::min_element(candidates.begin(), candidates.end(), [&](size_t a, size_t b)
			{
				const Entry& ea = mHistory[a]; const Entry& eb = mHistory[b];
				const bool fa = IsFire(ea.requests[0]), fb = IsFire(eb.requests[0]);
				if (fa != fb) return fa;
				const uint64_t ka = sourceKey(ea), kb = sourceKey(eb);
				if (sources[ka].lastAdmission != sources[kb].lastAdmission)
					return sources[ka].lastAdmission < sources[kb].lastAdmission;
				if (ka != kb) return ka < kb;
				return ea.requests[0].authoredGameplaySeconds < eb.requests[0].authoredGameplaySeconds;
			});
			Entry& entry = mHistory[*best];
			candidates.erase(best);
			const bool fire = IsFire(entry.requests[0]);
			auto& occupied = usage(entry);
			auto available = budget(entry);
			auto& source = sources[sourceKey(entry)];
			if (fire && source.residentGroups >= MaximumFireCohortsPerSource) continue;
			const uint32_t desired = std::min(entry.count, tier == 1u ? 2u :
				(fire ? fireDetail : (IsBorrowExplosion(entry.requests[0]) ?
					entry.borrowDetail : mProfile.maximumLobesPerGroup)));
			if (tier == 0u && (occupied.groups >= available.groups || occupied.lobes + desired > available.lobes))
			{
				// Known-outside padded view may yield its detail to an actual Hot
				// cohort. History is retained, so this cannot freeze/replay the source.
				for (Entry& warm : mHistory)
				{
					if (occupied.groups < available.groups && occupied.lobes + desired <= available.lobes) break;
					if (IsFire(warm.requests[0]) != fire || warm.interest !=
						NRISmokeTransientInterest::Warm || !clouds.IsLive(warm.handle)) continue;
					clouds.Release(warm.handle);
					++mSnapshot.releasedGroups;
					auto& warmUse = usage(warm);
					--warmUse.groups; warmUse.lobes -= warm.residentLobes;
					--sources[sourceKey(warm)].residentGroups;
					warm.handle = {}; warm.residentLobes = 0u;
					available = budget(entry);
				}
			}
			if (occupied.groups >= available.groups || occupied.lobes >= available.lobes) continue;
			const uint32_t detail = std::min(desired, available.lobes - occupied.lobes);
			const auto admission = clouds.AdmitRetainedBatch(entry.requests.data(), entry.count, detail);
			if (!admission.Accepted()) continue;
			if (entry.admissionOrdinal == 0u && IsBorrowExplosion(entry.requests[0]))
				mSnapshot.maximumExplosionAdmissionDelayMilliseconds = std::max(
					mSnapshot.maximumExplosionAdmissionDelayMilliseconds, static_cast<uint32_t>(std::clamp(
					(mTime - entry.submittedGameplaySeconds) * 1000.0, 0.0, static_cast<double>(UINT32_MAX))));
			if (entry.admissionOrdinal != 0u) ++mSnapshot.reenteredGroups;
			entry.handle = admission.handle;
			entry.residentLobes = admission.admittedLobes;
			entry.admissionOrdinal = ++mAdmissionOrdinal;
			source.lastAdmission = entry.admissionOrdinal;
			++source.residentGroups;
			++occupied.groups; occupied.lobes += entry.residentLobes;
			++mSnapshot.admittedGroups;
			if (entry.residentLobes < entry.count) ++mSnapshot.reducedGroups;
			clouds.SetInterest(entry.handle, entry.interest);
		}
	}
	Entry explosionBudgetProbe;
	explosionBudgetProbe.requests[0].transientClass = NRISmokeTransientClass::Explosion;
	explosionBudgetProbe.requests[0].burstBorrow = true;
	const auto explosionBudget = budget(explosionBudgetProbe);
	mSnapshot.borrowExplosionGroupBudget = explosionBudget.groups;
	mSnapshot.borrowExplosionLobeBudget = explosionBudget.lobes;
	RefreshSnapshot(clouds);
}

void NRISmokeTransientResidency::RefreshSnapshot(const NRISmokeTransientClouds& clouds)
{
	mSnapshot.historyGroups = static_cast<uint32_t>(mHistory.size());
	mSnapshot.allocatedHistoryBytes = mHistory.capacity() * sizeof(Entry);
	mSnapshot.hotGroups = mSnapshot.warmGroups = mSnapshot.dormantGroups = 0u;
	mSnapshot.residentFireGroups = mSnapshot.residentFireLobes = 0u;
	mSnapshot.residentBurstGroups = mSnapshot.residentBurstLobes = 0u;
	mSnapshot.hotFireDeferredGroups = mSnapshot.hotBurstDeferredGroups = 0u;
	mSnapshot.residentBorrowExplosionGroups = mSnapshot.residentBorrowExplosionLobes = 0u;
	mSnapshot.deferredBorrowExplosionGroups = 0u;
	mSnapshot.hiddenResidentGroups = mSnapshot.firstVisibleGroups = 0u;
	mSnapshot.largestDeferredBirthSpanMilliseconds = 0u;
	std::map<uint32_t, bool> hotFireSources;
	for (Entry& entry : mHistory)
	{
		const bool fire = IsFire(entry.requests[0]);
		if (entry.interest == NRISmokeTransientInterest::Hot)
		{
			++mSnapshot.hotGroups;
			if (fire) hotFireSources[entry.requests[0].sourceId] = true;
		}
		else if (entry.interest == NRISmokeTransientInterest::Warm) ++mSnapshot.warmGroups;
		else ++mSnapshot.dormantGroups;
		if (clouds.IsLive(entry.handle))
		{
			if (fire) { ++mSnapshot.residentFireGroups; mSnapshot.residentFireLobes += entry.residentLobes; }
			else { ++mSnapshot.residentBurstGroups; mSnapshot.residentBurstLobes += entry.residentLobes; }
			if (IsBorrowExplosion(entry.requests[0]))
			{
				++mSnapshot.residentBorrowExplosionGroups;
				mSnapshot.residentBorrowExplosionLobes += entry.residentLobes;
				if (!entry.firstPresented && entry.interest == NRISmokeTransientInterest::Hot &&
					clouds.GetGpuGroups()[entry.handle.slot].lobeCount > 0u)
				{
					entry.firstPresented = true;
					++mSnapshot.firstPresentedExplosionEvents;
					mSnapshot.maximumExplosionFirstAgeMilliseconds = std::max(
						mSnapshot.maximumExplosionFirstAgeMilliseconds, static_cast<uint32_t>(std::clamp(
						(mTime - entry.requests[0].authoredGameplaySeconds) * 1000.0, 0.0,
						static_cast<double>(UINT32_MAX))));
				}
			}
			if (entry.interest != NRISmokeTransientInterest::Hot) ++mSnapshot.hiddenResidentGroups;
		}
		else if (entry.interest == NRISmokeTransientInterest::Hot)
		{
			if (fire)
			{
				++mSnapshot.hotFireDeferredGroups;
				const double span = entry.count > 1u ? entry.requests[entry.count - 1u].lobeDelaySeconds *
					static_cast<double>(entry.count) / static_cast<double>(entry.count - 1u) : 0.0;
				mSnapshot.largestDeferredBirthSpanMilliseconds = std::max(
					mSnapshot.largestDeferredBirthSpanMilliseconds,
					static_cast<uint32_t>(std::min(span * 1000.0, static_cast<double>(UINT32_MAX))));
			}
			else
			{
				++mSnapshot.hotBurstDeferredGroups;
				if (IsBorrowExplosion(entry.requests[0])) ++mSnapshot.deferredBorrowExplosionGroups;
			}
		}
	}
	mSnapshot.hotFireSources = static_cast<uint32_t>(hotFireSources.size());
	// A profile down-switch never hard-evicts live visible puffs. Explicitly
	// report its temporary grandfathered overage until natural expiry, rather
	// than silently claiming the new smaller budget is already enforced.
	auto excess = [](uint32_t active, uint32_t budget) { return active > budget ? active - budget : 0u; };
	mSnapshot.loanedBurstGroups = excess(mSnapshot.residentBurstGroups, mSnapshot.burstGroupBudget);
	mSnapshot.loanedBurstLobes = excess(mSnapshot.residentBurstLobes, mSnapshot.burstLobeBudget);
	mSnapshot.overBudgetResidentGroups = std::max(excess(mSnapshot.residentFireGroups, mSnapshot.fireGroupBudget),
		excess(mSnapshot.residentFireGroups + mSnapshot.residentBurstGroups, mProfile.maximumActiveGroups));
	mSnapshot.overBudgetResidentLobes = std::max(excess(mSnapshot.residentFireLobes, mSnapshot.fireLobeBudget),
		excess(mSnapshot.residentFireLobes + mSnapshot.residentBurstLobes, mProfile.maximumActiveLobes));
	mSnapshot.firstVisibleGroups = clouds.GetSnapshot().fullLightFreshRequestedThisFrame;
}

bool NRISmokeTransientResidency::RetireLatest(uint64_t replacementKey,
	NRISmokeTransientClouds& clouds)
{
	if (replacementKey == 0u) return false;
	bool retired = false;
	for (auto it = mHistory.begin(); it != mHistory.end();)
	{
		if (it->requests[0].replacementKey == replacementKey)
		{
			clouds.Release(it->handle);
			it = mHistory.erase(it);
			retired = true;
		}
		else ++it;
	}
	RefreshSnapshot(clouds);
	return retired;
}
