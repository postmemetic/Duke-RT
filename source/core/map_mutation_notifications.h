#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

extern int PlayClock;

// A bounded list of sources written since the PT consumer last drained it.
// This is notification, not source truth, dirty-bit ownership, or publication.
enum class MapMutationDomain : uint8_t { Sector, Wall };
enum class MapMutationField : uint8_t
{
	FloorZ, CeilingZ, FloorSlope, CeilingSlope, FloorStat, CeilingStat,
	FloorTexture, CeilingTexture, FloorPanX, FloorPanY, CeilingPanX, CeilingPanY,
	FloorShade, CeilingShade, FloorPalette, CeilingPalette,
	WallPosX, WallPosY, WallTexture, WallOverTexture, WallPanX, WallPanY,
	WallShade, WallPalette, WallCstat,
};
enum class MapMutationWriter : uint8_t
{
	Unknown, SectorSetFloorZ, SectorSetCeilingZ, SectorAddFloorZ, SectorAddCeilingZ,
	SectorFloorSlope, SectorCeilingSlope, SectorPan, WallPan, WallTexture,
	WallOverTexture, WallMove, InterpolationApply, InterpolationRestore,
	DukeCycler, DukeSE04, SectorTexture, DukeAnimateVertex, DukeSE03, DukeSE08, DukeGlass,
};
enum class MapMutationValueType : uint8_t { Integer, Float, Double };

inline const char* MapMutationFieldName(MapMutationField field)
{
	static constexpr const char* names[] = {
		"floorz", "ceilingz", "floorslope", "ceilingslope", "floorstat", "ceilingstat",
		"floortexture", "ceilingtexture", "floorpanx", "floorpany", "ceilingpanx", "ceilingpany",
		"floorshade", "ceilingshade", "floorpalette", "ceilingpalette", "wallposx", "wallposy",
		"walltexture", "wallovertexture", "wallpanx", "wallpany", "wallshade", "wallpalette", "wallcstat"
	};
	return (size_t)field < sizeof(names) / sizeof(*names) ? names[(size_t)field] : "unknown";
}
inline const char* MapMutationWriterName(MapMutationWriter writer)
{
	static constexpr const char* names[] = {
		"unknown", "sector-set-floorz", "sector-set-ceilingz", "sector-add-floorz", "sector-add-ceilingz",
		"sector-floor-slope", "sector-ceiling-slope", "sector-pan", "wall-pan", "wall-texture",
		"wall-overtexture", "wall-move", "interpolation-apply", "interpolation-restore",
		"duke-cycler", "duke-se04", "sector-texture", "duke-animate-vertex", "duke-se03", "duke-se08", "duke-glass"
	};
	return (size_t)writer < sizeof(names) / sizeof(*names) ? names[(size_t)writer] : "unknown";
}

struct MapMutationSourceEvent
{
	MapMutationDomain domain = MapMutationDomain::Sector;
	uint32_t index = 0;
	uint64_t worldEpoch = 0, fieldMask = 0, writerMask = 0;
	uint64_t firstSerial = 0, lastSerial = 0, writeCount = 0;
};
struct MapMutationSourceChange
{
	MapMutationDomain domain = MapMutationDomain::Sector;
	MapMutationField field = MapMutationField::FloorZ;
	MapMutationWriter writer = MapMutationWriter::Unknown;
	MapMutationValueType type = MapMutationValueType::Integer;
	uint32_t index = 0;
	int32_t clock = 0;
	uint64_t worldEpoch = 0, serial = 0, before = 0, after = 0;
};
struct MapMutationConsumeResult
{
	uint64_t worldEpoch = 0, latestSerial = 0, suppressedChanges = 0, rejectedWrites = 0;
};

class MapMutationNotifications
{
public:
	void Reset(uint32_t sectorCount, uint32_t wallCount)
	{
		++mEpoch;
		mPositions[0].assign(sectorCount, UINT32_MAX);
		mPositions[1].assign(wallCount, UINT32_MAX);
		mPending.clear(); mPending.reserve((size_t)sectorCount + wallCount);
		mChanges.clear(); mSuppressed = mRejected = 0;
		ClearTraceSources();
	}
	uint64_t WorldEpoch() const { return mEpoch; }
	uint64_t LatestSerial() const { return mSerial; }
	void SetTraceEnabled(bool enabled)
	{
		mTrace = enabled;
		if (!enabled) { mChanges.clear(); mSuppressed = 0; ClearTraceSources(); }
	}
	// Configure after draining the previous frame. Filtering only applies to
	// future diagnostic detail rows; source events and serials are unaffected.
	void SetTraceSources(const std::vector<uint32_t>& sectors, const std::vector<uint32_t>& walls)
	{
		if (!mTrace) return;
		mFilteredTrace = true;
		const std::vector<uint32_t>* sources[] = { &sectors, &walls };
		for (size_t domain = 0; domain < 2; ++domain)
		{
			auto& selected = mTraceSources[domain];
			selected = *sources[domain];
			selected.erase(std::remove_if(selected.begin(), selected.end(), [&](uint32_t index)
				{ return index >= mPositions[domain].size(); }), selected.end());
			std::sort(selected.begin(), selected.end());
			selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
		}
	}
	MapMutationWriter ExchangeWriter(MapMutationWriter writer)
	{
		const auto previous = mWriter; mWriter = writer; return previous;
	}
	void Record(MapMutationDomain domain, uint32_t index, MapMutationField field,
		MapMutationWriter writer, int64_t before, int64_t after)
	{
		RecordBits(domain, index, field, writer, MapMutationValueType::Integer, (uint64_t)before, (uint64_t)after);
	}
	void Record(MapMutationDomain domain, uint32_t index, MapMutationField field,
		MapMutationWriter writer, float before, float after)
	{
		uint32_t a, b; std::memcpy(&a, &before, sizeof(a)); std::memcpy(&b, &after, sizeof(b));
		RecordBits(domain, index, field, writer, MapMutationValueType::Float, a, b);
	}
	void Record(MapMutationDomain domain, uint32_t index, MapMutationField field,
		MapMutationWriter writer, double before, double after)
	{
		uint64_t a, b; std::memcpy(&a, &before, sizeof(a)); std::memcpy(&b, &after, sizeof(b));
		RecordBits(domain, index, field, writer, MapMutationValueType::Double, a, b);
	}
	MapMutationConsumeResult Consume(std::vector<MapMutationSourceEvent>& out,
		std::vector<MapMutationSourceChange>* changes = nullptr)
	{
		MapMutationConsumeResult result{ mEpoch, mSerial, mSuppressed, mRejected };
		for (const auto& entry : mPending) mPositions[(size_t)entry.domain][entry.index] = UINT32_MAX;
		out.clear(); out.swap(mPending);
		if (changes) { changes->clear(); changes->swap(mChanges); }
		else mChanges.clear();
		mSuppressed = mRejected = 0;
		return result;
	}

private:
	std::vector<uint32_t> mPositions[2];
	std::vector<MapMutationSourceEvent> mPending;
	std::vector<MapMutationSourceChange> mChanges;
	std::vector<uint32_t> mTraceSources[2];
	uint64_t mEpoch = 0, mSerial = 0, mSuppressed = 0, mRejected = 0;
	bool mTrace = false, mFilteredTrace = false;
	MapMutationWriter mWriter = MapMutationWriter::Unknown;
	void ClearTraceSources()
	{
		mFilteredTrace = false;
		mTraceSources[0].clear(); mTraceSources[1].clear();
	}
	void RecordBits(MapMutationDomain domain, uint32_t index, MapMutationField field,
		MapMutationWriter writer, MapMutationValueType type, uint64_t before, uint64_t after)
	{
		if (before == after) return;
		if (mWriter != MapMutationWriter::Unknown) writer = mWriter;
		if ((size_t)domain >= 2 || index >= mPositions[(size_t)domain].size() ||
			(uint32_t)field >= 64 || (uint32_t)writer >= 64)
		{
			++mRejected; return;
		}
		const uint64_t serial = ++mSerial;
		auto& position = mPositions[(size_t)domain][index];
		if (position == UINT32_MAX)
		{
			position = (uint32_t)mPending.size();
			mPending.push_back({ domain, index, mEpoch, 0, 0, serial, serial, 0 });
		}
		auto& entry = mPending[position];
		entry.fieldMask |= uint64_t(1) << (uint32_t)field;
		entry.writerMask |= uint64_t(1) << (uint32_t)writer;
		entry.lastSerial = serial; ++entry.writeCount;
		if (mTrace && (!mFilteredTrace || std::binary_search(mTraceSources[(size_t)domain].begin(),
			mTraceSources[(size_t)domain].end(), index)))
		{
			if (mChanges.size() < 512)
				mChanges.push_back({ domain, field, writer, type, index, PlayClock, mEpoch, serial, before, after });
			else ++mSuppressed;
		}
	}
};

inline MapMutationNotifications& GetMapMutationNotifications()
{
	static MapMutationNotifications notifications;
	return notifications;
}

// The engine applies and restores interpolation synchronously on the same
// thread as these writers. Scope affects diagnostic attribution only.
class MapMutationWriterScope
{
	MapMutationWriter mPrevious;
public:
	explicit MapMutationWriterScope(MapMutationWriter writer)
		: mPrevious(GetMapMutationNotifications().ExchangeWriter(writer)) {}
	~MapMutationWriterScope() { GetMapMutationNotifications().ExchangeWriter(mPrevious); }
	MapMutationWriterScope(const MapMutationWriterScope&) = delete;
	MapMutationWriterScope& operator=(const MapMutationWriterScope&) = delete;
};

// Map loading also uses setters on temporary wall/sector arrays. Only actual
// global-array members are published; avoid pointer subtraction across arrays.
template<class T> inline uint32_t MapMutationSourceIndex(const T* source, const T* base, size_t count)
{
	const uintptr_t address = (uintptr_t)source, first = (uintptr_t)base;
	if (base == nullptr || address < first) return UINT32_MAX;
	const uintptr_t delta = address - first;
	return delta % sizeof(T) == 0 && delta / sizeof(T) < count ? (uint32_t)(delta / sizeof(T)) : UINT32_MAX;
}
