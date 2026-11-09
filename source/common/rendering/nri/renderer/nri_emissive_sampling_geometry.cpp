#include "nri_emissive_sampling_geometry.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_hash.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace
{
	using Clock = std::chrono::steady_clock;
	double ElapsedMs(Clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
	}
	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}
	float ComputePrimitiveArea(const nri_scene::GeometryData& geometry, uint32_t primitiveIndex)
	{
		if (primitiveIndex >= geometry.primitives.size())
		{
			return 0.0f;
		}

		const auto& primitive = geometry.primitives[primitiveIndex];
		if (primitive.indices[0] >= geometry.vertices.size() ||
			primitive.indices[1] >= geometry.vertices.size() ||
			primitive.indices[2] >= geometry.vertices.size())
		{
			return 0.0f;
		}

		const auto& a = geometry.vertices[primitive.indices[0]];
		const auto& b = geometry.vertices[primitive.indices[1]];
		const auto& c = geometry.vertices[primitive.indices[2]];
		const float abx = b.position[0] - a.position[0];
		const float aby = b.position[1] - a.position[1];
		const float abz = b.position[2] - a.position[2];
		const float acx = c.position[0] - a.position[0];
		const float acy = c.position[1] - a.position[1];
		const float acz = c.position[2] - a.position[2];
		const float crossX = aby * acz - abz * acy;
		const float crossY = abz * acx - abx * acz;
		const float crossZ = abx * acy - aby * acx;
		return 0.5f * std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
	}

	void ComputePrimitiveBounds(const nri_scene::GeometryData& geometry, uint32_t primitiveIndex, float outCenter[3], float& outRadius)
	{
		outCenter[0] = 0.0f;
		outCenter[1] = 0.0f;
		outCenter[2] = 0.0f;
		outRadius = 0.0f;
		if (primitiveIndex >= geometry.primitives.size())
		{
			return;
		}

		const auto& primitive = geometry.primitives[primitiveIndex];
		if (primitive.indices[0] >= geometry.vertices.size() ||
			primitive.indices[1] >= geometry.vertices.size() ||
			primitive.indices[2] >= geometry.vertices.size())
		{
			return;
		}

		const auto& a = geometry.vertices[primitive.indices[0]];
		const auto& b = geometry.vertices[primitive.indices[1]];
		const auto& c = geometry.vertices[primitive.indices[2]];
		outCenter[0] = (a.position[0] + b.position[0] + c.position[0]) / 3.0f;
		outCenter[1] = (a.position[1] + b.position[1] + c.position[1]) / 3.0f;
		outCenter[2] = (a.position[2] + b.position[2] + c.position[2]) / 3.0f;
		for (const auto* vertex : { &a, &b, &c })
		{
			const float dx = vertex->position[0] - outCenter[0];
			const float dy = vertex->position[1] - outCenter[1];
			const float dz = vertex->position[2] - outCenter[2];
			outRadius = std::max(outRadius, std::sqrt(dx * dx + dy * dy + dz * dz));
		}
	}

	uint64_t HashGeometryForEmissiveSampling(const nri_scene::GeometryData* geometry)
	{
		uint64_t hash = 1469598103934665603ull;
		if (geometry == nullptr)
		{
			return nri_scene::HashCombine64(hash, 0ull);
		}

		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry->vertices.size());
		hash = nri_scene::HashCombine64(hash, (uint64_t)geometry->primitives.size());
		for (const nri_scene::SceneVertex& vertex : geometry->vertices)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(vertex.position[0]));
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(vertex.position[1]));
			hash = nri_scene::HashCombine64(hash, (uint64_t)FloatBits(vertex.position[2]));
		}

		for (const nri_scene::PrimitiveData& primitive : geometry->primitives)
		{
			hash = nri_scene::HashCombine64(hash, (uint64_t)primitive.indices[0]);
			hash = nri_scene::HashCombine64(hash, (uint64_t)primitive.indices[1]);
			hash = nri_scene::HashCombine64(hash, (uint64_t)primitive.indices[2]);
			hash = nri_scene::HashCombine64(hash, (uint64_t)primitive.materialIndex);
		}

		return hash;
	}

}

void NRIEmissiveGeometryCache::Reset()
{
	mDomains = {};
	mStats = {};
	mCollectTiming = false;
}

void NRIEmissiveGeometryCache::BeginUpdate(bool collectTiming)
{
	mStats = {};
	mCollectTiming = collectTiming;
	for (const auto& state : mDomains)
		mStats.quarantinedDomains += state.quarantined ? 1u : 0u;
}

uint64_t NRIEmissiveGeometryCache::PublishTransientIdentity()
{
	static uint64_t publicationSerial = 0;
	return ++publicationSerial;
}

uint64_t NRIEmissiveGeometryCache::ResolveSnapshot(DomainState& state,
	const nri_scene::GeometryData* geometry)
{
	// Observe every sampling field at consumption. Transient publication stamps
	// also change for UV/history writes and byte-identical recaptures; they do
	// not certify sampling content. Exact snapshots cover all existing producers.
	const size_t vertexCount = geometry != nullptr ? geometry->vertices.size() : 0;
	const size_t primitiveCount = geometry != nullptr ? geometry->primitives.size() : 0;
	const bool presenceChanged = !state.snapshotValid || state.snapshotPresent != (geometry != nullptr);
	bool positionsChanged = presenceChanged || state.positions.size() != vertexCount;
	bool connectivityChanged = presenceChanged || state.topology.size() != primitiveCount;
	bool materialsChanged = connectivityChanged;
	const size_t oldPositionCount = state.positions.size();
	const size_t oldTopologyCount = state.topology.size();
	const size_t oldPositionCapacity = state.positions.capacity();
	const size_t oldTopologyCapacity = state.topology.capacity();
	state.positions.resize(vertexCount);
	state.topology.resize(primitiveCount);
	if (state.snapshotValid)
		mStats.snapshotComparedBytes += std::min(vertexCount, oldPositionCount) * sizeof(SamplingPosition) +
			std::min(primitiveCount, oldTopologyCount) * sizeof(SamplingPrimitive);
	for (size_t i = 0; i < vertexCount; ++i)
	{
		SamplingPosition current;
		std::memcpy(current.bits, geometry->vertices[i].position, sizeof(current.bits));
		const bool existing = state.snapshotValid && i < oldPositionCount;
		if (!existing || std::memcmp(current.bits, state.positions[i].bits, sizeof(current.bits)) != 0)
		{
			positionsChanged = true;
			state.positions[i] = current;
			mStats.snapshotWrittenBytes += sizeof(current.bits);
		}
	}
	for (size_t i = 0; i < primitiveCount; ++i)
	{
		const auto& source = geometry->primitives[i];
		auto& previous = state.topology[i];
		const bool existing = state.snapshotValid && i < oldTopologyCount;
		if (!existing || std::memcmp(source.indices, previous.indices, sizeof(previous.indices)) != 0)
		{
			connectivityChanged = true;
			std::memcpy(previous.indices, source.indices, sizeof(previous.indices));
			mStats.snapshotWrittenBytes += sizeof(previous.indices);
		}
		if (!existing || previous.material != source.materialIndex)
		{
			materialsChanged = true;
			previous.material = source.materialIndex;
			mStats.snapshotWrittenBytes += sizeof(previous.material);
		}
	}
	const auto growth = [&](size_t before, size_t after, size_t stride)
	{
		if (after > before)
		{
			++mStats.capacityGrowths;
			mStats.capacityGrowthBytes += (after - before) * stride;
		}
	};
	growth(oldPositionCapacity, state.positions.capacity(), sizeof(SamplingPosition));
	growth(oldTopologyCapacity, state.topology.capacity(), sizeof(SamplingPrimitive));
	++mStats.snapshotChecks;
	if (positionsChanged || connectivityChanged || materialsChanged)
		state.snapshotIdentity = PublishTransientIdentity();
	else ++mStats.snapshotHits;
	// Ranges depend on material membership/order, not positions/connectivity.
	// Primitive facts depend on positions/connectivity, not material identity.
	if (materialsChanged) state.snapshotRangesIdentity = PublishTransientIdentity();
	if (positionsChanged || connectivityChanged) state.snapshotPrimitiveIdentity = PublishTransientIdentity();
	if (positionsChanged && !connectivityChanged && !materialsChanged)
		++mStats.snapshotPositionOnlyChanges;
	state.snapshotValid = true;
	state.snapshotPresent = geometry != nullptr;
	state.rangesIdentity = state.snapshotRangesIdentity;
	state.primitiveIdentity = state.snapshotPrimitiveIdentity;
	return nri_scene::HashCombine64(0x454d4953534e4150ull, state.snapshotIdentity);
}

uint64_t NRIEmissiveGeometryCache::ResolveIdentity(NRIEmissiveGeometryDomain domain,
	const nri_scene::GeometryData* geometry, uint64_t producerIdentity, bool validate)
{
	const auto start = mCollectTiming ? Clock::now() : Clock::time_point{};
	const double previousHashMs = mStats.validationHashMs;
	auto& state = mDomains[(size_t)domain];
	// Keep existing certified static/retained producer identities. Transient
	// producers get exact sampling-only identities independent of their stamps.
	const bool transientContent = domain == NRIEmissiveGeometryDomain::Captured ||
		domain == NRIEmissiveGeometryDomain::RuntimeMutation || domain == NRIEmissiveGeometryDomain::Dynamic;
	uint64_t identity = nri_scene::HashCombine64(0x454d495347454f4dull, producerIdentity);
	identity = nri_scene::HashCombine64(identity, geometry != nullptr ? geometry->vertices.size() : 0);
	identity = nri_scene::HashCombine64(identity, geometry != nullptr ? geometry->primitives.size() : 0);
	identity = nri_scene::HashCombine64(identity, geometry != nullptr ? 1u : 0u);
	const bool useSnapshot = transientContent && producerIdentity != 0 && !state.quarantined;
	if (useSnapshot) identity = ResolveSnapshot(state, geometry);
	if (validate || producerIdentity == 0 || state.quarantined)
	{
		const auto validationStart = mCollectTiming ? Clock::now() : Clock::time_point{};
		const uint64_t exactHash = HashGeometry(geometry);
		if (geometry != nullptr)
		{
			mStats.hashedVertices += geometry->vertices.size();
			mStats.hashedPrimitives += geometry->primitives.size();
		}
		if (validate && producerIdentity != 0)
		{
			mStats.identityChecks++;
			if (state.validationValid && state.validatedIdentity == identity && state.validatedHash != exactHash)
			{
				mStats.identityMismatches++;
				if (!state.quarantined) mStats.quarantinedDomains++;
				state.quarantined = true;
			}
			state.validationValid = true;
			state.validatedIdentity = identity;
			state.validatedHash = exactHash;
		}
		if (producerIdentity == 0 || state.quarantined)
			identity = nri_scene::HashCombine64(0x454d495346554c4cull, exactHash);
		if (mCollectTiming) mStats.validationHashMs += ElapsedMs(validationStart);
	}
	// Keep the last checked identity across diagnostic-off intervals. Reusing a
	// producer identity still promises the same geometry when checks resume.
	// On a first check, keep cached topology intact so the full-payload shadow
	// comparison can detect stale geometry from previously unchecked frames.
	state.identity = identity;
	if (!useSnapshot || state.quarantined)
	{
		state.rangesIdentity = identity;
		state.primitiveIdentity = identity;
	}
	if (mCollectTiming) mStats.identityMs += ElapsedMs(start) - (mStats.validationHashMs - previousHashMs);
	return identity;
}

const std::vector<NRIEmissiveMaterialPrimitiveRange>& NRIEmissiveGeometryCache::Ranges(
	NRIEmissiveGeometryDomain domain, const nri_scene::GeometryData* geometry)
{
	const auto start = mCollectTiming ? Clock::now() : Clock::time_point{};
	auto& state = mDomains[(size_t)domain];
	const auto oldPrimitiveCapacity = state.primitives.capacity();
	if (state.cachedPrimitiveIdentity != state.primitiveIdentity ||
		state.primitives.size() != (geometry != nullptr ? geometry->primitives.size() : 0))
	{
		state.primitives.clear();
		state.primitives.resize(geometry != nullptr ? geometry->primitives.size() : 0);
		state.cachedPrimitiveIdentity = state.primitiveIdentity;
		++mStats.primitiveInvalidations;
	}
	if (state.primitives.capacity() > oldPrimitiveCapacity)
	{
		mStats.capacityGrowths++;
		mStats.capacityGrowthBytes += (state.primitives.capacity() - oldPrimitiveCapacity) * sizeof(NRIEmissivePrimitiveGeometry);
	}
	if (state.topologyValid && state.topologyIdentity == state.rangesIdentity)
	{
		mStats.topologyReuses++;
		if (mCollectTiming) mStats.topologyMs += ElapsedMs(start);
		return state.ranges;
	}
	const auto oldRangeCapacity = state.ranges.capacity();
	BuildRanges(geometry, state.ranges);
	if (state.ranges.capacity() > oldRangeCapacity)
	{
		mStats.capacityGrowths++;
		mStats.capacityGrowthBytes += (state.ranges.capacity() - oldRangeCapacity) * sizeof(NRIEmissiveMaterialPrimitiveRange);
	}
	if (geometry != nullptr)
	{
		auto& scanned = domain == NRIEmissiveGeometryDomain::Static ?
			mStats.staticPrimitivesScanned : mStats.dynamicPrimitivesScanned;
		scanned += 2u * geometry->primitives.size();
	}
	state.topologyIdentity = state.rangesIdentity;
	state.topologyValid = true;
	mStats.topologyBuilds++;
	if (mCollectTiming) mStats.topologyMs += ElapsedMs(start);
	return state.ranges;
}

const NRIEmissivePrimitiveGeometry& NRIEmissiveGeometryCache::Primitive(
	NRIEmissiveGeometryDomain domain, const nri_scene::GeometryData& geometry, uint32_t primitiveIndex)
{
	auto& primitive = mDomains[(size_t)domain].primitives[primitiveIndex];
	if (!primitive.valid)
	{
		const auto start = mCollectTiming ? Clock::now() : Clock::time_point{};
		primitive = BuildPrimitive(geometry, primitiveIndex);
		mStats.primitiveBuilds++;
		if (mCollectTiming) mStats.topologyMs += ElapsedMs(start);
	}
	else mStats.primitiveReuses++;
	return primitive;
}

uint64_t NRIEmissiveGeometryCache::CapacityBytes() const
{
	uint64_t bytes = SnapshotCapacityBytes();
	for (const auto& state : mDomains)
		bytes += state.ranges.capacity() * sizeof(NRIEmissiveMaterialPrimitiveRange) +
			state.primitives.capacity() * sizeof(NRIEmissivePrimitiveGeometry);
	return bytes;
}

uint64_t NRIEmissiveGeometryCache::SnapshotCapacityBytes() const
{
	uint64_t bytes = 0;
	for (const auto& state : mDomains)
		bytes += state.positions.capacity() * sizeof(SamplingPosition) +
			state.topology.capacity() * sizeof(SamplingPrimitive);
	return bytes;
}

void NRIEmissiveGeometryCache::QuarantineAll()
{
	for (auto& state : mDomains)
	{
		state.quarantined = true;
		state.topologyValid = false;
	}
	mStats.quarantinedDomains = (uint32_t)mDomains.size();
}

uint64_t NRIEmissiveGeometryCache::HashGeometry(const nri_scene::GeometryData* geometry)
{
	return HashGeometryForEmissiveSampling(geometry);
}

bool NRIEmissiveGeometryCache::CopyChangesSamplingGeometry(const nri_scene::GeometryData& source,
	const NRIEmissiveGeometryCopyRange& sourceRange, const nri_scene::GeometryData& destination,
	const NRIEmissiveGeometryCopyRange& destinationRange, bool copyPrimitives)
{
	const auto fits = [](uint32_t offset, uint32_t count, size_t size)
	{
		return offset <= size && count <= size - offset;
	};
	if (sourceRange.vertexCount != destinationRange.vertexCount ||
		!fits(sourceRange.vertexOffset, sourceRange.vertexCount, source.vertices.size()) ||
		!fits(destinationRange.vertexOffset, sourceRange.vertexCount, destination.vertices.size()))
		return true;
	for (uint32_t i = 0; i < sourceRange.vertexCount; ++i)
	{
		const auto& sourceVertex = source.vertices[sourceRange.vertexOffset + i];
		const auto& destinationVertex = destination.vertices[destinationRange.vertexOffset + i];
		if (std::memcmp(sourceVertex.position, destinationVertex.position, sizeof(sourceVertex.position)) != 0)
			return true;
	}
	if (!copyPrimitives) return false;
	if (sourceRange.primitiveCount != destinationRange.primitiveCount ||
		!fits(sourceRange.primitiveOffset, sourceRange.primitiveCount, source.primitives.size()) ||
		!fits(destinationRange.primitiveOffset, sourceRange.primitiveCount, destination.primitives.size()))
		return true;
	for (uint32_t i = 0; i < sourceRange.primitiveCount; ++i)
	{
		const auto& sourcePrimitive = source.primitives[sourceRange.primitiveOffset + i];
		const auto& destinationPrimitive = destination.primitives[destinationRange.primitiveOffset + i];
		for (uint32_t j = 0; j < 3; ++j)
			if (destinationPrimitive.indices[j] != destinationRange.vertexOffset + sourcePrimitive.indices[j] - sourceRange.vertexOffset)
				return true;
		if (destinationPrimitive.materialIndex != destinationRange.materialOffset + sourcePrimitive.materialIndex - sourceRange.materialOffset)
			return true;
	}
	return false;
}

NRIEmissivePrimitiveGeometry NRIEmissiveGeometryCache::BuildPrimitive(
	const nri_scene::GeometryData& geometry, uint32_t primitiveIndex)
{
	NRIEmissivePrimitiveGeometry primitive = {};
	primitive.area = ComputePrimitiveArea(geometry, primitiveIndex);
	ComputePrimitiveBounds(geometry, primitiveIndex, primitive.center, primitive.radius);
	primitive.valid = true;
	return primitive;
}

void NRIEmissiveGeometryCache::BuildRanges(const nri_scene::GeometryData* geometry,
	std::vector<NRIEmissiveMaterialPrimitiveRange>& outRanges)
{
	outRanges.clear();
	if (geometry == nullptr) return;
	uint32_t maxMaterialIndex = 0;
	for (const auto& primitive : geometry->primitives)
		maxMaterialIndex = std::max(maxMaterialIndex, primitive.materialIndex);
	outRanges.assign((size_t)maxMaterialIndex + 1u, {});
	for (uint32_t primitiveIndex = 0; primitiveIndex < geometry->primitives.size(); ++primitiveIndex)
	{
		auto& range = outRanges[geometry->primitives[primitiveIndex].materialIndex];
		if (range.count == 0) range.first = primitiveIndex;
		range.count++;
	}
}
