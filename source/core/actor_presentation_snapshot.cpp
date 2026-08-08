#include "actor_presentation_snapshot.h"

#include <algorithm>
#include <cstring>

#include "actor_lifecycle_journal.h"
#include "build.h"
#include "c_dispatch.h"
#include "coreactor.h"
#include "printf.h"
#include "textures/buildtiles.h"

namespace
{
	ActorPresentationSnapshot gSnapshot;
	uint64_t gNextSnapshotGeneration = 1;
	uint64_t gNextPlacementGeneration = 1;
	bool gFullReconcileRequired = true;

	uint64_t HashBytes(uint64_t hash, const void* data, size_t size)
	{
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	template<class T>
	uint64_t HashValue(uint64_t hash, const T& value)
	{
		return HashBytes(hash, &value, sizeof(value));
	}

	bool OwnerLess(const ActorPresentationOwnerKey& a, const ActorPresentationOwnerKey& b)
	{
		return a.ownerWorldEpoch != b.ownerWorldEpoch ?
			a.ownerWorldEpoch < b.ownerWorldEpoch :
			a.ownerLifetimeGeneration < b.ownerLifetimeGeneration;
	}

	uint64_t HashState(const ActorPresentationState& state)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashValue(hash, state.owner.ownerWorldEpoch);
		hash = HashValue(hash, state.owner.ownerLifetimeGeneration);
		hash = HashValue(hash, state.actorIndex);
		hash = HashValue(hash, state.statNumber);
		hash = HashValue(hash, state.physicalSectorIndex);
		hash = HashValue(hash, state.picnum);
		hash = HashValue(hash, state.textureId);
		hash = HashValue(hash, state.displayTextureId);
		hash = HashValue(hash, state.position.x);
		hash = HashValue(hash, state.position.y);
		hash = HashValue(hash, state.position.z);
		hash = HashValue(hash, state.previousPosition.x);
		hash = HashValue(hash, state.previousPosition.y);
		hash = HashValue(hash, state.previousPosition.z);
		hash = HashValue(hash, state.angles.yawDegrees);
		hash = HashValue(hash, state.angles.pitchDegrees);
		hash = HashValue(hash, state.angles.rollDegrees);
		hash = HashValue(hash, state.previousAngles.yawDegrees);
		hash = HashValue(hash, state.previousAngles.pitchDegrees);
		hash = HashValue(hash, state.previousAngles.rollDegrees);
		hash = HashValue(hash, state.scale.x);
		hash = HashValue(hash, state.scale.y);
		hash = HashValue(hash, state.modelRotation.yawDegrees);
		hash = HashValue(hash, state.modelRotation.pitchDegrees);
		hash = HashValue(hash, state.modelRotation.rollDegrees);
		hash = HashValue(hash, state.modelPositionOffset.x);
		hash = HashValue(hash, state.modelPositionOffset.y);
		hash = HashValue(hash, state.modelPositionOffset.z);
		hash = HashValue(hash, state.cstat);
		hash = HashValue(hash, state.cstat2);
		hash = HashValue(hash, state.renderFlags);
		hash = HashValue(hash, state.alpha);
		hash = HashValue(hash, state.authorityCurrent);
		return HashValue(hash, state.publicationEligible);
	}

	ActorPresentationAngles CopyAngles(const DRotator& angles)
	{
		return { angles.Yaw.Degrees(), angles.Pitch.Degrees(), angles.Roll.Degrees() };
	}

	ActorPresentationState CaptureState(const DCoreActor& actor, uint64_t worldEpoch)
	{
		ActorPresentationState state;
		state.actorIndex = (int32_t)actor.GetIndex();
		state.owner = { worldEpoch, state.actorIndex };
		state.statNumber = actor.exists() ? (int32_t)actor.spr.statnum : -1;
		state.physicalSectorIndex = actor.sectno();
		state.picnum = actor.spr.picnum;
		state.textureId = actor.spr.spritetexture().GetIndex();
		state.displayTextureId = actor.dispictex.GetIndex();
		state.position = { actor.spr.pos.X, actor.spr.pos.Y, actor.spr.pos.Z };
		state.previousPosition = { actor.opos.X, actor.opos.Y, actor.opos.Z };
		state.angles = CopyAngles(actor.spr.Angles);
		state.previousAngles = CopyAngles(actor.PrevAngles);
		state.scale = { actor.spr.scale.X, actor.spr.scale.Y };
		state.modelRotation = CopyAngles(actor.sprext.rot);
		state.modelPositionOffset = {
			actor.sprext.position_offset.X,
			actor.sprext.position_offset.Y,
			actor.sprext.position_offset.Z
		};
		state.cstat = (uint32_t)actor.spr.cstat;
		state.cstat2 = actor.spr.cstat2;
		state.renderFlags = actor.sprext.renderflags;
		state.alpha = actor.sprext.alpha;
		state.authorityCurrent = actor.exists() && (actor.ObjectFlags & OF_EuthanizeMe) == 0;
		state.publicationEligible = state.authorityCurrent && state.physicalSectorIndex >= 0 &&
			actor.spr.scale.X != 0.0 && actor.spr.scale.Y != 0.0 &&
			(actor.spr.cstat & CSTAT_SPRITE_INVISIBLE) == 0 &&
			(actor.sprext.renderflags & SPREXT_TEMPINVISIBLE) == 0;
		state.placementStateHash = HashState(state);
		return state;
	}

	const ActorPresentationState* FindState(const ActorPresentationSnapshot& snapshot, const ActorPresentationOwnerKey& owner)
	{
		auto found = std::lower_bound(snapshot.actors.begin(), snapshot.actors.end(), owner,
			[](const ActorPresentationState& state, const ActorPresentationOwnerKey& key)
			{
				return OwnerLess(state.owner, key);
			});
		return found != snapshot.actors.end() && found->owner == owner ? &*found : nullptr;
	}

	uint64_t ResolvePlacementGeneration(
		const ActorPresentationState* previous,
		const ActorPresentationState& current,
		uint64_t& nextGeneration)
	{
		if (previous != nullptr && previous->placementStateHash == current.placementStateHash)
		{
			return previous->placementGeneration;
		}
		const uint64_t generation = nextGeneration++;
		if (nextGeneration == 0) nextGeneration = 1;
		return generation;
	}

	bool FailSelfTest(std::string* failureReason, const char* reason)
	{
		if (failureReason != nullptr) *failureReason = reason;
		return false;
	}
}

void PublishActorPresentationSnapshot(uint64_t simulationGeneration)
{
	ActorPresentationSnapshot next;
	next.snapshotGeneration = gNextSnapshotGeneration++;
	if (gNextSnapshotGeneration == 0) gNextSnapshotGeneration = 1;
	next.worldEpoch = GetActorLifecycleJournal().WorldEpoch();
	next.simulationGeneration = simulationGeneration;
	next.lifecycleSerial = GetActorLifecycleJournal().LatestSerial();
	next.complete = true;
	next.fullReconcileRequired = gFullReconcileRequired || gSnapshot.worldEpoch != next.worldEpoch;

	TSpriteIterator<DCoreActor> it;
	while (DCoreActor* actor = it.Next())
	{
		if (actor == nullptr || !actor->exists() || (actor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			continue;
		}
		ActorPresentationState state = CaptureState(*actor, next.worldEpoch);
		if (!state.owner.IsValid())
		{
			continue;
		}
		next.actors.push_back(state);
	}

	std::sort(next.actors.begin(), next.actors.end(),
		[](const ActorPresentationState& a, const ActorPresentationState& b)
		{
			return OwnerLess(a.owner, b.owner);
		});

	uint64_t snapshotHash = 1469598103934665603ull;
	for (ActorPresentationState& state : next.actors)
	{
		const ActorPresentationState* previous = FindState(gSnapshot, state.owner);
		state.placementGeneration = ResolvePlacementGeneration(previous, state, gNextPlacementGeneration);
		snapshotHash = HashValue(snapshotHash, state.placementStateHash);
		snapshotHash = HashValue(snapshotHash, state.placementGeneration);
	}
	next.stateHash = snapshotHash;
	gSnapshot = std::move(next);
	gFullReconcileRequired = false;
}

void ResetActorPresentationSnapshot(uint64_t worldEpoch)
{
	gSnapshot = {};
	gSnapshot.worldEpoch = worldEpoch;
	gFullReconcileRequired = true;
}

const ActorPresentationSnapshot& GetActorPresentationSnapshot()
{
	return gSnapshot;
}

const ActorPresentationState* LookupActorPresentationByOwnerKey(const ActorPresentationOwnerKey& owner)
{
	return owner.IsValid() ? FindState(gSnapshot, owner) : nullptr;
}

const ActorPresentationState* LookupActorPresentationByActor(const DCoreActor* actor)
{
	if (actor == nullptr)
	{
		return nullptr;
	}
	return LookupActorPresentationByOwnerKey({ gSnapshot.worldEpoch, (int64_t)actor->GetIndex() });
}

bool RunActorPresentationSnapshotSelfTests(std::string* failureReason)
{
	if (!ActorPresentationOwnerKey{ 7, 0 }.IsValid() ||
		ActorPresentationOwnerKey{ 7, 0 } == ActorPresentationOwnerKey{ 8, 0 })
	{
		return FailSelfTest(failureReason, "owner epoch/lifetime-zero contract failed");
	}

	ActorPresentationState base;
	base.owner = { 7, 41 };
	base.actorIndex = 41;
	base.authorityCurrent = true;
	base.publicationEligible = true;
	base.position = { 1.0, 2.0, 3.0 };
	base.previousPosition = base.position;
	base.scale = { 1.0, 1.0 };
	base.physicalSectorIndex = 3;
	base.placementStateHash = HashState(base);
	base.placementGeneration = 12;
	uint64_t nextGeneration = 20;
	if (ResolvePlacementGeneration(&base, base, nextGeneration) != 12 || nextGeneration != 20)
	{
		return FailSelfTest(failureReason, "unchanged state advanced placement generation");
	}

	ActorPresentationState moved = base;
	moved.position.x += 1.0;
	moved.placementStateHash = HashState(moved);
	if (ResolvePlacementGeneration(&base, moved, nextGeneration) != 20 || nextGeneration != 21)
	{
		return FailSelfTest(failureReason, "position change did not advance placement generation");
	}

	auto expectPlacementChange = [&](ActorPresentationState changed, const char* reason)
	{
		changed.placementStateHash = HashState(changed);
		const uint64_t before = nextGeneration;
		if (ResolvePlacementGeneration(&base, changed, nextGeneration) != before ||
			nextGeneration != before + 1)
		{
			return FailSelfTest(failureReason, reason);
		}
		return true;
	};
	ActorPresentationState relinked = base;
	relinked.physicalSectorIndex++;
	if (!expectPlacementChange(relinked, "sector relink did not advance placement generation")) return false;
	ActorPresentationState rotated = base;
	rotated.angles.yawDegrees += 45.0;
	if (!expectPlacementChange(rotated, "rotation did not advance placement generation")) return false;
	ActorPresentationState scaled = base;
	scaled.scale.x *= 2.0;
	if (!expectPlacementChange(scaled, "scale did not advance placement generation")) return false;
	ActorPresentationState represented = base;
	represented.picnum++;
	if (!expectPlacementChange(represented, "representation did not advance placement generation")) return false;

	ActorPresentationState hidden = base;
	hidden.publicationEligible = false;
	hidden.placementStateHash = HashState(hidden);
	const uint64_t hiddenGeneration = nextGeneration;
	if (ResolvePlacementGeneration(&base, hidden, nextGeneration) != hiddenGeneration)
	{
		return FailSelfTest(failureReason, "renderability change did not advance placement generation");
	}

	ActorLifecycleJournal journal(2);
	journal.Record(ActorLifecycleEventType::Inserted, 1, 41, -1, 1, 3);
	const uint64_t firstEpoch = journal.WorldEpoch();
	journal.Record(ActorLifecycleEventType::Reset);
	journal.Record(ActorLifecycleEventType::Inserted, 2, 41, -1, 1, 4);
	std::vector<ActorLifecycleEvent> events;
	const ActorLifecycleReadResult read = journal.ReadAfter(0, events);
	if (!read.overflowed || events.size() != 2 || journal.WorldEpoch() == firstEpoch ||
		events.front().type != ActorLifecycleEventType::Reset ||
		events.back().worldEpoch != journal.WorldEpoch() || events.back().sectorIndex != 4)
	{
		return FailSelfTest(failureReason, "journal reset/overflow authority contract failed");
	}

	if (failureReason != nullptr) failureReason->clear();
	return true;
}

CCMD(actor_presentation_selftest)
{
	std::string failure;
	const bool passed = RunActorPresentationSnapshotSelfTests(&failure);
	Printf("Actor presentation snapshot self-test: result=%s%s%s\n",
		passed ? "pass" : "fail",
		failure.empty() ? "" : " reason=",
		failure.c_str());
}
