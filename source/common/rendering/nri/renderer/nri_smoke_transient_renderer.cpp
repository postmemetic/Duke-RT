#include "nri_smoke.h"
#include "nri_renderer.h"
#include "printf.h"

#include <algorithm>
#include <cmath>

namespace
{
float Dot3(const float* a, const float* b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

NRISmokeTransientView BuildTransientView(const float* position, const float* forward,
	const float* right, const float* up, float tanHalfFovX, float tanHalfFovY, float farDepth)
{
	NRISmokeTransientView view;
	std::copy_n(position, 3, view.position);
	view.valid = std::isfinite(tanHalfFovX) && tanHalfFovX > 0.0f &&
		std::isfinite(tanHalfFovY) && tanHalfFovY > 0.0f &&
		std::isfinite(farDepth) && farDepth > 0.0f;
	// A stale/uninitialized or malformed camera must never suppress smoke.
	const float* basis[] = { forward, right, up };
	for (uint32_t axis = 0u; axis < 3u; ++axis)
	{
		view.valid = view.valid && std::isfinite(position[axis]);
		const float norm = Dot3(basis[axis], basis[axis]);
		view.valid = view.valid && std::isfinite(norm) && std::abs(norm - 1.0f) < 0.01f;
		for (uint32_t other = 0u; other < axis; ++other)
			view.valid = view.valid && std::abs(Dot3(basis[axis], basis[other])) < 0.01f;
	}
	if (!view.valid) return view;
	view.planeCount = 6u;
	for (uint32_t axis = 0u; axis < 3u; ++axis)
	{
		view.planes[0][axis] = forward[axis] * tanHalfFovX + right[axis];
		view.planes[1][axis] = forward[axis] * tanHalfFovX - right[axis];
		view.planes[2][axis] = forward[axis] * tanHalfFovY + up[axis];
		view.planes[3][axis] = forward[axis] * tanHalfFovY - up[axis];
		view.planes[4][axis] = forward[axis];
		view.planes[5][axis] = -forward[axis];
	}
	for (auto& plane : view.planes) plane[3] = -Dot3(plane, position);
	view.planes[5][3] += farDepth;
	return view;
}
}

bool NRISmokeSystem::PrepareTransientFrame(NRIRenderer& renderer, double gameplaySeconds)
{
	mTransientProfile = NRISmokeTransientClouds::ProfileForQuality(
		(uint32_t)mWorkScheduler.GetSnapshot().effectiveProfile);
	if (mTransientClouds.GetSnapshot().epoch != mStatus.simulationEpoch)
		mTransientResources.InvalidateCache();
	const auto view = BuildTransientView(renderer.mCurrentCameraPos, renderer.mCurrentCameraForward,
		renderer.mCurrentCameraRight, renderer.mCurrentCameraUp, renderer.mCurrentTanHalfFovX,
		renderer.mCurrentTanHalfFovY, mSettings.froxelMaxDistance);
	mTransientResidency.BeginFrame(gameplaySeconds, mStatus.simulationEpoch, mTransientProfile,
		view, mTransientClouds);
	const uint32_t lightingKey = mSettings.lightMode | (mSettings.pointLights ? 4u : 0u) |
		(mSettings.directionalLight ? 8u : 0u) | (mSettings.transientEmissiveLights ? 16u : 0u) |
		(mSettings.indirect ? 32u : 0u) | (mSettings.transientSelfShadow ? 64u : 0u);
	if (mTransientLightingPolicyKey != UINT32_MAX && lightingKey != mTransientLightingPolicyKey)
		mTransientClouds.InvalidateLighting();
	mTransientLightingPolicyKey = lightingKey;
	for (uint32_t index = 0u; index < mPendingTransientRequests.size();)
	{
		const uint32_t count = std::max(1u, std::min(mPendingTransientRequests[index].batchCount,
			uint32_t(mPendingTransientRequests.size()) - index));
		mTransientResidency.SubmitBatch(mPendingTransientRequests.data() + index, count);
		index += count;
	}
	mPendingTransientRequests.clear();
	mTransientResidency.Resolve(mTransientClouds);
	const auto services = BuildGridServices(renderer);
	if (!mTransientResources.Prepare(services, mResourceFroxelWidth, mResourceFroxelHeight, mResourceFroxelDepth))
		return false;
	if (mTransientResources.ConsumeCacheRecreated())
		mTransientClouds.InvalidateLighting();
	if (!mTransientResources.Upload(services, mTransientClouds.GetGpuGroups(), mTransientClouds.GetGpuLobes(), renderer.mResetHistory))
		return false;
	if (mSettings.traceMode != 0u) PrintTransientResidency();
	return true;
}

void NRISmokeSystem::PrintTransientResidency() const
{
	const auto& state = mTransientResidency.GetSnapshot();
	Printf("NRI PT smoke transient residency: epoch=%u history=%u hot=%u warm=%u dormant=%u "
		"hot_fire_sources=%u supported_fire_sources=%u fire_groups=%u/%u fire_lobes=%u/%u "
		"burst_groups=%u/%u burst_lobes=%u/%u deferred_hot_fire=%u deferred_hot_burst=%u "
		"deferred_birth_span_ms=%u hidden_resident=%u fresh_light_groups=%u admitted=%llu "
		"reentered=%llu released_offscreen=%llu reduced=%llu history_rejected=%llu history_evicted=%llu "
		"unsupported_load_frames=%llu oversized_fire_sources=%u budget_overage_groups=%u "
		"budget_overage_lobes=%u cpu_history_bytes=%llu coverage_filter=%u "
		"borrow_explosion_groups=%u/%u borrow_explosion_lobes=%u/%u deferred_borrow_explosion=%u "
		"protected_fire_groups=%u protected_fire_lobes=%u loaned_burst_groups=%u loaned_burst_lobes=%u "
		"compact_explosion_events=%llu first_presented_explosion_events=%llu maximum_explosion_first_age_ms=%u "
		"maximum_explosion_admission_delay_ms=%u "
		"policy=retained-age-frustum-hysteresis\n",
		state.epoch, state.historyGroups, state.hotGroups, state.warmGroups, state.dormantGroups,
		state.hotFireSources, state.supportedFireSources,
		state.residentFireGroups, state.fireGroupBudget, state.residentFireLobes, state.fireLobeBudget,
		state.residentBurstGroups, state.burstGroupBudget, state.residentBurstLobes, state.burstLobeBudget,
		state.hotFireDeferredGroups, state.hotBurstDeferredGroups, state.largestDeferredBirthSpanMilliseconds,
		state.hiddenResidentGroups, mTransientClouds.GetSnapshot().fullLightFreshRequestedThisFrame,
		(unsigned long long)state.admittedGroups, (unsigned long long)state.reenteredGroups,
		(unsigned long long)state.releasedGroups, (unsigned long long)state.reducedGroups,
		(unsigned long long)state.historyRejectedGroups, (unsigned long long)state.historyEvictedGroups,
		(unsigned long long)state.unsupportedLoadFrames, state.unsupportedFireSources,
		state.overBudgetResidentGroups, state.overBudgetResidentLobes,
		(unsigned long long)state.allocatedHistoryBytes, mSettings.transientCoverage ? 1u : 0u,
		state.residentBorrowExplosionGroups, state.borrowExplosionGroupBudget,
		state.residentBorrowExplosionLobes, state.borrowExplosionLobeBudget, state.deferredBorrowExplosionGroups,
		state.protectedFireGroups, state.protectedFireLobes, state.loanedBurstGroups, state.loanedBurstLobes,
		(unsigned long long)state.compactExplosionEvents, (unsigned long long)state.firstPresentedExplosionEvents,
		state.maximumExplosionFirstAgeMilliseconds, state.maximumExplosionAdmissionDelayMilliseconds);
}
