#pragma once
#include "nri_persistent_voxels.h"

bool IsPersistentVoxelCacheEntryPublicationCurrent(
	const nri_scene::PersistentVoxelCacheEntryView& entry);
void CopyPersistentVoxelActorAuthority(
	const nri_scene::PersistentVoxelCacheEntryView& source,
	PersistentVoxelBatch::ActorEntry& target);
void CopyPersistentVoxelInstanceAuthority(
	const nri_scene::PersistentVoxelCacheEntryView& source,
	PersistentVoxelInstanceRecord& target);
uint64_t BuildPersistentVoxelActorBindingGeneration(
	const PersistentVoxelBatch::ActorEntry& actor);
NRIPersistentVoxelMaterialRangeHandle PersistentVoxelMaterialRangeHandle(
	const PersistentVoxelMaterialVariantResource& resource);
bool PersistentVoxelMaterialRangeMatches(
	const PersistentVoxelBatch::ActorEntry& actor,
	const PersistentVoxelMaterialVariantResource& resource);
uint64_t ResolvePersistentVoxelCacheEntryGeometrySignature(const nri_scene::PersistentVoxelCacheEntryView& entry);

class NRIVoxelPublicationEntryRange
{
public:
	explicit NRIVoxelPublicationEntryRange(const std::vector<nri_scene::PersistentVoxelCacheEntryView>& source) : entries(source) {}
	void SetOrder(const NRIVoxelPublicationOrder& value) { order = &value; }
	size_t size() const { return entries.size(); }
	struct Iterator
	{
		const NRIVoxelPublicationEntryRange& range;
		size_t index;
		const nri_scene::PersistentVoxelCacheEntryView& operator*() const
		{
			return range.entries[range.order ? range.order->keys[range.order->Indices()[index]].inputIndex : index];
		}
		void operator++() { ++index; }
		bool operator!=(const Iterator& other) const { return index != other.index; }
	};
	Iterator begin() const { return {*this, 0}; }
	Iterator end() const { return {*this, entries.size()}; }
private:
	const std::vector<nri_scene::PersistentVoxelCacheEntryView>& entries;
	const NRIVoxelPublicationOrder* order = nullptr;
};
