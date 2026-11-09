#include "nri_static_filter_cache.h"
#include "nri_static_scene.h"
#include "nri_filter_candidate_policy.h"
#include "nri_diagnostic_names.h"
#include "perf_audit.h"
#include "printf.h"

#include <algorithm>

namespace
{
uint32_t MaterialPolicy(const nri_scene::MaterialData& material)
{
	uint32_t mask = NRIFilterCandidatePolicy_None;
	if ((material.flags & nri_scene::MaterialFlag_OneWay) != 0u)
		mask |= NRIFilterCandidatePolicy_OneWay;
	if ((material.lightingFlags & nri_scene::MaterialLightingFlag_NoShadowCast) != 0u)
		mask |= NRIFilterCandidatePolicy_NoShadow;
	if ((material.flags & nri_scene::MaterialFlag_AlphaClip) != 0u || material.alpha < 0.999f)
		mask |= NRIFilterCandidatePolicy_Alpha;
	return mask;
}

struct StaticFilterAudit
{
	PerfAuditScope timer{ PerfAuditMetric::StaticFilterNs };
	bool enabled = PerfAuditActive();
	uint64_t instances = 0, staticInstances = 0, directHits = 0;
	uint64_t registryFallbackEntries = 0, chunkFallbackEntries = 0;
	uint64_t invalidSpans = 0, hiddenDetermined = 0, primitiveRows = 0;
	uint64_t invalidMaterials = 0, forced = 0, earlyOuts = 0;
	uint64_t hits = 0, builds = 0, fallbacks = 0, checks = 0, mismatches = 0, referenceRows = 0;
	~StaticFilterAudit()
	{
		if (!enabled) return;
		PerfAuditAdd(PerfAuditMetric::StaticFilterCalls);
		PerfAuditAdd(PerfAuditMetric::StaticFilterInstances, instances);
		PerfAuditAdd(PerfAuditMetric::StaticFilterStaticInstances, staticInstances);
		PerfAuditAdd(PerfAuditMetric::StaticFilterRegistryDirectHits, directHits);
		PerfAuditAdd(PerfAuditMetric::StaticFilterRegistryFallbackEntries, registryFallbackEntries);
		PerfAuditAdd(PerfAuditMetric::StaticFilterChunkFallbackEntries, chunkFallbackEntries);
		PerfAuditAdd(PerfAuditMetric::StaticFilterEmptyOrInvalidSpans, invalidSpans);
		PerfAuditAdd(PerfAuditMetric::StaticFilterHiddenDeterminedInstances, hiddenDetermined);
		PerfAuditAdd(PerfAuditMetric::StaticFilterPrimitiveRowsVisited, primitiveRows);
		PerfAuditAdd(PerfAuditMetric::StaticFilterInvalidMaterialRows, invalidMaterials);
		PerfAuditAdd(PerfAuditMetric::StaticFilterForcedInstances, forced);
		PerfAuditAdd(PerfAuditMetric::StaticFilterVisibilityEarlyOuts, earlyOuts);
		PerfAuditAdd(PerfAuditMetric::StaticFilterCacheHits, hits);
		PerfAuditAdd(PerfAuditMetric::StaticFilterCacheBuilds, builds);
		PerfAuditAdd(PerfAuditMetric::StaticFilterCacheFallbacks, fallbacks);
		PerfAuditAdd(PerfAuditMetric::StaticFilterValidationChecks, checks);
		PerfAuditAdd(PerfAuditMetric::StaticFilterValidationMismatches, mismatches);
		PerfAuditAdd(PerfAuditMetric::StaticFilterValidationPrimitiveRows, referenceRows);
	}
};

struct StaticSpan { uint32_t count = 0; uint32_t slot = UINT32_MAX; };

StaticSpan ResolveSpan(const StaticMapSceneCache& scene, const ResidentMapChunkRegistry& registry,
	uint32_t chunkIndex, uint32_t primitiveOffset, StaticFilterAudit& audit)
{
	if (registry.valid && chunkIndex < registry.entries.size())
	{
		const auto& entry = registry.entries[chunkIndex];
		if (entry.valid && entry.chunkIndex == chunkIndex && entry.primitiveOffset == primitiveOffset)
		{
			if (audit.enabled) ++audit.directHits;
			return { entry.primitiveCount, entry.staticSceneChunkListIndex };
		}
	}
	if (registry.valid)
	{
		for (const auto& entry : registry.entries)
		{
			if (audit.enabled) ++audit.registryFallbackEntries;
			if (entry.valid && entry.chunkIndex == chunkIndex && entry.primitiveOffset == primitiveOffset)
				return { entry.primitiveCount, entry.staticSceneChunkListIndex };
		}
	}
	for (uint32_t slot = 0; slot < scene.chunks.size(); ++slot)
	{
		if (audit.enabled) ++audit.chunkFallbackEntries;
		const auto& chunk = scene.chunks[slot];
		if (chunk.active && chunk.chunkIndex == chunkIndex && chunk.primitiveOffset == primitiveOffset)
			return { chunk.primitiveCount, slot };
	}
	return {};
}

// Deliberately independent of MaterialPolicy and cached classification. Keep the
// original short-circuit predicate and invalid-material-before-reflection order.
bool RawRequiresCandidate(const StaticMapSceneCache& scene, const SceneInstanceData& instance,
	uint32_t count, uint32_t enabledPolicyMask, bool hiddenDetermines,
	uint64_t& rows, uint64_t& invalidMaterials, bool countWork)
{
	bool requiresCandidate = hiddenDetermines;
	for (uint32_t local = 0; local < count; ++local)
	{
		if (countWork) ++rows;
		const auto& primitive = scene.geometry.primitives[instance.primitiveBase + local];
		if (primitive.materialIndex >= scene.gpuMaterials.size())
		{
			if (countWork) ++invalidMaterials;
			continue;
		}
		const auto& material = scene.gpuMaterials[primitive.materialIndex];
		if (((enabledPolicyMask & NRIFilterCandidatePolicy_ReflectionOnly) != 0u &&
				(primitive.flags & nri_scene::PrimitiveFlag_ReflectionOnly) != 0u) ||
			((enabledPolicyMask & NRIFilterCandidatePolicy_OneWay) != 0u &&
				(material.flags & nri_scene::MaterialFlag_OneWay) != 0u) ||
			((enabledPolicyMask & NRIFilterCandidatePolicy_NoShadow) != 0u &&
				(material.lightingFlags & nri_scene::MaterialLightingFlag_NoShadowCast) != 0u) ||
			((enabledPolicyMask & NRIFilterCandidatePolicy_Alpha) != 0u &&
				((material.flags & nri_scene::MaterialFlag_AlphaClip) != 0u || material.alpha < 0.999f)))
		{
			requiresCandidate = true;
			break;
		}
	}
	return requiresCandidate;
}
}

bool NRIStaticFilterCache::Key::operator==(const Key& rhs) const
{
	return contentBuildSerial == rhs.contentBuildSerial && materialRevision == rhs.materialRevision &&
		chunkIndex == rhs.chunkIndex && primitiveOffset == rhs.primitiveOffset && primitiveCount == rhs.primitiveCount &&
		materialOffset == rhs.materialOffset && materialCount == rhs.materialCount && instanceMaterialBase == rhs.instanceMaterialBase;
}

void NRIStaticFilterCache::InvalidateGeometry()
{
	mEntries.clear();
}

void NRIStaticFilterCache::InvalidateChunk(uint32_t slot)
{
	if (slot < mEntries.size()) mEntries[slot].valid = false;
}

void NRIStaticFilterCache::InvalidatePrimitiveRange(uint32_t first, uint32_t count)
{
	PerfAuditScope timer(PerfAuditMetric::StaticFilterPrimitiveInvalidationNs);
	if (count == 0) return;
	for (auto& entry : mEntries)
	{
		// Also invalidate aliases/old slots when a free atlas range is recycled.
		if (entry.valid && (uint64_t)entry.key.primitiveOffset + entry.key.primitiveCount > first &&
			(uint64_t)first + count > entry.key.primitiveOffset) entry.valid = false;
	}
	if (PerfAuditActive()) PerfAuditAdd(PerfAuditMetric::StaticFilterInvalidationEntriesVisited, mEntries.size());
}

void NRIStaticFilterCache::PublishMaterials(const std::vector<nri_scene::MaterialData>& materials,
	uint32_t first, uint32_t count)
{
	PerfAuditScope timer(PerfAuditMetric::StaticFilterMaterialPublishNs);
	const bool audit = PerfAuditActive();
	if (audit) PerfAuditAdd(PerfAuditMetric::StaticFilterMaterialPublications);
	bool changed = !mHasMaterialSnapshot || mMaterialPolicies.size() != materials.size();
	if (changed || count == UINT32_MAX)
	{
		first = 0;
		count = (uint32_t)materials.size();
	}
	if ((uint64_t)first + count > materials.size())
	{
		mMaterialsPublished = false;
		return;
	}
	mMaterialPolicies.resize(materials.size());
	for (uint32_t row = first; row < first + count; ++row)
	{
		const uint32_t policy = MaterialPolicy(materials[row]);
		changed = changed || mMaterialPolicies[row] != policy;
		mMaterialPolicies[row] = policy;
	}
	if (changed)
	{
		if (++mMaterialRevision == 0) { mMaterialRevision = 1; InvalidateGeometry(); }
		if (audit) PerfAuditAdd(PerfAuditMetric::StaticFilterMaterialPolicyChanges);
	}
	mHasMaterialSnapshot = true;
	mMaterialsPublished = mMaterialWriteDepth == 0;
	if (audit)
	{
		PerfAuditAdd(PerfAuditMetric::StaticFilterMaterialRowsCompared, count);
		PerfAuditSet(PerfAuditMetric::StaticFilterCapacityBytes, CapacityBytes());
	}
}

bool NRIStaticFilterCache::ResolveIntrinsicMask(const StaticMapSceneCache& scene, uint32_t slot,
	const SceneInstanceData& instance, uint32_t count, uint32_t& mask, bool& hit,
	uint64_t& rows, uint64_t& invalidMaterials)
{
	hit = false;
	if (mQuarantined || !mMaterialsPublished || mMaterialPolicies.size() != scene.gpuMaterials.size() ||
		scene.contentBuildSerial == 0 || slot >= scene.chunks.size()) return false;
	const auto& chunk = scene.chunks[slot];
	if (chunk.chunkIndex != instance.metadata0 || chunk.primitiveOffset != instance.primitiveBase ||
		chunk.primitiveCount != count || count == 0 || (uint64_t)instance.primitiveBase + count > scene.geometry.primitives.size())
		return false;
	const Key key{ scene.contentBuildSerial, mMaterialRevision, chunk.chunkIndex, instance.primitiveBase,
		count, chunk.materialOffset, chunk.materialCount, instance.materialBase };
	mEntries.resize(scene.chunks.size());
	auto& entry = mEntries[slot];
	if (entry.valid && entry.key == key)
	{
		hit = true;
		mask = entry.mask;
		return true;
	}
	mask = NRIFilterCandidatePolicy_None;
	const bool audit = PerfAuditActive();
	for (uint32_t local = 0; local < count; ++local)
	{
		if (audit) ++rows;
		const auto& primitive = scene.geometry.primitives[instance.primitiveBase + local];
		// Invalid material suppresses reflection-only too, matching the old path.
		if (primitive.materialIndex >= scene.gpuMaterials.size())
		{
			if (audit) ++invalidMaterials;
			continue;
		}
		mask |= mMaterialPolicies[primitive.materialIndex];
		if ((primitive.flags & nri_scene::PrimitiveFlag_ReflectionOnly) != 0u)
			mask |= NRIFilterCandidatePolicy_ReflectionOnly;
	}
	entry = { key, mask, true };
	return true;
}

uint64_t NRIStaticFilterCache::CapacityBytes() const
{
	return mEntries.capacity() * sizeof(Entry) + mMaterialPolicies.capacity() * sizeof(uint32_t);
}

void ApplyNRIStaticFilterCandidateFlags(StaticMapSceneCache& scene,
	const ResidentMapChunkRegistry& registry, const std::vector<uint32_t>& visibleChunkWords,
	std::vector<nri::TopLevelInstance>& instances, const std::vector<SceneInstanceData>& sceneInstances,
	const NRIStaticFilterOptions& options)
{
	StaticFilterAudit audit;
	if (audit.enabled) audit.instances = sceneInstances.size();
	if (!options.queryEnabled || instances.size() != sceneInstances.size()) return;
	auto& cache = scene.filterCandidateCache;
	for (size_t index = 0; index < sceneInstances.size(); ++index)
	{
		const auto& instance = sceneInstances[index];
		if (instance.dataSource != nri_diag::SceneDataSourceStatic) continue;
		if (audit.enabled) ++audit.staticInstances;
		const auto span = ResolveSpan(scene, registry, instance.metadata0, instance.primitiveBase, audit);
		if (span.count == 0 || (uint64_t)instance.primitiveBase + span.count > scene.geometry.primitives.size())
		{
			if (audit.enabled) ++audit.invalidSpans;
			continue;
		}
		const uint32_t word = instance.metadata0 >> 5u;
		const bool visible = word < visibleChunkWords.size() &&
			(visibleChunkWords[word] & (1u << (instance.metadata0 & 31u))) != 0u;
		const bool hiddenDetermines = (options.enabledPolicyMask & NRIFilterCandidatePolicy_Visibility) != 0u &&
			options.visibleChunkGate && !visible;
		if (audit.enabled && hiddenDetermines) ++audit.hiddenDetermined;
		bool requiresCandidate = hiddenDetermines;
		bool usedHiddenEarlyOut = false, usedCacheHit = false, usedCacheBuild = false;
		if (options.useCache && !cache.IsQuarantined() && hiddenDetermines)
		{
			usedHiddenEarlyOut = true;
			if (audit.enabled) ++audit.earlyOuts;
		}
		else
		{
			uint32_t intrinsic = 0;
			bool hit = false;
			if (options.useCache && cache.ResolveIntrinsicMask(scene, span.slot, instance, span.count,
				intrinsic, hit, audit.primitiveRows, audit.invalidMaterials))
			{
				requiresCandidate = hiddenDetermines || (intrinsic & options.enabledPolicyMask) != 0u;
				usedCacheHit = hit;
				usedCacheBuild = !hit;
				if (audit.enabled) { if (hit) ++audit.hits; else ++audit.builds; }
			}
			else
			{
				if (audit.enabled) ++audit.fallbacks;
				requiresCandidate = RawRequiresCandidate(scene, instance, span.count, options.enabledPolicyMask,
					hiddenDetermines, audit.primitiveRows, audit.invalidMaterials, audit.enabled);
			}
		}
		if (options.validate)
		{
			PerfAuditScope timer(PerfAuditMetric::StaticFilterValidationNs);
			uint64_t ignoredInvalid = 0;
			const bool reference = RawRequiresCandidate(scene, instance, span.count, options.enabledPolicyMask,
				hiddenDetermines, audit.referenceRows, ignoredInvalid, audit.enabled);
			++audit.checks;
			if (audit.enabled)
			{
				if (usedHiddenEarlyOut) PerfAuditAdd(PerfAuditMetric::StaticFilterVisibilityValidationChecks);
				if (usedCacheHit) PerfAuditAdd(PerfAuditMetric::StaticFilterCacheHitValidationChecks);
				if (usedCacheBuild) PerfAuditAdd(PerfAuditMetric::StaticFilterCacheBuildValidationChecks);
			}
			if (reference != requiresCandidate)
			{
				++audit.mismatches;
				if (!cache.IsQuarantined())
				{
					if (audit.enabled) PerfAuditAdd(PerfAuditMetric::StaticFilterQuarantines);
					Printf("NRI PT static filter reference mismatch: chunk=%u primitive_base=%u count=%u action=reference-quarantine\n",
						instance.metadata0, instance.primitiveBase, span.count);
				}
				cache.Quarantine();
				requiresCandidate = reference;
			}
		}
		if (requiresCandidate)
		{
			if (audit.enabled) ++audit.forced;
			instances[index].flags = (nri::TopLevelInstanceBits)((uint32_t)instances[index].flags |
				(uint32_t)nri::TopLevelInstanceBits::FORCE_NON_OPAQUE);
		}
	}
	if (audit.enabled)
	{
		PerfAuditSet(PerfAuditMetric::StaticFilterCapacityBytes, cache.CapacityBytes());
		PerfAuditSet(PerfAuditMetric::StaticFilterQuarantined, cache.IsQuarantined() ? 1u : 0u);
	}
}
