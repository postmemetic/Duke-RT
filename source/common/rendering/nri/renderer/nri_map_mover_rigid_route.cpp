#include "nri_map_mover_rigid_route.h"

#include "nri_map_mover_shadow_state.h"
#include "nri_map_movers.h"
#include "nri_runtime_mutation.h"
#include "nri_static_scene.h"

#include "../scene/nri_map_mover_authored_topology.h"
#include "../scene/nri_map_world.h"
#include "../scene/nri_scene_surface_types.h"

#include "printf.h"

#include <algorithm>
#include <iterator>
#include <set>

namespace
{
	constexpr uint32_t SupportedBoundaryMask = NRIMapMoverShadowQuarantine_AdjacencyUnproven;
	constexpr uint32_t EmissiveMaterialMask =
		nri_scene::MaterialFlag_Fullbright | nri_scene::MaterialFlag_TintEmission;

	const nri_scene::PTMapChunk* FindChunk(
		const nri_scene::PTMapWorld& mapWorld,
		uint32_t chunkIndex)
	{
		const auto found = std::find_if(
			mapWorld.chunks.begin(),
			mapWorld.chunks.end(),
			[chunkIndex](const nri_scene::PTMapChunk& chunk)
			{
				return chunk.chunkIndex == chunkIndex;
			});
		return found == mapWorld.chunks.end() ? nullptr : &*found;
	}

	bool HasPortalOrEmissiveSurface(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk,
		bool& outHasPortal,
		bool& outHasEmissive)
	{
		outHasPortal = false;
		outHasEmissive = false;
		if (chunk.firstSurface > mapWorld.surfaces.size() ||
			chunk.surfaceCount > mapWorld.surfaces.size() - chunk.firstSurface)
		{
			return false;
		}

		for (uint32_t offset = 0; offset < chunk.surfaceCount; ++offset)
		{
			const nri_scene::PTMapSurface& surface = mapWorld.surfaces[chunk.firstSurface + offset];
			outHasPortal |= surface.kind == nri_scene::PTMapSurfaceKind::Portal ||
				(surface.surface.material.flags &
					(nri_scene::MaterialFlag_Portal | nri_scene::MaterialFlag_Mirror)) != 0;
			outHasEmissive |=
				(surface.surface.material.flags & EmissiveMaterialMask) != 0 ||
				surface.surface.material.emissiveSourceTexture != nullptr;
		}
		return true;
	}

	bool ViewHasEmissiveSurface(const nri_scene::SceneView& view)
	{
		const auto surfacesHaveEmission = [](const std::vector<nri_scene::SurfaceRef>& surfaces)
		{
			for (const nri_scene::SurfaceRef& surface : surfaces)
			{
				if ((surface.material.flags & EmissiveMaterialMask) != 0 ||
					surface.material.emissiveSourceTexture != nullptr)
				{
					return true;
				}
			}
			return false;
		};
		return surfacesHaveEmission(view.opaqueWalls) || surfacesHaveEmission(view.opaqueFlats);
	}

	bool HasExactWholeChunkOwnership(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk,
		const nri_scene::CanonicalLocalMapMoverGeometry& canonical)
	{
		if (!canonical.valid || chunk.firstSurface > mapWorld.surfaces.size() ||
			chunk.surfaceCount > mapWorld.surfaces.size() - chunk.firstSurface ||
			canonical.surfaces.size() != chunk.surfaceCount)
		{
			return false;
		}

		std::vector<nri_scene::MapMoverSurfaceProvenance> expected;
		std::vector<nri_scene::MapMoverSurfaceProvenance> captured;
		expected.reserve(chunk.surfaceCount);
		captured.reserve(canonical.surfaces.size());
		for (uint32_t offset = 0; offset < chunk.surfaceCount; ++offset)
		{
			const nri_scene::PTMapSurface& source = mapWorld.surfaces[chunk.firstSurface + offset];
			const nri_scene::SurfaceProvenance& provenance = source.surface.provenance;
			if ((provenance.mapChunkIndex >= 0 && (uint32_t)provenance.mapChunkIndex != chunk.chunkIndex) ||
				(provenance.sectorIndex >= 0 && provenance.sectorIndex != chunk.sectorIndex))
				return false;
			nri_scene::MapMoverSurfaceProvenance identity;
			identity.sourceType = (uint32_t)provenance.sourceType;
			identity.sectorIndex = provenance.sectorIndex;
			identity.wallIndex = provenance.wallIndex;
			identity.sectionIndex = provenance.sectionIndex;
			identity.surfaceKind = (uint32_t)source.kind;
			identity.stableSubSurfaceId = source.key.secondary;
			if ((source.kind == nri_scene::PTMapSurfaceKind::Floor || source.kind == nri_scene::PTMapSurfaceKind::Ceiling) &&
				(provenance.sectionIndex < 0 || source.key.primary != (uint32_t)provenance.sectionIndex))
				return false;
			if (source.kind != nri_scene::PTMapSurfaceKind::Floor && source.kind != nri_scene::PTMapSurfaceKind::Ceiling &&
				(provenance.wallIndex < 0 || source.key.primary != (uint32_t)provenance.wallIndex))
				return false;
			expected.push_back(identity);
		}
		for (const nri_scene::CanonicalLocalMapMoverSurface& surface : canonical.surfaces)
			captured.push_back(surface.provenance);
		std::sort(expected.begin(), expected.end());
		std::sort(captured.begin(), captured.end());
		return expected == captured &&
			std::adjacent_find(expected.begin(), expected.end()) == expected.end();
	}

	bool BuildPresentationTransform(
		const RuntimeMapMoverPose& pose,
		nri_scene::MapMoverLocalToWorldTransform& outTransform)
	{
		float ignoredRows[12] = {};
		return nri_scene::BuildPTMapMoverSceneTransformFromDukePose(
			pose.translation.X,
			pose.translation.Y,
			pose.translation.Z,
			pose.rotation.Radians(),
			outTransform,
			ignoredRows);
	}

	uint32_t FirstSetBit(uint32_t value)
	{
		for (uint32_t bit = 0; bit < 32; ++bit)
		{
			if ((value & (1u << bit)) != 0) return bit;
		}
		return 32;
	}
}

bool NRIMapMoverRigidRoute::ResidentFingerprint::operator==(const ResidentFingerprint& other) const
{
	return staticSceneChunkListIndex == other.staticSceneChunkListIndex &&
		vertexOffset == other.vertexOffset && vertexCount == other.vertexCount &&
		indexOffset == other.indexOffset && indexCount == other.indexCount &&
		primitiveOffset == other.primitiveOffset && primitiveCount == other.primitiveCount &&
		materialOffset == other.materialOffset && materialCount == other.materialCount &&
		baselineSignature == other.baselineSignature &&
		exactGeometrySignature == other.exactGeometrySignature &&
		geometryPayloadHash == other.geometryPayloadHash &&
		materialPayloadHash == other.materialPayloadHash &&
		chunkBlas == other.chunkBlas;
}

void NRIMapMoverRigidRoute::Update(const NRIMapMoverRigidRouteFrameInput& input)
{
	m_frameStats = {};
	m_frameStats.frameIndex = input.frameIndex;
	m_activeRoutes.clear();
	m_traceMode = input.traceMode;
	if (input.movers == nullptr || input.shadowState == nullptr || input.mapWorld == nullptr ||
		input.staticScene == nullptr || input.atlas == nullptr || input.registry == nullptr ||
		input.runtimeMutation == nullptr)
	{
		return;
	}

	const uint64_t buildSerial = input.mapWorld->buildSerial;
	const uint64_t mapEpoch = input.movers->GetMapEpoch();
	m_frameStats.buildSerial = buildSerial;
	m_frameStats.mapEpoch = mapEpoch;
	m_frameStats.shadowRecordCount = input.shadowState->GetRecordCount();
	if (m_buildSerial != buildSerial || m_mapEpoch != mapEpoch)
	{
		m_resources.clear();
		m_buildSerial = buildSerial;
		m_mapEpoch = mapEpoch;
	}

	if (input.mode < 2 || !input.mapWorld->valid ||
		input.movers->GetBuildSerial() != buildSerial ||
		!input.staticScene->valid || input.staticScene->buildSerial != buildSerial ||
		!input.atlas->valid || input.atlas->buildSerial != buildSerial ||
		!input.registry->valid || input.registry->buildSerial != buildSerial)
	{
		return;
	}

	std::set<NRIMapMoverShadowRecordKey> seenKeys;
	input.shadowState->ForEachRecord([&](const NRIMapMoverShadowRecord& shadowRecord)
	{
		seenKeys.insert(shadowRecord.key);
		const RuntimeMapMoverSnapshot* snapshot = input.movers->FindGroup(shadowRecord.key.stableGroupId);
		if (snapshot == nullptr || snapshot->effectorLotag != 15)
		{
			return;
		}
		m_frameStats.candidateCount++;

		const nri_scene::PTMapChunk* mapChunk = FindChunk(*input.mapWorld, shadowRecord.key.chunkIndex);
		const ResidentMapChunkRegistry::Entry* registryEntry =
			shadowRecord.key.chunkIndex < input.registry->entries.size() ?
			&input.registry->entries[shadowRecord.key.chunkIndex] : nullptr;
		const RuntimeMapMutationCache::ChunkReplacement* replacement =
			input.runtimeMutation->FindReplacement(shadowRecord.key.chunkIndex);

		const bool registryReady = registryEntry != nullptr && registryEntry->valid &&
			registryEntry->active && registryEntry->mappedInStaticScene &&
			registryEntry->accelerationResident &&
			!registryEntry->hasAnimatedTextureCandidates &&
			!registryEntry->animatedRefreshSuppressed &&
			registryEntry->staticSceneChunkListIndex < input.staticScene->chunks.size() &&
			registryEntry->staticSceneChunkListIndex < input.atlas->chunks.size();
		const uint32_t staticChunkListIndex = registryReady ?
			registryEntry->staticSceneChunkListIndex : UINT32_MAX;
		const StaticMapSceneCache::ChunkCache* staticChunk = registryReady ?
			&input.staticScene->chunks[staticChunkListIndex] : nullptr;
		const StaticMapChunkAtlas::ChunkEntry* atlasChunk = registryReady ?
			&input.atlas->chunks[staticChunkListIndex] : nullptr;
		const bool atlasReady = atlasChunk != nullptr && atlasChunk->valid &&
			atlasChunk->chunkIndex == shadowRecord.key.chunkIndex &&
			atlasChunk->vertexOffset == registryEntry->vertexOffset &&
			atlasChunk->vertexCount == registryEntry->vertexCount &&
			atlasChunk->indexOffset == registryEntry->indexOffset &&
			atlasChunk->indexCount == registryEntry->indexCount &&
			atlasChunk->primitiveOffset == registryEntry->primitiveOffset &&
			atlasChunk->primitiveCount == registryEntry->primitiveCount &&
			atlasChunk->materialOffset == registryEntry->materialOffset &&
			atlasChunk->materialCount == registryEntry->materialCount;
		const bool blasReady = staticChunk != nullptr && staticChunk->active &&
			staticChunk->accelerationStructure.accelerationStructure != nullptr;
		const bool replacementSettled = replacement != nullptr &&
			replacement->residentAuthoritative && !replacement->active && !replacement->valid &&
			!replacement->deferredMaterialRefresh && !replacement->deferredStructuralRebuild &&
			!replacement->excludeStaticChunk && registryEntry != nullptr &&
			replacement->baselineSignature == registryEntry->baselineSignature &&
			registryEntry->liveSignature == registryEntry->baselineSignature;

		bool hasPortal = true;
		bool hasEmissive = true;
		const bool validSurfaceRange = mapChunk != nullptr &&
			HasPortalOrEmissiveSurface(*input.mapWorld, *mapChunk, hasPortal, hasEmissive);
		if (staticChunkListIndex < input.staticScene->lightChunkViews.size())
		{
			hasEmissive |= ViewHasEmissiveSurface(input.staticScene->lightChunkViews[staticChunkListIndex]);
		}
		const bool wholeChunkOwnership = validSurfaceRange && mapChunk != nullptr &&
			HasExactWholeChunkOwnership(*input.mapWorld, *mapChunk, shadowRecord.canonical);

		ResidentFingerprint fingerprint;
		if (registryReady && atlasReady && blasReady && replacementSettled)
		{
			fingerprint.staticSceneChunkListIndex = staticChunkListIndex;
			fingerprint.vertexOffset = registryEntry->vertexOffset;
			fingerprint.vertexCount = registryEntry->vertexCount;
			fingerprint.indexOffset = registryEntry->indexOffset;
			fingerprint.indexCount = registryEntry->indexCount;
			fingerprint.primitiveOffset = registryEntry->primitiveOffset;
			fingerprint.primitiveCount = registryEntry->primitiveCount;
			fingerprint.materialOffset = registryEntry->materialOffset;
			fingerprint.materialCount = registryEntry->materialCount;
			fingerprint.baselineSignature = registryEntry->baselineSignature;
			fingerprint.exactGeometrySignature = registryEntry->exactGeometrySignature;
			fingerprint.geometryPayloadHash = registryEntry->geometryPayloadHash;
			fingerprint.materialPayloadHash = registryEntry->materialPayloadHash;
			fingerprint.chunkBlas = staticChunk->accelerationStructure.accelerationStructure;
		}

		auto retained = m_resources.find(shadowRecord.key);
		if (retained != m_resources.end() && !(retained->second.fingerprint == fingerprint))
		{
			m_resources.erase(retained);
			m_frameStats.residentDriftCount++;
			m_frameStats.rejectBitCounts[FirstSetBit(NRIMapMoverRigidRouteReject_IdentityMismatch)]++;
			return;
		}

		if (retained == m_resources.end() && registryReady && atlasReady && blasReady && replacementSettled)
		{
			RetainedResource resource;
			resource.fingerprint = fingerprint;
			resource.policyResource.key = shadowRecord.key;
			resource.policyResource.mapEpoch = mapEpoch;
			resource.policyResource.canonicalResourceKey = shadowRecord.canonical.resourceKey;
			resource.policyResource.capturedGenerations = shadowRecord.generations;
			resource.policyResource.capturedBaseTransform = shadowRecord.currentTransform;
			resource.policyResource.atlasResident = atlasReady;
			resource.policyResource.blasResident = blasReady;
			resource.policyResource.registryResident = registryReady;
			retained = m_resources.emplace(shadowRecord.key, std::move(resource)).first;
			m_frameStats.capturedResourceCount++;
		}

		nri_scene::MapMoverLocalToWorldTransform previousTransform;
		nri_scene::MapMoverLocalToWorldTransform currentTransform;
		const bool presentationValid =
			BuildPresentationTransform(snapshot->presentationPreviousPose, previousTransform) &&
			BuildPresentationTransform(snapshot->presentationCurrentPose, currentTransform);

		NRIMapMoverRigidRouteAdmissionInput admission;
		admission.snapshot = snapshot;
		admission.shadowRecord = &shadowRecord;
		admission.resource = retained != m_resources.end() ? &retained->second.policyResource : nullptr;
		admission.currentPreviousTransform = previousTransform;
		admission.currentCurrentTransform = currentTransform;
		admission.hasCurrentPresentationTransforms = presentationValid;
		admission.wholeChunkOwnershipProven = wholeChunkOwnership;
		admission.hasOverlappingOwner =
			(shadowRecord.quarantineMask & NRIMapMoverShadowQuarantine_OverlappingGeometryOwner) != 0;
		admission.hasPortalSurface = hasPortal;
		admission.hasEmissiveSurface = hasEmissive;
		admission.provenSafeBoundaryMask = wholeChunkOwnership ?
			(shadowRecord.quarantineMask & SupportedBoundaryMask) : 0u;
		NRIMapMoverRigidRouteAdmissionResult result =
			EvaluateNRIMapMoverRigidRouteAdmission(admission);

		for (uint32_t bit = 0; bit < m_frameStats.rejectBitCounts.size(); ++bit)
		{
			if ((result.rejectMask & (1u << bit)) != 0) m_frameStats.rejectBitCounts[bit]++;
		}
		if (!result.admitted) return;

		const auto inserted = m_activeRoutes.emplace(
			shadowRecord.key.chunkIndex,
			ActiveRoute{ shadowRecord.key, result.transforms });
		if (!inserted.second)
		{
			m_activeRoutes.erase(shadowRecord.key.chunkIndex);
			m_frameStats.rejectBitCounts[FirstSetBit(NRIMapMoverRigidRouteReject_OverlappingOwner)]++;
			return;
		}
		m_frameStats.admittedCount++;
	});

	for (auto retained = m_resources.begin(); retained != m_resources.end();)
	{
		retained = seenKeys.find(retained->first) == seenKeys.end() ?
			m_resources.erase(retained) : std::next(retained);
	}
	m_frameStats.bypassChunkCount = (uint32_t)m_activeRoutes.size();

	if (input.traceMode > 0)
	{
		Printf("PERF pt map mover rigid route NRI: frame=%llu mode=%d build_serial=%llu map_epoch=%llu shadow_records=%u candidates=%u admitted=%u retained=%u captured=%u resident_drift=%u bypass_chunks=%u publication=%s\n",
			(unsigned long long)input.frameIndex,
			input.mode,
			(unsigned long long)buildSerial,
			(unsigned long long)mapEpoch,
			m_frameStats.shadowRecordCount,
			m_frameStats.candidateCount,
			m_frameStats.admittedCount,
			(uint32_t)m_resources.size(),
			m_frameStats.capturedResourceCount,
			m_frameStats.residentDriftCount,
			m_frameStats.bypassChunkCount,
			m_activeRoutes.empty() ? "exact" : "rigid-route");
	}
}

bool NRIMapMoverRigidRoute::ShouldBypassExactChunk(uint32_t chunkIndex) const
{
	return m_activeRoutes.find(chunkIndex) != m_activeRoutes.end();
}

bool NRIMapMoverRigidRoute::PatchStaticInstances(
	std::vector<nri::TopLevelInstance>& tlasInstances,
	std::vector<SceneInstanceData>& sceneInstances)
{
	m_frameStats.patchedInstanceCount = 0;
	m_frameStats.patchMissCount = 0;
	if (tlasInstances.size() != sceneInstances.size())
	{
		m_frameStats.patchMissCount = (uint32_t)m_activeRoutes.size();
		return m_activeRoutes.empty();
	}

	for (const auto& routeEntry : m_activeRoutes)
	{
		const uint32_t chunkIndex = routeEntry.first;
		bool patched = false;
		for (size_t instanceIndex = 0; instanceIndex < sceneInstances.size(); ++instanceIndex)
		{
			SceneInstanceData& sceneInstance = sceneInstances[instanceIndex];
			if (sceneInstance.metadata0 != chunkIndex) continue;
			PatchNRIMapMoverRigidRouteInstance(
				routeEntry.second.transforms,
				tlasInstances[instanceIndex],
				sceneInstance);
			patched = true;
			m_frameStats.patchedInstanceCount++;
			break;
		}
		if (!patched) m_frameStats.patchMissCount++;
	}
	if (m_traceMode > 0 && !m_activeRoutes.empty())
	{
		Printf("PERF pt map mover rigid route patch NRI: frame=%llu active=%u patched=%u misses=%u atlas_bytes=0 material_rewrites=0 blas_commands=0 tlas_transform_patches=%u\n",
			(unsigned long long)m_frameStats.frameIndex,
			(uint32_t)m_activeRoutes.size(),
			m_frameStats.patchedInstanceCount,
			m_frameStats.patchMissCount,
			m_frameStats.patchedInstanceCount);
	}
	return m_frameStats.patchMissCount == 0;
}

void NRIMapMoverRigidRoute::Reset()
{
	m_resources.clear();
	m_activeRoutes.clear();
	m_frameStats = {};
	m_buildSerial = 0;
	m_mapEpoch = 0;
	m_traceMode = 0;
}
