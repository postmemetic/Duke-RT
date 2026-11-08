#include "nri_persistent_voxels.h"
#include "nri_renderer.h"
#include "../scene/nri_cpu_perf_audit.h"
#include "../scene/nri_voxel_material_slots.h"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

namespace
{
using AuditRange = std::pair<uint64_t, uint64_t>;

// Aliases can share arena slices. Sum the union, never one allocation per key.
uint64_t CoveredBytes(std::vector<AuditRange>& ranges, uint64_t cursor, uint64_t stride)
{
	std::sort(ranges.begin(), ranges.end());
	uint64_t covered = 0, end = 0;
	for (const auto& range : ranges)
	{
		const uint64_t begin = std::min(cursor, range.first);
		const uint64_t next = std::min(cursor, range.second);
		if (next > std::max(begin, end)) covered += next - std::max(begin, end);
		end = std::max(end, next);
	}
	return covered * stride;
}
}

void NRIRenderer::CapturePerfAuditVoxelCensus() const
{
	mPersistentVoxels.CapturePerfAuditCensus(mFrameIndex);
}

void NRIPersistentVoxelResidency::CapturePerfAuditCensus(uint32_t rendererFrameIndex) const
{
	if (!PerfAuditCensusRequested()) return;
	PerfAuditScope census(PerfAuditMetric::VoxelRegistryCensusNs);
	uint64_t meshCold = 0, meshBytes = 0, meshUnused = 0, meshUnusedBytes = 0;
	uint64_t materialCold = 0, materialRows = 0, materialBytes = 0, materialUnused = 0, materialUnusedRows = 0;
	uint64_t readyZeroRef = 0, readyZeroRefBytes = 0, pinned = 0, policyMaterials = 0, highestActiveRow = 0;
	uint64_t actorBytes = 0;
	uint64_t privateGeometryBytes = 0, admissionBufferBytes = 0, admissionAsBytes = 0, admissionCpuBytes = 0;
	uint64_t capturedActors = 0, retainedActors = 0;
	uint64_t meshProtected = 0, materialProtected = 0;
	std::vector<AuditRange> vertices, indices, primitives;
	vertices.reserve(meshVariantResources.size());
	indices.reserve(meshVariantResources.size());
	primitives.reserve(meshVariantResources.size());
	std::unordered_set<uint64_t> payloadHashes;
	for (const auto& pair : meshVariantResources)
	{
		const auto& resource = pair.second;
		meshCold += resource.cold;
		meshBytes += resource.residentBytes;
		privateGeometryBytes += resource.vertexBuffer.memorySize + resource.indexBuffer.memorySize;
		pinned += resource.gpuForce;
		if (resource.activeActorReferences == 0)
		{
			if (resource.tlasPublished || resource.directComputePublished)
			{
				++readyZeroRef;
				readyZeroRefBytes += resource.residentBytes;
			}
			if (resource.lastDesiredMapGeneration == residencyMapGeneration)
			{
				++meshUnused;
				meshUnusedBytes += resource.residentBytes;
				meshProtected += !resource.cold;
			}
		}
		vertices.emplace_back(resource.vertexOffset, (uint64_t)resource.vertexOffset + resource.vertexCapacity);
		indices.emplace_back(resource.indexOffset, (uint64_t)resource.indexOffset + resource.indexCapacity);
		primitives.emplace_back(resource.primitiveOffset, (uint64_t)resource.primitiveOffset + resource.primitiveCapacity);
	}
	for (const auto& pair : materialVariantResources)
	{
		const auto& resource = pair.second;
		materialCold += resource.cold;
		materialRows += resource.materialCount;
		materialBytes += NRIAuditMaterialBridgeBytes(resource.materialBridge, true);
		policyMaterials += resource.materialCount == nri_scene::VoxelPaletteMaterialSlotCount;
		if (resource.materialPayloadHash != 0) payloadHashes.insert(resource.materialPayloadHash);
		if (resource.activeActorReferences != 0)
			highestActiveRow = std::max(highestActiveRow, (uint64_t)resource.materialOffset + resource.materialCount);
		else if (resource.lastDesiredMapGeneration == residencyMapGeneration)
		{
			++materialUnused;
			materialUnusedRows += resource.materialCount;
			materialProtected += !resource.cold;
		}
	}
	// In-flight reservations are still owned. Include them so unused prefix
	// does not silently classify admission work as abandoned storage.
	for (const auto& pair : admissionQueue)
	{
		const auto& entry = pair.second;
		const auto& resource = entry.uploadMeshResource;
		vertices.emplace_back(resource.vertexOffset, (uint64_t)resource.vertexOffset + resource.vertexCapacity);
		indices.emplace_back(resource.indexOffset, (uint64_t)resource.indexOffset + resource.indexCapacity);
		primitives.emplace_back(resource.primitiveOffset, (uint64_t)resource.primitiveOffset + resource.primitiveCapacity);
		admissionBufferBytes += resource.vertexBuffer.memorySize + resource.indexBuffer.memorySize + entry.directBlasScratchBuffer.memorySize;
		admissionAsBytes += resource.accelerationStructure.memorySize;
		admissionCpuBytes += entry.uploadGeometry.vertices.capacity() * sizeof(nri_scene::SceneVertex) +
			entry.uploadGeometry.indices.capacity() * sizeof(uint32_t) +
			entry.uploadGeometry.primitives.capacity() * sizeof(nri_scene::PrimitiveData) +
			entry.uploadGeometry.primitiveProvenance.capacity() * sizeof(nri_scene::SurfaceProvenance) +
			entry.uploadGpuIndices.capacity() * sizeof(uint32_t) +
			entry.uploadGpuPrimitives.capacity() * sizeof(nri_scene::PrimitiveData);
	}
	for (const auto& actor : batch.actors)
	{
		actorBytes += NRIAuditMaterialBridgeBytes(actor.materialBridge, true);
		if (actor.active)
		{
			capturedActors += actor.capturedThisFrame;
			retainedActors += !actor.capturedThisFrame;
		}
	}
	const auto& ranges = materialRangeAllocator.Stats();
	const uint64_t used = (uint64_t)arenaVertexCursor * sizeof(nri_scene::SceneVertex) +
		(uint64_t)arenaIndexCursor * sizeof(uint32_t) + (uint64_t)arenaPrimitiveCursor * sizeof(nri_scene::PrimitiveData);
	const uint64_t live = CoveredBytes(vertices, arenaVertexCursor, sizeof(nri_scene::SceneVertex)) +
		CoveredBytes(indices, arenaIndexCursor, sizeof(uint32_t)) + CoveredBytes(primitives, arenaPrimitiveCursor, sizeof(nri_scene::PrimitiveData));
	const auto compute = GetNRIVoxelComputeMemoryUsage();
	PerfAuditSet(PerfAuditMetric::VoxelActorCount, batch.actors.size());
	PerfAuditSet(PerfAuditMetric::VoxelActorCapacity, batch.actors.capacity());
	PerfAuditSet(PerfAuditMetric::VoxelActiveCount, batch.activeActorCount);
	PerfAuditSet(PerfAuditMetric::VoxelActorActiveCapturedCount, capturedActors);
	PerfAuditSet(PerfAuditMetric::VoxelActorActiveRetainedCount, retainedActors);
	PerfAuditSet(PerfAuditMetric::VoxelMeshResourceCount, meshVariantResources.size());
	PerfAuditSet(PerfAuditMetric::VoxelMeshResourceColdCount, meshCold);
	PerfAuditSet(PerfAuditMetric::VoxelMeshResourceBytes, meshBytes);
	PerfAuditSet(PerfAuditMetric::VoxelMeshCurrentMapUnusedCount, meshUnused);
	PerfAuditSet(PerfAuditMetric::VoxelMeshCurrentMapUnusedBytes, meshUnusedBytes);
	PerfAuditSet(PerfAuditMetric::VoxelMeshCurrentMapProtectedCount, meshProtected);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialResourceCount, materialVariantResources.size());
	PerfAuditSet(PerfAuditMetric::VoxelMaterialResourceColdCount, materialCold);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialResourceRows, materialRows);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialResourceCapacityBytes, materialBytes);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialCurrentMapUnusedCount, materialUnused);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialCurrentMapUnusedRows, materialUnusedRows);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialCurrentMapProtectedCount, materialProtected);
	uint64_t quarantinedMaterialCount = 0, quarantinedMaterialBytes = 0;
	for (const auto range : quarantinedMaterialRanges)
		if (materialRangeAllocator.Owns(range))
		{
			++quarantinedMaterialCount;
			quarantinedMaterialBytes += (uint64_t)range.capacity * sizeof(nri_scene::MaterialData);
		}
	PerfAuditSet(PerfAuditMetric::VoxelMaterialRangeQuarantinedCount, quarantinedMaterialCount);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialRangeQuarantinedBytes, quarantinedMaterialBytes);
	const auto sharing = materialPayloads.Snapshot();
	PerfAuditSet(PerfAuditMetric::VoxelMaterialSharingBindings, sharing.bindings);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialSharingPayloads, sharing.payloads);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialSharingPhysicalRows, sharing.physicalRows);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialSharingAliasRowsAvoided, sharing.aliasRows);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialSharingExcludedBindings, sharing.excludedBindings);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialSharingExcludedRows, sharing.excludedRows);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialSharingRetiringPayloads, sharing.retiring);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialSharingRetiringRows, sharing.retiringRows);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialSharingDescriptorBytes, sharing.descriptorBytes);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialSharingRetiringDescriptorBytes, sharing.retiringDescriptorBytes);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialUniquePayloadCount, payloadHashes.size());
	PerfAuditSet(PerfAuditMetric::VoxelMaterialPolicyResourceCount, policyMaterials);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialHighestActiveRow, highestActiveRow);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialArenaCursorRows, ranges.cursorRows);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialArenaLiveRows, ranges.liveRows);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialArenaHoleRows, ranges.holeRows);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialBridgeCapacityBytes, NRIAuditMaterialBridgeBytes(batch.materialBridge, true));
	PerfAuditSet(PerfAuditMetric::VoxelMaterialActorCapacityBytes, actorBytes);
	PerfAuditSet(PerfAuditMetric::VoxelMaterialMirrorCapacityBytes, materialUploadMirror.CapacityBytes());
	PerfAuditSet(PerfAuditMetric::VoxelArenaCommittedBytes, vertexBuffer.memorySize + indexBuffer.memorySize + primitiveBuffer.memorySize);
	PerfAuditSet(PerfAuditMetric::VoxelArenaUsedBytes, used);
	PerfAuditSet(PerfAuditMetric::VoxelArenaLiveBytes, live);
	PerfAuditSet(PerfAuditMetric::VoxelArenaDeadBytes, used - std::min(used, live));
	PerfAuditSet(PerfAuditMetric::VoxelReadyZeroRefCount, readyZeroRef);
	PerfAuditSet(PerfAuditMetric::VoxelReadyZeroRefBytes, readyZeroRefBytes);
	PerfAuditSet(PerfAuditMetric::VoxelPinnedCount, pinned);
	PerfAuditSet(PerfAuditMetric::VoxelPendingCount, admissionQueue.size());
	PerfAuditSet(PerfAuditMetric::VoxelPendingBytes, admissionBufferBytes + admissionAsBytes);
	PerfAuditSet(PerfAuditMetric::VoxelCpuScratchBytes, admissionCpuBytes);
	PerfAuditSet(PerfAuditMetric::VoxelPrivateGeometryBytes, privateGeometryBytes);
	PerfAuditSet(PerfAuditMetric::VoxelWorkQueueCount, admissionQueue.size());
	PerfAuditSet(PerfAuditMetric::VoxelRawArchiveBytes, compute.rawCpuBytes + compute.rawDeviceBytes + compute.rawUploadBytes);
	PerfAuditSet(PerfAuditMetric::VoxelRawArchiveCpuBytes, compute.rawCpuBytes);
	PerfAuditSet(PerfAuditMetric::VoxelRawArchiveDeviceBytes, compute.rawDeviceBytes);
	PerfAuditSet(PerfAuditMetric::VoxelRawArchiveUploadBytes, compute.rawUploadBytes);
	PerfAuditSet(PerfAuditMetric::VoxelRawArchiveSources, compute.rawSourceCount);
	PerfAuditSet(PerfAuditMetric::VoxelRawArchivePages, compute.rawArchivePageCount);
	PerfAuditSet(PerfAuditMetric::VoxelComputeQueuedJobs, compute.queuedJobCount);
	PerfAuditSet(PerfAuditMetric::VoxelComputePendingJobs, compute.pendingJobCount);
	PerfAuditSet(PerfAuditMetric::VoxelComputeInputDeviceBytes, compute.transientInputDeviceBytes);
	PerfAuditSet(PerfAuditMetric::VoxelComputeInputUploadBytes, compute.transientInputUploadBytes);
	PerfAuditSet(PerfAuditMetric::VoxelComputeGeneratedBytes, compute.transientGeneratedBytes);
	PerfAuditSet(PerfAuditMetric::VoxelComputeReadbackBytes, compute.statusReadbackBytes + compute.geometryReadbackBufferBytes);
	char fields[256];
	std::snprintf(fields, sizeof(fields), "\"map_generation\":%u,\"build_serial\":%llu,\"renderer_frame\":%u,\"last_world_tlas_frame\":%u,\"world_tlas_committed_this_frame\":%s,\"state\":\"sampled\"",
		residencyMapGeneration, (unsigned long long)residencyLastBuildSerial, rendererFrameIndex, committedWorldTlasFrameIndex,
		committedWorldTlasFrameIndex == rendererFrameIndex ? "true" : "false");
	PerfAuditWriteEvent("voxel_owner_census", PerfAuditCurrentToken(), fields);
}
