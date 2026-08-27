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

// Shared CPU-side reason codes for primary-surface temporal correspondence.
// Keep these values in sync with shaders/Include/MotionContracts.hlsli.
enum class MotionValidityReason : uint32_t
{
	Valid = 0,
	NoHistory = 1,
	TopologyMismatch = 2,
	AmbiguousCorrespondence = 3,
	CurrentBehindCamera = 4,
	PreviousBehindCamera = 5,
	CurrentNonFinite = 6,
	PreviousNonFinite = 7,
	ReprojectedIdentityMismatch = 8,
	ReprojectedDepthMismatch = 9,
	ReprojectedNormalMismatch = 10,
	ReprojectedOutsideViewport = 11,
	ActorCensusRejected = 12,
	GlobalReset = 13,
	UnsupportedSource = 14,
	Reappeared = 15,
	DuplicatePublication = 16,
};

struct TemporalSurfaceMetadata
{
	uint64_t occurrenceId = 0;
	uint64_t topologyKey = 0;
	uint32_t generation = 0;
	uint32_t historyAge = 0;
	// Build wall V coordinates are anchored by referenceHeight + position.y.
	// Preserve that material-space anchor so moving clip boundaries do not get
	// mistaken for motion of the wall texture itself.
	float materialVerticalReference = 0.0f;
	MotionValidityReason reason = MotionValidityReason::UnsupportedSource;
	bool identityValid = false;
	bool correspondenceValid = false;
	bool materialVerticalReferenceValid = false;
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
	// CPU-only semantic corner identity. It is intentionally not copied into
	// SceneVertex; the map motion owner uses it before geometry bridging.
	uint64_t temporalCornerKey = UINT64_MAX;
};

struct SurfaceRef
{
	std::vector<CapturedVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<uint8_t> primitiveLocalMaterialSlots;
	uint32_t materialRowSpan = 1u;
	MaterialRef material;
	SurfaceProvenance provenance;
	TemporalSurfaceMetadata temporal;
};
}
