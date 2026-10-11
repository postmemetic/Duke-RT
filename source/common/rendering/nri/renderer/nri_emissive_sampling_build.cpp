#include "nri_scene_lights.h"
#include "nri_cvars.h"
#include "nri_diagnostic_names.h"
#include "../scene/nri_hash.h"
#include "printf.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace
{
	constexpr uint32_t NriMaxEmissivePrimitives = 16384u;
	void Copy3f(const float* source, float* destination)
	{
		destination[0] = source[0];
		destination[1] = source[1];
		destination[2] = source[2];
	}
	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}
	bool IsGlowDrivenEmissive(uint32_t sourceFlags, uint32_t emissiveMode)
	{
		if (emissiveMode == nri_scene::MaterialEmissiveMode_UseGlowmapTexture)
		{
			return true;
		}

		return (sourceFlags & (SceneEmissiveSurfaceSourceFlag_AutoTextureGlow | SceneEmissiveSurfaceSourceFlag_AutoGlowmap)) != 0;
	}

	float ResolveGlowSamplingScale(uint32_t sourceFlags, uint32_t emissiveMode, const NRILightingSettings& settings)
	{
		return IsGlowDrivenEmissive(sourceFlags, emissiveMode) ? std::max(settings.glowReach, 0.0f) : 1.0f;
	}

}


void SceneLightSystem::BuildEmissiveSamplingUpload(
	const EmissiveSamplingBuildContext& context,
	NRIEmissivePrimitiveHeaderGpuData& outHeader,
	std::vector<NRIEmissivePrimitiveGpuData>& outPrimitives,
	std::vector<float>& outCdf,
	std::vector<NRIEmissiveMaterialResponseGpuData>& outMaterialResponses,
	std::vector<NRIEmissivePrimitiveDebugRecord>& outDebugRecords,
	EmissiveSamplingUploadStats* outStats)
{
	// Shadow construction starts from the same proposal history. It must not
	// advance production bound growth, dark retention or stable ordering twice.
	const bool validate = (bool)nri_ptemissivecachevalidate;
	NRIEmissiveSamplingDistribution referenceDistribution;
	if (validate) referenceDistribution = mEmissiveSamplingDistribution;
	EmissiveSamplingUploadStats stats = {};
	const bool timing = mEmissiveGeometryCache.TimingEnabled();
	const auto buildStart = timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	const double topologyStartMs = mEmissiveGeometryCache.Stats().topologyMs;
	const size_t candidateCapacity = mEmissiveSamplingCandidates.capacity();
	const size_t distributionCapacity = mEmissiveDistributionCandidates.capacity();
	const size_t entryCapacity = mEmissiveDistributionEntries.capacity();
	BuildEmissiveSamplingUploadImpl(context, outHeader, outPrimitives, outCdf,
		outMaterialResponses, outDebugRecords, &stats, (bool)nri_ptemissivecache);
	const auto recordGrowth = [&](size_t before, size_t after, size_t stride)
	{
		if (after > before)
		{
			stats.buildCapacityGrowths++;
			stats.buildCapacityGrowthBytes += (after - before) * stride;
		}
	};
	recordGrowth(candidateCapacity, mEmissiveSamplingCandidates.capacity(), sizeof(EmissiveSamplingBuiltCandidate));
	recordGrowth(distributionCapacity, mEmissiveDistributionCandidates.capacity(), sizeof(NRIEmissiveSamplingDistributionCandidate));
	recordGrowth(entryCapacity, mEmissiveDistributionEntries.capacity(), sizeof(NRIEmissiveSamplingDistributionEntry));
	if (timing)
		stats.weightsBuildMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - buildStart).count() -
			(mEmissiveGeometryCache.Stats().topologyMs - topologyStartMs);
	if (validate)
	{
		const auto start = std::chrono::steady_clock::now();
		NRIEmissivePrimitiveHeaderGpuData referenceHeader = {};
		std::vector<NRIEmissivePrimitiveGpuData> referencePrimitives;
		std::vector<float> referenceCdf;
		std::vector<NRIEmissiveMaterialResponseGpuData> referenceResponses;
		std::vector<NRIEmissivePrimitiveDebugRecord> referenceDebug;
		EmissiveSamplingUploadStats referenceStats = {};
		std::swap(referenceDistribution, mEmissiveSamplingDistribution);
		BuildEmissiveSamplingUploadImpl(context, referenceHeader, referencePrimitives, referenceCdf,
			referenceResponses, referenceDebug, &referenceStats, false);
		std::swap(referenceDistribution, mEmissiveSamplingDistribution);
		const auto equalBytes = [](const auto& lhs, const auto& rhs)
		{
			return lhs.size() == rhs.size() && (lhs.empty() ||
				std::memcmp(lhs.data(), rhs.data(), lhs.size() * sizeof(lhs[0])) == 0);
		};
		bool debugEqual = outDebugRecords.size() == referenceDebug.size();
		for (size_t i = 0; debugEqual && i < outDebugRecords.size(); ++i)
		{
			// Debug records end in bools; exclude trailing struct padding.
			debugEqual = std::memcmp(&outDebugRecords[i], &referenceDebug[i],
				offsetof(NRIEmissivePrimitiveDebugRecord, sectorResponseApplied) + sizeof(bool)) == 0;
		}
		const bool equal = std::memcmp(&outHeader, &referenceHeader, sizeof(outHeader)) == 0 &&
			equalBytes(outPrimitives, referencePrimitives) && equalBytes(outCdf, referenceCdf) &&
			equalBytes(outMaterialResponses, referenceResponses) && debugEqual;
		stats.payloadValidationChecks = 1;
		stats.payloadValidationMismatches = equal ? 0u : 1u;
		if (!equal)
		{
			mEmissiveGeometryCache.QuarantineAll();
			outHeader = referenceHeader;
			outPrimitives.swap(referencePrimitives);
			outCdf.swap(referenceCdf);
			outMaterialResponses.swap(referenceResponses);
			outDebugRecords.swap(referenceDebug);
			mEmissiveSamplingDistribution = std::move(referenceDistribution);
			// Report the semantic counts for the reference payload actually used,
			// while keeping primary-build work and validation timing separate.
			referenceStats.buildCapacityGrowths = stats.buildCapacityGrowths;
			referenceStats.buildCapacityGrowthBytes = stats.buildCapacityGrowthBytes;
			referenceStats.fullStaticPrimitivesScanned = stats.fullStaticPrimitivesScanned;
			referenceStats.fullDynamicPrimitivesScanned = stats.fullDynamicPrimitivesScanned;
			referenceStats.payloadValidationChecks = stats.payloadValidationChecks;
			referenceStats.payloadValidationMismatches = stats.payloadValidationMismatches;
			referenceStats.weightsBuildMs = stats.weightsBuildMs;
			stats = referenceStats;
			Printf("NRI PT emissive cache validation: frame=%llu payload_mismatch=1 action=quarantine-full-hash\n",
				(unsigned long long)mFrameSerial);
		}
		stats.payloadValidationMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start).count();
		uint64_t topologyHash = 1469598103934665603ull;
		uint64_t cdfHash = 1469598103934665603ull;
		for (const auto& record : outDebugRecords)
		{
			topologyHash = nri_scene::HashCombine64(topologyHash, record.stableKey);
			topologyHash = nri_scene::HashCombine64(topologyHash, record.primitiveIndex);
		}
		for (float value : outCdf) cdfHash = nri_scene::HashCombine64(cdfHash, FloatBits(value));
		Printf("NRI PT emissive payload validation: frame=%llu checks=1 mismatches=%u topology_hash=0x%016llx cdf_hash=0x%016llx\n",
			(unsigned long long)mFrameSerial, stats.payloadValidationMismatches,
			(unsigned long long)topologyHash, (unsigned long long)cdfHash);
	}
	if (outStats != nullptr) *outStats = stats;
}
void SceneLightSystem::BuildEmissiveSamplingUploadImpl(
	const EmissiveSamplingBuildContext& context,
	NRIEmissivePrimitiveHeaderGpuData& outHeader,
	std::vector<NRIEmissivePrimitiveGpuData>& outPrimitives,
	std::vector<float>& outCdf,
	std::vector<NRIEmissiveMaterialResponseGpuData>& outMaterialResponses,
	std::vector<NRIEmissivePrimitiveDebugRecord>& outDebugRecords,
	EmissiveSamplingUploadStats* outStats,
	bool useGeometryCache)
{
	EmissiveSamplingUploadStats localStats = {};
	outHeader = {};
	outHeader.dominantIndex = UINT32_MAX;
	outHeader.flags = 0u;
	outPrimitives.clear();
	outCdf.clear();
	outMaterialResponses.clear();
	outDebugRecords.clear();
	NRIEmissiveMaterialResponseGpuData materialResponseHeader = {};
	materialResponseHeader.primitiveIndex = UINT32_MAX;
	materialResponseHeader.materialScale = 1.0f;
	outMaterialResponses.push_back(materialResponseHeader);

	using MaterialPrimitiveRange = NRIEmissiveMaterialPrimitiveRange;
	using BuiltCandidate = EmissiveSamplingBuiltCandidate;
	std::array<std::vector<MaterialPrimitiveRange>, 5> referenceRanges;
	std::array<const std::vector<MaterialPrimitiveRange>*, 5> preparedRanges = {};
	auto rangesFor = [&](NRIEmissiveGeometryDomain domain, const nri_scene::GeometryData* geometry)
		-> const std::vector<MaterialPrimitiveRange>&
	{
		const auto* prepared = preparedRanges[(size_t)domain];
		if (prepared != nullptr) return *prepared;
		if (useGeometryCache)
		{
			preparedRanges[(size_t)domain] = &mEmissiveGeometryCache.Ranges(domain, geometry);
			return *preparedRanges[(size_t)domain];
		}
		auto& ranges = referenceRanges[(size_t)domain];
		NRIEmissiveGeometryCache::BuildRanges(geometry, ranges);
		if (geometry != nullptr)
		{
			// BuildRanges makes one pass for slots and one for primitive spans.
			auto& scans = domain == NRIEmissiveGeometryDomain::Static ?
				localStats.fullStaticPrimitivesScanned : localStats.fullDynamicPrimitivesScanned;
			scans += geometry->primitives.size() * 2u;
		}
		preparedRanges[(size_t)domain] = &ranges;
		return ranges;
	};
	if (!useGeometryCache)
	{
		rangesFor(NRIEmissiveGeometryDomain::Static, context.staticGeometry);
		rangesFor(NRIEmissiveGeometryDomain::Captured, context.capturedGeometry);
		rangesFor(NRIEmissiveGeometryDomain::RuntimeMutation, context.runtimeMutationGeometry);
		rangesFor(NRIEmissiveGeometryDomain::Dynamic, context.dynamicGeometry);
		rangesFor(NRIEmissiveGeometryDomain::SurfaceLightOverlay, context.surfaceLightOverlayGeometry);
	}

	const NRILightingSettings settings = CaptureSettings();
	auto& candidates = mEmissiveSamplingCandidates;
	candidates.clear();
	auto& materialResponseLookup = mEmissiveMaterialResponseLookup;
	materialResponseLookup.clear();
	const auto& activeSurfaces = mEmissiveSurfaces.activeSurfaces;
	candidates.reserve(activeSurfaces.size());

	auto appendSurfacePrimitives = [&](const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface, const nri_scene::GeometryData* geometry, const std::vector<MaterialPrimitiveRange>& ranges, uint32_t dataSource, uint32_t primitiveBase, NRIEmissiveGeometryDomain domain)
	{
		if (geometry == nullptr || surface.materialIndex == UINT32_MAX || surface.materialIndex >= ranges.size())
		{
			return;
		}

		const auto& range = ranges[surface.materialIndex];
		if (range.count == 0 || range.first == UINT32_MAX)
		{
			return;
		}

		float representativeLuminance = 0.0f;
		if (surface.surfaceArea > 0.0f && surface.emissiveIntensity > 0.0f)
		{
			representativeLuminance = std::max(surface.powerEstimate / (surface.surfaceArea * surface.emissiveIntensity), 0.0f);
		}
		const float samplingScale = ResolveGlowSamplingScale(surface.sourceFlags, surface.emissiveMode, settings) * std::max(surface.reachScale, 0.0f);
		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		bool sectorResponseApplied = false;
		const float sectorRawResponseScale = ResolveSectorEmissionScale(surface, sectorResponseApplied);
		const float sectorResponseScale = sectorResponseApplied ? ResolveSectorEmissionIntensityScale(surface, sectorRawResponseScale) : 1.0f;
		const float sectorReachScale = sectorResponseApplied ? ResolveSectorEmissionReachScale(surface, sectorRawResponseScale) : 1.0f;
		bool materialResponseApplied = false;
		const float materialResponseScale = ResolveEmissiveMaterialResponseScale(surface, materialResponseApplied);
		const bool materialResponseEligible = IsEmissiveSurfaceMaterialResponseEligible(surface);
		const float sectorReachBound = sectorResponseEligible ?
			(surface.hasSectorResponseReachMax ?
				std::max(std::max(0.0f, surface.sectorResponseReachMin), surface.sectorResponseReachMax) :
				std::max(std::max(0.0f, (float)nri_ptsectoremissionreachmin), (float)nri_ptsectoremissionreachmax)) :
			1.0f;
		uint64_t surfacePrimitiveKey = surface.stableKey;

		for (uint32_t localOffset = 0; localOffset < range.count; ++localOffset)
		{
			const uint32_t localPrimitiveIndex = range.first + localOffset;
			const uint32_t primitiveIndex = primitiveBase + localPrimitiveIndex;
			const NRIEmissivePrimitiveGeometry primitiveGeometry = useGeometryCache ?
				mEmissiveGeometryCache.Primitive(domain, *geometry, localPrimitiveIndex) :
				NRIEmissiveGeometryCache::BuildPrimitive(*geometry, localPrimitiveIndex);
			const float primitiveArea = primitiveGeometry.area;
			if (primitiveArea <= 0.0f)
			{
				continue;
			}

			BuiltCandidate candidate = {};
			candidate.gpu.dataSource = dataSource;
			candidate.gpu.primitiveIndex = primitiveIndex;
			candidate.gpu.sourceFlags = surface.sourceFlags;
			candidate.gpu.textureId = surface.textureId;
			candidate.gpu.primitiveArea = primitiveArea;
			const float basePowerEstimate = std::max(primitiveArea * representativeLuminance * surface.emissiveIntensity, 0.0f);
			candidate.gpu.powerEstimate = basePowerEstimate * sectorResponseScale * materialResponseScale;
			candidate.gpu.selectionWeight = basePowerEstimate * samplingScale * sectorReachScale * materialResponseScale;
			candidate.gpu.emissionScale = sectorResponseScale * materialResponseScale;
			candidate.gpu.materialResponseScale = std::max(materialResponseScale, 0.0f);
			candidate.referenceProposalWeight = basePowerEstimate * samplingScale * sectorReachBound * materialResponseScale;
			candidate.hasReferenceProposalWeight = sectorResponseEligible;

			candidate.debug.stableKey = nri_scene::HashCombine64(surfacePrimitiveKey, ((uint64_t)dataSource << 32u) | localOffset);
			candidate.debug.surfaceStableKey = surface.stableKey;
			candidate.debug.dataSource = dataSource;
			candidate.debug.primitiveIndex = primitiveIndex;
			candidate.debug.materialIndex = surface.materialIndex;
			candidate.debug.sourceFlags = surface.sourceFlags;
			candidate.debug.sourceRuleId = surface.sourceRuleId;
			candidate.debug.overrideRuleId = surface.overrideRuleId;
			candidate.debug.textureId = surface.textureId;
			candidate.debug.emissiveMode = surface.emissiveMode;
			candidate.debug.emissiveTextureIndex = surface.emissiveTextureIndex;
			candidate.debug.actorIndex = surface.actorIndex;
			candidate.debug.sectorIndex = surface.sectorIndex;
			candidate.debug.primitiveArea = primitiveArea;
			candidate.debug.powerEstimate = candidate.gpu.powerEstimate;
			candidate.debug.selectionWeight = candidate.gpu.selectionWeight;
			candidate.debug.selectionPdf = 0.0f;
			candidate.debug.emissiveIntensity = surface.emissiveIntensity * sectorResponseScale;
			candidate.debug.sectorResponseScale = sectorResponseScale;
			candidate.debug.sectorReachScale = sectorReachScale;
			candidate.debug.materialResponseEnabled = materialResponseEligible;
			candidate.debug.materialResponseScale = materialResponseScale;
			candidate.debug.sectorResponseApplied = sectorResponseApplied;
			Copy3f(surface.emissiveColor, candidate.debug.emissiveColor);
			Copy3f(primitiveGeometry.center, candidate.gpu.boundsCenter);
			candidate.gpu.boundsRadius = primitiveGeometry.radius;
			Copy3f(candidate.gpu.boundsCenter, candidate.debug.center);
			candidate.debug.boundsRadius = candidate.gpu.boundsRadius;

			candidate.gpu.stableKeyLo = (uint32_t)(candidate.debug.stableKey & 0xffffffffu);
			candidate.gpu.stableKeyHi = (uint32_t)(candidate.debug.stableKey >> 32u);
			candidates.push_back(candidate);

			if (materialResponseEligible)
			{
				const uint64_t responseKey = ((uint64_t)dataSource << 32u) | primitiveIndex;
				if (materialResponseLookup.find(responseKey) == materialResponseLookup.end())
				{
					materialResponseLookup.emplace(responseKey, (uint32_t)outMaterialResponses.size());
					NRIEmissiveMaterialResponseGpuData response = {};
					response.dataSource = dataSource;
					response.primitiveIndex = primitiveIndex;
					response.materialScale = std::max(0.0f, materialResponseScale);
					outMaterialResponses.push_back(response);
				}
			}
		}
	};
	auto appendPlacedVoxelRange = [&](const EmissiveSurfaceRegistry::EmissiveSurfaceRecord& surface)
	{
		if (surface.sceneInstanceIndex == UINT32_MAX || surface.placedPrimitiveCount == 0u)
		{
			localStats.skippedPersistentVoxelSurfaces++;
			return;
		}
		const float samplingScale = ResolveGlowSamplingScale(surface.sourceFlags, surface.emissiveMode, settings) * std::max(surface.reachScale, 0.0f);
		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		bool sectorResponseApplied = false;
		const float sectorRawResponseScale = ResolveSectorEmissionScale(surface, sectorResponseApplied);
		const float sectorResponseScale = sectorResponseApplied ? ResolveSectorEmissionIntensityScale(surface, sectorRawResponseScale) : 1.0f;
		const float sectorReachScale = sectorResponseApplied ? ResolveSectorEmissionReachScale(surface, sectorRawResponseScale) : 1.0f;
		bool materialResponseApplied = false;
		const float materialResponseScale = ResolveEmissiveMaterialResponseScale(surface, materialResponseApplied);
		const float sectorReachBound = sectorResponseEligible ?
			(surface.hasSectorResponseReachMax ?
				std::max(std::max(0.0f, surface.sectorResponseReachMin), surface.sectorResponseReachMax) :
				std::max(std::max(0.0f, (float)nri_ptsectoremissionreachmin), (float)nri_ptsectoremissionreachmax)) :
			1.0f;

		BuiltCandidate candidate = {};
		candidate.gpu.dataSource = nri_diag::SceneDataSourcePersistentVoxel;
		candidate.gpu.primitiveIndex = surface.placedPrimitiveBase;
		candidate.gpu.primitiveCount = surface.placedPrimitiveCount;
		candidate.gpu.sceneInstanceIndex = surface.sceneInstanceIndex;
		candidate.gpu.occurrenceKeyLo = surface.occurrenceKeyLo;
		candidate.gpu.occurrenceKeyHi = surface.occurrenceKeyHi;
		candidate.gpu.occurrenceGeneration = surface.occurrenceGeneration;
		Copy3f(surface.center, candidate.gpu.boundsCenter);
		candidate.gpu.boundsRadius = std::max(surface.boundsRadius, 0.0f);
		candidate.gpu.sourceFlags = surface.sourceFlags;
		candidate.gpu.textureId = surface.textureId;
		candidate.gpu.primitiveArea = std::max(surface.surfaceArea, 0.0f);
		candidate.gpu.powerEstimate = std::max(surface.powerEstimate, 0.0f) * sectorResponseScale * materialResponseScale;
		candidate.gpu.selectionWeight = std::max(surface.powerEstimate, 0.0f) * samplingScale * sectorReachScale * materialResponseScale;
		candidate.gpu.emissionScale = sectorResponseScale * materialResponseScale;
		candidate.gpu.materialResponseScale = std::max(materialResponseScale, 0.0f);
		candidate.referenceProposalWeight = std::max(surface.powerEstimate, 0.0f) * samplingScale * sectorReachBound * materialResponseScale;
		candidate.hasReferenceProposalWeight = sectorResponseEligible;

		candidate.debug.stableKey = nri_scene::HashCombine64(surface.stableKey, 0x504C41434544564Full);
		candidate.debug.surfaceStableKey = surface.stableKey;
		candidate.debug.dataSource = candidate.gpu.dataSource;
		candidate.debug.primitiveIndex = candidate.gpu.primitiveIndex;
		candidate.debug.primitiveCount = candidate.gpu.primitiveCount;
		candidate.debug.sceneInstanceIndex = candidate.gpu.sceneInstanceIndex;
		candidate.debug.occurrenceKeyLo = candidate.gpu.occurrenceKeyLo;
		candidate.debug.occurrenceKeyHi = candidate.gpu.occurrenceKeyHi;
		candidate.debug.occurrenceGeneration = candidate.gpu.occurrenceGeneration;
		candidate.debug.materialIndex = surface.materialIndex;
		candidate.debug.sourceFlags = surface.sourceFlags;
		candidate.debug.sourceRuleId = surface.sourceRuleId;
		candidate.debug.overrideRuleId = surface.overrideRuleId;
		candidate.debug.textureId = surface.textureId;
		candidate.debug.emissiveMode = surface.emissiveMode;
		candidate.debug.emissiveTextureIndex = surface.emissiveTextureIndex;
		candidate.debug.actorIndex = surface.actorIndex;
		candidate.debug.sectorIndex = surface.sectorIndex;
		candidate.debug.boundsRadius = candidate.gpu.boundsRadius;
		candidate.debug.primitiveArea = candidate.gpu.primitiveArea;
		candidate.debug.powerEstimate = candidate.gpu.powerEstimate;
		candidate.debug.selectionWeight = candidate.gpu.selectionWeight;
		candidate.debug.emissiveIntensity = surface.emissiveIntensity * sectorResponseScale * materialResponseScale;
		candidate.debug.sectorResponseScale = sectorResponseScale;
		candidate.debug.sectorReachScale = sectorReachScale;
		candidate.debug.materialResponseEnabled = IsEmissiveSurfaceMaterialResponseEligible(surface);
		candidate.debug.materialResponseScale = materialResponseScale;
		candidate.debug.sectorResponseApplied = sectorResponseApplied;
		Copy3f(surface.center, candidate.debug.center);
		Copy3f(surface.emissiveColor, candidate.debug.emissiveColor);
		candidate.gpu.stableKeyLo = (uint32_t)(candidate.debug.stableKey & 0xffffffffu);
		candidate.gpu.stableKeyHi = (uint32_t)(candidate.debug.stableKey >> 32u);
		candidates.push_back(candidate);
	};

	for (const auto& surface : activeSurfaces)
	{
		switch (surface.source)
		{
		case SceneLightRecordSource::StaticMapScene:
			localStats.surfaceStatic++;
			appendSurfacePrimitives(surface, context.staticGeometry, rangesFor(NRIEmissiveGeometryDomain::Static, context.staticGeometry), nri_diag::SceneDataSourceStatic, 0u, NRIEmissiveGeometryDomain::Static);
			break;
		case SceneLightRecordSource::CapturedScene:
			localStats.surfaceCaptured++;
			appendSurfacePrimitives(surface, context.capturedGeometry, rangesFor(NRIEmissiveGeometryDomain::Captured, context.capturedGeometry), nri_diag::SceneDataSourceDynamic, 0u, NRIEmissiveGeometryDomain::Captured);
			break;
		case SceneLightRecordSource::RuntimeMutationScene:
			localStats.surfaceRuntimeMutation++;
			appendSurfacePrimitives(surface, context.runtimeMutationGeometry, rangesFor(NRIEmissiveGeometryDomain::RuntimeMutation, context.runtimeMutationGeometry), nri_diag::SceneDataSourceDynamic, context.runtimeMutationPrimitiveBaseOffset, NRIEmissiveGeometryDomain::RuntimeMutation);
			break;
		case SceneLightRecordSource::DynamicScene:
			localStats.surfaceDynamic++;
			appendSurfacePrimitives(surface, context.dynamicGeometry, rangesFor(NRIEmissiveGeometryDomain::Dynamic, context.dynamicGeometry), nri_diag::SceneDataSourceDynamic, context.dynamicPrimitiveBaseOffset, NRIEmissiveGeometryDomain::Dynamic);
			break;
		case SceneLightRecordSource::SurfaceLightOverlayScene:
			localStats.surfaceLightOverlay++;
			appendSurfacePrimitives(surface, context.surfaceLightOverlayGeometry, rangesFor(NRIEmissiveGeometryDomain::SurfaceLightOverlay, context.surfaceLightOverlayGeometry), nri_diag::SceneDataSourceDynamic, context.surfaceLightOverlayPrimitiveBaseOffset, NRIEmissiveGeometryDomain::SurfaceLightOverlay);
			break;
		case SceneLightRecordSource::PersistentVoxelScene:
			localStats.surfacePersistentVoxel++;
			appendPlacedVoxelRange(surface);
			break;
		default:
			break;
		}
	}

	auto& distributionCandidates = mEmissiveDistributionCandidates;
	distributionCandidates.clear();
	distributionCandidates.reserve(candidates.size());
	for (const auto& candidate : candidates)
	{
		NRIEmissiveSamplingDistributionCandidate distributionCandidate = {};
		distributionCandidate.stableKey = candidate.debug.stableKey;
		distributionCandidate.bindingKey = nri_scene::HashCombine64(
			candidate.debug.stableKey,
			(uint64_t)candidate.debug.emissiveMode);
		distributionCandidate.tieBreakKey = nri_scene::HashCombine64(
			nri_scene::HashCombine64((uint64_t)candidate.gpu.dataSource, (uint64_t)candidate.gpu.primitiveIndex),
			(uint64_t)candidate.gpu.sceneInstanceIndex);
		distributionCandidate.proposalWeight = candidate.gpu.selectionWeight;
		distributionCandidate.referenceProposalWeight = candidate.referenceProposalWeight;
		distributionCandidate.hasReferenceProposalWeight = candidate.hasReferenceProposalWeight;
		distributionCandidate.live = candidate.gpu.powerEstimate > 0.0f && candidate.gpu.emissionScale > 0.0f;
		distributionCandidates.push_back(distributionCandidate);
	}
	auto& distributionEntries = mEmissiveDistributionEntries;
	distributionEntries.clear();
	NRIEmissiveSamplingDistributionStats distributionStats = {};
	mEmissiveSamplingDistribution.Build(
		distributionCandidates,
		mFrameSerial,
		NriMaxEmissivePrimitives,
		distributionEntries,
		outCdf,
		&distributionStats);
	localStats.buildCapacityGrowths = distributionStats.scratchCapacityGrowths;
	localStats.buildCapacityGrowthBytes = distributionStats.scratchCapacityGrowthBytes;
	localStats.proposalBoundGrowthCount = distributionStats.boundGrowthCount;
	localStats.lastProposalBoundGrowthStableKey = distributionStats.lastBoundGrowthStableKey;
	localStats.lastProposalBoundGrowthOldWeight = distributionStats.lastBoundGrowthOldWeight;
	localStats.lastProposalBoundGrowthNewWeight = distributionStats.lastBoundGrowthNewWeight;
	localStats.lastProposalBoundGrowthWasAuthored = distributionStats.lastBoundGrowthWasAuthored;
	localStats.proposalActiveCount = distributionStats.activeCount;
	localStats.proposalRetainedDarkCount = distributionStats.retainedDarkCount;
	localStats.proposalReactivatedCount = distributionStats.reactivatedCount;
	localStats.proposalRetiredMissingCount = distributionStats.retiredMissingCount;
	localStats.proposalRetiredReplacedCount = distributionStats.retiredReplacedCount;
	localStats.proposalRecordCount = distributionStats.recordCount;

	outPrimitives.reserve(candidates.size());
	outDebugRecords.reserve(candidates.size());

	float totalPower = 0.0f;
	float dominantPower = -1.0f;

	for (const auto& distributionEntry : distributionEntries)
	{
		BuiltCandidate candidate = candidates[distributionEntry.inputIndex];
		// The distribution resolves duplicate authored keys into unique,
		// deterministically ordered identities. Publish that resolved key to
		// GPU consumers so temporal reservoirs cannot confuse two candidates
		// that shared the pre-distribution key.
		candidate.gpu.stableKeyLo = (uint32_t)(distributionEntry.stableKey & 0xffffffffu);
		candidate.gpu.stableKeyHi = (uint32_t)(distributionEntry.stableKey >> 32u);
		candidate.debug.stableKey = distributionEntry.stableKey;
		candidate.gpu.selectionWeight = distributionEntry.proposalWeight;
		candidate.gpu.selectionPdf = distributionEntry.selectionPdf;
		candidate.debug.selectionWeight = distributionEntry.proposalWeight;
		candidate.debug.selectionPdf = distributionEntry.selectionPdf;
		outPrimitives.push_back(candidate.gpu);
		outDebugRecords.push_back(candidate.debug);
		if (candidate.debug.dataSource == nri_diag::SceneDataSourceStatic)
		{
			localStats.outputStaticRecords++;
		}
		else if (candidate.debug.dataSource == nri_diag::SceneDataSourceDynamic)
		{
			localStats.outputDynamicRecords++;
		}
		else if (candidate.debug.dataSource == nri_diag::SceneDataSourcePersistentVoxel)
		{
			localStats.outputPersistentVoxelRecords++;
			localStats.outputPersistentVoxelPrimitivesRepresented += candidate.gpu.primitiveCount;
		}
		totalPower += candidate.gpu.powerEstimate;
		if (candidate.gpu.powerEstimate > dominantPower)
		{
			dominantPower = candidate.gpu.powerEstimate;
			outHeader.dominantIndex = (uint32_t)outPrimitives.size() - 1u;
		}
	}

	outHeader.activeCount = (uint32_t)outPrimitives.size();
	outHeader.totalPower = totalPower;
	FinalizeNRIEmissiveMaterialResponses(outMaterialResponses, (int)nri_ptemissiveresponselookup);
	if (outStats != nullptr)
	{
		*outStats = localStats;
	}

}

uint64_t SceneLightSystem::BuildEmissiveSamplingPayloadHash(const EmissiveSamplingBuildContext& context, bool collectTiming)
{
	mEmissiveGeometryCache.BeginUpdate(collectTiming);
	std::array<bool, (size_t)NRIEmissiveGeometryDomain::Count> contributing = {};
	for (const auto& surface : mEmissiveSurfaces.activeSurfaces)
	{
		switch (surface.source)
		{
		case SceneLightRecordSource::StaticMapScene: contributing[(size_t)NRIEmissiveGeometryDomain::Static] = true; break;
		case SceneLightRecordSource::CapturedScene: contributing[(size_t)NRIEmissiveGeometryDomain::Captured] = true; break;
		case SceneLightRecordSource::RuntimeMutationScene: contributing[(size_t)NRIEmissiveGeometryDomain::RuntimeMutation] = true; break;
		case SceneLightRecordSource::DynamicScene: contributing[(size_t)NRIEmissiveGeometryDomain::Dynamic] = true; break;
		case SceneLightRecordSource::SurfaceLightOverlayScene: contributing[(size_t)NRIEmissiveGeometryDomain::SurfaceLightOverlay] = true; break;
		default: break;
		}
	}
	const auto geometryIdentity = [&](NRIEmissiveGeometryDomain domain,
		const nri_scene::GeometryData* geometry, uint64_t identity)
	{
		// Nonemissive actor capture cannot affect this distribution. Conservative
		// producer stamps must not force static sampling uploads for that domain.
		return contributing[(size_t)domain] || !(bool)nri_ptemissivecache ?
			mEmissiveGeometryCache.ResolveIdentity(domain, geometry,
				(bool)nri_ptemissivecache ? identity : 0, (bool)nri_ptemissivecachevalidate) : 0ull;
	};
	uint64_t hash = 1469598103934665603ull;
	// Lookup policy changes the uploaded order/header even when scene content is
	// unchanged. Include it in shared and frame-slot payload reuse identity.
	hash = nri_scene::HashCombine64(hash, ResolveNRIEmissiveResponseLookupMode((int)nri_ptemissiveresponselookup));
	hash = nri_scene::HashCombine64(hash, geometryIdentity(NRIEmissiveGeometryDomain::Static,
		context.staticGeometry, context.staticGeometryIdentity));
	hash = nri_scene::HashCombine64(hash, geometryIdentity(NRIEmissiveGeometryDomain::Captured,
		context.capturedGeometry, context.capturedGeometryIdentity));
	hash = nri_scene::HashCombine64(hash, geometryIdentity(NRIEmissiveGeometryDomain::RuntimeMutation,
		context.runtimeMutationGeometry, context.runtimeMutationGeometryIdentity));
	hash = nri_scene::HashCombine64(hash, (uint64_t)context.runtimeMutationPrimitiveBaseOffset);
	hash = nri_scene::HashCombine64(hash, geometryIdentity(NRIEmissiveGeometryDomain::Dynamic,
		context.dynamicGeometry, context.dynamicGeometryIdentity));
	hash = nri_scene::HashCombine64(hash, (uint64_t)context.dynamicPrimitiveBaseOffset);
	hash = nri_scene::HashCombine64(hash, geometryIdentity(NRIEmissiveGeometryDomain::SurfaceLightOverlay,
		context.surfaceLightOverlayGeometry, context.surfaceLightOverlayGeometryIdentity));
	hash = nri_scene::HashCombine64(hash, (uint64_t)context.surfaceLightOverlayPrimitiveBaseOffset);

	hash = nri_scene::HashCombine64(hash, (uint64_t)mEmissiveSurfaces.activeSurfaces.size());
	for (const auto& surface : mEmissiveSurfaces.activeSurfaces)
	{
		hash = nri_scene::HashCombine64(hash, surface.stableKey);

		const auto propertyIt = mEmissiveSurfaces.activePropertyHashes.find(surface.stableKey);
		hash = nri_scene::HashCombine64(hash, propertyIt != mEmissiveSurfaces.activePropertyHashes.end() ? propertyIt->second : 0ull);

		const auto bindingIt = mEmissiveSurfaces.activeBindingHashes.find(surface.stableKey);
		hash = nri_scene::HashCombine64(hash, bindingIt != mEmissiveSurfaces.activeBindingHashes.end() ? bindingIt->second : 0ull);

		const bool sectorResponseEligible = IsEmissiveSurfaceSectorResponseEligible(surface);
		if (sectorResponseEligible)
		{
			const uint32_t sectorIndex = (uint32_t)surface.sectorIndex;
			bool applied = false;
			const float responseScale = ResolveSectorEmissionScale(surface, applied);
			const float intensityScale = applied ? ResolveSectorEmissionIntensityScale(surface, responseScale) : 1.0f;
			const float reachScale = applied ? ResolveSectorEmissionReachScale(surface, responseScale) : 1.0f;
			hash = nri_scene::HashCombine64(hash, (uint64_t)sectorIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(responseScale));
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(intensityScale));
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(reachScale));
		}
		if (IsEmissiveSurfaceMaterialResponseEligible(surface))
		{
			bool applied = false;
			const float materialScale = ResolveEmissiveMaterialResponseScale(surface, applied);
			hash = nri_scene::HashCombine64(hash, 0x4d415452455350ull);
			hash = nri_scene::HashCombine64(hash, (uint64_t)(uint32_t)surface.sectorIndex);
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(materialScale));
		}
	}

	return hash;
}
