#pragma once

#include "nri_persistent_voxel_retention_policy.h"
#include "../scene/nri_voxel_preload_policy.h"
#include <array>
#include <cstdint>
#include <unordered_map>

enum class NRIVoxelAdmissionOrigin : uint8_t { Preload = 1, RuntimeActor = 2, WarmupActor = 4 };

struct NRIVoxelGeometryAdmissionIntent
{
    uint32_t sourceBits = 0;
    int32_t priority = 0;
    uint8_t origins = 0;
    bool force = false, prefer = false, ownsWork = false, requiredPreload = false;
    bool runtimePump = false, afterHandoff = false;

    bool RuntimeActor() const { return (origins & uint8_t(NRIVoxelAdmissionOrigin::RuntimeActor)) != 0; }
    bool PreferredManifest() const
    { return nri_scene::IsOptionalPreferredVoxelPreload(sourceBits, priority, force, prefer); }
    bool SyntheticWarmupForce() const
    { return !requiredPreload && force && (origins & uint8_t(NRIVoxelAdmissionOrigin::WarmupActor)) != 0; }
    bool Required() const
    {
        if (SyntheticWarmupForce() && runtimePump && afterHandoff) return false;
        return requiredPreload || force || nri_scene::IsRequiredVoxelPreload(sourceBits, priority, force, prefer);
    }
    bool MayYield() const
    { return runtimePump && !ownsWork && !Required() && (RuntimeActor() || PreferredManifest() || SyntheticWarmupForce()); }
    const char* Kind() const
    {
        if (!origins) return "actor-batch";
        if (ownsWork) return "owned-work";
        if (SyntheticWarmupForce()) return runtimePump && afterHandoff ? "handed-off-warmup-actor" : "warmup-forced";
        if (Required()) return "required-or-forced";
        if (PreferredManifest()) return "preferred-manifest";
        if (RuntimeActor()) return "runtime-actor";
        return "other-preload";
    }
};

struct NRIVoxelOptionalGrowth
{
    static constexpr uint64_t MinimumCopyBytes = 128ull * 1024 * 1024;
    static constexpr uint64_t ProbeTicks = 960;
    static constexpr unsigned MaximumOwners = 256;

    struct Demand
    {
        bool authoritative = false;
        uint32_t consumers = 0, active = 0;
        bool family = false, staged = false;
        const char* Bypass() const
        {
            if (!authoritative) return "authority-unavailable";
            if (consumers) return "actor-demand";
            if (active) return "active-reference";
            if (family) return "family-pin";
            if (staged) return "family-stage";
            return nullptr;
        }
    };
    struct Footprint
    {
        std::array<uint32_t, 3> rows = {};
        uint64_t generation = 0, bytes = 0;
    };
    struct Owner { Footprint footprint; uint64_t probe = 0; };
    struct Decision { bool defer; const char* reason; bool first = false; };

    // Sample the adapter once per presentation, only if at least one idle
    // optional mesh would copy a large live backing. Material pairs share it.
    uint64_t sampleTick = UINT64_MAX, usage = 0, budget = 0;
    std::unordered_map<uint64_t, Owner> owners;

    Decision Consider(uint64_t mesh, uint64_t tick, const Footprint& footprint,
        uint64_t floor, uint64_t availableBudget, uint64_t observedCeiling)
    {
        if (!availableBudget) return {false, "live-unavailable"};
        if (floor <= NRIVoxelCacheLowTarget(availableBudget)) return {false, "headroom"};
        // Let the ordinary transaction verify its auxiliary costs against
        // actual previously observed room, including an equality fit.
        if (observedCeiling && floor <= observedCeiling) return {false, "fits-observed-ceiling"};
        auto prior = owners.find(mesh);
        if (prior == owners.end())
        {
            if (owners.size() >= MaximumOwners) return {false, "owner-limit"};
            owners.emplace(mesh, Owner{footprint, tick});
            return {true, "idle-mesh-growth", true};
        }
        auto& owner = prior->second;
        const bool newBacking = owner.footprint.generation != footprint.generation;
        bool smaller = footprint.bytes < owner.footprint.bytes;
        for (std::size_t i = 0; i != footprint.rows.size(); ++i)
            smaller |= footprint.rows[i] < owner.footprint.rows[i];
        owner.footprint = footprint;
        if (newBacking || smaller || tick < owner.probe || tick - owner.probe >= ProbeTicks)
        {
            owner.probe = tick;
            return {false, newBacking ? "backing-changed" : smaller ? "smaller-request" : "fair-probe"};
        }
        return {true, "same-idle-mesh"};
    }
};
