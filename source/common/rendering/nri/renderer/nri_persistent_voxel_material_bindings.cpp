#include "nri_persistent_voxels.h"
#include "nri_persistent_voxel_material_bindings.h"
#include "nri_cvars.h"
#include "perf_audit.h"
#include "printf.h"

#include <cstdio>

NRIVoxelMaterialRetirementScope::NRIVoxelMaterialRetirementScope(
	NRIPersistentVoxelResidency& owner, const NRIPersistentVoxelResetServices& services)
	: owner(owner), services(services)
{
	owner.RetirePendingMaterialRanges(services);
}

NRIVoxelMaterialRetirementScope::~NRIVoxelMaterialRetirementScope()
{
	owner.RetirePendingMaterialRanges(services);
}

bool NRIPersistentVoxelResidency::AcquireMaterialBinding(
	PersistentVoxelMaterialVariantResource& resource, bool contentChanged, bool& moved)
{
	auto range = materialPayloads.FindBinding(resource.materialKeyHash);
	if (contentChanged || !range)
	{
		PerfAuditScope audit(PerfAuditMetric::VoxelMaterialSharingAcquireNs);
		PerfAuditAdd(materialSharingPolicy.Mode().Enabled() ?
			PerfAuditMetric::VoxelMaterialSharingEnabledAcquires : PerfAuditMetric::VoxelMaterialSharingPrivateAcquires);
		const auto before = materialPayloads.Counters();
		NRIPersistentVoxelMaterialRangeHandle retired;
		const bool acquired = materialPayloads.AcquireBinding(resource.materialKeyHash,
			materialSharingPolicy.Certify(resource.materialBridge), resource.materialBridge,
			materialRangeAllocator, range, retired);
		const auto& after = materialPayloads.Counters();
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialSharingLookups, after.lookups - before.lookups);
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialSharingHits, after.hits - before.hits);
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialSharingComparisons, after.comparisons - before.comparisons);
		PerfAuditAdd(PerfAuditMetric::VoxelMaterialSharingBytesHashed, after.bytesHashed - before.bytesHashed);
		if (!acquired) return false;
		if (retired) pendingMaterialRangeRetirements.push_back(retired);
	}
	moved = resource.materialOffset != range.offset || resource.materialSlotGeneration != range.generation;
	resource.materialOffset = range.offset;
	resource.materialCapacity = range.capacity;
	resource.materialSlotGeneration = range.generation;
	if (moved) resource.materialUploadHash = 0;
	return true;
}

bool NRIPersistentVoxelResidency::PrepareMaterialSharing(uint64_t buildSerial, uint32_t frameIndex)
{
	const bool wasLatched = materialSharingPolicy.Mode().Latched();
	if (materialSharingPolicy.ObserveRequestedMode((bool)nri_ptvoxelmaterialsharing))
	{
		const auto& mode = materialSharingPolicy.Mode();
		const bool ignored = mode.Requested() != mode.Enabled();
		const char* action = !wasLatched ? "latch" : ignored ? "ignore-request" : "request-restored";
		if (ignored) PerfAuditAdd(PerfAuditMetric::VoxelMaterialSharingModeChangeIgnored);
		Printf("NRI PT voxel material sharing mode: effective=%s requested=%s action=%s latch_scope=shared-resource-lifetime\n",
			mode.Enabled() ? "sharing" : "private", mode.Requested() ? "sharing" : "private", action);
		char fields[256];
		std::snprintf(fields, sizeof(fields), "\"enabled\":%s,\"requested\":%s,\"latched\":true,\"action\":\"%s\"",
			mode.Enabled() ? "true" : "false", mode.Requested() ? "true" : "false", action);
		PerfAuditWriteEvent("voxel_material_sharing_mode", PerfAuditCurrentToken(), fields);
	}
	materialSharingRecertificationPending |= materialSharingPolicy.BeginFrame(buildSerial, frameIndex);
	const auto& actors = materialSharingPolicy.ChangedActors();
	if (!materialSharingRecertificationPending && actors.empty()) return true;
	PerfAuditScope audit(PerfAuditMetric::VoxelMaterialSharingRecertifyNs);
	bool bindingsMoved = false;
	// Includes inactive resources and completed/pending admissions that may be
	// reused later. Every alias is recertified before either batch fast path.
	auto recertify = [&](PersistentVoxelMaterialVariantResource& resource)
	{
		if (!resource.materialCount || resource.materialBridge.materials.empty()) return true;
		bool moved = false;
		if (!AcquireMaterialBinding(resource, true, moved)) return false;
		if (moved)
		{
			bindingsMoved = true;
			dirtyMaterialResourceKeys.insert(resource.materialKeyHash);
			materialResourceGeneration++;
		}
		return true;
	};
	if (materialSharingRecertificationPending)
	{
		for (auto& entry : materialVariantResources)
			if (!recertify(entry.second)) return false;
	}
	else for (int32_t actor : actors)
	{
		for (uint64_t key : materialPayloads.BindingsForActor(actor))
		{
			const auto resource = materialVariantResources.find(key);
			if (resource != materialVariantResources.end() && !recertify(resource->second))
			{
				materialSharingRecertificationPending = true;
				return false;
			}
		}
	}
	if (bindingsMoved || materialSharingRecertificationPending)
	{
		publicationBatchReady = false;
		MarkMaintenanceMutation();
	}
	materialSharingRecertificationPending = false;
	return true;
}

void NRIPersistentVoxelResidency::RetirePendingMaterialRanges(const NRIPersistentVoxelResetServices& services)
{
	if (!services.retireMaterialRange) return;
	for (const auto range : pendingMaterialRangeRetirements) services.RetireMaterialRange(range);
	pendingMaterialRangeRetirements.clear();
}
