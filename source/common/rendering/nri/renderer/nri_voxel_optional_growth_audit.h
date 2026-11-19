#pragma once

#include "nri_persistent_voxel_optional_growth.h"
#include "perf_audit.h"
#include "perf_audit_frame_report.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

// Keep exact totals for every evaluated large idle allocation, including all
// material aliases. Detailed records establish individual decisions sparsely.
class NRIVoxelOptionalGrowthAudit
{
    enum Origin { Runtime, Warmup, Preferred, Merged, OriginCount };
    enum Pressure { None, Live, Recent, RecentOnly };
    enum Action { First, Repeat, FitsCeiling, Headroom, Fair, Backing, Smaller, Other, ActionCount };

    struct Counts
    {
        uint64_t first = 0, repeat = 0, probe = 0, fair = 0, queries = 0, cpu = 0, peak = 0, details = 0;
        uint64_t live = 0, recent = 0, recentOnly = 0;
        std::array<uint64_t, OriginCount> held{};
    };
    Counts frame;
    uint64_t epoch = 0, map = 0;
    bool pending = false;
    // A representative for each pressure/intent/decision in this audit/map.
    std::array<std::array<uint8_t, ActionCount>, OriginCount> seen{};
    // Backing or shape can oscillate between aliases. Bound transition detail
    // while retaining each decision count and native transaction's reason.
    uint32_t frameTransitions = 0;

    static NRIVoxelOptionalGrowthAudit& State()
    { static NRIVoxelOptionalGrowthAudit state; return state; }

    static void Flush()
    {
        auto& state = State();
        if (!state.pending) return;
        const auto& c = state.frame;
        char line[768];
        std::snprintf(line,sizeof(line),
            "\"checks\":%llu,\"defer\":%llu,\"repeat\":%llu,\"probe\":%llu,\"fair_probes\":%llu,"
            "\"queries\":%llu,\"cpu_ns\":%llu,\"max_check_ns\":%llu,\"detail_rows\":%llu,"
            "\"held_live\":%llu,\"held_recent\":%llu,\"held_recent_only\":%llu,\"held_runtime_actor\":%llu,"
            "\"held_warmup_actor\":%llu,\"held_preferred_manifest\":%llu,\"held_merged_preference\":%llu",
            (unsigned long long)(c.first+c.repeat+c.probe), (unsigned long long)c.first,
            (unsigned long long)c.repeat,(unsigned long long)c.probe,(unsigned long long)c.fair,
            (unsigned long long)c.queries,(unsigned long long)c.cpu,(unsigned long long)c.peak,
            (unsigned long long)c.details,(unsigned long long)c.live,(unsigned long long)c.recent,
            (unsigned long long)c.recentOnly,(unsigned long long)c.held[Runtime],(unsigned long long)c.held[Warmup],
            (unsigned long long)c.held[Preferred],(unsigned long long)c.held[Merged]);
        PerfAuditWriteEvent("voxel_optional_arena_growth_frame",PerfAuditCurrentToken(),line);
        state.pending = false;
        state.frame = {};
        state.frameTransitions = 0;
    }

    static Action Classify(const NRIVoxelOptionalGrowth::Decision& result)
    {
        if (result.defer) return result.first ? First : Repeat;
        if (std::strcmp(result.reason,"fits-observed-ceiling")==0) return FitsCeiling;
        if (std::strcmp(result.reason,"headroom")==0) return Headroom;
        if (std::strcmp(result.reason,"fair-probe")==0) return Fair;
        if (std::strcmp(result.reason,"backing-changed")==0) return Backing;
        if (std::strcmp(result.reason,"smaller-request")==0) return Smaller;
        return Other;
    }

public:
    static bool Record(uint64_t map, const NRIVoxelGeometryAdmissionIntent& intent,
        const NRIVoxelOptionalGrowth::Decision& result, uint64_t floor, uint64_t budget,
        uint64_t lastDenied, uint64_t cpuNs, bool query)
    {
        auto& state = State();
        const auto token = PerfAuditCurrentToken();
        if (state.epoch != token.epoch)
        {
            // A failed capture can close before its pending report is drained.
            state = {};
            state.epoch = token.epoch;
        }
        if (state.map != map)
        {
            state.map = map;
            state.seen = {};
        }
        if (!state.pending)
        {
            state.pending = true;
            PerfAuditDeferFrameReport(Flush);
        }
        const Origin origin = intent.MergedPreference() ? Merged : intent.SyntheticWarmupForce() ? Warmup :
            intent.PreferredManifest() ? Preferred : Runtime;
        const bool live = budget && floor > NRIVoxelCacheLowTarget(budget);
        const Pressure pressure = lastDenied && floor > lastDenied ? (live ? Recent : RecentOnly) : live ? Live : None;
        const Action action = Classify(result);
        auto& c = state.frame;
        c.cpu += cpuNs;
        c.peak = std::max(c.peak,cpuNs);
        c.queries += query;
        c.first += action == First;
        c.repeat += action == Repeat;
        c.probe += !result.defer;
        c.fair += action == Fair;
        if (result.defer)
        {
            ++c.held[origin];
            c.live += pressure == Live;
            c.recent += pressure == Recent || pressure == RecentOnly;
            c.recentOnly += pressure == RecentOnly;
        }

        auto& mask = state.seen[origin][action];
        const uint8_t bit = static_cast<uint8_t>(1u << pressure);
        const bool transition = (action == Backing || action == Smaller) && state.frameTransitions < 4;
        if (transition) ++state.frameTransitions;
        const bool detail = !(mask & bit) || action == First || action == Fair || transition;
        mask |= bit;
        c.details += detail;
        return detail;
    }
};
