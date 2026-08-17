#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

constexpr uint32_t NRI_PERSISTENT_VOXEL_MATERIAL_SEED_VERSION = 2;

enum class NRIPersistentVoxelMaterialClosureSource : uint8_t
{
	PreloadKnown,
	RuntimeUnknown,
};

enum class NRIPersistentVoxelTextureClosureState : uint8_t
{
	Pending,
	Ready,
	NotRequired,
	Deferred,
	Failed,
};

enum class NRIPersistentVoxelTextureClosureFailure : uint8_t
{
	None,
	DynamicTexture,
	PayloadUnavailable,
	ResourceCreation,
	DescriptorUnavailable,
	ResidencyLost,
};

enum class NRIPersistentVoxelMaterialClosureState : uint8_t
{
	Missing,
	Pending,
	Ready,
	Deferred,
	Failed,
	Stale,
};

struct NRIPersistentVoxelTextureDependency
{
	uint64_t key = 0;
	uint64_t estimatedBytes = 0;
	uint32_t materialTextureIndex = UINT32_MAX;
	uint32_t residencyIndex = UINT32_MAX;
	uint32_t width = 0;
	uint32_t height = 0;
	NRIPersistentVoxelTextureClosureState state = NRIPersistentVoxelTextureClosureState::Pending;
	NRIPersistentVoxelTextureClosureFailure failure = NRIPersistentVoxelTextureClosureFailure::None;
	bool indexed = false;
	// This confirms a resource view exists; active descriptor-table publication remains consumer-owned.
	bool descriptorReady = false;
};

struct NRIPersistentVoxelMaterialRowSeed
{
	uint32_t textureIndex = 0;
	uint32_t paletteIndex = 0;
	uint32_t flags = 0;
	uint32_t materialClass = 0;
	uint32_t lightingFlags = 0;
	uint32_t normalTextureIndex = UINT32_MAX;
	uint32_t metallicTextureIndex = UINT32_MAX;
	uint32_t roughnessTextureIndex = UINT32_MAX;
	uint32_t sectorIndex = UINT32_MAX;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	float lightLevel = 1.0f;
	float alpha = 1.0f;
	float roughnessHint = 0.45f;
	float metalnessHint = 0.0f;
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	float emissiveMaskScale = 0.0f;
	uint32_t emissiveMode = 0;
	float emissiveReserved = 0.0f;
};

struct NRIPersistentVoxelMaterialLightingSeed
{
	uint64_t materialKey = 0;
	uint64_t textureContentKey = 0;
	uint64_t glowmapContentKey = 0;
	uint64_t normalContentKey = 0;
	uint64_t metallicContentKey = 0;
	uint64_t roughnessContentKey = 0;
	uint32_t textureId = 0;
	uint32_t baseTextureId = 0;
	uint32_t textureIndex = 0;
	uint32_t glowmapTextureIndex = UINT32_MAX;
	uint32_t normalTextureIndex = UINT32_MAX;
	uint32_t metallicTextureIndex = UINT32_MAX;
	uint32_t roughnessTextureIndex = UINT32_MAX;
	uint32_t emissiveTextureIndex = UINT32_MAX;
	uint32_t paletteIndex = 0;
	uint32_t materialFlags = 0;
	uint32_t lightingFlags = 0;
	uint32_t materialClass = 0;
	uint32_t emissiveMode = 0;
	uint32_t emissiveStableFrames = 0;
	uint32_t voxelPaletteIndex = UINT32_MAX;
	uint32_t voxelPalettePolicyFlags = 0;
	bool voxelPalettePolicyApplied = false;
	uint32_t sourceType = 0;
	int32_t sectorIndex = -1;
	int32_t actorIndex = -1;
	uint32_t actorOverlayRuleCount = 0;
	uint32_t actorOverlayRuleIds[4] = {};
	int32_t shade = 0;
	float alpha = 1.0f;
	float lightLevel = 1.0f;
	float averageColor[3] = { 1.0f, 1.0f, 1.0f };
	float glowColor[3] = {};
	float emissiveColor[3] = {};
	float emissiveIntensity = 0.0f;
	float emissiveMaskScale = 0.0f;
	float visibleFullbrightBoost = 1.0f;
};

struct NRIPersistentVoxelMaterialSeed
{
	uint32_t version = NRI_PERSISTENT_VOXEL_MATERIAL_SEED_VERSION;
	uint64_t materialKey = 0;
	uint64_t validatedSignature = 0;
	uint64_t payloadSignature = 0;
	uint64_t ruleResultId = 0;
	uint64_t paletteSignature = 0;
	uint32_t paletteWidth = 0;
	uint32_t paletteHeight = 0;
	bool actorSensitive = false;
	std::vector<NRIPersistentVoxelMaterialRowSeed> materials;
	std::vector<NRIPersistentVoxelMaterialLightingSeed> lighting;
	std::vector<NRIPersistentVoxelTextureDependency> textures;
};

struct NRIPersistentVoxelMaterialClosureResult
{
	NRIPersistentVoxelMaterialClosureState state = NRIPersistentVoxelMaterialClosureState::Missing;
	uint64_t materialKey = 0;
	uint64_t validatedSignature = 0;
	uint64_t payloadSignature = 0;
	uint64_t ruleResultId = 0;
	uint32_t materialRows = 0;
	uint32_t textureDependencies = 0;
	uint32_t readyTextures = 0;
	uint32_t deferredTextures = 0;
	uint32_t failedTextures = 0;
	bool actorSensitive = false;
	bool reusedSeed = false;
	std::vector<NRIPersistentVoxelTextureDependency> textures;
};

struct NRIPersistentVoxelMaterialClosureRegistryStats
{
	uint64_t buildSerial = 0;
	uint32_t seedRegistrations = 0;
	uint32_t seedDeduplications = 0;
	uint32_t readySeeds = 0;
	uint32_t pendingSeeds = 0;
	uint32_t deferredSeeds = 0;
	uint32_t failedSeeds = 0;
	uint32_t lookups = 0;
	uint32_t lookupHits = 0;
	uint32_t lookupMisses = 0;
};

uint64_t HashNRIPersistentVoxelMaterialRuleResult(const NRIPersistentVoxelMaterialSeed& seed);
uint64_t HashNRIPersistentVoxelMaterialSeed(const NRIPersistentVoxelMaterialSeed& seed);
NRIPersistentVoxelMaterialClosureResult BuildNRIPersistentVoxelMaterialClosureResult(
	const NRIPersistentVoxelMaterialSeed& seed,
	bool reusedSeed = false);
bool NRIPersistentVoxelMaterialTextureLayoutPreservesPrefix(
	const std::vector<uint64_t>& uploadedTextureKeys,
	const std::vector<uint64_t>& rebuiltTextureKeys);
bool NRIPersistentVoxelMaterialBindingNeedsRebind(
	uint32_t actorOffset,
	uint32_t actorCount,
	uint32_t canonicalOffset,
	uint32_t canonicalCount);

class NRIPersistentVoxelMaterialClosureRegistry
{
public:
	void Reset(uint64_t buildSerial);
	bool Register(NRIPersistentVoxelMaterialSeed seed, NRIPersistentVoxelMaterialClosureResult& outResult);
	const NRIPersistentVoxelMaterialSeed* Find(
		uint64_t buildSerial,
		uint64_t materialKey,
		uint64_t validatedSignature,
		NRIPersistentVoxelMaterialClosureResult& outResult);
	const NRIPersistentVoxelMaterialSeed* FindReady(
		uint64_t buildSerial,
		uint64_t materialKey,
		uint64_t validatedSignature,
		NRIPersistentVoxelMaterialClosureResult& outResult);
	// Texture dependency failure invalidates the shared payload and every binding
	// that references it. Keeping sibling bindings would allow a stale seed to be
	// rediscovered after the failing binding rebuilds.
	void Invalidate(uint64_t materialKey, uint64_t validatedSignature);
	const NRIPersistentVoxelMaterialClosureRegistryStats& Stats() const { return mStats; }

private:
	struct BindingIdentity
	{
		uint64_t materialKey = 0;
		uint64_t validatedSignature = 0;

		bool operator==(const BindingIdentity& other) const
		{
			return materialKey == other.materialKey && validatedSignature == other.validatedSignature;
		}
	};

	struct BindingIdentityHash
	{
		size_t operator()(const BindingIdentity& identity) const;
	};

	std::unordered_map<uint64_t, NRIPersistentVoxelMaterialSeed> mSeedsByPayload;
	std::unordered_map<BindingIdentity, uint64_t, BindingIdentityHash> mBindings;
	NRIPersistentVoxelMaterialClosureRegistryStats mStats = {};
};
