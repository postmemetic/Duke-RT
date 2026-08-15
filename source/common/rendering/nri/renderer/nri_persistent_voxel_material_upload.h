#pragma once

#include <cstdint>
#include <vector>

struct NRIPersistentVoxelMaterialUploadRange
{
	uint64_t byteOffset = 0;
	uint64_t size = 0;
	uint64_t dirtySize = 0;
};

struct NRIPersistentVoxelMaterialUploadPlan
{
	std::vector<NRIPersistentVoxelMaterialUploadRange> ranges;
	uint32_t rejectedMerges = 0;
};

class NRIPersistentVoxelMaterialUploadMirror
{
public:
	bool Prepare(
		uint64_t arenaSize,
		const uint8_t* activeArenaData,
		uint64_t activeArenaSize,
		const std::vector<NRIPersistentVoxelMaterialUploadRange>& dirtyRanges,
		NRIPersistentVoxelMaterialUploadPlan& outPlan);
	void Commit();
	void Rollback();
	void Reset();

	const uint8_t* Data() const { return bytes.empty() ? nullptr : bytes.data(); }
	uint64_t Size() const { return bytes.size(); }

private:
	struct RollbackRange
	{
		uint64_t byteOffset = 0;
		uint64_t rollbackOffset = 0;
		uint64_t size = 0;
	};

	std::vector<uint8_t> bytes;
	std::vector<RollbackRange> rollbackRanges;
	std::vector<uint8_t> rollbackBytes;
	uint64_t rollbackSize = 0;
	bool updatePending = false;
};
