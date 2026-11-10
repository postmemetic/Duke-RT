#pragma once

#include "../scene/nri_wall_decal_types.h"
#include <algorithm>
#include <cassert>
#include <vector>

// CPU staging only; no GPU publication identity. Prepare overwrites the entire
// exposed payload. The returned vector is borrowed until the next synchronous
// Prepare/Reset; no asynchronous or recursively overlapping consumer may retain it.
class NRIWallDecalHeaderPadding
{
public:
    const std::vector<nri_scene::WallDecalHeaderGpuData>& Prepare(
        const std::vector<nri_scene::WallDecalHeaderGpuData>& headers, size_t storageRows)
    {
        assert(storageRows >= headers.size());
        mRows.resize(storageRows);
        std::copy(headers.begin(), headers.end(), mRows.begin());
        std::fill(mRows.begin() + headers.size(), mRows.end(), nri_scene::WallDecalHeaderGpuData{});
        return mRows;
    }
    void Reset()
    {
        std::vector<nri_scene::WallDecalHeaderGpuData>().swap(mRows);
        mQuarantined = false;
    }
    uint64_t CapacityBytes() const { return mRows.capacity() * sizeof(nri_scene::WallDecalHeaderGpuData); }
    bool Quarantined() const { return mQuarantined; }
    void Quarantine() { mQuarantined = true; }
private:
    std::vector<nri_scene::WallDecalHeaderGpuData> mRows;
    bool mQuarantined = false;
};
