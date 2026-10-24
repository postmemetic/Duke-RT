#include "nri_sector_color_policy.h"
#include "lightoverlay.h"
#include "gamefuncs.h"
#include "c_cvars.h"
#include "printf.h"

#include <algorithm>
#include <vector>

CVAR(Bool, nri_ptsectorcolor, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

namespace nri_scene
{
namespace
{
	const ParsedLightOverlaySectorColorRule* SectorOverride(int index)
	{
		const ParsedLightOverlaySectorColorRule* result = nullptr;
		for (const auto& rule : GetResolvedLightOverlaySet().sectorColorRules)
			if (rule.sector == index && rule.mapName.IsNotEmpty()) result = &rule;
		return result;
	}

	const ParsedLightOverlaySectorColorRule* PaletteProfile(int palette)
	{
		const ParsedLightOverlaySectorColorRule* result = nullptr;
		for (const auto& rule : GetResolvedLightOverlaySet().sectorColorRules)
			if (rule.mapName.IsEmpty() && rule.palette == palette) result = &rule;
		return result != nullptr && result->enabled ? result : nullptr;
	}

	SectorEmissionHue DirectHue(int index)
	{
		SectorEmissionHue result;
		const auto* overrideRule = SectorOverride(index);
		if (overrideRule != nullptr && !overrideRule->enabled) return result;
		const auto& sec = sector[index];
		const int source = overrideRule != nullptr ? overrideRule->paletteSource : 0;
		const auto* profile = source != 2 ? PaletteProfile(sec.floorpal) : nullptr;
		int palette = sec.floorpal;
		if (profile == nullptr && source != 1 && (source == 2 || (sec.ceilingstat & CSTAT_SECTOR_SKY) == 0))
		{
			profile = PaletteProfile(sec.ceilingpal);
			palette = sec.ceilingpal;
		}
		const auto* colorRule = overrideRule != nullptr && overrideRule->hasColor ? overrideRule : profile;
		if (colorRule == nullptr) return result;
		const float* color = colorRule->color;
		const float luminance = color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
		if (luminance <= 0.0f) return result;
		for (int channel = 0; channel < 3; ++channel) result.rgb[channel] = color[channel] / luminance;
		result.sectorIndex = index;
		result.palette = profile != nullptr ? palette : -1;
		result.strength = overrideRule != nullptr && overrideRule->hasStrength ? overrideRule->strength :
			profile != nullptr ? profile->strength : 1.0f;
		result.gain = overrideRule != nullptr && overrideRule->hasGain ? overrideRule->gain :
			profile != nullptr ? profile->gain : 1.0f;
		return result;
	}

	bool OpenColorPortal(const sectortype& front, const walltype& wal)
	{
		if (!wal.twoSided() || wal.nextsector < 0 || (unsigned)wal.nextsector >= sector.Size() ||
			(wal.cstat & (CSTAT_WALL_1WAY | CSTAT_WALL_MASKED)) != 0) return false;
		const auto& back = sector[wal.nextsector];
		const auto midpoint = (wal.pos + wal.point2Wall()->pos) * 0.5;
		double frontCeiling, frontFloor, backCeiling, backFloor;
		calcSlope(&front, midpoint, &frontCeiling, &frontFloor);
		calcSlope(&back, midpoint, &backCeiling, &backFloor);
		return std::min(frontFloor, backFloor) - std::max(frontCeiling, backCeiling) > 0.01;
	}
}

bool TransfersSectorPalette(int sectorIndex, int palette)
{
	if (!nri_ptsectorcolor || sectorIndex < 0 || (unsigned)sectorIndex >= sector.Size()) return false;
	const auto* overrideRule = SectorOverride(sectorIndex);
	if (overrideRule != nullptr && (!overrideRule->enabled || !overrideRule->neutralize)) return false;
	const auto* profile = PaletteProfile(palette);
	return profile != nullptr && profile->neutralize;
}

uint64_t SectorColorMaterialPolicySignature()
{
	uint64_t hash = nri_ptsectorcolor ? 1469598103934665603ull : 0;
	if (!nri_ptsectorcolor) return hash;
	const auto append = [&](uint32_t value) { hash = (hash ^ value) * 1099511628211ull; };
	// Hue changes affect light payloads, but not texture palettes or their mip chains.
	for (const auto& rule : GetResolvedLightOverlaySet().sectorColorRules)
	{
		// Only the last rule for a sector determines its material policy.
		if (rule.mapName.IsNotEmpty() && SectorOverride(rule.sector) != &rule) continue;
		// Default map policy only adjusts emission. Creating an RGB/numeric override
		// must not invalidate material palettes that it leaves unchanged.
		if (rule.mapName.IsNotEmpty() && rule.enabled && rule.neutralize) continue;
		append((uint32_t)rule.palette);
		append((uint32_t)rule.sector);
		append(rule.enabled);
		append(rule.neutralize);
	}
	return hash;
}

uint32_t ResolveSectorRenderPalette(uint32_t palette, SurfaceSourceType source, int sectorIndex, uint32_t flags)
{
	if ((flags & (MaterialFlag_Sprite | MaterialFlag_Sky | MaterialFlag_Mirror | MaterialFlag_Portal)) != 0) return palette;
	const bool mapSurface = source == SurfaceSourceType::DrawListWall || source == SurfaceSourceType::FloorFlat ||
		source == SurfaceSourceType::CeilingFlat || source == SurfaceSourceType::MapWallBand ||
		source == SurfaceSourceType::MapFloorSection || source == SurfaceSourceType::MapCeilingSection;
	return mapSurface && TransfersSectorPalette(sectorIndex, (int)palette) ? 0u : palette;
}

SectorEmissionHue ResolveSectorEmissionHue(int sectorIndex, bool neighbors)
{
	if (!nri_ptsectorcolor || sectorIndex < 0 || (unsigned)sectorIndex >= sector.Size()) return {};
	const auto* overrideRule = SectorOverride(sectorIndex);
	if (overrideRule != nullptr && !overrideRule->enabled) return {};
	auto result = DirectHue(sectorIndex);
	if (result.Active() || !neighbors) return result;
	// Only local, vertically open Build portals connect automatic color controls.
	// Breadth-first traversal chooses fewest hops, then lowest sector id on ties.
	std::vector<int> visited = { sectorIndex };
	std::vector<int> frontier = { sectorIndex };
	for (int hop = 1; hop <= 2; ++hop)
	{
		std::vector<int> next;
		for (int current : frontier)
		{
			for (const auto& wal : sector[current].walls)
			{
				if (!OpenColorPortal(sector[current], wal) || std::find(visited.begin(), visited.end(), wal.nextsector) != visited.end()) continue;
				visited.push_back(wal.nextsector);
				const auto* policy = SectorOverride(wal.nextsector);
				if (policy != nullptr && !policy->enabled) continue;
				next.push_back(wal.nextsector);
			}
		}
		std::sort(next.begin(), next.end());
		for (int candidate : next)
		{
			result = DirectHue(candidate);
			if (result.Active())
			{
				result.portalHops = hop;
				// A local numeric correction also applies when the target hue is borrowed.
				if (overrideRule != nullptr && overrideRule->hasStrength) result.strength = overrideRule->strength;
				if (overrideRule != nullptr && overrideRule->hasGain) result.gain = overrideRule->gain;
				return result;
			}
		}
		frontier.swap(next);
	}
	return {};
}

int FindSectorColorAnchor(const float renderPosition[3])
{
	// Authored ceiling/sky lights can deliberately sit outside the sector's Z range.
	// Prefer full containment, then deterministic XY containment as a fallback.
	const DVector2 point(renderPosition[0], -renderPosition[2]);
	const double z = -renderPosition[1];
	int fallback = -1;
	for (unsigned i = 0; i < sector.Size(); ++i)
	{
		if (!inside(point.X, point.Y, &sector[i])) continue;
		if (fallback < 0) fallback = (int)i;
		if (z >= getceilzofslopeptr(&sector[i], point) && z <= getflorzofslopeptr(&sector[i], point)) return (int)i;
	}
	return fallback;
}

void BuildEmissionColorResponse(const SectorEmissionHue& hue, float response[4])
{
	// RGB weights replace hue; W retains authored color. Identity is (0,0,0,1).
	const float replacement = hue.Active() ? hue.strength * hue.gain : 0.0f;
	for (int channel = 0; channel < 3; ++channel) response[channel] = hue.rgb[channel] * replacement;
	response[3] = hue.Active() ? (1.0f - hue.strength) * hue.gain : 1.0f;
}

void ApplyEmissionColorResponse(float color[3], const SectorEmissionHue& hue)
{
	float response[4];
	BuildEmissionColorResponse(hue, response);
	const float luminance = std::max(0.0f, color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f);
	for (int channel = 0; channel < 3; ++channel) color[channel] = color[channel] * response[3] + response[channel] * luminance;
}

void PrintSectorColorPolicy(int sectorIndex)
{
	for (unsigned i = 0; i < sector.Size(); ++i)
	{
		if (sectorIndex >= 0 && i != (unsigned)sectorIndex) continue;
		const auto hue = ResolveSectorEmissionHue((int)i);
		if (sectorIndex < 0 && !hue.Active()) continue;
		const auto& sec = sector[i];
		Printf("NRI PT sector color: sector=%u floorpal=%u ceilingpal=%u source=%d palette=%d hops=%d hue=(%.6f,%.6f,%.6f) neutral_floor=%u neutral_ceiling=%u enabled=%u strength=%.6f gain=%.6f\n",
			i, (unsigned)sec.floorpal, (unsigned)sec.ceilingpal, hue.sectorIndex, hue.palette, hue.portalHops,
			hue.rgb[0], hue.rgb[1], hue.rgb[2], TransfersSectorPalette(i, sec.floorpal) ? 1u : 0u,
			TransfersSectorPalette(i, sec.ceilingpal) && (sec.ceilingstat & CSTAT_SECTOR_SKY) == 0 ? 1u : 0u,
			nri_ptsectorcolor ? 1u : 0u, hue.strength, hue.gain);
	}
}
}
