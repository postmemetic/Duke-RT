#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

constexpr uint32_t NRI_VOXEL_ADMISSION_INVALID_SLOT = UINT32_MAX;

enum class NRIVoxelAdmissionFairnessClass : uint8_t
{
	VisibleNoFallback,
	OldVisiblePending,
	RequiredLoading,
	OptionalLoading,
	Diagnostics,
	Count,
};

enum class NRIVoxelAdmissionStage : uint8_t
{
	DependencyPending,
	ComputeQueued,
	ComputeSubmitted,
	ComputeReady,
	BlasQueued,
	BlasSubmitted,
	PublicationPending,
	TlasPending,
	Ready,
	Cancelled,
	Failed,
	Stale,
	Count,
};

enum class NRIVoxelAdmissionReservationState : uint8_t
{
	Active,
	RetirePending,
	Abandoned,
	Released,
};

enum class NRIVoxelAdmissionResultCode : uint8_t
{
	Accepted,
	BindingAttached,
	DuplicateBinding,
	InvalidRequest,
	StaleGeneration,
	JobLimit,
	ByteLimit,
	BlasLimit,
	OptionalReserve,
	PairConflict,
	IncompatibleReservation,
	BindingCapacity,
};

enum class NRIVoxelAdmissionInvariant : uint32_t
{
	None = 0,
	StageCounts = 1u << 0,
	ActiveCounts = 1u << 1,
	ReservationBytes = 1u << 2,
	OwnerRegistries = 1u << 3,
	SlotRegistries = 1u << 4,
	TokenState = 1u << 5,
	Limits = 1u << 6,
};

struct NRIVoxelAdmissionMeshIdentity
{
	uint64_t mapGeneration = 0;
	uint64_t meshKey = 0;

	bool operator==(const NRIVoxelAdmissionMeshIdentity& other) const
	{
		return mapGeneration == other.mapGeneration && meshKey == other.meshKey;
	}
};

struct NRIVoxelAdmissionPairIdentity
{
	uint64_t mapGeneration = 0;
	uint64_t pairKey = 0;

	bool operator==(const NRIVoxelAdmissionPairIdentity& other) const
	{
		return mapGeneration == other.mapGeneration && pairKey == other.pairKey;
	}
};

struct NRIVoxelAdmissionReservation
{
	uint32_t vertexCapacity = 0;
	uint32_t indexCapacity = 0;
	uint32_t primitiveCapacity = 0;
	uint32_t materialBindingCapacity = 0;
	uint64_t vertexBytes = 0;
	uint64_t indexBytes = 0;
	uint64_t primitiveBytes = 0;
	uint64_t materialBytes = 0;
	uint64_t blasBytes = 0;
	uint64_t blasScratchBytes = 0;

	uint64_t TotalBytes() const;
	uint64_t BlasReservationBytes() const;
	bool operator==(const NRIVoxelAdmissionReservation& other) const;
};

struct NRIVoxelAdmissionLimits
{
	uint64_t activeMapGeneration = 0;
	uint32_t maxActiveJobs = 0;
	uint64_t maxReservedBytes = 0;
	uint64_t maxReservedBlasBytes = 0;
	uint32_t maxComputeSlots = 0;
	uint32_t maxBlasLanes = 0;
	uint64_t oversizedReservationBytes = 0;
	uint64_t oversizedBlasBytes = 0;
	uint32_t optionalActiveJobReserve = 0;
	uint64_t optionalByteReserve = 0;
	uint64_t optionalBlasByteReserve = 0;
	uint32_t optionalComputeSlotReserve = 0;
	uint32_t optionalBlasLaneReserve = 0;
};

struct NRIVoxelAdmissionRequest
{
	NRIVoxelAdmissionMeshIdentity mesh;
	uint64_t pairKey = 0;
	uint64_t materialKey = 0;
	NRIVoxelAdmissionReservation reservation;
	NRIVoxelAdmissionFairnessClass fairnessClass = NRIVoxelAdmissionFairnessClass::OptionalLoading;
	uint64_t age = 0;
	bool dependenciesReady = false;
	bool forceExclusive = false;
};

struct NRIVoxelAdmissionResult
{
	NRIVoxelAdmissionResultCode code = NRIVoxelAdmissionResultCode::InvalidRequest;
	uint64_t tokenId = 0;
};

struct NRIVoxelAdmissionBindingSnapshot
{
	uint64_t pairKey = 0;
	uint64_t materialKey = 0;
};

struct NRIVoxelAdmissionTokenSnapshot
{
	uint64_t tokenId = 0;
	NRIVoxelAdmissionMeshIdentity mesh;
	NRIVoxelAdmissionReservation reservation;
	NRIVoxelAdmissionFairnessClass fairnessClass = NRIVoxelAdmissionFairnessClass::OptionalLoading;
	NRIVoxelAdmissionStage stage = NRIVoxelAdmissionStage::DependencyPending;
	NRIVoxelAdmissionReservationState reservationState = NRIVoxelAdmissionReservationState::Active;
	uint64_t age = 0;
	uint64_t sequence = 0;
	uint32_t computeSlot = NRI_VOXEL_ADMISSION_INVALID_SLOT;
	uint32_t blasLane = NRI_VOXEL_ADMISSION_INVALID_SLOT;
	bool sourceReady = false;
	bool oversizedExclusive = false;
	std::vector<NRIVoxelAdmissionBindingSnapshot> bindings;
};

struct NRIVoxelAdmissionSnapshot
{
	uint64_t activeMapGeneration = 0;
	std::array<uint32_t, (size_t)NRIVoxelAdmissionStage::Count> stageCounts = {};
	std::array<uint32_t, (size_t)NRIVoxelAdmissionFairnessClass::Count> activeFairnessCounts = {};
	uint32_t activeJobs = 0;
	uint32_t activeBindings = 0;
	uint32_t meshOwnerCount = 0;
	uint32_t pairOwnerCount = 0;
	uint32_t computeInFlight = 0;
	uint32_t blasInFlight = 0;
	uint64_t activeReservationBytes = 0;
	uint64_t activeBlasBytes = 0;
	uint64_t retirePendingBytes = 0;
	uint64_t retirePendingBlasBytes = 0;
	uint64_t abandonedBytes = 0;
	uint64_t abandonedBlasBytes = 0;
	uint64_t heldReservationBytes = 0;
	uint64_t heldBlasBytes = 0;
	uint64_t acceptedJobs = 0;
	uint64_t withdrawnPreflights = 0;
	uint64_t tokenRecords = 0;
	uint64_t computeQueueEntries = 0;
	uint64_t blasQueueEntries = 0;
	uint64_t attachedBindings = 0;
	uint64_t rejectedJobs = 0;
	uint64_t invalidTransitions = 0;
};

struct NRIVoxelAdmissionInvariantReport
{
	bool valid = true;
	uint32_t violations = 0;
	NRIVoxelAdmissionInvariant flags = NRIVoxelAdmissionInvariant::None;
};

class NRIVoxelAdmissionScheduler
{
public:
	explicit NRIVoxelAdmissionScheduler(const NRIVoxelAdmissionLimits& limits);

	NRIVoxelAdmissionResult Admit(const NRIVoxelAdmissionRequest& request);
	bool MarkDependenciesReady(uint64_t tokenId);
	bool AcquireNextCompute(uint32_t computeSlot, uint64_t& tokenId);
	bool SubmitCompute(uint64_t tokenId);
	bool MarkComputeReady(uint64_t tokenId);
	bool QueueBlas(uint64_t tokenId);
	bool AcquireNextBlas(uint32_t blasLane, uint64_t& tokenId);
	bool SubmitBlas(uint64_t tokenId);
	bool MarkBlasReady(uint64_t tokenId);
	bool MarkPublished(uint64_t tokenId);
	bool MarkTlasReady(uint64_t tokenId);
	bool Cancel(uint64_t tokenId);
	// Reverse a new sole-owner queue reservation before any physical work is acquired.
	bool WithdrawUnsubmittedPreflight(uint64_t tokenId, uint64_t pairKey);
	bool Fail(uint64_t tokenId);
	bool RetireTerminalReservation(uint64_t tokenId);
	bool AbandonTerminalReservation(uint64_t tokenId);

	uint32_t SetActiveMapGeneration(uint64_t mapGeneration);
	void Reset(uint64_t activeMapGeneration);

	uint64_t FindMeshOwner(const NRIVoxelAdmissionMeshIdentity& identity) const;
	uint64_t FindTlasPending(const NRIVoxelAdmissionMeshIdentity& identity) const;
	uint64_t FindPairOwner(const NRIVoxelAdmissionPairIdentity& identity) const;
	// Poll live lane ownership without copying retained terminal-token history.
	bool IsDirectBlasLaneBlocked(uint64_t tokenId, bool exclusive) const;
	bool GetTokenSnapshot(uint64_t tokenId, NRIVoxelAdmissionTokenSnapshot& snapshot) const;
	std::vector<NRIVoxelAdmissionTokenSnapshot> GetTokenSnapshots() const;
	NRIVoxelAdmissionSnapshot GetSnapshot() const;
	NRIVoxelAdmissionInvariantReport ValidateInvariants() const;

private:
	struct IdentityHash
	{
		size_t operator()(const NRIVoxelAdmissionMeshIdentity& identity) const;
		size_t operator()(const NRIVoxelAdmissionPairIdentity& identity) const;
	};

	struct Token
	{
		uint64_t tokenId = 0;
		NRIVoxelAdmissionMeshIdentity mesh;
		NRIVoxelAdmissionReservation reservation;
		NRIVoxelAdmissionFairnessClass fairnessClass = NRIVoxelAdmissionFairnessClass::OptionalLoading;
		NRIVoxelAdmissionStage stage = NRIVoxelAdmissionStage::DependencyPending;
		NRIVoxelAdmissionReservationState reservationState = NRIVoxelAdmissionReservationState::Active;
		uint64_t age = 0;
		uint64_t sequence = 0;
		uint32_t computeSlot = NRI_VOXEL_ADMISSION_INVALID_SLOT;
		uint32_t blasLane = NRI_VOXEL_ADMISSION_INVALID_SLOT;
		bool sourceReady = false;
		bool oversizedExclusive = false;
		std::vector<NRIVoxelAdmissionBindingSnapshot> bindings;
	};

	using TokenMap = std::unordered_map<uint64_t, Token>;

	bool IsBulk(NRIVoxelAdmissionFairnessClass fairnessClass) const;
	bool IsTerminal(NRIVoxelAdmissionStage stage) const;
	bool IsAdmissionActive(NRIVoxelAdmissionStage stage) const;
	bool IsValidRequest(const NRIVoxelAdmissionRequest& request, uint64_t& totalBytes, uint64_t& blasBytes) const;
	bool Transition(Token& token, NRIVoxelAdmissionStage expected, NRIVoxelAdmissionStage next);
	bool Terminalize(Token& token, NRIVoxelAdmissionStage terminalStage);
	void ReleaseReservation(Token& token);
	void ReleaseAdmissionOwnership(Token& token, bool releaseReservation);
	void RemoveOwners(const Token& token);
	void FillTokenSnapshot(const Token& token, NRIVoxelAdmissionTokenSnapshot& snapshot) const;
	Token* FindToken(uint64_t tokenId);
	const Token* FindToken(uint64_t tokenId) const;
	Token* SelectQueuedToken(std::deque<uint64_t>& queue, bool compute);
	bool CanAcquire(const Token& token, bool compute) const;
	bool HasQueuedExclusiveToken() const;
	void CountStageTransition(NRIVoxelAdmissionStage from, NRIVoxelAdmissionStage to);

	NRIVoxelAdmissionLimits m_limits;
	TokenMap m_tokens;
	std::unordered_map<NRIVoxelAdmissionMeshIdentity, uint64_t, IdentityHash> m_meshOwners;
	std::unordered_map<NRIVoxelAdmissionMeshIdentity, uint64_t, IdentityHash> m_tlasPending;
	std::unordered_map<NRIVoxelAdmissionPairIdentity, uint64_t, IdentityHash> m_pairOwners;
	std::unordered_map<uint32_t, uint64_t> m_computeSlots;
	std::unordered_map<uint32_t, uint64_t> m_blasLanes;
	std::deque<uint64_t> m_computeQueue;
	std::deque<uint64_t> m_blasQueue;
	NRIVoxelAdmissionSnapshot m_snapshot;
	uint64_t m_nextTokenId = 1;
	uint64_t m_nextSequence = 1;
	uint32_t m_bulkActiveJobs = 0;
	uint32_t m_bulkComputeInFlight = 0;
	uint32_t m_bulkBlasInFlight = 0;
	uint64_t m_bulkHeldBytes = 0;
	uint64_t m_bulkHeldBlasBytes = 0;
	uint64_t m_oversizedComputeToken = 0;
	uint64_t m_oversizedBlasToken = 0;
};
