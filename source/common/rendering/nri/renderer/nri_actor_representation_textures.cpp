#include "nri_scene_textures.h"
#include "../scene/nri_material_bridge.h"
#include "../system/nri_renderdevice.h"
#include "perf_audit.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

bool NRISceneTextureResidency::EnsurePriorityMaterialTextures(NRIRenderDevice& device,
    const nri_scene::MaterialBridgeData& materials, const std::vector<uint32_t>& selectedMaterialIndices,
    NRIActorRepresentationTextureResult& result)
{
    result = {};
    const auto begin = std::chrono::steady_clock::now();
    std::vector<uint32_t> rows = selectedMaterialIndices;
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    std::vector<uint32_t> textures;
    textures.reserve(rows.size() * 6);
    const auto dependency = [&](uint32_t index, bool required)
    {
        if (index == UINT32_MAX && !required) return;
        if (index >= materials.textures.size()) { ++result.failures; return; }
        textures.push_back(index);
    };
    for (uint32_t row : rows)
    {
        if (row >= materials.materials.size()) { ++result.failures; continue; }
        ++result.materials;
        const auto& material = materials.materials[row];
        dependency(material.textureIndex, true);
        dependency(material.normalTextureIndex, false);
        dependency(material.metallicTextureIndex, false);
        dependency(material.roughnessTextureIndex, false);
        dependency(material.emissiveTextureIndex, false);
        if (row < materials.lightMetadata.size()) dependency(materials.lightMetadata[row].glowmapTextureIndex, false);
    }
    std::sort(textures.begin(), textures.end());
    textures.erase(std::unique(textures.begin(), textures.end()), textures.end());
    result.textures = uint32_t(textures.size());
    for (uint32_t index : textures)
    {
        const auto& upload = materials.textures[index];
        const uint32_t beforeIndex = FindCacheIndex(upload.key);
        NRISceneTextureClosureResult previous;
        QueryRuntimeClosure(device, upload.key, previous);
        // The established synchronous resolver retires a pending async upload
        // before replacing it. Never wait directly on an unsubmitted frame's
        // future fence here. Keep the overlapping retirement cost explicit.
        const bool pending = previous.state == NRISceneTextureClosureState::Pending && beforeIndex < CachedTextures().size();
        const uint64_t pendingBytes = pending ? CachedTextures()[beforeIndex].resource.memorySize : 0;
        const auto pendingResource = pending ? CachedTextures()[beforeIndex].resource.texture : nullptr;
        SceneTextureResolveResult resolved;
        const bool resolvedReady = ResolveTextureDescriptor(device, upload, true, NRISceneTextureMissPolicy::Synchronous, resolved);
        const uint32_t afterIndex = FindCacheIndex(upload.key);
        const auto afterResource = afterIndex < CachedTextures().size() ? CachedTextures()[afterIndex].resource.texture : nullptr;
        if (pending && pendingResource != afterResource)
        {
            ++result.pendingReplacements;
            result.pendingRetirementBytes += pendingBytes;
        }
        if (resolved.inserted)
        {
            ++result.inserted;
            if (afterIndex < CachedTextures().size()) result.insertedDeviceBytes += CachedTextures()[afterIndex].resource.memorySize;
        }
        if (!resolvedReady || resolved.pending || resolved.activeCanvasSelfReference || resolved.descriptor == nullptr)
        {
            ++result.failures;
            continue;
        }
        ++result.ready;
    }
    result.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    if (PerfAuditActive())
    {
        char fields[640];
        std::snprintf(fields, sizeof(fields), "\"materials\":%u,\"textures\":%u,\"ready\":%u,\"inserted\":%u,"
            "\"pending_replacements\":%u,\"failures\":%u,\"inserted_device_bytes\":%llu,"
            "\"pending_retirement_bytes\":%llu,\"cpu_ms\":%.6f,\"resource_views_ready\":%s",
            result.materials, result.textures, result.ready, result.inserted, result.pendingReplacements, result.failures,
            (unsigned long long)result.insertedDeviceBytes, (unsigned long long)result.pendingRetirementBytes,
            result.cpuMs, result.failures == 0 ? "true" : "false");
        PerfAuditWriteEvent("actor_representation_textures", PerfAuditCurrentToken(), fields);
    }
    return result.failures == 0;
}
