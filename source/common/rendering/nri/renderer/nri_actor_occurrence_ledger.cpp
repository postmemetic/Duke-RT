#include "nri_actor_occurrence_ledger.h"

#include <cmath>
#include <cstring>

namespace
{
	uint64_t MixHash(uint64_t hash, uint64_t value)
	{
		value ^= value >> 30u;
		value *= 0xbf58476d1ce4e5b9ull;
		value ^= value >> 27u;
		value *= 0x94d049bb133111ebull;
		value ^= value >> 31u;
		hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
		return hash;
	}

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		static_assert(sizeof(bits) == sizeof(value), "float hash width mismatch");
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	bool TransformFinite(const std::array<float, 12>& transform)
	{
		for (float value : transform)
		{
			if (!std::isfinite(value)) return false;
		}
		return true;
	}
}

size_t NRIActorOccurrenceOwnerKeyHash::operator()(const NRIActorOccurrenceOwnerKey& key) const
{
	return (size_t)MixHash(key.worldEpoch, key.lifetimeGeneration);
}

uint64_t BuildNRIActorOccurrencePublicationHash(const NRIActorOccurrencePublicationFacts& facts)
{
	uint64_t hash = 0x4143544f52505542ull; // ACTORPUB
	hash = MixHash(hash, facts.owner.worldEpoch);
	hash = MixHash(hash, facts.owner.lifetimeGeneration);
	hash = MixHash(hash, facts.placementGeneration);
	hash = MixHash(hash, facts.placementStateHash);
	hash = MixHash(hash, facts.bindingGeneration);
	hash = MixHash(hash, (uint64_t)(uint32_t)facts.physicalSectorIndex);
	for (float value : facts.transform)
	{
		hash = MixHash(hash, FloatBits(value));
	}
	hash = MixHash(hash, facts.roleMask);
	hash = MixHash(hash, facts.exactWorkloadMask);
	hash = MixHash(hash, facts.proxyWorkloadMask);
	hash = MixHash(hash, facts.exactBlasHandle);
	hash = MixHash(hash, facts.proxyBlasHandle);
	return hash != 0 ? hash : 1u;
}

NRIActorOccurrenceLedgerDecision EvaluateNRIActorOccurrencePublication(
	const NRIActorOccurrencePublicationFacts& facts)
{
	NRIActorOccurrenceLedgerDecision decision = {};
	auto reject = [&](NRIActorOccurrenceLedgerReason reason)
	{
		decision.reason = reason;
		return decision;
	};

	if (!facts.owner.IsValid()) return reject(NRIActorOccurrenceLedgerReason::MissingOwner);
	if (facts.placementGeneration == 0 || facts.placementStateHash == 0)
		return reject(NRIActorOccurrenceLedgerReason::MissingPlacement);
	if (facts.physicalSectorIndex < 0) return reject(NRIActorOccurrenceLedgerReason::MissingSector);
	if (!facts.authorityCurrent) return reject(NRIActorOccurrenceLedgerReason::StaleAuthority);
	if (facts.pendingRemoval) return reject(NRIActorOccurrenceLedgerReason::PendingRemoval);
	if (!facts.publicationEligible) return reject(NRIActorOccurrenceLedgerReason::PublicationIneligible);
	if (!TransformFinite(facts.transform)) return reject(NRIActorOccurrenceLedgerReason::InvalidTransform);
	if (facts.bindingGeneration == 0) return reject(NRIActorOccurrenceLedgerReason::MissingBinding);
	if (facts.roleMask == NRI_ACTOR_OCCURRENCE_ROLE_NONE)
		return reject(NRIActorOccurrenceLedgerReason::MissingRole);
	if (facts.exactWorkloadMask == 0 && facts.proxyWorkloadMask == 0)
		return reject(NRIActorOccurrenceLedgerReason::MissingWorkload);
	if ((facts.roleMask & NRI_ACTOR_OCCURRENCE_ROLE_EXACT) != 0 &&
		(facts.exactWorkloadMask == 0 || facts.exactBlasHandle == 0))
		return reject(NRIActorOccurrenceLedgerReason::MissingExactBlas);
	if ((facts.roleMask & NRI_ACTOR_OCCURRENCE_ROLE_SHADOW_PROXY) != 0 &&
		(facts.proxyWorkloadMask == 0 || facts.proxyBlasHandle == 0))
		return reject(NRIActorOccurrenceLedgerReason::MissingProxyBlas);

	decision.reason = NRIActorOccurrenceLedgerReason::Current;
	decision.eligible = true;
	decision.publicationHash = BuildNRIActorOccurrencePublicationHash(facts);
	return decision;
}

const char* GetNRIActorOccurrenceLedgerReasonName(NRIActorOccurrenceLedgerReason reason)
{
	switch (reason)
	{
	case NRIActorOccurrenceLedgerReason::Current: return "current";
	case NRIActorOccurrenceLedgerReason::MissingOwner: return "missing-owner";
	case NRIActorOccurrenceLedgerReason::MissingPlacement: return "missing-placement";
	case NRIActorOccurrenceLedgerReason::MissingSector: return "missing-sector";
	case NRIActorOccurrenceLedgerReason::StaleAuthority: return "stale-authority";
	case NRIActorOccurrenceLedgerReason::PendingRemoval: return "pending-removal";
	case NRIActorOccurrenceLedgerReason::PublicationIneligible: return "publication-ineligible";
	case NRIActorOccurrenceLedgerReason::InvalidTransform: return "invalid-transform";
	case NRIActorOccurrenceLedgerReason::MissingBinding: return "missing-binding";
	case NRIActorOccurrenceLedgerReason::MissingRole: return "missing-role";
	case NRIActorOccurrenceLedgerReason::MissingWorkload: return "missing-workload";
	case NRIActorOccurrenceLedgerReason::MissingExactBlas: return "missing-exact-blas";
	case NRIActorOccurrenceLedgerReason::MissingProxyBlas: return "missing-proxy-blas";
	case NRIActorOccurrenceLedgerReason::DuplicateOwner: return "duplicate-owner";
	case NRIActorOccurrenceLedgerReason::FrameMismatch: return "frame-mismatch";
	case NRIActorOccurrenceLedgerReason::PublicationMismatch: return "publication-mismatch";
	default: return "unknown";
	}
}

void NRIActorOccurrenceLedger::Reset()
{
	mFrameIndex = UINT32_MAX;
	mCommittedFrameIndex = UINT32_MAX;
	mPreparedPublicationHash = 0;
	mCommittedPublicationHash = 0;
	mPublicationSerial = 0;
	mPreparedPublicationCount = 0;
	mEntries.clear();
}

void NRIActorOccurrenceLedger::BeginFrame(uint32_t frameIndex)
{
	mFrameIndex = frameIndex;
	mCommittedFrameIndex = UINT32_MAX;
	mPreparedPublicationHash = 0;
	mPreparedPublicationCount = 0;
	mEntries.clear();
}

NRIActorOccurrenceLedgerDecision NRIActorOccurrenceLedger::CommitPublication(
	const NRIActorOccurrencePublicationFacts& facts)
{
	NRIActorOccurrenceLedgerDecision decision = EvaluateNRIActorOccurrencePublication(facts);
	if (!decision.eligible) return decision;

	auto inserted = mEntries.emplace(facts.owner, Entry{ facts, decision.publicationHash, false });
	if (!inserted.second)
	{
		Entry& existing = inserted.first->second;
		if (!existing.conflicted)
		{
			mPreparedPublicationHash ^= existing.publicationHash;
			if (mPreparedPublicationCount != 0) mPreparedPublicationCount--;
		}
		existing.conflicted = true;
		decision.eligible = false;
		decision.publicationHash = 0;
		decision.reason = NRIActorOccurrenceLedgerReason::DuplicateOwner;
		return decision;
	}

	mPreparedPublicationHash ^= decision.publicationHash;
	mPreparedPublicationCount++;
	return decision;
}

void NRIActorOccurrenceLedger::CommitFrame(uint32_t frameIndex)
{
	if (frameIndex != mFrameIndex)
	{
		mCommittedFrameIndex = UINT32_MAX;
		return;
	}
	const uint64_t preparedHash = MixHash(mPreparedPublicationHash, mPreparedPublicationCount);
	if (preparedHash != mCommittedPublicationHash)
	{
		mCommittedPublicationHash = preparedHash;
		mPublicationSerial++;
	}
	mCommittedFrameIndex = frameIndex;
}

bool NRIActorOccurrenceLedger::IsCommitted(
	uint32_t frameIndex,
	const NRIActorOccurrenceOwnerKey& owner,
	uint64_t placementGeneration,
	uint64_t bindingGeneration,
	uint64_t publicationHash,
	uint32_t requiredRoleMask) const
{
	if (frameIndex != mFrameIndex || frameIndex != mCommittedFrameIndex || publicationHash == 0)
		return false;
	const auto found = mEntries.find(owner);
	if (found == mEntries.end() || found->second.conflicted) return false;
	const Entry& entry = found->second;
	return entry.publicationHash == publicationHash &&
		entry.facts.placementGeneration == placementGeneration &&
		entry.facts.bindingGeneration == bindingGeneration &&
		(entry.facts.roleMask & requiredRoleMask) == requiredRoleMask;
}

bool RunNRIActorOccurrenceLedgerSelfTests(std::string* failureReason)
{
	auto fail = [&](const char* reason)
	{
		if (failureReason != nullptr) *failureReason = reason;
		return false;
	};
	auto baseFacts = []()
	{
		NRIActorOccurrencePublicationFacts facts;
		facts.owner = { 3u, 17u };
		facts.placementGeneration = 9u;
		facts.placementStateHash = 0x1234u;
		facts.bindingGeneration = 0x5678u;
		facts.physicalSectorIndex = 4;
		facts.roleMask = NRI_ACTOR_OCCURRENCE_ROLE_EXACT |
			NRI_ACTOR_OCCURRENCE_ROLE_EMISSIVE_SURFACE |
			NRI_ACTOR_OCCURRENCE_ROLE_LIGHT_ANCHOR;
		facts.exactWorkloadMask = 0x7fu;
		facts.exactBlasHandle = 0x9876u;
		facts.authorityCurrent = true;
		facts.publicationEligible = true;
		return facts;
	};

	const NRIActorOccurrencePublicationFacts base = baseFacts();
	const NRIActorOccurrenceLedgerDecision valid = EvaluateNRIActorOccurrencePublication(base);
	if (!valid.eligible || valid.publicationHash == 0) return fail("current publication rejected");

	auto expectHashChange = [&](NRIActorOccurrencePublicationFacts changed, const char* reason)
	{
		if (BuildNRIActorOccurrencePublicationHash(changed) == valid.publicationHash)
		{
			if (failureReason != nullptr) *failureReason = reason;
			return false;
		}
		return true;
	};
	NRIActorOccurrencePublicationFacts changed = base;
	changed.owner.worldEpoch++;
	if (!expectHashChange(changed, "world epoch missing from hash")) return false;
	changed = base; changed.owner.lifetimeGeneration++;
	if (!expectHashChange(changed, "owner lifetime missing from hash")) return false;
	changed = base; changed.placementGeneration++;
	if (!expectHashChange(changed, "placement generation missing from hash")) return false;
	changed = base; changed.placementStateHash++;
	if (!expectHashChange(changed, "placement state missing from hash")) return false;
	changed = base; changed.physicalSectorIndex++;
	if (!expectHashChange(changed, "sector missing from hash")) return false;
	changed = base; changed.bindingGeneration++;
	if (!expectHashChange(changed, "binding generation missing from hash")) return false;
	changed = base; changed.transform[7] += 1.0f;
	if (!expectHashChange(changed, "full transform missing from hash")) return false;
	changed = base; changed.roleMask ^= NRI_ACTOR_OCCURRENCE_ROLE_EMISSIVE_SURFACE;
	if (!expectHashChange(changed, "role mask missing from hash")) return false;
	changed = base; changed.exactWorkloadMask ^= 0x20u;
	if (!expectHashChange(changed, "workload mask missing from hash")) return false;
	changed = base; changed.exactBlasHandle++;
	if (!expectHashChange(changed, "BLAS handle missing from hash")) return false;

	changed = base; changed.pendingRemoval = true;
	if (EvaluateNRIActorOccurrencePublication(changed).reason != NRIActorOccurrenceLedgerReason::PendingRemoval)
		return fail("pending removal remained eligible");
	changed = base; changed.authorityCurrent = false;
	if (EvaluateNRIActorOccurrencePublication(changed).reason != NRIActorOccurrenceLedgerReason::StaleAuthority)
		return fail("stale authority remained eligible");
	changed = base; changed.publicationEligible = false;
	if (EvaluateNRIActorOccurrencePublication(changed).reason != NRIActorOccurrenceLedgerReason::PublicationIneligible)
		return fail("ineligible placement remained eligible");

	NRIActorOccurrenceLedger ledger;
	ledger.BeginFrame(11u);
	const NRIActorOccurrenceLedgerDecision committed = ledger.CommitPublication(base);
	ledger.CommitFrame(11u);
	if (!committed.eligible || !ledger.IsCommitted(
		11u, base.owner, base.placementGeneration, base.bindingGeneration,
		committed.publicationHash, NRI_ACTOR_OCCURRENCE_ROLE_LIGHT_ANCHOR))
		return fail("committed light role did not join TLAS publication");
	const uint64_t firstAggregateHash = ledger.CommittedPublicationHash();
	const uint64_t firstSerial = ledger.PublicationSerial();

	ledger.BeginFrame(12u);
	const NRIActorOccurrenceLedgerDecision stable = ledger.CommitPublication(base);
	ledger.CommitFrame(12u);
	if (!stable.eligible || ledger.CommittedPublicationHash() != firstAggregateHash ||
		ledger.PublicationSerial() != firstSerial)
		return fail("unchanged publication advanced generation");

	ledger.BeginFrame(13u);
	changed = base;
	changed.placementGeneration++;
	const NRIActorOccurrenceLedgerDecision moved = ledger.CommitPublication(changed);
	ledger.CommitFrame(13u);
	if (!moved.eligible || moved.publicationHash == committed.publicationHash ||
		ledger.PublicationSerial() == firstSerial)
		return fail("moved placement reused old publication");

	ledger.BeginFrame(14u);
	if (!ledger.CommitPublication(base).eligible ||
		ledger.CommitPublication(base).reason != NRIActorOccurrenceLedgerReason::DuplicateOwner)
		return fail("duplicate owner was not rejected");
	ledger.CommitFrame(14u);
	if (ledger.IsCommitted(14u, base.owner, base.placementGeneration, base.bindingGeneration,
		valid.publicationHash, NRI_ACTOR_OCCURRENCE_ROLE_EXACT))
		return fail("conflicted owner remained committed");

	// A deferred overlay frame is explicitly current-or-nothing: an empty
	// commit retires the preceding actor publication without evicting its BLAS,
	// and a later frame can rebind the same representation safely.
	ledger.BeginFrame(15u);
	ledger.CommitFrame(15u);
	if (ledger.IsCommitted(15u, changed.owner, changed.placementGeneration,
		changed.bindingGeneration, moved.publicationHash,
		NRI_ACTOR_OCCURRENCE_ROLE_EXACT))
		return fail("deferred empty frame retained the preceding publication");
	ledger.BeginFrame(16u);
	const NRIActorOccurrenceLedgerDecision resumed = ledger.CommitPublication(changed);
	ledger.CommitFrame(16u);
	if (!resumed.eligible || !ledger.IsCommitted(16u, changed.owner,
		changed.placementGeneration, changed.bindingGeneration,
		resumed.publicationHash, NRI_ACTOR_OCCURRENCE_ROLE_EXACT))
		return fail("current publication did not recover after deferred empty frame");

	return true;
}
