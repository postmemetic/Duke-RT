#pragma once

#include "../scene/nri_material_bridge.h"
#include "nri_actor_material_override_cache.h"

#include <memory>

class SceneLightSystem;
struct ResolvedLightOverlaySet;
struct ResolvedFluidOverlaySet;

// Compiled ordered selectors and per-operation settings/identity matches.
// Scalar row transformation is evaluated directly from current clean inputs; no
// transformed rows or per-row dependency records are retained. Use separate
// owners before/after lights. External names/animation are operation-local.
class NRIMaterialPolicySnapshot
{
public:
	struct Settings
	{
		float glowScale = 1.0f;
		float glowFalloff = 1.0f;
		float glowBlend = 1.0f;
		float fullbrightBoost = 1.0f;
	};
	NRIMaterialPolicySnapshot();
	~NRIMaterialPolicySnapshot();
	void Reset();
	void Begin(const SceneLightSystem& lights, const ResolvedLightOverlaySet& overlays,
		const nri_material_policy::ActorMaterialOverrideMap* actors,
		const ResolvedFluidOverlaySet* fluids, const Settings& settings);
	void Transform(const nri_scene::MaterialLightingMetadata* metadata,
		nri_scene::MaterialData& cleanRow);
	// Returns whether a memoized ordered-selector result was consumed.
	bool Finish();
	void Quarantine();
	bool Quarantined() const;

private:
	struct Impl;
	std::unique_ptr<Impl> mImpl;
};
