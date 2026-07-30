#include "nri_renderer.h"
#include "nri_cvars.h"

#include "../scene/nri_material_bridge.h"
#include "../system/nri_hwtexture.h"
#include "../system/nri_renderdevice.h"
#include "nri_diagnostic_names.h"
#include "nri_shader_contracts.h"
#include "nri_scene_material_texture_slots.h"
#include "../../../engine/perf_capture.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_set>


namespace
{
	constexpr uint32_t NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES = 16;

	struct MaterialTextureAttributionCounts
	{
		uint32_t materialCount = 0;
		uint32_t actorMaterialCount = 0;
		uint32_t textureCount = 0;
		uint32_t baseTextureCount = 0;
		uint32_t glowTextureCount = 0;
		uint32_t normalTextureCount = 0;
		uint32_t metallicTextureCount = 0;
		uint32_t roughnessTextureCount = 0;
		uint32_t emissiveTextureCount = 0;
	};

	bool ShouldTraceSceneTexturePerf()
	{
		return (int)perf_looptraceframes > 0 || ShouldEmitRendererTemporalTraceLogs();
	}

	bool ShouldTraceActorOverflow()
	{
		return (int)perf_looptraceframes > 0;
	}

	bool MaterialReferencesTextureSlot(
		const nri_scene::MaterialData& material,
		const std::unordered_set<uint32_t>& textureSlots)
	{
		return textureSlots.find(material.textureIndex) != textureSlots.end() ||
			textureSlots.find(material.normalTextureIndex) != textureSlots.end() ||
			textureSlots.find(material.metallicTextureIndex) != textureSlots.end() ||
			textureSlots.find(material.roughnessTextureIndex) != textureSlots.end() ||
			textureSlots.find(material.emissiveTextureIndex) != textureSlots.end();
	}

	void MakePendingTextureMaterialTransparent(nri_scene::MaterialData& material)
	{
		// A white descriptor is not a safe fallback for an alpha-tested actor: it
		// turns the cutout into an opaque ray occluder. Make only the affected
		// material row reject every ray until its upload fence completes. The
		// original row is rebuilt from the immutable bridge on the next frame.
		material.textureIndex = UINT32_MAX;
		material.normalTextureIndex = UINT32_MAX;
		material.metallicTextureIndex = UINT32_MAX;
		material.roughnessTextureIndex = UINT32_MAX;
		material.emissiveTextureIndex = UINT32_MAX;
		material.flags &= ~(nri_scene::MaterialFlag_Indexed | nri_scene::MaterialFlag_PlainMirror);
		material.lightingFlags = 0;
		material.alpha = 0.0f;
		material.emissiveColor[0] = 0.0f;
		material.emissiveColor[1] = 0.0f;
		material.emissiveColor[2] = 0.0f;
		material.emissiveIntensity = 0.0f;
		material.emissiveMaskScale = 0.0f;
		material.emissiveMode = nri_scene::MaterialEmissiveMode_None;
	}

	double SceneTextureDurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	}

	uint64_t EstimateSceneTextureUploadBytes(const nri_scene::TextureUpload& upload)
	{
		if (upload.width == 0 || upload.height == 0)
		{
			return 0;
		}
		const uint64_t bytesPerPixel = upload.indexed ? 1ull : 4ull;
		return (uint64_t)upload.width * (uint64_t)upload.height * bytesPerPixel;
	}

	uint64_t NoteTextureFirstUse(
		uint64_t eventId,
		uint64_t textureKey,
		uint64_t rendererFrame,
		uint32_t queuedSlot,
		uint64_t submittedFence,
		uint64_t publicationFrame,
		uint64_t bytes,
		double cpuMs,
		PerfCompactFirstUseStage stage,
		PerfCompactFirstUseState state,
		uint32_t flags,
		uint64_t producerFrame = 0)
	{
		if (eventId == 0 && (flags & PerfCompactFirstUseBegin) == 0)
		{
			return 0;
		}

		PerfCompactFirstUseRecord record = {};
		record.eventId = eventId;
		record.textureKey = textureKey;
		record.rendererFrame = rendererFrame;
		record.producerFrame = producerFrame != 0 ? producerFrame : rendererFrame;
		record.submittedFence = submittedFence;
		record.publicationFrame = publicationFrame;
		record.bytes = bytes;
		record.cpuMs = cpuMs;
		record.queuedSlot = queuedSlot;
		record.count = 1;
		record.domain = PerfCompactFirstUseDomain::Texture;
		record.stage = stage;
		record.state = state;
		record.flags = flags;
		return PerfCompactCaptureNoteFirstUse(record);
	}

	uint64_t SceneTextureHashCombine64(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	uint64_t HashSceneTextureDescriptorList(const nri::Descriptor* const* descriptors, size_t count)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = SceneTextureHashCombine64(hash, (uint64_t)count);
		for (size_t i = 0; i < count; ++i)
		{
			hash = SceneTextureHashCombine64(hash, (uint64_t)(uintptr_t)descriptors[i]);
		}
		return hash;
	}

	MaterialTextureAttributionCounts GatherMaterialTextureAttribution(
		const std::vector<nri_scene::MaterialData>& materials,
		const std::vector<nri_scene::MaterialLightingMetadata>& lightMetadata,
		size_t textureCount)
	{
		MaterialTextureAttributionCounts counts = {};
		counts.materialCount = (uint32_t)materials.size();
		counts.textureCount = (uint32_t)textureCount;

		std::unordered_set<uint32_t> baseTextures;
		std::unordered_set<uint32_t> glowTextures;
		std::unordered_set<uint32_t> normalTextures;
		std::unordered_set<uint32_t> metallicTextures;
		std::unordered_set<uint32_t> roughnessTextures;
		std::unordered_set<uint32_t> emissiveTextures;
		baseTextures.reserve(materials.size());
		glowTextures.reserve(lightMetadata.size());
		normalTextures.reserve(materials.size());
		metallicTextures.reserve(materials.size());
		roughnessTextures.reserve(materials.size());
		emissiveTextures.reserve(materials.size());

		const auto addTextureIndex = [textureCount](std::unordered_set<uint32_t>& destination, uint32_t textureIndex)
		{
			if (textureIndex != UINT32_MAX && (size_t)textureIndex < textureCount)
			{
				destination.insert(textureIndex);
			}
		};

		for (uint32_t materialIndex = 0; materialIndex < (uint32_t)materials.size(); ++materialIndex)
		{
			const auto& material = materials[materialIndex];
			addTextureIndex(baseTextures, material.textureIndex);
			addTextureIndex(normalTextures, material.normalTextureIndex);
			addTextureIndex(metallicTextures, material.metallicTextureIndex);
			addTextureIndex(roughnessTextures, material.roughnessTextureIndex);
			addTextureIndex(emissiveTextures, material.emissiveTextureIndex);
			if (materialIndex < lightMetadata.size())
			{
				const auto& metadata = lightMetadata[materialIndex];
				addTextureIndex(glowTextures, metadata.glowmapTextureIndex);
				if (metadata.actorIndex >= 0)
				{
					counts.actorMaterialCount++;
				}
			}
		}

		counts.baseTextureCount = (uint32_t)baseTextures.size();
		counts.glowTextureCount = (uint32_t)glowTextures.size();
		counts.normalTextureCount = (uint32_t)normalTextures.size();
		counts.metallicTextureCount = (uint32_t)metallicTextures.size();
		counts.roughnessTextureCount = (uint32_t)roughnessTextures.size();
		counts.emissiveTextureCount = (uint32_t)emissiveTextures.size();
		return counts;
	}

}

uint32_t NRIPreservePendingTextureMaterialProxies(
	const std::vector<nri_scene::MaterialData>& publishedMaterials,
	std::vector<nri_scene::MaterialData>& refreshedMaterials,
	const std::vector<uint32_t>& deferredMaterialIndices)
{
	const size_t materialCount = std::min(publishedMaterials.size(), refreshedMaterials.size());
	uint32_t preservedCount = 0;
	for (uint32_t materialIndex : deferredMaterialIndices)
	{
		if (materialIndex >= materialCount)
		{
			continue;
		}

		refreshedMaterials[materialIndex] = publishedMaterials[materialIndex];
		preservedCount++;
	}
	return preservedCount;
}

bool NRISceneTextureResidency::EnsurePaletteTexture(NRIRenderDevice& device, const nri_scene::MaterialBridgeData& materials)
{
	if (mPaletteTexture.texture != nullptr &&
		mPaletteTexture.width == materials.paletteWidth &&
		mPaletteTexture.height == materials.paletteHeight)
	{
		return true;
	}

	device.DestroyTextureResource(mPaletteTexture);
	if (!device.CreateOwnedTexture(mPaletteTexture, materials.paletteWidth, materials.paletteHeight, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE))
	{
		return false;
	}

	return device.UploadTextureData(mPaletteTexture, materials.paletteLookup.data(), materials.paletteWidth, materials.paletteHeight, materials.paletteWidth * 4u);
}

bool NRISceneTextureResidency::EnsureCacheEntry(NRIRenderDevice& device, const nri_scene::TextureUpload& upload, double* outRealizeMs)
{
	if (upload.width == 0 || upload.height == 0)
	{
		return true;
	}

	if (device.mActiveCanvasSourceTexture != nullptr &&
		upload.sourceTexture == device.mActiveCanvasSourceTexture)
	{
		return true;
	}

	if (upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas())
	{
		return true;
	}

	const uint32_t existingIndex = FindCacheIndex(upload.key);
	if (existingIndex != UINT32_MAX)
	{
		NRISceneTextureClosureResult existingResult = {};
		const CachedTextureReadiness readiness = PollCachedTexture(device, existingIndex, existingResult);
		if (readiness == CachedTextureReadiness::Ready)
		{
			return true;
		}
		if (readiness != CachedTextureReadiness::Abandoned)
		{
			return false;
		}
	}

	const auto realizeStart = outRealizeMs != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	std::vector<uint8_t> realizedPixels;
	uint32_t realizedWidth = upload.width;
	uint32_t realizedHeight = upload.height;
	const uint8_t* pixelData = upload.pixels.data();
	if (upload.pixels.empty())
	{
		if (!nri_scene::RealizeTextureUploadPayload(upload, realizedPixels, realizedWidth, realizedHeight))
		{
			return false;
		}
		pixelData = realizedPixels.data();
	}

	if (pixelData == nullptr || realizedWidth == 0 || realizedHeight == 0)
	{
		return false;
	}

	NRISceneCachedTexture cacheEntry = {};
	cacheEntry.key = upload.key;
	const nri::Format format = upload.indexed ? nri::Format::R8_UNORM : nri::Format::BGRA8_UNORM;
	const uint32_t rowPitch = upload.indexed ? realizedWidth : realizedWidth * 4u;
	if (!device.CreateOwnedTexture(cacheEntry.resource, realizedWidth, realizedHeight, format, nri::TextureUsageBits::SHADER_RESOURCE) ||
		!device.UploadTextureData(cacheEntry.resource, pixelData, realizedWidth, realizedHeight, rowPitch))
	{
		device.DestroyTextureResource(cacheEntry.resource);
		return false;
	}

	AddCachedTexture(std::move(cacheEntry));
	if (outRealizeMs != nullptr)
	{
		*outRealizeMs += SceneTextureDurationMs(realizeStart, std::chrono::steady_clock::now());
	}
	return true;
}

bool NRISceneTextureResidency::QueryPreloadClosure(uint64_t key, NRISceneTextureClosureResult& outResult) const
{
	outResult = {};
	outResult.key = key;
	const uint32_t cacheIndex = FindCacheIndex(key);
	if (cacheIndex == UINT32_MAX || cacheIndex >= mTextureCache.size())
	{
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::ResidencyLost;
		return false;
	}

	const NRISceneCachedTexture& cached = mTextureCache[cacheIndex];
	outResult.residencyIndex = cacheIndex;
	outResult.descriptorReady = cached.resource.shaderView != nullptr;
	if (cached.uploadFenceValue != 0)
	{
		outResult.state = NRISceneTextureClosureState::Pending;
		outResult.descriptorReady = false;
		return false;
	}
	if (cached.resource.texture == nullptr)
	{
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::ResidencyLost;
		return false;
	}
	if (!outResult.descriptorReady)
	{
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::DescriptorUnavailable;
		return false;
	}

	outResult.state = NRISceneTextureClosureState::Ready;
	outResult.reused = true;
	return true;
}

bool NRISceneTextureResidency::QueryRuntimeClosure(
	NRIRenderDevice& device,
	uint64_t key,
	NRISceneTextureClosureResult& outResult)
{
	outResult = {};
	outResult.key = key;
	const uint32_t cacheIndex = FindCacheIndex(key);
	if (cacheIndex == UINT32_MAX)
	{
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::ResidencyLost;
		return false;
	}

	const CachedTextureReadiness readiness = PollCachedTexture(device, cacheIndex, outResult);
	if (readiness == CachedTextureReadiness::Ready)
	{
		outResult.reused = true;
		return true;
	}
	return false;
}

bool NRISceneTextureResidency::EnsurePreloadClosure(
	NRIRenderDevice& device,
	const nri_scene::TextureUpload& upload,
	NRISceneTextureClosureResult& outResult)
{
	outResult = {};
	outResult.key = upload.key;
	outResult.estimatedBytes = EstimateSceneTextureUploadBytes(upload);
	if (upload.width == 0 || upload.height == 0)
	{
		outResult.state = NRISceneTextureClosureState::NotRequired;
		return true;
	}
	if (device.mActiveCanvasSourceTexture != nullptr && upload.sourceTexture == device.mActiveCanvasSourceTexture)
	{
		outResult.state = NRISceneTextureClosureState::Deferred;
		outResult.failure = NRISceneTextureClosureFailure::DynamicTexture;
		return false;
	}
	if (upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas())
	{
		outResult.state = NRISceneTextureClosureState::Deferred;
		outResult.failure = NRISceneTextureClosureFailure::DynamicTexture;
		return false;
	}
	if (upload.key == 0)
	{
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::PayloadUnavailable;
		return false;
	}

	const uint32_t existingIndex = FindCacheIndex(upload.key);
	if (existingIndex != UINT32_MAX)
	{
		outResult.estimatedBytes = EstimateSceneTextureUploadBytes(upload);
		const CachedTextureReadiness readiness = PollCachedTexture(device, existingIndex, outResult);
		outResult.estimatedBytes = EstimateSceneTextureUploadBytes(upload);
		if (readiness == CachedTextureReadiness::Ready)
		{
			outResult.reused = true;
			return true;
		}
		if (readiness != CachedTextureReadiness::Abandoned)
		{
			return false;
		}
	}

	const auto realizeStart = std::chrono::steady_clock::now();
	std::vector<uint8_t> realizedPixels;
	uint32_t realizedWidth = upload.width;
	uint32_t realizedHeight = upload.height;
	const uint8_t* pixelData = upload.pixels.data();
	if (upload.pixels.empty())
	{
		if (!nri_scene::RealizeTextureUploadPayload(upload, realizedPixels, realizedWidth, realizedHeight))
		{
			outResult = {};
			outResult.key = upload.key;
			outResult.estimatedBytes = EstimateSceneTextureUploadBytes(upload);
			outResult.state = NRISceneTextureClosureState::Failed;
			outResult.failure = NRISceneTextureClosureFailure::PayloadUnavailable;
			return false;
		}
		pixelData = realizedPixels.data();
	}
	if (pixelData == nullptr || realizedWidth == 0 || realizedHeight == 0)
	{
		outResult = {};
		outResult.key = upload.key;
		outResult.estimatedBytes = EstimateSceneTextureUploadBytes(upload);
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::PayloadUnavailable;
		return false;
	}

	NRISceneCachedTexture cacheEntry = {};
	cacheEntry.key = upload.key;
	const nri::Format format = upload.indexed ? nri::Format::R8_UNORM : nri::Format::BGRA8_UNORM;
	const uint32_t rowPitch = upload.indexed ? realizedWidth : realizedWidth * 4u;
	if (!device.CreateOwnedTexture(cacheEntry.resource, realizedWidth, realizedHeight, format, nri::TextureUsageBits::SHADER_RESOURCE) ||
		!device.UploadTextureData(cacheEntry.resource, pixelData, realizedWidth, realizedHeight, rowPitch))
	{
		device.DestroyTextureResource(cacheEntry.resource);
		outResult = {};
		outResult.key = upload.key;
		outResult.estimatedBytes = EstimateSceneTextureUploadBytes(upload);
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::ResourceCreation;
		return false;
	}

	const uint32_t cacheIndex = AddCachedTexture(std::move(cacheEntry));
	outResult = {};
	outResult.key = upload.key;
	outResult.estimatedBytes = EstimateSceneTextureUploadBytes(upload);
	outResult.residencyIndex = cacheIndex;
	outResult.realizeMs = SceneTextureDurationMs(realizeStart, std::chrono::steady_clock::now());
	outResult.realized = true;
	outResult.descriptorReady =
		cacheIndex < mTextureCache.size() && mTextureCache[cacheIndex].resource.shaderView != nullptr;
	if (!outResult.descriptorReady)
	{
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::DescriptorUnavailable;
		return false;
	}
	outResult.state = NRISceneTextureClosureState::Ready;
	return true;
}

bool NRISceneTextureResidency::EnsureRuntimeClosure(
	NRIRenderDevice& device,
	const nri_scene::TextureUpload& upload,
	NRISceneTextureClosureResult& outResult)
{
	outResult = {};
	outResult.key = upload.key;
	outResult.estimatedBytes = EstimateSceneTextureUploadBytes(upload);
	if (upload.width == 0 || upload.height == 0)
	{
		outResult.state = NRISceneTextureClosureState::NotRequired;
		return true;
	}
	if (upload.key == 0)
	{
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::PayloadUnavailable;
		return false;
	}
	if ((device.mActiveCanvasSourceTexture != nullptr && upload.sourceTexture == device.mActiveCanvasSourceTexture) ||
		(upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas()))
	{
		outResult.state = NRISceneTextureClosureState::Deferred;
		outResult.failure = NRISceneTextureClosureFailure::DynamicTexture;
		return false;
	}

	const uint32_t existingIndex = FindCacheIndex(upload.key);
	if (existingIndex != UINT32_MAX && existingIndex < mTextureCache.size())
	{
		const CachedTextureReadiness readiness = PollCachedTexture(device, existingIndex, outResult);
		outResult.estimatedBytes = EstimateSceneTextureUploadBytes(upload);
		if (readiness == CachedTextureReadiness::Ready)
		{
			outResult.reused = true;
			return true;
		}
		if (readiness != CachedTextureReadiness::Abandoned)
		{
			return false;
		}
		outResult = {};
		outResult.key = upload.key;
		outResult.estimatedBytes = EstimateSceneTextureUploadBytes(upload);
	}

	const uint64_t rendererFrame = device.mFrameIndex + 1ull;
	const uint32_t queuedSlot = device.mCurrentQueuedFrameIndex;
	const uint64_t estimatedBytes = EstimateSceneTextureUploadBytes(upload);
	const uint64_t firstUseEventId = NoteTextureFirstUse(
		0,
		upload.key,
		rendererFrame,
		queuedSlot,
		0,
		0,
		estimatedBytes,
		0.0,
		PerfCompactFirstUseStage::Request,
		PerfCompactFirstUseState::Pending,
		PerfCompactFirstUseBegin);
	const auto realizeStart = std::chrono::steady_clock::now();
	std::vector<uint8_t> realizedPixels;
	uint32_t realizedWidth = upload.width;
	uint32_t realizedHeight = upload.height;
	const uint8_t* pixelData = upload.pixels.data();
	if (upload.pixels.empty())
	{
		if (!nri_scene::RealizeTextureUploadPayload(upload, realizedPixels, realizedWidth, realizedHeight))
		{
			NoteTextureFirstUse(
				firstUseEventId, upload.key, rendererFrame, queuedSlot, 0, 0, estimatedBytes,
				SceneTextureDurationMs(realizeStart, std::chrono::steady_clock::now()),
				PerfCompactFirstUseStage::IndexedPayload, PerfCompactFirstUseState::Failed,
				PerfCompactFirstUseEnd);
			outResult.state = NRISceneTextureClosureState::Failed;
			outResult.failure = NRISceneTextureClosureFailure::PayloadUnavailable;
			return false;
		}
		pixelData = realizedPixels.data();
	}
	if (pixelData == nullptr || realizedWidth == 0 || realizedHeight == 0)
	{
		NoteTextureFirstUse(
			firstUseEventId, upload.key, rendererFrame, queuedSlot, 0, 0, estimatedBytes,
			SceneTextureDurationMs(realizeStart, std::chrono::steady_clock::now()),
			PerfCompactFirstUseStage::IndexedPayload, PerfCompactFirstUseState::Failed,
			PerfCompactFirstUseEnd);
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::PayloadUnavailable;
		return false;
	}

	NRISceneCachedTexture cacheEntry = {};
	cacheEntry.key = upload.key;
	if (upload.indexed)
	{
		NoteTextureFirstUse(
			firstUseEventId, upload.key, rendererFrame, queuedSlot, 0, 0, estimatedBytes,
			SceneTextureDurationMs(realizeStart, std::chrono::steady_clock::now()),
			PerfCompactFirstUseStage::IndexedPayload, PerfCompactFirstUseState::Instant, 0);
	}
	cacheEntry.firstUseEventId = firstUseEventId;
	cacheEntry.firstUseRequestFrame = rendererFrame;
	cacheEntry.estimatedUploadBytes = estimatedBytes;
	cacheEntry.firstUseQueuedSlot = queuedSlot;
	const nri::Format format = upload.indexed ? nri::Format::R8_UNORM : nri::Format::BGRA8_UNORM;
	const uint32_t rowPitch = upload.indexed ? realizedWidth : realizedWidth * 4u;
	const auto resourceStart = std::chrono::steady_clock::now();
	if (!device.CreateOwnedTexture(cacheEntry.resource, realizedWidth, realizedHeight, format, nri::TextureUsageBits::SHADER_RESOURCE))
	{
		const double resourceMs = SceneTextureDurationMs(resourceStart, std::chrono::steady_clock::now());
		NoteTextureFirstUse(
			firstUseEventId, upload.key, rendererFrame, queuedSlot, 0, 0, estimatedBytes, resourceMs,
			PerfCompactFirstUseStage::TextureResource, PerfCompactFirstUseState::Failed,
			PerfCompactFirstUseEnd);
		device.DestroyTextureResource(cacheEntry.resource);
		outResult.state = NRISceneTextureClosureState::Deferred;
		outResult.failure = NRISceneTextureClosureFailure::ResourceCreation;
		return false;
	}
	NoteTextureFirstUse(
		firstUseEventId, upload.key, rendererFrame, queuedSlot, 0, 0, estimatedBytes,
		SceneTextureDurationMs(resourceStart, std::chrono::steady_clock::now()),
		PerfCompactFirstUseStage::TextureResource, PerfCompactFirstUseState::Instant, 0);

	const auto uploadStart = std::chrono::steady_clock::now();
	if (!device.UploadTextureDataAsync(
			cacheEntry.resource,
			pixelData,
			realizedWidth,
			realizedHeight,
			rowPitch,
			cacheEntry.uploadFenceValue))
	{
		NoteTextureFirstUse(
			firstUseEventId, upload.key, rendererFrame, queuedSlot, 0, 0, estimatedBytes,
			SceneTextureDurationMs(uploadStart, std::chrono::steady_clock::now()),
			PerfCompactFirstUseStage::UploadRecord, PerfCompactFirstUseState::Failed,
			PerfCompactFirstUseEnd);
		device.DestroyTextureResource(cacheEntry.resource);
		outResult.state = NRISceneTextureClosureState::Deferred;
		outResult.failure = NRISceneTextureClosureFailure::ResourceCreation;
		return false;
	}
	NoteTextureFirstUse(
		firstUseEventId, upload.key, rendererFrame, queuedSlot, cacheEntry.uploadFenceValue, 0,
		estimatedBytes, SceneTextureDurationMs(uploadStart, std::chrono::steady_clock::now()),
		PerfCompactFirstUseStage::UploadRecord, PerfCompactFirstUseState::Pending, 0);

	const uint32_t cacheIndex = AddCachedTexture(std::move(cacheEntry));
	outResult.residencyIndex = cacheIndex;
	outResult.realizeMs = SceneTextureDurationMs(realizeStart, std::chrono::steady_clock::now());
	outResult.realized = true;
	outResult.descriptorReady = cacheIndex < mTextureCache.size() && mTextureCache[cacheIndex].resource.shaderView != nullptr;
	outResult.state = NRISceneTextureClosureState::Pending;
	return false;
}

bool NRISceneTextureResidency::ResolveTextureDescriptor(
	NRIRenderDevice& device,
	const nri_scene::TextureUpload& upload,
	bool tracePerf,
	NRISceneTextureMissPolicy missPolicy,
	SceneTextureResolveResult& outResult)
{
	outResult = {};

	if (device.mActiveCanvasSourceTexture != nullptr &&
		upload.sourceTexture == device.mActiveCanvasSourceTexture)
	{
		outResult.activeCanvasSelfReference = true;
		return true;
	}

	if (upload.sourceTexture != nullptr && upload.sourceTexture->isHardwareCanvas())
	{
		auto* hardwareTexture = static_cast<NRIHardwareTexture*>(upload.sourceTexture->GetHardwareTexture(0, 0));
		if (hardwareTexture != nullptr)
		{
			hardwareTexture->EnsureCanvas(upload.sourceTexture);
			if (hardwareTexture->GetResource().shaderView != nullptr)
			{
				TrackLiveResource(hardwareTexture->GetResource());
				outResult.descriptor = hardwareTexture->GetResource().shaderView;
			}
		}
		return true;
	}

	if (upload.width == 0 || upload.height == 0)
	{
		return true;
	}

	uint32_t cacheIndex = UINT32_MAX;
	if (tracePerf)
	{
		const auto start = std::chrono::steady_clock::now();
		cacheIndex = FindCacheIndex(upload.key);
		outResult.lookupMs += SceneTextureDurationMs(start, std::chrono::steady_clock::now());
	}
	else
	{
		cacheIndex = FindCacheIndex(upload.key);
	}

	if (cacheIndex == UINT32_MAX)
	{
		outResult.cacheMiss = true;
		if (missPolicy == NRISceneTextureMissPolicy::Synchronous)
		{
			if (!EnsureCacheEntry(device, upload, &outResult.realizeMs))
			{
				return false;
			}
			cacheIndex = FindReadyCacheIndex(upload.key);
			outResult.inserted = cacheIndex != UINT32_MAX;
		}
		else
		{
			NRISceneTextureClosureResult closureResult = {};
			if (!EnsureRuntimeClosure(device, upload, closureResult))
			{
				outResult.inserted = closureResult.realized;
				outResult.pending = closureResult.state == NRISceneTextureClosureState::Pending;
				outResult.closureState = closureResult.state;
				outResult.closureFailure = closureResult.failure;
				outResult.realizeMs += closureResult.realizeMs;
				return false;
			}
			cacheIndex = closureResult.residencyIndex;
			outResult.inserted = closureResult.realized;
			outResult.realizeMs += closureResult.realizeMs;
		}
	}

	if (cacheIndex != UINT32_MAX)
	{
		NRISceneTextureClosureResult closureResult = {};
		const CachedTextureReadiness readiness = PollCachedTexture(device, cacheIndex, closureResult);
		if (readiness == CachedTextureReadiness::Abandoned)
		{
			if (missPolicy == NRISceneTextureMissPolicy::Synchronous)
			{
				if (!EnsureCacheEntry(device, upload, &outResult.realizeMs))
				{
					return false;
				}
				cacheIndex = FindReadyCacheIndex(upload.key);
				outResult.inserted = cacheIndex != UINT32_MAX;
			}
			else
			{
				if (!EnsureRuntimeClosure(device, upload, closureResult))
				{
					outResult.inserted = closureResult.realized;
					outResult.pending = closureResult.state == NRISceneTextureClosureState::Pending;
					outResult.closureState = closureResult.state;
					outResult.closureFailure = closureResult.failure;
					outResult.realizeMs += closureResult.realizeMs;
					return false;
				}
				cacheIndex = closureResult.residencyIndex;
				outResult.inserted = closureResult.realized;
				outResult.realizeMs += closureResult.realizeMs;
			}
		}
		else if (readiness == CachedTextureReadiness::Pending && missPolicy == NRISceneTextureMissPolicy::Synchronous)
		{
			InvalidateCachedTexture(device, cacheIndex);
			if (!EnsureCacheEntry(device, upload, &outResult.realizeMs))
			{
				return false;
			}
			cacheIndex = FindReadyCacheIndex(upload.key);
			outResult.inserted = cacheIndex != UINT32_MAX;
		}
		else if (readiness != CachedTextureReadiness::Ready)
		{
			outResult.pending = readiness == CachedTextureReadiness::Pending;
			return false;
		}
		if (cacheIndex != UINT32_MAX && cacheIndex < mTextureCache.size())
		{
			outResult.descriptor = mTextureCache[cacheIndex].resource.shaderView;
		}
	}
	return true;
}

nri::Descriptor* NRISceneTextureResidency::FindStableSlotDescriptor(
	uint64_t key,
	NRISceneTextureSlotHandle handle) const
{
	if (!handle || handle.slot >= mStableSlotDescriptors.size())
	{
		return nullptr;
	}
	const StableSlotDescriptorCacheEntry& entry = mStableSlotDescriptors[handle.slot];
	return entry.key == key && entry.handle == handle ? entry.descriptor : nullptr;
}

void NRISceneTextureResidency::StoreStableSlotDescriptor(
	uint64_t key,
	NRISceneTextureSlotHandle handle,
	nri::Descriptor* descriptor)
{
	if (!handle)
	{
		return;
	}
	if (mStableSlotDescriptors.size() <= handle.slot)
	{
		mStableSlotDescriptors.resize((size_t)handle.slot + 1u);
	}
	mStableSlotDescriptors[handle.slot] = { key, handle, descriptor };
}

void NRISceneTextureResidency::PopulateStableSlotDescriptors(
	std::vector<nri::Descriptor*>& descriptors,
	uint32_t descriptorOffset) const
{
	for (uint32_t slot = 0; slot < (uint32_t)mStableSlotDescriptors.size(); ++slot)
	{
		const StableSlotDescriptorCacheEntry& entry = mStableSlotDescriptors[slot];
		const size_t descriptorIndex = (size_t)descriptorOffset + slot;
		if (entry.descriptor != nullptr && descriptorIndex < descriptors.size() &&
			mSlotTable.Owns(entry.key, entry.handle))
		{
			descriptors[descriptorIndex] = entry.descriptor;
		}
	}
}

uint32_t NRISceneTextureResidency::TransitionInputsForCompute(NRIRenderDevice& device)
{
	uint32_t transitionCount = 0;

	if (mPaletteTexture.texture != nullptr)
	{
		device.TransitionTexture(mPaletteTexture, NRIComputeShaderResourceState());
	}

	if (device.mWhiteTexture != nullptr)
	{
		device.TransitionTexture(device.mWhiteTexture->GetResource(), NRIComputeShaderResourceState());
	}

	for (uint32_t cacheIndex = 0; cacheIndex < mTextureCache.size(); ++cacheIndex)
	{
		NRISceneTextureClosureResult closureResult = {};
		if (PollCachedTexture(device, cacheIndex, closureResult) == CachedTextureReadiness::Ready)
		{
			transitionCount++;
			device.TransitionTexture(mTextureCache[cacheIndex].resource, NRIComputeShaderResourceState());
		}
	}

	for (NRITextureResource* resource : mLiveResources)
	{
		if (resource != nullptr && resource->texture != nullptr)
		{
			transitionCount++;
			device.TransitionTexture(*resource, NRIComputeShaderResourceState());
		}
	}

	return transitionCount;
}

bool NRIRenderer::UpdateSceneTextureSet(const std::vector<nri::Descriptor*>& descriptors, const char* reason)
{
	nri::DescriptorSet* sceneTextureSet = GetCurrentSceneTextureSet();
	if (sceneTextureSet == nullptr)
	{
		return false;
	}

	const uint64_t descriptorHash = HashSceneTextureDescriptorList(
		reinterpret_cast<const nri::Descriptor* const*>(descriptors.data()),
		descriptors.size());
	const uint32_t queuedFrameIndex = GetCurrentQueuedFrameIndex();
	if (queuedFrameIndex < mSceneTextureSetHashes.size() &&
		queuedFrameIndex < mSceneTextureSetHashValid.size() &&
		mSceneTextureSetHashValid[queuedFrameIndex] != 0 &&
		mSceneTextureSetHashes[queuedFrameIndex] == descriptorHash)
	{
		mSceneTextures.CacheStats().descriptorSkipsLastBuild = 1;
		mLastPerfShellTraceStats.sceneTextureDescriptorSkips = 1;
		return true;
	}

	nri::UpdateDescriptorRangeDesc update = {};
	update.descriptorSet = sceneTextureSet;
	update.rangeIndex = 0;
	update.descriptors = reinterpret_cast<const nri::Descriptor* const*>(descriptors.data());
	update.descriptorNum = (uint32_t)descriptors.size();
	mFrameBuffer->mCore.UpdateDescriptorRanges(&update, 1);
	mCurrentSceneTextureDescriptors = descriptors;
	if (queuedFrameIndex < mSceneTextureSetHashes.size() && queuedFrameIndex < mSceneTextureSetHashValid.size())
	{
		mSceneTextureSetHashes[queuedFrameIndex] = descriptorHash;
		mSceneTextureSetHashValid[queuedFrameIndex] = 1;
	}
	mSceneTextures.CacheStats().descriptorWritesLastBuild = 1;
	mSceneTextures.CacheStats().descriptorRowsWrittenLastBuild = (uint32_t)descriptors.size();
	mLastPerfShellTraceStats.sceneTextureDescriptorWrites = 1;
	mLastPerfShellTraceStats.sceneTextureDescriptorRowsWritten = (uint32_t)descriptors.size();
	TraceSharedDescriptorRewrite(
		"scene_textures",
		reason != nullptr ? reason : "unlabeled",
		descriptorHash,
		(uint32_t)descriptors.size(),
		true);
	return true;
}

void NRIRenderer::PrepareSceneTextureInputsForCompute()
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	const bool tracePerf = ShouldTraceSceneTexturePerf();
	const auto transitionStart = tracePerf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	const uint32_t transitionCount = mSceneTextures.TransitionInputsForCompute(*mFrameBuffer);

	const double transitionMs = tracePerf ? SceneTextureDurationMs(transitionStart, std::chrono::steady_clock::now()) : 0.0;
	mSceneTextures.CacheStats().transitionCountLastFrame = transitionCount;
	mSceneTextures.CacheStats().transitionMsLastFrame = transitionMs;
	mLastPerfShellTraceStats.sceneTextureCacheCount = mSceneTextures.CacheCount();
	mLastPerfShellTraceStats.sceneTextureTransitionCount = transitionCount;
	mLastPerfShellTraceStats.sceneTextureTransitionMs = transitionMs;
}

bool NRIRenderer::EnsurePaletteTexture(const nri_scene::MaterialBridgeData& materials)
{
	Clocker clock(NriPTPaletteUpload);
	return mFrameBuffer != nullptr && mSceneTextures.EnsurePaletteTexture(*mFrameBuffer, materials);
}

uint32_t NRIRenderer::FindSceneTextureCacheIndex(uint64_t key) const
{
	return mSceneTextures.FindReadyCacheIndex(key);
}

bool NRIRenderer::EnsureSceneTextureCacheEntry(const nri_scene::TextureUpload& upload, double* outRealizeMs)
{
	return mFrameBuffer != nullptr && mSceneTextures.EnsureCacheEntry(*mFrameBuffer, upload, outRealizeMs);
}

bool NRISceneTextureResidency::WarmMaterialTextures(NRIRenderDevice& device, const nri_scene::MaterialBridgeData& materials, NRIMaterialTextureWarmupResult& outResult)
{
	outResult = {};
	for (const nri_scene::TextureUpload& upload : materials.textures)
	{
		if (upload.width == 0 || upload.height == 0)
		{
			continue;
		}

		outResult.textureRequests++;
		const bool wasCached = FindReadyCacheIndex(upload.key) != UINT32_MAX;
		if (wasCached)
		{
			outResult.textureHits++;
			continue;
		}

		outResult.textureMisses++;
		outResult.estimatedBytes += EstimateSceneTextureUploadBytes(upload);
		double realizeMs = 0.0;
		if (!EnsureCacheEntry(device, upload, &realizeMs))
		{
			return false;
		}
		outResult.realizeMs += realizeMs;
		if (FindReadyCacheIndex(upload.key) != UINT32_MAX)
		{
			outResult.textureInserts++;
		}
	}
	return true;
}

bool NRISceneTextureResidency::WarmMaterialTexturesBudgeted(
	NRIRenderDevice& device,
	const nri_scene::MaterialBridgeData& materials,
	const NRIMaterialTextureWarmupOptions& options,
	NRIMaterialTextureWarmupCursor& cursor,
	NRIMaterialTextureWarmupResult& outResult)
{
	outResult = {};
	if (cursor.nextTextureIndex >= materials.textures.size())
	{
		cursor.nextTextureIndex = (uint32_t)materials.textures.size();
		cursor.ready = true;
		return true;
	}

	cursor.ready = false;
	const auto start = std::chrono::steady_clock::now();
	auto elapsedMs = [&]() -> double
	{
		return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
			std::chrono::steady_clock::now() - start).count();
	};

	for (size_t textureIndex = cursor.nextTextureIndex; textureIndex < materials.textures.size(); ++textureIndex)
	{
		if (options.maxMilliseconds > 0.0 && outResult.textureRequests != 0 && elapsedMs() >= options.maxMilliseconds)
		{
			outResult.pending = true;
			outResult.msBudgetHit = true;
			break;
		}

		const nri_scene::TextureUpload& upload = materials.textures[textureIndex];
		cursor.nextTextureIndex = (uint32_t)textureIndex + 1u;
		if (upload.width == 0 || upload.height == 0)
		{
			continue;
		}

		outResult.textureRequests++;
		const bool wasCached = FindReadyCacheIndex(upload.key) != UINT32_MAX;
		if (wasCached)
		{
			outResult.textureHits++;
			continue;
		}

		const uint64_t uploadBytes = EstimateSceneTextureUploadBytes(upload);
		if (options.maxTextureInserts != 0 && outResult.textureInserts >= options.maxTextureInserts)
		{
			cursor.nextTextureIndex = (uint32_t)textureIndex;
			outResult.pending = true;
			outResult.textureBudgetHit = true;
			break;
		}
		if (options.maxUploadBytes != 0 &&
			outResult.textureInserts != 0 &&
			outResult.estimatedBytes + uploadBytes > options.maxUploadBytes)
		{
			cursor.nextTextureIndex = (uint32_t)textureIndex;
			outResult.pending = true;
			outResult.byteBudgetHit = true;
			break;
		}

		outResult.textureMisses++;
		outResult.estimatedBytes += uploadBytes;
		double realizeMs = 0.0;
		if (!EnsureCacheEntry(device, upload, &realizeMs))
		{
			return false;
		}
		outResult.realizeMs += realizeMs;
		if (FindReadyCacheIndex(upload.key) != UINT32_MAX)
		{
			outResult.textureInserts++;
		}

		if (options.maxTextureInserts != 0 && outResult.textureInserts >= options.maxTextureInserts)
		{
			outResult.pending = cursor.nextTextureIndex < materials.textures.size();
			outResult.textureBudgetHit = outResult.pending;
			break;
		}
		if (options.maxUploadBytes != 0 &&
			outResult.textureInserts != 0 &&
			outResult.estimatedBytes >= options.maxUploadBytes)
		{
			outResult.pending = cursor.nextTextureIndex < materials.textures.size();
			outResult.byteBudgetHit = outResult.pending;
			break;
		}
	}

	if (cursor.nextTextureIndex >= materials.textures.size())
	{
		cursor.nextTextureIndex = (uint32_t)materials.textures.size();
		cursor.ready = true;
		outResult.pending = false;
	}
	else
	{
		outResult.pending = true;
	}
	return true;
}

void NRIRenderer::ResolveSceneMaterialTextureSlots(
	const nri_scene::MaterialBridgeData& materials,
	std::vector<nri_scene::MaterialData>& gpuMaterials) const
{
	if (!mSceneTextureStableSlotsActive)
	{
		return;
	}

	NRIResolveSceneMaterialTextureSlots(mSceneTextures.SlotTable(), materials, gpuMaterials);
}

bool NRIRenderer::EnsureSceneTextures(
	const nri_scene::SceneView& sceneView,
	const nri_scene::MaterialBridgeData& materials,
	std::vector<nri_scene::MaterialData>& outGpuMaterials,
	bool preserveExistingSky,
	const char* reason,
	const NRISceneTextureFrameReuseInputs* reuseInputs,
	NRISceneTextureMissPolicy missPolicy,
	std::vector<uint32_t>* outDeferredMaterialIndices)
{
	Clocker clock(NriPTSceneTextures);
	static bool sLoggedActiveCanvasTextureReuse = false;
	const bool tracePerf = ShouldTraceSceneTexturePerf();
	uint32_t lookupMisses = 0;
	uint32_t insertCount = 0;
	double lookupMs = 0.0;
	double realizeMs = 0.0;
	double descriptorMs = 0.0;
	mSceneTextures.OverflowStats().textureCountLastBuild = (uint32_t)materials.textures.size();
	mSceneTextures.OverflowStats().truncatedTextureCountLastBuild =
		mSceneTextures.OverflowStats().textureCountLastBuild > NRI_MAX_SCENE_TEXTURES ?
		mSceneTextures.OverflowStats().textureCountLastBuild - NRI_MAX_SCENE_TEXTURES : 0;
	mSceneTextures.OverflowStats().baseTextureClampCountLastBuild = 0;
	mSceneTextures.OverflowStats().normalTextureClampCountLastBuild = 0;
	mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild = 0;
	mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild = 0;
	mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild = 0;
	mSceneTextures.CacheStats().cacheEntriesLastBuild = mSceneTextures.CacheCount();
	mSceneTextures.CacheStats().lookupMissesLastBuild = 0;
	mSceneTextures.CacheStats().insertCountLastBuild = 0;
	mSceneTextures.CacheStats().lookupMsLastBuild = 0.0;
	mSceneTextures.CacheStats().realizeMsLastBuild = 0.0;
	mSceneTextures.CacheStats().descriptorMsLastBuild = 0.0;
	mSceneTextures.CacheStats().descriptorWritesLastBuild = 0;
	mSceneTextures.CacheStats().descriptorSkipsLastBuild = 0;
	mSceneTextures.CacheStats().descriptorRowsWrittenLastBuild = 0;
	mSceneTextures.CacheStats().stableSlotModeLastBuild = 0;
	mSceneTextures.CacheStats().stableDescriptorHitsLastBuild = 0;
	mSceneTextures.CacheStats().stableDescriptorMissesLastBuild = 0;
	if (outDeferredMaterialIndices != nullptr)
	{
		outDeferredMaterialIndices->clear();
	}
	mSceneTextures.ClearLiveResources();
	mLastPerfShellTraceStats.sceneTextureCacheCount = mSceneTextures.CacheCount();
	mLastPerfShellTraceStats.sceneTextureCacheMisses = 0;
	mLastPerfShellTraceStats.sceneTextureCacheInserts = 0;
	mLastPerfShellTraceStats.sceneTextureLookupMs = 0.0;
	mLastPerfShellTraceStats.sceneTextureRealizeMs = 0.0;
	mLastPerfShellTraceStats.sceneTextureDescriptorMs = 0.0;
	mLastPerfShellTraceStats.sceneTextureDescriptorWrites = 0;
	mLastPerfShellTraceStats.sceneTextureDescriptorSkips = 0;
	mLastPerfShellTraceStats.sceneTextureDescriptorRowsWritten = 0;
	mLastPerfShellTraceStats.sceneTextureStableSlotMode = 0;
	mLastPerfShellTraceStats.sceneTextureStableDescriptorHits = 0;
	mLastPerfShellTraceStats.sceneTextureStableDescriptorMisses = 0;
	mLastPerfShellTraceStats.sceneTextureReason = reason != nullptr ? reason : "none";
	mLastPerfShellTraceStats.sceneTextureRequestedCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedBaseCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedGlowCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedNormalCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount = 0;
	mLastPerfShellTraceStats.actorOverflowMaterialCount = 0;
	mLastPerfShellTraceStats.actorOverflowBaseClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowNormalClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowMetallicClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowRoughnessClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowEmissiveClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowTraceOmittedCount = 0;
	mSceneTextureStableSlotsActive = false;
	if (reuseInputs != nullptr)
	{
		mLastPerfShellTraceStats.sceneReuseTextureCalled = 1;
		mLastPerfShellTraceStats.sceneReuseTextureHit = 0;
		mLastPerfShellTraceStats.sceneReuseTextureCandidateHit = 0;
		mLastPerfShellTraceStats.sceneReuseTextureBuild = 1;
		mLastPerfShellTraceStats.sceneReuseTextureReject = 0;
		mLastPerfShellTraceStats.sceneReuseTextureValidationChecked = 0;
		mLastPerfShellTraceStats.sceneReuseTextureValidationMismatch = 0;
		mLastPerfShellTraceStats.sceneReuseTextureDynamicCount = 0;
		mLastPerfShellTraceStats.sceneReuseTextureMissReasonMask = 0;
		mLastPerfShellTraceStats.sceneReuseTextureKey = 0;
	}
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.ensureSceneTexturesCalls++;
		if (preserveExistingSky)
		{
			gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveTrueCalls++;
		}
		else
		{
			gRendererSkyPerfTraceStats.ensureSceneTexturesPreserveFalseCalls++;
		}
	}

	const bool combinedMaterialSet = &materials == &mSceneMaterialFrameCache.Materials();
	const bool combinedUsesResidentStaticBuffer =
		combinedMaterialSet &&
		mStaticMapScene.valid &&
		mStaticMapScene.buffersResident &&
		!mStaticMapScene.gpuMaterials.empty();
	const bool reuseOwnerEligible =
		reuseInputs != nullptr &&
		reuseInputs->allowReuse &&
		reuseInputs->engineUpdateGeneration != 0 &&
		reuseInputs->mapBuildSerial != 0 &&
		combinedUsesResidentStaticBuffer &&
		mStaticMapScene.gpuMaterialsUseStableTextureSlots;
	NRISceneTextureFrameProductKey reuseKey = {};
	const NRISceneTextureFrameProduct* reuseProduct = nullptr;
	uint64_t reuseTraceKey = 0;
	if (reuseOwnerEligible)
	{
		reuseKey.engineUpdateGeneration = reuseInputs->engineUpdateGeneration;
		reuseKey.mapBuildSerial = reuseInputs->mapBuildSerial;
		reuseKey.slotMappingRevision = mSceneTextures.SlotTable().MappingRevision();
		reuseKey.activeCanvasSourcePointer = (uintptr_t)mFrameBuffer->mActiveCanvasSourceTexture;
		reuseKey.textureCount = (uint32_t)materials.textures.size();
		reuseKey.residentStaticUsesStableSlots = true;
		if (reuseInputs->ticksExecutedThisPresentation == 0)
		{
			reuseProduct = mSceneTextureFrameCache.Find(
				reuseKey,
				materials,
				&mLastPerfShellTraceStats.sceneReuseTextureMissReasonMask);
		}
		reuseTraceKey = reuseProduct != nullptr ? reuseProduct->traceKey : 0;
		if (reuseInputs != nullptr)
		{
			mLastPerfShellTraceStats.sceneReuseTextureCandidateHit = reuseProduct != nullptr ? 1u : 0u;
			mLastPerfShellTraceStats.sceneReuseTextureKey = reuseTraceKey;
		}
	}

	mSceneTextureKeyScratch.clear();
	mSceneTextureKeyScratch.reserve(materials.textures.size());
	for (const nri_scene::TextureUpload& upload : materials.textures)
	{
		if (upload.key != 0)
		{
			mSceneTextureKeyScratch.push_back(upload.key);
		}
	}
	const uint64_t currentTextureSerial = (uint64_t)mFrameIndex + 1ull;
	const uint64_t queuedFrameCount = mFrameBuffer != nullptr ?
		std::max<uint64_t>(1ull, (uint64_t)mFrameBuffer->mQueuedFrames.size()) : 1ull;
	const uint64_t completedTextureSerial =
		currentTextureSerial > queuedFrameCount ? currentTextureSerial - queuedFrameCount : 0ull;
	const bool authoritativeTextureSet =
		combinedMaterialSet ||
		(reason != nullptr && std::strcmp(reason, "static_map_scene") == 0);
	if (combinedUsesResidentStaticBuffer && !mStaticMapScene.gpuMaterialsUseStableTextureSlots)
	{
		// The combined bridge preserves the static bridge's texture prefix, so
		// legacy indices remain valid. Do not switch descriptors to stable order
		// without also converting and uploading the durable static atlas.
		bool staticTexturePrefixMatches =
			materials.textures.size() >= mStaticMapScene.materialBridge.textures.size();
		for (size_t textureIndex = 0;
			staticTexturePrefixMatches && textureIndex < mStaticMapScene.materialBridge.textures.size();
			++textureIndex)
		{
			staticTexturePrefixMatches =
				materials.textures[textureIndex].key == mStaticMapScene.materialBridge.textures[textureIndex].key;
		}
		if (!staticTexturePrefixMatches)
		{
			if (nri_ptscenestats || ShouldTraceSceneTexturePerf())
			{
				Printf("NRI PT scene textures: event=namespace_mismatch reason=%s static_namespace=legacy combined_prefix=changed action=reject\n",
					reason != nullptr ? reason : "none");
			}
			return false;
		}
		mSceneTextureStableSlotsActive = false;
	}
	else
	{
		mSceneTextureStableSlotsActive = authoritativeTextureSet ?
			mSceneTextures.SlotTable().UpdateActiveKeys(
				mSceneTextureKeyScratch,
				currentTextureSerial,
				completedTextureSerial) :
			mSceneTextures.SlotTable().EnsureActiveKeys(
				mSceneTextureKeyScratch,
				completedTextureSerial);
		if (combinedUsesResidentStaticBuffer && !mSceneTextureStableSlotsActive)
		{
			// The static atlas already contains stable slot indices. Slot-table
			// allocation is transactional; preserve the last coherent descriptor
			// set and fail this scene update rather than publishing legacy order.
			if (nri_ptscenestats || ShouldTraceSceneTexturePerf())
			{
				Printf("NRI PT scene textures: event=namespace_mismatch reason=%s static_namespace=stable combined_namespace=legacy action=reject\n",
					reason != nullptr ? reason : "none");
			}
			return false;
		}
	}
	if (reuseProduct != nullptr &&
		(reuseProduct->key.slotMappingRevision != mSceneTextures.SlotTable().MappingRevision() ||
		 reuseProduct->stableSlotMode != mSceneTextureStableSlotsActive))
	{
		if (reuseProduct->key.slotMappingRevision != mSceneTextures.SlotTable().MappingRevision())
			mLastPerfShellTraceStats.sceneReuseTextureMissReasonMask |= NRISceneTextureFrameMiss_SlotMapping;
		if (reuseProduct->stableSlotMode != mSceneTextureStableSlotsActive)
			mLastPerfShellTraceStats.sceneReuseTextureMissReasonMask |= NRISceneTextureFrameMiss_Namespace;
		mLastPerfShellTraceStats.sceneReuseTextureCandidateHit = 0;
		mLastPerfShellTraceStats.sceneReuseTextureKey = 0;
		reuseProduct = nullptr;
	}
	const nri_scene::MaterialBridgeData* gpuMaterialSource = &materials;
	if (mSceneTextureStableSlotsActive && &materials == &mSceneMaterialFrameCache.Materials())
	{
		gpuMaterialSource = &mSceneMaterialFrameCache.ResolveTextureSlots(mSceneTextures.SlotTable());
	}
	outGpuMaterials = gpuMaterialSource->materials;
	ApplyEmissiveMaterialOverrides(*gpuMaterialSource, outGpuMaterials);
	ApplyActorShadowMaterialOverrides(*gpuMaterialSource, outGpuMaterials);
	if (mSceneTextureStableSlotsActive)
	{
		if (gpuMaterialSource == &materials)
		{
			ResolveSceneMaterialTextureSlots(materials, outGpuMaterials);
		}
	}
	if (mSceneTextureStableSlotsActive)
	{
		mSceneTextures.CacheStats().stableSlotModeLastBuild = 1;
		mLastPerfShellTraceStats.sceneTextureStableSlotMode = 1;
		mSceneTextures.OverflowStats().truncatedTextureCountLastBuild = 0;
	}
	const NRISceneTextureSlotStats slotStats = mSceneTextures.SlotTable().GetStats();
	mLastPerfShellTraceStats.sceneTextureSlotsLive = (uint32_t)slotStats.live;
	mLastPerfShellTraceStats.sceneTextureSlotsQuarantined = (uint32_t)slotStats.quarantined;
	mLastPerfShellTraceStats.sceneTextureSlotsFree = (uint32_t)slotStats.free;
	mLastPerfShellTraceStats.sceneTextureSlotReuses = slotStats.reuseCount;
	mLastPerfShellTraceStats.sceneTextureSlotExhaustions = slotStats.exhaustionCount;
	MaterialTextureAttributionCounts sceneTextureAttribution = {};
	if (mSceneTextureStableSlotsActive)
	{
		sceneTextureAttribution.materialCount = (uint32_t)outGpuMaterials.size();
		sceneTextureAttribution.textureCount = (uint32_t)slotStats.live;
	}
	else
	{
		sceneTextureAttribution =
			GatherMaterialTextureAttribution(outGpuMaterials, materials.lightMetadata, materials.textures.size());
	}
	mLastPerfShellTraceStats.sceneTextureRequestedCount = sceneTextureAttribution.textureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount = sceneTextureAttribution.actorMaterialCount;
	mLastPerfShellTraceStats.sceneTextureReferencedBaseCount = sceneTextureAttribution.baseTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedGlowCount = sceneTextureAttribution.glowTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedNormalCount = sceneTextureAttribution.normalTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount = sceneTextureAttribution.metallicTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount = sceneTextureAttribution.roughnessTextureCount;
	mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount = sceneTextureAttribution.emissiveTextureCount;
	if (!EnsureSkyTexture(sceneView, preserveExistingSky))
	{
		return false;
	}

	std::vector<nri::Descriptor*> descriptors;
	std::vector<NRISceneTextureDynamicDependency> dynamicDependencies;
	std::unordered_set<uint32_t> pendingTextureSlots;
	const bool useCachedDescriptorProduct = reuseProduct != nullptr && !reuseInputs->validateReuse;
	if (useCachedDescriptorProduct)
	{
		descriptors = reuseProduct->descriptorTemplate;
		dynamicDependencies = reuseProduct->dynamicDependencies;
		if (descriptors.size() != NRI_SCENE_DESCRIPTOR_NUM)
		{
			mLastPerfShellTraceStats.sceneReuseTextureReject = 1;
			mSceneTextureFrameCache.Reset();
			return false;
		}
		descriptors[0] = mSceneTextures.PaletteTexture().shaderView;
		descriptors[1] = GetActiveSkyTexture() != nullptr ? GetActiveSkyTexture()->shaderView : mFrameBuffer->mWhiteTexture->GetResource().shaderView;
		mSceneTextures.CacheStats().stableDescriptorHitsLastBuild = reuseProduct->telemetry.stableDescriptorHits;
		mLastPerfShellTraceStats.sceneTextureStableDescriptorHits = reuseProduct->telemetry.stableDescriptorHits;
		for (const NRISceneTextureDynamicDependency& dependency : dynamicDependencies)
		{
			if (dependency.uploadIndex >= materials.textures.size() || dependency.descriptorIndex >= descriptors.size())
			{
				mLastPerfShellTraceStats.sceneReuseTextureReject = 1;
				mSceneTextureFrameCache.Reset();
				return false;
			}
			descriptors[dependency.descriptorIndex] = mFrameBuffer->mWhiteTexture->GetResource().shaderView;
			SceneTextureResolveResult textureResult = {};
			if (!mSceneTextures.ResolveTextureDescriptor(
				*mFrameBuffer,
				materials.textures[dependency.uploadIndex],
				tracePerf,
				missPolicy,
				textureResult))
			{
				if (!textureResult.pending || dependency.descriptorIndex < 2u)
				{
					mLastPerfShellTraceStats.sceneReuseTextureReject = 1;
					return false;
				}
				pendingTextureSlots.insert(dependency.descriptorIndex - 2u);
			}
			lookupMisses += textureResult.cacheMiss ? 1u : 0u;
			insertCount += textureResult.inserted ? 1u : 0u;
			lookupMs += textureResult.lookupMs;
			realizeMs += textureResult.realizeMs;
			if (!textureResult.activeCanvasSelfReference && textureResult.descriptor != nullptr)
			{
				descriptors[dependency.descriptorIndex] = textureResult.descriptor;
			}
		}
	}
	else
	{
		descriptors.assign(NRI_SCENE_DESCRIPTOR_NUM, mFrameBuffer->mWhiteTexture->GetResource().shaderView);
		descriptors[0] = mSceneTextures.PaletteTexture().shaderView;
		descriptors[1] = GetActiveSkyTexture() != nullptr ? GetActiveSkyTexture()->shaderView : mFrameBuffer->mWhiteTexture->GetResource().shaderView;
		if (mSceneTextureStableSlotsActive)
		{
			// EnsureSceneTextures is also used by subset owners such as resident
			// mutation slices. Preserve descriptors for every other live stable slot.
			mSceneTextures.PopulateStableSlotDescriptors(descriptors, 2u);
		}

		for (uint32_t i = 0; i < (uint32_t)materials.textures.size(); ++i)
		{
			const auto& upload = materials.textures[i];
			const NRISceneTextureSlotHandle stableHandle =
				mSceneTextureStableSlotsActive ? mSceneTextures.SlotTable().Lookup(upload.key) : NRISceneTextureSlotHandle{};
			if (mSceneTextureStableSlotsActive && !stableHandle)
			{
				continue;
			}
			if (!mSceneTextureStableSlotsActive && i >= NRI_MAX_SCENE_TEXTURES)
			{
				break;
			}
			SceneTextureResolveResult textureResult = {};
			const bool dynamicDescriptor =
				upload.sourceTexture != nullptr &&
				(upload.sourceTexture == mFrameBuffer->mActiveCanvasSourceTexture || upload.sourceTexture->isHardwareCanvas());
			if (dynamicDescriptor)
			{
				const uint32_t descriptorSlot = mSceneTextureStableSlotsActive ? stableHandle.slot : i;
				dynamicDependencies.push_back({ i, 2u + descriptorSlot });
			}
			nri::Descriptor* stableDescriptor =
				mSceneTextureStableSlotsActive && !dynamicDescriptor ?
				mSceneTextures.FindStableSlotDescriptor(upload.key, stableHandle) : nullptr;
			if (stableDescriptor != nullptr)
			{
				textureResult.descriptor = stableDescriptor;
				mSceneTextures.CacheStats().stableDescriptorHitsLastBuild++;
				mLastPerfShellTraceStats.sceneTextureStableDescriptorHits++;
			}
			else if (!mSceneTextures.ResolveTextureDescriptor(*mFrameBuffer, upload, tracePerf, missPolicy, textureResult))
			{
				if (!textureResult.pending)
				{
					if (nri_ptdebug > 0)
					{
						Printf("NRI PT scene textures: event=resolve_failed reason=%s upload=%u key=0x%llx size=%ux%u indexed=%u pixels=%zu source=%p closure_state=%u closure_failure=%u\n",
							reason != nullptr ? reason : "unknown",
							i,
							(unsigned long long)upload.key,
							upload.width,
							upload.height,
							upload.indexed ? 1u : 0u,
							upload.pixels.size(),
							(void*)upload.sourceTexture,
							(uint32_t)textureResult.closureState,
							(uint32_t)textureResult.closureFailure);
					}
					return false;
				}
				const uint32_t descriptorSlot = mSceneTextureStableSlotsActive ? stableHandle.slot : i;
				pendingTextureSlots.insert(descriptorSlot);
			}
			else if (mSceneTextureStableSlotsActive && !dynamicDescriptor && textureResult.descriptor != nullptr)
			{
				mSceneTextures.StoreStableSlotDescriptor(upload.key, stableHandle, textureResult.descriptor);
				mSceneTextures.CacheStats().stableDescriptorMissesLastBuild++;
				mLastPerfShellTraceStats.sceneTextureStableDescriptorMisses++;
			}
			if (textureResult.activeCanvasSelfReference)
			{
				if (!sLoggedActiveCanvasTextureReuse || nri_ptdebug > 0)
				{
					Printf(TEXTCOLOR_ORANGE "NRI PT textures: using a fallback descriptor for the canvas currently being rendered to avoid self-referential camera-texture uploads.\n");
					sLoggedActiveCanvasTextureReuse = true;
				}
				continue;
			}
			if (textureResult.cacheMiss)
			{
				lookupMisses++;
			}
			if (textureResult.inserted)
			{
				insertCount++;
			}
			lookupMs += textureResult.lookupMs;
			realizeMs += textureResult.realizeMs;
			if (textureResult.descriptor != nullptr)
			{
				const uint32_t descriptorSlot = mSceneTextureStableSlotsActive ? stableHandle.slot : i;
				descriptors[2 + descriptorSlot] = textureResult.descriptor;
			}
		}
	}

	uint32_t deferredMaterialCount = 0;
	if (!pendingTextureSlots.empty())
	{
		for (uint32_t materialIndex = 0; materialIndex < (uint32_t)outGpuMaterials.size(); ++materialIndex)
		{
			nri_scene::MaterialData& material = outGpuMaterials[materialIndex];
			if (!MaterialReferencesTextureSlot(material, pendingTextureSlots))
			{
				continue;
			}
			MakePendingTextureMaterialTransparent(material);
			if (outDeferredMaterialIndices != nullptr)
			{
				outDeferredMaterialIndices->push_back(materialIndex);
			}
			deferredMaterialCount++;
		}
		if (nri_ptscenestats || ShouldTraceSceneTexturePerf())
		{
			Printf(
				"NRI PT scene textures: event=runtime_pending_defer reason=%s pending_slots=%u deferred_materials=%u action=transparent-material-proxy\n",
				reason != nullptr ? reason : "none",
				(uint32_t)pendingTextureSlots.size(),
				deferredMaterialCount);
		}
	}

	uint32_t actorOverflowTraceLines = 0;
	if (!mSceneTextureStableSlotsActive)
	{
		for (uint32_t materialIndex = 0; materialIndex < (uint32_t)outGpuMaterials.size(); ++materialIndex)
		{
		auto& material = outGpuMaterials[materialIndex];
		const uint32_t originalTextureIndex = material.textureIndex;
		const uint32_t originalNormalTextureIndex = material.normalTextureIndex;
		const uint32_t originalMetallicTextureIndex = material.metallicTextureIndex;
		const uint32_t originalRoughnessTextureIndex = material.roughnessTextureIndex;
		const uint32_t originalEmissiveTextureIndex = material.emissiveTextureIndex;
		bool baseClamped = false;
		bool normalClamped = false;
		bool metallicClamped = false;
		bool roughnessClamped = false;
		bool emissiveClamped = false;
		if (material.textureIndex != UINT32_MAX && material.textureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextures.OverflowStats().baseTextureClampCountLastBuild++;
			material.textureIndex = 0;
			baseClamped = true;
		}
		if (material.normalTextureIndex != UINT32_MAX && material.normalTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextures.OverflowStats().normalTextureClampCountLastBuild++;
			material.normalTextureIndex = UINT32_MAX;
			normalClamped = true;
		}
		if (material.metallicTextureIndex != UINT32_MAX && material.metallicTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild++;
			material.metallicTextureIndex = UINT32_MAX;
			metallicClamped = true;
		}
		if (material.roughnessTextureIndex != UINT32_MAX && material.roughnessTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild++;
			material.roughnessTextureIndex = UINT32_MAX;
			roughnessClamped = true;
		}
		if (material.emissiveTextureIndex != UINT32_MAX && material.emissiveTextureIndex >= NRI_MAX_SCENE_TEXTURES)
		{
			mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild++;
			material.emissiveTextureIndex = 0;
			emissiveClamped = true;
		}

		if (!(baseClamped || normalClamped || metallicClamped || roughnessClamped || emissiveClamped))
		{
			continue;
		}

		const nri_scene::MaterialLightingMetadata* metadata =
			materialIndex < materials.lightMetadata.size() ? &materials.lightMetadata[materialIndex] : nullptr;
		if (metadata == nullptr || metadata->actorIndex < 0)
		{
			continue;
		}

		mLastPerfShellTraceStats.actorOverflowMaterialCount++;
		if (baseClamped)
		{
			mLastPerfShellTraceStats.actorOverflowBaseClampCount++;
		}
		if (normalClamped)
		{
			mLastPerfShellTraceStats.actorOverflowNormalClampCount++;
		}
		if (metallicClamped)
		{
			mLastPerfShellTraceStats.actorOverflowMetallicClampCount++;
		}
		if (roughnessClamped)
		{
			mLastPerfShellTraceStats.actorOverflowRoughnessClampCount++;
		}
		if (emissiveClamped)
		{
			mLastPerfShellTraceStats.actorOverflowEmissiveClampCount++;
		}

		if (!ShouldTraceActorOverflow())
		{
			continue;
		}

		if (actorOverflowTraceLines < NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES)
		{
			Printf(
				"PERF pt actor overflow NRI: frame=%llu reason=%s actor=%d source=%s material=%u texture_id=%u base=%u->%u normal=%u->%u metallic=%u->%u roughness=%u->%u emissive=%u->%u\n",
				(unsigned long long)mFrameIndex,
				mLastPerfShellTraceStats.sceneTextureReason.empty() ? "none" : mLastPerfShellTraceStats.sceneTextureReason.c_str(),
				metadata->actorIndex,
				nri_diag::GetSurfaceSourceTypeName(metadata->sourceType),
				materialIndex,
				metadata->textureId,
				originalTextureIndex,
				material.textureIndex,
				originalNormalTextureIndex,
				material.normalTextureIndex,
				originalMetallicTextureIndex,
				material.metallicTextureIndex,
				originalRoughnessTextureIndex,
				material.roughnessTextureIndex,
				originalEmissiveTextureIndex,
				material.emissiveTextureIndex);
			actorOverflowTraceLines++;
		}
		else
		{
			mLastPerfShellTraceStats.actorOverflowTraceOmittedCount++;
		}
		}
	}
	if (mLastPerfShellTraceStats.actorOverflowTraceOmittedCount > 0 && ShouldTraceActorOverflow())
	{
		Printf(
			"PERF pt actor overflow NRI: frame=%llu reason=%s omitted=%u limit=%u\n",
			(unsigned long long)mFrameIndex,
			mLastPerfShellTraceStats.sceneTextureReason.empty() ? "none" : mLastPerfShellTraceStats.sceneTextureReason.c_str(),
			mLastPerfShellTraceStats.actorOverflowTraceOmittedCount,
			NRI_MAX_ACTOR_OVERFLOW_TRACE_LINES);
	}

	const bool sceneTextureOverflow =
		mSceneTextures.OverflowStats().truncatedTextureCountLastBuild > 0 ||
		mSceneTextures.OverflowStats().baseTextureClampCountLastBuild > 0 ||
		mSceneTextures.OverflowStats().normalTextureClampCountLastBuild > 0 ||
		mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild > 0 ||
		mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild > 0 ||
		mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild > 0;
	if (sceneTextureOverflow)
	{
		mSceneTextures.OverflowStats().totalOverflowBuilds++;
		if (!mSceneTextures.OverflowStats().warningLogged || (int)nri_pttraceframes > 0 || (int)nri_ptactorspritetrace > 0 || nri_ptdebug > 0)
		{
			Printf(TEXTCOLOR_ORANGE "NRI PT scene textures: requested=%u cap=%u truncated=%u clamps=base:%u normal:%u metallic:%u roughness:%u emissive:%u\n",
				mSceneTextures.OverflowStats().textureCountLastBuild,
				NRI_MAX_SCENE_TEXTURES,
				mSceneTextures.OverflowStats().truncatedTextureCountLastBuild,
				mSceneTextures.OverflowStats().baseTextureClampCountLastBuild,
				mSceneTextures.OverflowStats().normalTextureClampCountLastBuild,
				mSceneTextures.OverflowStats().metallicTextureClampCountLastBuild,
				mSceneTextures.OverflowStats().roughnessTextureClampCountLastBuild,
				mSceneTextures.OverflowStats().emissiveTextureClampCountLastBuild);
			mSceneTextures.OverflowStats().warningLogged = true;
		}
	}

	mSceneTextures.CacheStats().cacheEntriesLastBuild = mSceneTextures.CacheCount();
	mSceneTextures.CacheStats().cacheEntriesHighWater = std::max(mSceneTextures.CacheStats().cacheEntriesHighWater, mSceneTextures.CacheCount());
	mSceneTextures.CacheStats().lookupMissesLastBuild = lookupMisses;
	mSceneTextures.CacheStats().insertCountLastBuild = insertCount;
	mSceneTextures.CacheStats().lookupMsLastBuild = lookupMs;
	mSceneTextures.CacheStats().realizeMsLastBuild = realizeMs;
	mLastPerfShellTraceStats.sceneTextureCacheCount = mSceneTextures.CacheCount();
	mLastPerfShellTraceStats.sceneTextureCacheMisses = lookupMisses;
	mLastPerfShellTraceStats.sceneTextureCacheInserts = insertCount;
	mLastPerfShellTraceStats.sceneTextureLookupMs = lookupMs;
	mLastPerfShellTraceStats.sceneTextureRealizeMs = realizeMs;
	bool updated = false;
	if (tracePerf)
	{
		const auto descriptorStart = std::chrono::steady_clock::now();
		updated = UpdateSceneTextureSet(descriptors, reason);
		descriptorMs = SceneTextureDurationMs(descriptorStart, std::chrono::steady_clock::now());
	}
	else
	{
		updated = UpdateSceneTextureSet(descriptors, reason);
	}
	mSceneTextures.CacheStats().descriptorMsLastBuild = descriptorMs;
	mLastPerfShellTraceStats.sceneTextureDescriptorMs = descriptorMs;
	if (useCachedDescriptorProduct)
	{
		mLastPerfShellTraceStats.sceneReuseTextureHit = updated ? 1u : 0u;
		mLastPerfShellTraceStats.sceneReuseTextureBuild = 0;
		mLastPerfShellTraceStats.sceneReuseTextureReject = updated ? 0u : 1u;
		mLastPerfShellTraceStats.sceneReuseTextureDynamicCount = (uint32_t)dynamicDependencies.size();
	}
	if (reuseOwnerEligible && updated && !useCachedDescriptorProduct && pendingTextureSlots.empty())
	{
		const uint64_t currentMappingRevision = mSceneTextures.SlotTable().MappingRevision();
		NRISceneTextureFrameProductKey storedKey = reuseKey;
		storedKey.slotMappingRevision = currentMappingRevision;
		NRISceneTextureFrameProductTelemetry productTelemetry = {};
		productTelemetry.requestedTextureCount = mLastPerfShellTraceStats.sceneTextureRequestedCount;
		productTelemetry.referencedActorMaterialCount = mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount;
		productTelemetry.referencedBaseCount = mLastPerfShellTraceStats.sceneTextureReferencedBaseCount;
		productTelemetry.referencedGlowCount = mLastPerfShellTraceStats.sceneTextureReferencedGlowCount;
		productTelemetry.referencedNormalCount = mLastPerfShellTraceStats.sceneTextureReferencedNormalCount;
		productTelemetry.referencedMetallicCount = mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount;
		productTelemetry.referencedRoughnessCount = mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount;
		productTelemetry.referencedEmissiveCount = mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount;
		productTelemetry.truncatedTextureCount = mSceneTextures.OverflowStats().truncatedTextureCountLastBuild;
		productTelemetry.stableDescriptorHits = mLastPerfShellTraceStats.sceneTextureStableDescriptorHits;

		if (reuseProduct != nullptr && reuseInputs->validateReuse)
		{
			bool descriptorTemplateMatches = reuseProduct->descriptorTemplate.size() == descriptors.size();
			if (descriptorTemplateMatches)
			{
				std::vector<uint8_t> dynamicDescriptorSlots(descriptors.size(), 0u);
				for (const NRISceneTextureDynamicDependency& dependency : dynamicDependencies)
				{
					if (dependency.descriptorIndex < dynamicDescriptorSlots.size())
						dynamicDescriptorSlots[dependency.descriptorIndex] = 1u;
				}
				for (size_t descriptorIndex = 2; descriptorIndex < descriptors.size(); ++descriptorIndex)
				{
					if (dynamicDescriptorSlots[descriptorIndex] == 0u &&
						reuseProduct->descriptorTemplate[descriptorIndex] != descriptors[descriptorIndex])
					{
						descriptorTemplateMatches = false;
						break;
					}
				}
			}
			const bool dynamicDependenciesMatch =
				reuseProduct->dynamicDependencies.size() == dynamicDependencies.size() &&
				(reuseProduct->dynamicDependencies.empty() ||
				 std::memcmp(
					reuseProduct->dynamicDependencies.data(),
					dynamicDependencies.data(),
					dynamicDependencies.size() * sizeof(dynamicDependencies[0])) == 0);
			const bool validationMismatch =
				!descriptorTemplateMatches ||
				!dynamicDependenciesMatch ||
				reuseProduct->stableSlotMode != mSceneTextureStableSlotsActive ||
				reuseProduct->key.slotMappingRevision != currentMappingRevision;
			mLastPerfShellTraceStats.sceneReuseTextureValidationChecked = 1;
			mLastPerfShellTraceStats.sceneReuseTextureValidationMismatch = validationMismatch ? 1u : 0u;
		}

		mSceneTextureFrameCache.Store(
			storedKey,
			materials,
			descriptors,
			dynamicDependencies,
			productTelemetry,
			mSceneTextureStableSlotsActive);
		mLastPerfShellTraceStats.sceneReuseTextureKey = mSceneTextureFrameCache.LastTraceKey();
		mLastPerfShellTraceStats.sceneReuseTextureDynamicCount = (uint32_t)dynamicDependencies.size();
	}
	if (updated && &materials == &mStaticMapScene.materialBridge)
	{
		// The resident static material buffer can outlive this call and later be
		// patched by runtime mutation handling. Record the index namespace that
		// was actually published instead of consulting the transient mode of a
		// subsequent texture owner.
		mStaticMapScene.gpuMaterialsUseStableTextureSlots = mSceneTextureStableSlotsActive;
	}
	return updated;
}

bool NRIRenderer::UseFallbackSceneTextures(bool preserveExistingSky, const char* reason)
{
	mSceneTextureStableSlotsActive = false;
	mSceneTextures.ClearLiveResources();
	mLastPerfShellTraceStats.sceneTextureReason = reason != nullptr ? reason : "fallback";
	mLastPerfShellTraceStats.sceneTextureRequestedCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedActorMaterialCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedBaseCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedGlowCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedNormalCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedMetallicCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedRoughnessCount = 0;
	mLastPerfShellTraceStats.sceneTextureReferencedEmissiveCount = 0;
	mLastPerfShellTraceStats.actorOverflowMaterialCount = 0;
	mLastPerfShellTraceStats.actorOverflowBaseClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowNormalClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowMetallicClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowRoughnessClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowEmissiveClampCount = 0;
	mLastPerfShellTraceStats.actorOverflowTraceOmittedCount = 0;
	if (!preserveExistingSky || GetActiveSkyTexture() == nullptr)
	{
		EnsureSkyTexture(nri_scene::SceneView{}, false);
	}
	std::vector<nri::Descriptor*> descriptors(NRI_SCENE_DESCRIPTOR_NUM, mFrameBuffer->mWhiteTexture->GetResource().shaderView);
	descriptors[0] = mFrameBuffer->mWhiteTexture->GetResource().shaderView;
	descriptors[1] = GetActiveSkyTexture() != nullptr && GetActiveSkyTexture()->shaderView != nullptr ? GetActiveSkyTexture()->shaderView : mFrameBuffer->mWhiteTexture->GetResource().shaderView;
	return UpdateSceneTextureSet(descriptors, reason != nullptr ? reason : "fallback");
}

uint32_t NRISceneTextureResidency::FindCacheIndex(uint64_t key) const
{
	const auto it = mTextureCacheKeyIndex.find(key);
	if (it == mTextureCacheKeyIndex.end())
	{
		return UINT32_MAX;
	}

	const uint32_t cacheIndex = it->second;
	if (cacheIndex >= mTextureCache.size() || mTextureCache[cacheIndex].key != key)
	{
		return UINT32_MAX;
	}

	return cacheIndex;
}

uint32_t NRISceneTextureResidency::FindReadyCacheIndex(uint64_t key) const
{
	const uint32_t cacheIndex = FindCacheIndex(key);
	if (cacheIndex == UINT32_MAX)
	{
		return UINT32_MAX;
	}

	const NRISceneCachedTexture& cached = mTextureCache[cacheIndex];
	return cached.uploadFenceValue == 0 &&
		cached.resource.texture != nullptr &&
		cached.resource.shaderView != nullptr ? cacheIndex : UINT32_MAX;
}

NRISceneTextureResidency::CachedTextureReadiness NRISceneTextureResidency::PollCachedTexture(
	NRIRenderDevice& device,
	uint32_t cacheIndex,
	NRISceneTextureClosureResult& outResult)
{
	outResult = {};
	if (cacheIndex >= mTextureCache.size())
	{
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::ResidencyLost;
		return CachedTextureReadiness::Failed;
	}

	NRISceneCachedTexture& cached = mTextureCache[cacheIndex];
	outResult.key = cached.key;
	outResult.residencyIndex = cacheIndex;
	if (cached.uploadFenceValue != 0)
	{
		if (device.IsCommandFenceValueAbandoned(cached.uploadFenceValue))
		{
			NoteTextureFirstUse(
				cached.firstUseEventId, cached.key, device.mFrameIndex + 1ull, device.mCurrentQueuedFrameIndex,
				cached.uploadFenceValue, 0, cached.estimatedUploadBytes, 0.0,
				PerfCompactFirstUseStage::UploadComplete, PerfCompactFirstUseState::Cancelled,
				PerfCompactFirstUseEnd, cached.firstUseRequestFrame);
			cached.firstUseEventId = 0;
			outResult.state = NRISceneTextureClosureState::Deferred;
			outResult.failure = NRISceneTextureClosureFailure::ResourceCreation;
			InvalidateCachedTexture(device, cacheIndex);
			return CachedTextureReadiness::Abandoned;
		}
		if (!device.IsCommandFenceValueComplete(cached.uploadFenceValue))
		{
			outResult.state = NRISceneTextureClosureState::Pending;
			return CachedTextureReadiness::Pending;
		}
		const uint64_t completedFenceValue = cached.uploadFenceValue;
		cached.uploadFenceValue = 0;
		NoteTextureFirstUse(
			cached.firstUseEventId, cached.key, device.mFrameIndex + 1ull, device.mCurrentQueuedFrameIndex,
			completedFenceValue, device.mFrameIndex + 1ull, cached.estimatedUploadBytes, 0.0,
			PerfCompactFirstUseStage::UploadComplete, PerfCompactFirstUseState::Ready, 0,
			cached.firstUseRequestFrame);
	}

	outResult.descriptorReady = cached.resource.shaderView != nullptr;
	if (cached.resource.texture == nullptr)
	{
		NoteTextureFirstUse(
			cached.firstUseEventId, cached.key, device.mFrameIndex + 1ull, device.mCurrentQueuedFrameIndex,
			0, 0, cached.estimatedUploadBytes, 0.0,
			PerfCompactFirstUseStage::Publication, PerfCompactFirstUseState::Failed,
			PerfCompactFirstUseEnd, cached.firstUseRequestFrame);
		cached.firstUseEventId = 0;
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::ResidencyLost;
		return CachedTextureReadiness::Failed;
	}
	if (!outResult.descriptorReady)
	{
		NoteTextureFirstUse(
			cached.firstUseEventId, cached.key, device.mFrameIndex + 1ull, device.mCurrentQueuedFrameIndex,
			0, 0, cached.estimatedUploadBytes, 0.0,
			PerfCompactFirstUseStage::Publication, PerfCompactFirstUseState::Failed,
			PerfCompactFirstUseEnd, cached.firstUseRequestFrame);
		cached.firstUseEventId = 0;
		outResult.state = NRISceneTextureClosureState::Failed;
		outResult.failure = NRISceneTextureClosureFailure::DescriptorUnavailable;
		return CachedTextureReadiness::Failed;
	}

	outResult.state = NRISceneTextureClosureState::Ready;
	if (cached.firstUseEventId != 0)
	{
		NoteTextureFirstUse(
			cached.firstUseEventId, cached.key, device.mFrameIndex + 1ull, device.mCurrentQueuedFrameIndex,
			0, device.mFrameIndex + 1ull, cached.estimatedUploadBytes, 0.0,
			PerfCompactFirstUseStage::Publication, PerfCompactFirstUseState::Ready,
			PerfCompactFirstUseEnd, cached.firstUseRequestFrame);
		cached.firstUseEventId = 0;
	}
	return CachedTextureReadiness::Ready;
}

void NRISceneTextureResidency::InvalidateCachedTexture(NRIRenderDevice& device, uint32_t cacheIndex)
{
	if (cacheIndex >= mTextureCache.size())
	{
		return;
	}

	NRISceneCachedTexture& cached = mTextureCache[cacheIndex];
	if (cached.firstUseEventId != 0)
	{
		NoteTextureFirstUse(
			cached.firstUseEventId, cached.key, device.mFrameIndex + 1ull, device.mCurrentQueuedFrameIndex,
			cached.uploadFenceValue, 0, cached.estimatedUploadBytes, 0.0,
			PerfCompactFirstUseStage::Publication, PerfCompactFirstUseState::Cancelled,
			PerfCompactFirstUseEnd, cached.firstUseRequestFrame);
		cached.firstUseEventId = 0;
	}
	const auto indexed = mTextureCacheKeyIndex.find(cached.key);
	if (indexed != mTextureCacheKeyIndex.end() && indexed->second == cacheIndex)
	{
		mTextureCacheKeyIndex.erase(indexed);
	}
	device.RetireTextureResource(cached.resource);
	cached = {};
}

uint32_t NRISceneTextureResidency::AddCachedTexture(NRISceneCachedTexture&& texture)
{
	for (uint32_t cacheIndex = 0; cacheIndex < mTextureCache.size(); ++cacheIndex)
	{
		NRISceneCachedTexture& cached = mTextureCache[cacheIndex];
		if (cached.key == 0 && cached.resource.texture == nullptr)
		{
			cached = std::move(texture);
			mTextureCacheKeyIndex[cached.key] = cacheIndex;
			return cacheIndex;
		}
	}

	const uint32_t cacheIndex = (uint32_t)mTextureCache.size();
	mTextureCacheKeyIndex[texture.key] = cacheIndex;
	mTextureCache.push_back(std::move(texture));
	return cacheIndex;
}

void NRISceneTextureResidency::ClearLiveResources()
{
	mLiveResources.clear();
}

void NRISceneTextureResidency::TrackLiveResource(NRITextureResource& resource)
{
	if (resource.texture == nullptr)
	{
		return;
	}

	for (NRITextureResource* existing : mLiveResources)
	{
		if (existing == &resource)
		{
			return;
		}
	}

	mLiveResources.push_back(&resource);
}

void NRISceneTextureResidency::ClearCachedTextures(NRIRenderDevice* device)
{
	const uint64_t rendererFrame = device != nullptr ? device->mFrameIndex + 1ull : 0;
	const uint32_t queuedSlot = device != nullptr ? device->mCurrentQueuedFrameIndex : UINT32_MAX;
	for (NRISceneCachedTexture& cached : mTextureCache)
	{
		if (cached.firstUseEventId == 0)
		{
			continue;
		}
		NoteTextureFirstUse(
			cached.firstUseEventId, cached.key,
			rendererFrame != 0 ? rendererFrame : cached.firstUseRequestFrame,
			queuedSlot != UINT32_MAX ? queuedSlot : cached.firstUseQueuedSlot,
			cached.uploadFenceValue, 0, cached.estimatedUploadBytes, 0.0,
			PerfCompactFirstUseStage::Publication, PerfCompactFirstUseState::Cancelled,
			PerfCompactFirstUseEnd, cached.firstUseRequestFrame);
		cached.firstUseEventId = 0;
	}
	mTextureCache.clear();
	mTextureCacheKeyIndex.clear();
	mStableSlotDescriptors.clear();
	mLiveResources.clear();
}
