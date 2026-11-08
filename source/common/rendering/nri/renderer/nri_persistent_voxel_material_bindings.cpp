#include "nri_persistent_voxels.h"
#include "nri_persistent_voxel_material_bindings.h"
#include "perf_audit.h"

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
