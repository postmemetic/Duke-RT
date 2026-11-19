#pragma once
#include <cstdint>

inline double NRIVoxelLoadingAdmissionBudgetMs(int configured)
{
	// Yield to the loading screen between jobs. The guard cannot preempt a
	// single native allocation or build already in progress.
	return configured > 0 ? double(configured) : 32.0;
}

// A mounted voxel prewarm preference is not a requirement to block gameplay.
// Budget denial hands the intact admission queue to runtime when actor sprites
// are enabled. Explicit strict-prewarm diagnostics retain their closure gate.
struct NRIPersistentVoxelPrewarmHandoff
{
    uint64_t mapEpoch = 0;
    bool budgetDenied = false;
    bool yielded = false;

    void ObserveBudgetDenial(uint64_t epoch)
    {
        if (mapEpoch != epoch) { *this = {}; mapEpoch = epoch; }
        budgetDenied = true;
    }
    bool MayYield(uint64_t epoch, bool allocationBudget, bool actorFallback, bool voxels, bool strict) const
    {
        return epoch == mapEpoch && budgetDenied && allocationBudget && actorFallback && voxels && !strict;
    }
};
