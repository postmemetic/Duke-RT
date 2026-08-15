#include "nri_renderer.h"
#include "nri_acceleration.h"
#include "nri_cvars.h"
#include "nri_frame_resources.h"
#include "nri_upload_hash.h"

#include <algorithm>
#include <cstdint>

namespace
{
	static constexpr size_t MaxDynamicOverlayBlasAssets = 64;
	static constexpr uint32_t MaxFilterCandidateBlasBuildsPerFrame = 6;

	static bool IsNonEmptyPrimitiveSpan(const NRIRenderer::SceneBufferUploadDomainSpan& span)
	{
		return span.primitiveCount != 0 || span.indexCount != 0 || span.vertexCount != 0;
	}

	static uint64_t BuildDynamicOverlayBlasKey(
		const NRIRenderer::SceneBufferUploadDomainSpan& span,
		const std::vector<nri_scene::SceneVertex>& vertices,
		const std::vector<uint32_t>& indices)
	{
		uint64_t key = 1469598103934665603ull;
		key = NRIHashCombine64(key, (uint64_t)span.domain);
		key = NRIHashCombine64(key, (uint64_t)span.vertexCount);
		key = NRIHashCombine64(key, (uint64_t)span.indexCount);
		key = NRIHashCombine64(key, (uint64_t)span.primitiveCount);
		// A BLAS depends only on geometry build input. Producer stamps also
		// cover primitive/material publication and can conservatively advance
		// every frame (notably for local-player reflection capture), which would
		// rebuild an identical AS. Hash the exact compacted vertices and indices
		// below; primitive/material changes remain scene-buffer/TLAS concerns.
		key = NRIHashCombine64(key, NRIHashUploadPayloadBytes(vertices.data(), (uint64_t)vertices.size() * sizeof(nri_scene::SceneVertex)));
		key = NRIHashCombine64(key, NRIHashUploadPayloadBytes(indices.data(), (uint64_t)indices.size() * sizeof(uint32_t)));
		return key != 0 ? key : 1;
	}
}

void NRIRenderer::ResetDynamicOverlayBlasCache()
{
	for (DynamicOverlayBlasAsset& asset : mDynamicOverlayBlasAssets)
	{
		DestroyAccelerationStructureResource(asset.accelerationStructure);
		DestroyBufferResource(asset.vertexBuffer);
		DestroyBufferResource(asset.indexBuffer);
	}
	mDynamicOverlayBlasAssets.clear();
	mSelectedDynamicOverlayBlasOccurrences.clear();
	mDynamicOverlayBlasVertexScratch.clear();
	mDynamicOverlayBlasIndexScratch.clear();
}

bool NRIRenderer::BuildDynamicOverlayBlasRoute(
	const nri_scene::GeometryData& geometry,
	const std::vector<SceneBufferUploadDomainSpan>& uploadSpans,
	DynamicOverlayBlasRoute& outRoute)
{
	outRoute = {};

	const bool filterPartitionEnabled = (bool)nri_ptfilterquery;
	const bool buildEnabled = (bool)nri_ptdynamicoverlayblasbuild || filterPartitionEnabled;
	const bool routeEnabled = (bool)nri_ptdynamicoverlayblasroute || filterPartitionEnabled;
	const uint32_t buildBudget = (uint32_t)std::max(0, (int)nri_ptdynamicoverlayblasbuilds);
	uint32_t remainingBuildBudget = buildBudget;
	if (!buildEnabled && !routeEnabled)
	{
		return true;
	}

	std::vector<const SceneBufferUploadDomainSpan*> selectedSpans;
	for (const SceneBufferUploadDomainSpan& span : uploadSpans)
	{
		if (!IsNonEmptyPrimitiveSpan(span))
		{
			continue;
		}

		// The legacy experiment routes only a sole ordinary dynamic domain.
		// Phase 23 partitions every contiguous upload domain so an exact
		// reflection-only occurrence can be marked without duplicating geometry.
		if (!filterPartitionEnabled && span.domain != SceneBufferUploadDomain::Dynamic)
		{
			return true;
		}
		selectedSpans.push_back(&span);
	}

	if (selectedSpans.empty() || (!filterPartitionEnabled && selectedSpans.size() != 1))
	{
		return true;
	}
	if (filterPartitionEnabled)
	{
		// The filter route is atomic: a residual monolithic BLAS is not part of
		// this bounded slice. Cover the six known overlay producer domains in
		// one warm-up frame or fail open, rather than spending the legacy
		// one-build experiment budget forever without ever publishing a route.
		if (selectedSpans.size() > MaxFilterCandidateBlasBuildsPerFrame)
		{
			return true;
		}
		remainingBuildBudget = (uint32_t)selectedSpans.size();
	}
	mLastPerfShellTraceStats.dynamicOverlayBlasBuildBudget = remainingBuildBudget;
	// Route occurrences retain pointers into this cache until TLAS assembly.
	// Fix the cache capacity before collecting them so later span insertions do
	// not invalidate an earlier occurrence in the same frame.
	if (mDynamicOverlayBlasAssets.capacity() < MaxDynamicOverlayBlasAssets)
	{
		mDynamicOverlayBlasAssets.reserve(MaxDynamicOverlayBlasAssets);
	}

	std::vector<uint64_t> selectedKeys;
	selectedKeys.reserve(selectedSpans.size());
	for (const SceneBufferUploadDomainSpan* spanPtr : selectedSpans)
	{
		const SceneBufferUploadDomainSpan& span = *spanPtr;
		if (span.vertexCount == 0 || span.indexCount == 0 || span.primitiveCount == 0)
		{
			outRoute = {};
			return true;
		}
		if (span.vertexOffset > geometry.vertices.size() ||
			span.indexOffset > geometry.indices.size() ||
			(uint64_t)span.vertexOffset + span.vertexCount > geometry.vertices.size() ||
			(uint64_t)span.indexOffset + span.indexCount > geometry.indices.size() ||
			(uint64_t)span.primitiveOffset + span.primitiveCount > geometry.primitives.size())
		{
			outRoute = {};
			return true;
		}

		mDynamicOverlayBlasVertexScratch.assign(
			geometry.vertices.begin() + span.vertexOffset,
			geometry.vertices.begin() + span.vertexOffset + span.vertexCount);
		mDynamicOverlayBlasIndexScratch.clear();
		mDynamicOverlayBlasIndexScratch.reserve(span.indexCount);
		const uint32_t vertexEnd = span.vertexOffset + span.vertexCount;
		for (uint32_t i = 0; i < span.indexCount; ++i)
		{
			const uint32_t index = geometry.indices[span.indexOffset + i];
			if (index < span.vertexOffset || index >= vertexEnd)
			{
				outRoute = {};
				return true;
			}
			mDynamicOverlayBlasIndexScratch.push_back(index - span.vertexOffset);
		}

		const uint64_t key = BuildDynamicOverlayBlasKey(span, mDynamicOverlayBlasVertexScratch, mDynamicOverlayBlasIndexScratch);
		auto found = std::find_if(mDynamicOverlayBlasAssets.begin(), mDynamicOverlayBlasAssets.end(),
			[key](const DynamicOverlayBlasAsset& asset)
			{
				return asset.key == key;
			});

		DynamicOverlayBlasAsset* asset = found != mDynamicOverlayBlasAssets.end() ? &*found : nullptr;
		if (asset != nullptr &&
			asset->accelerationStructure.accelerationStructure != nullptr &&
			asset->vertexBuffer.buffer != nullptr &&
			asset->indexBuffer.buffer != nullptr)
		{
			mLastPerfShellTraceStats.dynamicOverlayBlasCacheHits++;
			asset->lastUsedFrame = mFrameIndex;
		}
		else
		{
			mLastPerfShellTraceStats.dynamicOverlayBlasCacheMisses++;
			if (!buildEnabled || remainingBuildBudget == 0)
			{
				outRoute = {};
				return true;
			}

			remainingBuildBudget--;
			mLastPerfShellTraceStats.dynamicOverlayBlasBuildAttempts++;
			if (asset == nullptr)
			{
				if (mDynamicOverlayBlasAssets.size() >= MaxDynamicOverlayBlasAssets)
				{
					auto evictIt = std::min_element(mDynamicOverlayBlasAssets.begin(), mDynamicOverlayBlasAssets.end(),
						[](const DynamicOverlayBlasAsset& a, const DynamicOverlayBlasAsset& b)
						{
							return a.lastUsedFrame < b.lastUsedFrame;
						});
					if (evictIt != mDynamicOverlayBlasAssets.end())
					{
						RetireResidentAccelerationStructure(evictIt->accelerationStructure);
						RetireResidentBufferResource(evictIt->vertexBuffer);
						RetireResidentBufferResource(evictIt->indexBuffer);
						mDynamicOverlayBlasAssets.erase(evictIt);
					}
				}

				mDynamicOverlayBlasAssets.emplace_back();
				asset = &mDynamicOverlayBlasAssets.back();
				asset->key = key;
			}

			asset->vertexCount = span.vertexCount;
			asset->indexCount = span.indexCount;
			asset->primitiveCount = span.primitiveCount;
			asset->lastUsedFrame = mFrameIndex;

			const uint64_t vertexBytes = (uint64_t)mDynamicOverlayBlasVertexScratch.size() * sizeof(nri_scene::SceneVertex);
			const uint64_t indexBytes = (uint64_t)mDynamicOverlayBlasIndexScratch.size() * sizeof(uint32_t);
			const bool uploaded =
				EnsureResidentStructuredBuffer(
					asset->vertexBuffer,
					asset->vertexStats,
					mDynamicOverlayBlasVertexScratch.data(),
					vertexBytes,
					sizeof(nri_scene::SceneVertex),
					NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
					NRIResourceAccelerationStructureBuildInputAccess(),
					"dynamic-overlay-blas-vertex",
					ResidentUploadKind_Vertex) &&
				EnsureResidentStructuredBuffer(
					asset->indexBuffer,
					asset->indexStats,
					mDynamicOverlayBlasIndexScratch.data(),
					indexBytes,
					sizeof(uint32_t),
					NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT),
					NRIResourceAccelerationStructureBuildInputAccess(),
					"dynamic-overlay-blas-index",
					ResidentUploadKind_Index);
			if (!uploaded)
			{
				return false;
			}

			if (!BuildBottomLevelAccelerationStructure(
				asset->vertexBuffer,
				asset->indexBuffer,
				0u,
				span.vertexCount,
				0u,
				span.indexCount,
				span.primitiveCount,
				asset->accelerationStructure,
				false))
			{
				return false;
			}
			mLastPerfShellTraceStats.dynamicOverlayBlasBuildSuccesses++;
		}

		if (routeEnabled && asset != nullptr && asset->accelerationStructure.accelerationStructure != nullptr)
		{
			selectedKeys.push_back(key);
		}
	}

	for (size_t selectedIndex = 0; selectedIndex < selectedKeys.size(); ++selectedIndex)
	{
		const uint64_t key = selectedKeys[selectedIndex];
		auto found = std::find_if(mDynamicOverlayBlasAssets.begin(), mDynamicOverlayBlasAssets.end(),
			[key](const DynamicOverlayBlasAsset& asset)
			{
				return asset.key == key && asset.accelerationStructure.accelerationStructure != nullptr;
			});
		if (found == mDynamicOverlayBlasAssets.end())
		{
			outRoute = {};
			return true;
		}
		DynamicOverlayBlasRoute::Occurrence occurrence = {};
		occurrence.accelerationStructure = &found->accelerationStructure;
		occurrence.span = *selectedSpans[selectedIndex];
		outRoute.occurrences.push_back(occurrence);
	}

	outRoute.routeAllOverlay = routeEnabled && outRoute.occurrences.size() == selectedSpans.size();
	if (outRoute.routeAllOverlay)
	{
		mLastPerfShellTraceStats.dynamicOverlayBlasRoutedInstances = (uint32_t)outRoute.occurrences.size();
		mLastPerfShellTraceStats.dynamicOverlayBlasFallbackDomains = 0;
		mLastPerfShellTraceStats.dynamicOverlayBlasFallbackPrimitives = 0;
		mLastPerfShellTraceStats.dynamicOverlayBlasMonolithicRefs = 0;
	}

	return true;
}
