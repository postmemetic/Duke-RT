#pragma once

#include "nri_scene_frame_coordinator_types.h"
#include "../scene/nri_retained_scratch.h"
#include "../scene/nri_scene_view_scratch.h"
#include "printf.h"

#include <algorithm>
#include <array>

inline void ResetRenderSceneFrameRetainingCapacity(RenderSceneFrameBuildResult& frame)
{
	nri_scene::ClearSceneViewRetainingCapacity(frame.capturedSceneView);
	nri_scene::ClearSceneViewRetainingCapacity(frame.dynamicSceneView);
	nri_scene::ClearSceneViewRetainingCapacity(frame.localPlayerReflectionSceneView);
	nri_scene::ClearSceneViewRetainingCapacity(frame.surfaceLightSceneView);
	nri_scene::ClearSceneViewRetainingCapacity(frame.sceneLightMergedDynamicSceneView);
	nri_scene::ClearSceneViewRetainingCapacity(frame.mergedDynamicSceneView);
	nri_scene::ClearGeometryRetainingCapacity(frame.capturedGeometry);
	nri_scene::ClearGeometryRetainingCapacity(frame.runtimeSpaceLinkGeometry);
	nri_scene::ClearGeometryRetainingCapacity(frame.dynamicGeometry);
	nri_scene::ClearGeometryRetainingCapacity(frame.mergedDynamicGeometry);
	nri_scene::ClearGeometryRetainingCapacity(frame.actorFilteredDynamicGeometry);
	nri_scene::ClearGeometryRetainingCapacity(frame.debugSphereGeometry);
	nri_scene::ClearGeometryRetainingCapacity(frame.surfaceLightGeometry);
	nri_scene::ClearGeometryRetainingCapacity(frame.runtimeMutationFrame.geometry);
	nri_scene::ClearMaterialBridgeRetainingCapacity(frame.materialBridge);
	nri_scene::ClearMaterialBridgeRetainingCapacity(frame.runtimeSpaceLinkMaterialBridge);
	nri_scene::ClearMaterialBridgeRetainingCapacity(frame.dynamicMaterialBridge);
	nri_scene::ClearMaterialBridgeRetainingCapacity(frame.localPlayerReflectionMaterialBridge);
	nri_scene::ClearMaterialBridgeRetainingCapacity(frame.sceneLightMergedDynamicMaterialBridge);
	nri_scene::ClearMaterialBridgeRetainingCapacity(frame.mergedDynamicMaterialBridge);
	nri_scene::ClearMaterialBridgeRetainingCapacity(frame.debugSphereMaterialBridge);
	nri_scene::ClearMaterialBridgeRetainingCapacity(frame.surfaceLightMaterialBridge);
	nri_scene::ClearMaterialBridgeRetainingCapacity(frame.combinedMaterialBridge);
	nri_scene::ClearMaterialBridgeRetainingCapacity(frame.runtimeMutationFrame.materialBridge);
	frame.runtimeMutationFrame.hasOverlay = false;
	frame.runtimeMutationFrame.residentStaticSceneChanged = false;
	frame.uploadDomainSpans.clear();
	frame.activeSceneView = nullptr;
	frame.activeGeometry = nullptr;
	frame.activeGpuMaterials = nullptr;
	frame.activeMaterialBridge = nullptr;
	frame.activeDynamicSceneView = nullptr;
	frame.activeDynamicGeometry = nullptr;
	frame.activeDynamicMaterials = nullptr;
	frame.activeStats = {};
	frame.paletteReady = true;
	frame.texturesReady = true;
	frame.buffersReady = true;
	frame.accelerationReady = true;
	frame.usingPersistentDynamicEmissiveCache = false;
}

struct NRISceneFrameScratchReset
{
	explicit NRISceneFrameScratchReset(RenderSceneFrameBuildResult& value) : frame(value)
	{
		ResetRenderSceneFrameRetainingCapacity(frame);
	}
	~NRISceneFrameScratchReset() { ResetRenderSceneFrameRetainingCapacity(frame); }
	RenderSceneFrameBuildResult& frame;
};

// Dense chunk IDs have an existing map-sized bound. Generation marks preserve
// first-occurrence order without allocating a hash node for every placed chunk.
class NRIStagedChunkScratch
{
public:
	void Begin(size_t size)
	{
		mMarks.resize(size, 0);
		if (++mGeneration == 0)
		{
			std::fill(mMarks.begin(), mMarks.end(), 0);
			mGeneration = 1;
		}
	}
	bool Insert(size_t chunk)
	{
		assert(chunk < mMarks.size());
		if (mMarks[chunk] == mGeneration) return false;
		mMarks[chunk] = mGeneration;
		return true;
	}
private:
	std::vector<uint64_t> mMarks;
	uint64_t mGeneration = 0;
};

struct NRISceneFrameScratch
{
	// Separate main/offscreen history lifetimes, and separate leases for any
	// nested call. Dispatch and completion consume each lease synchronously.
	NRIRetainedScratch<RenderSceneFrameBuildResult> views[2];
	NRIStagedChunkScratch stagedChunks;
};

class NRISceneFrameScratchTrace
{
	struct Capacities
	{
		std::array<size_t, 81> elements = {};
		uint64_t bytes = 0;
		size_t count = 0;
		template<class T> void Add(const std::vector<T>& values)
		{
			elements[count++] = values.capacity();
			bytes += values.capacity() * sizeof(T);
		}
	};
	static Capacities Read(const RenderSceneFrameBuildResult& frame)
	{
		Capacities result;
		result.Add(frame.capturedSceneView.opaqueWalls);
		result.Add(frame.capturedSceneView.opaqueFlats);
		result.Add(frame.capturedSceneView.opaqueSprites);
		result.Add(frame.dynamicSceneView.opaqueWalls);
		result.Add(frame.dynamicSceneView.opaqueFlats);
		result.Add(frame.dynamicSceneView.opaqueSprites);
		result.Add(frame.localPlayerReflectionSceneView.opaqueWalls);
		result.Add(frame.localPlayerReflectionSceneView.opaqueFlats);
		result.Add(frame.localPlayerReflectionSceneView.opaqueSprites);
		result.Add(frame.surfaceLightSceneView.opaqueWalls);
		result.Add(frame.surfaceLightSceneView.opaqueFlats);
		result.Add(frame.surfaceLightSceneView.opaqueSprites);
		result.Add(frame.sceneLightMergedDynamicSceneView.opaqueWalls);
		result.Add(frame.sceneLightMergedDynamicSceneView.opaqueFlats);
		result.Add(frame.sceneLightMergedDynamicSceneView.opaqueSprites);
		result.Add(frame.mergedDynamicSceneView.opaqueWalls);
		result.Add(frame.mergedDynamicSceneView.opaqueFlats);
		result.Add(frame.mergedDynamicSceneView.opaqueSprites);
		result.Add(frame.capturedGeometry.vertices);
		result.Add(frame.capturedGeometry.indices);
		result.Add(frame.capturedGeometry.primitives);
		result.Add(frame.capturedGeometry.primitiveProvenance);
		result.Add(frame.runtimeSpaceLinkGeometry.vertices);
		result.Add(frame.runtimeSpaceLinkGeometry.indices);
		result.Add(frame.runtimeSpaceLinkGeometry.primitives);
		result.Add(frame.runtimeSpaceLinkGeometry.primitiveProvenance);
		result.Add(frame.dynamicGeometry.vertices);
		result.Add(frame.dynamicGeometry.indices);
		result.Add(frame.dynamicGeometry.primitives);
		result.Add(frame.dynamicGeometry.primitiveProvenance);
		result.Add(frame.mergedDynamicGeometry.vertices);
		result.Add(frame.mergedDynamicGeometry.indices);
		result.Add(frame.mergedDynamicGeometry.primitives);
		result.Add(frame.mergedDynamicGeometry.primitiveProvenance);
		result.Add(frame.actorFilteredDynamicGeometry.vertices);
		result.Add(frame.actorFilteredDynamicGeometry.indices);
		result.Add(frame.actorFilteredDynamicGeometry.primitives);
		result.Add(frame.actorFilteredDynamicGeometry.primitiveProvenance);
		result.Add(frame.debugSphereGeometry.vertices);
		result.Add(frame.debugSphereGeometry.indices);
		result.Add(frame.debugSphereGeometry.primitives);
		result.Add(frame.debugSphereGeometry.primitiveProvenance);
		result.Add(frame.surfaceLightGeometry.vertices);
		result.Add(frame.surfaceLightGeometry.indices);
		result.Add(frame.surfaceLightGeometry.primitives);
		result.Add(frame.surfaceLightGeometry.primitiveProvenance);
		result.Add(frame.runtimeMutationFrame.geometry.vertices);
		result.Add(frame.runtimeMutationFrame.geometry.indices);
		result.Add(frame.runtimeMutationFrame.geometry.primitives);
		result.Add(frame.runtimeMutationFrame.geometry.primitiveProvenance);
		result.Add(frame.materialBridge.materials);
		result.Add(frame.materialBridge.lightMetadata);
		result.Add(frame.materialBridge.textures);
		result.Add(frame.runtimeSpaceLinkMaterialBridge.materials);
		result.Add(frame.runtimeSpaceLinkMaterialBridge.lightMetadata);
		result.Add(frame.runtimeSpaceLinkMaterialBridge.textures);
		result.Add(frame.dynamicMaterialBridge.materials);
		result.Add(frame.dynamicMaterialBridge.lightMetadata);
		result.Add(frame.dynamicMaterialBridge.textures);
		result.Add(frame.localPlayerReflectionMaterialBridge.materials);
		result.Add(frame.localPlayerReflectionMaterialBridge.lightMetadata);
		result.Add(frame.localPlayerReflectionMaterialBridge.textures);
		result.Add(frame.sceneLightMergedDynamicMaterialBridge.materials);
		result.Add(frame.sceneLightMergedDynamicMaterialBridge.lightMetadata);
		result.Add(frame.sceneLightMergedDynamicMaterialBridge.textures);
		result.Add(frame.mergedDynamicMaterialBridge.materials);
		result.Add(frame.mergedDynamicMaterialBridge.lightMetadata);
		result.Add(frame.mergedDynamicMaterialBridge.textures);
		result.Add(frame.debugSphereMaterialBridge.materials);
		result.Add(frame.debugSphereMaterialBridge.lightMetadata);
		result.Add(frame.debugSphereMaterialBridge.textures);
		result.Add(frame.surfaceLightMaterialBridge.materials);
		result.Add(frame.surfaceLightMaterialBridge.lightMetadata);
		result.Add(frame.surfaceLightMaterialBridge.textures);
		result.Add(frame.combinedMaterialBridge.materials);
		result.Add(frame.combinedMaterialBridge.lightMetadata);
		result.Add(frame.combinedMaterialBridge.textures);
		result.Add(frame.runtimeMutationFrame.materialBridge.materials);
		result.Add(frame.runtimeMutationFrame.materialBridge.lightMetadata);
		result.Add(frame.runtimeMutationFrame.materialBridge.textures);
		result.Add(frame.uploadDomainSpans);
		return result;
	}
public:
	NRISceneFrameScratchTrace(const RenderSceneFrameBuildResult& frame, uint32_t index, bool offscreen, bool enabled)
		: mFrame(frame), mIndex(index), mOffscreen(offscreen), mEnabled(enabled)
	{
		if (enabled) mBefore = Read(frame);
	}
	~NRISceneFrameScratchTrace()
	{
		if (!mEnabled) return;
		const auto after = Read(mFrame);
		uint32_t growths = 0;
		uint64_t growthBytes = 0;
		for (size_t i = 0; i < after.count; ++i)
			growths += after.elements[i] > mBefore.elements[i] ? 1u : 0u;
		if (after.bytes > mBefore.bytes) growthBytes = after.bytes - mBefore.bytes;
		Printf("PERF pt frame scratch NRI: frame=%u offscreen=%u vectors=%zu capacity_bytes=%llu vector_growths=%u retained_growth_bytes=%llu\n",
			mIndex, mOffscreen ? 1u : 0u, after.count, (unsigned long long)after.bytes,
			growths, (unsigned long long)growthBytes);
	}
private:
	const RenderSceneFrameBuildResult& mFrame;
	uint32_t mIndex;
	bool mOffscreen;
	bool mEnabled;
	Capacities mBefore;
};
