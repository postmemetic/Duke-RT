#include "nri_renderer.h"
#include "nri_acceleration.h"
#include "nri_cvars.h"
#include "nri_frame_resources.h"
#include "nri_filter_candidate_policy.h"
#include "nri_upload_hash.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace
{
	static constexpr size_t MaxDynamicOverlayBlasAssets = 128;
	static constexpr uint32_t MaxFilterCandidateRunsPerFrame = 32;

	struct PlannedDynamicOverlaySpan
	{
		NRIRenderer::SceneBufferUploadDomainSpan span = {};
		uint32_t filterPolicyMask = NRIFilterCandidatePolicy_None;
	};

	static bool IsNonEmptyPrimitiveSpan(const NRIRenderer::SceneBufferUploadDomainSpan& span)
	{
		return span.primitiveCount != 0 || span.indexCount != 0 || span.vertexCount != 0;
	}

	static uint64_t BuildDynamicOverlayBlasKey(
		const NRIRenderer::SceneBufferUploadDomainSpan& span,
		uint32_t filterPolicyMask,
		nri::AccelerationStructureBits buildFlags,
		const std::vector<nri_scene::SceneVertex>& vertices,
		const std::vector<uint32_t>& indices)
	{
		uint64_t key = 1469598103934665603ull;
		key = NRIHashCombine64(key, (uint64_t)span.domain);
		key = NRIHashCombine64(key, (uint64_t)span.vertexCount);
		key = NRIHashCombine64(key, (uint64_t)span.indexCount);
		key = NRIHashCombine64(key, (uint64_t)span.primitiveCount);
		key = NRIHashCombine64(key, (uint64_t)filterPolicyMask);
		key = NRIHashCombine64(key, (uint64_t)buildFlags);
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
	const std::vector<nri_scene::MaterialData>& materials,
	const std::vector<SceneBufferUploadDomainSpan>& uploadSpans,
	DynamicOverlayBlasRoute& outRoute)
{
	outRoute = {};

	const bool filterPartitionEnabled = (bool)nri_ptfilterquery;
	const bool buildEnabled = (bool)nri_ptdynamicoverlayblasbuild || filterPartitionEnabled;
	const bool routeEnabled = (bool)nri_ptdynamicoverlayblasroute || filterPartitionEnabled;
	const int32_t requestedPolicy = (int)nri_ptdynamicoverlayblaspolicy;
	const uint32_t effectivePolicy = requestedPolicy == 1 ? 1u : 0u;
	const nri::AccelerationStructureBits buildFlags = effectivePolicy == 1u ?
		nri::AccelerationStructureBits::PREFER_FAST_TRACE : nri::AccelerationStructureBits::PREFER_FAST_BUILD;
	NRIDynamicOverlayBlasPolicyStats& policyStats = mLastPerfShellTraceStats.dynamicOverlayBlasPolicy;
	policyStats.requestedPolicy = requestedPolicy;
	policyStats.effectivePolicy = effectivePolicy;
	policyStats.buildFlags = (uint32_t)buildFlags;
	policyStats.requestedBuild = (bool)nri_ptdynamicoverlayblasbuild;
	policyStats.requestedRoute = (bool)nri_ptdynamicoverlayblasroute;
	policyStats.filterPartition = filterPartitionEnabled;
	policyStats.effectiveBuild = buildEnabled;
	policyStats.effectiveRoute = routeEnabled;
	policyStats.cacheLimit = (uint32_t)MaxDynamicOverlayBlasAssets;
	// The snapshot is refreshed on every exit, including disabled/fallback
	// routes. This bounded scan never grows with historical map churn.
	struct CacheSnapshotScope
	{
		const std::vector<DynamicOverlayBlasAsset>& assets;
		const NRIBufferResource& sharedScratch;
		uint64_t frame;
		NRIDynamicOverlayBlasPolicyStats& stats;
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
		~CacheSnapshotScope()
		{
			stats.cachedAssets = (uint32_t)assets.size();
			stats.touchedAssets = 0u;
			stats.cachedAsBytes = 0u;
			stats.cachedGeometryBytes = 0u;
			stats.touchedAsBytes = 0u;
			for (const DynamicOverlayBlasAsset& asset : assets)
			{
				stats.cachedAsBytes += asset.accelerationStructure.memorySize;
				stats.cachedGeometryBytes += asset.vertexBuffer.memorySize + asset.indexBuffer.memorySize;
				if (asset.lastUsedFrame == frame)
				{
					stats.touchedAssets++;
					stats.touchedAsBytes += asset.accelerationStructure.memorySize;
				}
			}
			stats.sharedScratchBytes = sharedScratch.memorySize;
			stats.totalCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		}
	} cacheSnapshotScope { mDynamicOverlayBlasAssets, mScratchBuffer, mFrameIndex, policyStats };
	const uint32_t buildBudget = (uint32_t)std::max(0, (int)nri_ptdynamicoverlayblasbuilds);
	uint32_t remainingBuildBudget = buildBudget;
	if (!buildEnabled && !routeEnabled)
	{
		return true;
	}

	std::vector<PlannedDynamicOverlaySpan> selectedSpans;
	std::vector<NRIFilterCandidateRun> filterRuns;
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
		if (filterPartitionEnabled)
		{
			const uint32_t remainingRunCapacity = MaxFilterCandidateRunsPerFrame - (uint32_t)selectedSpans.size();
			if (!BuildFilterCandidateRuns(
				geometry,
				materials,
				span.primitiveOffset,
				span.primitiveCount,
				span.indexOffset,
				span.indexCount,
				(uint32_t)std::max(0, (int)nri_ptfilterpolicymask),
				remainingRunCapacity,
				filterRuns))
			{
				return true;
			}
			for (const NRIFilterCandidateRun& run : filterRuns)
			{
				PlannedDynamicOverlaySpan planned = {};
				planned.span = span;
				planned.span.primitiveOffset = run.primitiveOffset;
				planned.span.primitiveCount = run.primitiveCount;
				planned.span.indexOffset = run.indexOffset;
				planned.span.indexCount = run.indexCount;
				planned.filterPolicyMask = run.policyMask;
				selectedSpans.push_back(planned);
			}
		}
		else
		{
			PlannedDynamicOverlaySpan planned = {};
			planned.span = span;
			selectedSpans.push_back(planned);
		}
	}

	if (selectedSpans.empty() || (!filterPartitionEnabled && selectedSpans.size() != 1))
	{
		return true;
	}
	if (filterPartitionEnabled)
	{
		// The filter route is atomic. Fragmentation beyond the bounded run cap
		// fails open to the monolithic legacy BLAS instead of publishing gaps.
		if (selectedSpans.size() > MaxFilterCandidateRunsPerFrame)
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
	for (const PlannedDynamicOverlaySpan& planned : selectedSpans)
	{
		const SceneBufferUploadDomainSpan& span = planned.span;
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

		const uint64_t key = BuildDynamicOverlayBlasKey(span, planned.filterPolicyMask, buildFlags, mDynamicOverlayBlasVertexScratch, mDynamicOverlayBlasIndexScratch);
		auto found = std::find_if(mDynamicOverlayBlasAssets.begin(), mDynamicOverlayBlasAssets.end(),
			[key](const DynamicOverlayBlasAsset& asset)
			{
				return asset.key == key;
			});

		DynamicOverlayBlasAsset* asset = found != mDynamicOverlayBlasAssets.end() ? &*found : nullptr;
		if (asset != nullptr &&
			asset->accelerationStructure.accelerationStructure != nullptr &&
			asset->accelerationStructure.buildFlags == buildFlags &&
			asset->vertexBuffer.buffer != nullptr &&
			asset->indexBuffer.buffer != nullptr)
		{
			mLastPerfShellTraceStats.dynamicOverlayBlasCacheHits++;
			const uint64_t age = mFrameIndex >= asset->lastUsedFrame ? mFrameIndex - asset->lastUsedFrame : 0u;
			policyStats.cacheHitAgeSumFrames += age;
			policyStats.cacheHitAgeMaxFrames = (uint32_t)std::max((uint64_t)policyStats.cacheHitAgeMaxFrames, std::min(age, (uint64_t)UINT32_MAX));
			asset->lastUsedFrame = mFrameIndex;
		}
		else
		{
			const auto coldStart = std::chrono::steady_clock::now();
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
				false,
				nullptr,
				buildFlags,
				true))
			{
				return false;
			}
			mLastPerfShellTraceStats.dynamicOverlayBlasBuildSuccesses++;
			policyStats.coldCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - coldStart).count();
			policyStats.builtAsBytes += asset->accelerationStructure.memorySize;
			policyStats.buildScratchMaxBytes = std::max(policyStats.buildScratchMaxBytes, asset->accelerationStructure.buildScratchSize);
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
		occurrence.span = selectedSpans[selectedIndex].span;
		occurrence.filterPolicyMask = selectedSpans[selectedIndex].filterPolicyMask;
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
