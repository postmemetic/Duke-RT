#include "nri_persistent_voxel_material_upload.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAILED: " << message << '\n';
		std::exit(1);
	}
}

void ApplyPlan(
	std::vector<uint8_t>& target,
	const NRIPersistentVoxelMaterialUploadMirror& mirror,
	const NRIPersistentVoxelMaterialUploadPlan& plan)
{
	for (const auto& range : plan.ranges)
	{
		std::memcpy(
			target.data() + (size_t)range.byteOffset,
			mirror.Data() + (size_t)range.byteOffset,
			(size_t)range.size);
	}
}

void TestCrossGapMergePreservesInactiveBytes()
{
	NRIPersistentVoxelMaterialUploadMirror mirror;
	NRIPersistentVoxelMaterialUploadPlan plan;
	const std::vector<uint8_t> initial = {
		0x11, 0x12, 0x13, 0x14,
		0x21, 0x22, 0x23, 0x24,
		0x31, 0x32, 0x33, 0x34 };
	Require(mirror.Prepare(initial.size(), initial.data(), initial.size(), { { 0, initial.size(), initial.size() } }, plan),
		"initial arena upload can be prepared");
	mirror.Commit();

	std::vector<uint8_t> active = {
		0xa1, 0xa2, 0xa3, 0xa4,
		0x00, 0x00, 0x00, 0x00,
		0xc1, 0xc2, 0xc3, 0xc4 };
	Require(mirror.Prepare(active.size(), active.data(), active.size(), { { 0, 4, 4 }, { 8, 4, 4 } }, plan),
		"separated active ranges can be prepared from the arena mirror");
	Require(plan.ranges.size() == 1 && plan.ranges[0].byteOffset == 0 &&
		plan.ranges[0].size == 12 && plan.ranges[0].dirtySize == 8,
		"bounded active ranges coalesce across one retained inactive row");

	std::vector<uint8_t> gpu = initial;
	ApplyPlan(gpu, mirror, plan);
	Require(std::memcmp(gpu.data(), active.data(), 4) == 0,
		"first dirty range reaches the simulated GPU arena");
	Require(std::memcmp(gpu.data() + 4, initial.data() + 4, 4) == 0,
		"inactive gap bytes remain byte-identical to their last upload");
	Require(std::memcmp(gpu.data() + 8, active.data() + 8, 4) == 0,
		"second dirty range reaches the simulated GPU arena");
	mirror.Commit();
}

void TestCoalescingBudgets()
{
	NRIPersistentVoxelMaterialUploadMirror mirror;
	NRIPersistentVoxelMaterialUploadPlan plan;
	std::vector<uint8_t> data(13000, 0x5a);

	Require(mirror.Prepare(data.size(), data.data(), data.size(), { { 0, 4097, 4097 }, { 8193, 4097, 4097 } }, plan),
		"maximum gap plan can be prepared");
	Require(plan.ranges.size() == 1,
		"a gap of exactly 4 KiB remains mergeable");
	mirror.Commit();

	Require(mirror.Prepare(data.size(), data.data(), data.size(), { { 0, 4097, 4097 }, { 8194, 4097, 4097 } }, plan),
		"oversized gap plan can be prepared");
	Require(plan.ranges.size() == 2 && plan.rejectedMerges == 1,
		"a gap larger than 4 KiB remains two copy ranges");
	mirror.Commit();

	Require(mirror.Prepare(data.size(), data.data(), data.size(), { { 0, 2, 2 }, { 6, 2, 2 } }, plan),
		"maximum expansion plan can be prepared");
	Require(plan.ranges.size() == 1,
		"a span of exactly twice its dirty bytes remains mergeable");
	mirror.Commit();

	Require(mirror.Prepare(data.size(), data.data(), data.size(), { { 0, 2, 2 }, { 7, 2, 2 } }, plan),
		"excessive expansion plan can be prepared");
	Require(plan.ranges.size() == 2 && plan.rejectedMerges == 1,
		"a span larger than twice its dirty bytes remains two copy ranges");
	mirror.Commit();

	Require(mirror.Prepare(data.size(), data.data(), data.size(), { { 10, 2, 2 }, { 12, 3, 3 } }, plan),
		"adjacent range plan can be prepared");
	Require(plan.ranges.size() == 1 && plan.ranges[0].size == 5 && plan.ranges[0].dirtySize == 5,
		"physically adjacent ranges always merge");
	mirror.Commit();

	Require(mirror.Prepare(data.size(), data.data(), data.size(), { { 20, 4, 4 }, { 26, 4, 4 }, { 32, 4, 4 } }, plan),
		"three-range chain can be prepared");
	Require(plan.ranges.size() == 1 && plan.ranges[0].size == 16 && plan.ranges[0].dirtySize == 12,
		"a bounded chain of animated variants coalesces transitively");
	mirror.Commit();
}

void TestFailedStageRollback()
{
	NRIPersistentVoxelMaterialUploadMirror mirror;
	NRIPersistentVoxelMaterialUploadPlan plan;
	const std::vector<uint8_t> initial = { 1, 2, 3, 4, 5, 6 };
	Require(mirror.Prepare(initial.size(), initial.data(), initial.size(), { { 0, initial.size(), initial.size() } }, plan),
		"rollback fixture can initialize the mirror");
	mirror.Commit();

	const std::vector<uint8_t> changed = { 9, 9, 3, 4, 8, 8, 7, 7 };
	Require(mirror.Prepare(changed.size(), changed.data(), changed.size(), { { 0, 2, 2 }, { 4, 4, 4 } }, plan),
		"rollback fixture can prepare dirty ranges and grow");
	mirror.Rollback();
	Require(mirror.Size() == initial.size() &&
		std::memcmp(mirror.Data(), initial.data(), initial.size()) == 0,
		"a failed stage restores prior bytes and prior arena size");
	Require(mirror.Prepare(changed.size(), changed.data(), changed.size(), { { 0, 2, 2 }, { 4, 4, 4 } }, plan),
		"a rolled-back update can be prepared again");
	mirror.Commit();
	Require(mirror.Size() == changed.size() && mirror.Data()[0] == 9 && mirror.Data()[6] == 7,
		"a retried update can commit after rollback");

	Require(!mirror.Prepare(changed.size(), changed.data(), changed.size(), { { 7, 2, 2 } }, plan),
		"out-of-bounds dirty ranges fail closed");
	Require(mirror.Size() == changed.size() && mirror.Data()[0] == 9 && mirror.Data()[6] == 7,
		"a rejected plan leaves the mirror unchanged");

	mirror.Reset();
	Require(mirror.Size() == 0 && mirror.Data() == nullptr,
		"reset drops all mirrored arena authority");
}

void TestInactiveTailRemainsAuthoritative()
{
	NRIPersistentVoxelMaterialUploadMirror mirror;
	NRIPersistentVoxelMaterialUploadPlan plan;
	const std::vector<uint8_t> initial = {
		1, 2, 3, 4, 5, 6, 7, 8,
		9, 10, 11, 12, 13, 14, 15, 16 };
	Require(mirror.Prepare(initial.size(), initial.data(), initial.size(), { { 0, initial.size(), initial.size() } }, plan),
		"inactive-tail fixture can initialize the mirror");
	mirror.Commit();

	const std::vector<uint8_t> active = { 21, 22, 23, 24, 0, 0, 0, 0, 31, 32, 33, 34 };
	Require(mirror.Prepare(initial.size(), active.data(), active.size(), { { 0, 4, 4 }, { 8, 4, 4 } }, plan),
		"active coverage may be shorter than the retained arena");
	mirror.Commit();
	Require(mirror.Size() == initial.size() &&
		std::memcmp(mirror.Data() + 12, initial.data() + 12, 4) == 0,
		"inactive retained tail bytes survive an active-only update");
}

void TestManyRangeRollback()
{
	NRIPersistentVoxelMaterialUploadMirror mirror;
	NRIPersistentVoxelMaterialUploadPlan plan;
	std::vector<uint8_t> initial(256);
	for (size_t i = 0; i < initial.size(); i++)
	{
		initial[i] = (uint8_t)i;
	}
	Require(mirror.Prepare(initial.size(), initial.data(), initial.size(), { { 0, initial.size(), initial.size() } }, plan),
		"many-range fixture can initialize the mirror");
	mirror.Commit();

	std::vector<uint8_t> changed(initial.size(), 0xee);
	std::vector<NRIPersistentVoxelMaterialUploadRange> ranges;
	for (uint64_t offset = 0; offset < initial.size(); offset += 8)
	{
		ranges.push_back({ offset, 4, 4 });
	}
	Require(mirror.Prepare(initial.size(), changed.data(), changed.size(), ranges, plan),
		"many separated dirty ranges can share one rollback transaction");
	mirror.Rollback();
	Require(std::memcmp(mirror.Data(), initial.data(), initial.size()) == 0,
		"many-range rollback restores the complete authoritative mirror");
	Require(!mirror.Prepare(initial.size(), initial.data(), initial.size(), { { 0, 4, 3 } }, plan),
		"raw dirty ranges reject inconsistent dirty-byte accounting");
}
}

int main()
{
	TestCrossGapMergePreservesInactiveBytes();
	TestCoalescingBudgets();
	TestFailedStageRollback();
	TestInactiveTailRemainsAuthoritative();
	TestManyRangeRollback();
	std::cout << "Persistent voxel material upload tests passed.\n";
	return 0;
}
