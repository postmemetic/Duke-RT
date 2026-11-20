#pragma once

#include "nri_persistent_voxel_allocation.h"
#include "nri_voxel_compute_allocation_policy.h"
#include <cstdint>

class NRIRenderer;
class FVoxelModel;

struct NRIVoxelComputeBufferAllocation
{
    NRIBufferResource* target = nullptr;
    uint64_t bytes = 0;
    uint32_t stride = 0;
    nri::BufferUsageBits usage = nri::BufferUsageBits::NONE;
    nri::MemoryLocation location = nri::MemoryLocation::DEVICE;
    nri::BufferView view = nri::BufferView::STRUCTURED_BUFFER;
    bool createView = false;
};

struct NRIVoxelComputeProviderForecast
{
    uint64_t bytes = 0;
    NRIVoxelComputeProviderReservation reservation;
    uint32_t bufferCount = 0;
    uint32_t sourcePage = UINT32_MAX;
};

struct NRIVoxelComputeAllocationOwners
{
    NRIVoxelComputeProviderStage stage = NRIVoxelComputeProviderStage::Dispatch;
    const FVoxelModel* const* sourceModels = nullptr;
    uint32_t sourceCount = 0;
    const uint64_t* meshKeys = nullptr;
    uint32_t meshCount = 0;
};

struct NRIVoxelComputeProviderSource
{
    uint32_t pageIndex = UINT32_MAX;
    uint32_t slabCount = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t primitiveCount = 0;
    uint32_t slabCapacity = 0;
    uint32_t colorRunCapacity = 0;
    bool slabAllocated = false;
    bool colorRunsAllocated = false;
    bool uploaded = false;
    bool fullReadback = false;
};

// Read the count/capacity snapshot. Only an actual forecast requests CPU
// ingestion when the raw model is missing from the archive.
bool ReadNRIVoxelComputeProviderSource(FVoxelModel* model, NRIVoxelComputeProviderSource& source, bool queueIfMissing);

// Forecasts a complete direct-publication provider footprint before committing
// geometry/AS. An unknown source queues only CPU ingestion and defers admission.
NRIPersistentVoxelAllocationResult ForecastNRIVoxelComputeProviderAllocations(
    const NRIResourceContext& context, FVoxelModel* model, NRIVoxelComputeProviderForecast& forecast);

// Exact physical allocations, queried and allocated together before commands
// reference them. Call only for a free completion slot or an archive page whose
// previous upload fence is complete; existing archive DEVICE buffers never move.
NRIPersistentVoxelAllocationResult ReserveNRIVoxelComputeBuffers(NRIRenderer& renderer,
    const NRIVoxelComputeBufferAllocation* requests, uint32_t count, const char* phase,
    const NRIVoxelComputeAllocationOwners& owners);

bool QueryNRIVoxelComputeBufferAllocation(const NRIResourceContext& context,
    const NRIVoxelComputeBufferAllocation& request, uint64_t& bytes);
