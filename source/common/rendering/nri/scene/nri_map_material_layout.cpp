#include "nri_map_material_layout.h"

#ifndef NRI_MAP_MATERIAL_LAYOUT_VIEW_ONLY
#include "nri_map_world.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

namespace nri_scene
{
namespace
{
	constexpr uint64_t HashOffset = 1469598103934665603ull;
	constexpr uint64_t HashPrime = 1099511628211ull;
	constexpr uint64_t SlotIdDomain = 0x50544d4154534c54ull;
	constexpr uint64_t LayoutKeyDomain = 0x50544d41544c4159ull;
	constexpr uint64_t StateKeyDomain = 0x50544d4154535441ull;
	constexpr uint32_t SurfaceFloor = 0;
	constexpr uint32_t SurfaceCeiling = 1;
	constexpr uint32_t SurfaceWallOneSided = 2;
	constexpr uint32_t SurfaceWallLower = 5;

	struct CanonicalCandidate
	{
		PTMapMaterialSemanticSlot semantic;
		uint64_t slotId = 0;
		uint64_t stateKey = 0;
		uint32_t sourceIndex = UINT32_MAX;
		uint32_t emittedSlot = UINT32_MAX;
	};

	template<class T>
	uint64_t HashValue(uint64_t hash, const T& value)
	{
		const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
		for (size_t i = 0; i < sizeof(value); ++i)
		{
			hash = (hash ^ bytes[i]) * HashPrime;
		}
		return hash;
	}

	uint64_t HashSemanticSlot(const PTMapMaterialSemanticSlot& slot)
	{
		uint64_t hash = HashValue(HashOffset, SlotIdDomain);
		hash = HashValue(hash, slot.sourceType);
		hash = HashValue(hash, slot.sectorIndex);
		hash = HashValue(hash, slot.wallIndex);
		hash = HashValue(hash, slot.sectionIndex);
		hash = HashValue(hash, slot.surfaceKind);
		hash = HashValue(hash, slot.keyPrimary);
		return HashValue(hash, slot.keySecondary);
	}

	void SetFailure(
		PTMapMaterialLayoutValidation& validation,
		PTMapMaterialLayoutFailure failure,
		uint32_t surfaceIndex = UINT32_MAX,
		uint32_t emittedSlot = UINT32_MAX,
		uint32_t canonicalSlot = UINT32_MAX)
	{
		validation = {};
		validation.failure = failure;
		validation.surfaceIndex = surfaceIndex;
		validation.emittedMaterialSlot = emittedSlot;
		validation.canonicalMaterialSlot = canonicalSlot;
	}

	bool IsWallKind(uint32_t kind)
	{
		return kind >= SurfaceWallOneSided && kind <= SurfaceWallLower;
	}

	bool BuildSemanticSlot(
		const PTMapMaterialSurfaceView& view,
		PTMapMaterialSemanticSlot& outSlot,
		PTMapMaterialLayoutFailure& outFailure)
	{
		const SurfaceProvenance& provenance = view.surface->provenance;
		if (view.chunkIndex == UINT32_MAX || provenance.mapChunkIndex < 0 || provenance.sectorIndex < 0 ||
			view.keyPrimary == UINT32_MAX || view.keySecondary == UINT32_MAX)
		{
			outFailure = PTMapMaterialLayoutFailure::MissingSemanticSlot;
			return false;
		}
		if ((uint32_t)provenance.mapChunkIndex != view.chunkIndex ||
			provenance.actorIndex >= 0 || provenance.drawListType != UINT32_MAX)
		{
			outFailure = PTMapMaterialLayoutFailure::InvalidSemanticSlot;
			return false;
		}

		if (view.surfaceKind == SurfaceFloor || view.surfaceKind == SurfaceCeiling)
		{
			const uint32_t expectedPlane = view.surfaceKind == SurfaceFloor ? 0u : 1u;
			const SurfaceSourceType expectedSource = view.surfaceKind == SurfaceFloor ?
				SurfaceSourceType::MapFloorSection : SurfaceSourceType::MapCeilingSection;
			if (provenance.sourceType != expectedSource || provenance.sectionIndex < 0 || provenance.wallIndex >= 0 ||
				view.keyPrimary != (uint32_t)provenance.sectionIndex || view.keySecondary != expectedPlane)
			{
				outFailure = PTMapMaterialLayoutFailure::InvalidSemanticSlot;
				return false;
			}
		}
		else if (IsWallKind(view.surfaceKind))
		{
			const uint32_t expectedBand = view.surfaceKind - SurfaceWallOneSided;
			if (provenance.sourceType != SurfaceSourceType::MapWallBand || provenance.wallIndex < 0 || provenance.sectionIndex >= 0 ||
				view.keyPrimary != (uint32_t)provenance.wallIndex || view.keySecondary != expectedBand)
			{
				outFailure = PTMapMaterialLayoutFailure::InvalidSemanticSlot;
				return false;
			}
		}
		else
		{
			outFailure = PTMapMaterialLayoutFailure::UnsupportedSurface;
			return false;
		}

		const uint32_t unsupportedFlags = MaterialFlag_Mirror | MaterialFlag_Sky | MaterialFlag_Portal | MaterialFlag_PlainMirror;
		if ((view.surface->material.flags & unsupportedFlags) != 0)
		{
			outFailure = PTMapMaterialLayoutFailure::UnsupportedSurface;
			return false;
		}

		outSlot.sourceType = (uint32_t)provenance.sourceType;
		outSlot.sectorIndex = provenance.sectorIndex;
		outSlot.wallIndex = provenance.wallIndex;
		outSlot.sectionIndex = provenance.sectionIndex;
		outSlot.surfaceKind = view.surfaceKind;
		outSlot.keyPrimary = view.keyPrimary;
		outSlot.keySecondary = view.keySecondary;
		outFailure = PTMapMaterialLayoutFailure::None;
		return true;
	}

	bool BuildMaterialStateKey(
		const MaterialRef& material,
		const PTMapMaterialLayoutOptions& options,
		uint64_t& outKey,
		PTMapMaterialLayoutFailure& outFailure)
	{
		if (!std::isfinite(material.alpha))
		{
			outFailure = PTMapMaterialLayoutFailure::NonFiniteMaterialState;
			return false;
		}
		if (material.texture == nullptr || options.resolveTextureIdentity == nullptr)
		{
			outFailure = PTMapMaterialLayoutFailure::MissingMaterialIdentity;
			return false;
		}

		uint64_t textureIdentity = 0;
		if (!options.resolveTextureIdentity(material.texture, textureIdentity, options.textureIdentityUserData))
		{
			outFailure = PTMapMaterialLayoutFailure::MissingMaterialIdentity;
			return false;
		}

		uint64_t emissiveIdentity = 0;
		if (material.emissiveSourceTexture != nullptr &&
			!options.resolveTextureIdentity(material.emissiveSourceTexture, emissiveIdentity, options.textureIdentityUserData))
		{
			outFailure = PTMapMaterialLayoutFailure::MissingMaterialIdentity;
			return false;
		}

		uint32_t alphaBits = 0;
		const float canonicalAlpha = material.alpha == 0.0f ? 0.0f : material.alpha;
		static_assert(sizeof(alphaBits) == sizeof(canonicalAlpha), "float hash size mismatch");
		std::memcpy(&alphaBits, &canonicalAlpha, sizeof(alphaBits));

		uint64_t hash = HashOffset;
		hash = HashValue(hash, textureIdentity);
		hash = HashValue(hash, emissiveIdentity);
		hash = HashValue(hash, material.authoredTextureId);
		hash = HashValue(hash, material.palette);
		hash = HashValue(hash, material.shade);
		hash = HashValue(hash, alphaBits);
		outKey = HashValue(hash, material.flags);
		outFailure = PTMapMaterialLayoutFailure::None;
		return true;
	}

	bool SemanticLess(const PTMapMaterialSemanticSlot& a, const PTMapMaterialSemanticSlot& b)
	{
		return std::tie(a.sourceType, a.sectorIndex, a.wallIndex, a.sectionIndex, a.surfaceKind, a.keyPrimary, a.keySecondary) <
			std::tie(b.sourceType, b.sectorIndex, b.wallIndex, b.sectionIndex, b.surfaceKind, b.keyPrimary, b.keySecondary);
	}
}

bool operator==(const PTMapMaterialSemanticSlot& a, const PTMapMaterialSemanticSlot& b)
{
	return a.sourceType == b.sourceType &&
		a.sectorIndex == b.sectorIndex &&
		a.wallIndex == b.wallIndex &&
		a.sectionIndex == b.sectionIndex &&
		a.surfaceKind == b.surfaceKind &&
		a.keyPrimary == b.keyPrimary &&
		a.keySecondary == b.keySecondary;
}

bool BuildCanonicalPTMapMaterialLayoutFromViews(
	const std::vector<PTMapMaterialSurfaceView>& surfaces,
	CanonicalPTMapMaterialLayout& outLayout,
	PTMapMaterialLayoutValidation& outValidation,
	const PTMapMaterialLayoutOptions& options)
{
	outLayout = {};
	outValidation = {};
	if (surfaces.empty())
	{
		SetFailure(outValidation, PTMapMaterialLayoutFailure::EmptyInput);
		return false;
	}

	const uint32_t emittedMaterialCount = options.emittedMaterialCount == UINT32_MAX ?
		(uint32_t)surfaces.size() : options.emittedMaterialCount;
	if (emittedMaterialCount == 0)
	{
		SetFailure(outValidation, PTMapMaterialLayoutFailure::MissingEmittedSlot, UINT32_MAX, 0);
		return false;
	}
	std::vector<bool> emittedSlots(emittedMaterialCount, false);
	std::vector<CanonicalCandidate> candidates;
	candidates.reserve(surfaces.size());
	uint32_t chunkIndex = UINT32_MAX;
	for (uint32_t surfaceIndex = 0; surfaceIndex < (uint32_t)surfaces.size(); ++surfaceIndex)
	{
		const PTMapMaterialSurfaceView& source = surfaces[surfaceIndex];
		if (source.surface == nullptr)
		{
			SetFailure(outValidation, PTMapMaterialLayoutFailure::InvalidSurfaceReference, surfaceIndex);
			return false;
		}
		if (source.emittedMaterialSlot >= emittedMaterialCount)
		{
			SetFailure(outValidation, PTMapMaterialLayoutFailure::InvalidEmittedSlot, surfaceIndex, source.emittedMaterialSlot);
			return false;
		}
		if (emittedSlots[source.emittedMaterialSlot])
		{
			SetFailure(outValidation, PTMapMaterialLayoutFailure::DuplicateEmittedSlot, surfaceIndex, source.emittedMaterialSlot);
			return false;
		}
		emittedSlots[source.emittedMaterialSlot] = true;

		if (chunkIndex == UINT32_MAX)
		{
			chunkIndex = source.chunkIndex;
		}
		else if (source.chunkIndex != chunkIndex)
		{
			SetFailure(outValidation, PTMapMaterialLayoutFailure::InvalidSemanticSlot, surfaceIndex, source.emittedMaterialSlot);
			return false;
		}

		CanonicalCandidate candidate = {};
		PTMapMaterialLayoutFailure failure = PTMapMaterialLayoutFailure::None;
		if (!BuildSemanticSlot(source, candidate.semantic, failure) ||
			!BuildMaterialStateKey(source.surface->material, options, candidate.stateKey, failure))
		{
			SetFailure(outValidation, failure, surfaceIndex, source.emittedMaterialSlot);
			return false;
		}
		candidate.slotId = HashSemanticSlot(candidate.semantic);
		candidate.sourceIndex = surfaceIndex;
		candidate.emittedSlot = source.emittedMaterialSlot;
		candidates.push_back(candidate);
	}

	for (uint32_t slot = 0; slot < (uint32_t)emittedSlots.size(); ++slot)
	{
		if (!emittedSlots[slot])
		{
			SetFailure(outValidation, PTMapMaterialLayoutFailure::MissingEmittedSlot, UINT32_MAX, slot);
			return false;
		}
	}

	std::sort(candidates.begin(), candidates.end(), [](const CanonicalCandidate& a, const CanonicalCandidate& b)
	{
		if (a.slotId != b.slotId)
		{
			return a.slotId < b.slotId;
		}
		return SemanticLess(a.semantic, b.semantic);
	});
	for (uint32_t canonicalSlot = 1; canonicalSlot < (uint32_t)candidates.size(); ++canonicalSlot)
	{
		const CanonicalCandidate& previous = candidates[canonicalSlot - 1];
		const CanonicalCandidate& current = candidates[canonicalSlot];
		if (previous.semantic == current.semantic)
		{
			const PTMapMaterialLayoutFailure failure = previous.stateKey == current.stateKey ?
				PTMapMaterialLayoutFailure::DuplicateSemanticSlot : PTMapMaterialLayoutFailure::AmbiguousSemanticSlot;
			SetFailure(outValidation, failure, current.sourceIndex, current.emittedSlot, canonicalSlot);
			return false;
		}
		if (previous.slotId == current.slotId)
		{
			SetFailure(outValidation, PTMapMaterialLayoutFailure::AmbiguousSemanticSlot, current.sourceIndex, current.emittedSlot, canonicalSlot);
			return false;
		}
	}

	outLayout.chunkIndex = chunkIndex;
	outLayout.canonicalSlotIds.reserve(candidates.size());
	outLayout.canonicalSemanticSlots.reserve(candidates.size());
	outLayout.canonicalMaterialStateKeys.reserve(candidates.size());
	outLayout.emittedToCanonical.resize(candidates.size(), UINT32_MAX);
	outLayout.canonicalToEmitted.reserve(candidates.size());
	uint64_t layoutKey = HashValue(HashOffset, LayoutKeyDomain);
	uint64_t stateKey = HashValue(HashOffset, StateKeyDomain);
	const uint32_t slotCount = (uint32_t)candidates.size();
	layoutKey = HashValue(layoutKey, slotCount);
	stateKey = HashValue(stateKey, slotCount);
	for (uint32_t canonicalSlot = 0; canonicalSlot < slotCount; ++canonicalSlot)
	{
		const CanonicalCandidate& candidate = candidates[canonicalSlot];
		outLayout.canonicalSlotIds.push_back(candidate.slotId);
		outLayout.canonicalSemanticSlots.push_back(candidate.semantic);
		outLayout.canonicalMaterialStateKeys.push_back(candidate.stateKey);
		outLayout.emittedToCanonical[candidate.emittedSlot] = canonicalSlot;
		outLayout.canonicalToEmitted.push_back(candidate.emittedSlot);
		layoutKey = HashValue(layoutKey, candidate.slotId);
		stateKey = HashValue(stateKey, candidate.slotId);
		stateKey = HashValue(stateKey, candidate.stateKey);
	}
	outLayout.layoutKey = layoutKey;
	outLayout.stateKey = stateKey;
	outLayout.valid = true;
	outValidation.valid = true;
	return true;
}

#ifndef NRI_MAP_MATERIAL_LAYOUT_VIEW_ONLY
bool BuildCanonicalPTMapMaterialLayout(
	const std::vector<PTMapMaterialSurfaceSlot>& surfaces,
	CanonicalPTMapMaterialLayout& outLayout,
	PTMapMaterialLayoutValidation& outValidation,
	const PTMapMaterialLayoutOptions& options)
{
	std::vector<PTMapMaterialSurfaceView> views;
	views.reserve(surfaces.size());
	for (const PTMapMaterialSurfaceSlot& source : surfaces)
	{
		PTMapMaterialSurfaceView view = {};
		if (source.surface != nullptr)
		{
			view.surface = &source.surface->surface;
			view.surfaceKind = (uint32_t)source.surface->kind;
			view.keyPrimary = source.surface->key.primary;
			view.keySecondary = source.surface->key.secondary;
			view.chunkIndex = source.surface->chunkIndex;
		}
		view.emittedMaterialSlot = source.emittedMaterialSlot;
		views.push_back(view);
	}
	return BuildCanonicalPTMapMaterialLayoutFromViews(views, outLayout, outValidation, options);
}
#endif

bool ValidatePTMapMaterialLayoutCompatibility(
	const CanonicalPTMapMaterialLayout& retained,
	const CanonicalPTMapMaterialLayout& current,
	PTMapMaterialLayoutValidation& outValidation)
{
	outValidation = {};
	if (!retained.valid || !current.valid ||
		retained.canonicalSlotIds.size() != retained.canonicalSemanticSlots.size() ||
		current.canonicalSlotIds.size() != current.canonicalSemanticSlots.size())
	{
		SetFailure(outValidation, PTMapMaterialLayoutFailure::InvalidSemanticSlot);
		return false;
	}
	if (retained.canonicalSlotIds.size() != current.canonicalSlotIds.size())
	{
		SetFailure(outValidation, PTMapMaterialLayoutFailure::LayoutMismatch, UINT32_MAX, UINT32_MAX, 0);
		return false;
	}
	for (uint32_t slot = 0; slot < (uint32_t)retained.canonicalSlotIds.size(); ++slot)
	{
		if (retained.canonicalSlotIds[slot] != current.canonicalSlotIds[slot] ||
			!(retained.canonicalSemanticSlots[slot] == current.canonicalSemanticSlots[slot]))
		{
			SetFailure(outValidation, PTMapMaterialLayoutFailure::LayoutMismatch, UINT32_MAX, UINT32_MAX, slot);
			return false;
		}
	}
	outValidation.valid = true;
	return true;
}

const char* GetPTMapMaterialLayoutFailureName(PTMapMaterialLayoutFailure failure)
{
	switch (failure)
	{
	case PTMapMaterialLayoutFailure::None: return "none";
	case PTMapMaterialLayoutFailure::EmptyInput: return "empty-input";
	case PTMapMaterialLayoutFailure::InvalidSurfaceReference: return "invalid-surface-reference";
	case PTMapMaterialLayoutFailure::UnsupportedSurface: return "unsupported-surface";
	case PTMapMaterialLayoutFailure::MissingSemanticSlot: return "missing-semantic-slot";
	case PTMapMaterialLayoutFailure::InvalidSemanticSlot: return "invalid-semantic-slot";
	case PTMapMaterialLayoutFailure::DuplicateSemanticSlot: return "duplicate-semantic-slot";
	case PTMapMaterialLayoutFailure::AmbiguousSemanticSlot: return "ambiguous-semantic-slot";
	case PTMapMaterialLayoutFailure::InvalidEmittedSlot: return "invalid-emitted-slot";
	case PTMapMaterialLayoutFailure::DuplicateEmittedSlot: return "duplicate-emitted-slot";
	case PTMapMaterialLayoutFailure::MissingEmittedSlot: return "missing-emitted-slot";
	case PTMapMaterialLayoutFailure::MissingMaterialIdentity: return "missing-material-identity";
	case PTMapMaterialLayoutFailure::NonFiniteMaterialState: return "non-finite-material-state";
	case PTMapMaterialLayoutFailure::LayoutMismatch: return "layout-mismatch";
	}
	return "unknown";
}
}
