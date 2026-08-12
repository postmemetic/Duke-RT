#include "nri_voxel_compute_preload.h"

#include "nri_persistent_voxel_geometry_arena_policy.h"

#include "nri_cvars.h"
#include "nri_persistent_voxels.h"
#include "nri_voxel_compute_meshing.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace
{
	double DurationMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	bool IsRequiredComputePreloadVariant(const nri_scene::PrecachedVoxelVariantView& variant)
	{
		return
			variant.priority <= 0 &&
			(variant.sourceBits & nri_scene::PrecachedVoxelSourceBit_MountedVoxelPreload) != 0 &&
			(variant.gpuForce ||
				(variant.gpuPrefer && (variant.sourceBits & nri_scene::PrecachedVoxelSourceBit_MountedPreloadMap) != 0));
	}

	bool IsRequiredComputePreloadRawVariant(const nri_scene::PrecachedVoxelRawManifestView& variant)
	{
		return
			variant.priority <= 0 &&
			(variant.sourceBits & nri_scene::PrecachedVoxelSourceBit_MountedVoxelPreload) != 0 &&
			(variant.gpuForce ||
				(variant.gpuPrefer && (variant.sourceBits & nri_scene::PrecachedVoxelSourceBit_MountedPreloadMap) != 0));
	}

	uint64_t EstimateVariantGeometryBytes(const nri_scene::PrecachedVoxelVariantView& variant)
	{
		if (variant.surface != nullptr)
		{
			return
				(uint64_t)variant.surface->vertices.size() * (uint64_t)sizeof(nri_scene::SceneVertex) +
				(uint64_t)variant.surface->indices.size() * (uint64_t)sizeof(uint32_t) +
				(uint64_t)variant.primitiveCount * (uint64_t)sizeof(nri_scene::PrimitiveData);
		}

		return
			(uint64_t)variant.primitiveCount * 2ull * (uint64_t)sizeof(nri_scene::SceneVertex) +
			(uint64_t)variant.primitiveCount * 3ull * (uint64_t)sizeof(uint32_t) +
			(uint64_t)variant.primitiveCount * (uint64_t)sizeof(nri_scene::PrimitiveData);
	}

	uint64_t EstimateRawVariantGeometryBytes(const nri_scene::PrecachedVoxelRawManifestView& variant)
	{
		return
			(uint64_t)variant.primitiveCount * 2ull * (uint64_t)sizeof(nri_scene::SceneVertex) +
			(uint64_t)variant.primitiveCount * 3ull * (uint64_t)sizeof(uint32_t) +
			(uint64_t)variant.primitiveCount * (uint64_t)sizeof(nri_scene::PrimitiveData);
	}

	bool ShouldEmitPreloadTrace(const NRIVoxelComputePreloadSettings& settings)
	{
		return settings.traceLevel >= 1 || (int)nri_ptloadingtrace >= 1 || (int)nri_ptvoxelcomputetrace >= 1;
	}

	uint64_t HashValue(uint64_t hash, uint64_t value)
	{
		hash ^= value;
		hash *= 1099511628211ull;
		return hash;
	}

	nri_scene::PrecachedVoxelVariantView BuildDirectVariant(const nri_scene::PrecachedVoxelRawManifestView& rawVariant)
	{
		nri_scene::PrecachedVoxelVariantView variant = {};
		variant.meshKeyHash = rawVariant.meshKeyHash;
		variant.materialKeyHash = rawVariant.materialKeyHash;
		variant.geometrySignature = rawVariant.meshVariantHash != 0 ? rawVariant.meshVariantHash : rawVariant.meshKeyHash;
		variant.geometryContentHash = rawVariant.geometryContentHash;
		variant.renderPrimitiveHash = rawVariant.renderPrimitiveHash;
		variant.meshVariantHash = rawVariant.meshVariantHash;
		variant.materialVariantHash = rawVariant.materialVariantHash;
		variant.sourceBits = rawVariant.sourceBits;
		variant.priority = rawVariant.priority;
		variant.admissionRank = rawVariant.admissionRank;
		variant.sourcePicnum = rawVariant.sourcePicnum;
		variant.resolvedVoxelIndex = rawVariant.resolvedVoxelIndex;
		variant.primitiveCount = rawVariant.primitiveCount;
		variant.gpuForce = rawVariant.gpuForce;
		variant.gpuPrefer = rawVariant.gpuPrefer;
		variant.model = rawVariant.model;
		variant.surface = nullptr;
		variant.material = rawVariant.material;
		variant.materialSurface.material = rawVariant.material;
		variant.materialSurface.provenance.sourceType = nri_scene::SurfaceSourceType::VoxelProxySprite;
		variant.materialSurface.provenance.materialFlags = rawVariant.material.flags;
		variant.directOnlyAdmission = true;
		return variant;
	}

	enum class PlannedBindingDisposition : uint8_t
	{
		Admitted,
		Failed,
		CapSkipped,
	};

	struct PlannedBinding
	{
		uint64_t meshResourceKey = 0;
		uint64_t materialKey = 0;
		uint64_t sourceKey = 0;
		uint64_t textureKey = 0;
		bool runtimeWithheld = false;
		PlannedBindingDisposition disposition = PlannedBindingDisposition::Failed;
	};

	struct RawPlanResult
	{
		std::vector<nri_scene::PrecachedVoxelVariantView> directVariants;
		std::vector<PlannedBinding> bindings;
		uint32_t candidateBindings = 0;
		uint32_t admittedBindings = 0;
		uint32_t admittedRequired = 0;
		uint32_t admittedOptional = 0;
		uint32_t capSkippedBindings = 0;
		uint32_t failedBindings = 0;
		uint32_t runtimeWithheldBindings = 0;
		uint32_t balancedRequiredBindings = 0;
		uint32_t balancedImportanceBindings = 0;
		uint32_t balancedLargeBindings = 0;
		uint32_t balancedFilteredOptionalBindings = 0;
		uint32_t skippedSourceMissing = 0;
		uint32_t skippedMaterialMissing = 0;
		uint32_t skippedJobBudget = 0;
		uint32_t skippedByteBudget = 0;
		uint32_t skippedMaterialBudget = 0;
		uint64_t admittedPairGeometryBytes = 0;
		uint64_t uniqueGeometryBytes = 0;
		uint64_t largestUniqueGeometryBytes = 0;
		uint64_t uniqueSourceBytes = 0;
		uint64_t runtimeWithheldUniqueGeometryBytes = 0;
		uint64_t runtimeWithheldManifestHash = 1469598103934665603ull;
		uint64_t balancedRequiredGeometryBytes = 0;
		uint64_t balancedImportanceGeometryBytes = 0;
		uint64_t balancedLargeGeometryBytes = 0;
		uint64_t runtimeProbeDigest = 1469598103934665603ull;
		uint64_t manifestHash = 1469598103934665603ull;
		std::unordered_set<uint64_t> uniqueSources;
		std::unordered_set<uint64_t> uniqueMeshes;
		std::unordered_set<uint64_t> uniqueMaterials;
		std::unordered_set<uint64_t> uniqueTextures;
		std::unordered_set<uint64_t> runtimeWithheldMeshes;
		std::unordered_set<uint64_t> runtimeProbeMeshes;
	};

	RawPlanResult BuildRawPlan(
		const std::vector<nri_scene::PrecachedVoxelRawManifestView>& rawVariants,
		const NRIVoxelComputePreloadSettings& settings)
	{
		RawPlanResult result = {};
		result.directVariants.reserve(rawVariants.size());
		result.bindings.reserve(rawVariants.size());
		std::unordered_set<uint64_t> selectedMaterialRows;
		std::unordered_set<uint64_t> requiredMeshResources;
		std::unordered_set<uint64_t> optionalMeshResources;
		if (settings.runtimeWithholdModulo != 0 || settings.runtimeProbeModulo != 0)
		{
			for (const nri_scene::PrecachedVoxelRawManifestView& rawVariant : rawVariants)
			{
				if (!rawVariant.rawSourceResident || rawVariant.primitiveCount == 0 ||
					rawVariant.meshKeyHash == 0 || rawVariant.model == nullptr ||
					!rawVariant.materialContextReady || rawVariant.materialKeyHash == 0)
				{
					continue;
				}
				const uint64_t meshResourceKey = BuildPersistentVoxelVariantMeshResourceKey(BuildDirectVariant(rawVariant));
				if (IsRequiredComputePreloadRawVariant(rawVariant))
				{
					requiredMeshResources.insert(meshResourceKey);
				}
				else
				{
					optionalMeshResources.insert(meshResourceKey);
				}
			}
		}
		if (settings.runtimeProbeModulo != 0)
		{
			for (uint64_t meshResourceKey : optionalMeshResources)
			{
				if (requiredMeshResources.find(meshResourceKey) == requiredMeshResources.end() &&
					meshResourceKey % settings.runtimeProbeModulo == settings.runtimeProbeRemainder)
				{
					result.runtimeProbeMeshes.insert(meshResourceKey);
				}
			}
		}
		std::vector<uint64_t> sortedProbeMeshes(result.runtimeProbeMeshes.begin(), result.runtimeProbeMeshes.end());
		std::sort(sortedProbeMeshes.begin(), sortedProbeMeshes.end());
		for (uint64_t meshResourceKey : sortedProbeMeshes)
		{
			result.runtimeProbeDigest = HashValue(result.runtimeProbeDigest, meshResourceKey);
		}

		uint64_t selectedPairBytes = 0;
		for (const nri_scene::PrecachedVoxelRawManifestView& rawVariant : rawVariants)
		{
			const bool required = IsRequiredComputePreloadRawVariant(rawVariant);
			if ((required && !settings.includeRequired) || (!required && !settings.includeOptional))
			{
				continue;
			}

			nri_scene::PrecachedVoxelVariantView directVariant = BuildDirectVariant(rawVariant);
			PlannedBinding binding = {};
			binding.meshResourceKey = BuildPersistentVoxelVariantMeshResourceKey(directVariant);
			binding.materialKey = rawVariant.materialKeyHash;
			binding.sourceKey = rawVariant.resolvedVoxelIndex >= 0 ?
				(uint64_t)(uint32_t)rawVariant.resolvedVoxelIndex + 1ull :
				(rawVariant.geometryContentHash != 0 ? rawVariant.geometryContentHash : rawVariant.meshKeyHash);
			binding.textureKey = rawVariant.sourcePicnum >= 0 ? (uint64_t)(uint32_t)rawVariant.sourcePicnum + 1ull : rawVariant.materialKeyHash;
			const bool runtimeProbe =
				!required &&
				result.runtimeProbeMeshes.find(binding.meshResourceKey) != result.runtimeProbeMeshes.end();
			const bool selectedByImportance = !required && rawVariant.priority <= settings.balancedMaxPriority;
			const bool selectedByLarge = !required && rawVariant.primitiveCount >= settings.balancedMinPrimitives;
			if (!required && settings.balancedOptional &&
				(runtimeProbe || (!selectedByImportance && !selectedByLarge)))
			{
				result.balancedFilteredOptionalBindings++;
				continue;
			}
			result.candidateBindings++;
			result.manifestHash = HashValue(result.manifestHash, binding.meshResourceKey);
			result.manifestHash = HashValue(result.manifestHash, binding.materialKey);
			result.manifestHash = HashValue(result.manifestHash, rawVariant.geometryContentHash);
			result.manifestHash = HashValue(result.manifestHash, (uint32_t)rawVariant.sourcePicnum);
			result.manifestHash = HashValue(result.manifestHash, (uint32_t)rawVariant.resolvedVoxelIndex);

			if (!rawVariant.rawSourceResident || rawVariant.primitiveCount == 0 || rawVariant.meshKeyHash == 0 || rawVariant.model == nullptr)
			{
				result.failedBindings++;
				result.skippedSourceMissing++;
				binding.disposition = PlannedBindingDisposition::Failed;
				result.bindings.push_back(binding);
				continue;
			}
			if (!rawVariant.materialContextReady || rawVariant.materialKeyHash == 0)
			{
				result.failedBindings++;
				result.skippedMaterialMissing++;
				binding.disposition = PlannedBindingDisposition::Failed;
				result.bindings.push_back(binding);
				continue;
			}

			const uint64_t estimatedBytes = EstimateRawVariantGeometryBytes(rawVariant);
			bool capSkipped = false;
			if (settings.maxJobs != 0 && result.admittedBindings >= settings.maxJobs)
			{
				result.skippedJobBudget++;
				capSkipped = true;
			}
			else if (settings.maxBytes != 0 && selectedPairBytes + estimatedBytes > settings.maxBytes)
			{
				result.skippedByteBudget++;
				capSkipped = true;
			}
			else if (settings.preloadMaterials &&
				selectedMaterialRows.find(rawVariant.materialKeyHash) == selectedMaterialRows.end() &&
				settings.maxMaterialRows != 0 &&
				selectedMaterialRows.size() >= settings.maxMaterialRows)
			{
				result.skippedMaterialBudget++;
				capSkipped = true;
			}
			if (capSkipped)
			{
				result.capSkippedBindings++;
				binding.disposition = PlannedBindingDisposition::CapSkipped;
				result.bindings.push_back(binding);
				continue;
			}

			const bool runtimeWithheld =
				!required &&
				settings.runtimeWithholdModulo != 0 &&
				(!settings.includeRequired || requiredMeshResources.find(binding.meshResourceKey) == requiredMeshResources.end()) &&
				binding.meshResourceKey % settings.runtimeWithholdModulo == settings.runtimeWithholdRemainder;
			directVariant.preloadGeometry = !runtimeWithheld;
			binding.runtimeWithheld = runtimeWithheld;

			binding.disposition = PlannedBindingDisposition::Admitted;
			result.bindings.push_back(binding);
			result.directVariants.push_back(std::move(directVariant));
			result.admittedBindings++;
			result.admittedRequired += required ? 1u : 0u;
			result.admittedOptional += required ? 0u : 1u;
			result.runtimeWithheldBindings += runtimeWithheld ? 1u : 0u;
			selectedPairBytes += estimatedBytes;
			result.admittedPairGeometryBytes += estimatedBytes;
			if (required)
			{
				result.balancedRequiredBindings++;
				result.balancedRequiredGeometryBytes += estimatedBytes;
			}
			else if (settings.balancedOptional && selectedByImportance)
			{
				result.balancedImportanceBindings++;
				result.balancedImportanceGeometryBytes += estimatedBytes;
			}
			else if (settings.balancedOptional)
			{
				result.balancedLargeBindings++;
				result.balancedLargeGeometryBytes += estimatedBytes;
			}
			selectedMaterialRows.insert(rawVariant.materialKeyHash);
			if (result.uniqueMeshes.insert(binding.meshResourceKey).second)
			{
				result.largestUniqueGeometryBytes =
					std::max(result.largestUniqueGeometryBytes, estimatedBytes);
				if (!runtimeWithheld)
				{
					result.uniqueGeometryBytes += estimatedBytes;
				}
			}
			if (runtimeWithheld && result.runtimeWithheldMeshes.insert(binding.meshResourceKey).second)
			{
				result.runtimeWithheldUniqueGeometryBytes += estimatedBytes;
			}
			if (result.uniqueSources.insert(binding.sourceKey).second)
			{
				result.uniqueSourceBytes += rawVariant.rawBytes;
			}
			result.uniqueMaterials.insert(binding.materialKey);
			result.uniqueTextures.insert(binding.textureKey);
		}
		std::vector<uint64_t> sortedWithheldMeshes(result.runtimeWithheldMeshes.begin(), result.runtimeWithheldMeshes.end());
		std::sort(sortedWithheldMeshes.begin(), sortedWithheldMeshes.end());
		for (uint64_t meshResourceKey : sortedWithheldMeshes)
		{
			result.runtimeWithheldManifestHash = HashValue(result.runtimeWithheldManifestHash, meshResourceKey);
		}
		return result;
	}

	struct PreloadPlanState
	{
		uint64_t buildSerial = 0;
		uint64_t closureSequence = 0;
		NRIVoxelComputePreloadSettings settings = {};
		NRIVoxelComputePreloadStats stats = {};
		std::vector<PlannedBinding> bindings;
		std::unordered_set<uint64_t> runtimeWithheldMeshes;
		std::unordered_set<uint64_t> runtimeProbeMeshes;
		bool runtimeTailReleased = false;
	};

	PreloadPlanState gPreloadPlanState;
}

NRIVoxelComputePreloadSettings BuildNRIVoxelComputePreloadSettingsFromCVars()
{
	NRIVoxelComputePreloadSettings settings = {};
	settings.enabled = (bool)nri_ptvoxelcomputepreload;
	settings.dryRun = (bool)nri_ptvoxelcomputepreloaddryrun;
	settings.traceLevel = std::max(0, (int)nri_ptvoxelcomputepreloadtrace);
	settings.includeRequired = (bool)nri_ptvoxelcomputepreloadrequired;
	settings.includeOptional = (bool)nri_ptvoxelcomputepreloadoptional;
	settings.preloadMaterials = (bool)nri_ptvoxelcomputepreloadmaterials;
	settings.strict = (bool)nri_ptvoxelcomputepreloadstrict;
	settings.balancedOptional = (bool)nri_ptvoxelcomputepreloadbalancedoptional;
	settings.balancedMaxPriority = (int)nri_ptvoxelcomputepreloadbalancedmaxpriority;
	settings.balancedMinPrimitives = (uint32_t)std::max(0, (int)nri_ptvoxelcomputepreloadbalancedminprimitives);
	settings.runtimeProbeModulo = (uint32_t)std::max(0, (int)nri_ptvoxelcomputepreloadruntimeprobemod);
	settings.runtimeProbeRemainder = settings.runtimeProbeModulo != 0 ?
		(uint32_t)std::max(0, (int)nri_ptvoxelcomputepreloadruntimeproberem) % settings.runtimeProbeModulo : 0u;
	settings.runtimeWithholdModulo = (uint32_t)std::max(0, (int)nri_ptvoxelcomputepreloadruntimewithholdmod);
	settings.runtimeWithholdRemainder = settings.runtimeWithholdModulo != 0 ?
		(uint32_t)std::max(0, (int)nri_ptvoxelcomputepreloadruntimewithholdrem) % settings.runtimeWithholdModulo : 0u;
	settings.maxMilliseconds = std::max(0, (int)nri_ptvoxelcomputepreloadmaxms);
	settings.maxJobs = std::max(0, (int)nri_ptvoxelcomputepreloadmaxjobs);
	settings.maxBlasBuilds = std::max(0, (int)nri_ptvoxelcomputepreloadmaxblas);
	settings.maxBytes = (int)nri_ptvoxelcomputepreloadmaxbytes <= 0 ? 0ull : (uint64_t)(int)nri_ptvoxelcomputepreloadmaxbytes;
	settings.maxMaterialRows = std::max(0, (int)nri_ptvoxelcomputepreloadmaxmaterialrows);
	settings.watchdogMilliseconds = std::max(0, (int)nri_ptvoxelcomputepreloadwatchdogms);
	settings.peakEstimatePercent = (uint32_t)std::max(100, (int)nri_ptvoxelcomputepreloadpeakpercent);
	settings.minimumLocalMemoryReserveBytes = (uint64_t)std::max(0, (int)nri_ptvoxelcomputepreloadminreservemb) * 1024ull * 1024ull;
	return settings;
}

void BuildNRIVoxelComputePreloadDirectVariants(
	const std::vector<nri_scene::PrecachedVoxelRawManifestView>& rawVariants,
	const NRIVoxelComputePreloadSettings& settings,
	std::vector<nri_scene::PrecachedVoxelVariantView>& outVariants)
{
	outVariants.clear();
	if (!settings.enabled || settings.dryRun)
	{
		return;
	}

	RawPlanResult result = BuildRawPlan(rawVariants, settings);
	outVariants = std::move(result.directVariants);
}

NRIVoxelComputePreloadStats PlanNRIVoxelComputePreload(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	const std::vector<nri_scene::PrecachedVoxelRawManifestView>& rawVariants,
	const nri_scene::PrecachedVoxelRawManifestStats& rawManifestStats,
	const NRIPersistentVoxelResidency& residency,
	const NRIVoxelComputePreloadSettings& settings,
	const char* levelName,
	uint64_t buildSerial,
	uint32_t frameIndex,
	const char* timelineStage,
	uint64_t currentTrackedBytes,
	uint64_t localMemoryBudgetBytes)
{
	const auto start = std::chrono::steady_clock::now();
	NRIVoxelComputePreloadStats stats = {};
	stats.enabled = settings.enabled;
	stats.dryRun = settings.dryRun;
	stats.variants = (uint32_t)variants.size();
	stats.rawVariants = (uint32_t)rawVariants.size();
	stats.manifestSources = rawManifestStats.manifestSources;
	stats.manifestLines = rawManifestStats.manifestLines;
	stats.manifestRequests = rawManifestStats.manifestRequests;
	stats.manifestSkippedInactive = rawManifestStats.manifestSkippedInactive;
	stats.manifestSkippedSyntax = rawManifestStats.manifestSkippedSyntax;
	stats.manifestSkippedActor = rawManifestStats.manifestSkippedActor;
	stats.manifestSkippedUnsupported = rawManifestStats.manifestSkippedUnsupported;
	stats.manifestDiscovered = rawManifestStats.discovered;
	stats.manifestUniqueRequests = rawManifestStats.uniqueRequests;
	stats.manifestSkippedInvalid = rawManifestStats.skippedInvalid;
	stats.manifestSkippedDuplicate = rawManifestStats.skippedDuplicate;

	std::unordered_set<uint64_t> uniqueMeshes;
	std::unordered_set<uint64_t> uniqueMaterials;
	std::unordered_set<uint64_t> selectedMaterials;
	std::unordered_set<uint64_t> rawUniqueMeshes;
	std::unordered_set<uint64_t> rawUniqueMaterials;
	std::unordered_set<uint64_t> rawRequiredMaterials;
	std::unordered_set<uint64_t> rawOptionalMaterials;
	std::unordered_set<uint64_t> rawSelectedMaterials;
	std::unordered_set<uint64_t> rawActorScopedMaterials;
	uniqueMeshes.reserve(variants.size());
	uniqueMaterials.reserve(variants.size());
	selectedMaterials.reserve(variants.size());
	rawUniqueMeshes.reserve(rawVariants.size());
	rawUniqueMaterials.reserve(rawVariants.size());
	rawRequiredMaterials.reserve(rawVariants.size());
	rawOptionalMaterials.reserve(rawVariants.size());
	rawSelectedMaterials.reserve(rawVariants.size());
	rawActorScopedMaterials.reserve(rawVariants.size());

	for (const nri_scene::PrecachedVoxelVariantView& variant : variants)
	{
		const bool required = IsRequiredComputePreloadVariant(variant);
		if (required)
		{
			stats.required++;
		}
		else
		{
			stats.optional++;
		}

		if (variant.meshKeyHash != 0)
		{
			uniqueMeshes.insert(variant.meshKeyHash);
		}
		if (variant.materialKeyHash != 0)
		{
			uniqueMaterials.insert(variant.materialKeyHash);
		}
		if (variant.directOnlyAdmission)
		{
			stats.directOnly++;
		}
		if (variant.surface != nullptr)
		{
			stats.surfaceReady++;
		}
		if (variant.model != nullptr)
		{
			stats.sourceReady++;
		}
		if (variant.materialKeyHash != 0 && variant.materialVariantHash != 0)
		{
			stats.materialContextReady++;
		}

		const uint64_t meshResourceKey = BuildPersistentVoxelVariantMeshResourceKey(variant);
		const PersistentVoxelReadinessStatus readiness = residency.GetSharedVariantReadiness(meshResourceKey, variant.materialKeyHash);
		if (readiness.meshPresent)
		{
			stats.meshResident++;
		}
		if (readiness.materialPresent)
		{
			stats.materialResident++;
		}
		if (readiness.blasReady)
		{
			stats.blasReady++;
		}
		if (readiness.ready)
		{
			stats.ready++;
		}
		else
		{
			stats.notReady++;
		}

		const uint64_t estimatedBytes = EstimateVariantGeometryBytes(variant);
		stats.estimatedGeometryBytes += estimatedBytes;
		if (!settings.enabled)
		{
			stats.skippedDisabled++;
			continue;
		}
		if (required && !settings.includeRequired)
		{
			stats.skippedRequiredOff++;
			continue;
		}
		if (!required && !settings.includeOptional)
		{
			stats.skippedOptionalOff++;
			continue;
		}
		if (settings.maxJobs != 0 && stats.selected >= settings.maxJobs)
		{
			stats.skippedJobBudget++;
			continue;
		}
		if (settings.maxBytes != 0 && stats.selectedGeometryBytes + estimatedBytes > settings.maxBytes)
		{
			stats.skippedByteBudget++;
			continue;
		}
		if (settings.preloadMaterials &&
			variant.materialKeyHash != 0 &&
			selectedMaterials.find(variant.materialKeyHash) == selectedMaterials.end() &&
			settings.maxMaterialRows != 0 &&
			stats.materialRowsPlanned + 1u > settings.maxMaterialRows)
		{
			stats.skippedMaterialBudget++;
			continue;
		}

		stats.selected++;
		stats.selectedGeometryBytes += estimatedBytes;
		if (required)
		{
			stats.selectedRequired++;
		}
		else
		{
			stats.selectedOptional++;
		}
		if (settings.preloadMaterials && variant.materialKeyHash != 0 && selectedMaterials.insert(variant.materialKeyHash).second)
		{
			stats.materialRowsPlanned++;
		}
	}

	for (const nri_scene::PrecachedVoxelRawManifestView& variant : rawVariants)
	{
		const bool required = IsRequiredComputePreloadRawVariant(variant);
		if (required)
		{
			stats.rawRequired++;
		}
		else
		{
			stats.rawOptional++;
		}

		if (variant.meshKeyHash != 0)
		{
			rawUniqueMeshes.insert(variant.meshKeyHash);
		}
		if (variant.materialKeyHash != 0)
		{
			rawUniqueMaterials.insert(variant.materialKeyHash);
			if (required)
			{
				rawRequiredMaterials.insert(variant.materialKeyHash);
			}
			else
			{
				rawOptionalMaterials.insert(variant.materialKeyHash);
			}
			if (variant.materialVariantHash != 0 && variant.materialVariantHash != variant.materialKeyHash)
			{
				rawActorScopedMaterials.insert(variant.materialKeyHash);
			}
		}
		if (variant.material.texture != nullptr)
		{
			stats.rawMaterialTextureRefs++;
		}
		if (variant.rawSourceResident)
		{
			stats.rawSourceResident++;
		}
		else
		{
			stats.rawSourceMissing++;
		}
		if (variant.materialContextReady)
		{
			stats.rawMaterialContextReady++;
		}
		else
		{
			stats.rawMaterialContextMissing++;
		}
		if (variant.cpuSurfaceReady)
		{
			stats.rawCpuSurfaceReady++;
		}
		if (variant.legacyGpuCandidate)
		{
			stats.rawLegacyGpuCandidate++;
		}
		else
		{
			stats.rawLegacyGpuSourceSkipped++;
		}

		const uint64_t estimatedBytes = EstimateRawVariantGeometryBytes(variant);
		stats.rawEstimatedGeometryBytes += estimatedBytes;
		if (!settings.enabled)
		{
			stats.rawSkippedDisabled++;
			continue;
		}
		if (required && !settings.includeRequired)
		{
			stats.rawSkippedRequiredOff++;
			continue;
		}
		if (!required && !settings.includeOptional)
		{
			stats.rawSkippedOptionalOff++;
			continue;
		}
		if (!variant.rawSourceResident || variant.primitiveCount == 0)
		{
			stats.rawSkippedSourceMissing++;
			continue;
		}
		if (!variant.materialContextReady)
		{
			stats.rawSkippedMaterialMissing++;
			continue;
		}
		if (settings.maxJobs != 0 && stats.rawSelected >= settings.maxJobs)
		{
			stats.rawSkippedJobBudget++;
			continue;
		}
		if (settings.maxBytes != 0 && stats.rawSelectedGeometryBytes + estimatedBytes > settings.maxBytes)
		{
			stats.rawSkippedByteBudget++;
			continue;
		}

		stats.rawSelected++;
		stats.rawSelectedGeometryBytes += estimatedBytes;
		if (variant.materialKeyHash != 0)
		{
			rawSelectedMaterials.insert(variant.materialKeyHash);
		}
		if (required)
		{
			stats.rawSelectedRequired++;
		}
		else
		{
			stats.rawSelectedOptional++;
		}
	}

	RawPlanResult rawPlan = BuildRawPlan(rawVariants, settings);
	stats.rawSelected = rawPlan.admittedBindings;
	stats.rawSelectedRequired = rawPlan.admittedRequired;
	stats.rawSelectedOptional = rawPlan.admittedOptional;
	stats.rawSelectedGeometryBytes = rawPlan.admittedPairGeometryBytes;
	stats.rawSkippedSourceMissing = rawPlan.skippedSourceMissing;
	stats.rawSkippedMaterialMissing = rawPlan.skippedMaterialMissing;
	stats.rawSkippedJobBudget = rawPlan.skippedJobBudget;
	stats.rawSkippedByteBudget = rawPlan.skippedByteBudget;
	stats.rawCandidateBindings = rawPlan.candidateBindings;
	stats.rawCapSkippedBindings = rawPlan.capSkippedBindings;
	stats.rawFailedBindings = rawPlan.failedBindings;
	stats.rawRuntimeWithheldBindings = rawPlan.runtimeWithheldBindings;
	stats.rawRuntimeWithheldUniqueMeshes = (uint32_t)rawPlan.runtimeWithheldMeshes.size();
	stats.rawRuntimeWithheldUniqueGeometryBytes = rawPlan.runtimeWithheldUniqueGeometryBytes;
	stats.rawRuntimeWithheldManifestHash = rawPlan.runtimeWithheldManifestHash;
	stats.rawBalancedRequiredBindings = rawPlan.balancedRequiredBindings;
	stats.rawBalancedImportanceBindings = rawPlan.balancedImportanceBindings;
	stats.rawBalancedLargeBindings = rawPlan.balancedLargeBindings;
	stats.rawBalancedFilteredOptionalBindings = rawPlan.balancedFilteredOptionalBindings;
	stats.rawBalancedRequiredGeometryBytes = rawPlan.balancedRequiredGeometryBytes;
	stats.rawBalancedImportanceGeometryBytes = rawPlan.balancedImportanceGeometryBytes;
	stats.rawBalancedLargeGeometryBytes = rawPlan.balancedLargeGeometryBytes;
	stats.rawRuntimeProbeUniqueMeshes = (uint32_t)rawPlan.runtimeProbeMeshes.size();
	stats.rawRuntimeProbeDigest = rawPlan.runtimeProbeDigest;
	stats.rawSelectedUniqueSources = (uint32_t)rawPlan.uniqueSources.size();
	stats.rawSelectedUniqueMeshes = (uint32_t)rawPlan.uniqueMeshes.size();
	stats.rawSelectedUniqueMaterials = (uint32_t)rawPlan.uniqueMaterials.size();
	stats.rawSelectedUniqueTextures = (uint32_t)rawPlan.uniqueTextures.size();
	stats.rawSelectedUniqueGeometryBytes = rawPlan.uniqueGeometryBytes;
	stats.rawSelectedLargestUniqueGeometryBytes = rawPlan.largestUniqueGeometryBytes;
	stats.rawSelectedUniqueSourceBytes = rawPlan.uniqueSourceBytes;
	stats.manifestHash = rawPlan.manifestHash;
	stats.currentTrackedBytes = currentTrackedBytes;
	stats.localMemoryBudgetBytes = localMemoryBudgetBytes;
	stats.minimumLocalMemoryReserveBytes = settings.minimumLocalMemoryReserveBytes;
	stats.estimatedPeakAdditionalBytes =
		(rawPlan.uniqueGeometryBytes * (uint64_t)settings.peakEstimatePercent + 99ull) / 100ull +
		rawPlan.uniqueSourceBytes;
	if (settings.strict)
	{
		const NRIPersistentVoxelGeometryArenaPlan arenaPlan =
			BuildNRIPersistentVoxelGeometryArenaPlan(
				rawPlan.uniqueGeometryBytes,
				rawPlan.runtimeWithheldUniqueGeometryBytes,
				rawPlan.largestUniqueGeometryBytes);
		if (arenaPlan.overflow || arenaPlan.targetGeometryBytes >
			std::numeric_limits<uint64_t>::max() - rawPlan.uniqueSourceBytes)
		{
			stats.estimatedPeakAdditionalBytes = std::numeric_limits<uint64_t>::max();
		}
		else
		{
			stats.estimatedPeakAdditionalBytes = std::max(
				stats.estimatedPeakAdditionalBytes,
				arenaPlan.targetGeometryBytes + rawPlan.uniqueSourceBytes);
		}
	}
	stats.estimatedPeakTotalBytes =
		stats.estimatedPeakAdditionalBytes > std::numeric_limits<uint64_t>::max() - currentTrackedBytes ?
		std::numeric_limits<uint64_t>::max() :
		currentTrackedBytes + stats.estimatedPeakAdditionalBytes;
	stats.memoryGuardAvailable = localMemoryBudgetBytes != 0;
	stats.memoryGuardHit =
		stats.memoryGuardAvailable &&
		(stats.estimatedPeakTotalBytes > localMemoryBudgetBytes ||
		 settings.minimumLocalMemoryReserveBytes > localMemoryBudgetBytes - std::min(localMemoryBudgetBytes, stats.estimatedPeakTotalBytes));

	stats.uniqueMeshes = (uint32_t)uniqueMeshes.size();
	stats.uniqueMaterials = (uint32_t)uniqueMaterials.size();
	stats.rawUniqueMeshes = (uint32_t)rawUniqueMeshes.size();
	stats.rawUniqueMaterials = (uint32_t)rawUniqueMaterials.size();
	stats.rawMaterialRequiredKeys = (uint32_t)rawRequiredMaterials.size();
	stats.rawMaterialOptionalKeys = (uint32_t)rawOptionalMaterials.size();
	stats.rawMaterialSelectedKeys = rawPlan.admittedBindings != 0 ? (uint32_t)rawPlan.uniqueMaterials.size() : (uint32_t)rawSelectedMaterials.size();
	stats.rawMaterialActorScopedKeys = (uint32_t)rawActorScopedMaterials.size();
	const NRIPersistentVoxelMemoryUsage memoryUsage = residency.GetMemoryUsage();
	stats.residentSceneBytes = memoryUsage.sceneBufferBytes;
	stats.residentAsBytes = memoryUsage.accelerationStructureBytes;
	stats.actionReady = settings.enabled && (settings.dryRun || !stats.memoryGuardHit);
	stats.planMs = DurationMs(start, std::chrono::steady_clock::now());
	if (buildSerial != 0 &&
		(gPreloadPlanState.buildSerial != buildSerial || rawPlan.candidateBindings >= gPreloadPlanState.stats.rawCandidateBindings))
	{
		const uint64_t previousSequence = gPreloadPlanState.buildSerial == buildSerial ? gPreloadPlanState.closureSequence : 0;
		gPreloadPlanState = {};
		gPreloadPlanState.buildSerial = buildSerial;
		gPreloadPlanState.closureSequence = previousSequence;
		gPreloadPlanState.settings = settings;
		gPreloadPlanState.stats = stats;
		gPreloadPlanState.bindings = std::move(rawPlan.bindings);
		gPreloadPlanState.runtimeWithheldMeshes = std::move(rawPlan.runtimeWithheldMeshes);
		gPreloadPlanState.runtimeProbeMeshes = std::move(rawPlan.runtimeProbeMeshes);
	}

	if (ShouldEmitPreloadTrace(settings))
	{
		stats.emitted = true;
		Printf("PERF pt voxel preload accounting NRI: level=%s build_serial=%llu frame=%u manifest_hash=0x%llx strict=%u candidate_bindings=%u admitted_bindings=%u cap_skipped_bindings=%u failed_bindings=%u unique_sources=%u unique_meshes=%u unique_materials=%u unique_textures=%u logical_pair_geometry_bytes=%llu unique_geometry_bytes=%llu largest_unique_geometry_bytes=%llu unique_source_bytes=%llu current_tracked_bytes=%llu local_budget_bytes=%llu minimum_reserve_bytes=%llu estimated_peak_additional_bytes=%llu estimated_peak_total_bytes=%llu memory_guard_available=%u memory_guard_hit=%u peak_percent=%u\n",
			levelName != nullptr ? levelName : "unknown",
			(unsigned long long)buildSerial,
			frameIndex,
			(unsigned long long)stats.manifestHash,
			settings.strict ? 1u : 0u,
			stats.rawCandidateBindings,
			stats.rawSelected,
			stats.rawCapSkippedBindings,
			stats.rawFailedBindings,
			stats.rawSelectedUniqueSources,
			stats.rawSelectedUniqueMeshes,
			stats.rawSelectedUniqueMaterials,
			stats.rawSelectedUniqueTextures,
			(unsigned long long)stats.rawSelectedGeometryBytes,
			(unsigned long long)stats.rawSelectedUniqueGeometryBytes,
			(unsigned long long)stats.rawSelectedLargestUniqueGeometryBytes,
			(unsigned long long)stats.rawSelectedUniqueSourceBytes,
			(unsigned long long)stats.currentTrackedBytes,
			(unsigned long long)stats.localMemoryBudgetBytes,
			(unsigned long long)stats.minimumLocalMemoryReserveBytes,
			(unsigned long long)stats.estimatedPeakAdditionalBytes,
			(unsigned long long)stats.estimatedPeakTotalBytes,
			stats.memoryGuardAvailable ? 1u : 0u,
			stats.memoryGuardHit ? 1u : 0u,
			settings.peakEstimatePercent);
		Printf("PERF pt voxel preload runtime tail NRI: level=%s build_serial=%llu frame=%u enabled=%u modulo=%u remainder=%u withheld_bindings=%u withheld_unique_meshes=%u withheld_unique_geometry_bytes=%llu withheld_manifest_hash=0x%llx admitted_bindings=%u unique_materials=%u unique_textures=%u\n",
			levelName != nullptr ? levelName : "unknown",
			(unsigned long long)buildSerial,
			frameIndex,
			settings.runtimeWithholdModulo != 0 ? 1u : 0u,
			settings.runtimeWithholdModulo,
			settings.runtimeWithholdRemainder,
			stats.rawRuntimeWithheldBindings,
			stats.rawRuntimeWithheldUniqueMeshes,
			(unsigned long long)stats.rawRuntimeWithheldUniqueGeometryBytes,
			(unsigned long long)stats.rawRuntimeWithheldManifestHash,
			stats.rawSelected,
			stats.rawSelectedUniqueMaterials,
			stats.rawSelectedUniqueTextures);
		Printf("PERF pt voxel preload selection NRI: level=%s build_serial=%llu frame=%u balanced=%u max_priority=%d min_primitives=%u required_bindings=%u required_pair_bytes=%llu importance_bindings=%u importance_pair_bytes=%llu large_bindings=%u large_pair_bytes=%llu filtered_optional_bindings=%u probe_enabled=%u probe_modulo=%u probe_remainder=%u probe_meshes=%u probe_digest=0x%llx\n",
			levelName != nullptr ? levelName : "unknown",
			(unsigned long long)buildSerial,
			frameIndex,
			settings.balancedOptional ? 1u : 0u,
			settings.balancedMaxPriority,
			settings.balancedMinPrimitives,
			stats.rawBalancedRequiredBindings,
			(unsigned long long)stats.rawBalancedRequiredGeometryBytes,
			stats.rawBalancedImportanceBindings,
			(unsigned long long)stats.rawBalancedImportanceGeometryBytes,
			stats.rawBalancedLargeBindings,
			(unsigned long long)stats.rawBalancedLargeGeometryBytes,
			stats.rawBalancedFilteredOptionalBindings,
			settings.runtimeProbeModulo != 0 ? 1u : 0u,
			settings.runtimeProbeModulo,
			settings.runtimeProbeRemainder,
			stats.rawRuntimeProbeUniqueMeshes,
			(unsigned long long)stats.rawRuntimeProbeDigest);
		Printf("NRI PT voxel compute preload: event=plan stage=%s level=%s build_serial=%llu frame=%u enabled=%u dry_run=%u action=%s variants=%u required=%u optional=%u selected=%u selected_required=%u selected_optional=%u unique_meshes=%u unique_materials=%u surface_ready=%u direct_only=%u source_ready=%u material_context=%u mesh_resident=%u material_resident=%u blas_ready=%u ready=%u not_ready=%u material_rows_planned=%u estimated_bytes=%llu selected_bytes=%llu resident_scene_bytes=%llu resident_as_bytes=%llu skipped_disabled=%u skipped_required_off=%u skipped_optional_off=%u skipped_byte_budget=%u skipped_job_budget=%u skipped_material_budget=%u raw_variants=%u raw_required=%u raw_optional=%u raw_selected=%u raw_selected_required=%u raw_selected_optional=%u raw_unique_meshes=%u raw_unique_materials=%u raw_material_required_keys=%u raw_material_optional_keys=%u raw_material_selected_keys=%u raw_material_actor_scoped_keys=%u raw_material_texture_refs=%u raw_source_resident=%u raw_source_missing=%u raw_material_context=%u raw_material_missing=%u raw_cpu_surface_ready=%u raw_legacy_gpu_candidate=%u raw_legacy_gpu_source_skipped=%u raw_estimated_bytes=%llu raw_selected_bytes=%llu raw_skipped_disabled=%u raw_skipped_required_off=%u raw_skipped_optional_off=%u raw_skipped_source_missing=%u raw_skipped_material_missing=%u raw_skipped_byte_budget=%u raw_skipped_job_budget=%u manifest_sources=%u manifest_lines=%u manifest_requests=%u manifest_discovered=%u manifest_unique=%u manifest_skipped_inactive=%u manifest_skipped_syntax=%u manifest_skipped_actor=%u manifest_skipped_unsupported=%u manifest_skipped_invalid=%u manifest_skipped_duplicate=%u max_ms=%u max_jobs=%u max_blas=%u max_bytes=%llu max_material_rows=%u ms=%.3f\n",
			timelineStage != nullptr ? timelineStage : "snapshot",
			levelName != nullptr ? levelName : "unknown",
			(unsigned long long)buildSerial,
			frameIndex,
			settings.enabled ? 1u : 0u,
			settings.dryRun ? 1u : 0u,
			stats.actionReady ? "dry-run" : (settings.enabled ? "future-work" : "disabled"),
			stats.variants,
			stats.required,
			stats.optional,
			stats.selected,
			stats.selectedRequired,
			stats.selectedOptional,
			stats.uniqueMeshes,
			stats.uniqueMaterials,
			stats.surfaceReady,
			stats.directOnly,
			stats.sourceReady,
			stats.materialContextReady,
			stats.meshResident,
			stats.materialResident,
			stats.blasReady,
			stats.ready,
			stats.notReady,
			stats.materialRowsPlanned,
			(unsigned long long)stats.estimatedGeometryBytes,
			(unsigned long long)stats.selectedGeometryBytes,
			(unsigned long long)stats.residentSceneBytes,
			(unsigned long long)stats.residentAsBytes,
			stats.skippedDisabled,
			stats.skippedRequiredOff,
			stats.skippedOptionalOff,
			stats.skippedByteBudget,
			stats.skippedJobBudget,
			stats.skippedMaterialBudget,
			stats.rawVariants,
			stats.rawRequired,
			stats.rawOptional,
			stats.rawSelected,
			stats.rawSelectedRequired,
			stats.rawSelectedOptional,
			stats.rawUniqueMeshes,
			stats.rawUniqueMaterials,
			stats.rawMaterialRequiredKeys,
			stats.rawMaterialOptionalKeys,
			stats.rawMaterialSelectedKeys,
			stats.rawMaterialActorScopedKeys,
			stats.rawMaterialTextureRefs,
			stats.rawSourceResident,
			stats.rawSourceMissing,
			stats.rawMaterialContextReady,
			stats.rawMaterialContextMissing,
			stats.rawCpuSurfaceReady,
			stats.rawLegacyGpuCandidate,
			stats.rawLegacyGpuSourceSkipped,
			(unsigned long long)stats.rawEstimatedGeometryBytes,
			(unsigned long long)stats.rawSelectedGeometryBytes,
			stats.rawSkippedDisabled,
			stats.rawSkippedRequiredOff,
			stats.rawSkippedOptionalOff,
			stats.rawSkippedSourceMissing,
			stats.rawSkippedMaterialMissing,
			stats.rawSkippedByteBudget,
			stats.rawSkippedJobBudget,
			stats.manifestSources,
			stats.manifestLines,
			stats.manifestRequests,
			stats.manifestDiscovered,
			stats.manifestUniqueRequests,
			stats.manifestSkippedInactive,
			stats.manifestSkippedSyntax,
			stats.manifestSkippedActor,
			stats.manifestSkippedUnsupported,
			stats.manifestSkippedInvalid,
			stats.manifestSkippedDuplicate,
			settings.maxMilliseconds,
			settings.maxJobs,
			settings.maxBlasBuilds,
			(unsigned long long)settings.maxBytes,
			settings.maxMaterialRows,
			stats.planMs);
		Printf("PERF pt voxel preload summary NRI: frame=%u enabled=%u dry_run=%u variants=%u required=%u optional=%u selected=%u selected_required=%u selected_optional=%u unique_meshes=%u unique_materials=%u surface_ready=%u direct_only=%u source_ready=%u material_context=%u mesh_resident=%u material_resident=%u blas_ready=%u ready=%u not_ready=%u material_rows_planned=%u estimated_bytes=%llu selected_bytes=%llu resident_scene_bytes=%llu resident_as_bytes=%llu skipped_disabled=%u skipped_required_off=%u skipped_optional_off=%u skipped_byte_budget=%u skipped_job_budget=%u skipped_material_budget=%u raw_variants=%u raw_required=%u raw_optional=%u raw_selected=%u raw_selected_required=%u raw_selected_optional=%u raw_unique_meshes=%u raw_unique_materials=%u raw_material_required_keys=%u raw_material_optional_keys=%u raw_material_selected_keys=%u raw_material_actor_scoped_keys=%u raw_material_texture_refs=%u raw_source_resident=%u raw_source_missing=%u raw_material_context=%u raw_material_missing=%u raw_cpu_surface_ready=%u raw_legacy_gpu_candidate=%u raw_legacy_gpu_source_skipped=%u raw_estimated_bytes=%llu raw_selected_bytes=%llu raw_skipped_disabled=%u raw_skipped_required_off=%u raw_skipped_optional_off=%u raw_skipped_source_missing=%u raw_skipped_material_missing=%u raw_skipped_byte_budget=%u raw_skipped_job_budget=%u manifest_sources=%u manifest_lines=%u manifest_requests=%u manifest_discovered=%u manifest_unique=%u manifest_skipped_inactive=%u manifest_skipped_syntax=%u manifest_skipped_actor=%u manifest_skipped_unsupported=%u manifest_skipped_invalid=%u manifest_skipped_duplicate=%u max_ms=%u max_jobs=%u max_blas=%u max_bytes=%llu max_material_rows=%u plan_ms=%.3f\n",
			frameIndex,
			settings.enabled ? 1u : 0u,
			settings.dryRun ? 1u : 0u,
			stats.variants,
			stats.required,
			stats.optional,
			stats.selected,
			stats.selectedRequired,
			stats.selectedOptional,
			stats.uniqueMeshes,
			stats.uniqueMaterials,
			stats.surfaceReady,
			stats.directOnly,
			stats.sourceReady,
			stats.materialContextReady,
			stats.meshResident,
			stats.materialResident,
			stats.blasReady,
			stats.ready,
			stats.notReady,
			stats.materialRowsPlanned,
			(unsigned long long)stats.estimatedGeometryBytes,
			(unsigned long long)stats.selectedGeometryBytes,
			(unsigned long long)stats.residentSceneBytes,
			(unsigned long long)stats.residentAsBytes,
			stats.skippedDisabled,
			stats.skippedRequiredOff,
			stats.skippedOptionalOff,
			stats.skippedByteBudget,
			stats.skippedJobBudget,
			stats.skippedMaterialBudget,
			stats.rawVariants,
			stats.rawRequired,
			stats.rawOptional,
			stats.rawSelected,
			stats.rawSelectedRequired,
			stats.rawSelectedOptional,
			stats.rawUniqueMeshes,
			stats.rawUniqueMaterials,
			stats.rawMaterialRequiredKeys,
			stats.rawMaterialOptionalKeys,
			stats.rawMaterialSelectedKeys,
			stats.rawMaterialActorScopedKeys,
			stats.rawMaterialTextureRefs,
			stats.rawSourceResident,
			stats.rawSourceMissing,
			stats.rawMaterialContextReady,
			stats.rawMaterialContextMissing,
			stats.rawCpuSurfaceReady,
			stats.rawLegacyGpuCandidate,
			stats.rawLegacyGpuSourceSkipped,
			(unsigned long long)stats.rawEstimatedGeometryBytes,
			(unsigned long long)stats.rawSelectedGeometryBytes,
			stats.rawSkippedDisabled,
			stats.rawSkippedRequiredOff,
			stats.rawSkippedOptionalOff,
			stats.rawSkippedSourceMissing,
			stats.rawSkippedMaterialMissing,
			stats.rawSkippedByteBudget,
			stats.rawSkippedJobBudget,
			stats.manifestSources,
			stats.manifestLines,
			stats.manifestRequests,
			stats.manifestDiscovered,
			stats.manifestUniqueRequests,
			stats.manifestSkippedInactive,
			stats.manifestSkippedSyntax,
			stats.manifestSkippedActor,
			stats.manifestSkippedUnsupported,
			stats.manifestSkippedInvalid,
			stats.manifestSkippedDuplicate,
			settings.maxMilliseconds,
			settings.maxJobs,
			settings.maxBlasBuilds,
			(unsigned long long)settings.maxBytes,
			settings.maxMaterialRows,
			stats.planMs);
	}

	return stats;
}

const NRIVoxelComputePreloadStats& GetLastNRIVoxelComputePreloadStats()
{
	return gPreloadPlanState.stats;
}

bool IsNRIVoxelComputePreloadRuntimeWithheldMesh(uint64_t buildSerial, uint64_t meshResourceKey)
{
	return
		buildSerial != 0 &&
		gPreloadPlanState.buildSerial == buildSerial &&
		(gPreloadPlanState.runtimeWithheldMeshes.find(meshResourceKey) != gPreloadPlanState.runtimeWithheldMeshes.end() ||
		 gPreloadPlanState.runtimeProbeMeshes.find(meshResourceKey) != gPreloadPlanState.runtimeProbeMeshes.end());
}

bool IsNRIVoxelComputePreloadRuntimeProbeMesh(uint64_t buildSerial, uint64_t meshResourceKey)
{
	return
		buildSerial != 0 &&
		gPreloadPlanState.buildSerial == buildSerial &&
		gPreloadPlanState.runtimeProbeMeshes.find(meshResourceKey) != gPreloadPlanState.runtimeProbeMeshes.end();
}

bool IsNRIVoxelComputePreloadRuntimeTailReleased(uint64_t buildSerial)
{
	return
		buildSerial != 0 &&
		gPreloadPlanState.buildSerial == buildSerial &&
		gPreloadPlanState.runtimeTailReleased;
}

void NotifyNRIVoxelComputePreloadRuntimeTailReleased(uint64_t buildSerial, uint32_t frameIndex)
{
	if (buildSerial == 0 ||
		gPreloadPlanState.buildSerial != buildSerial ||
		(gPreloadPlanState.runtimeWithheldMeshes.empty() && gPreloadPlanState.runtimeProbeMeshes.empty()) ||
		gPreloadPlanState.runtimeTailReleased)
	{
		return;
	}
	gPreloadPlanState.runtimeTailReleased = true;
	if ((int)nri_ptvoxelcomputepreloadtrace > 0)
	{
		Printf("PERF pt voxel preload runtime tail release NRI: build_serial=%llu frame=%u withheld_unique_meshes=%u probe_unique_meshes=%u\n",
			(unsigned long long)buildSerial,
			frameIndex,
			(uint32_t)gPreloadPlanState.runtimeWithheldMeshes.size(),
			(uint32_t)gPreloadPlanState.runtimeProbeMeshes.size());
	}
}

NRIVoxelComputePreloadClosureStats BuildNRIVoxelComputePreloadClosure(
	const NRIPersistentVoxelResidency& residency,
	uint64_t buildSerial)
{
	NRIVoxelComputePreloadClosureStats closure = {};
	closure.buildSerial = buildSerial;
	if (buildSerial == 0 || gPreloadPlanState.buildSerial != buildSerial)
	{
		return closure;
	}

	closure.valid = true;
	closure.strictRequested = gPreloadPlanState.settings.strict;
	closure.dryRun = gPreloadPlanState.settings.dryRun;
	closure.memoryGuardHit = gPreloadPlanState.stats.memoryGuardHit;
	closure.sequence = ++gPreloadPlanState.closureSequence;
	closure.manifestHash = gPreloadPlanState.stats.manifestHash;
	closure.selectedBindings = gPreloadPlanState.stats.rawCandidateBindings;
	closure.admittedBindings = gPreloadPlanState.stats.rawSelected;
	closure.selectedUniqueSources = gPreloadPlanState.stats.rawSelectedUniqueSources;
	closure.selectedUniqueMeshes = gPreloadPlanState.stats.rawSelectedUniqueMeshes;
	closure.selectedUniqueMaterials = gPreloadPlanState.stats.rawSelectedUniqueMaterials;

	std::unordered_set<uint64_t> readyPairs;
	std::unordered_set<uint64_t> readyMeshes;
	std::unordered_set<uint64_t> readyMaterials;
	std::unordered_set<uint64_t> selectedTextures;
	std::unordered_set<uint64_t> readyTextures;
	std::unordered_set<uint64_t> withheldReadyMeshes;
	std::unordered_set<uint64_t> withheldReadyMaterials;
	for (const PlannedBinding& binding : gPreloadPlanState.bindings)
	{
		if (binding.disposition == PlannedBindingDisposition::Failed)
		{
			closure.failedBindings++;
			continue;
		}
		if (binding.disposition == PlannedBindingDisposition::CapSkipped)
		{
			closure.capSkippedBindings++;
			continue;
		}

		const PersistentVoxelReadinessStatus readiness = residency.GetSharedVariantReadiness(binding.meshResourceKey, binding.materialKey);
		std::vector<uint64_t> materialTextureKeys;
		const bool materialTexturesKnown = residency.AppendMaterialTextureKeys(binding.materialKey, materialTextureKeys);
		if (materialTexturesKnown)
		{
			for (uint64_t textureKey : materialTextureKeys)
			{
				selectedTextures.insert(textureKey);
				if (readiness.materialPublished)
				{
					readyTextures.insert(textureKey);
				}
			}
		}
		if (binding.runtimeWithheld)
		{
			closure.capSkippedBindings++;
			closure.runtimeWithheldBindings++;
			if (readiness.meshPresent && readiness.blasReady)
			{
				withheldReadyMeshes.insert(binding.meshResourceKey);
			}
			if (readiness.materialPublished)
			{
				withheldReadyMaterials.insert(binding.materialKey);
			}
			continue;
		}
		if (!readiness.ready)
		{
			closure.pendingBindings++;
			continue;
		}

		const uint64_t pairKey = HashValue(HashValue(1469598103934665603ull, binding.meshResourceKey), binding.materialKey);
		if (readyPairs.insert(pairKey).second)
		{
			closure.readyBindings++;
		}
		else
		{
			closure.reusedBindings++;
		}
		readyMeshes.insert(binding.meshResourceKey);
		readyMaterials.insert(binding.materialKey);
	}

	const NRIPersistentVoxelStatusSnapshot residencyStatus = residency.BuildStatusSnapshot();
	const uint32_t failedAdmissions = std::min(closure.pendingBindings, residencyStatus.failedAdmissionCount);
	closure.pendingBindings -= failedAdmissions;
	closure.failedBindings += failedAdmissions;
	closure.readyUniqueMeshes = (uint32_t)readyMeshes.size();
	closure.readyUniqueMaterials = (uint32_t)readyMaterials.size();
	closure.selectedUniqueTextures = (uint32_t)selectedTextures.size();
	closure.readyUniqueTextures = (uint32_t)readyTextures.size();
	closure.runtimeWithheldUniqueMeshes = gPreloadPlanState.stats.rawRuntimeWithheldUniqueMeshes;
	closure.runtimeWithheldReadyMeshes = (uint32_t)withheldReadyMeshes.size();
	closure.runtimeWithheldReadyMaterials = (uint32_t)withheldReadyMaterials.size();
	// The residency map intentionally retains ready entries. This field counts
	// only nonterminal admission work that can still block strict closure.
	closure.admissionQueueCount =
		residencyStatus.requiredAdmissionPendingCount +
		residencyStatus.optionalAdmissionPendingCount;
	const NRIVoxelComputeMemoryUsage computeMemory = GetNRIVoxelComputeMemoryUsage();
	closure.computeInFlightCount =
		residencyStatus.computeInFlightCount +
		computeMemory.queuedJobCount +
		computeMemory.pendingJobCount;
	closure.blasInFlightCount = residencyStatus.blasInFlightCount;
	closure.cpuGeometryBuilds = residencyStatus.cpuGeometryBuildCount;
	closure.cpuGeometryUploads = residencyStatus.cpuGeometryUploadCount;
	closure.cpuGeometryUploadBytes = residencyStatus.cpuGeometryUploadBytes;
	closure.cpuGeometryFallback = residencyStatus.cpuGeometryFallbackCount;
	closure.fullGeometryReadbackBytes = computeMemory.totalFullGeometryReadbackBytes;

	const bool reconciled =
		closure.selectedBindings ==
		closure.readyBindings +
		closure.reusedBindings +
		closure.failedBindings +
		closure.capSkippedBindings +
		closure.staleCancelledBindings +
		closure.pendingBindings;
	const bool complete =
		reconciled &&
		closure.selectedBindings != 0 &&
		closure.readyBindings + closure.reusedBindings == closure.selectedBindings &&
		closure.readyUniqueMeshes == closure.selectedUniqueMeshes &&
		closure.readyUniqueMaterials == closure.selectedUniqueMaterials &&
		closure.readyUniqueTextures == closure.selectedUniqueTextures &&
		closure.admissionQueueCount == 0 &&
		closure.computeInFlightCount == 0 &&
		closure.blasInFlightCount == 0 &&
		closure.cpuGeometryBuilds == 0 &&
		closure.cpuGeometryUploads == 0 &&
		closure.cpuGeometryFallback == 0 &&
		closure.fullGeometryReadbackBytes == 0;
	if (closure.dryRun)
	{
		closure.outcome = "dry-run";
	}
	else if (closure.memoryGuardHit)
	{
		closure.outcome = "memory-abort";
	}
	else if (!closure.strictRequested)
	{
		closure.outcome = "bounded";
	}
	else
	{
		closure.outcome = complete ? "complete" : "incomplete";
	}
	return closure;
}
