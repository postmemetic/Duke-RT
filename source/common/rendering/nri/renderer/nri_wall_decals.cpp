#include "nri_wall_decals.h"

#include "nri_renderer.h"
#include "../scene/nri_hash.h"
#include "../scene/nri_scene_view_scratch.h"
#include "../scene/nri_texture_signature.h"
#include "../system/nri_renderdevice.h"
#include "build.h"
#include "textures.h"

#include <algorithm>

namespace
{
uint64_t BuildMaterialSignature(const std::vector<nri_scene::WallDecalCapture>& captures, bool& persistent)
{
	uint64_t signature = nri_scene::NRIHashFnv1a64OffsetBasis;
	const uint64_t count = captures.size();
	nri_scene::Fnv1a64Append(signature, &count, sizeof(count));
	persistent = true;
	for (const auto& capture : captures)
	{
		const auto& material = capture.source.material;
		nri_scene::TextureSignatureRequest request = {};
		request.contentKind = (material.flags & nri_scene::MaterialFlag_Indexed) != 0
			? nri_scene::TextureSignatureContentKind::Indexed
			: nri_scene::TextureSignatureContentKind::ProcessedBGRA;
		nri_scene::TextureSignature textureSignature = {};
		if (!nri_scene::TryBuildTextureSignature(material.texture, request, textureSignature) ||
			!textureSignature.valid || !textureSignature.persistentEligible)
		{
			persistent = false;
		}
		const uintptr_t texture = (uintptr_t)material.texture;
		nri_scene::Fnv1a64Append(signature, &texture, sizeof(texture));
		nri_scene::Fnv1a64Append(signature, &textureSignature.key, sizeof(textureSignature.key));
		nri_scene::Fnv1a64Append(signature, &material.palette, sizeof(material.palette));
		nri_scene::Fnv1a64Append(signature, &material.shade, sizeof(material.shade));
		nri_scene::Fnv1a64Append(signature, &material.alpha, sizeof(material.alpha));
		nri_scene::Fnv1a64Append(signature, &material.flags, sizeof(material.flags));
	}
	return signature != 0 ? signature : 1;
}
}

void NRIWallDecals::Reset()
{
	mHeaders.clear();
	mRecords.clear();
	mCaptureScratch.clear();
	mBaseTextureIndices.clear();
	mResolvedTextureIndices.clear();
	mRecordMaterialIndices.clear();
	nri_scene::ClearMaterialBridgeRetainingCapacity(mMaterials);
	nri_scene::ClearSceneViewRetainingCapacity(mMaterialScene);
	mMaterialSignature = 0;
	mMaterialChanged = false;
	mNeedsTextureResolve = false;
}

void NRIWallDecals::Capture(HWDrawInfo& drawInfo)
{
	nri_scene::CaptureWallDecals(drawInfo, mCaptureScratch);
	bool persistent = false;
	const uint64_t signature = BuildMaterialSignature(mCaptureScratch, persistent);
	mMaterialChanged = !persistent || signature != mMaterialSignature;
	if (mMaterialChanged)
	{
		nri_scene::ClearSceneViewRetainingCapacity(mMaterialScene);
		mMaterialScene.opaqueWalls.reserve(mCaptureScratch.size());
		for (const auto& capture : mCaptureScratch)
			mMaterialScene.opaqueWalls.push_back(capture.source);
		nri_scene::BuildMaterials(mMaterialScene, mMaterials);
		mMaterialSignature = signature;
		mResolvedTextureIndices.assign(mMaterials.textures.size(), UINT32_MAX);
		mBaseTextureIndices.clear();
		// Paint consumes only base color. Auxiliary normal/glow/PBR textures in
		// BuildMaterials do not need descriptor slots or uploads for this owner.
		for (const auto& material : mMaterials.materials)
		{
			if (material.textureIndex < mMaterials.textures.size() &&
				std::find(mBaseTextureIndices.begin(), mBaseTextureIndices.end(), material.textureIndex) == mBaseTextureIndices.end())
			{
				mBaseTextureIndices.push_back(material.textureIndex);
			}
		}
		// Empty captures still need the descriptor product refreshed once to
		// retire removed keys and invalidate a former decal descriptor template.
		mNeedsTextureResolve = true;
	}

	mHeaders.assign(wall.Size(), {});
	mRecords.clear();
	mRecordMaterialIndices.clear();
	mRecords.reserve(mCaptureScratch.size());
	for (size_t index = 0; index < mCaptureScratch.size(); ++index)
	{
		auto projection = mCaptureScratch[index].projection;
		if (projection.wallIndex >= mHeaders.size() || index >= mMaterials.materials.size())
			continue;
		const auto& material = mMaterials.materials[index];
		projection.textureIndex = material.textureIndex < mResolvedTextureIndices.size()
			? mResolvedTextureIndices[material.textureIndex] : UINT32_MAX;
		projection.paletteIndex = material.paletteIndex;
		projection.flags = material.flags;
		projection.alpha = material.alpha;
		projection.lightLevel = material.lightLevel;
		auto& header = mHeaders[projection.wallIndex];
		if (header.count == 0) header.first = (uint32_t)mRecords.size();
		++header.count;
		mRecords.push_back(projection);
		mRecordMaterialIndices.push_back((uint32_t)index);
	}
}

void NRIWallDecals::AppendTextureKeys(std::vector<uint64_t>& keys) const
{
	for (const uint32_t index : mBaseTextureIndices)
	{
		const uint64_t key = mMaterials.textures[index].key;
		if (key != 0) keys.push_back(key);
	}
}

bool NRIWallDecals::ResolveTextureDescriptors(NRIRenderer& renderer,
	std::vector<nri::Descriptor*>& descriptors, bool stableSlots, uint32_t legacyTextureCount)
{
	if (renderer.mFrameBuffer == nullptr) return false;
	// Resolve every time the shared table is assembled: legacy offsets and
	// stable-slot generations can change even when the source art is unchanged.
	std::fill(mResolvedTextureIndices.begin(), mResolvedTextureIndices.end(), UINT32_MAX);
	for (size_t ordinal = 0; ordinal < mBaseTextureIndices.size(); ++ordinal)
	{
		const uint32_t uploadIndex = mBaseTextureIndices[ordinal];
		const auto& upload = mMaterials.textures[uploadIndex];
		const auto handle = stableSlots ? renderer.mSceneTextures.SlotTable().Lookup(upload.key)
			: NRISceneTextureSlotHandle{};
		const uint64_t slot = stableSlots ? handle.slot : (uint64_t)legacyTextureCount + ordinal;
		if ((stableSlots && !handle) || slot >= NRI_MAX_SCENE_TEXTURES || 2u + slot >= descriptors.size())
			continue;
		const bool dynamic = upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas();
		nri::Descriptor* descriptor = stableSlots && !dynamic
			? renderer.mSceneTextures.FindStableSlotDescriptor(upload.key, handle) : nullptr;
		if (descriptor == nullptr)
		{
			SceneTextureResolveResult resolved = {};
			if (!renderer.mSceneTextures.ResolveTextureDescriptor(*renderer.mFrameBuffer, upload, false,
				NRISceneTextureMissPolicy::Synchronous, resolved))
			{
				mNeedsTextureResolve = true;
				return false;
			}
			// A missing view or active-canvas self reference keeps the marking
			// absent. Publishing a white fallback here would paint a rectangle.
			if (resolved.activeCanvasSelfReference || resolved.descriptor == nullptr) continue;
			descriptor = resolved.descriptor;
			if (stableSlots && !dynamic)
				renderer.mSceneTextures.StoreStableSlotDescriptor(upload.key, handle, descriptor);
		}
		descriptors[(size_t)2u + slot] = descriptor;
		mResolvedTextureIndices[uploadIndex] = (uint32_t)slot;
	}
	// There is no voxel palette row expansion in this owner. Retain the row
	// association explicitly in case capture rejected an invalid receiver.
	for (size_t index = 0; index < mRecords.size(); ++index)
	{
		const auto textureIndex = mMaterials.materials[mRecordMaterialIndices[index]].textureIndex;
		mRecords[index].textureIndex = textureIndex < mResolvedTextureIndices.size()
			? mResolvedTextureIndices[textureIndex] : UINT32_MAX;
	}
	mNeedsTextureResolve = false;
	return true;
}
