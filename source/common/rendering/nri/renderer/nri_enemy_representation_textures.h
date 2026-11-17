#pragma once

#include <cstdint>

class NRIPersistentVoxelResidency;
class NRIRenderDevice;
class NRISceneTextureResidency;
struct NRIPersistentVoxelSettings;

void NRIPrepareEnemyFamilyTextures(uint32_t frameIndex, NRIPersistentVoxelResidency& voxels,
    const NRIPersistentVoxelSettings& settings, NRIRenderDevice& device, NRISceneTextureResidency& textures);
