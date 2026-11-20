#pragma once

#include <cstdint>

class NRIPersistentVoxelResidency;
struct NRIPersistentVoxelSettings;
struct NRIPersistentVoxelCacheDiagnosticState;

void NRIAuditVoxelRangeRetirement(uint64_t transaction, uint64_t recordingFence, bool queued);

// Correctness-only observation of batch publication. Never owns actor/resource
// pointers across calls, and allocates no history with cache validation disabled.
class NRIVoxelCacheDiagnosticScope
{
public:
    NRIVoxelCacheDiagnosticScope(NRIPersistentVoxelResidency& owner, uint32_t frame,
        const NRIPersistentVoxelSettings& settings);
    ~NRIVoxelCacheDiagnosticScope();
private:
    NRIPersistentVoxelResidency& owner;
    const NRIPersistentVoxelSettings& settings;
    uint32_t frame;
};
