#include "nri_persistent_voxel_material_payload.h"
#include "../scene/nri_material_payload_semantics.h"

#include <algorithm>

namespace
{
bool SameCertificate(const NRIVoxelMaterialSharingCertificate& a, const NRIVoxelMaterialSharingCertificate& b)
{
	return a.world == b.world && a.rules == b.rules && a.invariant == b.invariant && a.actorWorld == b.actorWorld;
}
bool SameRange(NRIPersistentVoxelMaterialRangeHandle a, NRIPersistentVoxelMaterialRangeHandle b)
{
	return a.offset == b.offset && a.capacity == b.capacity && a.generation == b.generation;
}
}

bool NRIVoxelMaterialPayloadOwner::AcquireBinding(uint64_t key, const NRIVoxelMaterialSharingCertificate& certificate,
	const nri_scene::MaterialBridgeData& source, NRIPersistentVoxelMaterialRangeAllocator& allocator,
	NRIPersistentVoxelMaterialRangeHandle& result, NRIPersistentVoxelMaterialRangeHandle& retired)
{
	result = {}; retired = {};
	if (!key || source.materials.empty() || source.materials.size() > UINT32_MAX) return false;
	++counters.lookups;
	const auto previous = bindings.find(key);
	if (previous != bindings.end())
	{
		auto& payload = payloads.at(previous->second);
		// A private binding is the sole authority for its existing range. Keep
		// the original same-size reuse behavior without duplicating its CPU bridge.
		const bool privateReuse = !payload.certificate.invariant && !certificate.invariant &&
			payload.aliases == 1 && payload.range.capacity == source.materials.size();
		bool equivalent = false;
		if (!privateReuse && SameCertificate(payload.certificate, certificate))
		{
			++counters.comparisons;
			equivalent = nri_scene::MaterialPayloadsEquivalent(payload.bridge, source);
		}
		if (privateReuse || equivalent)
		{
			payload.certificate = certificate;
			const int32_t actor = source.lightMetadata.empty() ? -1 : source.lightMetadata.front().actorIndex;
			if (bindingActors.at(key) != actor)
			{
				auto& old = actorBindings.at(bindingActors.at(key));
				old.erase(key);
				if (old.empty()) actorBindings.erase(bindingActors.at(key));
				bindingActors[key] = actor;
				actorBindings[actor].insert(key);
			}
			result = payload.range;
			return true;
		}
	}
	uint64_t bytesHashed = 0;
	const uint64_t hash = certificate.invariant ?
		(hashFunction ? hashFunction : nri_scene::HashMaterialPayload)(source, &bytesHashed) : 0;
	counters.bytesHashed += bytesHashed;
	uint64_t identity = 0;
	if (certificate.invariant)
	{
		const auto candidates = lookup.equal_range(hash);
		for (auto it = candidates.first; it != candidates.second; ++it)
		{
			const auto& payload = payloads.at(it->second);
			++counters.comparisons;
			if (SameCertificate(payload.certificate, certificate) &&
				nri_scene::MaterialPayloadsEquivalent(payload.bridge, source))
			{
				identity = it->second;
				++counters.hits;
				break;
			}
		}
	}
	if (!identity)
	{
		if (!nextIdentity || nextIdentity == UINT64_MAX) return false;
		const auto range = allocator.Allocate((uint32_t)source.materials.size());
		if (!range) return false; // The old alias is still valid on failure.
		Payload payload;
		payload.range = range;
		payload.certificate = certificate;
		if (certificate.invariant)
		{
			payload.bridge = source;
			for (auto& metadata : payload.bridge.lightMetadata) metadata.actorIndex = -1;
		}
		payload.hash = hash;
		identity = nextIdentity++;
		payloads.emplace(identity, std::move(payload));
		ranges.emplace(range.generation, identity);
		if (certificate.invariant) lookup.emplace(hash, identity);
	}
	retired = ReleaseBinding(key);
	bindings.emplace(key, identity);
	const int32_t actor = source.lightMetadata.empty() ? -1 : source.lightMetadata.front().actorIndex;
	bindingActors.emplace(key, actor);
	actorBindings[actor].insert(key);
	auto& payload = payloads.at(identity);
	++payload.aliases;
	result = payload.range;
	return true;
}

NRIPersistentVoxelMaterialRangeHandle NRIVoxelMaterialPayloadOwner::FindBinding(uint64_t key) const
{
	const auto binding = bindings.find(key);
	return binding == bindings.end() ? NRIPersistentVoxelMaterialRangeHandle{} : payloads.at(binding->second).range;
}

NRIPersistentVoxelMaterialRangeHandle NRIVoxelMaterialPayloadOwner::ReleaseBinding(uint64_t key)
{
	const auto binding = bindings.find(key);
	if (binding == bindings.end()) return {};
	const uint64_t identity = binding->second;
	const int32_t actor = bindingActors.at(key);
	auto& actorKeys = actorBindings.at(actor);
	actorKeys.erase(key);
	if (actorKeys.empty()) actorBindings.erase(actor);
	bindingActors.erase(key);
	bindings.erase(binding);
	auto& payload = payloads.at(identity);
	if (--payload.aliases != 0) return {};
	const auto candidates = lookup.equal_range(payload.hash);
	for (auto it = candidates.first; it != candidates.second; ++it)
	{
		if (it->second == identity) { lookup.erase(it); break; }
	}
	return payload.range;
}

std::vector<uint64_t> NRIVoxelMaterialPayloadOwner::BindingsForActor(int32_t actorIndex) const
{
	const auto found = actorBindings.find(actorIndex);
	return found == actorBindings.end() ? std::vector<uint64_t>{} :
		std::vector<uint64_t>(found->second.begin(), found->second.end());
}

bool NRIVoxelMaterialPayloadOwner::BindingIsShared(uint64_t key) const
{
	const auto found = bindings.find(key);
	return found != bindings.end() && payloads.at(found->second).aliases > 1;
}

bool NRIVoxelMaterialPayloadOwner::ReleaseRetiredRange(NRIPersistentVoxelMaterialRangeHandle range,
	NRIPersistentVoxelMaterialRangeAllocator& allocator)
{
	const auto indexed = ranges.find(range.generation);
	if (indexed == ranges.end()) return false;
	const auto payload = payloads.find(indexed->second);
	if (payload == payloads.end() || payload->second.aliases ||
		!SameRange(payload->second.range, range) || !allocator.Release(range)) return false;
	payloads.erase(payload);
	ranges.erase(indexed);
	return true;
}

bool NRIVoxelMaterialPayloadOwner::Compact(NRIPersistentVoxelMaterialRangeAllocator& allocator, uint64_t& movedRows)
{
	movedRows = 0;
	std::vector<Payload*> ordered;
	for (auto& entry : payloads)
	{
		if (!entry.second.aliases || !allocator.Owns(entry.second.range)) return false;
		ordered.push_back(&entry.second);
	}
	std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->range.offset < b->range.offset; });
	// Quiescent compaction is uncommon. Plan against an allocator copy so even
	// generation/row exhaustion cannot destructively invalidate live aliases.
	auto planned = allocator;
	for (auto it = ordered.rbegin(); it != ordered.rend(); ++it)
		if (!planned.Release((*it)->range)) return false;
	std::vector<NRIPersistentVoxelMaterialRangeHandle> replacements;
	for (auto* payload : ordered)
	{
		const auto replacement = planned.Allocate(payload->range.capacity);
		if (!replacement) return false;
		replacements.push_back(replacement);
	}
	allocator = std::move(planned);
	ranges.clear();
	for (size_t i = 0; i < ordered.size(); ++i)
	{
		if (ordered[i]->range.offset != replacements[i].offset) movedRows += ordered[i]->range.capacity;
		ordered[i]->range = replacements[i];
	}
	for (const auto& entry : payloads) ranges.emplace(entry.second.range.generation, entry.first);
	return true;
}

NRIVoxelMaterialPayloadStats NRIVoxelMaterialPayloadOwner::Snapshot() const
{
	auto stats = counters;
	stats.bindings = bindings.size();
	for (const auto& entry : payloads)
	{
		const auto& p = entry.second;
		const uint64_t bytes = p.bridge.materials.capacity() * sizeof(nri_scene::MaterialData) +
			p.bridge.lightMetadata.capacity() * sizeof(nri_scene::MaterialLightingMetadata) +
			p.bridge.textures.capacity() * sizeof(nri_scene::TextureUpload);
		if (!p.aliases) { ++stats.retiring; stats.retiringRows += p.range.capacity; stats.retiringDescriptorBytes += bytes; continue; }
		stats.descriptorBytes += bytes;
		++stats.payloads;
		stats.physicalRows += p.range.capacity;
		stats.aliasRows += (p.aliases - 1) * p.bridge.materials.size();
		if (p.certificate.invariant) stats.eligibleBindings += p.aliases;
		else { stats.excludedBindings += p.aliases; stats.excludedRows += p.aliases * p.range.capacity; }
	}
	return stats;
}

void NRIVoxelMaterialPayloadOwner::Reset()
{
	bindings.clear(); bindingActors.clear(); actorBindings.clear();
	payloads.clear(); lookup.clear(); ranges.clear(); counters = {};
	// Keep the monotonic identity sequence across resets, like range generations.
}
