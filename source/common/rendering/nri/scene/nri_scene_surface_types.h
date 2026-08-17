#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class FGameTexture;

namespace nri_scene
{
struct VoxelPalettePolicyDocument;

static constexpr uint32_t MaxActorOverlayRuleIdsPerSurface = 4;

enum class SurfaceSourceType : uint32_t
{
	Unknown = 0,
	DrawListWall,
	MirrorWall,
	FloorFlat,
	CeilingFlat,
	FacingSprite,
	VoxelProxySprite,
	MapWallBand,
	MapFloorSection,
	MapCeilingSection,
	MapPortalSurface,
	DebugSphere,
	SurfaceLightOverlay,
};

enum MaterialFlags : uint32_t
{
	MaterialFlag_None = 0,
	MaterialFlag_Indexed = 1u << 0,
	MaterialFlag_Fullbright = 1u << 1,
	MaterialFlag_Flat = 1u << 2,
	MaterialFlag_Sprite = 1u << 3,
	MaterialFlag_Mirror = 1u << 4,
	MaterialFlag_Sky = 1u << 5,
	MaterialFlag_Portal = 1u << 6,
	MaterialFlag_OneWay = 1u << 7,
	MaterialFlag_AlphaClip = 1u << 8,
	MaterialFlag_FacingBillboard = 1u << 9,
	MaterialFlag_PointSampled = 1u << 10,
	MaterialFlag_PlainMirror = 1u << 11,
	MaterialFlag_TintEmission = 1u << 12,
};

enum PrimitiveFlags : uint32_t
{
	PrimitiveFlag_None = 0,
	PrimitiveFlag_ReflectionOnly = 1u << 16,
};

struct SurfaceProvenance
{
	SurfaceSourceType sourceType = SurfaceSourceType::Unknown;
	int32_t sectorIndex = -1;
	int32_t wallIndex = -1;
	int32_t sectionIndex = -1;
	int32_t mapChunkIndex = -1;
	int32_t nextSectorIndex = -1;
	int32_t actorIndex = -1;
	uint32_t drawListType = UINT32_MAX;
	uint32_t cstat = 0;
	uint32_t materialFlags = 0;
	uint32_t actorOverlayRuleCount = 0;
	uint32_t actorOverlayRuleIds[MaxActorOverlayRuleIdsPerSurface] = {};
};

struct MaterialRef
{
	FGameTexture* texture = nullptr;
	FGameTexture* emissiveSourceTexture = nullptr;
	int palette = 0;
	int shade = 0;
	float alpha = 1.0f;
	uint32_t flags = MaterialFlag_None;
	std::shared_ptr<const VoxelPalettePolicyDocument> voxelPalettePolicy;
	uint64_t voxelPalettePolicyContentKey = 0;
};

struct CapturedVertex
{
	float position[3] = {};
	float prevPosition[3] = {};
	float uv[2] = {};
};

struct SurfaceRef
{
	std::vector<CapturedVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<uint8_t> primitiveLocalMaterialSlots;
	uint32_t materialRowSpan = 1u;
	MaterialRef material;
	SurfaceProvenance provenance;
};
}
