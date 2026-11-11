#include "nri_scene_material_frame_cache.h"
#include "nri_scene_material_texture_slots_reference.h"
#include "../scene/nri_material_bridge_exact.h"
#include "../scene/nri_cpu_perf_audit.h"
#include "nri_cvars.h"
#include "nri_cpu_detail_audit.h"
#include "printf.h"

#include <algorithm>

const nri_scene::MaterialBridgeData& NRISceneMaterialFrameCache::ResolveTextureSlots(
	const NRISceneTextureSlotTable& slotTable)
{
	PerfAuditScope auditResolve(PerfAuditMetric::MaterialProductReuseResolveNs);
	PerfAuditAdd(PerfAuditMetric::MaterialProductReuseRequests);
	PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveCalls);
	const bool detail = PerfAuditCpuDetailActive();
	const bool sameMapping = mTextureRemap.Prepare(slotTable, mCombinedMaterials);
	if (mResolvedProductQuarantined)
	{
		PerfAuditAdd(PerfAuditMetric::MaterialProductReuseReferenceFallbacks);
		BuildResolvedTextureReference(slotTable, mResolvedMaterials);
		ReportResolvedTextureStorage();
		return mResolvedMaterials;
	}
	if (mResolvedProductValid && sameMapping)
	{
		PerfAuditAdd(PerfAuditMetric::MaterialProductReuseHits);
		PerfAuditAdd(PerfAuditMetric::MaterialProductReuseRowsReused, mResolvedMaterials.materials.size());
		mResolvedTextureSlotRevision = slotTable.MappingRevision();
		ValidateResolvedTextureProduct(slotTable, true);
		ReportResolvedTextureStorage();
		return mResolvedMaterials;
	}
	PerfAuditAdd(PerfAuditMetric::MaterialProductReuseBuilds);
	// Lookup availability can change without MappingRevision changing. Keep
	// the old prefix optimization only when its actual dense mapping is intact.
	const bool ownerInvalid = !mResolvedResidentValid || mResidentLightMetadataCount != mResidentMaterialCount;
	const bool residentMappingChanged = mTextureRemap.FirstChangedIndex() < mResolvedResidentTextureLimit;
	if (residentMappingChanged ||
		mResidentLightMetadataCount != mResidentMaterialCount)
		mResolvedResidentValid = false;
	// Preserve invalid finite-index behavior too: a resident input can resolve
	// into a temporary overlay suffix. Keep a conservative high-water bound
	// while patching, then recompute it on every complete resident resolve.
	const auto resolveResidentRange = [&](uint32_t first, uint32_t end)
	{
		NRISceneMaterialTextureSlotResolveStats stats;
		NRIResolveSceneMaterialBridgeTextureSlotRangeWithRemap(mTextureRemap, mResolvedMaterials, first, end, &stats);
		mResolvedResidentTextureLimit = std::max(mResolvedResidentTextureLimit, stats.sourceIndexLimit);
	};
	const bool rebuildResident =
		!mResolvedResidentValid ||
		mResolvedTextureSlotRevision != slotTable.MappingRevision();
	if (rebuildResident)
	{
		// Exclusive revision categories retain the old rebuild decision. Owner
		// invalidation wins over mapping classification; prefix changes win over
		// a suffix-only or completely unchanged dense mapping.
		const bool revisionChanged = mResolvedTextureSlotRevision != slotTable.MappingRevision();
		const uint32_t category = ownerInvalid ? 0u : residentMappingChanged ? 1u : sameMapping ? 2u : 3u;
		static constexpr PerfAuditMetric calls[] = { PerfAuditMetric::MaterialResidentRevisionOwnerInvalidCalls,
			PerfAuditMetric::MaterialResidentRevisionPrefixChangedCalls, PerfAuditMetric::MaterialResidentRevisionMappingSameCalls,
			PerfAuditMetric::MaterialResidentRevisionSuffixOnlyCalls };
		static constexpr PerfAuditMetric rows[] = { PerfAuditMetric::MaterialResidentRevisionOwnerInvalidRows,
			PerfAuditMetric::MaterialResidentRevisionPrefixChangedRows, PerfAuditMetric::MaterialResidentRevisionMappingSameRows,
			PerfAuditMetric::MaterialResidentRevisionSuffixOnlyRows };
		static constexpr PerfAuditMetric bytes[] = { PerfAuditMetric::MaterialResidentRevisionOwnerInvalidBytes,
			PerfAuditMetric::MaterialResidentRevisionPrefixChangedBytes, PerfAuditMetric::MaterialResidentRevisionMappingSameBytes,
			PerfAuditMetric::MaterialResidentRevisionSuffixOnlyBytes };
		static constexpr PerfAuditMetric times[] = { PerfAuditMetric::MaterialResidentRevisionOwnerInvalidNs,
			PerfAuditMetric::MaterialResidentRevisionPrefixChangedNs, PerfAuditMetric::MaterialResidentRevisionMappingSameNs,
			PerfAuditMetric::MaterialResidentRevisionSuffixOnlyNs };
		NRICpuDetailAuditScope revisionScope(times[category], revisionChanged);
		if (detail && revisionChanged)
		{
			PerfAuditAdd(calls[category]);
			PerfAuditAdd(rows[category], mResidentMaterialCount);
			PerfAuditAdd(bytes[category], NRIAuditMaterialBridgeBytes(mCombinedMaterials));
		}
		if (mResolvedStaticPrefixDirty)
			PerfAuditAdd(PerfAuditMetric::MaterialFrameStaticPrefixFallbacks);
		PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRebuilds);
		if (!mResolvedResidentValid) PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveInvalidResident);
		if (mResolvedTextureSlotRevision != slotTable.MappingRevision())
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveSlotRevisionChanges);
		PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsCopied, mCombinedMaterials.materials.size());
		NRIAuditFrameBridgeCopy(mCombinedMaterials);
		{
			NRICpuDetailAuditScope copyScope(PerfAuditMetric::MaterialResidentFullCopyNs);
			mResolvedMaterials = mCombinedMaterials;
		}
		mResolvedMaterials.materials.resize(mResidentMaterialCount);
		mResolvedMaterials.lightMetadata.resize(mResidentLightMetadataCount);
		PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsRemapped, mResolvedMaterials.materials.size());
		mResolvedResidentTextureLimit = 0;
		{
			NRICpuDetailAuditScope remapScope(PerfAuditMetric::MaterialResidentFullRemapNs);
			resolveResidentRange(0u, mResidentMaterialCount);
		}
		mResolvedResidentValid = true;
		mResolvedTextureSlotRevision = slotTable.MappingRevision();
		mResolvedStaticPrefixDirty = false;
		mResolvedDirtyRows.clear();
	}
	else
	{
		PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveHits);
		mResolvedMaterials.materials.resize(mResidentMaterialCount);
		mResolvedMaterials.lightMetadata.resize(mResidentLightMetadataCount);
		mResolvedMaterials.textures = mCombinedMaterials.textures;
		mResolvedMaterials.paletteLookup = mCombinedMaterials.paletteLookup;
		mResolvedMaterials.paletteWidth = mCombinedMaterials.paletteWidth;
		mResolvedMaterials.paletteHeight = mCombinedMaterials.paletteHeight;
		if (mResolvedStaticPrefixDirty)
		{
			std::copy_n(mCombinedMaterials.materials.begin(), mStaticMaterialCount, mResolvedMaterials.materials.begin());
			std::copy_n(mCombinedMaterials.lightMetadata.begin(), mStaticLightMetadataCount, mResolvedMaterials.lightMetadata.begin());
			resolveResidentRange(0u, mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveStaticPatches);
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveStaticRowsCopied, mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolvePersistentRowsPreserved, mResidentMaterialCount - mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsCopied, mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsRemapped, mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeRowsCopied, mStaticMaterialCount);
			PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeBytesCopied,
				(uint64_t)mStaticMaterialCount * sizeof(nri_scene::MaterialData) +
				(uint64_t)mStaticLightMetadataCount * sizeof(nri_scene::MaterialLightingMetadata));
			mResolvedStaticPrefixDirty = false;
		}
		if (!mResolvedDirtyRows.empty())
		{
			nri_scene::MergeMaterialRowRanges(mResolvedDirtyRows, mResidentMaterialCount);
			PerfAuditAdd(PerfAuditMetric::MaterialFrameResolvePersistentPatches);
			for (const auto range : mResolvedDirtyRows)
			{
				std::copy_n(mCombinedMaterials.materials.begin() + range.first, range.count, mResolvedMaterials.materials.begin() + range.first);
				std::copy_n(mCombinedMaterials.lightMetadata.begin() + range.first, range.count, mResolvedMaterials.lightMetadata.begin() + range.first);
				resolveResidentRange(range.first, range.first + range.count);
				PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsCopied, range.count);
				PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsRemapped, range.count);
				PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeRowsCopied, range.count);
				PerfAuditAdd(PerfAuditMetric::VoxelSceneFrameBridgeBytesCopied,
					(uint64_t)range.count * (sizeof(nri_scene::MaterialData) + sizeof(nri_scene::MaterialLightingMetadata)));
			}
			mResolvedDirtyRows.clear();
		}
	}

	const uint32_t firstOverlayRow = (uint32_t)mResolvedMaterials.materials.size();
	PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsCopied, mCombinedMaterials.materials.size() - mResidentMaterialCount);
	mResolvedMaterials.materials.insert(
		mResolvedMaterials.materials.end(),
		mCombinedMaterials.materials.begin() + mResidentMaterialCount,
		mCombinedMaterials.materials.end());
	mResolvedMaterials.lightMetadata.insert(
		mResolvedMaterials.lightMetadata.end(),
		mCombinedMaterials.lightMetadata.begin() + mResidentLightMetadataCount,
		mCombinedMaterials.lightMetadata.end());
	PerfAuditAdd(PerfAuditMetric::MaterialFrameResolveRowsRemapped, mResolvedMaterials.materials.size() - firstOverlayRow);
	NRIResolveSceneMaterialBridgeTextureSlotRangeWithRemap(mTextureRemap, mResolvedMaterials,
		firstOverlayRow, (uint32_t)mResolvedMaterials.materials.size());
	mResolvedProductValid = true;
	ValidateResolvedTextureProduct(slotTable, false);
	ReportResolvedTextureStorage();
	return mResolvedMaterials;
}

void NRISceneMaterialFrameCache::BuildResolvedTextureReference(
 const NRISceneTextureSlotTable& slotTable, nri_scene::MaterialBridgeData& reference) const
{
 reference = mCombinedMaterials;
 reference.materials.resize(mResidentMaterialCount);
 reference.lightMetadata.resize(mResidentLightMetadataCount);
 NRIResolveSceneMaterialBridgeTextureSlotsReference(slotTable, reference, 0u);
 reference.materials.insert(reference.materials.end(),
  mCombinedMaterials.materials.begin() + mResidentMaterialCount, mCombinedMaterials.materials.end());
 reference.lightMetadata.insert(reference.lightMetadata.end(),
  mCombinedMaterials.lightMetadata.begin() + mResidentLightMetadataCount, mCombinedMaterials.lightMetadata.end());
 NRIResolveSceneMaterialBridgeTextureSlotsReference(slotTable, reference, mResidentMaterialCount);
}

void NRISceneMaterialFrameCache::ValidateResolvedTextureProduct(
 const NRISceneTextureSlotTable& slotTable, bool productHit)
{
 const bool validateProduct = (bool)nri_pttextureresolvevalidate;
 if (!validateProduct && !mValidateRangeUpdates) return;
 PerfAuditScope auditReference(PerfAuditMetric::MaterialProductReuseValidationNs);
 nri_scene::MaterialBridgeData reference;
 BuildResolvedTextureReference(slotTable, reference);
 if (mValidateRangeUpdates) PerfAuditAdd(PerfAuditMetric::MaterialRangeResolveValidations);
 if (validateProduct)
 {
  PerfAuditAdd(PerfAuditMetric::MaterialProductReuseValidationChecks);
  if (productHit) PerfAuditAdd(PerfAuditMetric::MaterialProductReuseHitValidationChecks);
 }
 if (nri_scene::MaterialBridgesExactlyEqual(mResolvedMaterials, reference)) return;
 if (mValidateRangeUpdates) PerfAuditAdd(PerfAuditMetric::MaterialRangeResolveMismatches);
 PerfAuditAdd(PerfAuditMetric::MaterialProductReuseValidationMismatches);
 PerfAuditAdd(PerfAuditMetric::MaterialProductReuseQuarantines);
 Printf("NRI PT texture product validation: hit=%u mismatch=1 action=reference-until-reset\n", productHit ? 1u : 0u);
 mResolvedMaterials = std::move(reference);
 mResolvedProductValid = false;
 mResolvedProductQuarantined = true;
 mResolvedResidentValid = false;
 mResolvedStaticPrefixDirty = false;
 mResolvedDirtyRows.clear();
 mUniformDefaultRanges.clear();
 mRangeUpdatesQuarantined = true;
}

void NRISceneMaterialFrameCache::ReportResolvedTextureStorage() const
{
 if (!PerfAuditActive()) return;
 PerfAuditSet(PerfAuditMetric::MaterialProductReuseRemapCapacityBytes, mTextureRemap.CapacityBytes());
 PerfAuditSet(PerfAuditMetric::MaterialProductReuseAppendRemapCapacityBytes,
  mAppendTextureRemapScratch.capacity() * sizeof(uint64_t));
 PerfAuditSet(PerfAuditMetric::MaterialProductReuseResolvedCapacityBytes,
  mResolvedMaterials.materials.capacity() * sizeof(nri_scene::MaterialData) +
  mResolvedMaterials.lightMetadata.capacity() * sizeof(nri_scene::MaterialLightingMetadata) +
  mResolvedMaterials.textures.capacity() * sizeof(nri_scene::TextureUpload));
}
