#ifndef NRI_SMOKE_PHASE_HLSLI
#define NRI_SMOKE_PHASE_HLSLI

float SmokePhaseSafeAnisotropy(float anisotropy, float limit)
{
	return isfinite(anisotropy) ? clamp(anisotropy, -limit, limit) : 0.0;
}

float SmokeHenyeyGreenstein(float cosineTheta, float anisotropy)
{
	const float safeCosine = isfinite(cosineTheta) ? clamp(cosineTheta, -1.0, 1.0) : 0.0;
	const float g = SmokePhaseSafeAnisotropy(anisotropy, 0.95);
	const float gSquared = g * g;
	const float denominatorBase = max(1.0 + gSquared - 2.0 * g * safeCosine, 1e-4);
	return (1.0 - gSquared) / (12.56637061436 * denominatorBase * sqrt(denominatorBase));
}

float2 SmokePhaseSettings()
{
	const uint packed = gSmokeConstants.OutputWidth;
	const float2 decoded = float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16u));
	return float2(
		isfinite(decoded.x) ? saturate(decoded.x) : 0.0,
		SmokePhaseSafeAnisotropy(decoded.y, 0.90));
}

float SmokePhaseResponse(float cosineTheta, float bodyAnisotropy)
{
	const float2 settings = SmokePhaseSettings();
	const float body = SmokeHenyeyGreenstein(cosineTheta, bodyAnisotropy);
	if (settings.x <= 0.0)
		return body;
	const float secondary = SmokeHenyeyGreenstein(cosineTheta, settings.y);
	const float response = lerp(body, secondary, settings.x);
	return isfinite(response) ? max(response, 0.0) : body;
}

float SmokePhaseEffectiveAnisotropy(float bodyAnisotropy)
{
	const float2 settings = SmokePhaseSettings();
	const float body = SmokePhaseSafeAnisotropy(bodyAnisotropy, 0.95);
	if (settings.x <= 0.0)
		return body;
	return SmokePhaseSafeAnisotropy(lerp(body, settings.y, settings.x), 0.95);
}

uint SmokePhaseSettingsSignature()
{
	return gSmokeConstants.OutputWidth;
}

#endif
