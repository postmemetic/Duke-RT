#include "nri_emissive_tail_composition.h"
#include "../scene/nri_scene_view_scratch.h"
#include "../scene/nri_voxel_material_slots.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace
{
using namespace nri_scene;
const std::vector<SurfaceRef>& Surfaces(const SceneView& view, size_t category)
{
	return category == 0 ? view.opaqueWalls : category == 1 ? view.opaqueFlats : view.opaqueSprites;
}
bool Fits(SceneProductRange range, size_t size)
{
	return range.first <= size && range.count <= size - range.first;
}
bool SameTexture(const TextureUpload& lhs, const TextureUpload& rhs)
{
	return lhs.key == rhs.key && lhs.width == rhs.width && lhs.height == rhs.height &&
		lhs.mipCount == rhs.mipCount && lhs.indexed == rhs.indexed &&
		lhs.sourceTexture == rhs.sourceTexture && lhs.pixels.signature() == rhs.pixels.signature() && lhs.pixels == rhs.pixels;
}
template<class T> bool SameBytes(const T& lhs, const T& rhs)
{
	return std::memcmp(&lhs, &rhs, sizeof(T)) == 0;
}
template<class T> bool SameVectorBytes(const std::vector<T>& lhs, const std::vector<T>& rhs)
{
	return lhs.size() == rhs.size() && (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size() * sizeof(T)) == 0);
}
bool SameProvenance(const SurfaceProvenance& lhs, const SurfaceProvenance& rhs)
{
#define SAME(field) SameBytes(lhs.field, rhs.field)
	return SAME(sourceType) && SAME(sectorIndex) && SAME(wallIndex) && SAME(sectionIndex) && SAME(mapChunkIndex) &&
		SAME(nextSectorIndex) && SAME(actorIndex) && SAME(drawListType) && SAME(cstat) && SAME(materialFlags) &&
		SAME(actorOverlayRuleCount) && SAME(actorOverlayRuleIds);
#undef SAME
}
bool SameStatsEntry(const VoxelDuplicateVariantTraceEntry& lhs, const VoxelDuplicateVariantTraceEntry& rhs)
{
	return SameBytes(lhs.valid, rhs.valid) &&
		SameBytes(lhs.meshKeyHash, rhs.meshKeyHash) &&
		SameBytes(lhs.exampleBasisSignature, rhs.exampleBasisSignature) &&
		SameBytes(lhs.sourcePicnum, rhs.sourcePicnum) &&
		SameBytes(lhs.actorCount, rhs.actorCount) &&
		SameBytes(lhs.persistentActorCount, rhs.persistentActorCount) &&
		SameBytes(lhs.uniqueBasisSignatureCount, rhs.uniqueBasisSignatureCount) &&
		SameBytes(lhs.transformKeyedActorCount, rhs.transformKeyedActorCount) &&
		SameBytes(lhs.bakeSpace, rhs.bakeSpace) &&
		SameBytes(lhs.primitiveCountPerActor, rhs.primitiveCountPerActor) &&
		SameBytes(lhs.totalDuplicatedPrimitives, rhs.totalDuplicatedPrimitives) &&
		SameBytes(lhs.duplicatedBytes, rhs.duplicatedBytes);
}
bool SameStatsEntry(const DynamicVoxelEscapeTraceEntry& lhs, const DynamicVoxelEscapeTraceEntry& rhs)
{
	return SameBytes(lhs.valid, rhs.valid) &&
		SameBytes(lhs.reason, rhs.reason) &&
		SameBytes(lhs.actorIndex, rhs.actorIndex) &&
		SameBytes(lhs.statnum, rhs.statnum) &&
		SameBytes(lhs.sourcePicnum, rhs.sourcePicnum) &&
		SameBytes(lhs.resolvedVoxelIndex, rhs.resolvedVoxelIndex) &&
		SameBytes(lhs.meshVariantHash, rhs.meshVariantHash) &&
		SameBytes(lhs.materialVariantHash, rhs.materialVariantHash) &&
		SameBytes(lhs.primitiveCount, rhs.primitiveCount) &&
		SameBytes(lhs.vertexBytes, rhs.vertexBytes) &&
		SameBytes(lhs.indexBytes, rhs.indexBytes) &&
		SameBytes(lhs.primitiveBytes, rhs.primitiveBytes) &&
		SameBytes(lhs.materialBytes, rhs.materialBytes) &&
		SameBytes(lhs.totalBytes, rhs.totalBytes) &&
		SameBytes(lhs.persistentReady, rhs.persistentReady) &&
		SameBytes(lhs.hasCachedSurface, rhs.hasCachedSurface);
}
bool SameSceneStats(const SceneDebugStats& lhs, const SceneDebugStats& rhs)
{
	if (!(SameBytes(lhs.totalDrawItems, rhs.totalDrawItems) &&
		SameBytes(lhs.wallDrawItems, rhs.wallDrawItems) &&
		SameBytes(lhs.flatDrawItems, rhs.flatDrawItems) &&
		SameBytes(lhs.spriteDrawItems, rhs.spriteDrawItems) &&
		SameBytes(lhs.translucentDrawItems, rhs.translucentDrawItems) &&
		SameBytes(lhs.triangleEstimate, rhs.triangleEstimate) &&
		SameBytes(lhs.materialRefs, rhs.materialRefs) &&
		SameBytes(lhs.mirrorSurfaces, rhs.mirrorSurfaces) &&
		SameBytes(lhs.skySurfaces, rhs.skySurfaces) &&
		SameBytes(lhs.portalViews, rhs.portalViews) &&
		SameBytes(lhs.portalCapturesSkipped, rhs.portalCapturesSkipped) &&
		SameBytes(lhs.modelDrawItems, rhs.modelDrawItems) &&
		SameBytes(lhs.voxelProxyDrawItems, rhs.voxelProxyDrawItems) &&
		SameBytes(lhs.unsupportedModelDrawItems, rhs.unsupportedModelDrawItems) &&
		SameBytes(lhs.voxelStableCandidates, rhs.voxelStableCandidates) &&
		SameBytes(lhs.voxelStableUncacheable, rhs.voxelStableUncacheable) &&
		SameBytes(lhs.voxelStableSignatureHits, rhs.voxelStableSignatureHits) &&
		SameBytes(lhs.voxelStableSignatureMisses, rhs.voxelStableSignatureMisses) &&
		SameBytes(lhs.voxelStableSignatureChanges, rhs.voxelStableSignatureChanges) &&
		SameBytes(lhs.voxelStableSplitStable, rhs.voxelStableSplitStable) &&
		SameBytes(lhs.voxelStableSplitLive, rhs.voxelStableSplitLive) &&
		SameBytes(lhs.voxelCacheEntries, rhs.voxelCacheEntries) &&
		SameBytes(lhs.voxelCacheSurfaceHits, rhs.voxelCacheSurfaceHits) &&
		SameBytes(lhs.voxelCacheSurfaceStores, rhs.voxelCacheSurfaceStores) &&
		SameBytes(lhs.voxelCacheSurfaceRebuilds, rhs.voxelCacheSurfaceRebuilds) &&
		SameBytes(lhs.voxelCacheTransformRebakes, rhs.voxelCacheTransformRebakes) &&
		SameBytes(lhs.voxelCacheSurfaceRemoves, rhs.voxelCacheSurfaceRemoves) &&
		SameBytes(lhs.voxelCacheNotCaptured, rhs.voxelCacheNotCaptured) &&
		SameBytes(lhs.voxelCacheDeferred, rhs.voxelCacheDeferred) &&
		SameBytes(lhs.voxelCachePrimitives, rhs.voxelCachePrimitives) &&
		SameBytes(lhs.voxelCacheActorSurfaces, rhs.voxelCacheActorSurfaces) &&
		SameBytes(lhs.voxelCacheUniqueMeshKeys, rhs.voxelCacheUniqueMeshKeys) &&
		SameBytes(lhs.voxelCacheUniqueMaterialKeys, rhs.voxelCacheUniqueMaterialKeys) &&
		SameBytes(lhs.voxelCacheLocalSpaceSurfaces, rhs.voxelCacheLocalSpaceSurfaces) &&
		SameBytes(lhs.voxelCacheBakedTransformSurfaces, rhs.voxelCacheBakedTransformSurfaces) &&
		SameBytes(lhs.voxelCacheUnknownSpaceSurfaces, rhs.voxelCacheUnknownSpaceSurfaces) &&
		SameBytes(lhs.voxelCacheTransformKeyedSurfaces, rhs.voxelCacheTransformKeyedSurfaces) &&
		SameBytes(lhs.voxelCacheUniqueTransformBases, rhs.voxelCacheUniqueTransformBases) &&
		SameBytes(lhs.voxelCacheInvariantWarnings, rhs.voxelCacheInvariantWarnings) &&
		SameBytes(lhs.voxelCacheDuplicatedVertexBytes, rhs.voxelCacheDuplicatedVertexBytes) &&
		SameBytes(lhs.voxelCacheDuplicatedIndexBytes, rhs.voxelCacheDuplicatedIndexBytes) &&
		SameBytes(lhs.voxelCacheDuplicatedPrimitiveBytes, rhs.voxelCacheDuplicatedPrimitiveBytes) &&
		SameBytes(lhs.voxelCacheDuplicatedTotalBytes, rhs.voxelCacheDuplicatedTotalBytes) &&
		SameBytes(lhs.voxelCacheDuplicateTopCount, rhs.voxelCacheDuplicateTopCount) &&
		SameBytes(lhs.dynamicVoxelEscapeActorCount, rhs.dynamicVoxelEscapeActorCount) &&
		SameBytes(lhs.dynamicVoxelEscapeEligibleActorCount, rhs.dynamicVoxelEscapeEligibleActorCount) &&
		SameBytes(lhs.dynamicVoxelEscapeForcedActorCount, rhs.dynamicVoxelEscapeForcedActorCount) &&
		SameBytes(lhs.dynamicVoxelEscapePrimitiveCount, rhs.dynamicVoxelEscapePrimitiveCount) &&
		SameBytes(lhs.dynamicVoxelEscapeVertexBytes, rhs.dynamicVoxelEscapeVertexBytes) &&
		SameBytes(lhs.dynamicVoxelEscapeIndexBytes, rhs.dynamicVoxelEscapeIndexBytes) &&
		SameBytes(lhs.dynamicVoxelEscapePrimitiveBytes, rhs.dynamicVoxelEscapePrimitiveBytes) &&
		SameBytes(lhs.dynamicVoxelEscapeMaterialBytes, rhs.dynamicVoxelEscapeMaterialBytes) &&
		SameBytes(lhs.dynamicVoxelEscapeTotalBytes, rhs.dynamicVoxelEscapeTotalBytes) &&
		SameBytes(lhs.dynamicVoxelExpectedEscapeActorCount, rhs.dynamicVoxelExpectedEscapeActorCount) &&
		SameBytes(lhs.dynamicVoxelUnexpectedEscapeActorCount, rhs.dynamicVoxelUnexpectedEscapeActorCount) &&
		SameBytes(lhs.dynamicVoxelExpectedEscapePrimitiveCount, rhs.dynamicVoxelExpectedEscapePrimitiveCount) &&
		SameBytes(lhs.dynamicVoxelUnexpectedEscapePrimitiveCount, rhs.dynamicVoxelUnexpectedEscapePrimitiveCount) &&
		SameBytes(lhs.dynamicVoxelExpectedEscapeTotalBytes, rhs.dynamicVoxelExpectedEscapeTotalBytes) &&
		SameBytes(lhs.dynamicVoxelUnexpectedEscapeTotalBytes, rhs.dynamicVoxelUnexpectedEscapeTotalBytes) &&
		SameBytes(lhs.dynamicVoxelEscapeTopCount, rhs.dynamicVoxelEscapeTopCount) &&
		SameBytes(lhs.dynamicVoxelUnexpectedEscapeTopCount, rhs.dynamicVoxelUnexpectedEscapeTopCount))) return false;
	for (size_t i = 0; i < lhs.voxelCacheDuplicateTopEntries.size(); ++i)
		if (!SameStatsEntry(lhs.voxelCacheDuplicateTopEntries[i], rhs.voxelCacheDuplicateTopEntries[i])) return false;
	for (size_t i = 0; i < lhs.dynamicVoxelEscapeTopEntries.size(); ++i)
		if (!SameStatsEntry(lhs.dynamicVoxelEscapeTopEntries[i], rhs.dynamicVoxelEscapeTopEntries[i])) return false;
	for (size_t i = 0; i < lhs.dynamicVoxelUnexpectedEscapeTopEntries.size(); ++i)
		if (!SameStatsEntry(lhs.dynamicVoxelUnexpectedEscapeTopEntries[i], rhs.dynamicVoxelUnexpectedEscapeTopEntries[i])) return false;
	return true;
}
bool ValidLayout(const SceneView& view, const GeometryData& geometry, const GeometryProductLayout& geometryLayout,
	const MaterialBridgeData& materials, const MaterialProductLayout& materialLayout)
{
	if (!geometryLayout.valid || !materialLayout.valid || geometry.primitives.size() != geometry.primitiveProvenance.size() ||
		materials.materials.size() != materials.lightMetadata.size() ||
		materialLayout.textureLookupKeys.size() != materials.textures.size()) return false;
	size_t vertices = 0, indices = 0, primitives = 0, rows = 0, textures = 0;
	for (size_t category = 0; category < 3; ++category)
	{
		const auto& geo = geometryLayout.groups[category];
		const auto& mat = materialLayout.groups[category];
		size_t expectedRows = 0;
		for (const auto& surface : Surfaces(view, category)) expectedRows += NormalizeVoxelMaterialRowSpan(surface.materialRowSpan);
		if (geo.vertices.first != vertices || geo.indices.first != indices || geo.primitives.first != primitives ||
			geo.materials.first != rows || mat.materials.first != rows || mat.textures.first != textures ||
			geo.materials.count != expectedRows || mat.materials.count != expectedRows ||
			!Fits(geo.vertices, geometry.vertices.size()) || !Fits(geo.indices, geometry.indices.size()) ||
			!Fits(geo.primitives, geometry.primitives.size()) || !Fits(mat.materials, materials.materials.size()) ||
			!Fits(mat.textures, materials.textures.size())) return false;
		vertices += geo.vertices.count; indices += geo.indices.count; primitives += geo.primitives.count;
		rows += mat.materials.count; textures += mat.textures.count;
	}
	return vertices == geometry.vertices.size() && indices == geometry.indices.size() && primitives == geometry.primitives.size() &&
		rows == materials.materials.size() && textures == materials.textures.size();
}
}

void NRIEmissiveTailScratch::Clear()
{
	nri_scene::ClearSceneViewRetainingCapacity(view);
	nri_scene::ClearGeometryRetainingCapacity(geometry);
	nri_scene::ClearMaterialBridgeRetainingCapacity(materials);
	materialLayout.Clear();
	textureLookup.clear(); liveTextureRemap.clear(); tailTextureRemap.clear();
}

bool ComposeNRIEmissiveTailProducts(
	const nri_scene::SceneView& liveView, const nri_scene::GeometryData& liveGeometry,
	const nri_scene::GeometryProductLayout& liveGeometryLayout,
	const nri_scene::MaterialBridgeData& liveMaterials, const nri_scene::MaterialProductLayout& liveMaterialLayout,
	const nri_scene::SceneView& tailView, const nri_scene::GeometryData& tailGeometry,
	const nri_scene::GeometryProductLayout& tailGeometryLayout,
	const nri_scene::MaterialBridgeData& tailMaterials, const nri_scene::MaterialProductLayout& tailMaterialLayout,
	NRIEmissiveTailScratch& scratch, nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials)
{
	using namespace nri_scene;
	if (!ValidLayout(liveView, liveGeometry, liveGeometryLayout, liveMaterials, liveMaterialLayout) ||
		!ValidLayout(tailView, tailGeometry, tailGeometryLayout, tailMaterials, tailMaterialLayout) ||
		liveView.primitiveFlags != tailView.primitiveFlags || liveMaterials.paletteWidth != tailMaterials.paletteWidth ||
		liveMaterials.paletteHeight != tailMaterials.paletteHeight ||
		liveMaterials.paletteLookup.signature() != tailMaterials.paletteLookup.signature() ||
		liveMaterials.paletteLookup != tailMaterials.paletteLookup)
		return false;
	const auto safeSum = [](size_t lhs, size_t rhs) { return lhs <= UINT32_MAX && rhs <= UINT32_MAX - lhs; };
	if (!safeSum(liveGeometry.vertices.size(), tailGeometry.vertices.size()) ||
		!safeSum(liveGeometry.indices.size(), tailGeometry.indices.size()) ||
		!safeSum(liveGeometry.primitives.size(), tailGeometry.primitives.size()) ||
		!safeSum(liveMaterials.materials.size(), tailMaterials.materials.size()) ||
		!safeSum(liveMaterials.textures.size(), tailMaterials.textures.size())) return false;
	ClearGeometryRetainingCapacity(outGeometry);
	ClearMaterialBridgeRetainingCapacity(outMaterials);
	outGeometry.vertices.reserve(liveGeometry.vertices.size() + tailGeometry.vertices.size());
	outGeometry.indices.reserve(liveGeometry.indices.size() + tailGeometry.indices.size());
	outGeometry.primitives.reserve(liveGeometry.primitives.size() + tailGeometry.primitives.size());
	outGeometry.primitiveProvenance.reserve(liveGeometry.primitives.size() + tailGeometry.primitives.size());
	outMaterials.materials.reserve(liveMaterials.materials.size() + tailMaterials.materials.size());
	outMaterials.lightMetadata.reserve(liveMaterials.lightMetadata.size() + tailMaterials.lightMetadata.size());
	outMaterials.textures.reserve(liveMaterials.textures.size() + tailMaterials.textures.size());
	outMaterials.paletteLookup = liveMaterials.paletteLookup;
	outMaterials.paletteWidth = liveMaterials.paletteWidth; outMaterials.paletteHeight = liveMaterials.paletteHeight;
	scratch.textureLookup.clear();
	scratch.textureLookup.reserve(liveMaterials.textures.size() + tailMaterials.textures.size());
	scratch.liveTextureRemap.assign(liveMaterials.textures.size(), UINT32_MAX);
	scratch.tailTextureRemap.assign(tailMaterials.textures.size(), UINT32_MAX);
	const auto addTextures = [&](const MaterialBridgeData& source, const MaterialProductLayout& layout,
		SceneProductRange range, std::vector<uint32_t>& remap)
	{
		for (size_t index = range.first; index < range.first + range.count; ++index)
		{
			const auto& texture = source.textures[index];
			auto inserted = scratch.textureLookup.emplace(layout.textureLookupKeys[index], (uint32_t)outMaterials.textures.size());
			if (inserted.second) outMaterials.textures.push_back(texture);
			else if (!SameTexture(outMaterials.textures[inserted.first->second], texture)) return false;
			remap[index] = inserted.first->second;
		}
		return true;
	};
	for (size_t category = 0; category < 3; ++category)
		if (!addTextures(liveMaterials, liveMaterialLayout, liveMaterialLayout.groups[category].textures, scratch.liveTextureRemap) ||
			!addTextures(tailMaterials, tailMaterialLayout, tailMaterialLayout.groups[category].textures, scratch.tailTextureRemap)) return false;
	const auto appendGroup = [&](const SceneView& view, const GeometryData& geometry, const GeometryProductGroup& geo,
		const MaterialBridgeData& materials, const MaterialProductGroup& mat, const std::vector<uint32_t>& remap, size_t category, bool refreshLiveProvenance)
	{
		const uint32_t vertexBase = (uint32_t)outGeometry.vertices.size();
		const uint32_t materialBase = (uint32_t)outMaterials.materials.size();
		outGeometry.vertices.insert(outGeometry.vertices.end(), geometry.vertices.begin() + geo.vertices.first,
			geometry.vertices.begin() + geo.vertices.first + geo.vertices.count);
		const auto remapVertex = [&](uint32_t index, uint32_t& result)
		{
			if (index < geo.vertices.first || index - geo.vertices.first >= geo.vertices.count) return false;
			result = vertexBase + (uint32_t)(index - geo.vertices.first); return true;
		};
		for (size_t index = geo.indices.first; index < geo.indices.first + geo.indices.count; ++index)
		{
			uint32_t value; if (!remapVertex(geometry.indices[index], value)) return false;
			outGeometry.indices.push_back(value);
		}
		const auto& surfaces = Surfaces(view, category);
		size_t surfaceIndex = 0, surfaceFirstRow = geo.materials.first;
		for (size_t index = geo.primitives.first; index < geo.primitives.first + geo.primitives.count; ++index)
		{
			auto primitive = geometry.primitives[index];
			if (primitive.materialIndex < geo.materials.first || primitive.materialIndex - geo.materials.first >= geo.materials.count)
				return false;
			// Geometry emits surfaces in canonical row order; local voxel slots may
			// be nonmonotonic within a surface but cannot cross its complete row span.
			while (surfaceIndex < surfaces.size() && primitive.materialIndex >= surfaceFirstRow + NormalizeVoxelMaterialRowSpan(surfaces[surfaceIndex].materialRowSpan))
			{
				surfaceFirstRow += NormalizeVoxelMaterialRowSpan(surfaces[surfaceIndex].materialRowSpan); ++surfaceIndex;
			}
			if (surfaceIndex >= surfaces.size() || primitive.materialIndex < surfaceFirstRow) return false;
			for (uint32_t& vertex : primitive.indices) if (!remapVertex(vertex, vertex)) return false;
			primitive.materialIndex = materialBase + (uint32_t)(primitive.materialIndex - geo.materials.first);
			outGeometry.primitives.push_back(primitive);
			// Live geometry predates its material wrapper stamp. Retained geometry
			// is built immediately before its wrapper, as in the full merged build.
			outGeometry.primitiveProvenance.push_back(refreshLiveProvenance ?
				surfaces[surfaceIndex].provenance : geometry.primitiveProvenance[index]);
		}
		const auto texture = [&](uint32_t index) { return index < remap.size() ? remap[index] : index; };
		for (size_t row = mat.materials.first; row < mat.materials.first + mat.materials.count; ++row)
		{
			auto material = materials.materials[row]; auto metadata = materials.lightMetadata[row];
#define REMAP(field) field = texture(field)
			REMAP(material.textureIndex); REMAP(material.normalTextureIndex); REMAP(material.metallicTextureIndex);
			REMAP(material.roughnessTextureIndex); REMAP(material.emissiveTextureIndex);
			REMAP(metadata.textureIndex); REMAP(metadata.glowmapTextureIndex); REMAP(metadata.normalTextureIndex);
			REMAP(metadata.metallicTextureIndex); REMAP(metadata.roughnessTextureIndex); REMAP(metadata.emissiveTextureIndex);
#undef REMAP
			outMaterials.materials.push_back(material); outMaterials.lightMetadata.push_back(metadata);
		}
		return true;
	};
	for (size_t category = 0; category < 3; ++category)
		if (!appendGroup(liveView, liveGeometry, liveGeometryLayout.groups[category], liveMaterials, liveMaterialLayout.groups[category], scratch.liveTextureRemap, category, true) ||
			!appendGroup(tailView, tailGeometry, tailGeometryLayout.groups[category], tailMaterials, tailMaterialLayout.groups[category], scratch.tailTextureRemap, category, false)) return false;
	return true;
}

bool NRIEmissiveTailGeometryEqual(const nri_scene::GeometryData& lhs, const nri_scene::GeometryData& rhs)
{
	if (!SameVectorBytes(lhs.vertices, rhs.vertices) || !SameVectorBytes(lhs.indices, rhs.indices) ||
		!SameVectorBytes(lhs.primitives, rhs.primitives) || lhs.primitiveProvenance.size() != rhs.primitiveProvenance.size()) return false;
	for (size_t i = 0; i < lhs.primitiveProvenance.size(); ++i)
		if (!SameProvenance(lhs.primitiveProvenance[i], rhs.primitiveProvenance[i])) return false;
	return true;
}

bool NRIEmissiveTailMaterialsEqual(const nri_scene::MaterialBridgeData& a, const nri_scene::MaterialBridgeData& b)
{
	if (!SameVectorBytes(a.materials, b.materials) || a.lightMetadata.size() != b.lightMetadata.size() ||
		a.textures.size() != b.textures.size() || a.paletteWidth != b.paletteWidth || a.paletteHeight != b.paletteHeight ||
		a.paletteLookup.signature() != b.paletteLookup.signature() || a.paletteLookup != b.paletteLookup) return false;
	for (size_t i = 0; i < a.lightMetadata.size(); ++i)
	{
		const auto& lhs = a.lightMetadata[i]; const auto& rhs = b.lightMetadata[i];
#define SAME(field) SameBytes(lhs.field, rhs.field)
		if (!(SAME(texture) && SAME(materialKey) && SAME(textureContentKey) && SAME(glowmapContentKey) &&
			SAME(normalContentKey) && SAME(metallicContentKey) && SAME(roughnessContentKey) && SAME(textureId) &&
			SAME(baseTextureId) && SAME(authoredTextureId) && SAME(textureIndex) && SAME(glowmapTextureIndex) &&
			SAME(normalTextureIndex) && SAME(metallicTextureIndex) && SAME(roughnessTextureIndex) && SAME(emissiveTextureIndex) &&
			SAME(paletteIndex) && SAME(materialFlags) && SAME(lightingFlags) && SAME(materialClass) && SAME(emissiveMode) &&
			SAME(emissiveStableFrames) && SAME(voxelPaletteIndex) && SAME(voxelPalettePolicyFlags) && SAME(voxelPalettePolicyApplied) &&
			SAME(sourceType) && SAME(sectorIndex) && SAME(actorIndex) && SAME(actorOverlayRuleCount) && SAME(actorOverlayRuleIds) &&
			SAME(shade) && SAME(alpha) && SAME(lightLevel) && SAME(averageColor) && SAME(glowColor) && SAME(emissiveColor) &&
			SAME(emissiveIntensity) && SAME(emissiveMaskScale) && SAME(visibleFullbrightBoost))) return false;
#undef SAME
	}
	for (size_t i = 0; i < a.textures.size(); ++i) if (!SameTexture(a.textures[i], b.textures[i])) return false;
	return true;
}

bool NRIEmissiveTailViewsEqual(const nri_scene::SceneView& lhs, const nri_scene::SceneView& rhs)
{
#define SAME(field) SameBytes(lhs.field, rhs.field)
	if (!SameSceneStats(lhs.stats, rhs.stats)) return false;
	if (!(SAME(drawInfo) && SAME(primitiveFlags) && SAME(skyColor) && SAME(groundColor) && SAME(sky.mode) &&
		SAME(sky.sourceType) && SAME(sky.texture) && SAME(sky.faceMask) && SAME(sky.priority) && SAME(sky.flipTop) && SAME(sky.isThreeFace)))
		return false;
#undef SAME
	for (size_t category = 0; category < 3; ++category)
	{
		const auto& a = Surfaces(lhs, category); const auto& b = Surfaces(rhs, category);
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i)
		{
			const auto& lhs = a[i]; const auto& rhs = b[i];
#define SAME(field) SameBytes(lhs.field, rhs.field)
			if (!(SAME(materialRowSpan) && SameVectorBytes(lhs.indices, rhs.indices) &&
				SameVectorBytes(lhs.primitiveLocalMaterialSlots, rhs.primitiveLocalMaterialSlots) &&
				SAME(material.texture) && SAME(material.emissiveSourceTexture) && SAME(material.authoredTextureId) &&
				SAME(material.palette) && SAME(material.shade) && SAME(material.alpha) && SAME(material.flags) &&
				lhs.material.voxelPalettePolicy == rhs.material.voxelPalettePolicy && SAME(material.voxelPalettePolicyContentKey) &&
				SameProvenance(lhs.provenance, rhs.provenance) && SAME(temporal.occurrenceId) && SAME(temporal.topologyKey) &&
				SAME(temporal.generation) && SAME(temporal.historyAge) && SAME(temporal.materialVerticalReference) &&
				SAME(temporal.materialUvAnchor) && SAME(temporal.materialUvTexelScale) && SAME(temporal.materialUvReactive) &&
				SAME(temporal.reason) && SAME(temporal.identityValid) && SAME(temporal.correspondenceValid) &&
				SAME(temporal.materialVerticalReferenceValid) && SAME(temporal.materialUvAnchorValid))) return false;
#undef SAME
			if (lhs.vertices.size() != rhs.vertices.size()) return false;
			for (size_t vertex = 0; vertex < lhs.vertices.size(); ++vertex)
			{
				const auto& x = lhs.vertices[vertex]; const auto& y = rhs.vertices[vertex];
				if (!SameBytes(x.position, y.position) || !SameBytes(x.prevPosition, y.prevPosition) || !SameBytes(x.uv, y.uv) ||
					x.temporalCornerKey != y.temporalCornerKey || x.fluidCarrierNormal != y.fluidCarrierNormal) return false;
			}
		}
	}
	return true;
}
