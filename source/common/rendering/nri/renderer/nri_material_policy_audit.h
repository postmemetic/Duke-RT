#pragma once

#include "perf_audit.h"

#include <cstdint>

// Five counters used in row/rule loops. Aggregate at the outermost synchronous
// material/emissive operation, so nested bulk/reference helpers count once and
// emit at most five atomic updates. No allocations or per-row timers.
enum class NRIMaterialPolicyAuditCounter : uint32_t
{
	LegacySettingsCaptures,
	EmissiveRuleTests,
	GlowRuleTests,
	GlowNameNormalizations,
	FluidRuleTests,
	Count
};

namespace nri_material_policy_audit
{
	inline constexpr PerfAuditMetric Metrics[] = {
		PerfAuditMetric::MaterialPolicyLegacySettingsCaptures,
		PerfAuditMetric::MaterialPolicyEmissiveRuleTests,
		PerfAuditMetric::MaterialPolicyGlowRuleTests,
		PerfAuditMetric::MaterialPolicyGlowNameNormalizations,
		PerfAuditMetric::MaterialPolicyFluidRuleTests
	};
	struct State
	{
		uint64_t values[(uint32_t)NRIMaterialPolicyAuditCounter::Count] = {};
		uint32_t depth = 0;
		uint32_t touched = 0;
		bool active = false;
	};
	inline thread_local State Current;
}

inline void NRIMaterialPolicyAuditAdd(NRIMaterialPolicyAuditCounter counter, uint64_t value = 1)
{
	auto& state = nri_material_policy_audit::Current;
	if (state.depth != 0)
	{
		if (state.active)
		{
			state.values[(uint32_t)counter] += value;
			state.touched |= 1u << (uint32_t)counter;
		}
		return;
	}
	// Small callers outside a bulk operation retain the ordinary audit behavior.
	PerfAuditAdd(nri_material_policy_audit::Metrics[(uint32_t)counter], value);
}

class NRIMaterialPolicyAuditScope
{
public:
	NRIMaterialPolicyAuditScope()
	{
		auto& state = nri_material_policy_audit::Current;
		if (state.depth++ == 0) state.active = PerfAuditActive();
	}
	~NRIMaterialPolicyAuditScope()
	{
		auto& state = nri_material_policy_audit::Current;
		if (--state.depth != 0 || !state.active) return;
		const uint32_t touched = state.touched;
		state.touched = 0;
		for (uint32_t i = 0; i < (uint32_t)NRIMaterialPolicyAuditCounter::Count; ++i)
		{
			const uint64_t value = state.values[i];
			state.values[i] = 0;
			if ((touched & (1u << i)) != 0) PerfAuditAdd(nri_material_policy_audit::Metrics[i], value);
		}
	}
	NRIMaterialPolicyAuditScope(const NRIMaterialPolicyAuditScope&) = delete;
	NRIMaterialPolicyAuditScope& operator=(const NRIMaterialPolicyAuditScope&) = delete;
};
