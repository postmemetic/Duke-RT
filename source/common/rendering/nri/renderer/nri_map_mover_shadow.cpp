#include "nri_map_mover_shadow.h"

#include "nri_map_movers.h"
#include "nri_static_scene_geometry.h"

#include "../scene/nri_map_builder.h"
#include "../scene/nri_map_mover_adapter.h"
#include "../scene/nri_map_mover_authored_topology.h"

#include "build.h"
#include "gametexture.h"
#include "hw_sections.h"
#include "printf.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <vector>

namespace
{
	using namespace nri_scene;

	const PTMapChunk* FindChunk(const PTMapWorld& mapWorld, uint32_t chunkIndex)
	{
		const auto found = std::find_if(mapWorld.chunks.begin(), mapWorld.chunks.end(),
			[chunkIndex](const PTMapChunk& chunk) { return chunk.chunkIndex == chunkIndex; });
		return found == mapWorld.chunks.end() ? nullptr : &*found;
	}

	bool IsPlaneKind(uint32_t surfaceKind)
	{
		return surfaceKind == (uint32_t)PTMapSurfaceKind::Floor ||
			surfaceKind == (uint32_t)PTMapSurfaceKind::Ceiling;
	}

	bool IsWallKind(uint32_t surfaceKind)
	{
		return surfaceKind >= (uint32_t)PTMapSurfaceKind::WallOneSided &&
			surfaceKind <= (uint32_t)PTMapSurfaceKind::Portal;
	}

	bool MemberOwnsGeometry(const RuntimeMapMoverMember& member)
	{
		const uint32_t geometryFlags = RuntimeMapMoverMember_OwnsWalls |
			RuntimeMapMoverMember_OwnsFloor | RuntimeMapMoverMember_OwnsCeiling;
		return (member.flags & RuntimeMapMoverMember_ControlOnly) == 0 &&
			(member.flags & geometryFlags) != 0;
	}

	bool SurfaceIsOwned(uint32_t surfaceKind, uint32_t memberFlags)
	{
		if (surfaceKind == (uint32_t)PTMapSurfaceKind::Floor)
			return (memberFlags & RuntimeMapMoverMember_OwnsFloor) != 0;
		if (surfaceKind == (uint32_t)PTMapSurfaceKind::Ceiling)
			return (memberFlags & RuntimeMapMoverMember_OwnsCeiling) != 0;
		return IsWallKind(surfaceKind) && (memberFlags & RuntimeMapMoverMember_OwnsWalls) != 0;
	}

	bool BuildChunkView(
		const PTMapWorld& liveWorld,
		const PTMapChunk& liveChunk,
		uint32_t memberFlags,
		PTMapMoverChunkView& outView,
		bool& outHasPortal)
	{
		outView = {};
		outHasPortal = false;
		outView.chunkIndex = liveChunk.chunkIndex;
		outView.sectorIndex = liveChunk.sectorIndex;
		if (liveChunk.firstSurface > liveWorld.surfaces.size() ||
			liveChunk.surfaceCount > liveWorld.surfaces.size() - liveChunk.firstSurface)
		{
			return false;
		}
		for (uint32_t surfaceOffset = 0; surfaceOffset < liveChunk.surfaceCount; ++surfaceOffset)
		{
			const PTMapSurface& source = liveWorld.surfaces[liveChunk.firstSurface + surfaceOffset];
			const uint32_t surfaceKind = (uint32_t)source.kind;
			if (!SurfaceIsOwned(surfaceKind, memberFlags)) continue;
			outView.surfaces.push_back({
				&source.surface,
				surfaceKind,
				source.key.primary,
				source.key.secondary,
				source.chunkIndex });
			outHasPortal |= source.kind == PTMapSurfaceKind::Portal ||
				(source.surface.material.flags & (MaterialFlag_Portal | MaterialFlag_Mirror)) != 0;
		}
		return !outView.surfaces.empty();
	}

	const Section* FindSourceSection(int32_t sectionIndex)
	{
		for (const Section& section : sections)
		{
			if ((int32_t)section.index == sectionIndex) return &section;
		}
		return nullptr;
	}

	void BuildExplicitAuthoredSources(
		const PTMapMoverChunkView& chunkView,
		std::vector<PTMapMoverSourceWall>& outWalls,
		std::vector<PTMapMoverSourceSection>& outSections,
		std::vector<PTMapMoverSourceSectionLine>& outSectionLines)
	{
		outWalls.clear();
		outSections.clear();
		outSectionLines.clear();
		std::set<int32_t> requiredWalls;
		std::set<int32_t> requiredSections;
		for (const PTMapMoverSurfaceView& surface : chunkView.surfaces)
		{
			if (IsPlaneKind(surface.surfaceKind) && surface.keyPrimary <= INT32_MAX)
				requiredSections.insert((int32_t)surface.keyPrimary);
			else if (IsWallKind(surface.surfaceKind) && surface.keyPrimary <= INT32_MAX)
				requiredWalls.insert((int32_t)surface.keyPrimary);
		}

		for (int32_t sectionIndex : requiredSections)
		{
			const Section* source = FindSourceSection(sectionIndex);
			if (source == nullptr) continue;
			PTMapMoverSourceSection sectionSource;
			sectionSource.sectionIndex = sectionIndex;
			for (int32_t lineIndex : source->lines)
			{
				sectionSource.lineIndices.push_back(lineIndex);
				if (lineIndex < 0 || (unsigned)lineIndex >= sectionLines.Size()) continue;
				const SectionLine& line = sectionLines[(unsigned)lineIndex];
				outSectionLines.push_back({ lineIndex, line.section, line.startpoint, line.endpoint });
				requiredWalls.insert(line.startpoint);
				requiredWalls.insert(line.endpoint);
			}
			outSections.push_back(std::move(sectionSource));
		}

		std::vector<int32_t> endpointWalls;
		for (int32_t wallIndex : requiredWalls)
		{
			if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size()) continue;
			endpointWalls.push_back(wall[(unsigned)wallIndex].point2);
		}
		for (int32_t endpoint : endpointWalls) requiredWalls.insert(endpoint);

		for (int32_t wallIndex : requiredWalls)
		{
			if (wallIndex < 0 || (unsigned)wallIndex >= wall.Size()) continue;
			const walltype& source = wall[(unsigned)wallIndex];
			PTMapMoverSourceWall wallSource;
			wallSource.wallIndex = wallIndex;
			wallSource.point2Index = source.point2;
			wallSource.position[0] = source.pos.X;
			wallSource.position[1] = source.pos.Y;
			outWalls.push_back(wallSource);
		}
	}

	bool ResolveTextureIdentity(const FGameTexture* texture, uint64_t& outStableIdentity, void*)
	{
		if (texture == nullptr)
		{
			outStableIdentity = 0;
			return true;
		}
		const FTextureID id = texture->GetID();
		if (!id.isValid()) return false;
		outStableIdentity = (uint64_t)(uint32_t)id.GetIndex() + 1ull;
		return true;
	}

	uint32_t BuildOwnershipQuarantine(
		const NRIMapMoverSystem& movers,
		const RuntimeMapMoverSnapshot& snapshot,
		const RuntimeMapMoverMember& member,
		bool hasPortal)
	{
		uint32_t quarantine = NRIMapMoverShadowQuarantine_None;
		if (snapshot.capability == RuntimeMapMoverCapability::Unknown)
			quarantine |= NRIMapMoverShadowQuarantine_UnknownCapability;
		if ((member.flags & RuntimeMapMoverMember_AdjacencyUnproven) != 0)
			quarantine |= NRIMapMoverShadowQuarantine_AdjacencyUnproven;
		if (hasPortal) quarantine |= NRIMapMoverShadowQuarantine_PortalSurface;

		const std::vector<uint64_t>* sectorGroups = movers.FindGroupsForSector(member.sectorIndex);
		if (sectorGroups != nullptr)
		{
			for (uint64_t otherGroupId : *sectorGroups)
			{
				if (otherGroupId == snapshot.stableGroupId) continue;
				const RuntimeMapMoverSnapshot* other = movers.FindGroup(otherGroupId);
				if (other == nullptr) continue;
				for (const RuntimeMapMoverMember& otherMember : other->members)
				{
					if (otherMember.sectorIndex == member.sectorIndex && MemberOwnsGeometry(otherMember))
					{
						quarantine |= NRIMapMoverShadowQuarantine_OverlappingGeometryOwner;
						break;
					}
				}
			}
		}
		return quarantine;
	}

	const char* GetCapabilityName(RuntimeMapMoverCapability capability)
	{
		switch (capability)
		{
		case RuntimeMapMoverCapability::RigidTranslation: return "rigid-translation";
		case RuntimeMapMoverCapability::RigidTransform: return "rigid-transform";
		case RuntimeMapMoverCapability::StableTopologyDeformer: return "stable-topology-deformer";
		case RuntimeMapMoverCapability::MaterialOrLightOnly: return "material-or-light-only";
		default: return "unknown";
		}
	}

	void EmitFailure(
		uint64_t frameIndex,
		const RuntimeMapMoverSnapshot& snapshot,
		const RuntimeMapMoverMember* member,
		uint32_t chunkIndex,
		uint32_t changeMask,
		const char* phase,
		const char* failure,
		uint32_t quarantine,
		uint32_t surfaceIndex = UINT32_MAX,
		uint32_t vertexIndex = UINT32_MAX,
		uint32_t primitiveIndex = UINT32_MAX,
		double nearestAuthoredCornerDistance = 0.0)
	{
		Printf("PERF pt map mover shadow observation NRI: frame=%llu group=0x%016llx sector=%d chunk=%u capability=%s change_mask=0x%x action=failure phase=%s failure=%s quarantine=0x%x member_flags=0x%x surface_index=%u vertex_index=%u primitive_index=%u nearest_authored_corner_distance=%.9f topology_generation=%llu geometry_generation=%llu material_generation=%llu transform_generation=%llu visibility_generation=%llu light_generation=%llu route_eligible=0 publication=old-path gpu_allocations=0 blas_commands=0 tlas_mutations=0\n",
			(unsigned long long)frameIndex,
			(unsigned long long)snapshot.stableGroupId,
			member != nullptr ? member->sectorIndex : -1,
			chunkIndex,
			GetCapabilityName(snapshot.capability),
			changeMask,
			phase,
			failure,
			quarantine,
			member != nullptr ? member->flags : 0,
			surfaceIndex,
			vertexIndex,
			primitiveIndex,
			nearestAuthoredCornerDistance,
			(unsigned long long)snapshot.topologyGeneration,
			(unsigned long long)snapshot.geometryGeneration,
			(unsigned long long)snapshot.materialGeneration,
			(unsigned long long)snapshot.transformGeneration,
			(unsigned long long)snapshot.visibilityGeneration,
			(unsigned long long)snapshot.lightGeneration);
	}

	void EmitSkip(
		uint64_t frameIndex,
		const RuntimeMapMoverSnapshot& snapshot,
		const RuntimeMapMoverMember* member,
		uint32_t changeMask,
		const char* reason)
	{
		Printf("PERF pt map mover shadow observation NRI: frame=%llu group=0x%016llx sector=%d capability=%s change_mask=0x%x action=skip reason=%s member_flags=0x%x topology_generation=%llu geometry_generation=%llu material_generation=%llu transform_generation=%llu visibility_generation=%llu light_generation=%llu publication=old-path gpu_allocations=0 blas_commands=0 tlas_mutations=0\n",
			(unsigned long long)frameIndex,
			(unsigned long long)snapshot.stableGroupId,
			member != nullptr ? member->sectorIndex : -1,
			GetCapabilityName(snapshot.capability),
			changeMask,
			reason,
			member != nullptr ? member->flags : 0,
			(unsigned long long)snapshot.topologyGeneration,
			(unsigned long long)snapshot.geometryGeneration,
			(unsigned long long)snapshot.materialGeneration,
			(unsigned long long)snapshot.transformGeneration,
			(unsigned long long)snapshot.visibilityGeneration,
			(unsigned long long)snapshot.lightGeneration);
	}

	void EmitAdjacentBoundaries(
		uint64_t frameIndex,
		const RuntimeMapMoverSnapshot& snapshot,
		const RuntimeMapMoverMember& member)
	{
		if (member.sectorIndex < 0 || (unsigned)member.sectorIndex >= sector.Size()) return;
		std::set<int32_t> adjacentSectors;
		for (const walltype& source : sector[(unsigned)member.sectorIndex].walls)
		{
			if (source.nextsector >= 0 && source.nextsector != member.sectorIndex)
				adjacentSectors.insert(source.nextsector);
		}
		for (int32_t adjacentSector : adjacentSectors)
		{
			Printf("PERF pt map mover shadow boundary NRI: frame=%llu group=0x%016llx owner_sector=%d adjacent_sector=%d member_flags=0x%x authority=unproven publication=old-path\n",
				(unsigned long long)frameIndex,
				(unsigned long long)snapshot.stableGroupId,
				member.sectorIndex,
				adjacentSector,
				member.flags);
		}
	}

	void EmitObservation(
		uint64_t frameIndex,
		const RuntimeMapMoverSnapshot& snapshot,
		const RuntimeMapMoverMember& member,
		uint32_t chunkIndex,
		uint32_t changeMask,
		const NRIMapMoverShadowObservationResult& result,
		const NRIMapMoverShadowRecord& record)
	{
		const char* action = result.createdCanonical ? "canonical-create" :
			(result.replacedCanonical ? "canonical-replace" : "classify");
		Printf("PERF pt map mover shadow observation NRI: frame=%llu group=0x%016llx sector=%d chunk=%u capability=%s change_mask=0x%x action=%s classification=%s valid=%u quarantine=0x%x member_flags=0x%x surfaces=%u vertices=%u triangles=%u topology_key=0x%016llx material_layout_key=0x%016llx material_state_key=0x%016llx resource_key=0x%016llx resource_stable=%u canonical_replaced=%u authority_generation_mismatch=%u generation_regression=%u rigid_contract_mismatch=%u material_state_acknowledged=%u order_changed=%u membership_changed=%u topology_changed=%u material_slot_changed=%u material_state_changed=%u attribute_changed=%u rigid_vertices=%u rigid_mean=%.9f rigid_max=%.9f bounds_max_error=%.9f observations=%llu rigid_streak=%llu remembered_resources=%u resource_overflow=%u aba_hits=%llu topology_generation=%llu geometry_generation=%llu material_generation=%llu transform_generation=%llu visibility_generation=%llu light_generation=%llu visibility_signature=0x%016llx sim_prev=(%.6f,%.6f,%.6f,%.9f) sim_current=(%.6f,%.6f,%.6f,%.9f) present_prev=(%.6f,%.6f,%.6f,%.9f) present_current=(%.6f,%.6f,%.6f,%.9f) route_eligible=0 publication=old-path gpu_allocations=0 atlas_bytes=0 material_rewrites=0 blas_commands=0 tlas_mutations=0\n",
			(unsigned long long)frameIndex,
			(unsigned long long)snapshot.stableGroupId,
			member.sectorIndex,
			chunkIndex,
			GetCapabilityName(snapshot.capability),
			changeMask,
			action,
			result.createdCanonical ? "canonical" : GetMapMoverGeometryClassificationName(result.comparison.classification),
			result.valid ? 1u : 0u,
			record.quarantineMask,
			member.flags,
			result.surfaceCount,
			result.vertexCount,
			result.triangleCount,
			(unsigned long long)record.canonical.topologyKey,
			(unsigned long long)record.canonical.materialLayoutKey,
			(unsigned long long)record.canonical.materialStateKey,
			(unsigned long long)record.canonical.resourceKey,
			result.resourceKeyStable ? 1u : 0u,
			result.replacedCanonical ? 1u : 0u,
			result.authorityGenerationMismatch ? 1u : 0u,
			result.generationRegression ? 1u : 0u,
			result.rigidContractMismatch ? 1u : 0u,
			result.materialStateAcknowledged ? 1u : 0u,
			result.comparison.generatedOrderChanged ? 1u : 0u,
			result.comparison.membershipChanged ? 1u : 0u,
			result.comparison.topologyChanged ? 1u : 0u,
			result.comparison.materialSlotChanged ? 1u : 0u,
			result.comparison.materialStateChanged ? 1u : 0u,
			result.comparison.vertexAttributeChanged ? 1u : 0u,
			result.comparison.rigidFitVertexCount,
			result.comparison.rigidFitMeanResidual,
			result.comparison.rigidFitMaxResidual,
			result.reconstructedBoundsMaxError,
			(unsigned long long)record.observationCount,
			(unsigned long long)record.consecutiveRigidCount,
			record.rememberedResourceKeyCount,
			record.resourceKeyOverflowCount,
			(unsigned long long)record.abaResourceHitCount,
			(unsigned long long)snapshot.topologyGeneration,
			(unsigned long long)snapshot.geometryGeneration,
			(unsigned long long)snapshot.materialGeneration,
			(unsigned long long)snapshot.transformGeneration,
			(unsigned long long)snapshot.visibilityGeneration,
			(unsigned long long)snapshot.lightGeneration,
			(unsigned long long)snapshot.visibilitySignature,
			snapshot.simulationPreviousPose.translation.X,
			snapshot.simulationPreviousPose.translation.Y,
			snapshot.simulationPreviousPose.translation.Z,
			snapshot.simulationPreviousPose.rotation.Radians(),
			snapshot.simulationCurrentPose.translation.X,
			snapshot.simulationCurrentPose.translation.Y,
			snapshot.simulationCurrentPose.translation.Z,
			snapshot.simulationCurrentPose.rotation.Radians(),
			snapshot.presentationPreviousPose.translation.X,
			snapshot.presentationPreviousPose.translation.Y,
			snapshot.presentationPreviousPose.translation.Z,
			snapshot.presentationPreviousPose.rotation.Radians(),
			snapshot.presentationCurrentPose.translation.X,
			snapshot.presentationCurrentPose.translation.Y,
			snapshot.presentationCurrentPose.translation.Z,
			snapshot.presentationCurrentPose.rotation.Radians());
	}
}

void NRIMapMoverShadow::CaptureChangedGroups(
	NRIMapMoverSystem& movers,
	const nri_scene::PTMapWorld& mapWorld,
	uint64_t frameIndex,
	int traceMode,
	uint32_t groupBudget)
{
	m_frameIndex = frameIndex;
	m_frameStats = {};
	m_frameStats.queuedBefore = movers.GetPendingChangedGroupCount();
	const uint64_t mapEpoch = movers.GetMapEpoch();
	m_state.Synchronize(mapWorld.buildSerial, mapEpoch);
	if (m_buildSerial != mapWorld.buildSerial || m_mapEpoch != mapEpoch)
	{
		m_pendingPairs.clear();
		m_observedPairs.clear();
		m_buildSerial = mapWorld.buildSerial;
		m_mapEpoch = mapEpoch;
	}
	if (!mapWorld.valid || movers.GetBuildSerial() != mapWorld.buildSerial)
	{
		m_frameStats.queuedAfter = movers.GetPendingChangedGroupCount();
		return;
	}

	NRIMapMoverChangedGroup changed;
	for (uint32_t captured = 0;
		captured < groupBudget && movers.PopChangedGroup(changed);
		++captured)
	{
		m_frameStats.groupsCaptured++;
		const uint64_t groupId = changed.stableGroupId;
		if (changed.removed)
		{
			for (auto it = m_pendingPairs.begin(); it != m_pendingPairs.end();)
				it = it->first.stableGroupId == groupId ? m_pendingPairs.erase(it) : std::next(it);
			for (auto it = m_observedPairs.begin(); it != m_observedPairs.end();)
				it = it->first.stableGroupId == groupId ? m_observedPairs.erase(it) : std::next(it);
			m_state.RemoveGroup(groupId);
			m_frameStats.groupsRemoved++;
			if (traceMode >= 2)
				Printf("PERF pt map mover shadow observation NRI: frame=%llu group=0x%016llx action=remove route_eligible=0 publication=old-path\n",
					(unsigned long long)frameIndex, (unsigned long long)groupId);
			continue;
		}

		const RuntimeMapMoverSnapshot& snapshot = changed.snapshot;
		std::map<uint32_t, RuntimeMapMoverMember> membersByChunk;
		for (const RuntimeMapMoverMember& sourceMember : snapshot.members)
		{
			if (!MemberOwnsGeometry(sourceMember))
			{
				if (traceMode >= 2)
					EmitSkip(frameIndex, snapshot, &sourceMember, changed.changeMask, "control-or-material-only");
				continue;
			}
			const int32_t semanticChunk =
				nri_static_scene_geometry::FindMapChunkIndexForSector(mapWorld, sourceMember.sectorIndex);
			if (semanticChunk < 0 || FindChunk(mapWorld, (uint32_t)semanticChunk) == nullptr)
			{
				m_frameStats.failures++;
				m_cumulativeWrapperFailures++;
				if (traceMode >= 2)
					EmitFailure(frameIndex, snapshot, &sourceMember, UINT32_MAX, changed.changeMask,
						"static-chunk", "missing-static-chunk",
						NRIMapMoverShadowQuarantine_AuthoredTopology);
				continue;
			}
			auto inserted = membersByChunk.emplace((uint32_t)semanticChunk, sourceMember);
			if (!inserted.second)
			{
				inserted.first->second.flags |= sourceMember.flags;
				inserted.first->second.wallCount += sourceMember.wallCount;
			}
		}

		if (membersByChunk.empty())
		{
			m_frameStats.groupsWithoutGeometry++;
			if (traceMode >= 2 && snapshot.members.Size() == 0)
				EmitSkip(frameIndex, snapshot, nullptr, changed.changeMask, "no-members");
		}
		std::vector<uint32_t> chunks;
		chunks.reserve(membersByChunk.size());
		for (const auto& entry : membersByChunk) chunks.push_back(entry.first);
		m_state.ReconcileGroup(groupId, chunks);

		for (auto it = m_pendingPairs.begin(); it != m_pendingPairs.end();)
		{
			const bool stale = it->first.stableGroupId == groupId &&
				membersByChunk.find(it->first.chunkIndex) == membersByChunk.end();
			it = stale ? m_pendingPairs.erase(it) : std::next(it);
		}
		for (auto it = m_observedPairs.begin(); it != m_observedPairs.end();)
		{
			const bool stale = it->first.stableGroupId == groupId &&
				membersByChunk.find(it->first.chunkIndex) == membersByChunk.end();
			it = stale ? m_observedPairs.erase(it) : std::next(it);
		}

		for (const auto& entry : membersByChunk)
		{
			const NRIMapMoverShadowRecordKey key = { groupId, entry.first };
			auto inserted = m_pendingPairs.emplace(key, PendingPair{});
			PendingPair& pending = inserted.first->second;
			if (inserted.second)
			{
				pending.firstQueuedFrame = frameIndex;
				m_frameStats.pairsQueued++;
			}
			else m_frameStats.pairsCoalesced++;
			pending.member = entry.second;
			pending.changeMask |= changed.changeMask;
			if (traceMode >= 2) EmitAdjacentBoundaries(frameIndex, snapshot, entry.second);
		}
	}

	// This schedules only a read if the exact path independently builds a known
	// chunk. It never requests live geometry or consumes map dirty state.
	if (traceMode > 0)
	{
		for (const auto& observed : m_observedPairs)
		{
			if (m_pendingPairs.find(observed.first) != m_pendingPairs.end()) continue;
			const RuntimeMapMoverSnapshot* snapshot = movers.FindGroup(observed.first.stableGroupId);
			if (snapshot == nullptr) continue;
			RuntimeMapMoverMember current = observed.second;
			bool retained = false;
			for (const RuntimeMapMoverMember& member : snapshot->members)
			{
				if (!MemberOwnsGeometry(member) || member.sectorIndex != observed.second.sectorIndex)
					continue;
				if (!retained) current = member;
				else current.flags |= member.flags;
				retained = true;
			}
			if (!retained) continue;
			PendingPair pending;
			pending.member = current;
			pending.firstQueuedFrame = frameIndex;
			m_pendingPairs.emplace(observed.first, pending);
			m_frameStats.pairsQueued++;
		}
	}
	m_frameStats.queuedAfter = movers.GetPendingChangedGroupCount();
}

void NRIMapMoverShadow::ObserveLiveChunk(
	const NRIMapMoverSystem& movers,
	const nri_scene::PTMapWorld& liveWorld,
	uint64_t frameIndex,
	int traceMode)
{
	if (!liveWorld.valid || liveWorld.chunks.size() != 1) return;
	const nri_scene::PTMapChunk& liveChunk = liveWorld.chunks[0];
	if (liveChunk.chunkIndex == UINT32_MAX) return;

	for (auto it = m_pendingPairs.begin(); it != m_pendingPairs.end();)
	{
		if (it->first.chunkIndex != liveChunk.chunkIndex)
		{
			++it;
			continue;
		}
		const NRIMapMoverShadowRecordKey key = it->first;
		PendingPair& pending = it->second;
		pending.lastAttemptFrame = frameIndex;
		pending.attemptCount++;
		m_frameStats.memberObservations++;

		const RuntimeMapMoverSnapshot* snapshot = movers.FindGroup(key.stableGroupId);
		RuntimeMapMoverMember member = pending.member;
		bool foundMember = false;
		if (snapshot != nullptr)
		{
			for (const RuntimeMapMoverMember& current : snapshot->members)
			{
				if (!MemberOwnsGeometry(current) || current.sectorIndex != pending.member.sectorIndex)
					continue;
				if (!foundMember) member = current;
				else member.flags |= current.flags;
				foundMember = true;
			}
		}
		if (snapshot == nullptr || !foundMember || member.sectorIndex != liveChunk.sectorIndex)
		{
			m_frameStats.failures++;
			m_cumulativeWrapperFailures++;
			if (snapshot != nullptr && traceMode >= 2)
				EmitFailure(frameIndex, *snapshot, &member, key.chunkIndex, pending.changeMask,
					"authority-member", "missing-or-mismatched-current-member",
					NRIMapMoverShadowQuarantine_AuthoredTopology);
			it = m_pendingPairs.erase(it);
			continue;
		}

		NRIMapMoverShadowObservation observation;
		observation.key = key;
		observation.sectorIndex = member.sectorIndex;
		observation.memberFlags = member.flags;
		observation.declaredRigid =
			snapshot->capability == RuntimeMapMoverCapability::RigidTranslation ||
			snapshot->capability == RuntimeMapMoverCapability::RigidTransform;
		observation.generations = { snapshot->topologyGeneration, snapshot->geometryGeneration,
			snapshot->materialGeneration, snapshot->transformGeneration,
			snapshot->visibilityGeneration, snapshot->lightGeneration };
		observation.visibilitySignature = snapshot->visibilitySignature;
		observation.quarantineMask = BuildOwnershipQuarantine(movers, *snapshot, member, false);

		const auto fail = [&](const char* phase, const char* failure, uint32_t bit,
			uint32_t surface = UINT32_MAX, uint32_t vertex = UINT32_MAX,
			uint32_t primitive = UINT32_MAX, double nearest = 0.0)
		{
			observation.quarantineMask |= bit;
			m_state.NoteFailure(observation);
			m_frameStats.failures++;
			m_cumulativeWrapperFailures++;
			if (traceMode >= 2)
				EmitFailure(frameIndex, *snapshot, &member, key.chunkIndex, pending.changeMask,
					phase, failure, observation.quarantineMask, surface, vertex, primitive, nearest);
		};

		PTMapMoverChunkView view;
		bool hasPortal = false;
		if (!BuildChunkView(liveWorld, liveChunk, member.flags, view, hasPortal))
		{
			fail("chunk-view", "empty-or-invalid-owned-view",
				NRIMapMoverShadowQuarantine_SurfaceAdapter);
			it = m_pendingPairs.erase(it);
			continue;
		}
		observation.quarantineMask = BuildOwnershipQuarantine(movers, *snapshot, member, hasPortal);

		std::vector<PTMapMoverSourceWall> sourceWalls;
		std::vector<PTMapMoverSourceSection> sourceSections;
		std::vector<PTMapMoverSourceSectionLine> sourceLines;
		BuildExplicitAuthoredSources(view, sourceWalls, sourceSections, sourceLines);
		PTMapMoverAuthoredTopology topology;
		PTMapMoverAuthoredTopologyValidation topologyValidation;
		if (!BuildPTMapMoverAuthoredTopology(
			view, sourceWalls, sourceSections, sourceLines, topology, topologyValidation))
		{
			fail("authored-topology",
				GetPTMapMoverAuthoredTopologyFailureName(topologyValidation.failure),
				NRIMapMoverShadowQuarantine_AuthoredTopology, topologyValidation.surfaceIndex);
			it = m_pendingPairs.erase(it);
			continue;
		}

		PTMapMoverAdapterOptions options;
		options.resolveTextureIdentity = ResolveTextureIdentity;
		WorldMapMoverGeometry worldGeometry;
		PTMapMoverAdapterValidation adapterValidation;
		if (!BuildWorldMapMoverGeometryFromPTMapChunkView(
			view, topology, worldGeometry, adapterValidation, options))
		{
			fail("surface-adapter", GetPTMapMoverAdapterFailureName(adapterValidation.failure),
				NRIMapMoverShadowQuarantine_SurfaceAdapter, adapterValidation.surfaceIndex,
				adapterValidation.vertexIndex, adapterValidation.primitiveIndex,
				adapterValidation.nearestAuthoredCornerDistance);
			it = m_pendingPairs.erase(it);
			continue;
		}

		float previousRows[12];
		float currentRows[12];
		// Section geometry has already sampled the render-time presentation pose
		// at this exact-path seam. Simulation poses are intentionally retained as
		// separate authority generations, not used to canonicalize these vertices.
		const RuntimeMapMoverPose& previousPose = snapshot->presentationPreviousPose;
		const RuntimeMapMoverPose& currentPose = snapshot->presentationCurrentPose;
		if (!BuildPTMapMoverSceneTransformFromDukePose(
				previousPose.translation.X, previousPose.translation.Y, previousPose.translation.Z,
				previousPose.rotation.Radians(), observation.previousTransform, previousRows) ||
			!BuildPTMapMoverSceneTransformFromDukePose(
				currentPose.translation.X, currentPose.translation.Y, currentPose.translation.Z,
				currentPose.rotation.Radians(), observation.currentTransform, currentRows))
		{
			fail("transform", "invalid-duke-pose",
				NRIMapMoverShadowQuarantine_TransformValidation);
			it = m_pendingPairs.erase(it);
			continue;
		}

		observation.worldGeometry = &worldGeometry;
		NRIMapMoverShadowObservationResult result;
		if (!m_state.Observe(observation, result))
		{
			m_frameStats.failures++;
			if (traceMode >= 2)
				EmitFailure(frameIndex, *snapshot, &member, key.chunkIndex, pending.changeMask,
					"canonical-geometry", GetMapMoverGeometryFailureName(result.validation.failure),
					observation.quarantineMask | NRIMapMoverShadowQuarantine_GeometryValidation);
			it = m_pendingPairs.erase(it);
			continue;
		}

		const NRIMapMoverShadowRecord* record = m_state.Find(key.stableGroupId, key.chunkIndex);
		if (record == nullptr)
		{
			m_frameStats.failures++;
			m_cumulativeWrapperFailures++;
			it = m_pendingPairs.erase(it);
			continue;
		}
		if (record->quarantineMask != 0) m_frameStats.quarantined++;
		if (result.createdCanonical) m_frameStats.canonicalCreates++;
		else
		{
			m_frameStats.validComparisons++;
			using C = MapMoverGeometryClassification;
			switch (result.comparison.classification)
			{
			case C::RigidTranslation:
			case C::RigidTransform: m_frameStats.rigid++; break;
			case C::StableLayoutDeformer: m_frameStats.deformer++; break;
			case C::TopologyChange:
			case C::MembershipChange: m_frameStats.topology++; break;
			case C::MaterialSlotChange:
			case C::MaterialStateChange: m_frameStats.material++; break;
			default: m_frameStats.unknown++; break;
			}
		}
		if (traceMode >= 2)
			EmitObservation(frameIndex, *snapshot, member, key.chunkIndex, pending.changeMask,
				result, *record);
		m_observedPairs[key] = member;
		it = m_pendingPairs.erase(it);
	}
}

void NRIMapMoverShadow::EndFrame(
	const NRIMapMoverSystem& movers,
	const nri_scene::PTMapWorld& mapWorld,
	uint64_t frameIndex,
	int traceMode)
{
	if (traceMode <= 0) return;
	uint64_t oldestAge = 0;
	for (const auto& pending : m_pendingPairs)
		oldestAge = std::max<uint64_t>(oldestAge, frameIndex >= pending.second.firstQueuedFrame
			? frameIndex - pending.second.firstQueuedFrame : 0ull);

	const NRIMapMoverShadowStateStats& stateStats = m_state.GetStats();
	Printf("PERF pt map mover shadow summary NRI: frame=%llu build_serial=%llu map_epoch=%llu authority_revision=%llu authority_queued_before=%u authority_queued_after=%u groups_captured=%u groups_removed=%u groups_without_geometry=%u pairs_queued=%u pairs_coalesced=%u pending_pairs=%u oldest_pending_age=%llu observations=%u creates=%u comparisons=%u failures=%u wrapper_failures=%llu quarantined=%u rigid=%u deformer=%u topology=%u material=%u unknown=%u retained_records=%u retained_cpu_bytes=%llu cumulative_observations=%llu cumulative_failures=%llu cumulative_creates=%llu cumulative_replacements=%llu cumulative_rigid=%llu cumulative_deformer=%llu cumulative_topology=%llu cumulative_material=%llu cumulative_unknown=%llu cumulative_aba_hits=%llu route_eligible=0 publication=old-path gpu_allocations=0 atlas_bytes=0 material_rewrites=0 blas_commands=0 tlas_mutations=0\n",
		(unsigned long long)frameIndex, (unsigned long long)mapWorld.buildSerial,
		(unsigned long long)movers.GetMapEpoch(),
		(unsigned long long)movers.GetFrameStats().authorityRevision,
		m_frameStats.queuedBefore, m_frameStats.queuedAfter,
		m_frameStats.groupsCaptured, m_frameStats.groupsRemoved,
		m_frameStats.groupsWithoutGeometry, m_frameStats.pairsQueued,
		m_frameStats.pairsCoalesced, (uint32_t)m_pendingPairs.size(),
		(unsigned long long)oldestAge, m_frameStats.memberObservations,
		m_frameStats.canonicalCreates, m_frameStats.validComparisons,
		m_frameStats.failures, (unsigned long long)m_cumulativeWrapperFailures,
		m_frameStats.quarantined, m_frameStats.rigid, m_frameStats.deformer,
		m_frameStats.topology, m_frameStats.material, m_frameStats.unknown,
		m_state.GetRecordCount(), (unsigned long long)m_state.EstimateRetainedCpuBytes(),
		(unsigned long long)stateStats.observations,
		(unsigned long long)stateStats.observationFailures,
		(unsigned long long)stateStats.canonicalCreates,
		(unsigned long long)stateStats.canonicalReplacements,
		(unsigned long long)stateStats.rigidObservations,
		(unsigned long long)stateStats.deformerObservations,
		(unsigned long long)stateStats.topologyObservations,
		(unsigned long long)stateStats.materialObservations,
		(unsigned long long)stateStats.unknownObservations,
		(unsigned long long)stateStats.abaResourceHits);
}

const char* GetNRIMapMoverShadowQuarantineName(uint32_t quarantineBit)
{
	switch (quarantineBit)
	{
	case NRIMapMoverShadowQuarantine_UnknownCapability: return "unknown-capability";
	case NRIMapMoverShadowQuarantine_AdjacencyUnproven: return "adjacency-unproven";
	case NRIMapMoverShadowQuarantine_OverlappingGeometryOwner: return "overlapping-geometry-owner";
	case NRIMapMoverShadowQuarantine_PortalSurface: return "portal-surface";
	case NRIMapMoverShadowQuarantine_AuthoredTopology: return "authored-topology";
	case NRIMapMoverShadowQuarantine_SurfaceAdapter: return "surface-adapter";
	case NRIMapMoverShadowQuarantine_GeometryValidation: return "geometry-validation";
	case NRIMapMoverShadowQuarantine_TransformValidation: return "transform-validation";
	case NRIMapMoverShadowQuarantine_AuthorityGenerationMismatch: return "authority-generation-mismatch";
	case NRIMapMoverShadowQuarantine_RigidContractMismatch: return "rigid-contract-mismatch";
	case NRIMapMoverShadowQuarantine_ReconstructedBoundsMismatch: return "reconstructed-bounds-mismatch";
	default: return "none";
	}
}
