#include "nri_scene_texture_frame_cache.h"

#include "../scene/nri_hash.h"

namespace
{
	uint64_t AppendTextureSourceHash(uint64_t hash, const nri_scene::TextureUpload& upload)
	{
		hash = nri_scene::HashCombine64(hash, upload.key);
		hash = nri_scene::HashCombine64(hash, upload.width);
		hash = nri_scene::HashCombine64(hash, upload.height);
		hash = nri_scene::HashCombine64(hash, upload.mipCount);
		hash = nri_scene::HashCombine64(hash, upload.pixels.signature());
		hash = nri_scene::HashCombine64(hash, upload.indexed ? 1ull : 0ull);
		return nri_scene::HashCombine64(hash, (uint64_t)(uintptr_t)upload.sourceTexture);
	}
}

const NRISceneTextureFrameProduct* NRISceneTextureFrameCache::Find(
	const NRISceneTextureFrameProductKey& key,
	const nri_scene::MaterialBridgeData& materials,
	uint32_t* outMissReasonMask) const
{
	uint32_t missReasonMask = 0;
	if (!mProduct.valid)
	{
		missReasonMask = NRISceneTextureFrameMiss_NoProduct;
	}
	else
	{
		if (mProduct.key.engineUpdateGeneration != key.engineUpdateGeneration)
			missReasonMask |= NRISceneTextureFrameMiss_EngineGeneration;
		if (mProduct.key.mapBuildSerial != key.mapBuildSerial)
			missReasonMask |= NRISceneTextureFrameMiss_MapSerial;
		if (mProduct.key.slotMappingRevision != key.slotMappingRevision)
			missReasonMask |= NRISceneTextureFrameMiss_SlotMapping;
		if (mProduct.key.activeCanvasSourcePointer != key.activeCanvasSourcePointer)
			missReasonMask |= NRISceneTextureFrameMiss_ActiveCanvas;
		if (mProduct.key.textureCount != key.textureCount)
			missReasonMask |= NRISceneTextureFrameMiss_Counts;
		if (mProduct.key.residentStaticUsesStableSlots != key.residentStaticUsesStableSlots)
			missReasonMask |= NRISceneTextureFrameMiss_Namespace;
		if (mProduct.sourceTextureKeys.size() != materials.textures.size())
		{
			missReasonMask |= NRISceneTextureFrameMiss_TextureSources;
		}
		else
		{
			for (size_t index = 0; index < materials.textures.size(); ++index)
			{
				const nri_scene::TextureUpload& upload = materials.textures[index];
				if (mProduct.sourceTextureKeys[index] != upload.key ||
					mProduct.sourceTextureWidths[index] != upload.width ||
					mProduct.sourceTextureHeights[index] != upload.height ||
					mProduct.sourceTextureMipCounts[index] != upload.mipCount ||
					mProduct.sourceTexturePayloadSignatures[index] != upload.pixels.signature() ||
					mProduct.sourceTexturePointers[index] != (uintptr_t)upload.sourceTexture ||
					mProduct.sourceTextureIndexed[index] != (upload.indexed ? 1u : 0u))
				{
					missReasonMask |= NRISceneTextureFrameMiss_TextureSources;
					break;
				}
			}
		}
	}
	if (outMissReasonMask != nullptr)
		*outMissReasonMask = missReasonMask;
	return missReasonMask == 0 ? &mProduct : nullptr;
}

void NRISceneTextureFrameCache::Store(
	const NRISceneTextureFrameProductKey& key,
	const nri_scene::MaterialBridgeData& materials,
	const std::vector<nri::Descriptor*>& descriptorTemplate,
	const std::vector<NRISceneTextureDynamicDependency>& dynamicDependencies,
	const NRISceneTextureFrameProductTelemetry& telemetry,
	bool stableSlotMode)
{
	mProduct.valid = false;
	mProduct.key = key;
	mProduct.sourceTextureKeys.clear();
	mProduct.sourceTextureWidths.clear();
	mProduct.sourceTextureHeights.clear();
	mProduct.sourceTextureMipCounts.clear();
	mProduct.sourceTexturePayloadSignatures.clear();
	mProduct.sourceTexturePointers.clear();
	mProduct.sourceTextureIndexed.clear();
	mProduct.sourceTextureKeys.reserve(materials.textures.size());
	mProduct.sourceTextureWidths.reserve(materials.textures.size());
	mProduct.sourceTextureHeights.reserve(materials.textures.size());
	mProduct.sourceTextureMipCounts.reserve(materials.textures.size());
	mProduct.sourceTexturePayloadSignatures.reserve(materials.textures.size());
	mProduct.sourceTexturePointers.reserve(materials.textures.size());
	mProduct.sourceTextureIndexed.reserve(materials.textures.size());
	for (const nri_scene::TextureUpload& upload : materials.textures)
	{
		mProduct.sourceTextureKeys.push_back(upload.key);
		mProduct.sourceTextureWidths.push_back(upload.width);
		mProduct.sourceTextureHeights.push_back(upload.height);
		mProduct.sourceTextureMipCounts.push_back(upload.mipCount);
		mProduct.sourceTexturePayloadSignatures.push_back(upload.pixels.signature());
		mProduct.sourceTexturePointers.push_back((uintptr_t)upload.sourceTexture);
		mProduct.sourceTextureIndexed.push_back(upload.indexed ? 1u : 0u);
	}
	mProduct.descriptorTemplate = descriptorTemplate;
	mProduct.dynamicDependencies = dynamicDependencies;
	mProduct.telemetry = telemetry;
	mProduct.traceKey = BuildTraceKey(key, materials);
	mProduct.stableSlotMode = stableSlotMode;
	mProduct.valid = true;
}

void NRISceneTextureFrameCache::Reset()
{
	mProduct = {};
}

uint64_t NRISceneTextureFrameCache::BuildTraceKey(
	const NRISceneTextureFrameProductKey& key,
	const nri_scene::MaterialBridgeData& materials)
{
	uint64_t hash = nri_scene::NRIHashFnv1a64OffsetBasis;
	hash = nri_scene::HashCombine64(hash, key.engineUpdateGeneration);
	hash = nri_scene::HashCombine64(hash, key.mapBuildSerial);
	hash = nri_scene::HashCombine64(hash, key.slotMappingRevision);
	hash = nri_scene::HashCombine64(hash, (uint64_t)key.activeCanvasSourcePointer);
	hash = nri_scene::HashCombine64(hash, key.textureCount);
	hash = nri_scene::HashCombine64(hash, key.residentStaticUsesStableSlots ? 1ull : 0ull);
	for (const nri_scene::TextureUpload& upload : materials.textures)
	{
		hash = AppendTextureSourceHash(hash, upload);
	}
	return hash == 0 ? 1 : hash;
}
