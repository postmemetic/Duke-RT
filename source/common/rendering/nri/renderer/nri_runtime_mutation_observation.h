#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include "perf_audit.h"
#include "printf.h"

// Diagnostic state only. No reference to the worklist, replacement, geometry or
// watchlist is held, so observation cannot admit work or repair discovery.
class NRIRuntimeMutationObservation
{
	struct Miss { uint64_t firstFrame = 0, signature = 0; uint32_t reason = 0; bool open = false; };
	std::vector<Miss> mMisses;
	uint64_t mBuildSerial = 0, mLastFrame = 0;
public:
	struct Sample
	{
		bool eligible = false, valid = false, selected = false, visible = false;
		bool active = false, residentValid = false, residentAuthoritative = false;
		bool sectorDirty = false, dragged = false, signatureChanged = false;
		uint32_t chunk = 0, source = 0, rawReason = 0, reason = 0, sectionDirty = 0;
		int32_t sector = -1;
		uint64_t signature = 0, baselineSignature = 0;
	};
	struct Result { bool started = false, changed = false, ended = false; uint64_t age = 0; };
	void Reset() { mMisses.clear(); mBuildSerial = 0; mLastFrame = 0; }
	void Begin(uint64_t buildSerial, size_t count, uint64_t frame)
	{
		if (mBuildSerial != buildSerial || mMisses.size() != count || frame < mLastFrame)
			mMisses.assign(count, {});
		mBuildSerial = buildSerial; mLastFrame = frame;
	}
	Result Record(uint32_t index, bool missing, uint64_t signature, uint32_t reason)
	{
		Result result;
		if (index >= mMisses.size()) return result;
		auto& miss = mMisses[index];
		if (missing)
		{
			result.started = !miss.open;
			result.changed = miss.open && (miss.signature != signature || miss.reason != reason);
			if (!miss.open) miss.firstFrame = mLastFrame;
			miss.open = true; miss.signature = signature; miss.reason = reason;
			result.age = mLastFrame - miss.firstFrame;
		}
		else if (miss.open)
		{
			result.ended = true; result.age = mLastFrame - miss.firstFrame; miss = {};
		}
		return result;
	}
	template<class ReadSample> void Observe(uint64_t buildSerial, size_t count, uint64_t frame, ReadSample read)
	{
		PerfAuditScope timing(PerfAuditMetric::MutationObservationNs);
		Begin(buildSerial, count, frame);
		uint64_t checks = 0, invalid = 0, missing = 0, starts = 0, changes = 0;
		uint64_t admitted = 0, cleared = 0, ageSum = 0, traces = 0, suppressed = 0;
		for (uint32_t i = 0; i < count; ++i)
		{
			const Sample sample = read(i);
			if (!sample.eligible) { mMisses[i] = {}; continue; }
			++checks;
			if (!sample.valid) { ++invalid; mMisses[i] = {}; continue; }
			const bool missed = sample.reason != 0 && !sample.selected;
			const auto result = Record(i, missed, sample.signature, sample.reason);
			missing += missed; starts += result.started; changes += result.changed;
			if (missed) ageSum += result.age;
			if (result.ended) { admitted += sample.selected; cleared += !sample.selected; }
			if (result.started || result.changed || result.ended)
			{
				if (traces++ >= 16) { ++suppressed; continue; }
				Printf("PERF pt mutation observation NRI: frame=%llu chunk=%u sector=%d action=%s age_frames=%llu raw_reason=0x%x normalized_reason=0x%x source_mask=0x%x selected=%u visible=%u active=%u valid=%u resident_authoritative=%u signature=%llu baseline_signature=%llu signature_changed=%u sector_dirty=%u section_dirty=%u dragged=%u recovery=0\n",
					(unsigned long long)frame, sample.chunk, sample.sector,
					result.started ? "miss-start" : result.changed ? "miss-change" : sample.selected ? "candidate-admitted" : "reason-cleared",
					(unsigned long long)result.age, sample.rawReason, sample.reason, sample.source,
					sample.selected, sample.visible, sample.active, sample.residentValid, sample.residentAuthoritative,
					(unsigned long long)sample.signature, (unsigned long long)sample.baselineSignature,
					sample.signatureChanged, sample.sectorDirty, sample.sectionDirty, sample.dragged);
			}
		}
		PerfAuditAdd(PerfAuditMetric::MutationObservationCalls);
		PerfAuditAdd(PerfAuditMetric::MutationObservationChecks, checks);
		PerfAuditAdd(PerfAuditMetric::MutationObservationInvalid, invalid);
		PerfAuditAdd(PerfAuditMetric::MutationObservationMissing, missing);
		PerfAuditAdd(PerfAuditMetric::MutationObservationMissStarts, starts);
		PerfAuditAdd(PerfAuditMetric::MutationObservationMissChanges, changes);
		PerfAuditAdd(PerfAuditMetric::MutationObservationAdmissions, admitted);
		PerfAuditAdd(PerfAuditMetric::MutationObservationCleared, cleared);
		PerfAuditAdd(PerfAuditMetric::MutationObservationAgeFrames, ageSum);
		PerfAuditAdd(PerfAuditMetric::MutationObservationTraceSuppressed, suppressed);
	}
};
