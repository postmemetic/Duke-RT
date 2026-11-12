#pragma once

#include "nri_scene_lights.h"
#include "nri_resources.h"

#include <array>
#include <cstring>

// Diagnostic CPU submission proof, not a GPU readback. Only the validation path
// allocates these vectors. Cache authority remains with input/history identity.
namespace nri_emissive_destination
{
struct Metadata
{
	uint32_t count = 0;
	uint32_t dominantPrimitive = UINT32_MAX;
	uint32_t dominantTile = 0;
	uint32_t dominantFlags = 0;
	uint32_t dominantDataSource = 0;
	float totalPower = 0.0f;
	float dominantPower = 0.0f;
};

inline Metadata ExpectedMetadata(const NRIEmissiveSamplingUploadScratch& payload)
{
	Metadata result;
	result.count = payload.header.activeCount;
	result.totalPower = payload.header.totalPower;
	const size_t index = payload.header.dominantIndex;
	if (index < payload.debugRecords.size()) result.dominantPrimitive = payload.debugRecords[index].primitiveIndex;
	if (index < payload.primitives.size())
	{
		const auto& primitive = payload.primitives[index];
		result.dominantTile = primitive.textureId;
		result.dominantFlags = primitive.sourceFlags;
		result.dominantDataSource = primitive.dataSource;
		result.dominantPower = primitive.powerEstimate;
	}
	return result;
}

inline bool EqualMetadata(const Metadata& a, const Metadata& b)
{
	return a.count == b.count && a.dominantPrimitive == b.dominantPrimitive &&
		a.dominantTile == b.dominantTile && a.dominantFlags == b.dominantFlags &&
		a.dominantDataSource == b.dominantDataSource &&
		std::memcmp(&a.totalPower, &b.totalPower, sizeof(float)) == 0 &&
		std::memcmp(&a.dominantPower, &b.dominantPower, sizeof(float)) == 0;
}

inline bool EqualDebug(const std::vector<NRIEmissivePrimitiveDebugRecord>& a,
	const std::vector<NRIEmissivePrimitiveDebugRecord>& b)
{
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (!NRIEqualEmissiveDebugRecord(a[i], b[i])) return false;
	return true;
}

template<class T> inline bool EqualBytes(const std::vector<T>& a, const std::vector<T>& b)
{
	return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

inline void Pack(NRIEmissiveSamplingUploadScratch& payload)
{
	payload.shaderPrimitives.clear();
	payload.shaderPrimitives.reserve(payload.primitives.size());
	for (const auto& primitive : payload.primitives)
		payload.shaderPrimitives.push_back(PackNRIEmissivePrimitiveShaderData(primitive));
}

using Resources = std::array<const NRIBufferResource*, 4>;
struct ResourceStamp
{
	const void* buffer = nullptr;
	const void* view = nullptr;
	uint64_t capacity = 0, used = 0, payloadHash = 0, payloadSize = 0;
	uint32_t stride = 0, payloadStride = 0;
	static ResourceStamp Capture(const NRIBufferResource& resource)
	{
		return { resource.buffer, resource.shaderView, resource.size, resource.usedSize,
			resource.payloadHash, resource.payloadSize, resource.stride, resource.payloadStride };
	}
	bool Matches(const NRIBufferResource& resource) const
	{
		return buffer == resource.buffer && view == resource.shaderView && capacity == resource.size &&
			used == resource.usedSize && payloadHash == resource.payloadHash && payloadSize == resource.payloadSize &&
			stride == resource.stride && payloadStride == resource.payloadStride;
	}
};

inline uint64_t HashBytes(uint64_t hash, const void* data, size_t size)
{
	const auto* bytes = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
	return hash;
}
template<class T> inline uint64_t HashVector(uint64_t hash, const std::vector<T>& values)
{
	const uint64_t size = values.size();
	hash = HashBytes(hash, &size, sizeof(size));
	return HashBytes(hash, values.data(), values.size() * sizeof(T));
}
inline uint64_t PayloadHash(const NRIEmissivePrimitiveHeaderGpuData& header,
	const std::vector<NRIEmissivePrimitiveShaderData>& primitives, const std::vector<float>& cdf,
	const std::vector<NRIEmissiveMaterialResponseGpuData>& responses)
{
	uint64_t hash = HashBytes(1469598103934665603ull, &header, sizeof(header));
	hash = HashVector(hash, primitives);
	hash = HashVector(hash, cdf);
	return HashVector(hash, responses);
}

struct Snapshot
{
	bool valid = false;
	NRIEmissivePrimitiveHeaderGpuData header = {};
	std::vector<NRIEmissivePrimitiveShaderData> primitives;
	std::vector<float> cdf;
	std::vector<NRIEmissiveMaterialResponseGpuData> responses;
	std::array<ResourceStamp, 4> resources{};

	void Reset() { *this = {}; }
	void Capture(const NRIEmissiveSamplingUploadScratch& submitted, const Resources& selected)
	{
		header = submitted.header;
		primitives = submitted.shaderPrimitives;
		cdf = submitted.cdf;
		responses = submitted.materialResponses;
		for (size_t i = 0; i < resources.size(); ++i) resources[i] = ResourceStamp::Capture(*selected[i]);
		valid = true;
	}
	bool Matches(const NRIEmissiveSamplingUploadScratch& reference, const Resources& selected) const
	{
		if (!valid || std::memcmp(&header, &reference.header, sizeof(header)) != 0 ||
			!EqualBytes(primitives, reference.shaderPrimitives) || !EqualBytes(cdf, reference.cdf) ||
			!EqualBytes(responses, reference.materialResponses)) return false;
		for (size_t i = 0; i < resources.size(); ++i)
			if (!resources[i].Matches(*selected[i])) return false;
		return true;
	}
	uint64_t PayloadHash() const { return nri_emissive_destination::PayloadHash(header, primitives, cdf, responses); }
	uint64_t CapacityBytes() const
	{
		return primitives.capacity() * sizeof(NRIEmissivePrimitiveShaderData) + cdf.capacity() * sizeof(float) +
			responses.capacity() * sizeof(NRIEmissiveMaterialResponseGpuData);
	}
};
}
