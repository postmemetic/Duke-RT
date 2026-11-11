#include "nri_sector_hue_product.h"
#include "nri_sector_emission_hue_cache.h"
#include "perf_audit.h"
#include "printf.h"
#include "c_cvars.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <vector>

// Session-only: 0 original, 1 independent shadow, 2 certified reuse.
CVAR(Int, nri_ptsectorhueproduct, 2, 0)
CVAR(Bool, nri_ptsectorhueproductvalidate, false, 0)

namespace nri_scene
{
namespace
{
thread_local ScopedSectorHueProduct* CurrentProduct = nullptr;
constexpr size_t MaxDependencies = 128;
constexpr size_t MaxSectors = 65536;
constexpr size_t MaxWalls = 1048576;
constexpr size_t MaxRules = 4096;
struct AccumulatedTimer
{
	uint64_t& total;
	uint64_t start;
	AccumulatedTimer(uint64_t& target, bool enabled) : total(target), start(enabled ? PerfAuditClockNs() : 0) {}
	~AccumulatedTimer() { if (start) total += PerfAuditClockNs() - start; }
};
template<class T> bool BitsEqual(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }
bool HueEqual(const SectorEmissionHue& a, const SectorEmissionHue& b)
{
	return BitsEqual(a.rgb, b.rgb) && BitsEqual(a.strength, b.strength) && BitsEqual(a.gain, b.gain) &&
		a.sectorIndex == b.sectorIndex && a.palette == b.palette && a.portalHops == b.portalHops;
}
bool RuleEqual(const SectorHueRuleInput& a, const SectorHueRuleInput& b)
{
	return a.sector == b.sector && a.palette == b.palette && a.paletteSource == b.paletteSource &&
		a.mapScoped == b.mapScoped && a.enabled == b.enabled && a.hasColor == b.hasColor &&
		a.hasStrength == b.hasStrength && a.hasGain == b.hasGain && BitsEqual(a.color, b.color) &&
		BitsEqual(a.strength, b.strength) && BitsEqual(a.gain, b.gain);
}
bool DirectEqual(const SectorHueDirectInput& a, const SectorHueDirectInput& b)
{ return a.floorPalette == b.floorPalette && a.ceilingPalette == b.ceilingPalette && a.ceilingSky == b.ceilingSky; }
bool PlaneEqual(const SectorHuePlaneInput& a, const SectorHuePlaneInput& b)
{
	return BitsEqual(a.ceilingZ, b.ceilingZ) && BitsEqual(a.floorZ, b.floorZ) && BitsEqual(a.origin, b.origin) &&
		BitsEqual(a.end, b.end) && BitsEqual(a.length, b.length) && a.ceilingSlope == b.ceilingSlope &&
		a.floorSlope == b.floorSlope && a.ceilingSloped == b.ceilingSloped && a.floorSloped == b.floorSloped;
}
bool WallEqual(const SectorHueWallInput& a, const SectorHueWallInput& b)
{
	return a.nextSector == b.nextSector && a.point2 == b.point2 && a.blocked == b.blocked &&
		BitsEqual(a.start, b.start) && BitsEqual(a.end, b.end);
}
}

struct SectorHueProduct::State
{
	struct DirectValue
	{
		SectorEmissionHue hue;
		bool disabled = false, hasStrength = false, hasGain = false;
		float strength = 1, gain = 1;
	};
	struct DirectRecord
	{
		SectorHueDirectInput input;
		DirectValue output;
		uint64_t epoch = 0, policy = 0, version = 0;
	};
	struct PlaneRecord { SectorHuePlaneInput input; uint64_t epoch = 0, version = 0; };
	struct EdgeRecord
	{
		SectorHueWallInput input;
		uint64_t front = 0, back = 0;
		bool open = false, valid = false;
	};
	struct AdjacencyRecord
	{
		std::vector<EdgeRecord> edges;
		size_t first = 0;
		uint64_t epoch = 0, version = 0;
	};
	struct Dependency { int sector = -1; bool adjacency = false; uint64_t version = 0; };
	struct Query
	{
		SectorEmissionHue hue;
		std::vector<Dependency> dependencies;
		bool valid = false;
	};
	std::vector<SectorHueRuleInput> rules, ruleScratch;
	std::vector<int> overrides;
	std::array<int, 256> profiles;
	std::vector<DirectRecord> direct;
	std::vector<PlaneRecord> planes;
	std::vector<AdjacencyRecord> adjacency;
	std::vector<Query> queries;
	size_t sectorCount = 0, wallCount = 0, edgeCapacity = 0, dependencyCapacity = 0;
	size_t edgeSize = 0, dependencySize = 0;
	uint64_t epoch = 0, policy = 0;
	bool enabled = false, quarantined = false;
	template<class T> static void Resize(std::vector<T>& values, size_t size, SectorHueProductStats& stats)
	{
		const size_t oldCapacity = values.capacity();
		values.resize(size);
		if (values.capacity() > oldCapacity) { ++stats.allocations; stats.allocatedBytes += values.capacity() * sizeof(T); }
	}

	bool Prepare(const SectorHueInputSource& source, SectorHueProductStats& stats)
	{
		if (source.sectors > MaxSectors || source.walls > MaxWalls || source.rules > MaxRules ||
			!source.rule || !source.direct || !source.plane || !source.wallRange || !source.wall || !source.openPortal)
		{ ++stats.capacityFallbacks; return false; }
		if (sectorCount != source.sectors || wallCount != source.walls)
		{
			*this = State{};
			sectorCount = source.sectors; wallCount = source.walls;
			Resize(direct, sectorCount, stats); Resize(planes, sectorCount, stats); Resize(adjacency, sectorCount, stats);
			Resize(queries, sectorCount * 2, stats); Resize(overrides, sectorCount, stats);
		}
		if (++epoch == 0) // Keep stamps unambiguous even after serial wrap.
		{
			for (auto& v : direct) v.epoch = 0;
			for (auto& v : planes) v.epoch = 0;
			for (auto& v : adjacency) v.epoch = 0;
			++epoch;
		}
		Resize(ruleScratch, source.rules, stats);
		bool changed = rules.size() != source.rules || enabled != source.enabled || policy == 0;
		for (size_t i = 0; i < source.rules; ++i)
		{
			if (!source.rule(source.context, i, ruleScratch[i])) return false;
			if (i >= rules.size() || !RuleEqual(rules[i], ruleScratch[i])) changed = true;
		}
		stats.policyRows += source.rules;
		if (changed)
		{
			++stats.policyChanges;
			rules.swap(ruleScratch); enabled = source.enabled;
			if (++policy == 0)
			{
				++policy;
				for (auto& v : direct) v.policy = 0;
			}
			std::fill(overrides.begin(), overrides.end(), -1); profiles.fill(-1);
			for (size_t i = 0; i < rules.size(); ++i)
			{
				const auto& r = rules[i];
				if (r.mapScoped) { if (r.sector >= 0 && (size_t)r.sector < sectorCount) overrides[r.sector] = (int)i; }
				else if (r.palette >= 0 && (size_t)r.palette < profiles.size()) profiles[r.palette] = (int)i;
			}
		}
		return true;
	}
	const SectorHueRuleInput* Profile(uint8_t pal) const
	{
		const int index = profiles[pal];
		return index >= 0 && rules[index].enabled ? &rules[index] : nullptr;
	}
	DirectValue EvaluateDirect(int index, const SectorHueDirectInput& in) const
	{
		DirectValue result;
		const auto* overrideRule = overrides[index] >= 0 ? &rules[overrides[index]] : nullptr;
		result.disabled = !enabled || (overrideRule && !overrideRule->enabled);
		if (overrideRule)
		{
			result.hasStrength = overrideRule->hasStrength; result.strength = overrideRule->strength;
			result.hasGain = overrideRule->hasGain; result.gain = overrideRule->gain;
		}
		if (result.disabled) return result;
		const int source = overrideRule ? overrideRule->paletteSource : 0;
		const auto* profile = source != 2 ? Profile(in.floorPalette) : nullptr;
		int palette = in.floorPalette;
		if (!profile && source != 1 && (source == 2 || !in.ceilingSky))
		{ profile = Profile(in.ceilingPalette); palette = in.ceilingPalette; }
		const auto* colorRule = overrideRule && overrideRule->hasColor ? overrideRule : profile;
		if (!colorRule) return result;
		const float* color = colorRule->color;
		const float luminance = color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
		if (luminance <= 0.0f) return result;
		for (int channel = 0; channel < 3; ++channel) result.hue.rgb[channel] = color[channel] / luminance;
		result.hue.sectorIndex = index; result.hue.palette = profile ? palette : -1;
		result.hue.strength = overrideRule && overrideRule->hasStrength ? overrideRule->strength : profile ? profile->strength : 1.0f;
		result.hue.gain = overrideRule && overrideRule->hasGain ? overrideRule->gain : profile ? profile->gain : 1.0f;
		return result;
	}
	bool Direct(int index, const SectorHueInputSource& source, SectorHueProductStats& stats)
	{
		auto& record = direct[index];
		if (record.epoch == epoch) return true;
		SectorHueDirectInput input{};
		++stats.directCaptures;
		if (!source.direct(index, input)) return false;
		if (!record.version || record.policy != policy || !DirectEqual(record.input, input))
		{
			++stats.directInputChanges;
			const auto result = EvaluateDirect(index, input);
			const auto& old = record.output;
			if (!record.version || !HueEqual(old.hue, result.hue) || old.disabled != result.disabled ||
				old.hasStrength != result.hasStrength || old.hasGain != result.hasGain ||
				!BitsEqual(old.strength, result.strength) || !BitsEqual(old.gain, result.gain))
			{ ++record.version; ++stats.directChanges; }
			record.input = input; record.output = result; record.policy = policy;
		}
		record.epoch = epoch;
		return true;
	}
	bool Plane(int index, const SectorHueInputSource& source, SectorHueProductStats& stats)
	{
		auto& record = planes[index];
		if (record.epoch == epoch) return true;
		SectorHuePlaneInput input{};
		++stats.planeCaptures;
		if (!source.plane(index, input)) return false;
		if (!record.version || !PlaneEqual(record.input, input)) { record.input = input; ++record.version; ++stats.planeChanges; }
		record.epoch = epoch;
		return true;
	}
	bool Adjacency(int index, const SectorHueInputSource& source, SectorHueProductStats& stats)
	{
		auto& record = adjacency[index];
		if (record.epoch == epoch) return true;
		size_t first, count;
		if (!source.wallRange(index, first, count) || first > wallCount || count > wallCount - first) return false;
		bool changed = !record.version || record.first != first || record.edges.size() != count;
		// A failed partial refresh must not leave edited edge outputs carrying the
		// old version. Restore the version only after a complete unchanged capture.
		const uint64_t previousVersion = record.version;
		++record.version;
		const size_t previousSize = record.edges.size();
		if (count > record.edges.capacity())
		{
			if (edgeCapacity - record.edges.capacity() + count > wallCount * 2)
			{ ++stats.capacityFallbacks; return false; }
			edgeCapacity -= record.edges.capacity();
			std::vector<EdgeRecord>(count).swap(record.edges);
			edgeCapacity += record.edges.capacity();
			++stats.allocations; stats.allocatedBytes += record.edges.capacity() * sizeof(EdgeRecord);
			changed = true;
		}
		else record.edges.resize(count);
		edgeSize = edgeSize - previousSize + record.edges.size();
		for (size_t i = 0; i < count; ++i)
		{
			SectorHueWallInput input{};
			++stats.wallCaptures;
			if (!source.wall(first + i, input)) return false;
			const bool eligible = input.nextSector >= 0 && (size_t)input.nextSector < sectorCount && input.blocked == 0;
			uint64_t front = 0, back = 0;
			if (eligible)
			{
				if (!Plane(index, source, stats) || !Plane(input.nextSector, source, stats)) return false;
				front = planes[index].version; back = planes[input.nextSector].version;
			}
			auto& edge = record.edges[i];
			if (!edge.valid || record.first != first || !WallEqual(edge.input, input) || edge.front != front || edge.back != back)
			{
				bool open = false;
				if (eligible) { ++stats.portalEvaluations; open = source.openPortal(index, first + i); }
				if (!edge.valid || edge.open != open || (open && edge.input.nextSector != input.nextSector)) changed = true;
				edge.input = input; edge.front = front; edge.back = back; edge.open = open; edge.valid = true;
			}
		}
		record.first = first;
		if (!changed) record.version = previousVersion;
		else ++stats.adjacencyChanges;
		record.epoch = epoch;
		return true;
	}
	bool Refresh(const Dependency& dep, const SectorHueInputSource& source, SectorHueProductStats& stats)
	{
		if (dep.sector < 0 || (size_t)dep.sector >= sectorCount) return false;
		return dep.adjacency ? Adjacency(dep.sector, source, stats) : Direct(dep.sector, source, stats);
	}
	uint64_t Version(const Dependency& dep) const
	{ return dep.adjacency ? adjacency[dep.sector].version : direct[dep.sector].version; }
	bool Resolve(int index, bool neighbors, const SectorHueInputSource& source, SectorHueProductStats& stats,
		SectorEmissionHue& result, bool& hit, bool timing)
	{
		auto& query = queries[(size_t)index * 2 + (neighbors ? 1 : 0)];
		hit = query.valid;
		if (hit)
		{
			AccumulatedTimer checkTimer(stats.checkNs, timing);
			for (const auto& dep : query.dependencies)
			{
				++stats.dependencies;
				if (!Refresh(dep, source, stats)) return false;
				if (Version(dep) != dep.version)
				{
					hit = false;
					if (dep.adjacency) ++stats.adjacencyInvalidations;
					else ++stats.directInvalidations;
				}
			}
			if (hit) { result = query.hue; return true; }
		}
		++stats.builds;
		query.valid = false;
		AccumulatedTimer buildTimer(stats.buildNs, timing);
		std::array<Dependency, MaxDependencies> deps;
		size_t depCount = 0;
		const auto require = [&](int sectorIndex, bool adjacent)
		{
			for (size_t i = 0; i < depCount; ++i)
				if (deps[i].sector == sectorIndex && deps[i].adjacency == adjacent) return true;
			if (depCount == deps.size()) { ++stats.capacityFallbacks; return false; }
			Dependency dep{sectorIndex, adjacent, 0};
			if (!Refresh(dep, source, stats)) return false;
			dep.version = Version(dep); deps[depCount++] = dep;
			return true;
		};
		if (!require(index, false)) return false;
		const auto root = direct[index].output;
		result = root.hue;
		if (!root.disabled && !result.Active() && neighbors)
		{
			std::array<int, MaxDependencies> visited{}, frontier{}, next{};
			size_t visitedCount = 1, frontierCount = 1;
			visited[0] = frontier[0] = index;
			for (int hop = 1; hop <= 2 && !result.Active(); ++hop)
			{
				size_t nextCount = 0;
				for (size_t f = 0; f < frontierCount; ++f)
				{
					const int current = frontier[f];
					if (!require(current, true)) return false;
					for (const auto& edge : adjacency[current].edges)
					{
						if (!edge.open) continue;
						const int target = edge.input.nextSector;
						if (std::find(visited.begin(), visited.begin() + visitedCount, target) != visited.begin() + visitedCount) continue;
						if (visitedCount == visited.size()) { ++stats.capacityFallbacks; return false; }
						visited[visitedCount++] = target;
						if (!require(target, false)) return false;
						if (!direct[target].output.disabled) next[nextCount++] = target;
					}
				}
				std::sort(next.begin(), next.begin() + nextCount);
				for (size_t n = 0; n < nextCount; ++n)
				{
					result = direct[next[n]].output.hue;
					if (result.Active())
					{
						result.portalHops = hop;
						if (root.hasStrength) result.strength = root.strength;
						if (root.hasGain) result.gain = root.gain;
						break;
					}
				}
				frontier = next; frontierCount = nextCount;
			}
		}
		if (!result.Active()) result = {};
		if (depCount > query.dependencies.capacity())
		{
			if (dependencyCapacity - query.dependencies.capacity() + depCount > std::max(MaxDependencies, sectorCount * 16))
			{ ++stats.capacityFallbacks; return false; }
			dependencyCapacity -= query.dependencies.capacity();
			dependencySize -= query.dependencies.size();
			std::vector<Dependency>(deps.begin(), deps.begin() + depCount).swap(query.dependencies);
			dependencyCapacity += query.dependencies.capacity();
			dependencySize += query.dependencies.size();
			++stats.allocations; stats.allocatedBytes += query.dependencies.capacity() * sizeof(Dependency);
		}
		else
		{
			dependencySize -= query.dependencies.size();
			query.dependencies.assign(deps.begin(), deps.begin() + depCount);
			dependencySize += query.dependencies.size();
		}
		query.hue = result; query.valid = true;
		return true;
	}
};

SectorHueProduct::SectorHueProduct() = default;
SectorHueProduct::~SectorHueProduct() = default;
void SectorHueProduct::Reset() { mState.reset(); if (++mLifetime == 0) ++mLifetime; }
uint64_t SectorHueProduct::CapacityBytes() const
{
	if (!mState) return 0;
	const auto& s = *mState;
	return sizeof(State) + (s.rules.capacity() + s.ruleScratch.capacity()) * sizeof(SectorHueRuleInput) +
		s.overrides.capacity() * sizeof(int) + s.direct.capacity() * sizeof(State::DirectRecord) +
		s.planes.capacity() * sizeof(State::PlaneRecord) + s.adjacency.capacity() * sizeof(State::AdjacencyRecord) +
		s.queries.capacity() * sizeof(State::Query) + s.edgeCapacity * sizeof(State::EdgeRecord) +
		s.dependencyCapacity * sizeof(State::Dependency);
}

ScopedSectorHueProduct::ScopedSectorHueProduct(SectorHueProduct& owner, int mode, bool validate)
	: ScopedSectorHueProduct(owner, mode == 1 || mode == 2 ? CaptureSectorHueInputSource() : SectorHueInputSource{}, mode, validate) {}

ScopedSectorHueProduct::ScopedSectorHueProduct(SectorHueProduct& owner, const SectorHueInputSource& source, int mode, bool validate)
	: mOwner(owner), mSource(source), mPrevious(CurrentProduct), mLifetime(owner.mLifetime),
	mMode(mode == 1 || mode == 2 ? mode : 0), mValidate(validate), mTiming(mMode != 0 && PerfAuditActive())
{
	if (mTiming) mStartNs = PerfAuditClockNs();
	if (mPrevious)
	{
		mPrevious->mInterrupted = true; mInterrupted = true;
		++mStats.nestedFallbacks;
	}
	CurrentProduct = this;
	if (mMode == 0 || mInterrupted) return;
	const uint64_t start = mTiming ? PerfAuditClockNs() : 0;
	if (!mOwner.mState)
	{
		mOwner.mState.reset(new SectorHueProduct::State);
		++mStats.allocations; mStats.allocatedBytes += sizeof(SectorHueProduct::State);
	}
	if (mOwner.mState->quarantined || !mOwner.mState->Prepare(source, mStats)) mInterrupted = true;
	else mEpoch = mOwner.mState->epoch;
	if (mTiming) mStats.prepareNs = PerfAuditClockNs() - start;
}

ScopedSectorHueProduct::~ScopedSectorHueProduct()
{
	assert(CurrentProduct == this);
	CurrentProduct = mPrevious;
	if (mMode == 0 || !PerfAuditActive()) return;
#define PRODUCT_AUDIT(name, field) PerfAuditAdd(PerfAuditMetric::SectorHueProduct##name, mStats.field)
	PRODUCT_AUDIT(Requests, requests); PRODUCT_AUDIT(Hits, hits); PRODUCT_AUDIT(Builds, builds);
	PRODUCT_AUDIT(DirectResults, directResults); PRODUCT_AUDIT(BorrowedResults, borrowedResults); PRODUCT_AUDIT(NeutralResults, neutralResults);
	PRODUCT_AUDIT(PolicyRows, policyRows); PRODUCT_AUDIT(PolicyChanges, policyChanges);
	PRODUCT_AUDIT(DirectCaptures, directCaptures); PRODUCT_AUDIT(PlaneCaptures, planeCaptures); PRODUCT_AUDIT(WallCaptures, wallCaptures);
	PRODUCT_AUDIT(PortalEvaluations, portalEvaluations); PRODUCT_AUDIT(Dependencies, dependencies);
	PRODUCT_AUDIT(Fallbacks, fallbacks); PRODUCT_AUDIT(CapacityFallbacks, capacityFallbacks); PRODUCT_AUDIT(NestedFallbacks, nestedFallbacks);
	PRODUCT_AUDIT(ValidationChecks, validationChecks); PRODUCT_AUDIT(HitValidationChecks, hitValidationChecks);
	PRODUCT_AUDIT(NeutralValidationChecks, neutralValidationChecks); PRODUCT_AUDIT(ValidationMismatches, validationMismatches);
	PRODUCT_AUDIT(Quarantines, quarantines); PRODUCT_AUDIT(ResolveNs, resolveNs); PRODUCT_AUDIT(PrepareNs, prepareNs);
	PRODUCT_AUDIT(ReferenceNs, referenceNs);
	PRODUCT_AUDIT(RawCalls, rawCalls); PRODUCT_AUDIT(BypassRawCalls, bypassRawCalls); PRODUCT_AUDIT(BypassRawNs, bypassRawNs);
	PRODUCT_AUDIT(AvoidedRawResolutions, avoidedRawResolutions);
	PRODUCT_AUDIT(DirectInputChanges, directInputChanges); PRODUCT_AUDIT(DirectChanges, directChanges);
	PRODUCT_AUDIT(PlaneChanges, planeChanges); PRODUCT_AUDIT(AdjacencyChanges, adjacencyChanges);
	PRODUCT_AUDIT(DirectInvalidations, directInvalidations); PRODUCT_AUDIT(AdjacencyInvalidations, adjacencyInvalidations);
	PRODUCT_AUDIT(Allocations, allocations); PRODUCT_AUDIT(AllocatedBytes, allocatedBytes);
	PRODUCT_AUDIT(CheckNs, checkNs); PRODUCT_AUDIT(BuildNs, buildNs);
#undef PRODUCT_AUDIT
	PerfAuditAdd(PerfAuditMetric::SectorHueProductCompletedOperations, 1);
	PerfAuditAdd(PerfAuditMetric::SectorHueProductSourceCaptureNs, mSource.captureNs);
	PerfAuditAdd(PerfAuditMetric::SectorHueProductOwnerNs, mSource.captureNs + mStats.prepareNs + mStats.resolveNs);
	PerfAuditAdd(PerfAuditMetric::SectorHueProductScopeNs, mTiming ? PerfAuditClockNs() - mStartNs : 0);
	PerfAuditSet(PerfAuditMetric::SectorHueProductCapacityBytes, mOwner.CapacityBytes());
	const auto* state = mOwner.mState.get();
	const uint64_t scratchBytes = state ? state->ruleScratch.capacity() * sizeof(SectorHueRuleInput) : 0;
	const uint64_t dependencyBytes = state ? state->dependencyCapacity * sizeof(SectorHueProduct::State::Dependency) : 0;
	const uint64_t edgeBytes = state ? state->edgeCapacity * sizeof(SectorHueProduct::State::EdgeRecord) : 0;
	const uint64_t liveBytes = state ? sizeof(SectorHueProduct::State) +
		(state->rules.size() + state->ruleScratch.size()) * sizeof(SectorHueRuleInput) + state->overrides.size() * sizeof(int) +
		state->direct.size() * sizeof(SectorHueProduct::State::DirectRecord) + state->planes.size() * sizeof(SectorHueProduct::State::PlaneRecord) +
		state->adjacency.size() * sizeof(SectorHueProduct::State::AdjacencyRecord) + state->queries.size() * sizeof(SectorHueProduct::State::Query) +
		state->dependencySize * sizeof(SectorHueProduct::State::Dependency) + state->edgeSize * sizeof(SectorHueProduct::State::EdgeRecord) : 0;
	PerfAuditSet(PerfAuditMetric::SectorHueProductScratchCapacityBytes, scratchBytes);
	PerfAuditSet(PerfAuditMetric::SectorHueProductDependencyCapacityBytes, dependencyBytes);
	PerfAuditSet(PerfAuditMetric::SectorHueProductEdgeCapacityBytes, edgeBytes);
	PerfAuditSet(PerfAuditMetric::SectorHueProductLiveBytes, liveBytes);
}

SectorEmissionHue ScopedSectorHueProduct::Resolve(int index, bool neighbors, SectorEmissionHue (*raw)(int, bool))
{
	if (mMode == 0) return raw(index, neighbors);
	const uint64_t start = mTiming ? PerfAuditClockNs() : 0;
	++mStats.requests;
	const auto finish = [&](SectorEmissionHue value)
	{ if (mTiming) mStats.resolveNs += PerfAuditClockNs() - start; return value; };
	const auto original = [&]() { ++mStats.rawCalls; return raw(index, neighbors); };
	if (mInterrupted || mLifetime != mOwner.mLifetime || !mOwner.mState || mOwner.mState->quarantined ||
		mOwner.mState->epoch != mEpoch || index < 0 || (size_t)index >= mSource.sectors)
	{ ++mStats.fallbacks; return finish(original()); }
	SectorEmissionHue candidate;
	bool hit = false;
	if (!mOwner.mState->Resolve(index, neighbors, mSource, mStats, candidate, hit, mTiming))
	{ ++mStats.fallbacks; return finish(original()); }
	if (hit) ++mStats.hits;
	if (!candidate.Active()) ++mStats.neutralResults;
	else if (candidate.portalHops) ++mStats.borrowedResults;
	else ++mStats.directResults;
	if (mMode == 1 || mValidate)
	{
		const uint64_t referenceStart = mTiming ? PerfAuditClockNs() : 0;
		const auto reference = original();
		if (mTiming) mStats.referenceNs += PerfAuditClockNs() - referenceStart;
		++mStats.validationChecks;
		if (hit) ++mStats.hitValidationChecks;
		if (!candidate.Active()) ++mStats.neutralValidationChecks;
		if (mInterrupted || mLifetime != mOwner.mLifetime || !mOwner.mState || mOwner.mState->epoch != mEpoch)
			return finish(reference);
		if (!HueEqual(candidate, reference))
		{
			++mStats.validationMismatches; ++mStats.quarantines;
			mOwner.mState->quarantined = true;
			Printf("NRI PT sector hue product validation: mismatch=1 action=level-quarantine sector=%d neighbors=%u\n", index, neighbors ? 1u : 0u);
			return finish(reference);
		}
		if (mMode == 1) return finish(reference);
	}
	else ++mStats.avoidedRawResolutions;
	return finish(candidate);
}

SectorEmissionHue ResolveSectorHueProduct(int sector, bool neighbors, SectorEmissionHue (*raw)(int, bool))
{
	if (!CurrentProduct) return raw(sector, neighbors);
	if (!IsSectorEmissionHueCacheBypassed()) return CurrentProduct->Resolve(sector, neighbors, raw);
	if (CurrentProduct->mMode == 0) return raw(sector, neighbors);
	++CurrentProduct->mStats.rawCalls;
	++CurrentProduct->mStats.bypassRawCalls;
	AccumulatedTimer timer(CurrentProduct->mStats.bypassRawNs, CurrentProduct->mTiming);
	return raw(sector, neighbors);
}
}
