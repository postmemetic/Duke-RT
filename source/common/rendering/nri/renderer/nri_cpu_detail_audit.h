#pragma once

#include "perf_audit.h"

// Optional CPU-detail observers are independent of production feature controls.
// Place these scopes around complete operations, never inside row/primitive loops.
class NRICpuDetailAuditScope
{
public:
	explicit NRICpuDetailAuditScope(PerfAuditMetric metric, bool enabled = true)
		: mMetric(metric), mStart(enabled && PerfAuditCpuDetailActive() ? PerfAuditClockNs() : 0) {}
	~NRICpuDetailAuditScope()
	{
		if (mStart != 0) PerfAuditAdd(mMetric, PerfAuditClockNs() - mStart);
	}
	NRICpuDetailAuditScope(const NRICpuDetailAuditScope&) = delete;
	NRICpuDetailAuditScope& operator=(const NRICpuDetailAuditScope&) = delete;
private:
	PerfAuditMetric mMetric;
	uint64_t mStart;
};
