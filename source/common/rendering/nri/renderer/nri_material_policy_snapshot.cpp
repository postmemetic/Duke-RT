#include "nri_material_policy_snapshot.h"

#include "nri_scene_lights.h"
#include "lightoverlay.h"
#include "fluidoverlay.h"
#include "texinfo.h"
#include "texturemanager.h"
#include "perf_audit.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace
{
	using nri_scene::MaterialData;
	using nri_scene::MaterialLightingMetadata;
	using Heuristic = SceneLightSystem::EmissiveSurfaceRegistry::EmissiveHeuristicRule;
	uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); return bits; }
	template<class T> void Put(std::vector<uint8_t>& bytes, const T& value)
	{
		const auto* data = reinterpret_cast<const uint8_t*>(&value);
		bytes.insert(bytes.end(), data, data + sizeof(value));
	}
	void PutName(std::vector<uint8_t>& bytes, const char* value)
	{
		const size_t length = std::strlen(value);
		Put(bytes, length);
		bytes.insert(bytes.end(), value, value + length);
	}
	std::string Normalize(const char* name, bool fluid)
	{
		std::string result = name;
		for (char& ch : result)
			ch = fluid && ch == '\\' ? '/' : (char)std::tolower((unsigned char)ch);
		const size_t slash = result.find_last_of(fluid ? "/" : "/\\");
		const size_t dot = result.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) result.resize(dot);
		return result;
	}
	uint32_t TileTexture(int tile)
	{
		if (tile < 0) return 0;
		const auto id = tileGetTextureID(tile);
		return id.isValid() ? (uint32_t)id.GetIndex() : 0u;
	}
	struct GlowRule
	{
		std::vector<uint32_t> ids;
		std::vector<std::pair<uint32_t, uint32_t>> ranges;
		std::vector<std::string> names;
		float blend = 1;
	};
	struct FluidRule
	{
		std::vector<int> tiles, families;
		std::vector<std::pair<int, int>> ranges;
		std::vector<std::string> names;
		bool hasPalette = false, hasSurface = false;
		int palette = 0;
		FluidOverlaySurfaceClass surface = FluidOverlaySurfaceClass::Any;
		uint32_t binding = UINT32_MAX;
	};
	struct GlowIdentity
	{
		uint32_t texture, base;
		FGameTexture* object;
		bool operator==(const GlowIdentity& rhs) const { return texture == rhs.texture && base == rhs.base && object == rhs.object; }
	};
	struct GlowIdentityHash
	{
		size_t operator()(const GlowIdentity& key) const
		{
			return std::hash<FGameTexture*>{}(key.object) ^ (size_t)key.texture * 0x9e3779b1u ^ (size_t)key.base * 0x85ebca6bu;
		}
	};
	struct FluidIdentity
	{
		uint32_t texture, palette;
		FluidOverlaySurfaceClass surface;
		bool operator==(const FluidIdentity& rhs) const { return texture == rhs.texture && palette == rhs.palette && surface == rhs.surface; }
	};
	struct FluidIdentityHash
	{
		size_t operator()(const FluidIdentity& key) const
		{
			return (size_t)key.texture * 0x9e3779b1u ^ (size_t)key.palette * 0x85ebca6bu ^ (size_t)key.surface;
		}
	};
	struct FluidTexture
	{
		int tile = -1, family = -1;
		std::string name;
	};
	bool FluidEligible(const MaterialLightingMetadata& metadata)
	{
		using namespace nri_scene;
		return (metadata.sourceType == SurfaceSourceType::MapFloorSection || metadata.sourceType == SurfaceSourceType::MapCeilingSection ||
			metadata.sourceType == SurfaceSourceType::MapWallBand) &&
			(metadata.materialFlags & (MaterialFlag_Sprite | MaterialFlag_Mirror | MaterialFlag_Sky | MaterialFlag_Portal | MaterialFlag_PlainMirror)) == 0;
	}

	void ApplyEmission(const MaterialLightingMetadata& metadata, const Heuristic* rule,
		const NRIMaterialPolicySnapshot::Settings& settings, MaterialData& row)
	{
		using namespace nri_scene;
		uint32_t mode = MaterialEmissiveMode_None, index = UINT32_MAX;
		float intensity = 0, color[3] = {};
		if (metadata.emissiveMode != MaterialEmissiveMode_None && metadata.emissiveIntensity > 0.0f)
		{
			mode = metadata.emissiveMode; index = metadata.emissiveTextureIndex; intensity = metadata.emissiveIntensity;
			std::copy_n(metadata.emissiveColor, 3, color);
		}
		if (rule != nullptr)
		{
			const float baseIntensity = intensity > 0.0f ? intensity : 1.0f;
			switch (rule->emissiveMode)
			{
			case MaterialEmissiveMode_UseBaseTexture:
				mode = MaterialEmissiveMode_UseBaseTexture; index = metadata.textureIndex;
				std::copy_n(metadata.averageColor, 3, color);
				break;
			case MaterialEmissiveMode_UseGlowmapTexture:
				if (metadata.glowmapTextureIndex != UINT32_MAX) { mode = MaterialEmissiveMode_UseGlowmapTexture; index = metadata.glowmapTextureIndex; }
				else mode = MaterialEmissiveMode_UseConstantColor;
				for (int i = 0; i < 3; ++i) color[i] = metadata.glowColor[i] > 0.0f ? metadata.glowColor[i] : metadata.averageColor[i];
				break;
			case MaterialEmissiveMode_UseConstantColor:
				mode = MaterialEmissiveMode_UseConstantColor;
				for (int i = 0; i < 3; ++i) color[i] = rule->hasExplicitColor ? rule->emissiveColor[i] :
					(metadata.glowColor[i] > 0.0f ? metadata.glowColor[i] : metadata.averageColor[i]);
				break;
			default: break;
			}
			intensity = baseIntensity * std::max(rule->intensityScale, 0.0f);
		}
		const bool glow = mode == MaterialEmissiveMode_UseGlowmapTexture ||
			(metadata.lightingFlags & (MaterialLightingFlag_TextureGlowing | MaterialLightingFlag_TextureAutoGlowing | MaterialLightingFlag_HasGlowmap)) != 0;
		bool emits = mode != MaterialEmissiveMode_None && !(intensity <= 0.0f);
		if (emits) { intensity *= glow ? std::max(settings.glowScale, 0.0f) : 1.0f; emits = intensity > 0.0f; }
		if (!emits)
		{
			std::fill_n(row.emissiveColor, 3, 0.0f);
			row.emissiveIntensity = row.emissiveMaskScale = 0.0f;
			row.emissiveMode = MaterialEmissiveMode_None; row.emissiveTextureIndex = UINT32_MAX;
			return;
		}
		std::copy_n(color, 3, row.emissiveColor);
		row.emissiveIntensity = intensity;
		row.emissiveMaskScale = std::max(glow ? std::clamp(settings.glowFalloff, 0.25f, 4.0f) : 1.0f, 0.0f);
		row.emissiveMode = mode; row.emissiveTextureIndex = index;
		if (row.materialClass != 3u) row.materialClass = 2u;
	}
	void ApplyActor(uint32_t bits, bool voxelPolicy, float boost, MaterialData& row)
	{
		using namespace nri_scene;
		using namespace nri_material_policy;
		if (!voxelPolicy && (bits & ActorMaterialOverride_NoShadowReceive)) row.lightingFlags |= MaterialLightingFlag_NoShadowReceive;
		if (!voxelPolicy && (bits & ActorMaterialOverride_NoShadowCast)) row.lightingFlags |= MaterialLightingFlag_NoShadowCast;
		if (bits & ActorMaterialOverride_SmokeForeground) row.lightingFlags |= MaterialLightingFlag_SmokeForeground;
		if (!voxelPolicy && (bits & ActorMaterialOverride_Fullbright))
		{
			row.flags |= MaterialFlag_Fullbright; row.lightLevel = 1.0f;
			row.roughnessHint = (row.flags & MaterialFlag_Sprite) ? 0.45f : (row.flags & MaterialFlag_Flat) ? 0.60f : 0.55f;
			row.lightingFlags |= MaterialLightingFlag_MaterialFullbright;
			row.emissiveMode = MaterialEmissiveMode_UseBaseTexture; row.emissiveTextureIndex = row.textureIndex;
			row.emissiveIntensity = row.emissiveMaskScale = 1.0f; row.emissiveReserved = boost;
			std::fill_n(row.emissiveColor, 3, 1.0f);
		}
	}
}

struct NRIMaterialPolicySnapshot::Impl
{
	Settings settings;
	const nri_material_policy::ActorMaterialOverrideMap* actors = nullptr;
	bool fluids = false, quarantined = false;
	std::vector<uint8_t> sourceBytes, scratchBytes;
	std::unordered_map<uint32_t, Heuristic> heuristics;
	std::vector<GlowRule> glowRules;
	std::vector<FluidRule> fluidRules;
	std::unordered_map<FGameTexture*, std::string> glowNames;
	std::unordered_map<uint32_t, FluidTexture> fluidTextures;
	std::unordered_map<GlowIdentity, float, GlowIdentityHash> glowMatches;
	std::unordered_map<FluidIdentity, uint32_t, FluidIdentityHash> fluidMatches;
	uint64_t transformedRows = 0, selectorHits = 0, selectorTests = 0, identityChecks = 0, normalizations = 0;

	void SnapshotRules(const SceneLightSystem& lights, const ResolvedLightOverlaySet& overlays, const ResolvedFluidOverlaySet* fluidSet)
	{
		scratchBytes.clear();
		const auto& rawHeuristics = lights.GetEmissiveSurfaces().textureRules;
		Put(scratchBytes, rawHeuristics.size());
		for (const auto& rule : rawHeuristics)
		{
			Put(scratchBytes, rule.textureId); Put(scratchBytes, rule.emissiveMode); Put(scratchBytes, Bits(rule.intensityScale));
			Put(scratchBytes, rule.hasExplicitColor);
			for (float color : rule.emissiveColor) Put(scratchBytes, Bits(color));
		}
		// Serialized source includes only fields that can affect material rows,
		// plus current tile-to-texture conversions. No struct padding or generation.
		size_t glowCount = 0; for (const auto& rule : overlays.emissiveMaterialResponseRules) glowCount += rule.hasVisibleGlowBlend ? 1 : 0;
		Put(scratchBytes, glowCount);
		for (const auto& rule : overlays.emissiveMaterialResponseRules)
		{
			if (!rule.hasVisibleGlowBlend) continue;
			Put(scratchBytes, Bits(rule.visibleGlowBlend));
			Put(scratchBytes, rule.tileFilters.Size()); for (int tile : rule.tileFilters) Put(scratchBytes, TileTexture(tile));
			Put(scratchBytes, rule.tileRanges.Size()); for (const auto& range : rule.tileRanges) { Put(scratchBytes, TileTexture(range.first)); Put(scratchBytes, TileTexture(range.last)); }
			Put(scratchBytes, rule.textureNames.Size()); for (const auto& name : rule.textureNames) PutName(scratchBytes, name.GetChars());
		}
		Put(scratchBytes, fluids);
		if (fluidSet != nullptr)
		{
			Put(scratchBytes, fluidSet->materialRules.Size());
			for (const auto& rule : fluidSet->materialRules)
			{
				Put(scratchBytes, rule.valid); Put(scratchBytes, rule.resolvedDisable); Put(scratchBytes, rule.bindingIndex);
				Put(scratchBytes, rule.hasPalette); Put(scratchBytes, rule.palette);
				Put(scratchBytes, rule.hasSurfaceClass); Put(scratchBytes, rule.surfaceClass);
				Put(scratchBytes, rule.tileFilters.Size()); for (int tile : rule.tileFilters) Put(scratchBytes, tile);
				Put(scratchBytes, rule.tileRanges.Size()); for (const auto& range : rule.tileRanges) { Put(scratchBytes, range.first); Put(scratchBytes, range.last); }
				Put(scratchBytes, rule.tileFamilies.Size()); for (int family : rule.tileFamilies) Put(scratchBytes, family);
				Put(scratchBytes, rule.textureNames.Size()); for (const auto& name : rule.textureNames) PutName(scratchBytes, name.GetChars());
			}
		}
		PerfAuditAdd(PerfAuditMetric::MaterialPolicySnapshotBytesCompared, scratchBytes.size());
		if (scratchBytes == sourceBytes) return;
		sourceBytes = scratchBytes;
		heuristics.clear(); glowRules.clear(); fluidRules.clear();
		for (const auto& rule : rawHeuristics) heuristics.emplace(rule.textureId, rule); // first match wins
		for (const auto& rule : overlays.emissiveMaterialResponseRules)
		{
			if (!rule.hasVisibleGlowBlend) continue;
			GlowRule compiled; compiled.blend = std::clamp(rule.visibleGlowBlend, 0.0f, 3.0f);
			for (int tile : rule.tileFilters) { const uint32_t id = TileTexture(tile); if (id != 0) compiled.ids.push_back(id); }
			for (const auto& range : rule.tileRanges)
			{
				uint32_t a = TileTexture(range.first), b = TileTexture(range.last);
				if (a == 0 || b == 0) continue;
				if (a > b) std::swap(a, b);
				compiled.ranges.emplace_back(a, b);
			}
			for (const auto& name : rule.textureNames) { compiled.names.push_back(Normalize(name.GetChars(), false)); ++normalizations; }
			glowRules.push_back(std::move(compiled));
		}
		if (fluidSet != nullptr) for (const auto& rule : fluidSet->materialRules)
		{
			if (!rule.valid) continue;
			FluidRule compiled;
			compiled.hasPalette = rule.hasPalette; compiled.palette = rule.palette;
			compiled.hasSurface = rule.hasSurfaceClass; compiled.surface = rule.surfaceClass;
			compiled.binding = rule.resolvedDisable ? UINT32_MAX : rule.bindingIndex;
			for (int tile : rule.tileFilters) compiled.tiles.push_back(tile);
			for (int family : rule.tileFamilies) compiled.families.push_back(family);
			for (const auto& range : rule.tileRanges) compiled.ranges.emplace_back(range.first, range.last);
			for (const auto& name : rule.textureNames) { compiled.names.push_back(Normalize(name.GetChars(), true)); ++normalizations; }
			fluidRules.push_back(std::move(compiled));
		}
		PerfAuditAdd(PerfAuditMetric::MaterialPolicySelectorBuilds);
	}

	float GlowBlend(const MaterialLightingMetadata& metadata)
	{
		if (glowRules.empty()) return settings.glowBlend;
		const GlowIdentity key = { metadata.textureId, metadata.baseTextureId, metadata.texture };
		const auto found = glowMatches.find(key);
		if (found != glowMatches.end()) { ++selectorHits; return found->second; }
		++identityChecks;
		auto name = glowNames.find(metadata.texture);
		if (name == glowNames.end())
		{
			name = glowNames.emplace(metadata.texture, Normalize(metadata.texture ? metadata.texture->GetName().GetChars() : "", false)).first;
			++normalizations;
		}
		float blend = settings.glowBlend;
		for (const auto& rule : glowRules)
		{
			++selectorTests;
			bool match = false;
			for (uint32_t id : rule.ids) if (id == key.texture || id == key.base) { match = true; break; }
			if (!match) for (const auto& range : rule.ranges)
				if ((key.texture >= range.first && key.texture <= range.second) || (key.base >= range.first && key.base <= range.second)) { match = true; break; }
			if (!match) for (const auto& selector : rule.names) if (name->second == selector) { match = true; break; }
			if (match) blend = rule.blend; // last matching rule wins
		}
		glowMatches.emplace(key, blend);
		return blend;
	}
	uint32_t FluidBinding(const MaterialLightingMetadata& metadata)
	{
		if (!fluids || !FluidEligible(metadata) || metadata.authoredTextureId == UINT32_MAX || fluidRules.empty()) return UINT32_MAX;
		const FluidIdentity key = { metadata.authoredTextureId, metadata.paletteIndex,
			metadata.sourceType == nri_scene::SurfaceSourceType::MapWallBand ? FluidOverlaySurfaceClass::Wall : FluidOverlaySurfaceClass::Flat };
		const auto found = fluidMatches.find(key);
		if (found != fluidMatches.end()) { ++selectorHits; return found->second; }
		++identityChecks;
		auto texture = fluidTextures.find(key.texture);
		if (texture == fluidTextures.end())
		{
			FluidTexture identity;
			if (key.texture >= (uint32_t)firstarttile && key.texture <= (uint32_t)(firstarttile + maxarttile)) identity.tile = legacyTileNum(FSetTextureID((int)key.texture));
			identity.family = identity.tile;
			if (identity.tile >= 0)
			{
				const auto& animation = GetExtInfo(FSetTextureID((int)key.texture)).picanm;
				if (animation.type() == PICANM_ANIMTYPE_BACK) identity.family -= (int)animation.num;
			}
			if (auto* object = TexMan.GameByIndex((int)key.texture)) identity.name = Normalize(object->GetName().GetChars(), true);
			++normalizations;
			texture = fluidTextures.emplace(key.texture, std::move(identity)).first;
		}
		const auto& identity = texture->second;
		uint32_t binding = UINT32_MAX;
		for (const auto& rule : fluidRules)
		{
			++selectorTests;
			if (rule.hasPalette && rule.palette != (int)key.palette) continue;
			if (rule.hasSurface && rule.surface != FluidOverlaySurfaceClass::Any && rule.surface != key.surface) continue;
			bool match = false;
			if (identity.tile >= 0) for (int tile : rule.tiles) if (tile == identity.tile) { match = true; break; }
			if (!match) for (const auto& range : rule.ranges) if (identity.tile >= range.first && identity.tile <= range.second) { match = true; break; }
			if (!match && identity.family >= 0) for (int family : rule.families) if (family == identity.family) { match = true; break; }
			if (!match && !identity.name.empty()) for (const auto& name : rule.names) if (identity.name == name) { match = true; break; }
			if (match) binding = rule.binding; // last matching rule, including disable
		}
		fluidMatches.emplace(key, binding);
		return binding;
	}
};

NRIMaterialPolicySnapshot::NRIMaterialPolicySnapshot() = default;
NRIMaterialPolicySnapshot::~NRIMaterialPolicySnapshot() = default;
void NRIMaterialPolicySnapshot::Reset() { mImpl.reset(); }
bool NRIMaterialPolicySnapshot::Quarantined() const { return mImpl != nullptr && mImpl->quarantined; }
void NRIMaterialPolicySnapshot::Quarantine()
{
	if (mImpl == nullptr) mImpl = std::make_unique<Impl>();
	if (!mImpl->quarantined) { mImpl->quarantined = true; PerfAuditAdd(PerfAuditMetric::MaterialPolicyQuarantines); }
}
void NRIMaterialPolicySnapshot::Begin(const SceneLightSystem& lights, const ResolvedLightOverlaySet& overlays,
	const nri_material_policy::ActorMaterialOverrideMap* actors, const ResolvedFluidOverlaySet* fluids, const Settings& settings)
{
	PerfAuditScope audit(PerfAuditMetric::MaterialPolicySnapshotNs);
	if (mImpl == nullptr) mImpl = std::make_unique<Impl>();
	auto& state = *mImpl;
	state.settings = settings; state.actors = actors; state.fluids = fluids != nullptr;
	state.transformedRows = state.selectorHits = state.selectorTests = state.identityChecks = state.normalizations = 0;
	// External names/animation mappings are borrowed only for this operation.
	state.glowNames.clear(); state.fluidTextures.clear(); state.glowMatches.clear(); state.fluidMatches.clear();
	state.SnapshotRules(lights, overlays, fluids);
	PerfAuditAdd(PerfAuditMetric::MaterialPolicySettingsSnapshots);
}
void NRIMaterialPolicySnapshot::Transform(const MaterialLightingMetadata* metadata, MaterialData& row)
{
	auto& state = *mImpl;
	++state.transformedRows;
	if (state.fluids) row.fluidBindingIndex = UINT32_MAX;
	if (metadata == nullptr) return;

	const Heuristic* rule = nullptr;
	if (!state.heuristics.empty())
	{
		const auto found = state.heuristics.find(metadata->textureId);
		if (found != state.heuristics.end()) rule = &found->second;
	}
	// Scalar emission is cheaper than constructing/comparing a full dependency
	// key and copying a retained GPU row. Reuse only the costly ordered selectors;
	// the product owner still decides which final rows need publication.
	ApplyEmission(*metadata, rule, state.settings, row);
	if (row.emissiveMode == nri_scene::MaterialEmissiveMode_UseGlowmapTexture)
		row.emissiveReserved = state.GlowBlend(*metadata);
	if (state.actors != nullptr && metadata->actorIndex >= 0)
	{
		const auto actor = state.actors->find(metadata->actorIndex);
		if (actor != state.actors->end())
			ApplyActor(actor->second.bits, metadata->voxelPalettePolicyApplied, state.settings.fullbrightBoost, row);
	}
	if (state.fluids) row.fluidBindingIndex = state.FluidBinding(*metadata);
}
bool NRIMaterialPolicySnapshot::Finish()
{
	PerfAuditScope audit(PerfAuditMetric::MaterialPolicySnapshotNs);
	if (mImpl == nullptr) return false;
	const auto& state = *mImpl;
	PerfAuditAdd(PerfAuditMetric::MaterialPolicyTransformedRows, state.transformedRows);
	PerfAuditAdd(PerfAuditMetric::MaterialPolicySelectorHits, state.selectorHits);
	PerfAuditAdd(PerfAuditMetric::MaterialPolicyCompiledSelectorTests, state.selectorTests);
	PerfAuditAdd(PerfAuditMetric::MaterialPolicyIdentityChecks, state.identityChecks);
	PerfAuditAdd(PerfAuditMetric::MaterialPolicyNameNormalizations, state.normalizations);
	if (PerfAuditActive())
	{
		// Capacity estimates describe owned payload and hash bucket storage, not
		// allocator headers/node links. They are charged inside owner upkeep.
		uint64_t compiledBytes = state.glowRules.capacity() * sizeof(GlowRule) + state.fluidRules.capacity() * sizeof(FluidRule) +
			state.heuristics.bucket_count() * sizeof(void*) + state.heuristics.size() * sizeof(decltype(state.heuristics)::value_type);
		for (const auto& rule : state.glowRules)
		{
			compiledBytes += rule.ids.capacity() * sizeof(uint32_t) + rule.ranges.capacity() * sizeof(decltype(rule.ranges)::value_type) + rule.names.capacity() * sizeof(std::string);
			for (const auto& name : rule.names) compiledBytes += name.capacity();
		}
		for (const auto& rule : state.fluidRules)
		{
			compiledBytes += (rule.tiles.capacity() + rule.families.capacity()) * sizeof(int) + rule.ranges.capacity() * sizeof(decltype(rule.ranges)::value_type) + rule.names.capacity() * sizeof(std::string);
			for (const auto& name : rule.names) compiledBytes += name.capacity();
		}
		uint64_t identityBytes = (state.glowNames.bucket_count() + state.fluidTextures.bucket_count() + state.glowMatches.bucket_count() + state.fluidMatches.bucket_count()) * sizeof(void*) +
			state.glowNames.size() * sizeof(decltype(state.glowNames)::value_type) + state.fluidTextures.size() * sizeof(decltype(state.fluidTextures)::value_type) +
			state.glowMatches.size() * sizeof(decltype(state.glowMatches)::value_type) + state.fluidMatches.size() * sizeof(decltype(state.fluidMatches)::value_type);
		for (const auto& item : state.glowNames) identityBytes += item.second.capacity();
		for (const auto& item : state.fluidTextures) identityBytes += item.second.name.capacity();
		PerfAuditSet(PerfAuditMetric::MaterialPolicyCompiledStorageEstimateBytes, compiledBytes);
		PerfAuditSet(PerfAuditMetric::MaterialPolicyIdentityStorageEstimateBytes, identityBytes);
	}
	PerfAuditSet(PerfAuditMetric::MaterialPolicyRowCapacityBytes, 0);
	PerfAuditSet(PerfAuditMetric::MaterialPolicySnapshotCapacityBytes, state.sourceBytes.capacity() + state.scratchBytes.capacity());
	PerfAuditSet(PerfAuditMetric::MaterialPolicyIdentityEntries, state.glowNames.size() + state.fluidTextures.size() + state.glowMatches.size() + state.fluidMatches.size());
	PerfAuditSet(PerfAuditMetric::MaterialPolicyIdentityBuckets, state.glowNames.bucket_count() + state.fluidTextures.bucket_count() + state.glowMatches.bucket_count() + state.fluidMatches.bucket_count());
	return state.selectorHits != 0;
}
