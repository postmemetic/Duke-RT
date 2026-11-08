#include "nri_persistent_voxel_material_lifecycle.h"
#include <algorithm>

bool NRIVoxelMaterialLifecycle::Poll(const ActorLifecycleJournal& journal)
{
	changedActors.clear();
	bool full = world != journal.WorldEpoch() || journal.LatestSerial() < cursor;
	if (full)
	{
		world = journal.WorldEpoch(); cursor = journal.LatestSerial();
		return true;
	}
	if (cursor == journal.LatestSerial()) return false;
	const auto result = journal.ReadAfter(cursor, events);
	cursor = result.latestSerial;
	full = result.overflowed;
	for (const auto& event : events)
	{
		if (event.worldEpoch != world) { full = true; continue; }
		switch (event.type)
		{
		case ActorLifecycleEventType::Inserted:
		case ActorLifecycleEventType::ClassChanged:
			if (event.actorIndex < 0) full = true;
			else changedActors.push_back(event.actorIndex);
			break;
		case ActorLifecycleEventType::Removed:
			// Authority removal already excludes the occurrence from rendering.
			// Keep its historical alias until retention releases it. A later insert
			// with the same index recertifies it before reuse; missing insert history
			// forces the full pass via the overflow guard above.
			break;
		case ActorLifecycleEventType::StatChanged:
			break;
		case ActorLifecycleEventType::Reset:
		default:
			full = true;
			break;
		}
	}
	std::sort(changedActors.begin(), changedActors.end());
	changedActors.erase(std::unique(changedActors.begin(), changedActors.end()), changedActors.end());
	return full;
}

void NRIVoxelMaterialLifecycle::Reset()
{
	cursor = world = 0; events.clear(); changedActors.clear();
}
