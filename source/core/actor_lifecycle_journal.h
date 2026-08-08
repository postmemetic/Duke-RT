#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

enum class ActorLifecycleEventType : uint8_t
{
	Inserted,
	Removed,
	StatChanged,
	Reset,
};

struct ActorLifecycleEvent
{
	uint64_t serial = 0;
	uint64_t worldEpoch = 0;
	ActorLifecycleEventType type = ActorLifecycleEventType::Inserted;
	// Legacy migration aid only. New persistent consumers must use
	// (worldEpoch, actorIndex) as the actor owner key.
	uintptr_t actorAddress = 0;
	int32_t actorIndex = -1;
	int32_t sectorIndex = -1;
	int32_t oldStat = -1;
	int32_t newStat = -1;
};

struct ActorLifecycleReadResult
{
	uint64_t latestSerial = 0;
	uint64_t oldestAvailableSerial = 0;
	bool overflowed = false;
};

class ActorLifecycleJournal
{
public:
	explicit ActorLifecycleJournal(size_t capacity = 8192);

	uint64_t Record(
		ActorLifecycleEventType type,
		uintptr_t actorAddress = 0,
		int32_t actorIndex = -1,
		int32_t oldStat = -1,
		int32_t newStat = -1,
		int32_t sectorIndex = -1);
	ActorLifecycleReadResult ReadAfter(uint64_t cursor, std::vector<ActorLifecycleEvent>& outEvents) const;
	uint64_t LatestSerial() const { return mLatestSerial; }
	uint64_t WorldEpoch() const { return mWorldEpoch; }
	size_t Capacity() const { return mEvents.size(); }

private:
	std::vector<ActorLifecycleEvent> mEvents;
	uint64_t mLatestSerial = 0;
	uint64_t mWorldEpoch = 1;
	size_t mCount = 0;
};

ActorLifecycleJournal& GetActorLifecycleJournal();
