#include "nri_map_chunk_view_inputs.h"
#include "nri_fluid_material_binding.h"
#include "nri_texture_signature.h"
#include "build.h"
#include "fluidoverlay.h"
#include "gamefuncs.h"
#include "hw_sections.h"
#include "sectorgeometry.h"
#include "texturemanager.h"
#include "perf_audit.h"
#include <cmath>
#include <cstring>

namespace nri_scene
{
namespace
{
struct Facts
{
	std::vector<uint64_t>& values;
	template<class T> void Add(T value)
	{
		static_assert(sizeof(T) <= sizeof(uint64_t), "Capture a named scalar, never struct padding.");
		uint64_t bits = 0; std::memcpy(&bits, &value, sizeof(T)); values.push_back(bits);
	}
	void Point(const DVector2& value) { Add(value.X); Add(value.Y); }
	bool Texture(FTextureID id)
	{
		Add(id.GetIndex());
		FGameTexture* texture = TexMan.GetGameTexture(id, true);
		Add(texture);
		if (texture == nullptr) return true;
		Add(texture->isValid());
		if (!texture->isValid()) return true;
		// This pointer is freshly resolved by TexMan above and already used by
		// the live producer. Preserve all wrapper/base mutability checks without
		// re-probing owned object/vtable pages for every repeated wall binding.
		if (!IsCurrentTextureManagerTexturePersistentSignatureEligible(texture)) return false;
		const double width = texture->GetDisplayWidth(), height = texture->GetDisplayHeight();
		if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) return false;
		Add(width); Add(height); Add(texture->GetDisplayLeftOffset()); Add(texture->GetDisplayTopOffset());
		Add(texture->GetID().GetIndex());
		Add(MakeMaterialRef(texture, 0, 0, 1.0f, 0).flags);
		Add(texture->GetGlowmap()); Add(texture->GetNormalmap()); Add(texture->GetMetallic()); Add(texture->GetRoughness());
		return true;
	}
	void PlaneHeights(const sectortype& sec, const DVector2& point)
	{
		float ceiling = 0, floor = 0;
		PlanesAtPoint(&sec, point.X, point.Y, &ceiling, &floor);
		Add(ceiling); Add(floor);
	}
};
}

bool CapturePlainMapChunkViewInputs(const PTMapChunk& chunk, PTMapChunkViewInputs& out)
{
	PerfAuditScope audit(PerfAuditMetric::MutationInputsNoopCertificateNs);
	PerfAuditAdd(PerfAuditMetric::MutationInputsNoopCertificateCaptures);
	out.valid = false; out.facts.clear();
	if (chunk.kind != PTMapChunkKind::Sector || chunk.sectorIndex < 0 ||
		(unsigned)chunk.sectorIndex >= sector.Size() || (unsigned)chunk.sectorIndex >= sectionsPerSector.Size()) return false;
	const auto& sec = sector[(unsigned)chunk.sectorIndex];
	if (sec.portalflags != 0 || ((sec.floorstat | sec.ceilingstat) & CSTAT_SECTOR_SKY) != 0 || sec.walls.Size() == 0) return false;
	MaterialLightingMetadata fluid;
	fluid.authoredTextureId = sec.floortexture.isValid() ? (uint32_t)sec.floortexture.GetIndex() : UINT32_MAX;
	fluid.sourceType = SurfaceSourceType::MapFloorSection; fluid.materialFlags = MaterialFlag_Flat; fluid.paletteIndex = sec.floorpal;
	if (ResolveFluidMaterialBindingIndex(GetResolvedFluidOverlaySet(), fluid) != UINT32_MAX) return false;
	if (!CaptureMapChunkMutationBaseline(chunk, out.authored)) return false;
	Facts facts{ out.facts };
	facts.Add(chunk.chunkIndex); facts.Add(GetPendingLevelGeometryBuildSerial());
	facts.Add(sector.Size()); facts.Add(sections.Size());
	// Validate every texture before SectionGeometry consumes dirty state. Unknown
	// sky/portal/canvas paths have side effects this certificate does not own.
	if (!facts.Texture(sec.floortexture) || !facts.Texture(sec.ceilingtexture)) return false;
	for (const auto& wal : sec.walls)
	{
		if (wal.portalflags != 0 || (unsigned)wal.point2 >= wall.Size()) return false;
		facts.Add(wall.IndexOf(&wal)); facts.Point(wal.point2Wall()->pos);
		facts.PlaneHeights(sec, wal.pos); facts.PlaneHeights(sec, wal.point2Wall()->pos);
		if (!facts.Texture(wal.walltexture) || !facts.Texture(wal.overtexture)) return false;
		facts.Add(wal.twoSided());
		if (wal.twoSided())
		{
			if ((unsigned)wal.nextwall >= wall.Size() || (unsigned)wal.nextsector >= sector.Size()) return false;
			const auto& adjacent = *wal.nextSector();
			if (adjacent.portalflags != 0) return false;
			facts.PlaneHeights(adjacent, wal.pos); facts.PlaneHeights(adjacent, wal.point2Wall()->pos);
			// Bottom-swapped UVs read the opposite wall's pan/cstat, not its
			// geometry snapshot fields alone. Capture even when the band is absent.
			const auto& opposite = *wal.nextWall();
			facts.Add(opposite.xpan_); facts.Add(opposite.ypan_); facts.Add((uint16_t)opposite.cstat);
			if (!facts.Texture(opposite.walltexture)) return false;
		}
	}
	// These are the producer's cached planes, not a new SceneView/geometry build.
	// Calling get preserves its required dirty consumption and regeneration.
	for (int sectionIndex : sectionsPerSector[(unsigned)chunk.sectorIndex])
	{
		if ((unsigned)sectionIndex >= sections.Size() || sections[sectionIndex].sector != chunk.sectorIndex) return false;
		facts.Add(sectionIndex);
		for (int plane = 0; plane < 2; ++plane)
		{
			const FTextureID textureId = plane == 0 ? sec.floortexture : sec.ceilingtexture;
			auto* texture = TexMan.GetGameTexture(textureId, true);
			if (texture == nullptr || !texture->isValid()) return false;
			TArray<int>* indices = nullptr;
			auto* mesh = sectionGeometry.get(&sections[sectionIndex], plane, { 0.0f, 0.0f }, &indices);
			if (mesh == nullptr || indices == nullptr || indices->Size() < 3 || mesh->vertices.Size() != mesh->texcoords.Size()) return false;
			facts.Add(mesh->vertices.Size()); facts.Add(indices->Size());
			for (unsigned i = 0; i < mesh->vertices.Size(); ++i)
			{
				facts.Add(mesh->vertices[i].X); facts.Add(mesh->vertices[i].Y); facts.Add(mesh->vertices[i].Z);
				facts.Add(mesh->texcoords[i].X); facts.Add(mesh->texcoords[i].Y);
			}
			for (int index : *indices) facts.Add(index);
		}
	}
	PerfAuditAdd(PerfAuditMetric::MutationInputsNoopCertificateWords, out.facts.size());
	out.valid = true;
	return true;
}
bool SamePlainMapChunkViewInputs(const PTMapChunkViewInputs& lhs, const PTMapChunkViewInputs& rhs)
{
	return lhs.valid && rhs.valid && lhs.facts == rhs.facts && SamePTMapMutationBaseline(lhs.authored, rhs.authored);
}
}
