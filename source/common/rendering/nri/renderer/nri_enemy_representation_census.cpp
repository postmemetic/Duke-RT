#include "nri_enemy_representation_census.h"
#include "perf_audit.h"
#include <cstdio>

void RecordNRIEnemyRepresentationCensus(uint32_t frame, uint64_t world, uint64_t assembly,
    bool enabled, bool fallbackEnabled, const NRIEnemyRepresentationCounters& policy,
    const NRIEnemyStageStats& staging, const NRIEnemyBodyCensus& bodies)
{
    if (!PerfAuditActive()) return;
    const bool available = enabled && fallbackEnabled && world && policy.world == world;
    const auto p = available ? policy : NRIEnemyRepresentationCounters{};
    const auto s = available && staging.world == world ? staging : NRIEnemyStageStats{};
    const auto b = available ? bodies : NRIEnemyBodyCensus{};
    char fields[2048];
    std::snprintf(fields, sizeof(fields),
        "\"schema_version\":1,\"renderer_frame\":%u,\"world_epoch\":%llu,\"policy_epoch\":%llu,\"assembly_ordinal\":%llu,"
        "\"enabled\":%s,\"available\":%s,\"first_sprite_total\":%llu,\"first_voxel_total\":%llu,"
        "\"supported_first_sprite_total\":%llu,\"supported_first_voxel_total\":%llu,\"emergency_total\":%llu,\"promotions_total\":%llu,"
        "\"live_sprite\":%u,\"live_voxel\":%u,\"considered_enemy\":%u,\"considered_supported\":%u,\"considered_managed\":%u,"
        "\"managed_voxel_bodies\":%u,\"managed_sprite_bodies\":%u,\"supported_voxel_bodies\":%u,\"supported_sprite_bodies\":%u,"
        "\"managed_missing\":%u,\"managed_duplicates\":%u,\"staging_owners\":%u,\"staging_families\":%u,\"staging_meshes\":%u,"
        "\"staging_geometry_bytes\":%llu,\"staging_physical_bytes\":%llu,\"staging_charged_bytes\":%llu,\"staging_limit_bytes\":%llu,"
        "\"staging_cap_refusals_total\":%llu,\"staging_pressure_drops_total\":%llu",
        frame, (unsigned long long)world, (unsigned long long)policy.epoch, (unsigned long long)assembly,
        enabled ? "true" : "false", available ? "true" : "false",
        (unsigned long long)p.firstSprite, (unsigned long long)p.firstVoxel,
        (unsigned long long)p.supportedFirstSprite, (unsigned long long)p.supportedFirstVoxel,
        (unsigned long long)p.emergencies, (unsigned long long)p.promotions, p.liveSprite, p.liveVoxel,
        b.consideredEnemy, b.consideredSupported, b.consideredManaged, b.managedVoxel, b.managedSprite,
        b.supportedVoxel, b.supportedSprite, b.missing, b.duplicates, s.owners, s.families, s.meshes,
        (unsigned long long)s.geometryBytes, (unsigned long long)s.physicalBytes, (unsigned long long)s.chargedBytes,
        (unsigned long long)s.limitBytes, (unsigned long long)s.capRefusalsTotal, (unsigned long long)s.pressureDropsTotal);
    PerfAuditWriteEvent("enemy_representation_census", PerfAuditCurrentToken(), fields);
}
