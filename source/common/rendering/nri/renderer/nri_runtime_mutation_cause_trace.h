#pragma once

#include "../scene/nri_map_builder.h"
#include "hw_sections.h"
#include "printf.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

// Diagnostic raw reads only. This owner cannot enqueue work, clear dirty bits,
// resolve a texture, build geometry, or publish a replacement baseline.
namespace nri_mutation_cause
{
inline bool IsFocusedChunk(uint32_t chunk, int32_t focusChunk = -1, const char* focusList = "")
{
	if (focusChunk == -2) return true;
	if (focusChunk >= 0) return chunk == (uint32_t)focusChunk;
	if (focusChunk != -1 || focusList == nullptr) return false;
	// Diagnostic configuration is strict unsigned decimal CSV. An invalid list
	// selects nothing, including when a matching item preceded the bad token.
	const char* cursor = focusList;
	const auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	bool found = false;
	for (;;)
	{
		while (whitespace(*cursor)) ++cursor;
		if (*cursor < '0' || *cursor > '9') return false;
		uint32_t value = 0;
		do
		{
			const uint32_t digit = (uint32_t)(*cursor - '0');
			if (value > (UINT32_MAX - digit) / 10) return false;
			value = value * 10 + digit;
			++cursor;
		} while (*cursor >= '0' && *cursor <= '9');
		found |= value == chunk;
		while (whitespace(*cursor)) ++cursor;
		if (*cursor == 0) return found;
		if (*cursor++ != ',') return false;
	}
}

class CauseTrace
{
public:
	uint64_t Epoch() const { return mEpoch; }
	struct Context
	{
		uint32_t sourceMask = 0;
		bool selected = false, visible = false, active = false;
		bool replacementValid = false, residentAuthoritative = false, watched = false;
		uint32_t visibleValidationRemaining = 0;
		uint64_t registryRevision = 0;
	};

	void Reset()
	{
		mEntries.clear();
		mEnabled = false;
		mBuild = mFrame = mTick = 0;
		mChunkCount = mRows = mFrameRows = mSuppressed = 0;
		// Epoch stays monotonic across map/reset boundaries for text joins.
	}

	void BeginFrame(uint64_t buildSerial, uint64_t frameIndex, uint64_t simulationTick,
		uint32_t chunkCount, bool enabled, int32_t focusChunk = -1, const char* focusList = "")
	{
		if (!enabled) { Reset(); return; }
		if (focusList == nullptr) focusList = "";
		if (!mEnabled || mBuild != buildSerial || mChunkCount != chunkCount ||
			frameIndex < mFrame || focusChunk != mFocus || mFocusList != focusList)
		{
			mEntries.clear();
			mRows = 0;
			++mEpoch;
			mFocusList = focusList;
		}
		mEnabled = true; mBuild = buildSerial; mFrame = frameIndex; mTick = simulationTick;
		mChunkCount = chunkCount; mFocus = focusChunk;
		mFrameRows = mSuppressed = 0;
		for (auto& entry : mEntries) entry.suppressed = 0;
	}

	void EndFrame()
	{
		if (mEnabled && mSuppressed != 0)
		{
			Printf("PERF pt mutation cause suppressed NRI: epoch=%llu build=%llu frame=%llu renderer_frame=%llu tick=%llu rows=%u total_rows=%u suppressed=%u recovery=0\n",
				(unsigned long long)mEpoch, (unsigned long long)mBuild, (unsigned long long)mFrame,
				(unsigned long long)(mFrame + 1), (unsigned long long)mTick, mFrameRows, mRows, mSuppressed);
			for (const auto& entry : mEntries)
				if (entry.suppressed != 0)
					Printf("PERF pt mutation cause chunk suppressed NRI: epoch=%llu build=%llu frame=%llu renderer_frame=%llu tick=%llu chunk=%u chunk_rows=%u suppressed=%u recovery=0\n",
						(unsigned long long)mEpoch, (unsigned long long)mBuild, (unsigned long long)mFrame,
						(unsigned long long)(mFrame + 1), (unsigned long long)mTick, entry.chunk, entry.rows, entry.suppressed);
		}
	}

	void Observe(const nri_scene::PTMapChunk& chunk,
		const nri_scene::PTMapChunkMutationBaseline& baseline,
		const nri_scene::PTMapChunkMutationAnalysis& analysis, const Context& context)
	{
		if (!mEnabled || !IsFocusedChunk(chunk.chunkIndex, mFocus, mFocusList.c_str()) ||
			chunk.kind != nri_scene::PTMapChunkKind::Sector || chunk.sectorIndex < 0 ||
			(unsigned)chunk.sectorIndex >= sector.Size()) return;
		std::vector<Field> current;
		CaptureCurrent(chunk.sectorIndex, current);
		std::vector<Field> published;
		CaptureBaseline(baseline, published);
		auto found = std::find_if(mEntries.begin(), mEntries.end(), [&](const Entry& entry)
			{ return entry.chunk == chunk.chunkIndex; });
		if (found == mEntries.end())
		{
			mEntries.push_back({});
			found = mEntries.end() - 1;
			found->chunk = chunk.chunkIndex;
		}
		auto& entry = *found;
		const bool hadPrevious = entry.valid;
		const bool changed = !hadPrevious || !Same(current, entry.fields) ||
			baseline.signature != entry.baselineSignature || analysis.signature != entry.liveSignature ||
			analysis.reasonMask != entry.reason || !SameContext(context, entry.context);
		if (changed)
		{
			uint32_t baselineDiffs = 0, previousDiffs = 0;
			VisitFields(published, current, [&](const Field* a, const Field* b)
			{
				if ((a != nullptr || (b != nullptr && b->id < 100)) && (!a || !b || a->bits != b->bits)) ++baselineDiffs;
			});
			if (hadPrevious) VisitFields(entry.fields, current, [&](const Field* a, const Field* b)
			{
				if (!a || !b || a->bits != b->bits) ++previousDiffs;
			});
			if (AllowRow(entry)) Printf("PERF pt mutation cause NRI: epoch=%llu build=%llu frame=%llu renderer_frame=%llu tick=%llu chunk=%u sector=%d previous_frame=%llu previous_valid=%u baseline_signature=%llu live_signature=%llu baseline_fluid_signature=%llu fluid_current_observed=0 reason=0x%x source_mask=0x%x selected=%u visible=%u active=%u replacement_valid=%u resident_authoritative=%u watched=%u visible_validation=%u registry_revision=%llu sector_dirty=%u section_dirty=%u dragged=%u baseline_diffs=%u previous_diffs=%u recovery=0\n",
				(unsigned long long)mEpoch, (unsigned long long)mBuild, (unsigned long long)mFrame,
				(unsigned long long)(mFrame + 1), (unsigned long long)mTick, chunk.chunkIndex, chunk.sectorIndex,
				(unsigned long long)entry.frame, hadPrevious, (unsigned long long)baseline.signature,
				(unsigned long long)analysis.signature, (unsigned long long)baseline.fluidNormalSignature,
				analysis.reasonMask, context.sourceMask, context.selected, context.visible, context.active,
				context.replacementValid, context.residentAuthoritative, context.watched, context.visibleValidationRemaining,
				(unsigned long long)context.registryRevision, analysis.sectorDirty, analysis.sectionDirtyCount,
				analysis.dragged, baselineDiffs, previousDiffs);
			// The current list plus removed baseline/previous fields forms the union.
			std::vector<Field> keys = current;
			AppendMissing(keys, published);
			if (hadPrevious) AppendMissing(keys, entry.fields);
			std::sort(keys.begin(), keys.end(), Less);
			for (const auto& key : keys)
			{
				const Field* live = Find(current, key);
				const Field* base = Find(published, key);
				const Field* previous = hadPrevious ? Find(entry.fields, key) : nullptr;
				// Fields absent from the baseline format (exflags and source IDs)
				// are previous/current observations, not invented baseline values.
				const bool baselineDelta = key.id < 100 && (!base || !live || base->bits != live->bits);
				const bool previousDelta = hadPrevious && (!previous || !live || previous->bits != live->bits);
				if (!baselineDelta && !previousDelta) continue;
				if (!AllowRow(entry)) continue;
				Printf("PERF pt mutation cause field NRI: epoch=%llu build=%llu frame=%llu renderer_frame=%llu tick=%llu chunk=%u sector=%d domain=%s local=%u field=%s owner_kind=%s baseline_owner=%d previous_owner=%d current_owner=%d type=%s baseline_valid=%u previous_valid=%u current_valid=%u baseline_bits=0x%llx previous_bits=0x%llx current_bits=0x%llx baseline_value=%.17g previous_value=%.17g current_value=%.17g baseline_delta=%u previous_delta=%u recovery=0\n",
					(unsigned long long)mEpoch, (unsigned long long)mBuild, (unsigned long long)mFrame,
					(unsigned long long)(mFrame + 1), (unsigned long long)mTick, chunk.chunkIndex, chunk.sectorIndex,
					key.domain == 0 ? "sector" : key.domain == 1 ? "wall" : "section", key.local, key.name,
					key.domain == 1 && (key.id < 15 || key.id > 21) ? "wall" : "sector",
					base ? base->owner : -1, previous ? previous->owner : -1, live ? live->owner : -1,
					TypeName(key.type), base != nullptr, previous != nullptr, live != nullptr,
					(unsigned long long)(base ? base->bits : 0), (unsigned long long)(previous ? previous->bits : 0),
					(unsigned long long)(live ? live->bits : 0), Number(base), Number(previous), Number(live),
					baselineDelta, previousDelta);
			}
		}
		entry.valid = true; entry.frame = mFrame; entry.fields = std::move(current);
		entry.baselineSignature = baseline.signature; entry.liveSignature = analysis.signature;
		entry.reason = analysis.reasonMask; entry.context = context;
	}

private:
	enum class Type { Integer, Float, Double };
	struct Field
	{
		uint32_t domain = 0, local = 0, id = 0;
		const char* name = "";
		int32_t owner = -1;
		Type type = Type::Integer;
		uint64_t bits = 0;
	};
	struct Entry
	{
		uint32_t chunk = 0, reason = 0, rows = 0, suppressed = 0;
		bool valid = false;
		uint64_t frame = 0, baselineSignature = 0, liveSignature = 0;
		Context context;
		std::vector<Field> fields;
	};
	std::vector<Entry> mEntries;
	bool mEnabled = false;
	uint64_t mEpoch = 0, mBuild = 0, mFrame = 0, mTick = 0;
	uint32_t mChunkCount = 0, mRows = 0, mFrameRows = 0, mSuppressed = 0;
	int32_t mFocus = -1;
	std::string mFocusList;
	bool AllowRow(Entry& entry)
	{
		if (mFrameRows >= 128 || mRows >= 32768 || entry.rows >= 4096)
		{
			++mSuppressed; ++entry.suppressed; return false;
		}
		++mFrameRows; ++mRows; ++entry.rows; return true;
	}
	static bool Less(const Field& a, const Field& b)
	{
		return a.domain != b.domain ? a.domain < b.domain : a.local != b.local ? a.local < b.local : a.id < b.id;
	}
	static const Field* Find(const std::vector<Field>& fields, const Field& key)
	{
		const auto found = std::lower_bound(fields.begin(), fields.end(), key, Less);
		return found != fields.end() && !Less(key, *found) ? &*found : nullptr;
	}
	static void AppendMissing(std::vector<Field>& keys, const std::vector<Field>& fields)
	{
		for (const auto& field : fields)
			if (std::find_if(keys.begin(), keys.end(), [&](const Field& key)
				{ return !Less(key, field) && !Less(field, key); }) == keys.end()) keys.push_back(field);
	}
	template<class F> static void VisitFields(const std::vector<Field>& a, const std::vector<Field>& b, F visit)
	{
		size_t i = 0, j = 0;
		while (i < a.size() || j < b.size())
		{
			if (j == b.size() || (i < a.size() && Less(a[i], b[j]))) visit(&a[i++], nullptr);
			else if (i == a.size() || Less(b[j], a[i])) visit(nullptr, &b[j++]);
			else { visit(&a[i], &b[j]); ++i; ++j; }
		}
	}
	static bool Same(const std::vector<Field>& a, const std::vector<Field>& b)
	{
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (Less(a[i], b[i]) || Less(b[i], a[i]) || a[i].bits != b[i].bits) return false;
		return true;
	}
	static bool SameContext(const Context& a, const Context& b)
	{
		return a.sourceMask == b.sourceMask && a.selected == b.selected && a.visible == b.visible &&
			a.active == b.active && a.replacementValid == b.replacementValid &&
			a.residentAuthoritative == b.residentAuthoritative && a.watched == b.watched &&
			a.visibleValidationRemaining == b.visibleValidationRemaining && a.registryRevision == b.registryRevision;
	}
	static const char* TypeName(Type type) { return type == Type::Float ? "float32" : type == Type::Double ? "float64" : "integer"; }
	static double Number(const Field* field)
	{
		if (!field) return 0;
		if (field->type == Type::Double) { double value; std::memcpy(&value, &field->bits, sizeof(value)); return value; }
		if (field->type == Type::Float) { uint32_t bits = (uint32_t)field->bits; float value; std::memcpy(&value, &bits, sizeof(value)); return value; }
		return (double)(int64_t)field->bits;
	}
	struct Writer
	{
		std::vector<Field>& fields;
		uint32_t domain, local, id = 0;
		int32_t owner;
		void Add(const char* name, int64_t value) { fields.push_back({ domain, local, id++, name, owner, Type::Integer, (uint64_t)value }); }
		void Add(const char* name, float value) { uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); fields.push_back({ domain, local, id++, name, owner, Type::Float, bits }); }
		void Add(const char* name, double value) { uint64_t bits; std::memcpy(&bits, &value, sizeof(bits)); fields.push_back({ domain, local, id++, name, owner, Type::Double, bits }); }
	};
	static void CaptureBaseline(const nri_scene::PTMapChunkMutationBaseline& state, std::vector<Field>& fields)
	{
		Writer s{ fields, 0, 0, 0, state.sectorIndex };
#define NRI_CAUSE_I(name) s.Add(#name, (int64_t)state.name)
#define NRI_CAUSE_F(name) s.Add(#name, state.name)
		NRI_CAUSE_I(sectorIndex); NRI_CAUSE_F(floorz); NRI_CAUSE_F(ceilingz);
		NRI_CAUSE_I(floorstat); NRI_CAUSE_I(ceilingstat); NRI_CAUSE_I(floorheinum); NRI_CAUSE_I(ceilingheinum);
		NRI_CAUSE_I(portalflags); NRI_CAUSE_I(floortexture); NRI_CAUSE_I(ceilingtexture);
		NRI_CAUSE_F(floorxpan); NRI_CAUSE_F(floorypan); NRI_CAUSE_F(ceilingxpan); NRI_CAUSE_F(ceilingypan);
		NRI_CAUSE_I(floorpal); NRI_CAUSE_I(ceilingpal); NRI_CAUSE_I(floorshade); NRI_CAUSE_I(ceilingshade);
		s.Add("wall_count", (int64_t)state.walls.size()); s.Add("section_count", (int64_t)state.sectionIndices.size());
#undef NRI_CAUSE_I
#undef NRI_CAUSE_F
		for (uint32_t i = 0; i < state.walls.size(); ++i)
		{
			const auto& w = state.walls[i];
			Writer out{ fields, 1, i, 0, -1 };
#define NRI_CAUSE_I(name) out.Add(#name, (int64_t)w.name)
#define NRI_CAUSE_F(name) out.Add(#name, w.name)
			out.Add("pos_x", w.pos.X); out.Add("pos_y", w.pos.Y);
			NRI_CAUSE_I(point2); NRI_CAUSE_I(nextwall); NRI_CAUSE_I(nextsector); NRI_CAUSE_I(cstat); NRI_CAUSE_I(portalflags);
			NRI_CAUSE_I(walltexture); NRI_CAUSE_I(overtexture); NRI_CAUSE_F(xpan); NRI_CAUSE_F(ypan);
			NRI_CAUSE_I(xrepeat); NRI_CAUSE_I(yrepeat); NRI_CAUSE_I(pal); NRI_CAUSE_I(shade);
			NRI_CAUSE_F(adjacentFloorz); NRI_CAUSE_F(adjacentCeilingz); NRI_CAUSE_I(adjacentFloorstat); NRI_CAUSE_I(adjacentCeilingstat);
			NRI_CAUSE_I(adjacentFloorheinum); NRI_CAUSE_I(adjacentCeilingheinum); NRI_CAUSE_I(adjacentPortalflags);
			NRI_CAUSE_I(nextWallCstat); NRI_CAUSE_I(nextWallTexture); NRI_CAUSE_I(nextOverTexture); NRI_CAUSE_I(nextWallPal); NRI_CAUSE_I(nextWallShade);
#undef NRI_CAUSE_I
#undef NRI_CAUSE_F
			for (size_t offset = fields.size() - out.id; offset < fields.size(); ++offset)
			{
				auto& field = fields[offset];
				if (field.id >= 15 && field.id <= 21) field.owner = w.nextsector;
				else if (field.id >= 22) field.owner = w.nextwall;
			}
		}
		for (uint32_t i = 0; i < state.sectionIndices.size(); ++i)
		{
			Writer out{ fields, 2, i, 0, state.sectorIndex };
			out.Add("section_index", (int64_t)state.sectionIndices[i]);
		}
	}
	static void CaptureCurrent(int32_t sectorIndex, std::vector<Field>& fields)
	{
		// Mirror the baseline's raw fields without calling baseline capture:
		// that helper also resolves fluid policies and hashes lazy wall state.
		const auto& sec = sector[(unsigned)sectorIndex];
		nri_scene::PTMapChunkMutationBaseline state;
		state.sectorIndex = sectorIndex;
#define NRI_CAUSE_COPY(name) state.name = sec.name
		NRI_CAUSE_COPY(floorz); NRI_CAUSE_COPY(ceilingz); NRI_CAUSE_COPY(floorheinum); NRI_CAUSE_COPY(ceilingheinum);
		NRI_CAUSE_COPY(portalflags); NRI_CAUSE_COPY(floorpal); NRI_CAUSE_COPY(ceilingpal); NRI_CAUSE_COPY(floorshade); NRI_CAUSE_COPY(ceilingshade);
#undef NRI_CAUSE_COPY
		state.floorstat = (uint16_t)sec.floorstat; state.ceilingstat = (uint16_t)sec.ceilingstat;
		state.floortexture = sec.floortexture.GetIndex(); state.ceilingtexture = sec.ceilingtexture.GetIndex();
		state.floorxpan = sec.floorxpan_; state.floorypan = sec.floorypan_;
		state.ceilingxpan = sec.ceilingxpan_; state.ceilingypan = sec.ceilingypan_;
		if ((unsigned)sectorIndex < sectionsPerSector.Size())
			for (int index : sectionsPerSector[(unsigned)sectorIndex]) state.sectionIndices.push_back(index);
		state.walls.reserve(sec.walls.Size());
		for (const auto& wallInput : sec.walls)
		{
			nri_scene::PTMapWallMutationSnapshot w;
			w.pos = wallInput.pos; w.point2 = wallInput.point2; w.nextwall = wallInput.nextwall; w.nextsector = wallInput.nextsector;
			w.cstat = (uint16_t)wallInput.cstat; w.portalflags = wallInput.portalflags;
			w.walltexture = wallInput.walltexture.GetIndex(); w.overtexture = wallInput.overtexture.GetIndex();
			w.xpan = wallInput.xpan_; w.ypan = wallInput.ypan_; w.xrepeat = wallInput.xrepeat; w.yrepeat = wallInput.yrepeat;
			w.pal = wallInput.pal; w.shade = wallInput.shade;
			if ((unsigned)wallInput.nextsector < sector.Size())
			{
				const auto& adjacent = sector[(unsigned)wallInput.nextsector];
				w.adjacentFloorz = adjacent.floorz; w.adjacentCeilingz = adjacent.ceilingz;
				w.adjacentFloorstat = (uint16_t)adjacent.floorstat; w.adjacentCeilingstat = (uint16_t)adjacent.ceilingstat;
				w.adjacentFloorheinum = adjacent.floorheinum; w.adjacentCeilingheinum = adjacent.ceilingheinum;
				w.adjacentPortalflags = adjacent.portalflags;
			}
			if ((unsigned)wallInput.nextwall < wall.Size())
			{
				const auto& next = wall[(unsigned)wallInput.nextwall];
				w.nextWallCstat = (uint16_t)next.cstat; w.nextWallTexture = next.walltexture.GetIndex();
				w.nextOverTexture = next.overtexture.GetIndex(); w.nextWallPal = next.pal; w.nextWallShade = next.shade;
			}
			state.walls.push_back(w);
		}
		CaptureBaseline(state, fields);
		for (auto& field : fields)
			if (field.domain == 1 && field.id < 15) field.owner = wall.IndexOf(&sec.walls[field.local]);
		Writer extras{ fields, 0, 0, 100, sectorIndex };
		extras.Add("exflags", (int64_t)sec.exflags);
		std::sort(fields.begin(), fields.end(), Less);
	}
};
}
