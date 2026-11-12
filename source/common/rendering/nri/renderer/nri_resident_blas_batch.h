#pragma once

#include "NRI.h"
#include "Extensions/NRIRayTracing.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// Recording only: selected geometry, AS creation/update eligibility, publication
// and retirement remain with the resident owner. Nothing is kept across calls.
namespace nri_resident_blas_batch
{
inline constexpr size_t MaxJobs = 32;
inline constexpr uint64_t ExtraScratchBytes = 16ull * 1024 * 1024;

enum class Rejection : uint8_t { None, JobCount, InvalidInput, Alias, Overflow, Budget };
struct Job
{
	const void* dst = nullptr;
	const void* src = nullptr;
	uint64_t scratchBytes = 0;
};
struct Plan
{
	std::array<uint64_t, MaxJobs> offsets{};
	uint32_t jobCount = 0;
	uint64_t requiredBytes = 0;
	uint64_t singleScratchBytes = 0;
	uint64_t alignment = 0;
	Rejection rejection = Rejection::JobCount;
	bool Eligible() const { return rejection == Rejection::None; }
};

inline Plan TryPlan(const Job* jobs, size_t count, uint64_t alignment)
{
	Plan plan;
	if (count < 2 || count > MaxJobs) return plan;
	plan.jobCount = static_cast<uint32_t>(count);
	plan.alignment = alignment;
	if (jobs == nullptr || alignment == 0) { plan.rejection = Rejection::InvalidInput; return plan; }
	uint64_t end = 0;
	for (size_t i = 0; i < count; ++i)
	{
		const auto& job = jobs[i];
		// The current resident path updates in place. A different source would
		// need a wider dependency contract and therefore keeps the serial path.
		if (job.dst == nullptr || job.scratchBytes == 0 || (job.src != nullptr && job.src != job.dst))
		{ plan.rejection = Rejection::InvalidInput; return plan; }
		for (size_t j = 0; j < i; ++j)
			if (job.dst == jobs[j].dst || job.dst == jobs[j].src || job.src == jobs[j].dst)
			{ plan.rejection = Rejection::Alias; return plan; }
		const uint64_t remainder = end % alignment;
		const uint64_t padding = remainder == 0 ? 0 : alignment - remainder;
		if (padding > UINT64_MAX - end || job.scratchBytes > UINT64_MAX - end - padding)
		{ plan.rejection = Rejection::Overflow; return plan; }
		plan.offsets[i] = end + padding;
		end = plan.offsets[i] + job.scratchBytes;
		if (job.scratchBytes > plan.singleScratchBytes) plan.singleScratchBytes = job.scratchBytes;
	}
	plan.requiredBytes = end;
	// This is an incremental budget over the largest serial requirement, not
	// an absolute 16 MiB cap. Subtraction avoids overflowing that bound.
	if (end - plan.singleScratchBytes > ExtraScratchBytes)
	{ plan.rejection = Rejection::Budget; return plan; }
	plan.rejection = Rejection::None;
	return plan;
}

// Independent diagnostic check: do not call TryPlan or reuse its arithmetic.
// This proves the normalized recording contract, not GPU AS byte identity.
inline bool ValidatePlan(const Job* jobs, size_t count, const Plan& plan)
{
	if (!plan.Eligible() || jobs == nullptr || count < 2 || count > MaxJobs ||
		plan.jobCount != count || plan.alignment == 0) return false;
	uint64_t expectedEnd = 0, largest = 0;
	for (size_t i = 0; i < count; ++i)
	{
		const auto& job = jobs[i];
		if (job.dst == nullptr || job.scratchBytes == 0 || (job.src && job.src != job.dst)) return false;
		uint64_t expectedOffset = expectedEnd;
		if (expectedOffset % plan.alignment)
		{
			const uint64_t quotient = expectedOffset / plan.alignment;
			if (quotient >= UINT64_MAX / plan.alignment) return false;
			expectedOffset = (quotient + 1) * plan.alignment;
		}
		if (plan.offsets[i] != expectedOffset || expectedOffset > plan.requiredBytes ||
			job.scratchBytes > plan.requiredBytes - expectedOffset) return false;
		expectedEnd = expectedOffset + job.scratchBytes;
		if (job.scratchBytes > largest) largest = job.scratchBytes;
		for (size_t j = 0; j < count; ++j)
			if (i != j && (job.dst == jobs[j].dst || job.dst == jobs[j].src || job.src == jobs[j].dst)) return false;
	}
	return expectedEnd == plan.requiredBytes && largest == plan.singleScratchBytes &&
		expectedEnd >= largest && expectedEnd - largest <= ExtraScratchBytes;
}

inline bool SameGeometry(const nri::BottomLevelGeometryDesc& a, const nri::BottomLevelGeometryDesc& b)
{
	if (a.type != b.type || a.flags != b.flags || a.type != nri::BottomLevelGeometryType::TRIANGLES) return false;
	const auto& x = a.triangles;
	const auto& y = b.triangles;
	return x.vertexBuffer == y.vertexBuffer && x.vertexOffset == y.vertexOffset &&
		x.vertexNum == y.vertexNum && x.vertexStride == y.vertexStride && x.vertexFormat == y.vertexFormat &&
		x.indexBuffer == y.indexBuffer && x.indexOffset == y.indexOffset && x.indexNum == y.indexNum &&
		x.indexType == y.indexType && x.transformBuffer == y.transformBuffer && x.transformOffset == y.transformOffset &&
		x.micromap == nullptr && y.micromap == nullptr;
}

inline bool ValidateDescriptors(const Job* jobs, size_t count, const Plan& plan,
	const nri::BuildBottomLevelAccelerationStructureDesc* original,
	const nri::BuildBottomLevelAccelerationStructureDesc* candidate, uint64_t scratchCapacity)
{
	if (!ValidatePlan(jobs, count, plan) || !original || !candidate || plan.requiredBytes > scratchCapacity) return false;
	for (size_t i = 0; i < count; ++i)
	{
		const auto& a = original[i];
		const auto& b = candidate[i];
		if (a.dst != jobs[i].dst || a.src != jobs[i].src || a.scratchOffset != 0 ||
			a.dst != b.dst || a.src != b.src || a.geometryNum != 1 || b.geometryNum != 1 ||
			a.geometries == nullptr || b.geometries == nullptr || a.scratchBuffer == nullptr ||
			a.scratchBuffer != b.scratchBuffer || b.scratchOffset != plan.offsets[i] ||
			!SameGeometry(a.geometries[0], b.geometries[0])) return false;
	}
	return true;
}

// All referenced geometry and descriptors live through emit(). Completion is
// performed once per original job and in original order after the command.
// A failed reference check emits no GPU work; the caller can record serially.
template<class Emit, class Complete>
bool Record(const Job* jobs, size_t count, const Plan& plan,
	const nri::BuildBottomLevelAccelerationStructureDesc* original,
	uint64_t scratchCapacity, bool validate, Emit&& emit, Complete&& complete)
{
	if (!plan.Eligible() || !original || count != plan.jobCount || count < 2 || count > MaxJobs ||
		plan.requiredBytes > scratchCapacity) return false;
	std::array<nri::BuildBottomLevelAccelerationStructureDesc, MaxJobs> builds{};
	for (size_t i = 0; i < count; ++i)
	{
		builds[i] = original[i];
		builds[i].scratchOffset = plan.offsets[i];
	}
	if (validate && !ValidateDescriptors(jobs, count, plan, original, builds.data(), scratchCapacity)) return false;
	emit(builds.data(), static_cast<uint32_t>(count));
	for (size_t i = 0; i < count; ++i) complete(i);
	return true;
}
}
