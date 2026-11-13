#include "nri_persistent_voxel_material_bridge.h"
#include "nri_cpu_detail_audit.h"
#include "../scene/nri_cpu_perf_audit.h"

#include <algorithm>

void NRIPersistentVoxelMaterialBridge::Update(
	const std::vector<NRIVoxelMaterialBridgeResourceView>& resources,
	uint32_t reserveRows, uint64_t beforeGeneration, uint64_t generation,
	nri_scene::MaterialBridgeData& bridge, bool validate)
{
	using namespace nri_scene;
	const bool detail = PerfAuditCpuDetailActive();
	if (detail) PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeObservedCalls);
	MaterialBridgeChanges changes;
	changes.beforeGeneration = beforeGeneration;
	changes.generation = generation;
	changes.beforeRows = (uint32_t)bridge.materials.size();
	const uint64_t namespaceStart = detail ? PerfAuditClockNs() : 0;
	uint64_t namespaceRows = 0, texturedRows = 0, sourceTextures = 0;
	MaterialBridgeData layout;
	std::unordered_map<uint64_t, uint32_t> textureLookup;
	bool aligned = true;
	uint32_t rowEnd = 0;
	mUniformDefaultRanges.clear();
	for (const auto& resource : resources)
	{
		if (resource.bridge == nullptr || resource.offset < rowEnd ||
			resource.count > UINT32_MAX - resource.offset ||
			resource.bridge->materials.size() != resource.count ||
			resource.bridge->lightMetadata.size() != resource.count)
			aligned = false;
		if (resource.offset > rowEnd)
			mUniformDefaultRanges.push_back({ rowEnd, resource.offset - rowEnd });
		rowEnd = resource.offset + resource.count;
		if (resource.bridge != nullptr)
		{
			if (detail)
			{
				namespaceRows += resource.bridge->materials.size();
				texturedRows += resource.bridge->textures.empty() ? 0 : resource.bridge->materials.size();
				sourceTextures += resource.bridge->textures.size();
			}
			CollectMaterialBridgeTextureLayout(*resource.bridge, layout, textureLookup);
		}
	}
	bool sameNamespace = bridge.textures.size() == layout.textures.size() &&
		bridge.paletteWidth == layout.paletteWidth && bridge.paletteHeight == layout.paletteHeight &&
		bridge.paletteLookup == layout.paletteLookup;
	for (size_t index = 0; sameNamespace && index < layout.textures.size(); ++index)
		sameNamespace = bridge.textures[index].key == layout.textures[index].key;
	if (detail)
	{
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeNamespaceNs, PerfAuditClockNs() - namespaceStart);
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeNamespaceResources, resources.size());
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeNamespaceMaterialRows, namespaceRows);
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeNamespaceTexturedRows, texturedRows);
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeNamespaceSourceTextures, sourceTextures);
	}
	const bool incremental = mValid && !mQuarantined && aligned && sameNamespace &&
		mChanges.generation == beforeGeneration && mChanges.afterRows == bridge.materials.size() &&
		bridge.lightMetadata.size() == bridge.materials.size();

	if (!incremental)
	{
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeFullRebuilds);
		ClearMaterialBridgeRetainingCapacity(bridge);
		bridge.materials.reserve(reserveRows);
		bridge.lightMetadata.reserve(reserveRows);
		textureLookup.clear();
		for (const auto& resource : resources)
		{
			if (resource.bridge == nullptr) continue;
			if (bridge.materials.size() < resource.offset)
			{
				PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeRowsInitialized, resource.offset - bridge.materials.size());
				bridge.materials.resize(resource.offset);
				bridge.lightMetadata.resize(resource.offset);
			}
			const uint64_t bytesBefore = PerfAuditActive() ? NRIAuditMaterialBridgeBytes(bridge) : 0;
			AppendMaterialBridge(*resource.bridge, bridge, textureLookup);
			PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeRowsCopied, resource.bridge->materials.size());
			if (PerfAuditActive()) PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeBytesCopied, NRIAuditMaterialBridgeBytes(bridge) - bytesBefore);
		}
		changes.rows.push_back({0u, (uint32_t)bridge.materials.size()});
	}
	else
	{
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeIncrementalUpdates);
		changes.fullRebuild = false;
		const uint32_t oldRows = (uint32_t)bridge.materials.size();
		bridge.materials.resize(rowEnd);
		bridge.lightMetadata.resize(rowEnd);
		bridge.textures = std::move(layout.textures);
		bridge.paletteLookup = layout.paletteLookup;
		bridge.paletteWidth = layout.paletteWidth;
		bridge.paletteHeight = layout.paletteHeight;
		bridge.buildStats = {};
		if (rowEnd > oldRows)
		{
			changes.rows.push_back({oldRows, rowEnd - oldRows});
			PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeRowsInitialized, rowEnd - oldRows);
		}
		const uint64_t matchingStart = detail ? PerfAuditClockNs() : 0;
		uint64_t matchingPrepared = 0;
		{
			std::unordered_map<uint64_t, const NRIVoxelMaterialBridgeResourceView*> current;
			std::unordered_map<uint64_t, const ResourceState*> previous;
			for (const auto& resource : resources) current.emplace(resource.key, &resource);
			const uint64_t prepared = detail ? PerfAuditClockNs() - matchingStart : 0;
			if (detail) PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeMatchPrepareNs, prepared);
			for (const auto& state : mResources)
			{
				previous.emplace(state.key, &state);
				const auto found = current.find(state.key);
				if (found != current.end() && found->second->offset == state.offset && found->second->count == state.count)
					continue;
				if (state.offset >= rowEnd) continue;
				const uint32_t clearCount = std::min(state.count, rowEnd - state.offset);
				std::fill_n(bridge.materials.begin() + state.offset, clearCount, MaterialData{});
				std::fill_n(bridge.lightMetadata.begin() + state.offset, clearCount, MaterialLightingMetadata{});
				changes.rows.push_back({state.offset, clearCount});
				PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeRowsInitialized, clearCount);
			}
			for (const auto& resource : resources)
			{
				const auto found = previous.find(resource.key);
				const bool unchanged = found != previous.end() && !resource.dirty &&
					found->second->offset == resource.offset && found->second->count == resource.count &&
					found->second->slotGeneration == resource.slotGeneration &&
					found->second->contentRevision == resource.contentRevision;
				if (unchanged) continue;
				CopyMaterialBridgeRowRange(*resource.bridge, {0u, resource.count}, bridge, resource.offset, textureLookup);
				changes.rows.push_back({resource.offset, resource.count});
				PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeRowsCopied, resource.count);
				PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeBytesCopied,
					(uint64_t)resource.count * (sizeof(MaterialData) + sizeof(MaterialLightingMetadata)));
			}
			if (detail) PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeMatchLegacyNodes, current.size() + previous.size());
			matchingPrepared = prepared;
		}
		if (detail)
		{
			PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeMatchOutputNs, PerfAuditClockNs() - matchingStart - matchingPrepared);
			PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeMatchCurrentRows, resources.size());
			PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeMatchPreviousRows, mResources.size());
			PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeMatchLegacyCalls);
		}
	}
	if (validate && incremental)
	{
		NRICpuDetailAuditScope referenceAudit(PerfAuditMetric::VoxelMaterialBridgeReferenceNs);
		NRIPersistentVoxelMaterialBridge referenceOwner;
		MaterialBridgeData reference;
		referenceOwner.Update(resources, reserveRows, beforeGeneration, generation, reference);
		PerfAuditAdd(PerfAuditMetric::MaterialRangeBridgeValidations);
		if (!MaterialBridgesEquivalent(bridge, reference))
		{
			PerfAuditAdd(PerfAuditMetric::MaterialRangeBridgeMismatches);
			bridge = std::move(reference);
			changes.fullRebuild = true;
			changes.rows = {{0u, (uint32_t)bridge.materials.size()}};
			mQuarantined = true;
		}
	}
	NRICpuDetailAuditScope finalizeAudit(PerfAuditMetric::VoxelMaterialBridgeFinalizeNs);
	changes.afterRows = (uint32_t)bridge.materials.size();
	MergeMaterialRowRanges(changes.rows, changes.afterRows);
	PerfAuditAdd(PerfAuditMetric::VoxelMaterialBridgeDirtyRanges, changes.rows.size());
	mChanges = std::move(changes);
	mResources.clear();
	for (const auto& resource : resources)
		mResources.push_back({resource.key, resource.slotGeneration, resource.contentRevision, resource.offset, resource.count});
	mValid = aligned;
	if (!aligned) mUniformDefaultRanges.clear();
}
