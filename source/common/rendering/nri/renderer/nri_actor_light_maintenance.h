#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Current-call values only. Actor class objects are content-owned; this contains
// no live actor pointer and does not rely on simulation snapshot freshness.
struct NRIActorLightInput
{
	int32_t actorIndex = -1;
	const void* actorClass = nullptr;
	int32_t palette = 0;
	uint32_t textureId = 0;
	float position[3] = {};
};

struct NRIActorLightRuleState
{
	struct Identity
	{
		int32_t actorIndex;
		const void* actorClass;
	};
	std::vector<NRIActorLightInput> inputs;
	std::vector<Identity> previous;
	uint64_t worldEpoch = 0;
	bool valid = false;
	uint64_t CapacityBytes() const
	{
		return inputs.capacity() * sizeof(NRIActorLightInput) + previous.capacity() * sizeof(Identity);
	}
};

struct NRIActorLightRuleUpdateStats
{
	bool reused = false;
	uint64_t refreshedRows = 0;
	uint64_t copiedRows = 0;
};

// Fresh-map insertion/erase/rehash ordering is observable in equal-distance
// light caps. Compare ALL eligible actors, including those matching no rule.
// This algorithm is shared with standalone reference tests.
template<class Templates, class RuleMap, class Matches, class Refresh>
NRIActorLightRuleUpdateStats UpdateNRIActorLightRules(
	NRIActorLightRuleState& state, uint64_t worldEpoch, bool forceRebuild,
	const Templates& templates, RuleMap& rules, Matches matches, Refresh refresh)
{
	NRIActorLightRuleUpdateStats stats;
	stats.reused = !forceRebuild && state.valid && state.worldEpoch == worldEpoch &&
		state.previous.size() == state.inputs.size();
	for (size_t i = 0; stats.reused && i < state.inputs.size(); ++i)
	{
		stats.reused = state.previous[i].actorIndex == state.inputs[i].actorIndex &&
			state.previous[i].actorClass == state.inputs[i].actorClass;
	}
	if (stats.reused)
	{
		for (const auto& actor : state.inputs)
		{
			const auto found = rules.find(actor.actorIndex);
			if (found == rules.end()) continue;
			for (auto& rule : found->second)
			{
				refresh(actor, rule);
				++stats.refreshedRows;
			}
		}
		return stats;
	}

	auto previousRules = std::move(rules);
	rules = RuleMap{};
	for (const auto& actor : state.inputs)
	{
		auto& actorRules = rules[actor.actorIndex];
		const auto previous = previousRules.find(actor.actorIndex);
		if (previous != previousRules.end())
		{
			actorRules.swap(previous->second);
			actorRules.clear();
		}
		for (const auto& actorTemplate : templates)
		{
			if (!matches(actor, actorTemplate)) continue;
			auto rule = actorTemplate.rule;
			refresh(actor, rule);
			actorRules.push_back(std::move(rule));
			++stats.refreshedRows;
			++stats.copiedRows;
		}
		if (actorRules.empty()) rules.erase(actor.actorIndex);
	}
	state.previous.resize(state.inputs.size());
	for (size_t i = 0; i < state.inputs.size(); ++i)
		state.previous[i] = { state.inputs[i].actorIndex, state.inputs[i].actorClass };
	state.worldEpoch = worldEpoch;
	state.valid = true;
	return stats;
}

// Only owners mentioned by the retained surface cache need a lookup. Values
// are borrowed during one prune call and cleared by the scope below. Capacity
// follows requested owners, not all live actors or historical actor IDs.
class NRIEmissiveActorLookup
{
public:
	void Begin(uint64_t epoch)
	{
		if (worldEpoch != epoch) quarantined = false;
		worldEpoch = epoch;
		mOwners.clear();
		mValues.clear();
	}
	void Request(int32_t owner) { if (owner >= 0) mOwners.push_back(owner); }
	void Prepare()
	{
		std::sort(mOwners.begin(), mOwners.end());
		mOwners.erase(std::unique(mOwners.begin(), mOwners.end()), mOwners.end());
		mValues.assign(mOwners.size(), nullptr);
	}
	void Set(int32_t owner, void* actor)
	{
		const auto found = std::lower_bound(mOwners.begin(), mOwners.end(), owner);
		if (found != mOwners.end() && *found == owner) mValues[found - mOwners.begin()] = actor;
	}
	void* Find(int32_t owner) const
	{
		const auto found = std::lower_bound(mOwners.begin(), mOwners.end(), owner);
		return found != mOwners.end() && *found == owner ? mValues[found - mOwners.begin()] : nullptr;
	}
	void ClearValues() { std::fill(mValues.begin(), mValues.end(), nullptr); }
	const std::vector<int32_t>& Owners() const { return mOwners; }
	uint64_t CapacityBytes() const { return mOwners.capacity() * sizeof(int32_t) + mValues.capacity() * sizeof(void*); }
	bool quarantined = false;
private:
	uint64_t worldEpoch = 0;
	std::vector<int32_t> mOwners;
	std::vector<void*> mValues;
};

struct NRIScopedEmissiveActorLookup
{
	NRIEmissiveActorLookup& lookup;
	~NRIScopedEmissiveActorLookup() { lookup.ClearValues(); }
};
