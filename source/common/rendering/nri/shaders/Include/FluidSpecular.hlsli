#ifndef NRI_FLUID_SPECULAR_HLSLI
#define NRI_FLUID_SPECULAR_HLSLI

// Opt-in primary reflected transport only. Keep the existing dielectric F0
// and NRD/DLRR material guides; the legacy transport omitted its grazing rise.
// The caller supplies the final valid reflection normal's view cosine, after
// applying its carrier/view-facing fallback. Omitted authoring is exact bypass.
float3 FluidApplyPrimarySpecularFresnel(float3 legacyF0, float viewCosine, bool enabled)
{
	if (!enabled || !isfinite(viewCosine))
	{
		return legacyF0;
	}
	const float grazing = 1.0 - saturate(viewCosine);
	const float grazingSquared = grazing * grazing;
	const float schlick = grazingSquared * grazingSquared * grazing;
	return lerp(legacyF0, 1.0.xxx, schlick);
}

#endif
