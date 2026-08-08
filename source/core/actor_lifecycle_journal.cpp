#include "actor_lifecycle_journal.h"

#include <algorithm>

ActorLifecycleJournal::ActorLifecycleJournal(size_t capacity)
	: mEvents((std::max<size_t>)(capacity, 1u))
{
}

uint64_t ActorLifecycleJournal::Record(
	ActorLifecycleEventType type,
	uintptr_t actorAddress,
	int32_t actorIndex,
	int32_t oldStat,
	int32_t newStat,
	int32_t sectorIndex)
{
	if (type == ActorLifecycleEventType::Reset)
	{
		++mWorldEpoch;
		if (mWorldEpoch == 0)
		{
			mWorldEpoch = 1;
		}
	}

	++mLatestSerial;
	if (mLatestSerial == 0)
	{
		mLatestSerial = 1;
		mCount = 0;
	}

	ActorLifecycleEvent& event = mEvents[(mLatestSerial - 1) % mEvents.size()];
	event.serial = mLatestSerial;
	event.worldEpoch = mWorldEpoch;
	event.type = type;
	event.actorAddress = actorAddress;
	event.actorIndex = actorIndex;
	event.sectorIndex = sectorIndex;
	event.oldStat = oldStat;
	event.newStat = newStat;
	mCount = (std::min)(mCount + 1, mEvents.size());
	return mLatestSerial;
}

ActorLifecycleReadResult ActorLifecycleJournal::ReadAfter(
	uint64_t cursor,
	std::vector<ActorLifecycleEvent>& outEvents) const
{
	outEvents.clear();
	ActorLifecycleReadResult result = {};
	result.latestSerial = mLatestSerial;
	if (mCount == 0)
	{
		return result;
	}

	result.oldestAvailableSerial = mLatestSerial - (uint64_t)mCount + 1;
	if (cursor >= mLatestSerial)
	{
		return result;
	}

	uint64_t firstSerial = cursor + 1;
	if (firstSerial < result.oldestAvailableSerial)
	{
		result.overflowed = true;
		firstSerial = result.oldestAvailableSerial;
	}

	outEvents.reserve((size_t)(mLatestSerial - firstSerial + 1));
	for (uint64_t serial = firstSerial; serial <= mLatestSerial; ++serial)
	{
		const ActorLifecycleEvent& event = mEvents[(serial - 1) % mEvents.size()];
		if (event.serial == serial)
		{
			outEvents.push_back(event);
		}
	}
	return result;
}

ActorLifecycleJournal& GetActorLifecycleJournal()
{
	static ActorLifecycleJournal journal;
	return journal;
}
