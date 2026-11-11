#pragma once

#include "perf_audit.h"
#include "nri_texture_signature.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

// Supplied by the core audit owner. Separate from the existing production audit
// controls so the new observer can be measured off/on on the same executable.
bool PerfAuditCpuDetailActive();

namespace nri_scene::material_color_audit
{
enum class Counter : uint32_t
{
	Operations,
	Requests,
	RecursiveCalls,
	AverageGuards,
	UploadGuards,
	AverageSignatures,
	UploadSignatures,
	PersistentHits,
	FrameHits,
	Computations,
	Successes,
	Failures,
	EligibleRequests,
	EligibleRepeats,
	ObserverCollisions,
	TextureLookupRequests,
	TextureLookupEntries,
	TextureLookupBucketGrowths,
	MaterialVectorGrowths,
	MetadataVectorGrowths,
	TextureVectorGrowths,
	CapacityGrowthBytes,
	Count
};
inline constexpr PerfAuditMetric Metrics[] = {
	PerfAuditMetric::MaterialColorOperations,
	PerfAuditMetric::MaterialColorRequests,
	PerfAuditMetric::MaterialColorRecursiveCalls,
	PerfAuditMetric::MaterialColorAverageGuards,
	PerfAuditMetric::MaterialColorUploadGuards,
	PerfAuditMetric::MaterialColorAverageSignatures,
	PerfAuditMetric::MaterialColorUploadSignatures,
	PerfAuditMetric::MaterialColorPersistentHits,
	PerfAuditMetric::MaterialColorFrameHits,
	PerfAuditMetric::MaterialColorComputations,
	PerfAuditMetric::MaterialColorSuccesses,
	PerfAuditMetric::MaterialColorFailures,
	PerfAuditMetric::MaterialColorEligibleRequests,
	PerfAuditMetric::MaterialColorEligibleRepeats,
	PerfAuditMetric::MaterialColorObserverCollisions,
	PerfAuditMetric::MaterialColorTextureLookupRequests,
	PerfAuditMetric::MaterialColorTextureLookupEntries,
	PerfAuditMetric::MaterialColorTextureLookupBucketGrowths,
	PerfAuditMetric::MaterialColorMaterialVectorGrowths,
	PerfAuditMetric::MaterialColorMetadataVectorGrowths,
	PerfAuditMetric::MaterialColorTextureVectorGrowths,
	PerfAuditMetric::MaterialColorCapacityGrowthBytes,
};
struct State
{
	uint64_t values[(uint32_t)Counter::Count];
	const void* eligible[64];
	uint32_t averageDepth = 0;
};
inline thread_local State* Current = nullptr;
inline void Add(Counter counter, uint64_t value = 1)
{
	if (Current != nullptr) Current->values[(uint32_t)counter] += value;
}
inline void Guard()
{
	if (Current != nullptr) Add(Current->averageDepth != 0 ? Counter::AverageGuards : Counter::UploadGuards);
}
inline void Signature()
{
	if (Current != nullptr) Add(Current->averageDepth != 0 ? Counter::AverageSignatures : Counter::UploadSignatures);
}
inline void Eligible(FGameTexture* texture, const TextureSignature& signature)
{
	if (Current == nullptr || !signature.valid || !signature.persistentEligible || !signature.metadataDerived ||
		signature.sourceKind != TextureSignatureSourceKind::ImageBacked) return;
	Add(Counter::EligibleRequests);
	// A fixed direct-mapped observer: repeats are a proven lower bound. A
	// collision only replaces observer history, never changes renderer output.
	const uintptr_t bits = (uintptr_t)texture;
	const size_t slot = ((bits >> 4) ^ (bits >> 11)) & 63u;
	if (Current->eligible[slot] == texture) Add(Counter::EligibleRepeats);
	else
	{
		if (Current->eligible[slot] != nullptr) Add(Counter::ObserverCollisions);
		Current->eligible[slot] = texture;
	}
}
class Scope
{
public:
	explicit Scope(const char* label, bool onlyIfAbsent = false) : mPrevious(Current), mLabel(label)
	{
		if ((onlyIfAbsent && Current != nullptr) || !PerfAuditCpuDetailActive()) return;
		mActive = true;
		std::fill_n(mState.values, (uint32_t)Counter::Count, uint64_t(0));
		std::fill_n(mState.eligible, 64, nullptr);
		Current = &mState;
		Add(Counter::Operations);
	}
	~Scope()
	{
		if (!mActive) return;
		Current = mPrevious;
		for (uint32_t i = 0; i < (uint32_t)Counter::Count; ++i) PerfAuditAdd(Metrics[i], mState.values[i]);
		if (!PerfAuditCensusRequested()) return;
		char fields[2048];
		// Labels are fixed renderer trace-slot names, not caller/user strings.
		std::snprintf(fields, sizeof(fields),
			"\"label\":\"%s\",\"requests\":%llu,\"recursive_calls\":%llu,\"average_guards\":%llu,\"upload_guards\":%llu,"
			"\"average_signatures\":%llu,\"upload_signatures\":%llu,\"persistent_hits\":%llu,\"frame_hits\":%llu,"
			"\"computations\":%llu,\"successes\":%llu,\"failures\":%llu,\"eligible_requests\":%llu,\"eligible_repeats\":%llu,"
			"\"observer_collisions\":%llu,\"texture_lookup_requests\":%llu,\"texture_lookup_entries\":%llu,\"texture_lookup_bucket_growths\":%llu,"
			"\"material_vector_growths\":%llu,\"metadata_vector_growths\":%llu,\"texture_vector_growths\":%llu,\"capacity_growth_bytes\":%llu",
			mLabel,
			(unsigned long long)mState.values[(uint32_t)Counter::Requests],
			(unsigned long long)mState.values[(uint32_t)Counter::RecursiveCalls],
			(unsigned long long)mState.values[(uint32_t)Counter::AverageGuards],
			(unsigned long long)mState.values[(uint32_t)Counter::UploadGuards],
			(unsigned long long)mState.values[(uint32_t)Counter::AverageSignatures],
			(unsigned long long)mState.values[(uint32_t)Counter::UploadSignatures],
			(unsigned long long)mState.values[(uint32_t)Counter::PersistentHits],
			(unsigned long long)mState.values[(uint32_t)Counter::FrameHits],
			(unsigned long long)mState.values[(uint32_t)Counter::Computations],
			(unsigned long long)mState.values[(uint32_t)Counter::Successes],
			(unsigned long long)mState.values[(uint32_t)Counter::Failures],
			(unsigned long long)mState.values[(uint32_t)Counter::EligibleRequests],
			(unsigned long long)mState.values[(uint32_t)Counter::EligibleRepeats],
			(unsigned long long)mState.values[(uint32_t)Counter::ObserverCollisions],
			(unsigned long long)mState.values[(uint32_t)Counter::TextureLookupRequests],
			(unsigned long long)mState.values[(uint32_t)Counter::TextureLookupEntries],
			(unsigned long long)mState.values[(uint32_t)Counter::TextureLookupBucketGrowths],
			(unsigned long long)mState.values[(uint32_t)Counter::MaterialVectorGrowths],
			(unsigned long long)mState.values[(uint32_t)Counter::MetadataVectorGrowths],
			(unsigned long long)mState.values[(uint32_t)Counter::TextureVectorGrowths],
			(unsigned long long)mState.values[(uint32_t)Counter::CapacityGrowthBytes]);
		PerfAuditWriteEvent("material_color_build", PerfAuditCurrentToken(), fields);
	}
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
private:
	State mState;
	State* mPrevious;
	const char* mLabel;
	bool mActive = false;
};
// The SEH-protected resolver remains in its separate function: do not introduce
// destructors into an MSVC function containing __try/__except.
class AverageCall
{
public:
	AverageCall() : mState(Current) { if (mState != nullptr) { ++mState->averageDepth; Add(Counter::Requests); } }
	~AverageCall() { if (mState != nullptr) --mState->averageDepth; }
private:
	State* mState;
};
}
