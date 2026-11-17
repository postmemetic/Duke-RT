#pragma once
#include "nri_enemy_family_staging.h"

// Actual body observations from one existing actor assembly pass. Suppressed
// candidates are considered, but intentionally absent bodies are not failures.
struct NRIEnemyBodyCensus
{
    uint32_t consideredEnemy = 0, consideredSupported = 0, consideredManaged = 0;
    uint32_t managedVoxel = 0, managedSprite = 0, supportedVoxel = 0, supportedSprite = 0;
    uint32_t missing = 0, duplicates = 0;
    void Observe(bool enemy, bool supported, bool managed, bool intended, bool voxel, bool sprite)
    {
        consideredEnemy += enemy || managed;
        consideredSupported += supported;
        consideredManaged += managed;
        if (!managed) return;
        managedVoxel += voxel; managedSprite += sprite;
        supportedVoxel += supported && voxel; supportedSprite += supported && sprite;
        missing += intended && !voxel && !sprite; duplicates += voxel && sprite;
    }
};

void RecordNRIEnemyRepresentationCensus(uint32_t frame, uint64_t world, uint64_t assembly,
    bool enabled, bool fallbackEnabled, const NRIEnemyRepresentationCounters& policy,
    const NRIEnemyStageStats& staging, const NRIEnemyBodyCensus& bodies);
