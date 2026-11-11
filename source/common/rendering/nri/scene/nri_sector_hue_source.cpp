#include "nri_sector_hue_product.h"
#include "lightoverlay.h"
#include "gamefuncs.h"
#include "c_cvars.h"
#include "perf_audit.h"

EXTERN_CVAR(Bool, nri_ptsectorcolor)

namespace nri_scene
{
namespace
{
bool ReadRule(const void* context, size_t index, SectorHueRuleInput& out)
{
	const auto& rules = static_cast<const ResolvedLightOverlaySet*>(context)->sectorColorRules;
	if (index >= rules.Size()) return false;
	const auto& r = rules[(unsigned)index];
	out.sector = r.sector; out.palette = r.palette; out.paletteSource = r.paletteSource;
	out.mapScoped = r.mapName.IsNotEmpty(); out.enabled = r.enabled; out.hasColor = r.hasColor;
	out.hasStrength = r.hasStrength; out.hasGain = r.hasGain;
	for (int i = 0; i < 3; ++i) out.color[i] = r.color[i];
	out.strength = r.strength; out.gain = r.gain;
	return true;
}
bool ReadDirect(int index, SectorHueDirectInput& out)
{
	if (index < 0 || (unsigned)index >= sector.Size()) return false;
	const auto& s = sector[index];
	out.floorPalette = s.floorpal; out.ceilingPalette = s.ceilingpal;
	out.ceilingSky = (s.ceilingstat & CSTAT_SECTOR_SKY) != 0;
	return true;
}
bool WallRange(int index, size_t& first, size_t& count)
{
	if (index < 0 || (unsigned)index >= sector.Size()) return false;
	const auto& span = sector[index].walls;
	count = span.Size();
	if (count == 0) { first = 0; return true; }
	// Compare addresses before subtracting: a malformed view is not a certificate.
	const uintptr_t base = reinterpret_cast<uintptr_t>(wall.Data());
	const uintptr_t address = reinterpret_cast<uintptr_t>(span.Data());
	if (address < base || (address - base) % sizeof(walltype) != 0) return false;
	first = (address - base) / sizeof(walltype);
	return first <= wall.Size() && count <= wall.Size() - first;
}
bool ReadPlane(int index, SectorHuePlaneInput& out)
{
	if (index < 0 || (unsigned)index >= sector.Size()) return false;
	const auto& s = sector[index];
	out.ceilingZ = s.ceilingz; out.floorZ = s.floorz;
	out.ceilingSloped = (s.ceilingstat & CSTAT_SECTOR_SLOPE) != 0;
	out.floorSloped = (s.floorstat & CSTAT_SECTOR_SLOPE) != 0;
	out.ceilingSlope = out.ceilingSloped ? s.ceilingheinum : 0;
	out.floorSlope = out.floorSloped ? s.floorheinum : 0;
	if (out.ceilingSloped || out.floorSloped)
	{
		size_t first, count;
		if (!WallRange(index, first, count) || count == 0) return false;
		auto& w = wall[(unsigned)first];
		if (w.point2 < 0 || (unsigned)w.point2 >= wall.Size()) return false;
		out.origin[0] = w.pos.X; out.origin[1] = w.pos.Y;
		out.end[0] = w.point2Wall()->pos.X; out.end[1] = w.point2Wall()->pos.Y;
		// Preserve the original effective cached length, including lazy updates.
		out.length = w.Length();
	}
	return true;
}
bool ReadWall(size_t index, SectorHueWallInput& out)
{
	if (index >= wall.Size()) return false;
	const auto& w = wall[(unsigned)index];
	out.nextSector = w.nextsector;
	out.blocked = (uint32_t)(w.cstat & (CSTAT_WALL_1WAY | CSTAT_WALL_MASKED));
	if (out.nextSector < 0 || (unsigned)out.nextSector >= sector.Size() || out.blocked != 0) return true;
	if (w.point2 < 0 || (unsigned)w.point2 >= wall.Size()) return false;
	out.point2 = w.point2;
	out.start[0] = w.pos.X; out.start[1] = w.pos.Y;
	out.end[0] = w.point2Wall()->pos.X; out.end[1] = w.point2Wall()->pos.Y;
	return true;
}
}

SectorHueInputSource CaptureSectorHueInputSource()
{
	const uint64_t start = PerfAuditActive() ? PerfAuditClockNs() : 0;
	const auto& rules = GetResolvedLightOverlaySet();
	SectorHueInputSource result;
	result.context = &rules; result.sectors = sector.Size(); result.walls = wall.Size();
	result.rules = rules.sectorColorRules.Size(); result.enabled = (bool)nri_ptsectorcolor;
	result.rule = ReadRule; result.direct = ReadDirect; result.plane = ReadPlane;
	result.wallRange = WallRange; result.wall = ReadWall;
	result.openPortal = IsSectorHuePortalOpenReference;
	result.captureNs = start != 0 ? PerfAuditClockNs() - start : 0;
	return result;
}
}
