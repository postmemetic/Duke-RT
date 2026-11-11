#include "nri_material_average_color_cache.h"
#include "perf_audit.h"
#include "c_cvars.h"
#include "printf.h"

#include <cstring>

CVAR(Bool, nri_ptmaterialcolorreuse, true, 0)
CVAR(Bool, nri_ptmaterialcolorvalidate, false, 0)

namespace nri_scene
{
namespace { thread_local ScopedMaterialAverageColorCache* Current = nullptr; }

ScopedMaterialAverageColorCache::ScopedMaterialAverageColorCache()
	: mPrevious(Current), mEnabled(nri_ptmaterialcolorreuse),
	  mValidate(mEnabled && nri_ptmaterialcolorvalidate), mAudit(PerfAuditCpuDetailActive())
{
	Current = mEnabled ? this : nullptr;
	if (mEnabled) for (auto& entry : mEntries) entry.texture = nullptr;
}

void ScopedMaterialAverageColorCache::Invalidate()
{
	++mEpoch;
	++mNestedInvalidations;
	for (auto& entry : mEntries) entry.texture = nullptr;
}

ScopedMaterialAverageColorCache::~ScopedMaterialAverageColorCache()
{
	Current = mPrevious;
	if (mPrevious != nullptr) mPrevious->Invalidate();
	if (!mEnabled || (!mAudit && !mValidate)) return;
	PerfAuditAdd(PerfAuditMetric::MaterialColorMemoRequests, mRequests);
	PerfAuditAdd(PerfAuditMetric::MaterialColorMemoHits, mHits);
	PerfAuditAdd(PerfAuditMetric::MaterialColorMemoMisses, mMisses);
	PerfAuditAdd(PerfAuditMetric::MaterialColorMemoBypasses, mBypasses);
	PerfAuditAdd(PerfAuditMetric::MaterialColorValidationChecks, mChecks);
	PerfAuditAdd(PerfAuditMetric::MaterialColorHitValidationChecks, mHitChecks);
	PerfAuditAdd(PerfAuditMetric::MaterialColorValidationMismatches, mMismatches);
	PerfAuditAdd(PerfAuditMetric::MaterialColorQuarantines, mQuarantined ? 1 : 0);
	PerfAuditAdd(PerfAuditMetric::MaterialColorNestedInvalidations, mNestedInvalidations);
}

bool ScopedMaterialAverageColorCache::Invoke(FGameTexture* texture, float* color, bool consolidate,
	bool& eligible, MaterialAverageColorResolver resolver)
{
	struct Depth { uint32_t& depth; Depth(uint32_t& d) : depth(d) { ++depth; } ~Depth() { --depth; } } guard(mDepth);
	return resolver(texture, color, consolidate, eligible);
}

bool ResolveMaterialAverageColor(FGameTexture* texture, float* outColor, MaterialAverageColorResolver resolver)
{
	auto* scope = Current;
	bool eligible = false;
	if (scope == nullptr) return resolver(texture, outColor, false, eligible);
	++scope->mRequests;
	if (scope->mQuarantined || scope->mDepth != 0 || texture == nullptr || outColor == nullptr)
	{
		++scope->mBypasses;
		return scope->Invoke(texture, outColor, false, eligible, resolver);
	}
	const uintptr_t bits = reinterpret_cast<uintptr_t>(texture);
	auto& entry = scope->mEntries[((bits >> 4) ^ (bits >> 11)) & 63u];
	const bool hit = entry.texture == texture;
	const uint64_t epoch = scope->mEpoch;
	float original[3];
	if (scope->mValidate) std::memcpy(original, outColor, sizeof(original));
	bool success;
	if (hit)
	{
		++scope->mHits;
		std::memcpy(outColor, entry.color, sizeof(entry.color));
		success = eligible = true;
	}
	else
	{
		++scope->mMisses;
		success = scope->Invoke(texture, outColor, true, eligible, resolver);
		if (!success || !eligible) ++scope->mBypasses;
	}
	if (scope->mValidate)
	{
		bool referenceEligible = false;
		const bool referenceSuccess = scope->Invoke(texture, original, false, referenceEligible, resolver);
		++scope->mChecks;
		if (hit) ++scope->mHitChecks;
		if (referenceSuccess != success || referenceEligible != eligible ||
			std::memcmp(original, outColor, sizeof(original)) != 0)
		{
			++scope->mMismatches;
			scope->mQuarantined = true;
			Printf("NRI material color reuse validation mismatch (hit=%d); quarantining this build.\n", hit ? 1 : 0);
		}
		// The reference is authoritative even if nested work invalidated a candidate.
		std::memcpy(outColor, original, sizeof(original));
		success = referenceSuccess;
		eligible = referenceEligible;
	}
	if (!hit && success && eligible && !scope->mQuarantined && scope->mEpoch == epoch)
	{
		entry.texture = texture;
		std::memcpy(entry.color, outColor, sizeof(entry.color));
	}
	return success;
}
}
