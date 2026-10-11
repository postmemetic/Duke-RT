#include "nri_scene_upload.h"

#include "nri_renderer.h"
#include "nri_cvars.h"
#include "nri_shader_contracts.h"
#include "c_cvars.h"

#include <algorithm>

bool NRISceneUploadManager::UpdateWallDecalBuffers(
	NRIRenderer& renderer, bool* ioWaitedForWrites, bool allowSceneDataRing)
{
	const nri_scene::WallDecalHeaderGpuData emptyHeader = {};
	const nri_scene::WallDecalGpuData emptyDecal = {};
	const auto& headers = renderer.mWallDecals.Headers();
	const auto& decals = renderer.mWallDecals.Records();
	if ((int)nri_pttraceframes > 0 && renderer.mWallDecalTraceFrameIndex != renderer.mFrameIndex)
	{
		renderer.mWallDecalTraceFrameIndex = renderer.mFrameIndex;
		uint32_t resolvedCount = 0;
		for (const auto& decal : decals)
			resolvedCount += decal.textureIndex < NRI_MAX_SCENE_TEXTURES ? 1u : 0u;
		Printf("NRI PT wall decals: frame=%u walls=%u records=%u resolved=%u\n",
			renderer.mFrameIndex, (uint32_t)headers.size(), (uint32_t)decals.size(), resolvedCount);
		for (size_t i = 0; i < std::min<size_t>(decals.size(), 16u); ++i)
		{
			const auto& decal = decals[i];
			Printf("NRI PT wall decal: frame=%u record=%u actor=%u wall=%u sector=%u texture=%u palette=%u alpha=%.3f flags=%u plane=(%.3f,%.3f,%.3f,%.3f) uv=(%.3f,%.3f,%.3f,%.3f)\n",
				renderer.mFrameIndex, (uint32_t)i, decal.actorIndex, decal.wallIndex, decal.sectorIndex,
				decal.textureIndex, decal.paletteIndex, decal.alpha, decal.flags,
				decal.plane[0], decal.plane[1], decal.plane[2], decal.plane[3],
				decal.uvBounds[0], decal.uvBounds[1], decal.uvBounds[2], decal.uvBounds[3]);
		}
	}
	const void* headerData = headers.empty() ? static_cast<const void*>(&emptyHeader) : headers.data();
	const void* decalData = decals.empty() ? static_cast<const void*>(&emptyDecal) : decals.data();
	const uint64_t headerSize = std::max<size_t>(headers.size(), 1u) * sizeof(emptyHeader);
	const uint64_t decalSize = std::max<size_t>(decals.size(), 1u) * sizeof(emptyDecal);

	NRISceneDataFrameSlot* slot =
		allowSceneDataRing && renderer.ShouldUseSceneDataFrameRing() ? &renderer.GetCurrentSceneDataFrameSlot() : nullptr;
	NRIBufferResource& headerBuffer = slot != nullptr ? slot->wallDecalHeaderBuffer : renderer.mWallDecalHeaderBuffer;
	NRIBufferResource& decalBuffer = slot != nullptr ? slot->wallDecalBuffer : renderer.mWallDecalBuffer;
	SceneBufferDebugStats& headerStats = slot != nullptr ? slot->wallDecalHeaderStats : renderer.mWallDecalHeaderBufferStats;
	SceneBufferDebugStats& decalStats = slot != nullptr ? slot->wallDecalStats : renderer.mWallDecalBufferStats;
	// Structured views expose the retained allocation, including a previous
	// map's larger wall domain. Clear its tail so an empty/smaller capture cannot
	// accidentally reuse old decal ranges through shader GetDimensions().
	const uint64_t headerStorageSize = GetNRIGrownBufferSize(headerBuffer.size, headerSize, sizeof(emptyHeader));
	std::vector<nri_scene::WallDecalHeaderGpuData> paddedHeaders;
	if (headerStorageSize > headerSize)
	{
		paddedHeaders.resize(headerStorageSize / sizeof(emptyHeader));
		std::copy(headers.begin(), headers.end(), paddedHeaders.begin());
		headerData = paddedHeaders.data();
	}

	const auto upload = [&](NRIBufferResource& buffer, SceneBufferDebugStats& stats,
		const void* data, uint64_t size, uint32_t stride)
	{
		WaitIfStructuredUpdateNeedsIt(renderer, buffer, data, size, stride, slot != nullptr ? nullptr : ioWaitedForWrites);
		return renderer.EnsureStructuredBuffer(
			buffer, stats, data, size, stride, nri::BufferUsageBits::SHADER_RESOURCE,
			NRIResourceComputeShaderResourceAccess(),
			slot != nullptr || (ioWaitedForWrites != nullptr && *ioWaitedForWrites), "scene_data_upload");
	};
	if (!upload(headerBuffer, headerStats, headerData, headerStorageSize, sizeof(emptyHeader)) ||
		!upload(decalBuffer, decalStats, decalData, decalSize, sizeof(emptyDecal)))
	{
		return false;
	}

	renderer.mSceneDataDescriptors[NRI_SCENE_DATA_WALL_DECAL_HEADER_SLOT] = headerBuffer.shaderView;
	renderer.mSceneDataDescriptors[NRI_SCENE_DATA_WALL_DECAL_SLOT] = decalBuffer.shaderView;
	if (SceneDataDescriptorsReady(renderer))
	{
		// A reused scene may keep its current queued descriptor set without a full
		// rebuild. Publish only the decal range so unrelated per-frame bindings
		// retain their existing ownership and this fresh capture is never deferred.
		const nri::Descriptor* descriptors[] = { headerBuffer.shaderView, decalBuffer.shaderView };
		nri::UpdateDescriptorRangeDesc update = {};
		update.descriptorSet = renderer.GetCurrentSceneDataSet();
		update.rangeIndex = 0;
		update.baseDescriptor = NRI_SCENE_DATA_WALL_DECAL_HEADER_SLOT;
		update.descriptors = descriptors;
		update.descriptorNum = 2;
		renderer.BuildResourceServices().context.core->UpdateDescriptorRanges(&update, 1);
		renderer.mLastPerfShellTraceStats.sceneDataSetDescriptorUpdateCount++;
		renderer.mSceneDataDescriptorGeneration++;
		renderer.mLastPerfShellTraceStats.sceneDataDescriptorGeneration = renderer.mSceneDataDescriptorGeneration;
	}
	return true;
}
