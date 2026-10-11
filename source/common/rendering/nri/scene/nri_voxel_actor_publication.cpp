#include "nri_voxel_actor_publication.h"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace nri_scene
{
namespace
{
bool SameMaterial(const MaterialRef& a, const MaterialRef& b)
{
	return std::tie(a.texture, a.emissiveSourceTexture, a.palette, a.shade, a.alpha, a.flags,
		a.voxelPalettePolicyContentKey) == std::tie(b.texture, b.emissiveSourceTexture, b.palette,
		b.shade, b.alpha, b.flags, b.voxelPalettePolicyContentKey) && a.voxelPalettePolicy == b.voxelPalettePolicy;
}

bool SameSurface(const SurfaceRef* a, const SurfaceRef* b, bool full)
{
	if ((a == nullptr) != (b == nullptr)) return false;
	if (a == nullptr) return true;
	const auto& p = a->provenance;
	const auto& q = b->provenance;
	const auto& t = a->temporal;
	const auto& u = b->temporal;
	if (a->materialRowSpan != b->materialRowSpan || !SameMaterial(a->material, b->material) ||
		std::tie(p.sourceType, p.sectorIndex, p.wallIndex, p.sectionIndex, p.mapChunkIndex, p.nextSectorIndex,
			p.actorIndex, p.drawListType, p.cstat, p.materialFlags, p.actorOverlayRuleCount) !=
		std::tie(q.sourceType, q.sectorIndex, q.wallIndex, q.sectionIndex, q.mapChunkIndex, q.nextSectorIndex,
			q.actorIndex, q.drawListType, q.cstat, q.materialFlags, q.actorOverlayRuleCount) ||
		!std::equal(std::begin(p.actorOverlayRuleIds), std::end(p.actorOverlayRuleIds), std::begin(q.actorOverlayRuleIds)) ||
		std::tie(t.occurrenceId, t.topologyKey, t.generation, t.historyAge, t.materialVerticalReference,
			t.reason, t.identityValid, t.correspondenceValid, t.materialVerticalReferenceValid) !=
		std::tie(u.occurrenceId, u.topologyKey, u.generation, u.historyAge, u.materialVerticalReference,
			u.reason, u.identityValid, u.correspondenceValid, u.materialVerticalReferenceValid) ||
		a->vertices.size() != b->vertices.size() || a->indices.size() != b->indices.size() ||
		a->primitiveLocalMaterialSlots.size() != b->primitiveLocalMaterialSlots.size()) return false;
	if (!full) return true;
	if (a->indices != b->indices || a->primitiveLocalMaterialSlots != b->primitiveLocalMaterialSlots) return false;
	for (size_t i = 0; i < a->vertices.size(); ++i)
	{
		const CapturedVertex& x = a->vertices[i];
		const CapturedVertex& y = b->vertices[i];
		if (std::memcmp(x.position, y.position, sizeof(x.position)) != 0 ||
			std::memcmp(x.prevPosition, y.prevPosition, sizeof(x.prevPosition)) != 0 ||
			std::memcmp(x.uv, y.uv, sizeof(x.uv)) != 0 || x.temporalCornerKey != y.temporalCornerKey) return false;
	}
	return true;
}

void Advance(uint64_t& generation)
{
	if (++generation == 0) generation = 1;
}
}

uint32_t CompareVoxelPublicationEntry(const PersistentVoxelCacheEntryView& a,
	const PersistentVoxelCacheEntryView& b, bool full)
{
	uint32_t changes = 0;
	if (a.identityKey != b.identityKey || a.ownerWorldEpoch != b.ownerWorldEpoch ||
		a.ownerLifetimeGeneration != b.ownerLifetimeGeneration || a.actorIndex != b.actorIndex)
		changes |= VoxelPublicationTopology | VoxelPublicationAuthority;
	if (a.authorityCurrent != b.authorityCurrent || a.publicationEligible != b.publicationEligible ||
		a.pendingRemoval != b.pendingRemoval) changes |= VoxelPublicationAuthority;
	if (std::tie(a.signature, a.geometrySignature, a.bakedSurfaceSignature, a.materialSignature,
		a.meshKeyHash, a.materialKeyHash, a.geometryContentHash, a.renderPrimitiveHash,
		a.meshVariantHash, a.materialVariantHash, a.meshBakeSpace, a.sourcePicnum, a.resolvedVoxelIndex,
		a.primitiveCount, a.model, a.sharedVariantSurface, a.desiredPending, a.directOnlyAdmission) !=
		std::tie(b.signature, b.geometrySignature, b.bakedSurfaceSignature, b.materialSignature,
		b.meshKeyHash, b.materialKeyHash, b.geometryContentHash, b.renderPrimitiveHash,
		b.meshVariantHash, b.materialVariantHash, b.meshBakeSpace, b.sourcePicnum, b.resolvedVoxelIndex,
		b.primitiveCount, b.model, b.sharedVariantSurface, b.desiredPending, b.directOnlyAdmission) ||
		!SameSurface(a.surface, b.surface, full || a.geometryContentHash == 0 || b.geometryContentHash == 0) ||
		!SameSurface(a.lightSurface, b.lightSurface, full) || !SameSurface(&a.materialSurface, &b.materialSurface, full))
		changes |= VoxelPublicationBinding;
	if (std::tie(a.placementGeneration, a.placementStateHash, a.physicalSectorIndex,
		a.surfaceSignature, a.transformBasisSignature, a.indirectOnly) !=
		std::tie(b.placementGeneration, b.placementStateHash, b.physicalSectorIndex,
		b.surfaceSignature, b.transformBasisSignature, b.indirectOnly) ||
		std::memcmp(a.instanceTransform, b.instanceTransform, sizeof(a.instanceTransform)) != 0 ||
		std::memcmp(a.currentTranslation, b.currentTranslation, sizeof(a.currentTranslation)) != 0 ||
		std::memcmp(a.bakedTranslation, b.bakedTranslation, sizeof(a.bakedTranslation)) != 0)
		changes |= VoxelPublicationPresentation;
	return changes;
}

bool SameVoxelPublicationEntry(const PersistentVoxelCacheEntryView& a,
	const PersistentVoxelCacheEntryView& b, bool full)
{
	return CompareVoxelPublicationEntry(a, b, full) == 0 && a.lastSeenFrame == b.lastSeenFrame &&
		a.retainedFrameAge == b.retainedFrameAge && a.capturedThisFrame == b.capturedThisFrame;
}

void SetVoxelPublicationCaptureFrame(PersistentVoxelCacheEntryView& entry, uint64_t captureFrame)
{
	entry.capturedThisFrame = entry.lastSeenFrame == captureFrame;
	entry.retainedFrameAge = entry.lastSeenFrame != 0 && captureFrame >= entry.lastSeenFrame ? captureFrame - entry.lastSeenFrame : 0;
}

VoxelPublicationPresentationData GetVoxelPublicationPresentation(const PersistentVoxelCacheEntryView& entry)
{
	VoxelPublicationPresentationData result;
	result.placementGeneration = entry.placementGeneration; result.placementStateHash = entry.placementStateHash;
	result.surfaceSignature = entry.surfaceSignature; result.transformBasisSignature = entry.transformBasisSignature;
	result.physicalSectorIndex = entry.physicalSectorIndex; result.indirectOnly = entry.indirectOnly;
	std::copy(std::begin(entry.instanceTransform), std::end(entry.instanceTransform), std::begin(result.instanceTransform));
	std::copy(std::begin(entry.currentTranslation), std::end(entry.currentTranslation), std::begin(result.currentTranslation));
	std::copy(std::begin(entry.bakedTranslation), std::end(entry.bakedTranslation), std::begin(result.bakedTranslation));
	return result;
}

void ApplyVoxelPublicationPresentation(const VoxelPublicationPresentationData& source, PersistentVoxelCacheEntryView& entry)
{
	entry.placementGeneration = source.placementGeneration; entry.placementStateHash = source.placementStateHash;
	entry.surfaceSignature = source.surfaceSignature; entry.transformBasisSignature = source.transformBasisSignature;
	entry.physicalSectorIndex = source.physicalSectorIndex; entry.indirectOnly = source.indirectOnly;
	std::copy(std::begin(source.instanceTransform), std::end(source.instanceTransform), std::begin(entry.instanceTransform));
	std::copy(std::begin(source.currentTranslation), std::end(source.currentTranslation), std::begin(entry.currentTranslation));
	std::copy(std::begin(source.bakedTranslation), std::end(source.bakedTranslation), std::begin(entry.bakedTranslation));
}

void VoxelActorPublication::SetSurfaceSource(const SurfaceRef*& currentSource, const SurfaceRef* source)
{
	if (currentSource == source) return;
	if (currentSource)
	{
		auto found = sharedSurfaces.find(currentSource);
		if (found != sharedSurfaces.end() && --found->second.users == 0) sharedSurfaces.erase(found);
	}
	currentSource = source;
	if (source) ++sharedSurfaces[source].users;
}

std::shared_ptr<const SurfaceRef> VoxelActorPublication::OwnSurface(const SurfaceRef* source,
	uint64_t geometryHash, uint64_t primitiveHash)
{
	if (!source) return {};
	auto& cached = sharedSurfaces.at(source);
	auto backing = cached.backing.lock();
	if (backing && cached.geometryHash == geometryHash && cached.primitiveHash == primitiveHash &&
		SameSurface(source, backing.get(), geometryHash == 0 || primitiveHash == 0)) return backing;
	// Canonical geometry is copied once per current source/content, then shared
	// by all actor bindings. Material/light-only changes retain unchanged meshes.
	backing = std::make_shared<SurfaceRef>(*source);
	cached.backing = backing;
	cached.geometryHash = geometryHash;
	cached.primitiveHash = primitiveHash;
	++stats.surfaceCopies;
	return backing;
}

std::shared_ptr<const VoxelPublicationEntry> VoxelActorPublication::OwnEntry(
	const PersistentVoxelCacheEntryView& view, CurrentEntry& state, bool bindingChanged)
{
	auto result = std::make_shared<VoxelPublicationEntry>();
	result->view = view;
	if (state.entry && !bindingChanged)
	{
		result->surface = state.entry->surface;
		result->lightSurface = state.entry->lightSurface;
	}
	else
	{
		SetSurfaceSource(state.surfaceSource, view.surface);
		SetSurfaceSource(state.lightSurfaceSource, view.lightSurface);
		result->surface = OwnSurface(view.surface, view.geometryContentHash, view.renderPrimitiveHash);
		result->lightSurface = OwnSurface(view.lightSurface, 0, 0);
	}
	result->view.surface = result->surface.get();
	result->view.lightSurface = result->lightSurface.get();
	return result;
}

void VoxelActorPublication::MarkDirty(uint64_t identity)
{
	if (identity == 0) return;
	auto& state = current[identity];
	state.removed = false;
	if (!state.dirty)
	{
		state.dirty = true;
		dirty.push_back(identity);
	}
}

void VoxelActorPublication::Remove(uint64_t identity)
{
	const auto found = current.find(identity);
	if (found == current.end()) return;
	if (!found->second.dirty) dirty.push_back(identity);
	found->second.dirty = true;
	found->second.removed = true;
}

void VoxelActorPublication::InvalidateAll(bool authorityChanged)
{
	for (auto& pair : current)
	{
		if (!pair.second.dirty)
		{
			pair.second.dirty = true;
			dirty.push_back(pair.first);
		}
	}
	if (authorityChanged) forcedChanges |= VoxelPublicationAuthority;
}

void VoxelActorPublication::Reset()
{
	current.clear();
	sharedSurfaces.clear();
	dirty.clear();
	externalReadiness.clear();
	snapshot.reset();
	forcedChanges = VoxelPublicationTopology | VoxelPublicationBinding | VoxelPublicationPresentation | VoxelPublicationAuthority;
	// Never reuse an epoch even when resetting an already empty world.
	Advance(generations.revision);
}

void VoxelActorPublication::InvalidateExternalReadiness()
{
	// Direct GPU readiness/content is published outside the actor cache. Never
	// infer it from the actor serial. Unpublished candidates also retry promotion.
	for (uint64_t identity : externalReadiness)
	{
		auto found = current.find(identity);
		if (found != current.end() && !found->second.removed && !found->second.dirty)
		{
			found->second.dirty = true;
			dirty.push_back(identity);
		}
	}
}

void VoxelActorPublication::SetExternalReadiness(uint64_t identity, bool needed)
{
	auto& state = current.at(identity);
	if (needed && state.externalIndex == UINT32_MAX)
	{
		state.externalIndex = (uint32_t)externalReadiness.size();
		externalReadiness.push_back(identity);
	}
	else if (!needed && state.externalIndex != UINT32_MAX)
	{
		const uint32_t index = state.externalIndex;
		const uint64_t replacement = externalReadiness.back();
		externalReadiness[index] = replacement;
		current.at(replacement).externalIndex = index;
		externalReadiness.pop_back();
		state.externalIndex = UINT32_MAX;
	}
}

std::shared_ptr<const VoxelActorPublicationSnapshot> VoxelActorPublication::Publish(uint64_t frame, const BuildEntry& build)
{
	const uint32_t highWater = stats.highWater;
	stats = {};
	stats.highWater = std::max(highWater, (uint32_t)current.size());
	stats.sourceEntries = (uint32_t)current.size();
	stats.sharedSurfaceSources = (uint32_t)sharedSurfaces.size();
	stats.retained = snapshot ? (uint32_t)snapshot->identities->size() : 0;
	if (snapshot && dirty.empty() && forcedChanges == 0 && snapshot->captureFrame == frame) return snapshot;
	auto next = std::make_shared<VoxelActorPublicationSnapshot>();
	next->baseRevision = snapshot ? snapshot->generations.revision : 0;
	next->captureFrame = frame;
	next->deltas.reserve(dirty.size());
	uint32_t changes = forcedChanges;
	forcedChanges = 0;
	if (!snapshot || snapshot->captureFrame != frame) changes |= VoxelPublicationPresentation;
	for (uint64_t identity : dirty)
	{
		auto found = current.find(identity);
		if (found == current.end()) continue;
		auto& state = found->second;
		state.dirty = false;
		PersistentVoxelCacheEntryView view;
		++stats.visited;
		const bool eligible = !state.removed && build(identity, view);
		if (!eligible)
		{
			SetExternalReadiness(identity, !state.removed);
			if (state.entry)
			{
				next->deltas.push_back({identity, 0, VoxelPublicationTopology | VoxelPublicationAuthority, true, {}});
				changes |= VoxelPublicationTopology | VoxelPublicationAuthority;
				state.entry.reset();
				SetSurfaceSource(state.surfaceSource, nullptr);
				SetSurfaceSource(state.lightSurfaceSource, nullptr);
				++stats.removed;
			}
			if (state.removed) current.erase(found);
			continue;
		}
		SetExternalReadiness(identity, view.desiredPending || view.geometryContentHash == 0 || view.renderPrimitiveHash == 0);
		uint32_t changed = VoxelPublicationTopology | VoxelPublicationBinding | VoxelPublicationPresentation | VoxelPublicationAuthority;
		if (state.entry)
		{
			PersistentVoxelCacheEntryView previous = state.entry->view;
			ApplyVoxelPublicationPresentation(state.presentation, previous);
			changed = CompareVoxelPublicationEntry(previous, view);
		}
		std::shared_ptr<const VoxelPublicationEntry> replacement;
		if (changed != 0)
		{
			if ((changed & ~VoxelPublicationPresentation) != 0)
			{
				replacement = OwnEntry(view, state, (changed & VoxelPublicationBinding) != 0);
				state.entry = replacement;
			}
			++stats.changed;
			stats.bindings += (changed & VoxelPublicationBinding) != 0;
			stats.transforms += (changed & VoxelPublicationPresentation) != 0;
		}
		state.presentation = GetVoxelPublicationPresentation(view);
		const bool presentationChanged = (changed & VoxelPublicationPresentation) != 0;
		if (state.lastSeenFrame != view.lastSeenFrame) changed |= VoxelPublicationPresentation;
		state.lastSeenFrame = view.lastSeenFrame;
		if (changed != 0) next->deltas.push_back({identity, state.lastSeenFrame, changed, false, replacement, state.presentation, presentationChanged});
		changes |= changed;
	}
	dirty.clear();
	if (!snapshot || (changes & VoxelPublicationTopology) != 0)
	{
		auto identities = std::make_shared<std::vector<uint64_t>>();
		identities->reserve(current.size());
		for (const auto& pair : current) if (pair.second.entry) identities->push_back(pair.first);
		std::sort(identities->begin(), identities->end());
		next->identities = identities;
		++stats.topologySorts;
	}
	else next->identities = snapshot->identities;
	Advance(generations.revision);
	if ((changes & VoxelPublicationTopology) != 0) Advance(generations.topology);
	if ((changes & VoxelPublicationBinding) != 0) Advance(generations.binding);
	if ((changes & VoxelPublicationPresentation) != 0) Advance(generations.presentation);
	if ((changes & VoxelPublicationAuthority) != 0) Advance(generations.authority);
	next->generations = generations;
	stats.retained = (uint32_t)next->identities->size();
	stats.sourceEntries = (uint32_t)current.size();
	stats.sharedSurfaceSources = (uint32_t)sharedSurfaces.size();
	snapshot = std::move(next);
	return snapshot;
}

std::shared_ptr<const VoxelPublicationEntry> VoxelActorPublication::Find(uint64_t identity, uint64_t& lastSeen,
	VoxelPublicationPresentationData* presentation) const
{
	const auto found = current.find(identity);
	if (found == current.end()) return {};
	lastSeen = found->second.lastSeenFrame;
	if (presentation) *presentation = found->second.presentation;
	return found->second.entry;
}

void VoxelActorPublication::CopyEntries(std::vector<PersistentVoxelCacheEntryView>& out) const
{
	out.clear();
	if (!snapshot) return;
	out.reserve(snapshot->identities->size());
	for (uint64_t identity : *snapshot->identities)
	{
		uint64_t lastSeen = 0;
		VoxelPublicationPresentationData presentation;
		auto entry = Find(identity, lastSeen, &presentation);
		if (!entry) continue;
		out.push_back(entry->view);
		ApplyVoxelPublicationPresentation(presentation, out.back());
		out.back().lastSeenFrame = lastSeen;
		SetVoxelPublicationCaptureFrame(out.back(), snapshot->captureFrame);
	}
}

bool VoxelActorPublicationCursor::Synchronize(const VoxelActorPublication& owner, const VoxelActorPublicationSnapshot& snapshot)
{
	lastPatched = 0;
	accepted = owner.IsCurrent(snapshot);
	if (!accepted) return false; // Do not combine an old topology with newer bindings.
	const bool resync = !initialized || generations.topology != snapshot.generations.topology ||
		(generations.revision != snapshot.baseRevision && generations.revision != snapshot.generations.revision);
	if (resync)
	{
		entries.clear(); owners.clear(); indices.clear();
		entries.reserve(snapshot.identities->size()); owners.reserve(snapshot.identities->size());
		indices.reserve(snapshot.identities->size());
		for (uint64_t identity : *snapshot.identities)
		{
			uint64_t lastSeen = 0;
			VoxelPublicationPresentationData presentation;
			auto entry = owner.Find(identity, lastSeen, &presentation);
			if (!entry) continue;
			indices.emplace(identity, (uint32_t)entries.size());
			owners.push_back(entry);
			entries.push_back(entry->view);
			ApplyVoxelPublicationPresentation(presentation, entries.back());
			entries.back().lastSeenFrame = lastSeen;
			++lastPatched;
		}
	}
	else if (generations.revision != snapshot.generations.revision)
	{
		for (const auto& delta : snapshot.deltas)
		{
			const auto found = indices.find(delta.identity);
			if (found == indices.end() || delta.removed) continue; // Topology changes took resync above.
			const uint32_t index = found->second;
			if (delta.entry)
			{
				owners[index] = delta.entry;
				entries[index] = delta.entry->view;
			}
			if (delta.presentationChanged || delta.entry)
				ApplyVoxelPublicationPresentation(delta.presentation, entries[index]);
			entries[index].lastSeenFrame = delta.lastSeenFrame;
			++lastPatched;
		}
	}
	for (auto& entry : entries) SetVoxelPublicationCaptureFrame(entry, snapshot.captureFrame);
	generations = snapshot.generations;
	initialized = true;
	return resync;
}

uint32_t VoxelActorPublicationCursor::FindIndex(uint64_t identity) const
{
	const auto found = indices.find(identity);
	return found != indices.end() ? found->second : UINT32_MAX;
}

void VoxelActorPublicationCursor::Reset()
{
	entries.clear(); owners.clear(); indices.clear(); initialized = false; accepted = false; generations = {}; lastPatched = 0;
}
}
