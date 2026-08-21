#include "ns.h"

#include <algorithm>
#include <limits>

#include "duke3d.h"
#include "printf.h"
#include "runtime_map_movers.h"
#include "serializer.h"

BEGIN_DUKE_NS

namespace
{
	constexpr uint64_t HashOffset = 1469598103934665603ull;
	constexpr uint64_t HashPrime = 1099511628211ull;

	struct MoverSource
	{
		DDukeActor* actor = nullptr;
		int sectorIndex = -1;
		int canonicalWallOffset = -1;
		int actorIndex = -1;
		int effectorLotag = 0;
	};

	struct MoverCandidate
	{
		RuntimeMapMoverSnapshot snapshot;
		TArray<MoverSource> sources;
	};

	TArray<RuntimeMapMoverSnapshot> MoverSnapshots;

	struct TerminalMoverAuthorityRecord
	{
		uint64_t stableGroupId = 0;
		int32_t ownerActorIndex = -1;
		int32_t ownerSectorIndex = -1;
		int32_t effectorLotag = 0;
		int32_t effectorHitag = 0;
	};

	TArray<TerminalMoverAuthorityRecord> TerminalMoverAuthorities;
	uint64_t MoverMapEpoch = 1;
	uint64_t MoverRevision = 1;

	FSerializer& Serialize(FSerializer& arc, const char* key, TerminalMoverAuthorityRecord& value,
		TerminalMoverAuthorityRecord* def)
	{
		static TerminalMoverAuthorityRecord nullRecord;
		if (def == nullptr)
		{
			def = &nullRecord;
			if (arc.isReading()) value = {};
		}
		if (arc.BeginObject(key))
		{
			arc("group_id", value.stableGroupId, def->stableGroupId)
				("actor_index", value.ownerActorIndex, def->ownerActorIndex)
				("sector_index", value.ownerSectorIndex, def->ownerSectorIndex)
				("lotag", value.effectorLotag, def->effectorLotag)
				("hitag", value.effectorHitag, def->effectorHitag)
				.EndObject();
		}
		return arc;
	}

	void HashBytes(uint64_t& hash, const void* data, size_t size)
	{
		auto bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; i++)
		{
			hash = (hash ^ bytes[i]) * HashPrime;
		}
	}

	template<class T>
	void HashValue(uint64_t& hash, const T& value)
	{
		HashBytes(hash, &value, sizeof(value));
	}

	bool UsesCanonicalWallOffsets(int lotag)
	{
		switch (lotag)
		{
		case SE_0_ROTATING_SECTOR:
		case SE_2_EARTHQUAKE:
		case SE_5_BOSS:
		case SE_6_SUBWAY:
		case SE_11_SWINGING_DOOR:
		case SE_14_SUBWAY_CAR:
		case SE_15_SLIDING_DOOR:
		case SE_16_REACTOR:
		case SE_26:
		case SE_30_TWO_WAY_TRAIN:
			return true;
		default:
			return false;
		}
	}

	bool HasCanonicalWallSpan(int offset, int count)
	{
		if (offset < 0 || count < 0) return false;
		const unsigned first = (unsigned)offset;
		const unsigned span = (unsigned)count;
		return first <= mspos.Size() && span <= mspos.Size() - first;
	}

	RuntimeMapMoverCapability ClassifyMover(const DDukeActor* actor)
	{
		switch (actor->spr.lotag)
		{
		case SE_15_SLIDING_DOOR:
			return RuntimeMapMoverCapability::RigidTranslation;
		case SE_0_ROTATING_SECTOR:
			return (actor->sector()->lotag & 0xff) == 30
				? RuntimeMapMoverCapability::StableTopologyDeformer
				: RuntimeMapMoverCapability::RigidTransform;
		case SE_6_SUBWAY:
		case SE_11_SWINGING_DOOR:
		case SE_14_SUBWAY_CAR:
		case SE_30_TWO_WAY_TRAIN:
			return RuntimeMapMoverCapability::RigidTransform;
		case SE_2_EARTHQUAKE:
		case SE_5_BOSS:
		case SE_16_REACTOR:
		case SE_17_WARP_ELEVATOR:
		case SE_18_INCREMENTAL_SECTOR_RISE_FALL:
		case SE_20_STRETCH_BRIDGE:
		case SE_21_DROP_FLOOR:
		case SE_22_TEETH_DOOR:
		case SE_25_PISTON:
		case SE_26:
		case SE_29_WAVES:
		case SE_31_FLOOR_RISE_FALL:
		case SE_32_CEILING_RISE_FALL:
			return RuntimeMapMoverCapability::StableTopologyDeformer;
		case SE_3_RANDOM_LIGHTS_AFTER_SHOT_OUT:
		case SE_4_RANDOM_LIGHTS:
		case SE_8_UP_OPEN_DOOR_LIGHTS:
		case SE_9_DOWN_OPEN_DOOR_LIGHTS:
		case SE_12_LIGHT_SWITCH:
		case SE_28_LIGHTNING:
		case SE_47_LIGHT_SWITCH:
		case SE_48_LIGHT_SWITCH:
		case SE_49_POINT_LIGHT:
		case SE_50_SPOT_LIGHT:
			return RuntimeMapMoverCapability::MaterialOrLightOnly;
		default:
			return RuntimeMapMoverCapability::Unknown;
		}
	}

	uint64_t StableGroupId(DDukeActor* actor)
	{
		const bool pivotGroup = actor->spr.lotag == SE_0_ROTATING_SECTOR && actor->GetOwner() != nullptr;
		const auto authority = pivotGroup ? actor->GetOwner() : actor;
		uint64_t hash = HashOffset;
		const uint32_t kind = pivotGroup ? 0x44554b30u : 0x44554b4du;
		// DCoreActor::time is the serialized game-issued spawn identity (GetIndex),
		// not a renderer/container index. It remains stable when a moving effector
		// changes sector or repurposes tags as live state, and across save restore.
		const int spawnIdentity = authority->GetIndex();
		HashValue(hash, kind);
		HashValue(hash, spawnIdentity);
		return hash;
	}

	RuntimeMapMoverPose CapturePose(const DDukeActor* actor)
	{
		RuntimeMapMoverPose pose = {};
		if (!UsesCanonicalWallOffsets(actor->spr.lotag)) return pose;
		auto translationOwner = actor->spr.lotag == SE_0_ROTATING_SECTOR && actor->ownerActor != nullptr
			? actor->ownerActor.Get() : actor;
		pose.translation = { translationOwner->spr.pos.X, translationOwner->spr.pos.Y, 0.0 };
		switch (actor->spr.lotag)
		{
		case SE_0_ROTATING_SECTOR:
		case SE_5_BOSS:
		case SE_6_SUBWAY:
		case SE_11_SWINGING_DOOR:
		case SE_14_SUBWAY_CAR:
		case SE_16_REACTOR:
		case SE_30_TWO_WAY_TRAIN:
			pose.rotation = actor->temp_angle;
			break;
		default:
			break;
		}
		return pose;
	}

	bool SamePose(const RuntimeMapMoverPose& a, const RuntimeMapMoverPose& b)
	{
		return a.translation == b.translation && a.rotation == b.rotation;
	}

	bool SameDeformerTarget(const RuntimeMapMoverDeformerPayload& a, const RuntimeMapMoverDeformerPayload& b)
	{
		return a.kind == b.kind && a.sectorIndex == b.sectorIndex;
	}

	bool SameDeformerState(const RuntimeMapMoverDeformerState& a, const RuntimeMapMoverDeformerState& b)
	{
		return a.floorZ == b.floorZ && a.floorHeinum == b.floorHeinum;
	}

	RuntimeMapMoverDeformerPayload CaptureDeformer(const MoverCandidate& candidate)
	{
		RuntimeMapMoverDeformerPayload deformer = {};
		if (candidate.snapshot.capability != RuntimeMapMoverCapability::StableTopologyDeformer ||
			candidate.sources.Size() != 1 || candidate.sources[0].actor->spr.lotag != SE_29_WAVES)
		{
			return deformer;
		}

		const auto& source = candidate.sources[0];
		const auto sector = source.actor->sector();
		deformer.kind = RuntimeMapMoverDeformerKind::SectorFloorPlane;
		deformer.sectorIndex = source.sectorIndex;
		deformer.simulationCurrent.floorZ = sector->floorz;
		deformer.simulationCurrent.floorHeinum = sector->floorheinum;
		return deformer;
	}

	MoverCandidate* FindCandidate(TArray<MoverCandidate>& candidates, uint64_t id)
	{
		for (auto& candidate : candidates)
			if (candidate.snapshot.stableGroupId == id) return &candidate;
		return nullptr;
	}

	const RuntimeMapMoverSnapshot* FindPrevious(uint64_t id)
	{
		for (const auto& snapshot : MoverSnapshots)
			if (snapshot.stableGroupId == id) return &snapshot;
		return nullptr;
	}

	TerminalMoverAuthorityRecord* FindTerminalAuthorityForSector(
		TArray<TerminalMoverAuthorityRecord>& authorities,
		int sectorIndex)
	{
		for (auto& authority : authorities)
			if (authority.ownerSectorIndex == sectorIndex) return &authority;
		return nullptr;
	}

	bool SameTerminalOwner(
		const RuntimeMapMoverSnapshot& live,
		const TerminalMoverAuthorityRecord& terminal)
	{
		return live.ownerActorIndex == terminal.ownerActorIndex &&
			live.ownerSectorIndex == terminal.ownerSectorIndex &&
			live.effectorLotag == terminal.effectorLotag &&
			live.effectorHitag == terminal.effectorHitag;
	}

	void AppendMember(RuntimeMapMoverSnapshot& snapshot, int sectorIndex, int wallOffset, int wallCount, uint32_t flags)
	{
		for (auto& member : snapshot.members)
		{
			if (member.sectorIndex == sectorIndex && member.canonicalWallOffset == wallOffset)
			{
				member.flags |= flags;
				return;
			}
		}
		const auto index = snapshot.members.Reserve(1);
		snapshot.members[index] = { sectorIndex, wallOffset, wallCount, flags };
	}

	void BuildCandidates(TArray<MoverCandidate>& candidates)
	{
		DukeStatIterator it(STAT_EFFECTOR);
		while (auto actor = it.Next())
		{
			if (!actor->exists() || actor->sector() == nullptr || actor->spr.lotag == SE_1_PIVOT) continue;
			// Analytic point/spot lights have their own actor-light owner and may cross
			// sectors while attached to moving geometry. They are not map-geometry
			// mutation authorities and must not manufacture topology work here.
			if (actor->spr.lotag == SE_49_POINT_LIGHT || actor->spr.lotag == SE_50_SPOT_LIGHT) continue;

			const auto id = StableGroupId(actor);
			auto candidate = FindCandidate(candidates, id);
			if (candidate == nullptr)
			{
				const auto index = candidates.Reserve(1);
				candidate = &candidates[index];
				auto authority = actor->spr.lotag == SE_0_ROTATING_SECTOR && actor->GetOwner() != nullptr
					? actor->GetOwner() : actor;
				candidate->snapshot.stableGroupId = id;
				candidate->snapshot.mapEpoch = MoverMapEpoch;
				candidate->snapshot.capability = ClassifyMover(actor);
				candidate->snapshot.ownerActorIndex = authority->GetIndex();
				candidate->snapshot.ownerSectorIndex = authority->sectno();
				candidate->snapshot.effectorLotag = actor->spr.lotag;
				candidate->snapshot.effectorHitag = actor->spr.hitag;
			}
			else if (candidate->snapshot.capability != ClassifyMover(actor))
			{
				candidate->snapshot.capability = RuntimeMapMoverCapability::Unknown;
			}

			const int sectorIndex = actor->sectno();
			const int wallCount = (int)actor->sector()->walls.Size();
			const int wallOffset = UsesCanonicalWallOffsets(actor->spr.lotag) ? actor->temp_data[1] : -1;
			uint32_t flags;
			if (candidate->snapshot.capability == RuntimeMapMoverCapability::MaterialOrLightOnly)
				flags = RuntimeMapMoverMember_ControlOnly;
			else if (actor->spr.lotag == SE_29_WAVES)
				flags = RuntimeMapMoverMember_OwnsFloor;
			else
				flags = RuntimeMapMoverMember_OwnsWalls | RuntimeMapMoverMember_OwnsFloor |
					RuntimeMapMoverMember_OwnsCeiling | RuntimeMapMoverMember_SharedVertexPropagation |
					RuntimeMapMoverMember_AdjacencyUnproven;
			AppendMember(candidate->snapshot, sectorIndex, wallOffset, wallCount, flags);
			candidate->sources.Push({ actor, sectorIndex, wallOffset, actor->GetIndex(), actor->spr.lotag });

			if (actor->spr.lotag == SE_0_ROTATING_SECTOR && actor->GetOwner() != nullptr && actor->GetOwner()->sector() != actor->sector())
			{
				AppendMember(candidate->snapshot, actor->GetOwner()->sectno(), -1,
					(int)actor->GetOwner()->sector()->walls.Size(), RuntimeMapMoverMember_ControlOnly);
			}
		}

		TArray<unsigned> prunedTerminalAuthorities;
		for (unsigned authorityIndex = 0; authorityIndex < TerminalMoverAuthorities.Size(); ++authorityIndex)
		{
			const auto& authority = TerminalMoverAuthorities[authorityIndex];
			if (authority.stableGroupId == 0 || authority.effectorLotag != SE_12_LIGHT_SWITCH ||
				authority.ownerSectorIndex < 0 || (unsigned)authority.ownerSectorIndex >= sector.Size())
			{
				prunedTerminalAuthorities.Push(authorityIndex);
				continue;
			}
			// A live controller with the same game-issued identity wins. This is a
			// fail-closed guard for malformed or incompatible save data; normal SE12
			// retirement destroys the actor before the next authority update.
			if (auto live = FindCandidate(candidates, authority.stableGroupId))
			{
				if (!SameTerminalOwner(live->snapshot, authority))
				{
					DPrintf(DMSG_WARNING,
						"Runtime mover terminal collision: group=0x%016llx terminal_actor=%d terminal_sector=%d live_actor=%d live_sector=%d; rejecting stale terminal authority.\n",
						(unsigned long long)authority.stableGroupId,
						authority.ownerActorIndex,
						authority.ownerSectorIndex,
						live->snapshot.ownerActorIndex,
						live->snapshot.ownerSectorIndex);
				}
				prunedTerminalAuthorities.Push(authorityIndex);
				continue;
			}

			const auto index = candidates.Reserve(1);
			auto& candidate = candidates[index];
			candidate.snapshot.stableGroupId = authority.stableGroupId;
			candidate.snapshot.mapEpoch = MoverMapEpoch;
			candidate.snapshot.capability = RuntimeMapMoverCapability::MaterialOrLightOnly;
			candidate.snapshot.lifecycle = RuntimeMapMoverLifecycle::Terminal;
			candidate.snapshot.ownerActorIndex = authority.ownerActorIndex;
			candidate.snapshot.ownerSectorIndex = authority.ownerSectorIndex;
			candidate.snapshot.effectorLotag = authority.effectorLotag;
			candidate.snapshot.effectorHitag = authority.effectorHitag;
			const int wallCount = (int)sector[authority.ownerSectorIndex].walls.Size();
			AppendMember(candidate.snapshot, authority.ownerSectorIndex, -1, wallCount,
				RuntimeMapMoverMember_ControlOnly);
			candidate.sources.Push({ nullptr, authority.ownerSectorIndex, -1,
				authority.ownerActorIndex, authority.effectorLotag });
		}
		for (unsigned index = prunedTerminalAuthorities.Size(); index > 0; --index)
		{
			TerminalMoverAuthorities.Delete(prunedTerminalAuthorities[index - 1]);
		}
	}

	void HashSector(const MoverSource& source, RuntimeMapMoverCapability capability,
		uint64_t& topology, uint64_t& geometry, uint64_t& material, uint64_t& visibility, uint64_t& light)
	{
		const auto actor = source.actor;
		const auto sec = actor != nullptr ? actor->sector() : &sector[source.sectorIndex];
		const int wallCount = (int)sec->walls.Size();
		HashValue(topology, source.sectorIndex);
		HashValue(topology, source.canonicalWallOffset);
		HashValue(topology, source.effectorLotag);
		HashValue(topology, wallCount);

		const bool canonicalRigid =
			(capability == RuntimeMapMoverCapability::RigidTranslation || capability == RuntimeMapMoverCapability::RigidTransform) &&
			HasCanonicalWallSpan(source.canonicalWallOffset, wallCount);
		const bool ownsGeometry = capability != RuntimeMapMoverCapability::MaterialOrLightOnly;
		if (ownsGeometry)
		{
			HashValue(geometry, sec->floorz);
			HashValue(geometry, sec->ceilingz);
			HashValue(geometry, sec->floorheinum);
			HashValue(geometry, sec->ceilingheinum);
		}

		const int floorTexture = sec->floortexture.GetIndex();
		const int ceilingTexture = sec->ceilingtexture.GetIndex();
		HashValue(material, floorTexture);
		HashValue(material, ceilingTexture);
		HashValue(material, sec->floorxpan_);
		HashValue(material, sec->floorypan_);
		HashValue(material, sec->ceilingxpan_);
		HashValue(material, sec->ceilingypan_);
		HashValue(material, sec->floorpal);
		HashValue(material, sec->ceilingpal);

		const auto floorFlags = sec->floorstat.GetValue();
		const auto ceilingFlags = sec->ceilingstat.GetValue();
		HashValue(visibility, floorFlags);
		HashValue(visibility, ceilingFlags);
		HashValue(visibility, sec->portalflags);
		HashValue(visibility, sec->portalnum);
		HashValue(visibility, sec->visibility);
		HashValue(light, sec->floorshade);
		HashValue(light, sec->ceilingshade);

		for (int i = 0; i < wallCount; i++)
		{
			const auto& wal = sec->walls[i];
			const int wallIndex = wallindex(&wal);
			HashValue(topology, wallIndex);
			HashValue(topology, wal.point2);
			HashValue(topology, wal.nextwall);
			HashValue(topology, wal.nextsector);
			if (canonicalRigid)
			{
				const auto& local = mspos[source.canonicalWallOffset + i];
				HashValue(geometry, local.X);
				HashValue(geometry, local.Y);
			}
			else if (ownsGeometry)
			{
				HashValue(geometry, wal.pos.X);
				HashValue(geometry, wal.pos.Y);
			}

			const int wallTexture = wal.walltexture.GetIndex();
			const int overTexture = wal.overtexture.GetIndex();
			HashValue(material, wallTexture);
			HashValue(material, overTexture);
			HashValue(material, wal.xpan_);
			HashValue(material, wal.ypan_);
			HashValue(material, wal.xrepeat);
			HashValue(material, wal.yrepeat);
			HashValue(material, wal.pal);
			const auto wallFlags = wal.cstat.GetValue();
			HashValue(visibility, wallFlags);
			HashValue(visibility, wal.portalflags);
			HashValue(visibility, wal.portalnum);
			HashValue(light, wal.shade);
		}
	}

	void FinishCandidate(MoverCandidate& candidate)
	{
		if (candidate.sources.Size() > 1)
		{
			std::sort(candidate.sources.Data(), candidate.sources.Data() + candidate.sources.Size(),
				[](const MoverSource& a, const MoverSource& b)
				{
					if (a.sectorIndex != b.sectorIndex) return a.sectorIndex < b.sectorIndex;
					return a.actorIndex < b.actorIndex;
				});
		}
		if (candidate.snapshot.members.Size() > 1)
		{
			std::sort(candidate.snapshot.members.Data(), candidate.snapshot.members.Data() + candidate.snapshot.members.Size(),
				[](const RuntimeMapMoverMember& a, const RuntimeMapMoverMember& b)
				{
					if (a.sectorIndex != b.sectorIndex) return a.sectorIndex < b.sectorIndex;
					return a.canonicalWallOffset < b.canonicalWallOffset;
				});
		}

		auto pose = candidate.sources[0].actor != nullptr
			? CapturePose(candidate.sources[0].actor) : RuntimeMapMoverPose{};
		for (const auto& source : candidate.sources)
		{
			if (source.actor != nullptr && !SamePose(pose, CapturePose(source.actor)))
				candidate.snapshot.capability = RuntimeMapMoverCapability::Unknown;
		}
		if (candidate.snapshot.capability == RuntimeMapMoverCapability::RigidTranslation ||
			candidate.snapshot.capability == RuntimeMapMoverCapability::RigidTransform)
		{
			for (const auto& source : candidate.sources)
			{
				if (source.actor == nullptr)
				{
					candidate.snapshot.capability = RuntimeMapMoverCapability::Unknown;
					break;
				}
				const int wallCount = (int)source.actor->sector()->walls.Size();
				if (!HasCanonicalWallSpan(source.canonicalWallOffset, wallCount))
				{
					candidate.snapshot.capability = RuntimeMapMoverCapability::Unknown;
					break;
				}
			}
		}
		candidate.snapshot.deformer = CaptureDeformer(candidate);

		uint64_t topology = HashOffset, geometry = HashOffset, material = HashOffset;
		uint64_t visibility = HashOffset, light = HashOffset;
		const uint8_t capability = (uint8_t)candidate.snapshot.capability;
		const uint8_t deformerKind = (uint8_t)candidate.snapshot.deformer.kind;
		HashValue(topology, capability);
		HashValue(topology, deformerKind);
		HashValue(topology, candidate.snapshot.deformer.sectorIndex);
		for (const auto& member : candidate.snapshot.members)
		{
			HashValue(topology, member.sectorIndex);
			HashValue(topology, member.canonicalWallOffset);
			HashValue(topology, member.wallCount);
			HashValue(topology, member.flags);
		}
		for (const auto& source : candidate.sources)
			HashSector(source, candidate.snapshot.capability, topology, geometry, material, visibility, light);

		auto& snapshot = candidate.snapshot;
		snapshot.topologySignature = topology;
		snapshot.geometrySignature = geometry;
		snapshot.materialSignature = material;
		snapshot.visibilitySignature = visibility;
		snapshot.lightSignature = light;
		snapshot.simulationCurrentPose = pose;

		const auto previous = FindPrevious(snapshot.stableGroupId);
		if (previous == nullptr)
		{
			snapshot.topologyGeneration = snapshot.geometryGeneration = snapshot.materialGeneration = 1;
			snapshot.transformGeneration = snapshot.visibilityGeneration = snapshot.lightGeneration = 1;
			snapshot.simulationPreviousPose = pose;
			snapshot.deformer.simulationPrevious = snapshot.deformer.simulationCurrent;
		}
		else
		{
			snapshot.topologyGeneration = previous->topologyGeneration + (previous->topologySignature != topology);
			const bool deformerChanged = !SameDeformerTarget(previous->deformer, snapshot.deformer) ||
				!SameDeformerState(previous->deformer.simulationCurrent, snapshot.deformer.simulationCurrent);
			snapshot.geometryGeneration = previous->geometryGeneration +
				(previous->geometrySignature != geometry || deformerChanged);
			snapshot.materialGeneration = previous->materialGeneration + (previous->materialSignature != material);
			snapshot.visibilityGeneration = previous->visibilityGeneration + (previous->visibilitySignature != visibility);
			snapshot.lightGeneration = previous->lightGeneration + (previous->lightSignature != light);
			snapshot.transformGeneration = previous->transformGeneration + !SamePose(previous->simulationCurrentPose, pose);
			snapshot.simulationPreviousPose = previous->simulationCurrentPose;
			snapshot.deformer.simulationPrevious = SameDeformerTarget(previous->deformer, snapshot.deformer)
				? previous->deformer.simulationCurrent : snapshot.deformer.simulationCurrent;
		}
		snapshot.presentationPreviousPose = snapshot.simulationPreviousPose;
		snapshot.presentationCurrentPose = snapshot.simulationCurrentPose;
		snapshot.deformer.presentationPrevious = snapshot.deformer.simulationPrevious;
		snapshot.deformer.presentationCurrent = snapshot.deformer.simulationCurrent;
	}

	bool SameAuthorityState(const TArray<RuntimeMapMoverSnapshot>& a, const TArray<RuntimeMapMoverSnapshot>& b)
	{
		if (a.Size() != b.Size()) return false;
		for (unsigned index = 0; index < a.Size(); ++index)
		{
			const auto& left = a[index];
			const auto& right = b[index];
			if (left.stableGroupId != right.stableGroupId || left.mapEpoch != right.mapEpoch ||
				left.capability != right.capability ||
				left.lifecycle != right.lifecycle ||
				left.ownerActorIndex != right.ownerActorIndex ||
				left.ownerSectorIndex != right.ownerSectorIndex ||
				left.effectorLotag != right.effectorLotag ||
				left.effectorHitag != right.effectorHitag ||
				left.topologyGeneration != right.topologyGeneration ||
				left.geometryGeneration != right.geometryGeneration ||
				left.materialGeneration != right.materialGeneration ||
				left.transformGeneration != right.transformGeneration ||
				left.visibilityGeneration != right.visibilityGeneration ||
				left.lightGeneration != right.lightGeneration ||
				left.topologySignature != right.topologySignature ||
				left.geometrySignature != right.geometrySignature ||
				left.materialSignature != right.materialSignature ||
				left.visibilitySignature != right.visibilitySignature ||
				left.lightSignature != right.lightSignature ||
				!SamePose(left.simulationPreviousPose, right.simulationPreviousPose) ||
				!SamePose(left.simulationCurrentPose, right.simulationCurrentPose) ||
				!SameDeformerTarget(left.deformer, right.deformer) ||
				!SameDeformerState(left.deformer.simulationPrevious, right.deformer.simulationPrevious) ||
				!SameDeformerState(left.deformer.simulationCurrent, right.deformer.simulationCurrent) ||
				!SameDeformerState(left.deformer.presentationPrevious, right.deformer.presentationPrevious) ||
				!SameDeformerState(left.deformer.presentationCurrent, right.deformer.presentationCurrent))
			{
				return false;
			}
			if (left.members.Size() != right.members.Size()) return false;
			for (unsigned memberIndex = 0; memberIndex < left.members.Size(); ++memberIndex)
			{
				const auto& leftMember = left.members[memberIndex];
				const auto& rightMember = right.members[memberIndex];
				if (leftMember.sectorIndex != rightMember.sectorIndex ||
					leftMember.canonicalWallOffset != rightMember.canonicalWallOffset ||
					leftMember.wallCount != rightMember.wallCount ||
					leftMember.flags != rightMember.flags)
				{
					return false;
				}
			}
		}
		return true;
	}
}

void ResetRuntimeMapMoverAuthority()
{
	MoverSnapshots.Clear();
	TerminalMoverAuthorities.Clear();
	if (++MoverMapEpoch == 0) MoverMapEpoch = 1;
	if (++MoverRevision == 0) MoverRevision = 1;
}

void RetireRuntimeMapMoverAuthority(DDukeActor* actor)
{
	if (actor == nullptr || !actor->exists() || actor->sector() == nullptr ||
		actor->spr.lotag != SE_12_LIGHT_SWITCH)
	{
		return;
	}

	TerminalMoverAuthorityRecord record = {};
	record.stableGroupId = StableGroupId(actor);
	record.ownerActorIndex = actor->GetIndex();
	record.ownerSectorIndex = actor->sectno();
	record.effectorLotag = actor->spr.lotag;
	record.effectorHitag = actor->spr.hitag;
	for (const auto& authority : TerminalMoverAuthorities)
	{
		if (authority.stableGroupId == record.stableGroupId &&
			authority.ownerSectorIndex != record.ownerSectorIndex)
		{
			DPrintf(DMSG_WARNING,
				"Runtime mover terminal collision: group=0x%016llx sectors=%d/%d; rejecting new terminal authority.\n",
				(unsigned long long)record.stableGroupId,
				authority.ownerSectorIndex,
				record.ownerSectorIndex);
			return;
		}
	}

	if (auto existing = FindTerminalAuthorityForSector(TerminalMoverAuthorities, record.ownerSectorIndex))
	{
		*existing = record;
	}
	else if (TerminalMoverAuthorities.Size() >= sector.Size())
	{
		DPrintf(DMSG_WARNING,
			"Runtime mover terminal capacity exhausted: terminals=%u sectors=%u; rejecting sector %d.\n",
			TerminalMoverAuthorities.Size(), sector.Size(), record.ownerSectorIndex);
	}
	else
	{
		TerminalMoverAuthorities.Push(record);
	}
}

void SerializeRuntimeMapMoverAuthority(FSerializer& arc)
{
	arc("runtime_map_mover_terminals", TerminalMoverAuthorities);
	if (!arc.isReading()) return;

	TArray<TerminalMoverAuthorityRecord> sanitized;
	unsigned invalidRecords = 0;
	unsigned collidingRecords = 0;
	unsigned coalescedRecords = 0;
	unsigned overflowRecords = 0;
	for (const auto& record : TerminalMoverAuthorities)
	{
		if (record.stableGroupId == 0 || record.effectorLotag != SE_12_LIGHT_SWITCH ||
			record.ownerActorIndex < 0 || record.ownerSectorIndex < 0 ||
			(unsigned)record.ownerSectorIndex >= sector.Size())
		{
			invalidRecords++;
			continue;
		}

		auto sameSector = FindTerminalAuthorityForSector(sanitized, record.ownerSectorIndex);
		bool collidingGroup = false;
		for (const auto& accepted : sanitized)
		{
			if (&accepted != sameSector && accepted.stableGroupId == record.stableGroupId)
			{
				collidingGroup = true;
				break;
			}
		}
		if (collidingGroup)
		{
			collidingRecords++;
			continue;
		}
		if (sameSector != nullptr)
		{
			*sameSector = record;
			coalescedRecords++;
		}
		else if (sanitized.Size() >= sector.Size())
		{
			overflowRecords++;
		}
		else
		{
			sanitized.Push(record);
		}
	}
	TerminalMoverAuthorities = std::move(sanitized);
	if (invalidRecords != 0 || collidingRecords != 0 || coalescedRecords != 0 || overflowRecords != 0)
	{
		DPrintf(DMSG_WARNING,
			"Runtime mover terminal restore sanitized: invalid=%u collision=%u coalesced=%u overflow=%u retained=%u cap=%u.\n",
			invalidRecords, collidingRecords, coalescedRecords, overflowRecords,
			TerminalMoverAuthorities.Size(), sector.Size());
	}
}

void RestoreRuntimeMapMoverActorIdentityAllocator()
{
	int maxActorIdentity = -1;
	TSpriteIterator<DDukeActor> iterator;
	while (auto actor = iterator.Next())
	{
		maxActorIdentity = std::max(maxActorIdentity, actor->GetIndex());
	}
	for (const auto& terminal : TerminalMoverAuthorities)
	{
		maxActorIdentity = std::max(maxActorIdentity, terminal.ownerActorIndex);
	}
	if (maxActorIdentity == std::numeric_limits<int>::max())
	{
		I_Error("Save game exhausted the actor identity range.");
	}
	leveltimer = maxActorIdentity + 1;
}

void UpdateRuntimeMapMoverAuthority()
{
	TArray<MoverCandidate> candidates;
	BuildCandidates(candidates);
	for (auto& candidate : candidates) FinishCandidate(candidate);
	if (candidates.Size() > 1)
	{
		std::sort(candidates.Data(), candidates.Data() + candidates.Size(),
			[](const MoverCandidate& a, const MoverCandidate& b)
			{
				return a.snapshot.stableGroupId < b.snapshot.stableGroupId;
			});
	}

	TArray<RuntimeMapMoverSnapshot> nextSnapshots;
	for (auto& candidate : candidates) nextSnapshots.Push(std::move(candidate.snapshot));
	const bool authorityChanged = !SameAuthorityState(MoverSnapshots, nextSnapshots);
	MoverSnapshots = std::move(nextSnapshots);
	if (authorityChanged && ++MoverRevision == 0) MoverRevision = 1;
}

void CaptureRuntimeMapMoverAuthority(TArray<RuntimeMapMoverSnapshot>& out)
{
	out = MoverSnapshots;
}

RuntimeMapMoverAuthorityState GameInterface::GetRuntimeMapMoverAuthorityState() const
{
	return { true, MoverMapEpoch, MoverRevision };
}

void GameInterface::CaptureRuntimeMapMovers(TArray<RuntimeMapMoverSnapshot>& out) const
{
	CaptureRuntimeMapMoverAuthority(out);
}

bool GameInterface::IsPortalClosureSector(int sectorIndex) const
{
	return sectorIndex >= 0 && (unsigned)sectorIndex < sector.Size() &&
		(sector[(unsigned)sectorIndex].lotag & 0xff) == ST_20_CEILING_DOOR;
}

END_DUKE_NS
