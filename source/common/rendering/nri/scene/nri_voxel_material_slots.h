#pragma once

#include <cmath>
#include <cstdint>

namespace nri_scene
{
static constexpr uint32_t VoxelPaletteMaterialSlotCount = 256u;
static constexpr uint32_t LegacyVoxelMaterialRowSpan = 1u;

inline bool DecodeVoxelPaletteMaterialSlot(float u, float v, uint8_t& outSlot)
{
	if (!std::isfinite(u) || !std::isfinite(v) ||
		u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f)
	{
		outSlot = 0u;
		return false;
	}

	const uint32_t column = (uint32_t)(u * 16.0f);
	const uint32_t row = (uint32_t)(v * 16.0f);
	outSlot = (uint8_t)(row * 16u + column);
	return true;
}

inline uint32_t NormalizeVoxelMaterialRowSpan(uint32_t materialRowSpan)
{
	return materialRowSpan == VoxelPaletteMaterialSlotCount ?
		VoxelPaletteMaterialSlotCount :
		LegacyVoxelMaterialRowSpan;
}

inline uint32_t ResolveVoxelPrimitiveMaterialIndex(
	uint32_t materialBase,
	uint32_t materialRowSpan,
	uint32_t localMaterialSlot)
{
	return NormalizeVoxelMaterialRowSpan(materialRowSpan) == VoxelPaletteMaterialSlotCount &&
		localMaterialSlot < VoxelPaletteMaterialSlotCount ?
		materialBase + localMaterialSlot :
		materialBase;
}

inline bool IsVoxelPrimitiveMaterialIndexCompatible(uint32_t localMaterialSlot, uint32_t materialCount)
{
	return localMaterialSlot < materialCount ||
		(materialCount == LegacyVoxelMaterialRowSpan && localMaterialSlot < VoxelPaletteMaterialSlotCount);
}
}
