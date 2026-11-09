#include "nri_scene_material_texture_slots.h"
#include "nri_scene_material_texture_slots_reference.h"
#include "../scene/nri_material_bridge_exact.h"
#include "nri_cvars.h"
#include "perf_audit.h"
#include "printf.h"

#include <algorithm>
#include <cstring>

bool NRISceneMaterialTextureSlotRemap::Prepare(
 const NRISceneTextureSlotTable& slotTable, const nri_scene::MaterialBridgeData& materials)
{
 PerfAuditScope auditResolve(PerfAuditMetric::TextureResolveNs);
 PerfAuditScope auditPrepare(PerfAuditMetric::TextureResolvePrepareNs);
 PerfAuditAdd(PerfAuditMetric::TextureResolvePreparations);
 PerfAuditAdd(PerfAuditMetric::TextureResolveSourceTextures, materials.textures.size());
 PerfAuditAdd(PerfAuditMetric::TextureResolveKeyLookups, materials.textures.size());
 const size_t previousSize = mSlots.size();
 mFirstChangedIndex = mPrepared ? std::min(previousSize, materials.textures.size()) : 0;
 mSlots.resize(materials.textures.size(), UINT32_MAX);
 bool unchanged = mPrepared && previousSize == mSlots.size();
 for (size_t index = 0; index < materials.textures.size(); ++index)
 {
  const auto handle = slotTable.Lookup(materials.textures[index].key);
  const uint32_t slot = handle ? handle.slot : UINT32_MAX;
  if (index >= previousSize || mSlots[index] != slot)
  {
   unchanged = false;
   mFirstChangedIndex = std::min(mFirstChangedIndex, index);
   mSlots[index] = slot;
  }
 }
 mPrepared = true;
 return unchanged;
}

uint32_t NRISceneMaterialTextureSlotRemap::Required(
 uint32_t index, NRISceneMaterialTextureSlotResolveStats& stats) const
{
 if (index != UINT32_MAX) stats.sourceIndexLimit = std::max(stats.sourceIndexLimit, (uint64_t)index + 1);
 if (index >= mSlots.size() || mSlots[index] == UINT32_MAX)
 { ++stats.requiredFallbacks; return 0; }
 return mSlots[index];
}
uint32_t NRISceneMaterialTextureSlotRemap::Optional(
 uint32_t index, NRISceneMaterialTextureSlotResolveStats& stats) const
{
 if (index == UINT32_MAX) return UINT32_MAX;
 stats.sourceIndexLimit = std::max(stats.sourceIndexLimit, (uint64_t)index + 1);
 if (index >= mSlots.size() || mSlots[index] == UINT32_MAX)
 { ++stats.optionalFallbacks; return UINT32_MAX; }
 return mSlots[index];
}

namespace
{
void ResolveRow(const NRISceneMaterialTextureSlotRemap& remap,
 nri_scene::MaterialData& material, NRISceneMaterialTextureSlotResolveStats& stats)
{
 material.textureIndex = remap.Required(material.textureIndex, stats);
 material.normalTextureIndex = remap.Optional(material.normalTextureIndex, stats);
 material.metallicTextureIndex = remap.Optional(material.metallicTextureIndex, stats);
 material.roughnessTextureIndex = remap.Optional(material.roughnessTextureIndex, stats);
 material.emissiveTextureIndex = remap.Optional(material.emissiveTextureIndex, stats);
 ++stats.materialRows;
}
bool SameFallbackStats(const NRISceneMaterialTextureSlotResolveStats& a,
 const NRISceneMaterialTextureSlotResolveStats& b)
{
 // keyLookups records actual implementation work; it deliberately differs.
 return a.materialRows == b.materialRows && a.requiredFallbacks == b.requiredFallbacks &&
  a.optionalFallbacks == b.optionalFallbacks;
}
void Report(const NRISceneMaterialTextureSlotResolveStats& stats)
{
 PerfAuditAdd(PerfAuditMetric::TextureResolveRows, stats.materialRows);
}
void Mismatch(const char* operation)
{
 PerfAuditAdd(PerfAuditMetric::TextureResolveValidationMismatches);
 PerfAuditAdd(PerfAuditMetric::TextureResolveQuarantines);
 Printf("NRI PT texture resolve validation: operation=%s mismatch=1 action=reference-for-operation\n", operation);
}
}

void NRIResolveSceneMaterialTextureSlots(
 const NRISceneTextureSlotTable& slotTable, const nri_scene::MaterialBridgeData& materials,
 std::vector<nri_scene::MaterialData>& gpuMaterials, NRISceneMaterialTextureSlotResolveStats* outStats)
{
 std::vector<nri_scene::MaterialData> reference;
 NRISceneMaterialTextureSlotResolveStats referenceStats;
 if ((bool)nri_pttextureresolvevalidate)
 {
  PerfAuditScope auditReference(PerfAuditMetric::TextureResolveValidationNs);
  reference = gpuMaterials;
  NRIResolveSceneMaterialTextureSlotsReference(slotTable, materials, reference, &referenceStats);
 }
 NRISceneMaterialTextureSlotResolveStats stats;
 NRISceneMaterialTextureSlotRemap remap;
 if (!gpuMaterials.empty())
 { remap.Prepare(slotTable, materials); stats.keyLookups = materials.textures.size(); }
 {
  PerfAuditScope auditResolve(PerfAuditMetric::TextureResolveNs);
  PerfAuditAdd(PerfAuditMetric::TextureResolveCalls);
  for (auto& row : gpuMaterials) ResolveRow(remap, row, stats);
  Report(stats);
 }
 if ((bool)nri_pttextureresolvevalidate)
 {
  PerfAuditScope auditReference(PerfAuditMetric::TextureResolveValidationNs);
  PerfAuditAdd(PerfAuditMetric::TextureResolveValidationChecks);
  if (!SameFallbackStats(stats, referenceStats) || (!gpuMaterials.empty() &&
   std::memcmp(gpuMaterials.data(), reference.data(), gpuMaterials.size() * sizeof(nri_scene::MaterialData))))
  { Mismatch("rows"); gpuMaterials = std::move(reference); stats = referenceStats; }
 }
 if (outStats) *outStats = stats;
}

void NRIResolveSceneMaterialBridgeTextureSlotRangeWithRemap(
 const NRISceneMaterialTextureSlotRemap& remap, nri_scene::MaterialBridgeData& materials,
 uint32_t firstMaterialRow, uint32_t endMaterialRow, NRISceneMaterialTextureSlotResolveStats* outStats)
{
 PerfAuditScope auditResolve(PerfAuditMetric::TextureResolveNs);
 PerfAuditAdd(PerfAuditMetric::TextureResolveCalls);
 NRISceneMaterialTextureSlotResolveStats stats;
 const size_t end = std::min<size_t>(endMaterialRow, materials.materials.size());
 for (uint32_t row = firstMaterialRow; row < end; ++row)
 {
  ResolveRow(remap, materials.materials[row], stats);
  if (row < materials.lightMetadata.size())
  {
   auto& metadata = materials.lightMetadata[row];
   metadata.textureIndex = remap.Required(metadata.textureIndex, stats);
   metadata.glowmapTextureIndex = remap.Optional(metadata.glowmapTextureIndex, stats);
   metadata.normalTextureIndex = remap.Optional(metadata.normalTextureIndex, stats);
   metadata.metallicTextureIndex = remap.Optional(metadata.metallicTextureIndex, stats);
   metadata.roughnessTextureIndex = remap.Optional(metadata.roughnessTextureIndex, stats);
   metadata.emissiveTextureIndex = remap.Optional(metadata.emissiveTextureIndex, stats);
  }
 }
 Report(stats);
 if (outStats) *outStats = stats;
}

void NRIResolveSceneMaterialBridgeTextureSlots(
 const NRISceneTextureSlotTable& slotTable, nri_scene::MaterialBridgeData& materials,
 uint32_t firstMaterialRow, NRISceneMaterialTextureSlotResolveStats* outStats)
{
 NRIResolveSceneMaterialBridgeTextureSlotRange(slotTable, materials, firstMaterialRow,
  (uint32_t)materials.materials.size(), outStats);
}

void NRIResolveSceneMaterialBridgeTextureSlotRange(
 const NRISceneTextureSlotTable& slotTable, nri_scene::MaterialBridgeData& materials,
 uint32_t firstMaterialRow, uint32_t endMaterialRow, NRISceneMaterialTextureSlotResolveStats* outStats)
{
 nri_scene::MaterialBridgeData reference;
 NRISceneMaterialTextureSlotResolveStats referenceStats;
 if ((bool)nri_pttextureresolvevalidate)
 {
  PerfAuditScope auditReference(PerfAuditMetric::TextureResolveValidationNs);
  reference = materials;
  NRIResolveSceneMaterialBridgeTextureSlotRangeReference(slotTable, reference,
   firstMaterialRow, endMaterialRow, &referenceStats);
 }
 NRISceneMaterialTextureSlotRemap remap;
 const bool hasRows = firstMaterialRow < std::min<size_t>(endMaterialRow, materials.materials.size());
 if (hasRows) remap.Prepare(slotTable, materials);
 NRISceneMaterialTextureSlotResolveStats stats;
 NRIResolveSceneMaterialBridgeTextureSlotRangeWithRemap(remap, materials, firstMaterialRow, endMaterialRow, &stats);
 stats.keyLookups = hasRows ? materials.textures.size() : 0;
 if ((bool)nri_pttextureresolvevalidate)
 {
  PerfAuditScope auditReference(PerfAuditMetric::TextureResolveValidationNs);
  PerfAuditAdd(PerfAuditMetric::TextureResolveValidationChecks);
  if (!SameFallbackStats(stats, referenceStats) || !nri_scene::MaterialBridgesExactlyEqual(materials, reference))
  { Mismatch("bridge-range"); materials = std::move(reference); stats = referenceStats; }
 }
 if (outStats) *outStats = stats;
}
