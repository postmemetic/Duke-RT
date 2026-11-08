#pragma once

#include "actor_lifecycle_journal.h"

// A bounded journal consumer: missing history invalidates all certificates;
// known class/owner changes identify only the affected occurrence bindings.
class NRIVoxelMaterialLifecycle
{
public:
	bool Poll(const ActorLifecycleJournal& journal);
	const std::vector<int32_t>& ChangedActors() const { return changedActors; }
	void Reset();
private:
	uint64_t cursor = 0, world = 0;
	std::vector<ActorLifecycleEvent> events;
	std::vector<int32_t> changedActors;
};
