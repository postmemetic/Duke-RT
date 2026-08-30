#pragma once

#include "nri_runtime_light_shadow_selection.h"

#include <algorithm>
#include <cstdint>

struct NRISceneDataLightBufferReuseView
{
	uintptr_t resourceIdentity = 0;
	uintptr_t descriptorIdentity = 0;
	bool resourceReady = false;
	bool descriptorReady = false;
	uint64_t usedSize = 0;
	uint64_t capacity = 0;
	uint32_t stride = 0;

	bool CanReuse(uint64_t requiredBytes, uint32_t requiredStride) const
	{
		return
			resourceReady &&
			descriptorReady &&
			stride == requiredStride &&
			usedSize >= requiredBytes &&
			capacity >= std::max<uint64_t>(requiredBytes, requiredStride);
	}

	bool Matches(uintptr_t expectedResourceIdentity, uintptr_t expectedDescriptorIdentity) const
	{
		return resourceIdentity != 0 &&
			descriptorIdentity != 0 &&
			resourceIdentity == expectedResourceIdentity &&
			descriptorIdentity == expectedDescriptorIdentity;
	}
};

struct NRISceneDataRuntimeLightSlotIdentity
{
	uint64_t payloadHash = 0;
	uintptr_t resourceIdentity = 0;
	uintptr_t descriptorIdentity = 0;
	uint32_t lightCount = 0;
	bool valid = false;

	bool CanReuse(
		uint64_t candidateHash,
		uint32_t candidateLightCount,
		const NRISceneDataLightBufferReuseView& buffer,
		uint32_t elementStride) const
	{
		return
			valid &&
			payloadHash == candidateHash &&
			lightCount == candidateLightCount &&
			buffer.Matches(resourceIdentity, descriptorIdentity) &&
			buffer.CanReuse((uint64_t)lightCount * elementStride, elementStride);
	}

	void Invalidate()
	{
		valid = false;
	}

	void Commit(
		uint64_t newPayloadHash,
		uint32_t newLightCount,
		const NRISceneDataLightBufferReuseView& buffer)
	{
		payloadHash = newPayloadHash;
		lightCount = newLightCount;
		resourceIdentity = buffer.resourceIdentity;
		descriptorIdentity = buffer.descriptorIdentity;
		valid = buffer.resourceReady && buffer.descriptorReady &&
			resourceIdentity != 0 && descriptorIdentity != 0;
	}
};

struct NRISceneDataRuntimeLightClusterSlotIdentity
{
	uint64_t payloadHash = 0;
	uintptr_t headerResourceIdentity = 0;
	uintptr_t headerDescriptorIdentity = 0;
	uintptr_t indexResourceIdentity = 0;
	uintptr_t indexDescriptorIdentity = 0;
	uint32_t tileCountX = 0;
	uint32_t tileCountY = 0;
	uint32_t tileIndexCount = 0;
	uint32_t maxTileOccupancy = 0;
	uint32_t shadowBudget = 0;
	uint32_t shadowCandidateReferenceCount = 0;
	uint32_t shadowSelectedReferenceCount = 0;
	uint32_t shadowOverflowReferenceCount = 0;
	uint32_t maxShadowCandidatesPerTile = 0;
	uint32_t maxShadowSelectedPerTile = 0;
	uint64_t shadowSelectionHash = 0;
	NRIRuntimeLightShadowTransitionTelemetry shadowTransitions;
	NRIRuntimeLightShadowSelectionSnapshot shadowSelection;
	bool valid = false;

	bool CanReuse(
		uint64_t candidateHash,
		const NRISceneDataLightBufferReuseView& headerBuffer,
		uint32_t headerStride,
		const NRISceneDataLightBufferReuseView& indexBuffer,
		uint32_t indexStride) const
	{
		return
			valid &&
			payloadHash == candidateHash &&
			headerBuffer.Matches(headerResourceIdentity, headerDescriptorIdentity) &&
			indexBuffer.Matches(indexResourceIdentity, indexDescriptorIdentity) &&
			headerBuffer.CanReuse((uint64_t)tileCountX * tileCountY * headerStride, headerStride) &&
			indexBuffer.CanReuse((uint64_t)tileIndexCount * indexStride, indexStride);
	}

	void Invalidate()
	{
		valid = false;
	}

	void Commit(
		uint64_t newPayloadHash,
		uint32_t newTileCountX,
		uint32_t newTileCountY,
		uint32_t newTileIndexCount,
		uint32_t newMaxTileOccupancy,
		uint32_t newShadowBudget,
		uint32_t newShadowCandidateReferenceCount,
		uint32_t newShadowSelectedReferenceCount,
		uint32_t newShadowOverflowReferenceCount,
		uint32_t newMaxShadowCandidatesPerTile,
		uint32_t newMaxShadowSelectedPerTile,
		uint64_t newShadowSelectionHash,
		const NRIRuntimeLightShadowTransitionTelemetry& newShadowTransitions,
		const NRIRuntimeLightShadowSelectionSnapshot& newShadowSelection,
		const NRISceneDataLightBufferReuseView& headerBuffer,
		const NRISceneDataLightBufferReuseView& indexBuffer)
	{
		payloadHash = newPayloadHash;
		tileCountX = newTileCountX;
		tileCountY = newTileCountY;
		tileIndexCount = newTileIndexCount;
		maxTileOccupancy = newMaxTileOccupancy;
		shadowBudget = newShadowBudget;
		shadowCandidateReferenceCount = newShadowCandidateReferenceCount;
		shadowSelectedReferenceCount = newShadowSelectedReferenceCount;
		shadowOverflowReferenceCount = newShadowOverflowReferenceCount;
		maxShadowCandidatesPerTile = newMaxShadowCandidatesPerTile;
		maxShadowSelectedPerTile = newMaxShadowSelectedPerTile;
		shadowSelectionHash = newShadowSelectionHash;
		shadowTransitions = newShadowTransitions;
		shadowSelection = newShadowSelection;
		headerResourceIdentity = headerBuffer.resourceIdentity;
		headerDescriptorIdentity = headerBuffer.descriptorIdentity;
		indexResourceIdentity = indexBuffer.resourceIdentity;
		indexDescriptorIdentity = indexBuffer.descriptorIdentity;
		valid = headerBuffer.resourceReady && headerBuffer.descriptorReady &&
			indexBuffer.resourceReady && indexBuffer.descriptorReady &&
			headerResourceIdentity != 0 && headerDescriptorIdentity != 0 &&
			indexResourceIdentity != 0 && indexDescriptorIdentity != 0;
	}
};

struct NRISceneDataSectorLightSlotIdentity
{
	uint64_t payloadHash = 0;
	uintptr_t headerResourceIdentity = 0;
	uintptr_t headerDescriptorIdentity = 0;
	uintptr_t dataResourceIdentity = 0;
	uintptr_t dataDescriptorIdentity = 0;
	uint32_t sectorCount = 0;
	bool valid = false;

	bool CanReuse(
		uint64_t candidateHash,
		uint32_t candidateSectorCount,
		const NRISceneDataLightBufferReuseView& headerBuffer,
		uint32_t headerStride,
		const NRISceneDataLightBufferReuseView& dataBuffer,
		uint32_t dataStride) const
	{
		return
			valid &&
			payloadHash == candidateHash &&
			sectorCount == candidateSectorCount &&
			headerBuffer.Matches(headerResourceIdentity, headerDescriptorIdentity) &&
			dataBuffer.Matches(dataResourceIdentity, dataDescriptorIdentity) &&
			headerBuffer.CanReuse(headerStride, headerStride) &&
			dataBuffer.CanReuse((uint64_t)sectorCount * dataStride, dataStride);
	}

	void Invalidate()
	{
		valid = false;
	}

	void Commit(
		uint64_t newPayloadHash,
		uint32_t newSectorCount,
		const NRISceneDataLightBufferReuseView& headerBuffer,
		const NRISceneDataLightBufferReuseView& dataBuffer)
	{
		payloadHash = newPayloadHash;
		sectorCount = newSectorCount;
		headerResourceIdentity = headerBuffer.resourceIdentity;
		headerDescriptorIdentity = headerBuffer.descriptorIdentity;
		dataResourceIdentity = dataBuffer.resourceIdentity;
		dataDescriptorIdentity = dataBuffer.descriptorIdentity;
		valid = headerBuffer.resourceReady && headerBuffer.descriptorReady &&
			dataBuffer.resourceReady && dataBuffer.descriptorReady &&
			headerResourceIdentity != 0 && headerDescriptorIdentity != 0 &&
			dataResourceIdentity != 0 && dataDescriptorIdentity != 0;
	}
};

struct NRISceneDataLightSlotReuseState
{
	NRISceneDataRuntimeLightSlotIdentity runtimeLight;
	NRISceneDataRuntimeLightClusterSlotIdentity runtimeLightCluster;
	NRISceneDataSectorLightSlotIdentity sectorLight;

	void Reset()
	{
		*this = {};
	}
};
