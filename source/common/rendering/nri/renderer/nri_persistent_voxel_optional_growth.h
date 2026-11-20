#pragma once

#include "nri_persistent_voxel_retention_policy.h"
#include "../scene/nri_voxel_preload_policy.h"
#include <array>
#include <cstdint>
#include <unordered_map>

enum class NRIVoxelAdmissionOrigin : uint8_t { Preload = 1, RuntimeActor = 2, WarmupActor = 4, FamilyRecovery = 8 };

inline bool NRIVoxelFamilyRecoveryOnly(uint8_t origins)
{ return origins == uint8_t(NRIVoxelAdmissionOrigin::FamilyRecovery); }

struct NRIVoxelGeometryAdmissionIntent
{
    uint32_t sourceBits = 0;
    int32_t priority = 0;
    uint8_t origins = 0;
    bool force = false, prefer = false, ownsWork = false, requiredPreload = false;
    bool runtimePump = false, afterHandoff = false;
    nri_scene::VoxelPreloadInputHints inputHints;
    uint64_t freshDirectPair = 0;

    bool RuntimeActor() const { return (origins & uint8_t(NRIVoxelAdmissionOrigin::RuntimeActor)) != 0; }
    bool PreferredManifest() const
    { return nri_scene::IsOptionalPreferredVoxelPreload(sourceBits, priority, force, prefer); }
    bool SyntheticWarmupForce() const
    { return !requiredPreload && force && (origins & uint8_t(NRIVoxelAdmissionOrigin::WarmupActor)) != 0; }
    bool MergedPreference() const
    {
        // A global force-priority preference and a map high-priority preference
        // can together rank as map-required, although neither requested that.
        return requiredPreload && (origins & uint8_t(NRIVoxelAdmissionOrigin::Preload)) != 0 &&
            prefer && inputHints.OnlyOptionalPreference() && !(sourceBits & nri_scene::PrecachedVoxelSourceBit_EnemyFamily) &&
            (!force || (origins & uint8_t(NRIVoxelAdmissionOrigin::WarmupActor)) != 0);
    }
    bool Required() const
    {
        if (inputHints.HasObligation()) return true;
        if (runtimePump && afterHandoff && (SyntheticWarmupForce() || MergedPreference())) return false;
        return requiredPreload || force || nri_scene::IsRequiredVoxelPreload(sourceBits, priority, force, prefer);
    }
    bool MayYield() const
    { return runtimePump && !ownsWork && !Required() && (RuntimeActor() || PreferredManifest() || SyntheticWarmupForce() || MergedPreference()); }
    bool IsUnownedGameplayRequest() const
    {
        return freshDirectPair && runtimePump && !ownsWork && origins == uint8_t(NRIVoxelAdmissionOrigin::RuntimeActor) &&
            !sourceBits && !force && !prefer && !requiredPreload && !inputHints.bits;
    }
    const char* Kind() const
    {
        if (!origins) return "actor-batch";
        if (NRIVoxelFamilyRecoveryOnly(origins)) return "family-recovery";
        if (ownsWork) return "owned-work";
        if (MergedPreference() && runtimePump && afterHandoff) return "handed-off-merged-preference";
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
        bool family = false, staged = false, requiredJob = false;
        const char* Bypass() const
        {
            if (!authoritative) return "authority-unavailable";
            if (consumers) return "actor-demand";
            if (active) return "active-reference";
            if (family) return "family-pin";
            if (staged) return "family-stage";
            if (requiredJob) return "required-job";
            return nullptr;
        }
    };
    struct Footprint
    {
        std::array<uint32_t, 3> rows = {};
        uint64_t generation = 0, bytes = 0;
    };
    struct Replacement { uint64_t oldBytes = 0, minimumBytes = 0; };
    struct Owner { Footprint footprint; uint64_t probe = 0; };
    struct Decision { bool defer; const char* reason; bool first = false; };

    // Sample the adapter once per presentation, only if at least one idle
    // optional mesh would copy a large live backing. Material pairs share it.
    uint64_t sampleTick = UINT64_MAX, usage = 0, budget = 0;
    std::unordered_map<uint64_t, Owner> owners;

    static const char* PressureBasis(uint64_t floor, uint64_t available, uint64_t recent)
    {
        if (recent && floor > recent) return "recent-post-commit-budget";
        if (available && floor > NRIVoxelCacheLowTarget(available)) return "live-budget-low-target";
        return nullptr;
    }
    Decision Consider(uint64_t mesh, uint64_t tick, const Footprint& footprint,
        uint64_t floor, uint64_t availableBudget, uint64_t observedCeiling, uint64_t recentCeiling = 0)
    {
        if (!availableBudget) return {false, "live-unavailable"};
        // Let the ordinary transaction verify its auxiliary costs against
        // actual previously observed room, including an equality fit.
        const uint64_t measured = observedCeiling ? observedCeiling : recentCeiling;
        if (measured && floor <= measured) return {false, "fits-observed-ceiling"};
        if (!PressureBasis(floor, availableBudget, recentCeiling)) return {false, "headroom"};
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
