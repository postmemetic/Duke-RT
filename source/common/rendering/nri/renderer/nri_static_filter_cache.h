#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nri { struct TopLevelInstance; }
namespace nri_scene { struct MaterialData; }
struct StaticMapSceneCache;
struct ResidentMapChunkRegistry;
struct SceneInstanceData;

// CPU classification only. The owner never changes geometry, scene membership,
// visibility, transforms, material bindings or acceleration-structure lifetime.
class NRIStaticFilterCache
{
public:
	void InvalidateGeometry();
	void InvalidateChunk(uint32_t chunkListIndex);
	void InvalidatePrimitiveRange(uint32_t first, uint32_t count);
	void BeginMaterialWrite() { ++mMaterialWriteDepth; mMaterialsPublished = false; }
	void EndMaterialWrite() { if (mMaterialWriteDepth != 0) --mMaterialWriteDepth; }
	void PublishMaterials(const std::vector<nri_scene::MaterialData>& materials,
		uint32_t first = 0, uint32_t count = UINT32_MAX);
	bool ResolveIntrinsicMask(const StaticMapSceneCache& scene, uint32_t chunkListIndex,
		const SceneInstanceData& instance, uint32_t primitiveCount, uint32_t& mask,
		bool& hit, uint64_t& rows, uint64_t& invalidMaterials);
	void Quarantine() { mQuarantined = true; }
	bool IsQuarantined() const { return mQuarantined; }
	uint64_t CapacityBytes() const;

private:
	struct Key
	{
		uint64_t contentBuildSerial = 0;
		uint64_t materialRevision = 0;
		uint32_t chunkIndex = UINT32_MAX;
		uint32_t primitiveOffset = 0, primitiveCount = 0;
		uint32_t materialOffset = 0, materialCount = 0, instanceMaterialBase = 0;
		bool operator==(const Key& rhs) const;
	};
	struct Entry { Key key; uint32_t mask = 0; bool valid = false; };
	std::vector<Entry> mEntries;
	std::vector<uint32_t> mMaterialPolicies;
	uint64_t mMaterialRevision = 1;
	uint32_t mMaterialWriteDepth = 0;
	bool mHasMaterialSnapshot = false;
	bool mMaterialsPublished = false;
	bool mQuarantined = false;
};

// Arm immediately before an actual write to the static GPU-material mirror.
// Every exit publishes the final CPU rows, including partial/failing producers.
class NRIStaticFilterMaterialPublication
{
public:
	NRIStaticFilterMaterialPublication(NRIStaticFilterCache& cache,
		const std::vector<nri_scene::MaterialData>* materials,
		uint32_t first = 0, uint32_t count = UINT32_MAX)
		: mCache(cache), mMaterials(materials), mFirst(first), mCount(count)
	{
		if (mMaterials != nullptr) mCache.BeginMaterialWrite();
	}
	~NRIStaticFilterMaterialPublication()
	{
		if (mMaterials != nullptr)
		{
			mCache.EndMaterialWrite();
			mCache.PublishMaterials(*mMaterials, mFirst, mCount);
		}
	}
	NRIStaticFilterMaterialPublication(const NRIStaticFilterMaterialPublication&) = delete;
	NRIStaticFilterMaterialPublication& operator=(const NRIStaticFilterMaterialPublication&) = delete;
private:
	NRIStaticFilterCache& mCache;
	const std::vector<nri_scene::MaterialData>* mMaterials;
	uint32_t mFirst, mCount;
};

struct NRIStaticFilterOptions
{
	uint32_t enabledPolicyMask = 0;
	bool queryEnabled = true;
	bool visibleChunkGate = true;
	bool useCache = true;
	bool validate = false;
};

void ApplyNRIStaticFilterCandidateFlags(StaticMapSceneCache& scene,
	const ResidentMapChunkRegistry& registry, const std::vector<uint32_t>& visibleChunkWords,
	std::vector<nri::TopLevelInstance>& instances, const std::vector<SceneInstanceData>& sceneInstances,
	const NRIStaticFilterOptions& options);
