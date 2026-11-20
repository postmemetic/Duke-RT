#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>

// Only the immutable, device-queried BLAS size is reused. All potentially
// cheaper alternatives (other owners' promises, scratch and provider backing)
// may be completely free and the new request must still provably fail.
class NRIVoxelPreflightDenials
{
public:
    struct Request
    {
        const void* device = nullptr;
        const void* model = nullptr;
        const void* scratch = nullptr;
        std::array<uint32_t, 3> rows{}, cursors{}, offsets{};
        std::array<uint32_t, 8> provider{};
        uint64_t backing = 0, retirement = 0, reserve = 0;
        uint64_t scratchBytes = 0;
        uint32_t flags = 0;

        bool operator==(const Request& other) const
        {
            return device == other.device && model == other.model && rows == other.rows && provider == other.provider &&
                scratch == other.scratch && scratchBytes == other.scratchBytes &&
                cursors == other.cursors && offsets == other.offsets && backing == other.backing &&
                retirement == other.retirement && reserve == other.reserve && flags == other.flags;
        }
    };

    void Begin(uint64_t map, uint64_t presentation, uint64_t pump)
    {
        if (map == mapEpoch && tick == presentation && pump == pumpSerial) return;
        records.clear();
        mapEpoch = map;
        tick = presentation;
        pumpSerial = pump;
    }

    struct Proof
    {
        uint64_t blasBytes = 0;
        // Chooses which fresh measurement to try; never evidence of denial.
        uint64_t insufficientTrackedCeiling = 0;
    };

    Proof* Find(uint64_t mesh, const Request& request)
    {
        const auto it = records.find(mesh);
        return it != records.end() && it->second.request == request ? &it->second.proof : nullptr;
    }

    void Remember(uint64_t mesh, const Request& request, uint64_t blasBytes)
    {
        if (blasBytes && (records.size() < 512 || records.count(mesh)))
            records[mesh] = {request, {blasBytes, 0}};
    }

private:
    struct Record { Request request; Proof proof; };
    std::unordered_map<uint64_t, Record> records;
    uint64_t mapEpoch = 0, tick = UINT64_MAX, pumpSerial = 0;
};
