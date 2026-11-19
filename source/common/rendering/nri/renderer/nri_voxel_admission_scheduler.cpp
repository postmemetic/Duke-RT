#include "nri_voxel_admission_scheduler.h"

#include <algorithm>
#include <limits>

namespace
{
	bool AddWouldOverflow(uint64_t a, uint64_t b)
	{
		return b > std::numeric_limits<uint64_t>::max() - a;
	}

	uint64_t SaturatingAdd(uint64_t a, uint64_t b)
	{
		return AddWouldOverflow(a, b) ? std::numeric_limits<uint64_t>::max() : a + b;
	}

	size_t HashIdentity(uint64_t generation, uint64_t key)
	{
		const size_t first = std::hash<uint64_t>{}(generation);
		const size_t second = std::hash<uint64_t>{}(key);
		return first ^ (second + (size_t)0x9e3779b9u + (first << 6) + (first >> 2));
	}

}

uint64_t NRIVoxelAdmissionReservation::TotalBytes() const
{
	uint64_t total = vertexBytes;
	total = SaturatingAdd(total, indexBytes);
	total = SaturatingAdd(total, primitiveBytes);
	total = SaturatingAdd(total, materialBytes);
	total = SaturatingAdd(total, blasBytes);
	return SaturatingAdd(total, blasScratchBytes);
}

uint64_t NRIVoxelAdmissionReservation::BlasReservationBytes() const
{
	return SaturatingAdd(blasBytes, blasScratchBytes);
}

bool NRIVoxelAdmissionReservation::operator==(const NRIVoxelAdmissionReservation& other) const
{
	return
		vertexCapacity == other.vertexCapacity &&
		indexCapacity == other.indexCapacity &&
		primitiveCapacity == other.primitiveCapacity &&
		materialBindingCapacity == other.materialBindingCapacity &&
		vertexBytes == other.vertexBytes &&
		indexBytes == other.indexBytes &&
		primitiveBytes == other.primitiveBytes &&
		materialBytes == other.materialBytes &&
		blasBytes == other.blasBytes &&
		blasScratchBytes == other.blasScratchBytes;
}

size_t NRIVoxelAdmissionScheduler::IdentityHash::operator()(const NRIVoxelAdmissionMeshIdentity& identity) const
{
	return HashIdentity(identity.mapGeneration, identity.meshKey);
}

size_t NRIVoxelAdmissionScheduler::IdentityHash::operator()(const NRIVoxelAdmissionPairIdentity& identity) const
{
	return HashIdentity(identity.mapGeneration, identity.pairKey);
}

NRIVoxelAdmissionScheduler::NRIVoxelAdmissionScheduler(const NRIVoxelAdmissionLimits& limits)
	: m_limits(limits)
{
	m_snapshot.activeMapGeneration = limits.activeMapGeneration;
}

bool NRIVoxelAdmissionScheduler::IsBulk(NRIVoxelAdmissionFairnessClass fairnessClass) const
{
	return fairnessClass == NRIVoxelAdmissionFairnessClass::OptionalLoading ||
		fairnessClass == NRIVoxelAdmissionFairnessClass::Diagnostics;
}

bool NRIVoxelAdmissionScheduler::IsTerminal(NRIVoxelAdmissionStage stage) const
{
	return stage == NRIVoxelAdmissionStage::Ready ||
		stage == NRIVoxelAdmissionStage::Cancelled ||
		stage == NRIVoxelAdmissionStage::Failed ||
		stage == NRIVoxelAdmissionStage::Stale;
}

bool NRIVoxelAdmissionScheduler::IsAdmissionActive(NRIVoxelAdmissionStage stage) const
{
	return !IsTerminal(stage) && stage != NRIVoxelAdmissionStage::TlasPending;
}

bool NRIVoxelAdmissionScheduler::IsValidRequest(
	const NRIVoxelAdmissionRequest& request,
	uint64_t& totalBytes,
	uint64_t& blasBytes) const
{
	if (request.mesh.mapGeneration == 0 || request.mesh.meshKey == 0 || request.pairKey == 0 || request.materialKey == 0 ||
		(uint8_t)request.fairnessClass >= (uint8_t)NRIVoxelAdmissionFairnessClass::Count ||
		request.reservation.vertexCapacity == 0 || request.reservation.indexCapacity == 0 ||
		request.reservation.primitiveCapacity == 0 || request.reservation.materialBindingCapacity == 0 ||
		request.reservation.vertexBytes == 0 || request.reservation.indexBytes == 0 ||
		request.reservation.primitiveBytes == 0 || request.reservation.blasBytes == 0)
	{
		return false;
	}

	const uint64_t values[] = {
		request.reservation.vertexBytes,
		request.reservation.indexBytes,
		request.reservation.primitiveBytes,
		request.reservation.materialBytes,
		request.reservation.blasBytes,
		request.reservation.blasScratchBytes,
	};
	totalBytes = 0;
	for (uint64_t value : values)
	{
		if (AddWouldOverflow(totalBytes, value))
		{
			return false;
		}
		totalBytes += value;
	}
	if (AddWouldOverflow(request.reservation.blasBytes, request.reservation.blasScratchBytes))
	{
		return false;
	}
	blasBytes = request.reservation.blasBytes + request.reservation.blasScratchBytes;
	return true;
}

NRIVoxelAdmissionResult NRIVoxelAdmissionScheduler::Admit(const NRIVoxelAdmissionRequest& request)
{
	uint64_t totalBytes = 0;
	uint64_t blasBytes = 0;
	if (!IsValidRequest(request, totalBytes, blasBytes))
	{
		m_snapshot.rejectedJobs++;
		return { NRIVoxelAdmissionResultCode::InvalidRequest, 0 };
	}
	if (m_limits.activeMapGeneration == 0 || request.mesh.mapGeneration != m_limits.activeMapGeneration)
	{
		m_snapshot.rejectedJobs++;
		return { NRIVoxelAdmissionResultCode::StaleGeneration, 0 };
	}
	const bool oversizedExclusive = request.forceExclusive ||
		(m_limits.oversizedReservationBytes != 0 && totalBytes > m_limits.oversizedReservationBytes) ||
		(m_limits.oversizedBlasBytes != 0 && blasBytes > m_limits.oversizedBlasBytes);

	const NRIVoxelAdmissionPairIdentity pairIdentity = { request.mesh.mapGeneration, request.pairKey };
	auto pairOwner = m_pairOwners.find(pairIdentity);
	if (pairOwner != m_pairOwners.end())
	{
		const Token* token = FindToken(pairOwner->second);
		if (token != nullptr && token->mesh == request.mesh)
		{
			if (!(token->reservation == request.reservation))
			{
				m_snapshot.rejectedJobs++;
				return { NRIVoxelAdmissionResultCode::IncompatibleReservation, token->tokenId };
			}
			const auto binding = std::find_if(token->bindings.begin(), token->bindings.end(), [&request](const auto& existing)
			{
				return existing.pairKey == request.pairKey;
			});
			if (binding != token->bindings.end() && binding->materialKey == request.materialKey)
			{
				return { NRIVoxelAdmissionResultCode::DuplicateBinding, token->tokenId };
			}
		}
		m_snapshot.rejectedJobs++;
		return { NRIVoxelAdmissionResultCode::PairConflict, 0 };
	}

	auto meshOwner = m_meshOwners.find(request.mesh);
	if (meshOwner != m_meshOwners.end())
	{
		Token* token = FindToken(meshOwner->second);
		if (token == nullptr || token->reservationState != NRIVoxelAdmissionReservationState::Active)
		{
			m_snapshot.rejectedJobs++;
			return { NRIVoxelAdmissionResultCode::PairConflict, 0 };
		}
		if (!(token->reservation == request.reservation))
		{
			m_snapshot.rejectedJobs++;
			return { NRIVoxelAdmissionResultCode::IncompatibleReservation, token->tokenId };
		}
		if (token->bindings.size() >= token->reservation.materialBindingCapacity)
		{
			m_snapshot.rejectedJobs++;
			return { NRIVoxelAdmissionResultCode::BindingCapacity, token->tokenId };
		}

		const bool wasBulk = IsBulk(token->fairnessClass);
		if ((uint8_t)request.fairnessClass < (uint8_t)token->fairnessClass)
		{
			m_snapshot.activeFairnessCounts[(size_t)token->fairnessClass]--;
			token->fairnessClass = request.fairnessClass;
			m_snapshot.activeFairnessCounts[(size_t)token->fairnessClass]++;
		}
		if (wasBulk && !IsBulk(token->fairnessClass))
		{
			m_bulkActiveJobs--;
			m_bulkHeldBytes -= totalBytes;
			m_bulkHeldBlasBytes -= blasBytes;
			if (token->stage == NRIVoxelAdmissionStage::ComputeSubmitted)
			{
				m_bulkComputeInFlight--;
			}
			if (token->stage == NRIVoxelAdmissionStage::BlasSubmitted)
			{
				m_bulkBlasInFlight--;
			}
		}
		token->age = std::max(token->age, request.age);
		token->bindings.push_back({ request.pairKey, request.materialKey });
		m_pairOwners.emplace(pairIdentity, token->tokenId);
		m_snapshot.activeBindings++;
		m_snapshot.attachedBindings++;
		if (request.dependenciesReady && token->stage == NRIVoxelAdmissionStage::DependencyPending)
		{
			MarkDependenciesReady(token->tokenId);
		}
		return { NRIVoxelAdmissionResultCode::BindingAttached, token->tokenId };
	}

	if (m_limits.maxActiveJobs == 0 || m_snapshot.activeJobs >= m_limits.maxActiveJobs)
	{
		m_snapshot.rejectedJobs++;
		return { NRIVoxelAdmissionResultCode::JobLimit, 0 };
	}
	if (m_limits.maxReservedBytes == 0 || AddWouldOverflow(m_snapshot.heldReservationBytes, totalBytes) ||
		m_snapshot.heldReservationBytes + totalBytes > m_limits.maxReservedBytes)
	{
		m_snapshot.rejectedJobs++;
		return { NRIVoxelAdmissionResultCode::ByteLimit, 0 };
	}
	if (m_limits.maxReservedBlasBytes == 0 || AddWouldOverflow(m_snapshot.heldBlasBytes, blasBytes) ||
		m_snapshot.heldBlasBytes + blasBytes > m_limits.maxReservedBlasBytes)
	{
		m_snapshot.rejectedJobs++;
		return { NRIVoxelAdmissionResultCode::BlasLimit, 0 };
	}

	const bool bulk = IsBulk(request.fairnessClass);
	const bool idleOversizedExclusive =
		bulk && oversizedExclusive &&
		m_snapshot.activeJobs == 0 &&
		m_snapshot.computeInFlight == 0 &&
		m_snapshot.blasInFlight == 0 &&
		m_snapshot.heldReservationBytes == 0 &&
		m_snapshot.heldBlasBytes == 0;
	if (bulk && !idleOversizedExclusive)
	{
		const uint32_t optionalJobLimit = m_limits.maxActiveJobs > m_limits.optionalActiveJobReserve ?
			m_limits.maxActiveJobs - m_limits.optionalActiveJobReserve : 0;
		const uint64_t optionalByteLimit = m_limits.maxReservedBytes > m_limits.optionalByteReserve ?
			m_limits.maxReservedBytes - m_limits.optionalByteReserve : 0;
		const uint64_t optionalBlasLimit = m_limits.maxReservedBlasBytes > m_limits.optionalBlasByteReserve ?
			m_limits.maxReservedBlasBytes - m_limits.optionalBlasByteReserve : 0;
		if (m_bulkActiveJobs >= optionalJobLimit || AddWouldOverflow(m_bulkHeldBytes, totalBytes) ||
			m_bulkHeldBytes + totalBytes > optionalByteLimit || AddWouldOverflow(m_bulkHeldBlasBytes, blasBytes) ||
			m_bulkHeldBlasBytes + blasBytes > optionalBlasLimit)
		{
			m_snapshot.rejectedJobs++;
			return { NRIVoxelAdmissionResultCode::OptionalReserve, 0 };
		}
	}

	Token token = {};
	token.tokenId = m_nextTokenId++;
	token.mesh = request.mesh;
	token.reservation = request.reservation;
	token.fairnessClass = request.fairnessClass;
	token.stage = request.dependenciesReady ? NRIVoxelAdmissionStage::ComputeQueued : NRIVoxelAdmissionStage::DependencyPending;
	token.age = request.age;
	token.sequence = m_nextSequence++;
	token.sourceReady = request.dependenciesReady;
	token.oversizedExclusive = oversizedExclusive;
	token.bindings.push_back({ request.pairKey, request.materialKey });
	const uint64_t tokenId = token.tokenId;
	m_tokens.emplace(tokenId, std::move(token));
	m_meshOwners.emplace(request.mesh, tokenId);
	m_pairOwners.emplace(pairIdentity, tokenId);
	if (request.dependenciesReady)
	{
		m_computeQueue.push_back(tokenId);
	}

	m_snapshot.stageCounts[(size_t)(request.dependenciesReady ? NRIVoxelAdmissionStage::ComputeQueued : NRIVoxelAdmissionStage::DependencyPending)]++;
	m_snapshot.activeFairnessCounts[(size_t)request.fairnessClass]++;
	m_snapshot.activeJobs++;
	m_snapshot.activeBindings++;
	m_snapshot.activeReservationBytes += totalBytes;
	m_snapshot.activeBlasBytes += blasBytes;
	m_snapshot.heldReservationBytes += totalBytes;
	m_snapshot.heldBlasBytes += blasBytes;
	m_snapshot.acceptedJobs++;
	if (bulk)
	{
		m_bulkActiveJobs++;
		m_bulkHeldBytes += totalBytes;
		m_bulkHeldBlasBytes += blasBytes;
	}
	return { NRIVoxelAdmissionResultCode::Accepted, tokenId };
}

void NRIVoxelAdmissionScheduler::CountStageTransition(NRIVoxelAdmissionStage from, NRIVoxelAdmissionStage to)
{
	m_snapshot.stageCounts[(size_t)from]--;
	m_snapshot.stageCounts[(size_t)to]++;
}

bool NRIVoxelAdmissionScheduler::Transition(Token& token, NRIVoxelAdmissionStage expected, NRIVoxelAdmissionStage next)
{
	if (token.stage != expected || IsTerminal(token.stage) || IsTerminal(next))
	{
		m_snapshot.invalidTransitions++;
		return false;
	}
	CountStageTransition(token.stage, next);
	token.stage = next;
	return true;
}

bool NRIVoxelAdmissionScheduler::MarkDependenciesReady(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	if (token == nullptr || !Transition(*token, NRIVoxelAdmissionStage::DependencyPending, NRIVoxelAdmissionStage::ComputeQueued))
	{
		return false;
	}
	token->sourceReady = true;
	m_computeQueue.push_back(tokenId);
	return true;
}

bool NRIVoxelAdmissionScheduler::CanAcquire(const Token& token, bool compute) const
{
	const uint32_t inFlight = compute ? m_snapshot.computeInFlight : m_snapshot.blasInFlight;
	const uint32_t otherInFlight = compute ? m_snapshot.blasInFlight : m_snapshot.computeInFlight;
	const uint32_t maximum = compute ? m_limits.maxComputeSlots : m_limits.maxBlasLanes;
	const uint64_t oversizedToken = compute ? m_oversizedComputeToken : m_oversizedBlasToken;
	const uint64_t otherOversizedToken = compute ? m_oversizedBlasToken : m_oversizedComputeToken;
	if (maximum == 0 || inFlight >= maximum || oversizedToken != 0 || otherOversizedToken != 0)
	{
		return false;
	}
	if (token.oversizedExclusive && (inFlight != 0 || otherInFlight != 0))
	{
		return false;
	}
	if (!token.oversizedExclusive && HasQueuedExclusiveToken())
	{
		return false;
	}
	if (IsBulk(token.fairnessClass))
	{
		const uint32_t reserve = compute ? m_limits.optionalComputeSlotReserve : m_limits.optionalBlasLaneReserve;
		const uint32_t bulkInFlight = compute ? m_bulkComputeInFlight : m_bulkBlasInFlight;
		const uint32_t bulkLimit = maximum > reserve ? maximum - reserve : 0;
		if (bulkInFlight >= bulkLimit)
		{
			return false;
		}
	}
	return true;
}

bool NRIVoxelAdmissionScheduler::HasQueuedExclusiveToken() const
{
	// Every queued token still owns its mesh. The live owner registry is
	// bounded by maxActiveJobs, even after a long history of cancelled jobs.
	for (const auto& owner : m_meshOwners)
	{
		const Token* token = FindToken(owner.second);
		if (token && token->oversizedExclusive &&
			(token->stage == NRIVoxelAdmissionStage::ComputeQueued || token->stage == NRIVoxelAdmissionStage::BlasQueued))
		{
			return true;
		}
	}
	return false;
}

NRIVoxelAdmissionScheduler::Token* NRIVoxelAdmissionScheduler::SelectQueuedToken(std::deque<uint64_t>& queue, bool compute)
{
	const NRIVoxelAdmissionStage queuedStage = compute ? NRIVoxelAdmissionStage::ComputeQueued : NRIVoxelAdmissionStage::BlasQueued;
	Token* selected = nullptr;
	size_t selectedIndex = 0;
	for (size_t index = 0; index < queue.size();)
	{
		Token* token = FindToken(queue[index]);
		if (token == nullptr || token->stage != queuedStage)
		{
			queue.erase(queue.begin() + index);
			continue;
		}
		if (CanAcquire(*token, compute) && (selected == nullptr ||
			(uint8_t)token->fairnessClass < (uint8_t)selected->fairnessClass ||
			(token->fairnessClass == selected->fairnessClass &&
				(token->age > selected->age || (token->age == selected->age && token->sequence < selected->sequence)))))
		{
			selected = token;
			selectedIndex = index;
		}
		index++;
	}
	if (selected != nullptr)
	{
		queue.erase(queue.begin() + selectedIndex);
	}
	return selected;
}

bool NRIVoxelAdmissionScheduler::AcquireNextCompute(uint32_t computeSlot, uint64_t& tokenId)
{
	tokenId = 0;
	if (computeSlot >= m_limits.maxComputeSlots || m_computeSlots.find(computeSlot) != m_computeSlots.end())
	{
		m_snapshot.invalidTransitions++;
		return false;
	}
	Token* token = SelectQueuedToken(m_computeQueue, true);
	if (token == nullptr || !Transition(*token, NRIVoxelAdmissionStage::ComputeQueued, NRIVoxelAdmissionStage::ComputeSubmitted))
	{
		return false;
	}
	token->computeSlot = computeSlot;
	m_computeSlots.emplace(computeSlot, token->tokenId);
	m_snapshot.computeInFlight++;
	if (IsBulk(token->fairnessClass))
	{
		m_bulkComputeInFlight++;
	}
	if (token->oversizedExclusive)
	{
		m_oversizedComputeToken = token->tokenId;
	}
	tokenId = token->tokenId;
	return true;
}

bool NRIVoxelAdmissionScheduler::SubmitCompute(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	if (token == nullptr)
	{
		return false;
	}
	if (token->stage == NRIVoxelAdmissionStage::DependencyPending && !MarkDependenciesReady(tokenId))
	{
		return false;
	}
	if (token->stage != NRIVoxelAdmissionStage::ComputeQueued || !CanAcquire(*token, true))
	{
		return false;
	}
	uint32_t slot = 0;
	while (slot < m_limits.maxComputeSlots && m_computeSlots.find(slot) != m_computeSlots.end())
	{
		slot++;
	}
	if (slot >= m_limits.maxComputeSlots || !Transition(*token, NRIVoxelAdmissionStage::ComputeQueued, NRIVoxelAdmissionStage::ComputeSubmitted))
	{
		return false;
	}
	token->computeSlot = slot;
	m_computeSlots.emplace(slot, tokenId);
	m_snapshot.computeInFlight++;
	if (IsBulk(token->fairnessClass))
	{
		m_bulkComputeInFlight++;
	}
	if (token->oversizedExclusive)
	{
		m_oversizedComputeToken = tokenId;
	}
	return true;
}

bool NRIVoxelAdmissionScheduler::MarkComputeReady(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	if (token == nullptr || token->stage != NRIVoxelAdmissionStage::ComputeSubmitted ||
		m_computeSlots.find(token->computeSlot) == m_computeSlots.end())
	{
		m_snapshot.invalidTransitions++;
		return false;
	}
	if (!Transition(*token, NRIVoxelAdmissionStage::ComputeSubmitted, NRIVoxelAdmissionStage::ComputeReady))
	{
		return false;
	}
	m_computeSlots.erase(token->computeSlot);
	token->computeSlot = NRI_VOXEL_ADMISSION_INVALID_SLOT;
	m_snapshot.computeInFlight--;
	if (IsBulk(token->fairnessClass))
	{
		m_bulkComputeInFlight--;
	}
	if (m_oversizedComputeToken == tokenId)
	{
		m_oversizedComputeToken = 0;
	}
	return true;
}

bool NRIVoxelAdmissionScheduler::QueueBlas(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	if (token == nullptr || !Transition(*token, NRIVoxelAdmissionStage::ComputeReady, NRIVoxelAdmissionStage::BlasQueued))
	{
		return false;
	}
	m_blasQueue.push_back(tokenId);
	return true;
}

bool NRIVoxelAdmissionScheduler::AcquireNextBlas(uint32_t blasLane, uint64_t& tokenId)
{
	tokenId = 0;
	if (blasLane >= m_limits.maxBlasLanes || m_blasLanes.find(blasLane) != m_blasLanes.end())
	{
		m_snapshot.invalidTransitions++;
		return false;
	}
	Token* token = SelectQueuedToken(m_blasQueue, false);
	if (token == nullptr || !Transition(*token, NRIVoxelAdmissionStage::BlasQueued, NRIVoxelAdmissionStage::BlasSubmitted))
	{
		return false;
	}
	token->blasLane = blasLane;
	m_blasLanes.emplace(blasLane, token->tokenId);
	m_snapshot.blasInFlight++;
	if (IsBulk(token->fairnessClass))
	{
		m_bulkBlasInFlight++;
	}
	if (token->oversizedExclusive)
	{
		m_oversizedBlasToken = token->tokenId;
	}
	tokenId = token->tokenId;
	return true;
}

bool NRIVoxelAdmissionScheduler::SubmitBlas(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	if (token == nullptr)
	{
		return false;
	}
	if (token->stage == NRIVoxelAdmissionStage::ComputeReady && !QueueBlas(tokenId))
	{
		return false;
	}
	if (token->stage != NRIVoxelAdmissionStage::BlasQueued || !CanAcquire(*token, false))
	{
		return false;
	}
	uint32_t lane = 0;
	while (lane < m_limits.maxBlasLanes && m_blasLanes.find(lane) != m_blasLanes.end())
	{
		lane++;
	}
	if (lane >= m_limits.maxBlasLanes || !Transition(*token, NRIVoxelAdmissionStage::BlasQueued, NRIVoxelAdmissionStage::BlasSubmitted))
	{
		return false;
	}
	token->blasLane = lane;
	m_blasLanes.emplace(lane, tokenId);
	m_snapshot.blasInFlight++;
	if (IsBulk(token->fairnessClass))
	{
		m_bulkBlasInFlight++;
	}
	if (token->oversizedExclusive)
	{
		m_oversizedBlasToken = tokenId;
	}
	return true;
}

bool NRIVoxelAdmissionScheduler::MarkBlasReady(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	if (token == nullptr || token->stage != NRIVoxelAdmissionStage::BlasSubmitted ||
		m_blasLanes.find(token->blasLane) == m_blasLanes.end())
	{
		m_snapshot.invalidTransitions++;
		return false;
	}
	if (!Transition(*token, NRIVoxelAdmissionStage::BlasSubmitted, NRIVoxelAdmissionStage::PublicationPending))
	{
		return false;
	}
	m_blasLanes.erase(token->blasLane);
	token->blasLane = NRI_VOXEL_ADMISSION_INVALID_SLOT;
	m_snapshot.blasInFlight--;
	if (IsBulk(token->fairnessClass))
	{
		m_bulkBlasInFlight--;
	}
	if (m_oversizedBlasToken == tokenId)
	{
		m_oversizedBlasToken = 0;
	}
	return true;
}

bool NRIVoxelAdmissionScheduler::MarkPublished(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	if (token == nullptr || !Transition(*token, NRIVoxelAdmissionStage::PublicationPending, NRIVoxelAdmissionStage::TlasPending))
	{
		return false;
	}
	m_tlasPending[token->mesh] = token->tokenId;
	ReleaseAdmissionOwnership(*token, true);
	return true;
}

void NRIVoxelAdmissionScheduler::RemoveOwners(const Token& token)
{
	auto meshOwner = m_meshOwners.find(token.mesh);
	if (meshOwner != m_meshOwners.end() && meshOwner->second == token.tokenId)
	{
		m_meshOwners.erase(meshOwner);
	}
	for (const NRIVoxelAdmissionBindingSnapshot& binding : token.bindings)
	{
		const NRIVoxelAdmissionPairIdentity identity = { token.mesh.mapGeneration, binding.pairKey };
		auto pairOwner = m_pairOwners.find(identity);
		if (pairOwner != m_pairOwners.end() && pairOwner->second == token.tokenId)
		{
			m_pairOwners.erase(pairOwner);
		}
	}
}

void NRIVoxelAdmissionScheduler::ReleaseReservation(Token& token)
{
	if (token.reservationState == NRIVoxelAdmissionReservationState::Released)
	{
		return;
	}
	const uint64_t totalBytes = token.reservation.TotalBytes();
	const uint64_t blasBytes = token.reservation.BlasReservationBytes();
	if (token.reservationState == NRIVoxelAdmissionReservationState::RetirePending)
	{
		m_snapshot.retirePendingBytes -= totalBytes;
		m_snapshot.retirePendingBlasBytes -= blasBytes;
	}
	else if (token.reservationState == NRIVoxelAdmissionReservationState::Abandoned)
	{
		m_snapshot.abandonedBytes -= totalBytes;
		m_snapshot.abandonedBlasBytes -= blasBytes;
	}
	m_snapshot.heldReservationBytes -= totalBytes;
	m_snapshot.heldBlasBytes -= blasBytes;
	if (IsBulk(token.fairnessClass))
	{
		m_bulkHeldBytes -= totalBytes;
		m_bulkHeldBlasBytes -= blasBytes;
	}
	token.reservationState = NRIVoxelAdmissionReservationState::Released;
}

void NRIVoxelAdmissionScheduler::ReleaseAdmissionOwnership(Token& token, bool releaseReservation)
{
	RemoveOwners(token);
	const uint64_t totalBytes = token.reservation.TotalBytes();
	const uint64_t blasBytes = token.reservation.BlasReservationBytes();
	m_snapshot.activeJobs--;
	m_snapshot.activeBindings -= (uint32_t)token.bindings.size();
	m_snapshot.activeFairnessCounts[(size_t)token.fairnessClass]--;
	m_snapshot.activeReservationBytes -= totalBytes;
	m_snapshot.activeBlasBytes -= blasBytes;
	if (IsBulk(token.fairnessClass))
	{
		m_bulkActiveJobs--;
	}
	if (releaseReservation)
	{
		ReleaseReservation(token);
	}
}

bool NRIVoxelAdmissionScheduler::Terminalize(Token& token, NRIVoxelAdmissionStage terminalStage)
{
	if (IsTerminal(token.stage) || !IsTerminal(terminalStage))
	{
		m_snapshot.invalidTransitions++;
		return false;
	}
	const bool fenceOwned = token.stage == NRIVoxelAdmissionStage::ComputeSubmitted ||
		token.stage == NRIVoxelAdmissionStage::BlasSubmitted;
	const bool admissionActive = IsAdmissionActive(token.stage);
	if (token.stage == NRIVoxelAdmissionStage::TlasPending)
	{
		m_tlasPending.erase(token.mesh);
	}
	if (token.stage == NRIVoxelAdmissionStage::ComputeSubmitted)
	{
		m_computeSlots.erase(token.computeSlot);
		token.computeSlot = NRI_VOXEL_ADMISSION_INVALID_SLOT;
		m_snapshot.computeInFlight--;
		if (IsBulk(token.fairnessClass))
		{
			m_bulkComputeInFlight--;
		}
		if (m_oversizedComputeToken == token.tokenId)
		{
			m_oversizedComputeToken = 0;
		}
	}
	if (token.stage == NRIVoxelAdmissionStage::BlasSubmitted)
	{
		m_blasLanes.erase(token.blasLane);
		token.blasLane = NRI_VOXEL_ADMISSION_INVALID_SLOT;
		m_snapshot.blasInFlight--;
		if (IsBulk(token.fairnessClass))
		{
			m_bulkBlasInFlight--;
		}
		if (m_oversizedBlasToken == token.tokenId)
		{
			m_oversizedBlasToken = 0;
		}
	}

	CountStageTransition(token.stage, terminalStage);
	token.stage = terminalStage;
	const uint64_t totalBytes = token.reservation.TotalBytes();
	const uint64_t blasBytes = token.reservation.BlasReservationBytes();
	if (admissionActive)
	{
		ReleaseAdmissionOwnership(token, !fenceOwned);
	}
	if (fenceOwned)
	{
		token.reservationState = NRIVoxelAdmissionReservationState::RetirePending;
		m_snapshot.retirePendingBytes += totalBytes;
		m_snapshot.retirePendingBlasBytes += blasBytes;
	}
	else if (token.reservationState != NRIVoxelAdmissionReservationState::Released)
	{
		ReleaseReservation(token);
	}
	return true;
}

bool NRIVoxelAdmissionScheduler::MarkTlasReady(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	if (token == nullptr || token->stage != NRIVoxelAdmissionStage::TlasPending)
	{
		m_snapshot.invalidTransitions++;
		return false;
	}
	m_tlasPending.erase(token->mesh);
	CountStageTransition(token->stage, NRIVoxelAdmissionStage::Ready);
	token->stage = NRIVoxelAdmissionStage::Ready;
	return true;
}

bool NRIVoxelAdmissionScheduler::Cancel(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	return token != nullptr && Terminalize(*token, NRIVoxelAdmissionStage::Cancelled);
}

bool NRIVoxelAdmissionScheduler::WithdrawUnsubmittedPreflight(uint64_t tokenId, uint64_t pairKey)
{
	// Admit appended this token in this same call. A later queued job,
	// shared material owner, or any submitted work needs its normal lifetime.
	if (m_computeQueue.empty() || m_computeQueue.back() != tokenId) return false;
	auto found = m_tokens.find(tokenId);
	if (found == m_tokens.end()) return false;
	Token& token = found->second;
	if (token.stage != NRIVoxelAdmissionStage::ComputeQueued ||
		token.reservationState != NRIVoxelAdmissionReservationState::Active ||
		token.bindings.size() != 1 || token.bindings.front().pairKey != pairKey)
		return false;
	ReleaseAdmissionOwnership(token, true);
	--m_snapshot.stageCounts[(size_t)NRIVoxelAdmissionStage::ComputeQueued];
	++m_snapshot.withdrawnPreflights;
	m_computeQueue.pop_back();
	m_tokens.erase(found);
	return true;
}

bool NRIVoxelAdmissionScheduler::Fail(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	return token != nullptr && Terminalize(*token, NRIVoxelAdmissionStage::Failed);
}

bool NRIVoxelAdmissionScheduler::RetireTerminalReservation(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	if (token == nullptr || !IsTerminal(token->stage) ||
		(token->reservationState != NRIVoxelAdmissionReservationState::RetirePending &&
			token->reservationState != NRIVoxelAdmissionReservationState::Abandoned))
	{
		m_snapshot.invalidTransitions++;
		return false;
	}
	ReleaseReservation(*token);
	return true;
}

bool NRIVoxelAdmissionScheduler::AbandonTerminalReservation(uint64_t tokenId)
{
	Token* token = FindToken(tokenId);
	if (token == nullptr || !IsTerminal(token->stage) ||
		token->reservationState != NRIVoxelAdmissionReservationState::RetirePending)
	{
		m_snapshot.invalidTransitions++;
		return false;
	}
	const uint64_t totalBytes = token->reservation.TotalBytes();
	const uint64_t blasBytes = token->reservation.BlasReservationBytes();
	m_snapshot.retirePendingBytes -= totalBytes;
	m_snapshot.retirePendingBlasBytes -= blasBytes;
	m_snapshot.abandonedBytes += totalBytes;
	m_snapshot.abandonedBlasBytes += blasBytes;
	token->reservationState = NRIVoxelAdmissionReservationState::Abandoned;
	return true;
}

uint32_t NRIVoxelAdmissionScheduler::SetActiveMapGeneration(uint64_t mapGeneration)
{
	m_limits.activeMapGeneration = mapGeneration;
	m_snapshot.activeMapGeneration = mapGeneration;
	std::vector<uint64_t> staleTokens;
	for (const auto& entry : m_tokens)
	{
		if (!IsTerminal(entry.second.stage) && entry.second.mesh.mapGeneration != mapGeneration)
		{
			staleTokens.push_back(entry.first);
		}
	}
	for (uint64_t tokenId : staleTokens)
	{
		Token* token = FindToken(tokenId);
		Terminalize(*token, NRIVoxelAdmissionStage::Stale);
	}
	return (uint32_t)staleTokens.size();
}

void NRIVoxelAdmissionScheduler::Reset(uint64_t activeMapGeneration)
{
	m_limits.activeMapGeneration = activeMapGeneration;
	m_tokens.clear();
	m_meshOwners.clear();
	m_tlasPending.clear();
	m_pairOwners.clear();
	m_computeSlots.clear();
	m_blasLanes.clear();
	m_computeQueue.clear();
	m_blasQueue.clear();
	m_snapshot = {};
	m_snapshot.activeMapGeneration = activeMapGeneration;
	m_nextTokenId = 1;
	m_nextSequence = 1;
	m_bulkActiveJobs = 0;
	m_bulkComputeInFlight = 0;
	m_bulkBlasInFlight = 0;
	m_bulkHeldBytes = 0;
	m_bulkHeldBlasBytes = 0;
	m_oversizedComputeToken = 0;
	m_oversizedBlasToken = 0;
}

uint64_t NRIVoxelAdmissionScheduler::FindMeshOwner(const NRIVoxelAdmissionMeshIdentity& identity) const
{
	auto found = m_meshOwners.find(identity);
	return found == m_meshOwners.end() ? 0 : found->second;
}

uint64_t NRIVoxelAdmissionScheduler::FindTlasPending(const NRIVoxelAdmissionMeshIdentity& identity) const
{
	auto found = m_tlasPending.find(identity);
	return found == m_tlasPending.end() ? 0 : found->second;
}

uint64_t NRIVoxelAdmissionScheduler::FindPairOwner(const NRIVoxelAdmissionPairIdentity& identity) const
{
	auto found = m_pairOwners.find(identity);
	return found == m_pairOwners.end() ? 0 : found->second;
}

bool NRIVoxelAdmissionScheduler::IsDirectBlasLaneBlocked(uint64_t tokenId, bool exclusive) const
{
	// Submission/ready/cancellation transitions maintain these live owners.
	// Preserve the admission poll's older-exclusive-compute ordering and its
	// self exception for an already-owned exclusive BLAS lane.
	return (exclusive && m_snapshot.blasInFlight != 0) ||
		(exclusive && m_oversizedComputeToken != 0 && m_oversizedComputeToken < tokenId) ||
		(m_oversizedBlasToken != 0 && m_oversizedBlasToken != tokenId);
}

NRIVoxelAdmissionScheduler::Token* NRIVoxelAdmissionScheduler::FindToken(uint64_t tokenId)
{
	auto found = m_tokens.find(tokenId);
	return found == m_tokens.end() ? nullptr : &found->second;
}

const NRIVoxelAdmissionScheduler::Token* NRIVoxelAdmissionScheduler::FindToken(uint64_t tokenId) const
{
	auto found = m_tokens.find(tokenId);
	return found == m_tokens.end() ? nullptr : &found->second;
}

void NRIVoxelAdmissionScheduler::FillTokenSnapshot(const Token& token, NRIVoxelAdmissionTokenSnapshot& snapshot) const
{
	snapshot.tokenId = token.tokenId;
	snapshot.mesh = token.mesh;
	snapshot.reservation = token.reservation;
	snapshot.fairnessClass = token.fairnessClass;
	snapshot.stage = token.stage;
	snapshot.reservationState = token.reservationState;
	snapshot.age = token.age;
	snapshot.sequence = token.sequence;
	snapshot.computeSlot = token.computeSlot;
	snapshot.blasLane = token.blasLane;
	snapshot.sourceReady = token.sourceReady;
	snapshot.oversizedExclusive = token.oversizedExclusive;
	snapshot.bindings = token.bindings;
}

bool NRIVoxelAdmissionScheduler::GetTokenSnapshot(uint64_t tokenId, NRIVoxelAdmissionTokenSnapshot& snapshot) const
{
	const Token* token = FindToken(tokenId);
	if (token == nullptr)
	{
		return false;
	}
	FillTokenSnapshot(*token, snapshot);
	return true;
}

std::vector<NRIVoxelAdmissionTokenSnapshot> NRIVoxelAdmissionScheduler::GetTokenSnapshots() const
{
	std::vector<NRIVoxelAdmissionTokenSnapshot> snapshots;
	snapshots.reserve(m_tokens.size());
	for (const auto& entry : m_tokens)
	{
		snapshots.push_back({});
		FillTokenSnapshot(entry.second, snapshots.back());
	}
	std::sort(snapshots.begin(), snapshots.end(), [](const auto& a, const auto& b)
	{
		return a.tokenId < b.tokenId;
	});
	return snapshots;
}

NRIVoxelAdmissionSnapshot NRIVoxelAdmissionScheduler::GetSnapshot() const
{
	NRIVoxelAdmissionSnapshot snapshot = m_snapshot;
	snapshot.tokenRecords = m_tokens.size();
	snapshot.computeQueueEntries = m_computeQueue.size();
	snapshot.blasQueueEntries = m_blasQueue.size();
	snapshot.meshOwnerCount = (uint32_t)m_meshOwners.size();
	snapshot.pairOwnerCount = (uint32_t)m_pairOwners.size();
	return snapshot;
}

NRIVoxelAdmissionInvariantReport NRIVoxelAdmissionScheduler::ValidateInvariants() const
{
	NRIVoxelAdmissionInvariantReport report = {};
	auto violate = [&report](NRIVoxelAdmissionInvariant flag)
	{
		report.valid = false;
		report.violations++;
		report.flags = (NRIVoxelAdmissionInvariant)((uint32_t)report.flags | (uint32_t)flag);
	};

	NRIVoxelAdmissionSnapshot derived = {};
	uint32_t derivedBulkActive = 0;
	uint32_t derivedBulkCompute = 0;
	uint32_t derivedBulkBlas = 0;
	uint64_t derivedBulkHeldBytes = 0;
	uint64_t derivedBulkHeldBlasBytes = 0;
	uint64_t derivedOversizedCompute = 0;
	uint64_t derivedOversizedBlas = 0;
	for (const auto& entry : m_tokens)
	{
		const Token& token = entry.second;
		derived.stageCounts[(size_t)token.stage]++;
		const uint64_t totalBytes = token.reservation.TotalBytes();
		const uint64_t blasBytes = token.reservation.BlasReservationBytes();
		const bool admissionActive = IsAdmissionActive(token.stage);
		if (admissionActive)
		{
			derived.activeJobs++;
			derived.activeBindings += (uint32_t)token.bindings.size();
			derived.activeFairnessCounts[(size_t)token.fairnessClass]++;
			derived.activeReservationBytes += totalBytes;
			derived.activeBlasBytes += blasBytes;
			if (token.reservationState != NRIVoxelAdmissionReservationState::Active ||
				token.mesh.mapGeneration != m_limits.activeMapGeneration)
			{
				violate(NRIVoxelAdmissionInvariant::TokenState);
			}
			if (FindMeshOwner(token.mesh) != token.tokenId)
			{
				violate(NRIVoxelAdmissionInvariant::OwnerRegistries);
			}
			if (IsBulk(token.fairnessClass))
			{
				derivedBulkActive++;
			}
		}
		else if (token.reservationState == NRIVoxelAdmissionReservationState::Active)
		{
			violate(NRIVoxelAdmissionInvariant::TokenState);
		}
		if (token.stage == NRIVoxelAdmissionStage::TlasPending && FindTlasPending(token.mesh) != token.tokenId)
		{
			violate(NRIVoxelAdmissionInvariant::OwnerRegistries);
		}

		for (const NRIVoxelAdmissionBindingSnapshot& binding : token.bindings)
		{
			const uint64_t owner = FindPairOwner({ token.mesh.mapGeneration, binding.pairKey });
			if ((admissionActive && owner != token.tokenId) || (!admissionActive && owner != 0))
			{
				violate(NRIVoxelAdmissionInvariant::OwnerRegistries);
			}
		}

		if (token.reservationState != NRIVoxelAdmissionReservationState::Released)
		{
			derived.heldReservationBytes += totalBytes;
			derived.heldBlasBytes += blasBytes;
			if (IsBulk(token.fairnessClass))
			{
				derivedBulkHeldBytes += totalBytes;
				derivedBulkHeldBlasBytes += blasBytes;
			}
		}
		if (token.reservationState == NRIVoxelAdmissionReservationState::RetirePending)
		{
			derived.retirePendingBytes += totalBytes;
			derived.retirePendingBlasBytes += blasBytes;
		}
		if (token.reservationState == NRIVoxelAdmissionReservationState::Abandoned)
		{
			derived.abandonedBytes += totalBytes;
			derived.abandonedBlasBytes += blasBytes;
		}
		if (token.stage == NRIVoxelAdmissionStage::ComputeSubmitted)
		{
			derived.computeInFlight++;
			if (IsBulk(token.fairnessClass))
			{
				derivedBulkCompute++;
			}
			if (token.oversizedExclusive)
			{
				derivedOversizedCompute = token.tokenId;
			}
			auto slot = m_computeSlots.find(token.computeSlot);
			if (slot == m_computeSlots.end() || slot->second != token.tokenId)
			{
				violate(NRIVoxelAdmissionInvariant::SlotRegistries);
			}
		}
		else if (token.computeSlot != NRI_VOXEL_ADMISSION_INVALID_SLOT)
		{
			violate(NRIVoxelAdmissionInvariant::TokenState);
		}
		if (token.stage == NRIVoxelAdmissionStage::BlasSubmitted)
		{
			derived.blasInFlight++;
			if (IsBulk(token.fairnessClass))
			{
				derivedBulkBlas++;
			}
			if (token.oversizedExclusive)
			{
				derivedOversizedBlas = token.tokenId;
			}
			auto lane = m_blasLanes.find(token.blasLane);
			if (lane == m_blasLanes.end() || lane->second != token.tokenId)
			{
				violate(NRIVoxelAdmissionInvariant::SlotRegistries);
			}
		}
		else if (token.blasLane != NRI_VOXEL_ADMISSION_INVALID_SLOT)
		{
			violate(NRIVoxelAdmissionInvariant::TokenState);
		}
	}

	if (derived.stageCounts != m_snapshot.stageCounts)
	{
		violate(NRIVoxelAdmissionInvariant::StageCounts);
	}
	if (derived.activeJobs != m_snapshot.activeJobs || derived.activeBindings != m_snapshot.activeBindings ||
		derived.activeFairnessCounts != m_snapshot.activeFairnessCounts || derivedBulkActive != m_bulkActiveJobs)
	{
		violate(NRIVoxelAdmissionInvariant::ActiveCounts);
	}
	if (derived.activeReservationBytes != m_snapshot.activeReservationBytes ||
		derived.activeBlasBytes != m_snapshot.activeBlasBytes ||
		derived.heldReservationBytes != m_snapshot.heldReservationBytes ||
		derived.heldBlasBytes != m_snapshot.heldBlasBytes ||
		derived.retirePendingBytes != m_snapshot.retirePendingBytes ||
		derived.retirePendingBlasBytes != m_snapshot.retirePendingBlasBytes ||
		derived.abandonedBytes != m_snapshot.abandonedBytes ||
		derived.abandonedBlasBytes != m_snapshot.abandonedBlasBytes ||
		derivedBulkHeldBytes != m_bulkHeldBytes || derivedBulkHeldBlasBytes != m_bulkHeldBlasBytes)
	{
		violate(NRIVoxelAdmissionInvariant::ReservationBytes);
	}
	if (derived.computeInFlight != m_snapshot.computeInFlight || derived.blasInFlight != m_snapshot.blasInFlight ||
		derivedBulkCompute != m_bulkComputeInFlight || derivedBulkBlas != m_bulkBlasInFlight ||
		derivedOversizedCompute != m_oversizedComputeToken || derivedOversizedBlas != m_oversizedBlasToken ||
		m_computeSlots.size() != derived.computeInFlight || m_blasLanes.size() != derived.blasInFlight)
	{
		violate(NRIVoxelAdmissionInvariant::SlotRegistries);
	}
	if (m_meshOwners.size() != derived.activeJobs || m_pairOwners.size() != derived.activeBindings ||
		m_tlasPending.size() != derived.stageCounts[(size_t)NRIVoxelAdmissionStage::TlasPending])
	{
		violate(NRIVoxelAdmissionInvariant::OwnerRegistries);
	}
	if (m_snapshot.activeJobs > m_limits.maxActiveJobs || m_snapshot.heldReservationBytes > m_limits.maxReservedBytes ||
		m_snapshot.heldBlasBytes > m_limits.maxReservedBlasBytes || m_snapshot.computeInFlight > m_limits.maxComputeSlots ||
		m_snapshot.blasInFlight > m_limits.maxBlasLanes)
	{
		violate(NRIVoxelAdmissionInvariant::Limits);
	}
	return report;
}
