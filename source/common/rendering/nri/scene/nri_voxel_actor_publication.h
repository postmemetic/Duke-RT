#pragma once

#include "nri_voxel_actor_publication_types.h"
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace nri_scene
{
enum VoxelPublicationChange : uint32_t
{
	VoxelPublicationTopology = 1u,
	VoxelPublicationBinding = 2u,
	VoxelPublicationPresentation = 4u,
	VoxelPublicationAuthority = 8u,
};

struct VoxelPublicationGenerations
{
	uint64_t revision = 1;
	uint64_t topology = 1;
	uint64_t binding = 1;
	uint64_t presentation = 1;
	uint64_t authority = 1;
};

// Owned records keep removed/replaced producer surfaces alive for existing readers.
struct VoxelPublicationEntry
{
	PersistentVoxelCacheEntryView view;
	std::shared_ptr<const SurfaceRef> surface;
	std::shared_ptr<const SurfaceRef> lightSurface;
};

struct VoxelPublicationPresentationData
{
	uint64_t placementGeneration = 0, placementStateHash = 0, surfaceSignature = 0, transformBasisSignature = 0;
	int32_t physicalSectorIndex = -1;
	bool indirectOnly = false;
	float instanceTransform[12] = {};
	float currentTranslation[3] = {}, bakedTranslation[3] = {};
};

struct VoxelPublicationDelta
{
	uint64_t identity = 0;
	uint64_t lastSeenFrame = 0;
	uint32_t changes = 0;
	bool removed = false;
	// Null on capture-stamp-only changes; the immutable binding is unchanged.
	std::shared_ptr<const VoxelPublicationEntry> entry;
	VoxelPublicationPresentationData presentation;
	bool presentationChanged = false;
};

struct VoxelActorPublicationSnapshot
{
	VoxelPublicationGenerations generations;
	uint64_t baseRevision = 0;
	uint64_t captureFrame = 0;
	std::shared_ptr<const std::vector<uint64_t>> identities;
	std::vector<VoxelPublicationDelta> deltas;
};

struct VoxelActorPublicationStats
{
	uint32_t visited = 0;
	uint32_t changed = 0;
	uint32_t bindings = 0;
	uint32_t transforms = 0;
	uint32_t removed = 0;
	uint32_t topologySorts = 0;
	uint32_t retained = 0;
	uint32_t sourceEntries = 0;
	uint32_t highWater = 0;
	uint32_t resyncs = 0;
	uint32_t sharedSurfaceSources = 0;
	uint32_t surfaceCopies = 0;
};

uint32_t CompareVoxelPublicationEntry(const PersistentVoxelCacheEntryView& left,
	const PersistentVoxelCacheEntryView& right, bool fullSurfaceComparison = false);
bool SameVoxelPublicationEntry(const PersistentVoxelCacheEntryView& left,
	const PersistentVoxelCacheEntryView& right, bool fullSurfaceComparison = false);
void SetVoxelPublicationCaptureFrame(PersistentVoxelCacheEntryView& entry, uint64_t captureFrame);
VoxelPublicationPresentationData GetVoxelPublicationPresentation(const PersistentVoxelCacheEntryView& entry);
void ApplyVoxelPublicationPresentation(const VoxelPublicationPresentationData& presentation, PersistentVoxelCacheEntryView& entry);

// Current-state registry, not an unbounded event history. Marking an already dirty
// identity is allocation-free; erased producer identities leave the registry.
class VoxelActorPublication
{
public:
	using BuildEntry = std::function<bool(uint64_t, PersistentVoxelCacheEntryView&)>;
	void MarkDirty(uint64_t identity);
	void Remove(uint64_t identity);
	void Reset();
	void InvalidateAll(bool authorityChanged = false);
	void InvalidateExternalReadiness();
	std::shared_ptr<const VoxelActorPublicationSnapshot> Publish(uint64_t captureFrame, const BuildEntry& build);
	std::shared_ptr<const VoxelPublicationEntry> Find(uint64_t identity, uint64_t& lastSeenFrame,
		VoxelPublicationPresentationData* presentation = nullptr) const;
	const VoxelActorPublicationStats& Stats() const { return stats; }
	bool IsCurrent(const VoxelActorPublicationSnapshot& candidate) const { return snapshot.get() == &candidate; }
	void CopyEntries(std::vector<PersistentVoxelCacheEntryView>& out) const;
private:
	struct CurrentEntry
	{
		std::shared_ptr<const VoxelPublicationEntry> entry;
		VoxelPublicationPresentationData presentation;
		uint64_t lastSeenFrame = 0;
		bool dirty = false;
		bool removed = false;
		uint32_t externalIndex = UINT32_MAX;
		const SurfaceRef* surfaceSource = nullptr;
		const SurfaceRef* lightSurfaceSource = nullptr;
	};
	struct SharedSurface
	{
		std::weak_ptr<const SurfaceRef> backing;
		uint64_t geometryHash = 0, primitiveHash = 0;
		uint32_t users = 0;
	};
	std::unordered_map<uint64_t, CurrentEntry> current;
	// Keys represent only current producer bindings. Old snapshots own their
	// backing independently and cannot keep dead source-address keys resident.
	std::unordered_map<const SurfaceRef*, SharedSurface> sharedSurfaces;
	std::vector<uint64_t> dirty;
	std::vector<uint64_t> externalReadiness;
	std::shared_ptr<const VoxelActorPublicationSnapshot> snapshot;
	VoxelPublicationGenerations generations;
	VoxelActorPublicationStats stats;
	uint32_t forcedChanges = 0;
	void SetExternalReadiness(uint64_t identity, bool needed);
	void SetSurfaceSource(const SurfaceRef*& currentSource, const SurfaceRef* source);
	std::shared_ptr<const SurfaceRef> OwnSurface(const SurfaceRef* source, uint64_t geometryHash, uint64_t primitiveHash);
	std::shared_ptr<const VoxelPublicationEntry> OwnEntry(const PersistentVoxelCacheEntryView& view,
		CurrentEntry& state, bool bindingChanged);
};

// A consumer keeps dense identity-sorted rows and their ownership. A missed
// publication or topology transition resynchronizes explicitly; ordinary deltas
// update only their indexed rows. Capture age is a presentation clock, not topology.
class VoxelActorPublicationCursor
{
public:
	bool Synchronize(const VoxelActorPublication& owner, const VoxelActorPublicationSnapshot& snapshot);
	void Reset();
	const std::vector<PersistentVoxelCacheEntryView>& Entries() const { return entries; }
	const VoxelPublicationGenerations& Generations() const { return generations; }
	uint32_t FindIndex(uint64_t identity) const;
	uint32_t LastPatchedCount() const { return lastPatched; }
	bool Accepted() const { return accepted; }
private:
	std::vector<PersistentVoxelCacheEntryView> entries;
	std::vector<std::shared_ptr<const VoxelPublicationEntry>> owners;
	std::unordered_map<uint64_t, uint32_t> indices;
	VoxelPublicationGenerations generations = {};
	bool initialized = false;
	bool accepted = false;
	uint32_t lastPatched = 0;
};
}
