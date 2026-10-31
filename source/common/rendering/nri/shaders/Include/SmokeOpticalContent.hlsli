#ifndef NRI_SMOKE_OPTICAL_CONTENT_HLSLI
#define NRI_SMOKE_OPTICAL_CONTENT_HLSLI

// Extinction attenuates the background even when scattering is zero. Separate
// deposit quantization can also produce that state from a positive albedo.
// Preserve each caller's existing scattering evidence while retaining every
// positive extinction coefficient for conservative view-work discovery.
bool SmokeHasOpticalContent(float extinction, bool scatteringPresent)
{
	return extinction > 0.0 || scatteringPresent;
}

#endif
