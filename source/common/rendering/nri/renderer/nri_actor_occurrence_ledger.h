#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

enum NRIActorOccurrencePublicationRole : uint32_t
{
	NRI_ACTOR_OCCURRENCE_ROLE_NONE = 0,
	NRI_ACTOR_OCCURRENCE_ROLE_EXACT = 1u << 0,
	NRI_ACTOR_OCCURRENCE_ROLE_SHADOW_PROXY = 1u << 1,
	NRI_ACTOR_OCCURRENCE_ROLE_EMISSIVE_SURFACE = 1u << 2,
	NRI_ACTOR_OCCURRENCE_ROLE_LIGHT_ANCHOR = 1u << 3,
};

enum class NRIActorOccurrenceLedgerReason : uint32_t
{
	Current = 0,
	MissingOwner,
	MissingPlacement,
	MissingSector,
	StaleAuthority,
	PendingRemoval,
	PublicationIneligible,
	InvalidTransform,
	MissingBinding,
	MissingRole,
	MissingWorkload,
	MissingExactBlas,
	MissingProxyBlas,
	DuplicateOwner,
	FrameMismatch,
	PublicationMismatch,
};

struct NRIActorOccurrenceOwnerKey
{
	uint64_t worldEpoch = 0;
	uint64_t lifetimeGeneration = 0;

	// Engine actor index 0 is a valid serialized lifetime generation. The world
	// epoch is the nonzero validity discriminator and prevents cross-reset ABA.
	bool IsValid() const { return worldEpoch != 0; }
	bool operator==(const NRIActorOccurrenceOwnerKey& other) const
	{
		return worldEpoch == other.worldEpoch && lifetimeGeneration == other.lifetimeGeneration;
	}
};

struct NRIActorOccurrenceOwnerKeyHash
{
	size_t operator()(const NRIActorOccurrenceOwnerKey& key) const;
};

// Complete, renderer-local publication facts. Resource residency is deliberately
// absent: a reusable BLAS/material may outlive every placement that references it.
struct NRIActorOccurrencePublicationFacts
{
	NRIActorOccurrenceOwnerKey owner;
	uint64_t placementGeneration = 0;
	uint64_t placementStateHash = 0;
	uint64_t bindingGeneration = 0;
	int32_t physicalSectorIndex = -1;
	uint32_t roleMask = NRI_ACTOR_OCCURRENCE_ROLE_NONE;
	uint32_t exactWorkloadMask = 0;
	uint32_t proxyWorkloadMask = 0;
	uint64_t exactBlasHandle = 0;
	uint64_t proxyBlasHandle = 0;
	bool authorityCurrent = false;
	bool publicationEligible = false;
	bool pendingRemoval = false;
	std::array<float, 12> transform = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};
};

struct NRIActorOccurrenceLedgerDecision
{
	NRIActorOccurrenceLedgerReason reason = NRIActorOccurrenceLedgerReason::MissingOwner;
	uint64_t publicationHash = 0;
	bool eligible = false;
};

NRIActorOccurrenceLedgerDecision EvaluateNRIActorOccurrencePublication(
	const NRIActorOccurrencePublicationFacts& facts);
uint64_t BuildNRIActorOccurrencePublicationHash(
	const NRIActorOccurrencePublicationFacts& facts);
const char* GetNRIActorOccurrenceLedgerReasonName(NRIActorOccurrenceLedgerReason reason);

class NRIActorOccurrenceLedger
{
public:
	void Reset();
	void BeginFrame(uint32_t frameIndex);
	NRIActorOccurrenceLedgerDecision CommitPublication(
		const NRIActorOccurrencePublicationFacts& facts);
	void CommitFrame(uint32_t frameIndex);
	bool IsCommitted(
		uint32_t frameIndex,
		const NRIActorOccurrenceOwnerKey& owner,
		uint64_t placementGeneration,
		uint64_t bindingGeneration,
		uint64_t publicationHash,
		uint32_t requiredRoleMask) const;

	uint64_t CommittedPublicationHash() const { return mCommittedPublicationHash; }
	uint64_t PublicationSerial() const { return mPublicationSerial; }
	uint32_t CommittedFrameIndex() const { return mCommittedFrameIndex; }

private:
	struct Entry
	{
		NRIActorOccurrencePublicationFacts facts;
		uint64_t publicationHash = 0;
		bool conflicted = false;
	};

	uint32_t mFrameIndex = UINT32_MAX;
	uint32_t mCommittedFrameIndex = UINT32_MAX;
	uint64_t mPreparedPublicationHash = 0;
	uint64_t mCommittedPublicationHash = 0;
	uint64_t mPublicationSerial = 0;
	uint32_t mPreparedPublicationCount = 0;
	std::unordered_map<NRIActorOccurrenceOwnerKey, Entry, NRIActorOccurrenceOwnerKeyHash> mEntries;
};

bool RunNRIActorOccurrenceLedgerSelfTests(std::string* failureReason = nullptr);
