#pragma once

#include "../scene/nri_texture_signature.h"
#include <array>
#include <cstddef>

// One synchronous material-signature loop only. The caller must not mutate
// texture metadata or invoke game callbacks during its lifetime. Failed,
// mutable and non-image signatures always retain the guarded resolver path.
class NRIWallDecalSignatureCache
{
public:
    static constexpr size_t Capacity = 16;
    template<class Resolver>
    bool Resolve(FGameTexture* texture, const nri_scene::TextureSignatureRequest& request,
        nri_scene::TextureSignature& signature, Resolver resolve)
    {
        for (size_t i = 0; i < mCount; ++i)
            if (mEntries[i].texture == texture && SameRequest(mEntries[i].request, request))
            {
                signature = mEntries[i].signature;
                ++hits;
                return true;
            }
        ++rawCalls;
        const bool success = resolve(texture, request, signature);
        if (success && signature.valid && signature.metadataDerived && signature.persistentEligible &&
            signature.sourceKind == nri_scene::TextureSignatureSourceKind::ImageBacked)
        {
            if (mCount < Capacity) mEntries[mCount++] = { texture, request, signature };
            else ++saturated;
        }
        return success;
    }
    uint64_t hits = 0, rawCalls = 0, saturated = 0;
private:
    static bool SameRequest(const nri_scene::TextureSignatureRequest& a, const nri_scene::TextureSignatureRequest& b)
    {
        return a.contentKind == b.contentKind && a.translation == b.translation && a.flags == b.flags &&
            a.scaler == b.scaler && a.scaleFactor == b.scaleFactor;
    }
    struct Entry
    {
        FGameTexture* texture;
        nri_scene::TextureSignatureRequest request;
        nri_scene::TextureSignature signature;
    };
    std::array<Entry, Capacity> mEntries;
    size_t mCount = 0;
};
