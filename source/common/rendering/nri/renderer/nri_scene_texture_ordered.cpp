#include "nri_scene_textures.h"
#include "../scene/nri_material_bridge.h"
#include "../system/nri_renderdevice.h"

bool NRISceneTextureResidency::TryResolveOrderedActorTexture(NRIRenderDevice& device,
    uint32_t cacheIndex, bool authorizeActorRead, SceneTextureResolveResult& outResult)
{
    if (cacheIndex >= mTextureCache.size()) return false;
    auto& cached = mTextureCache[cacheIndex];
    if ((!authorizeActorRead && !cached.actorGraphicsOrderedRead) || cached.uploadFenceValue == 0 ||
        cached.resource.texture == nullptr || cached.resource.shaderView == nullptr ||
        !device.IsCommandUploadOrderedBeforeCurrentRecording(cached.uploadFenceValue)) return false;

    cached.actorGraphicsOrderedRead = true;
    mHasOrderedActorUploads = true;
    outResult.descriptor = cached.resource.shaderView;
    outResult.pending = false; // No transparent proxy is needed for this ordered consumer.
    outResult.graphicsOrderedReady = true;
    outResult.orderedUploadBytes = cached.estimatedUploadBytes;
    outResult.closureState = NRISceneTextureClosureState::Pending; // CPU completion remains pending.
    outResult.closureFailure = NRISceneTextureClosureFailure::None;
    return true;
}

bool NRISceneTextureResidency::ResolveActorTextureOrdered(NRIRenderDevice& device,
    const nri_scene::TextureUpload& upload, bool tracePerf, SceneTextureResolveResult& outResult)
{
    // With no recording there is no safe place for an asynchronous copy. Let
    // the caller use its explicit emergency synchronous path without allocating
    // and retiring an unnecessary intermediate resource first.
    if (device.GetRecordingCommandFenceValue() == 0)
    {
        outResult = {};
        return false;
    }
    const bool ready = ResolveTextureDescriptor(device, upload, tracePerf,
        NRISceneTextureMissPolicy::RuntimeAsyncDeferOverlay, outResult);
    if (ready && !outResult.pending && outResult.descriptor != nullptr) return true;
    if (!outResult.pending) return false;
    return TryResolveOrderedActorTexture(device, FindCompatibleCacheIndex(upload), true, outResult);
}

bool NRISceneTextureResidency::RefreshOrderedActorTextureUploads(NRIRenderDevice& device)
{
    if (!mHasOrderedActorUploads) return false;
    bool pending = false, invalidated = false;
    for (uint32_t index = 0; index < mTextureCache.size(); ++index)
    {
        if (!mTextureCache[index].actorGraphicsOrderedRead) continue;
        NRISceneTextureClosureResult result;
        const auto readiness = PollCachedTexture(device, index, result);
        if (readiness == CachedTextureReadiness::Pending) pending = true;
        else
        {
            mTextureCache[index].actorGraphicsOrderedRead = false;
            invalidated |= readiness != CachedTextureReadiness::Ready;
        }
    }
    mHasOrderedActorUploads = pending;
    // Any product containing an incomplete/invalidated upload must be rebuilt.
    // In particular, abandoning a recording cannot leave its descriptor cached.
    return pending || invalidated;
}
