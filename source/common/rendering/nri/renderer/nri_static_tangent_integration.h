#pragma once
#include "nri_static_tangent_scratch.h"

enum class NRIStaticTangentRouteReason : uint32_t
{
    Disabled, Unsupported, DiagnosticWindow, ProbeQuota,
    MissingPublication, BindingMismatch, OwnerFallback, Active
};
struct NRIStaticTangentDispatchState
{
    uint32_t requestedMode = 0, activeMode = 0;
    NRIStaticTangentRouteReason reason = NRIStaticTangentRouteReason::Disabled;
    NRIStaticTangentReason ownerReason = NRIStaticTangentReason::Disabled;
    uint32_t primitiveCount = 0, probeDispatchesRemaining = 4;
    uint64_t outputAllocatedBytes = 0, fallbackAllocatedBytes = 0, totalOwnerAllocatedBytes = 0, builds = 0;
};
struct NRIStaticTangentIntegration
{
    NRIStaticTangentScratch scratch;
    NRIStaticTangentScratch::Binding binding;
    nri::DescriptorSet* sceneSet = nullptr;
    uint64_t sceneGeneration = 0;
    uint32_t lastRequestedMode = 0;
    uint32_t probeDispatchesRemaining = 4;
    NRIStaticTangentDispatchState dispatch;
};
