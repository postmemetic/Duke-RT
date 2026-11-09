#pragma once

#include "../scene/nri_material_bridge.h"
#include "nri_actor_material_override_cache.h"

#include <memory>

class SceneLightSystem;
struct ResolvedLightOverlaySet;
struct ResolvedFluidOverlaySet;

// A bounded cache of exact clean-row inputs and material policy results. Begin
// captures current external policy inputs; no frame number, pointer or hash is
// treated as a publication certificate. Use separate owners before/after lights.
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
		const ResolvedFluidOverlaySet* fluids, size_t rowCount, const Settings& settings);
	void Transform(size_t index, const nri_scene::MaterialLightingMetadata* metadata,
		nri_scene::MaterialData& cleanRow);
	bool Finish();
	void Quarantine();
	bool Quarantined() const;

private:
	struct Impl;
	std::unique_ptr<Impl> mImpl;
};
