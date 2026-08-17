#include "nri_persistent_voxel_material_closure.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace
{
	constexpr uint64_t FnvOffset = 1469598103934665603ull;
	constexpr uint64_t FnvPrime = 1099511628211ull;

	void HashBytes(uint64_t& hash, const void* data, size_t size)
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		for (size_t index = 0; index < size; ++index)
		{
			hash ^= bytes[index];
			hash *= FnvPrime;
		}
	}

	template <typename T>
	void HashValue(uint64_t& hash, const T& value)
	{
		HashBytes(hash, &value, sizeof(value));
	}

	void HashFloatArray(uint64_t& hash, const float* values, size_t count)
	{
		for (size_t index = 0; index < count; ++index)
		{
			uint32_t bits = 0;
			std::memcpy(&bits, values + index, sizeof(bits));
			HashValue(hash, bits);
		}
	}

	void HashMaterialRow(uint64_t& hash, const NRIPersistentVoxelMaterialRowSeed& row)
	{
		HashValue(hash, row.textureIndex);
		HashValue(hash, row.paletteIndex);
		HashValue(hash, row.flags);
		HashValue(hash, row.materialClass);
		HashValue(hash, row.lightingFlags);
		HashValue(hash, row.normalTextureIndex);
		HashValue(hash, row.metallicTextureIndex);
		HashValue(hash, row.roughnessTextureIndex);
		HashValue(hash, row.sectorIndex);
		HashValue(hash, row.emissiveTextureIndex);
		HashFloatArray(hash, &row.lightLevel, 1);
		HashFloatArray(hash, &row.alpha, 1);
		HashFloatArray(hash, &row.roughnessHint, 1);
		HashFloatArray(hash, &row.metalnessHint, 1);
		HashFloatArray(hash, row.emissiveColor, 3);
		HashFloatArray(hash, &row.emissiveIntensity, 1);
		HashFloatArray(hash, &row.emissiveMaskScale, 1);
		HashValue(hash, row.emissiveMode);
		HashFloatArray(hash, &row.emissiveReserved, 1);
	}

	void HashLightingSeed(uint64_t& hash, const NRIPersistentVoxelMaterialLightingSeed& row)
	{
		HashValue(hash, row.materialKey);
		HashValue(hash, row.textureContentKey);
		HashValue(hash, row.glowmapContentKey);
		HashValue(hash, row.normalContentKey);
		HashValue(hash, row.metallicContentKey);
		HashValue(hash, row.roughnessContentKey);
		HashValue(hash, row.textureId);
		HashValue(hash, row.baseTextureId);
		HashValue(hash, row.textureIndex);
		HashValue(hash, row.glowmapTextureIndex);
		HashValue(hash, row.normalTextureIndex);
		HashValue(hash, row.metallicTextureIndex);
		HashValue(hash, row.roughnessTextureIndex);
		HashValue(hash, row.emissiveTextureIndex);
		HashValue(hash, row.paletteIndex);
		HashValue(hash, row.materialFlags);
		HashValue(hash, row.lightingFlags);
		HashValue(hash, row.materialClass);
		HashValue(hash, row.emissiveMode);
		HashValue(hash, row.emissiveStableFrames);
		HashValue(hash, row.voxelPaletteIndex);
		HashValue(hash, row.voxelPalettePolicyFlags);
		HashValue(hash, row.voxelPalettePolicyApplied);
		HashValue(hash, row.sourceType);
		HashValue(hash, row.sectorIndex);
		HashValue(hash, row.actorIndex);
		HashValue(hash, row.actorOverlayRuleCount);
		for (uint32_t ruleId : row.actorOverlayRuleIds)
		{
			HashValue(hash, ruleId);
		}
		HashValue(hash, row.shade);
		HashFloatArray(hash, &row.alpha, 1);
		HashFloatArray(hash, &row.lightLevel, 1);
		HashFloatArray(hash, row.averageColor, 3);
		HashFloatArray(hash, row.glowColor, 3);
		HashFloatArray(hash, row.emissiveColor, 3);
		HashFloatArray(hash, &row.emissiveIntensity, 1);
		HashFloatArray(hash, &row.emissiveMaskScale, 1);
		HashFloatArray(hash, &row.visibleFullbrightBoost, 1);
	}

	void HashTextureDependency(uint64_t& hash, const NRIPersistentVoxelTextureDependency& texture)
	{
		HashValue(hash, texture.key);
		HashValue(hash, texture.materialTextureIndex);
		HashValue(hash, texture.width);
		HashValue(hash, texture.height);
		HashValue(hash, texture.indexed);
	}
}

uint64_t HashNRIPersistentVoxelMaterialRuleResult(const NRIPersistentVoxelMaterialSeed& seed)
{
	uint64_t hash = FnvOffset;
	HashValue(hash, seed.version);
	HashValue(hash, seed.actorSensitive);
	for (const NRIPersistentVoxelMaterialLightingSeed& row : seed.lighting)
	{
		HashLightingSeed(hash, row);
	}
	return hash;
}

uint64_t HashNRIPersistentVoxelMaterialSeed(const NRIPersistentVoxelMaterialSeed& seed)
{
	uint64_t hash = FnvOffset;
	HashValue(hash, seed.version);
	HashValue(hash, seed.validatedSignature);
	HashValue(hash, seed.ruleResultId);
	HashValue(hash, seed.paletteSignature);
	HashValue(hash, seed.paletteWidth);
	HashValue(hash, seed.paletteHeight);
	HashValue(hash, seed.actorSensitive);
	for (const NRIPersistentVoxelMaterialRowSeed& row : seed.materials)
	{
		HashMaterialRow(hash, row);
	}
	for (const NRIPersistentVoxelMaterialLightingSeed& row : seed.lighting)
	{
		HashLightingSeed(hash, row);
	}
	for (const NRIPersistentVoxelTextureDependency& texture : seed.textures)
	{
		HashTextureDependency(hash, texture);
	}
	return hash;
}

NRIPersistentVoxelMaterialClosureResult BuildNRIPersistentVoxelMaterialClosureResult(
	const NRIPersistentVoxelMaterialSeed& seed,
	bool reusedSeed)
{
	NRIPersistentVoxelMaterialClosureResult result = {};
	result.state = NRIPersistentVoxelMaterialClosureState::Ready;
	result.materialKey = seed.materialKey;
	result.validatedSignature = seed.validatedSignature;
	result.payloadSignature = seed.payloadSignature;
	result.ruleResultId = seed.ruleResultId;
	result.materialRows = (uint32_t)seed.materials.size();
	result.textureDependencies = (uint32_t)seed.textures.size();
	result.actorSensitive = seed.actorSensitive;
	result.reusedSeed = reusedSeed;
	result.textures = seed.textures;

	for (const NRIPersistentVoxelTextureDependency& texture : seed.textures)
	{
		switch (texture.state)
		{
		case NRIPersistentVoxelTextureClosureState::Ready:
		case NRIPersistentVoxelTextureClosureState::NotRequired:
			result.readyTextures++;
			break;
		case NRIPersistentVoxelTextureClosureState::Failed:
			result.failedTextures++;
			result.state = NRIPersistentVoxelMaterialClosureState::Failed;
			break;
		case NRIPersistentVoxelTextureClosureState::Deferred:
			result.deferredTextures++;
			if (result.state != NRIPersistentVoxelMaterialClosureState::Failed)
			{
				result.state = NRIPersistentVoxelMaterialClosureState::Deferred;
			}
			break;
		default:
			if (result.state == NRIPersistentVoxelMaterialClosureState::Ready)
			{
				result.state = NRIPersistentVoxelMaterialClosureState::Pending;
			}
			break;
		}
	}
	if (seed.materials.empty())
	{
		result.state = NRIPersistentVoxelMaterialClosureState::Failed;
	}
	return result;
}

bool NRIPersistentVoxelMaterialTextureLayoutPreservesPrefix(
	const std::vector<uint64_t>& uploadedTextureKeys,
	const std::vector<uint64_t>& rebuiltTextureKeys)
{
	return rebuiltTextureKeys.size() >= uploadedTextureKeys.size() &&
		std::equal(uploadedTextureKeys.begin(), uploadedTextureKeys.end(), rebuiltTextureKeys.begin());
}

bool NRIPersistentVoxelMaterialBindingNeedsRebind(
	uint32_t actorOffset,
	uint32_t actorCount,
	uint32_t canonicalOffset,
	uint32_t canonicalCount)
{
	return actorOffset != canonicalOffset || actorCount != canonicalCount;
}

size_t NRIPersistentVoxelMaterialClosureRegistry::BindingIdentityHash::operator()(const BindingIdentity& identity) const
{
	uint64_t hash = FnvOffset;
	HashValue(hash, identity.materialKey);
	HashValue(hash, identity.validatedSignature);
	return (size_t)hash;
}

void NRIPersistentVoxelMaterialClosureRegistry::Reset(uint64_t buildSerial)
{
	mSeedsByPayload.clear();
	mBindings.clear();
	mStats = {};
	mStats.buildSerial = buildSerial;
}

bool NRIPersistentVoxelMaterialClosureRegistry::Register(
	NRIPersistentVoxelMaterialSeed seed,
	NRIPersistentVoxelMaterialClosureResult& outResult)
{
	mStats.seedRegistrations++;
	if (seed.materialKey == 0 || seed.validatedSignature == 0 || seed.materials.empty())
	{
		outResult = BuildNRIPersistentVoxelMaterialClosureResult(seed);
		outResult.state = NRIPersistentVoxelMaterialClosureState::Failed;
		mStats.failedSeeds++;
		return false;
	}

	seed.ruleResultId = seed.ruleResultId != 0 ? seed.ruleResultId : HashNRIPersistentVoxelMaterialRuleResult(seed);
	seed.payloadSignature = seed.payloadSignature != 0 ? seed.payloadSignature : HashNRIPersistentVoxelMaterialSeed(seed);
	outResult = BuildNRIPersistentVoxelMaterialClosureResult(seed);
	if (outResult.state == NRIPersistentVoxelMaterialClosureState::Failed)
	{
		mStats.failedSeeds++;
		return false;
	}
	if (outResult.state == NRIPersistentVoxelMaterialClosureState::Deferred)
	{
		mStats.deferredSeeds++;
		return false;
	}
	if (outResult.state != NRIPersistentVoxelMaterialClosureState::Ready &&
		outResult.state != NRIPersistentVoxelMaterialClosureState::Pending)
	{
		mStats.failedSeeds++;
		return false;
	}

	auto existing = mSeedsByPayload.find(seed.payloadSignature);
	if (existing == mSeedsByPayload.end())
	{
		mSeedsByPayload.emplace(seed.payloadSignature, seed);
		if (outResult.state == NRIPersistentVoxelMaterialClosureState::Ready)
		{
			mStats.readySeeds++;
		}
		else
		{
			mStats.pendingSeeds++;
		}
	}
	else
	{
		mStats.seedDeduplications++;
		outResult.reusedSeed = true;
		const NRIPersistentVoxelMaterialClosureResult existingResult =
			BuildNRIPersistentVoxelMaterialClosureResult(existing->second, true);
		if (existingResult.state == NRIPersistentVoxelMaterialClosureState::Ready &&
			outResult.state == NRIPersistentVoxelMaterialClosureState::Pending)
		{
			outResult = existingResult;
			outResult.materialKey = seed.materialKey;
			outResult.validatedSignature = seed.validatedSignature;
		}
		if (existingResult.state == NRIPersistentVoxelMaterialClosureState::Pending &&
			outResult.state == NRIPersistentVoxelMaterialClosureState::Ready)
		{
			existing->second = seed;
			if (mStats.pendingSeeds != 0)
			{
				mStats.pendingSeeds--;
			}
			mStats.readySeeds++;
		}
	}
	mBindings[{ seed.materialKey, seed.validatedSignature }] = seed.payloadSignature;
	return outResult.state == NRIPersistentVoxelMaterialClosureState::Ready;
}

const NRIPersistentVoxelMaterialSeed* NRIPersistentVoxelMaterialClosureRegistry::Find(
	uint64_t buildSerial,
	uint64_t materialKey,
	uint64_t validatedSignature,
	NRIPersistentVoxelMaterialClosureResult& outResult)
{
	mStats.lookups++;
	outResult = {};
	outResult.materialKey = materialKey;
	outResult.validatedSignature = validatedSignature;
	if (buildSerial == 0 || buildSerial != mStats.buildSerial)
	{
		outResult.state = NRIPersistentVoxelMaterialClosureState::Stale;
		mStats.lookupMisses++;
		return nullptr;
	}

	const auto binding = mBindings.find({ materialKey, validatedSignature });
	if (binding == mBindings.end())
	{
		mStats.lookupMisses++;
		return nullptr;
	}
	const auto seed = mSeedsByPayload.find(binding->second);
	if (seed == mSeedsByPayload.end())
	{
		mStats.lookupMisses++;
		return nullptr;
	}

	outResult = BuildNRIPersistentVoxelMaterialClosureResult(seed->second, true);
	mStats.lookupHits++;
	return &seed->second;
}

const NRIPersistentVoxelMaterialSeed* NRIPersistentVoxelMaterialClosureRegistry::FindReady(
	uint64_t buildSerial,
	uint64_t materialKey,
	uint64_t validatedSignature,
	NRIPersistentVoxelMaterialClosureResult& outResult)
{
	const NRIPersistentVoxelMaterialSeed* seed =
		Find(buildSerial, materialKey, validatedSignature, outResult);
	return seed != nullptr && outResult.state == NRIPersistentVoxelMaterialClosureState::Ready ? seed : nullptr;
}

void NRIPersistentVoxelMaterialClosureRegistry::Invalidate(
	uint64_t materialKey,
	uint64_t validatedSignature)
{
	const auto binding = mBindings.find({ materialKey, validatedSignature });
	if (binding == mBindings.end())
	{
		return;
	}
	const uint64_t payloadSignature = binding->second;
	for (auto it = mBindings.begin(); it != mBindings.end();)
	{
		if (it->second == payloadSignature)
		{
			it = mBindings.erase(it);
		}
		else
		{
			++it;
		}
	}
	const auto seed = mSeedsByPayload.find(payloadSignature);
	if (seed == mSeedsByPayload.end())
	{
		return;
	}
	const NRIPersistentVoxelMaterialClosureResult result =
		BuildNRIPersistentVoxelMaterialClosureResult(seed->second);
	if (result.state == NRIPersistentVoxelMaterialClosureState::Ready && mStats.readySeeds != 0)
	{
		mStats.readySeeds--;
	}
	else if (result.state == NRIPersistentVoxelMaterialClosureState::Pending && mStats.pendingSeeds != 0)
	{
		mStats.pendingSeeds--;
	}
	mSeedsByPayload.erase(seed);
}
