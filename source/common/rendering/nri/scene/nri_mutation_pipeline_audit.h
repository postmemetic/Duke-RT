#pragma once

#include "perf_audit.h"

#include <array>
#include <cstddef>
#include <cstdint>

// Owner-thread diagnostic attribution only. Nested categories replace, rather
// than add to, their parent category. No clock/census work occurs without an
// audited mutation owner. Nested renderer invocations share the active ledger.
namespace nri_scene::mutation_audit
{
enum class Category : uint8_t
{
	Discovery, InputCapture, InputCheck, Structural, Material, ResidentCpu,
	Publication, ResourceCpu, Api, Wait, Temporal, Other, Count
};

struct Ledger
{
	std::array<uint64_t, (size_t)Category::Count> ns{};
	Category category = Category::Other;
	uint64_t last = 0, calls = 1, nestedCalls = 0, segments = 0;
	uint64_t analysisChanged = 0, analysisUnchanged = 0;
	uint64_t fluidSignatureAvoidedCalls = 0;
	void Advance(uint64_t now)
	{
		ns[(size_t)category] += now - last;
		last = now;
		++segments;
	}
};
inline thread_local Ledger* current = nullptr;

class Owner
{
	Ledger mLedger;
	bool mOwns = false;
public:
	Owner()
	{
		if (current != nullptr)
		{
			++current->calls;
			++current->nestedCalls;
			return;
		}
		if (!PerfAuditActive()) return;
		mOwns = true;
		mLedger.last = PerfAuditClockNs();
		current = &mLedger;
	}
	~Owner()
	{
		if (!mOwns) return;
		mLedger.Advance(PerfAuditClockNs());
		current = nullptr;
		const PerfAuditMetric metrics[] = {
			PerfAuditMetric::MutationPipelineDiscoveryNs, PerfAuditMetric::MutationPipelineInputCaptureNs,
			PerfAuditMetric::MutationPipelineInputCheckNs, PerfAuditMetric::MutationPipelineStructuralNs,
			PerfAuditMetric::MutationPipelineMaterialNs, PerfAuditMetric::MutationPipelineResidentCpuNs,
			PerfAuditMetric::MutationPipelinePublicationNs, PerfAuditMetric::MutationPipelineResourceCpuNs,
			PerfAuditMetric::MutationPipelineApiNs, PerfAuditMetric::MutationPipelineWaitNs,
			PerfAuditMetric::MutationPipelineTemporalNs, PerfAuditMetric::MutationPipelineOtherNs };
		uint64_t total = 0;
		for (size_t i = 0; i < mLedger.ns.size(); ++i)
		{
			total += mLedger.ns[i];
			PerfAuditAdd(metrics[i], mLedger.ns[i]);
		}
		PerfAuditAdd(PerfAuditMetric::MutationPipelineOwnerNs, total);
		PerfAuditAdd(PerfAuditMetric::MutationPipelineOwnerCalls, mLedger.calls);
		PerfAuditAdd(PerfAuditMetric::MutationPipelineNestedCalls, mLedger.nestedCalls);
		PerfAuditAdd(PerfAuditMetric::MutationPipelineSegments, mLedger.segments);
		PerfAuditAdd(PerfAuditMetric::FluidSignatureReuseAvoidedCalls, mLedger.fluidSignatureAvoidedCalls);
		PerfAuditAdd(PerfAuditMetric::MutationPipelineAnalysisChanged, mLedger.analysisChanged);
		PerfAuditAdd(PerfAuditMetric::MutationPipelineAnalysisUnchanged, mLedger.analysisUnchanged);
	}
	Owner(const Owner&) = delete;
	Owner& operator=(const Owner&) = delete;
};

class Scope
{
	Ledger* mLedger = nullptr;
	Category mPrevious = Category::Other;
public:
	explicit Scope(Category category) : mLedger(current)
	{
		if (mLedger == nullptr) return;
		mLedger->Advance(PerfAuditClockNs());
		mPrevious = mLedger->category;
		mLedger->category = category;
	}
	~Scope()
	{
		End();
	}
	void End()
	{
		if (mLedger == nullptr) return;
		mLedger->Advance(PerfAuditClockNs());
		mLedger->category = mPrevious;
		mLedger = nullptr;
	}
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
};

template<class F> decltype(auto) ApiCall(F&& call)
{
	Scope scope(Category::Api);
	return call();
}
}
