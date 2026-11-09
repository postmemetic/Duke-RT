#include "nri_static_scene.h"

#include "nri_runtime_mutation.h"
#include "perf_audit.h"
#include "printf.h"

#include <algorithm>
#include <cstring>
#include <vector>
uint32_t NRIStaticSceneResidency::GetStaticSceneChunkSlotPreference(
	const StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas,
	uint32_t chunkListIndex)
{
	if (chunkListIndex >= staticScene.chunks.size())
	{
		return 0;
	}

	const auto& chunk = staticScene.chunks[chunkListIndex];
	uint32_t score = 0;
	if (chunk.active)
	{
		score += 8u;
	}
	if (chunk.accelerationStructure.accelerationStructure != nullptr)
	{
		score += 4u;
	}
	if (chunk.primitiveCount > 0 && chunk.materialCount > 0)
	{
		score += 2u;
	}
	if (atlas.valid &&
		chunkListIndex < atlas.chunks.size() &&
		atlas.chunks[chunkListIndex].valid)
	{
		score += 1u;
	}
	return score;
}

namespace
{
struct RegistryAudit
{
	PerfAuditScope timer{ PerfAuditMetric::StaticRegistrySyncNs };
	bool enabled = PerfAuditActive();
	uint64_t copies = 0, bytes = 0, hashes = 0, rows = 0, refreshed = 0, preserved = 0;
	~RegistryAudit()
	{
		if (!enabled) return;
		PerfAuditAdd(PerfAuditMetric::StaticRegistryBaselineCopies, copies);
		PerfAuditAdd(PerfAuditMetric::StaticRegistryBaselineBytes, bytes);
		PerfAuditAdd(PerfAuditMetric::StaticRegistryMaterialHashes, hashes);
		PerfAuditAdd(PerfAuditMetric::StaticRegistryMaterialRowsHashed, rows);
		PerfAuditAdd(PerfAuditMetric::StaticRegistryEntriesRefreshed, refreshed);
		PerfAuditAdd(PerfAuditMetric::StaticRegistryHistoryPreserved, preserved);
	}
	void Copy(const nri_scene::PTMapChunkMutationBaseline& baseline)
	{
		++copies;
		bytes += baseline.walls.size() * sizeof(nri_scene::PTMapWallMutationSnapshot) +
			baseline.sectionIndices.size() * sizeof(int32_t);
	}
	void Hash(const nri_scene::MaterialBridgeData& bridge) { ++hashes; rows += bridge.materials.size(); }
};

RuntimeMutationResidentReplacementInfo GetReplacement(const NRIStaticSceneRegistrySyncInput& input, uint32_t index)
{
	return input.getReplacement != nullptr ? input.getReplacement(input.replacementOwner, index) : RuntimeMutationResidentReplacementInfo{};
}

// Independent full reconstruction remains the diagnostic oracle and recovery
// path. It intentionally does not consult any optimized payload stamps.
ResidentMapChunkRegistry BuildFullRegistry(const NRIStaticSceneRegistrySyncInput& input, RegistryAudit* audit)
{
	const nri_scene::PTMapWorld& mapWorld = *input.mapWorld;
	const StaticMapSceneCache& staticScene = *input.staticScene;
	const StaticMapChunkAtlas& atlas = *input.atlas;
	ResidentMapChunkRegistry registry;
	registry.valid = true;
	registry.buildSerial = mapWorld.buildSerial;
	registry.chunkCount = (uint32_t)mapWorld.chunks.size();
	registry.entries.resize(mapWorld.chunks.size());

	for (size_t chunkListIndex = 0; chunkListIndex < mapWorld.chunks.size(); ++chunkListIndex)
	{
		const auto& mapChunk = mapWorld.chunks[chunkListIndex];
		auto& entry = registry.entries[chunkListIndex];
		entry.valid = true;
		entry.chunkIndex = mapChunk.chunkIndex;
		const auto replacement = GetReplacement(input, (uint32_t)chunkListIndex);
		if (replacement.baseline != nullptr)
		{
			entry.appliedBaseline = *replacement.baseline;
			entry.baselineSignature = replacement.baselineSignature;
			entry.liveSignature = replacement.liveSignature != 0 ? replacement.liveSignature : replacement.baselineSignature;
			if (audit) audit->Copy(*replacement.baseline);
		}
	}

	const bool atlasMatchesStaticScene =
		atlas.valid &&
		atlas.buildSerial == staticScene.buildSerial &&
		atlas.chunks.size() == staticScene.chunks.size();
	std::vector<int32_t> bestSlotScores(registry.entries.size(), -1);
	for (size_t chunkListIndex = 0; chunkListIndex < staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& staticChunk = staticScene.chunks[chunkListIndex];
		if (staticChunk.chunkIndex >= registry.entries.size())
		{
			continue;
		}

		const int32_t candidateScore = (int32_t)NRIStaticSceneResidency::GetStaticSceneChunkSlotPreference(staticScene, atlas, (uint32_t)chunkListIndex);
		auto& entry = registry.entries[staticChunk.chunkIndex];
		if (candidateScore < bestSlotScores[staticChunk.chunkIndex])
		{
			continue;
		}
		if (candidateScore == bestSlotScores[staticChunk.chunkIndex] &&
			entry.staticSceneChunkListIndex != UINT32_MAX &&
			entry.staticSceneChunkListIndex > chunkListIndex)
		{
			continue;
		}

		bestSlotScores[staticChunk.chunkIndex] = candidateScore;
		entry.staticSceneChunkListIndex = (uint32_t)chunkListIndex;
		entry.active = staticChunk.active;
		entry.mappedInStaticScene = staticChunk.active;
		if (atlasMatchesStaticScene &&
			chunkListIndex < atlas.chunks.size() &&
			atlas.chunks[chunkListIndex].valid)
		{
			const auto& atlasChunk = atlas.chunks[chunkListIndex];
			entry.vertexOffset = atlasChunk.vertexOffset;
			entry.vertexCount = atlasChunk.vertexCount;
			entry.indexOffset = atlasChunk.indexOffset;
			entry.indexCount = atlasChunk.indexCount;
			entry.primitiveOffset = atlasChunk.primitiveOffset;
			entry.primitiveCount = atlasChunk.primitiveCount;
			entry.materialOffset = atlasChunk.materialOffset;
			entry.materialCount = atlasChunk.materialCount;
		}
		else
		{
			entry.vertexOffset = staticChunk.vertexOffset;
			entry.vertexCount = staticChunk.vertexCount;
			entry.indexOffset = staticChunk.indexOffset;
			entry.indexCount = staticChunk.indexCount;
			entry.primitiveOffset = staticChunk.primitiveOffset;
			entry.primitiveCount = staticChunk.primitiveCount;
			entry.materialOffset = staticChunk.materialOffset;
			entry.materialCount = staticChunk.materialCount;
		}
		entry.geometryTopologySignature = staticChunk.geometryTopologySignature;
		entry.animatedMaterialSignature = staticChunk.animatedMaterialSignature;
		entry.materialPayloadHash =
			staticChunk.active && input.hashResidentMaterialPayload != nullptr ?
			input.hashResidentMaterialPayload(staticChunk.materialBridge) :
			0;
		if (audit && staticChunk.active && input.hashResidentMaterialPayload != nullptr) audit->Hash(staticChunk.materialBridge);
		entry.geometryPayloadHash = staticChunk.active ? staticChunk.geometryPayloadHash : 0;
		entry.animatedGeometrySignature = staticChunk.animatedGeometrySignature;
		entry.exactGeometrySignature = staticChunk.exactGeometrySignature;
		entry.hasAnimatedTextureCandidates = staticChunk.hasAnimatedTextureCandidates;
		entry.animatedRefreshSuppressed = staticChunk.animatedRefreshSuppressed;
		entry.accelerationResident = staticChunk.active && staticChunk.accelerationStructure.accelerationStructure != nullptr;

		if (entry.active)
		{
			registry.activeChunkCount++;
			registry.mappedChunkCount++;
		}
		if (entry.accelerationResident)
		{
			registry.accelerationResidentChunkCount++;
		}
		if (entry.hasAnimatedTextureCandidates)
		{
			registry.animatedCandidateChunkCount++;
		}
		if (entry.animatedRefreshSuppressed)
		{
			registry.animatedRefreshSuppressedChunkCount++;
		}
	}
	if (audit) audit->refreshed += registry.entries.size();
	return registry;
}

template<class T> bool SameBits(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

bool SameBaseline(const nri_scene::PTMapChunkMutationBaseline& a, const nri_scene::PTMapChunkMutationBaseline& b)
{
#define REGISTRY_FIELD(name) if (!SameBits(a.name, b.name)) return false
	REGISTRY_FIELD(sectorIndex); REGISTRY_FIELD(signature); REGISTRY_FIELD(fluidNormalSignature);
	REGISTRY_FIELD(floorz); REGISTRY_FIELD(ceilingz); REGISTRY_FIELD(floorstat); REGISTRY_FIELD(ceilingstat);
	REGISTRY_FIELD(floorheinum); REGISTRY_FIELD(ceilingheinum); REGISTRY_FIELD(portalflags);
	REGISTRY_FIELD(floortexture); REGISTRY_FIELD(ceilingtexture); REGISTRY_FIELD(floorxpan); REGISTRY_FIELD(floorypan);
	REGISTRY_FIELD(ceilingxpan); REGISTRY_FIELD(ceilingypan); REGISTRY_FIELD(floorpal); REGISTRY_FIELD(ceilingpal);
	REGISTRY_FIELD(floorshade); REGISTRY_FIELD(ceilingshade);
#undef REGISTRY_FIELD
	if (a.sectionIndices != b.sectionIndices || a.walls.size() != b.walls.size()) return false;
	for (size_t i = 0; i < a.walls.size(); ++i)
	{
		const auto& x = a.walls[i]; const auto& y = b.walls[i];
#define REGISTRY_FIELD(name) if (!SameBits(x.name, y.name)) return false
		REGISTRY_FIELD(pos.X); REGISTRY_FIELD(pos.Y); REGISTRY_FIELD(point2); REGISTRY_FIELD(nextwall); REGISTRY_FIELD(nextsector);
		REGISTRY_FIELD(cstat); REGISTRY_FIELD(portalflags); REGISTRY_FIELD(walltexture); REGISTRY_FIELD(overtexture);
		REGISTRY_FIELD(xpan); REGISTRY_FIELD(ypan); REGISTRY_FIELD(xrepeat); REGISTRY_FIELD(yrepeat);
		REGISTRY_FIELD(pal); REGISTRY_FIELD(shade); REGISTRY_FIELD(adjacentFloorz); REGISTRY_FIELD(adjacentCeilingz);
		REGISTRY_FIELD(adjacentFloorstat); REGISTRY_FIELD(adjacentCeilingstat); REGISTRY_FIELD(adjacentFloorheinum);
		REGISTRY_FIELD(adjacentCeilingheinum); REGISTRY_FIELD(adjacentPortalflags); REGISTRY_FIELD(nextWallCstat);
		REGISTRY_FIELD(nextWallTexture); REGISTRY_FIELD(nextOverTexture); REGISTRY_FIELD(nextWallPal); REGISTRY_FIELD(nextWallShade);
#undef REGISTRY_FIELD
	}
	return true;
}

bool SamePublishedEntry(const ResidentMapChunkRegistry::Entry& a, const ResidentMapChunkRegistry::Entry& b)
{
#define REGISTRY_FIELD(name) if (a.name != b.name) return false
	REGISTRY_FIELD(chunkIndex); REGISTRY_FIELD(staticSceneChunkListIndex);
	REGISTRY_FIELD(vertexOffset); REGISTRY_FIELD(vertexCount); REGISTRY_FIELD(indexOffset); REGISTRY_FIELD(indexCount);
	REGISTRY_FIELD(primitiveOffset); REGISTRY_FIELD(primitiveCount); REGISTRY_FIELD(materialOffset); REGISTRY_FIELD(materialCount);
	REGISTRY_FIELD(geometryTopologySignature); REGISTRY_FIELD(exactGeometrySignature); REGISTRY_FIELD(animatedMaterialSignature);
	REGISTRY_FIELD(materialPayloadHash); REGISTRY_FIELD(geometryPayloadHash); REGISTRY_FIELD(animatedGeometrySignature);
	REGISTRY_FIELD(valid); REGISTRY_FIELD(active); REGISTRY_FIELD(mappedInStaticScene); REGISTRY_FIELD(accelerationResident);
	REGISTRY_FIELD(hasAnimatedTextureCandidates); REGISTRY_FIELD(animatedRefreshSuppressed);
#undef REGISTRY_FIELD
	return true;
}

bool SameRegistryPayload(const ResidentMapChunkRegistry& a, const ResidentMapChunkRegistry& b)
{
#define REGISTRY_FIELD(name) if (a.name != b.name) return false
	REGISTRY_FIELD(valid); REGISTRY_FIELD(buildSerial); REGISTRY_FIELD(chunkCount); REGISTRY_FIELD(activeChunkCount);
	REGISTRY_FIELD(mappedChunkCount); REGISTRY_FIELD(accelerationResidentChunkCount); REGISTRY_FIELD(animatedCandidateChunkCount);
	REGISTRY_FIELD(animatedRefreshSuppressedChunkCount);
#undef REGISTRY_FIELD
	if (a.entries.size() != b.entries.size()) return false;
	for (size_t i = 0; i < a.entries.size(); ++i)
	{
		const auto& x = a.entries[i]; const auto& y = b.entries[i];
		if (!SamePublishedEntry(x, y) || x.baselineSignature != y.baselineSignature || x.liveSignature != y.liveSignature ||
			!SameBaseline(x.appliedBaseline, y.appliedBaseline)) return false;
	}
	return true;
}
}

void NRIStaticSceneResidency::ResetResidentMapChunkRegistry()
{
	mResidentMapChunkRegistry = {};
	mRegistryPayloadStamps.clear();
	mRegistryPreferredSlots.clear();
	mRegistrySlotScores.clear();
	mRegistryContentBuildSerial = 0;
	mRegistryReplacementOwner = nullptr;
	mRegistryQuarantined = false;
}

void NRIStaticSceneResidency::SyncResidentMapChunkRegistryFromStaticScene(const NRIStaticSceneRegistrySyncInput& input)
{
	RegistryAudit audit;
	if (audit.enabled)
	{
		PerfAuditAdd(PerfAuditMetric::StaticRegistrySyncCalls);
		if (input.reason == NRIStaticSceneRegistrySyncReason::RuntimeCommit) PerfAuditAdd(PerfAuditMetric::StaticRegistryRuntimeCalls);
		if (input.reason == NRIStaticSceneRegistrySyncReason::AnimatedMaterial) PerfAuditAdd(PerfAuditMetric::StaticRegistryAnimatedCalls);
	}
	if (input.mapWorld == nullptr || !input.mapWorld->valid || input.staticScene == nullptr || input.atlas == nullptr)
	{
		ResetResidentMapChunkRegistry();
		return;
	}
	const auto& world = *input.mapWorld;
	const auto& scene = *input.staticScene;
	const auto& atlas = *input.atlas;
	auto& registry = mResidentMapChunkRegistry;
	const bool full = mRegistryQuarantined || !registry.valid || registry.buildSerial != world.buildSerial ||
		registry.entries.size() != world.chunks.size() || mRegistryPayloadStamps.size() != world.chunks.size() ||
		mRegistryContentBuildSerial != scene.contentBuildSerial || mRegistryReplacementOwner != input.replacementOwner ||
		input.reason == NRIStaticSceneRegistrySyncReason::FullBuild || input.reason == NRIStaticSceneRegistrySyncReason::AccelerationReset;
	if (audit.enabled) PerfAuditAdd(PerfAuditMetric::StaticRegistryEntriesVisited, world.chunks.size());
	if (full)
	{
		if (audit.enabled) PerfAuditAdd(PerfAuditMetric::StaticRegistryFullSyncs);
		registry = BuildFullRegistry(input, &audit);
		mRegistryPayloadStamps.assign(world.chunks.size(), {});
		for (uint32_t i = 0; i < registry.entries.size(); ++i)
		{
			auto& stamp = mRegistryPayloadStamps[i];
			stamp.baselineRevision = GetReplacement(input, i).revision;
			const auto& entry = registry.entries[i];
			stamp.slot = entry.staticSceneChunkListIndex;
			if (entry.active && stamp.slot < scene.chunks.size() && input.hashResidentMaterialPayload != nullptr)
			{
				stamp.materialValid = true;
				stamp.materialGeneration = scene.chunks[stamp.slot].lightGeneration;
				stamp.materialHash = entry.materialPayloadHash;
			}
		}
		mRegistryContentBuildSerial = scene.contentBuildSerial;
		mRegistryReplacementOwner = input.replacementOwner;
		return;
	}

	// Cheap scalar discovery remains O(map chunks + static slots), including
	// direct pre-commit writers and no-upload baseline updates. Payload work is
	// revision driven; upload-dirty lists alone are not complete authority here.
	mRegistryPreferredSlots.assign(world.chunks.size(), UINT32_MAX);
	mRegistrySlotScores.assign(world.chunks.size(), -1);
	registry.activeChunkCount = registry.mappedChunkCount = registry.accelerationResidentChunkCount = 0;
	registry.animatedCandidateChunkCount = registry.animatedRefreshSuppressedChunkCount = 0;
	for (uint32_t slot = 0; slot < scene.chunks.size(); ++slot)
	{
		const auto& chunk = scene.chunks[slot];
		if (chunk.chunkIndex >= mRegistryPreferredSlots.size()) continue;
		const int32_t score = (int32_t)GetStaticSceneChunkSlotPreference(scene, atlas, slot);
		if (score < mRegistrySlotScores[chunk.chunkIndex]) continue;
		mRegistrySlotScores[chunk.chunkIndex] = score;
		mRegistryPreferredSlots[chunk.chunkIndex] = slot;
		// Preserve legacy aggregate semantics for successively preferred duplicate
		// slots. Changing diagnostic count semantics is independent of this cache.
		registry.activeChunkCount += chunk.active;
		registry.mappedChunkCount += chunk.active;
		registry.accelerationResidentChunkCount += chunk.active && chunk.accelerationStructure.accelerationStructure != nullptr;
		registry.animatedCandidateChunkCount += chunk.hasAnimatedTextureCandidates;
		registry.animatedRefreshSuppressedChunkCount += chunk.animatedRefreshSuppressed;
	}
	const bool atlasMatches = atlas.valid && atlas.buildSerial == scene.buildSerial && atlas.chunks.size() == scene.chunks.size();
	for (uint32_t i = 0; i < world.chunks.size(); ++i)
	{
		auto& old = registry.entries[i];
		auto& stamp = mRegistryPayloadStamps[i];
		ResidentMapChunkRegistry::Entry next;
		next.valid = true;
		next.chunkIndex = world.chunks[i].chunkIndex;
		const auto replacement = GetReplacement(input, i);
		next.appliedBaseline = std::move(old.appliedBaseline);
		if (replacement.baseline != nullptr)
		{
			if (replacement.revision == 0 || stamp.baselineRevision != replacement.revision)
			{
				next.appliedBaseline = *replacement.baseline;
				audit.Copy(*replacement.baseline);
			}
			next.baselineSignature = replacement.baselineSignature;
			next.liveSignature = replacement.liveSignature != 0 ? replacement.liveSignature : replacement.baselineSignature;
		}
		else next.appliedBaseline = {};
		stamp.baselineRevision = replacement.revision;
		const uint32_t slot = mRegistryPreferredSlots[i];
		bool materialSourceUnchanged = true;
		if (slot < scene.chunks.size())
		{
			const auto& chunk = scene.chunks[slot];
			next.staticSceneChunkListIndex = slot;
			next.active = next.mappedInStaticScene = chunk.active;
			if (atlasMatches && atlas.chunks[slot].valid)
			{
				const auto& range = atlas.chunks[slot];
				next.vertexOffset = range.vertexOffset; next.vertexCount = range.vertexCount;
				next.indexOffset = range.indexOffset; next.indexCount = range.indexCount;
				next.primitiveOffset = range.primitiveOffset; next.primitiveCount = range.primitiveCount;
				next.materialOffset = range.materialOffset; next.materialCount = range.materialCount;
			}
			else
			{
				next.vertexOffset = chunk.vertexOffset; next.vertexCount = chunk.vertexCount;
				next.indexOffset = chunk.indexOffset; next.indexCount = chunk.indexCount;
				next.primitiveOffset = chunk.primitiveOffset; next.primitiveCount = chunk.primitiveCount;
				next.materialOffset = chunk.materialOffset; next.materialCount = chunk.materialCount;
			}
			next.geometryTopologySignature = chunk.geometryTopologySignature;
			next.animatedMaterialSignature = chunk.animatedMaterialSignature;
			next.geometryPayloadHash = chunk.active ? chunk.geometryPayloadHash : 0;
			next.animatedGeometrySignature = chunk.animatedGeometrySignature;
			next.exactGeometrySignature = chunk.exactGeometrySignature;
			next.hasAnimatedTextureCandidates = chunk.hasAnimatedTextureCandidates;
			next.animatedRefreshSuppressed = chunk.animatedRefreshSuppressed;
			next.accelerationResident = chunk.active && chunk.accelerationStructure.accelerationStructure != nullptr;
			if (chunk.active && input.hashResidentMaterialPayload != nullptr)
			{
				// Active chunk bridge publication sites increment lightGeneration;
				// contentBuildSerial and slot identity cover rebuild/replacement.
				materialSourceUnchanged = stamp.materialValid && stamp.slot == slot && stamp.materialGeneration == chunk.lightGeneration;
				if (!materialSourceUnchanged)
				{
					stamp.materialHash = input.hashResidentMaterialPayload(chunk.materialBridge);
					audit.Hash(chunk.materialBridge);
				}
				next.materialPayloadHash = stamp.materialHash;
				stamp.materialValid = true;
				stamp.materialGeneration = chunk.lightGeneration;
			}
			else stamp.materialValid = false;
		}
		else stamp.materialValid = false;
		stamp.slot = slot;
		if (materialSourceUnchanged && SamePublishedEntry(old, next))
		{
			next.wasVisibleLastFrame = old.wasVisibleLastFrame;
			next.visibleValidationTraceEmitted = old.visibleValidationTraceEmitted;
			next.visibleValidationFramesRemaining = old.visibleValidationFramesRemaining;
			next.animatedSuppressionEmitCount = old.animatedSuppressionEmitCount;
			next.runtimeAnimatedAttemptCount = old.runtimeAnimatedAttemptCount;
			next.runtimeAnimatedResidentApplyCount = old.runtimeAnimatedResidentApplyCount;
			next.runtimeAnimatedSyncSkipCount = old.runtimeAnimatedSyncSkipCount;
			++audit.preserved;
		}
		else ++audit.refreshed;
		old = std::move(next);
	}
	if (input.validate)
	{
		ResidentMapChunkRegistry reference = BuildFullRegistry(input, nullptr);
		if (audit.enabled) PerfAuditAdd(PerfAuditMetric::StaticRegistryValidationChecks);
		if (!SameRegistryPayload(registry, reference))
		{
			if (audit.enabled) PerfAuditAdd(PerfAuditMetric::StaticRegistryValidationMismatches);
			Printf("NRI PT static registry validation: mismatch=1 quarantine=1 build=%llu content=%llu\n",
				(unsigned long long)world.buildSerial, (unsigned long long)scene.contentBuildSerial);
			registry = std::move(reference);
			mRegistryQuarantined = true;
			mRegistryPayloadStamps.clear();
		}
	}
}

uint32_t NRIStaticSceneResidency::FindPreferredStaticSceneChunkListIndex(
	const StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas,
	uint32_t chunkIndex)
{
	uint32_t bestChunkListIndex = UINT32_MAX;
	uint32_t bestScore = 0;
	for (uint32_t chunkListIndex = 0; chunkListIndex < (uint32_t)staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& chunk = staticScene.chunks[chunkListIndex];
		if (chunk.chunkIndex != chunkIndex)
		{
			continue;
		}

		const uint32_t score = GetStaticSceneChunkSlotPreference(staticScene, atlas, chunkListIndex);
		if (bestChunkListIndex == UINT32_MAX ||
			score > bestScore ||
			(score == bestScore && chunkListIndex > bestChunkListIndex))
		{
			bestChunkListIndex = chunkListIndex;
			bestScore = score;
		}
	}

	return bestChunkListIndex;
}

uint32_t NRIStaticSceneResidency::CountStaticSceneChunkSlots(
	const StaticMapSceneCache& staticScene,
	uint32_t chunkIndex)
{
	uint32_t count = 0;
	for (const auto& chunk : staticScene.chunks)
	{
		if (chunk.chunkIndex == chunkIndex)
		{
			count++;
		}
	}
	return count;
}

NRIStaticSceneResidency::ChunkDiagnosticFacts NRIStaticSceneResidency::BuildChunkDiagnosticFacts(
	const StaticMapSceneCache& staticScene,
	const StaticMapChunkAtlas& atlas,
	uint32_t chunkIndex)
{
	ChunkDiagnosticFacts facts = {};
	uint32_t bestScore = 0;
	for (uint32_t chunkListIndex = 0; chunkListIndex < (uint32_t)staticScene.chunks.size(); ++chunkListIndex)
	{
		const auto& chunk = staticScene.chunks[chunkListIndex];
		if (chunk.chunkIndex != chunkIndex)
		{
			continue;
		}

		facts.duplicateChunkSlotCount++;
		const uint32_t score = GetStaticSceneChunkSlotPreference(staticScene, atlas, chunkListIndex);
		if (facts.preferredChunkListIndex == UINT32_MAX ||
			score > bestScore ||
			(score == bestScore && chunkListIndex > facts.preferredChunkListIndex))
		{
			facts.preferredChunkListIndex = chunkListIndex;
			bestScore = score;
		}
	}

	if (facts.preferredChunkListIndex >= staticScene.chunks.size())
	{
		return facts;
	}

	const auto& staticChunk = staticScene.chunks[facts.preferredChunkListIndex];
	facts.hasStaticChunk = true;
	facts.residentStatic = true;
	facts.staticTlasInstanced = staticChunk.active && staticChunk.accelerationStructure.accelerationStructure != nullptr;
	facts.staticProbeIncluded = staticChunk.active;
	facts.staticPrimitiveOffset = staticChunk.primitiveOffset;
	facts.staticPrimitiveCount = staticChunk.primitiveCount;
	facts.staticMaterialOffset = staticChunk.materialOffset;
	facts.staticMaterialCount = staticChunk.materialCount;
	facts.staticAsReady = staticChunk.accelerationStructure.accelerationStructure != nullptr;
	return facts;
}
