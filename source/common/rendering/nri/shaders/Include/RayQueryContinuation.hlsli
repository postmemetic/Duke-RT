#ifndef NRI_RAY_QUERY_CONTINUATION_HLSLI
#define NRI_RAY_QUERY_CONTINUATION_HLSLI

// A committed triangle distance is positive and finite. Advance by the smallest
// representable step: a fixed epsilon can round away at ordinary map distances,
// leaving the next query on the rejected intersection's numerical boundary.
// Keeping the original origin and using one ULP also avoids skipping nearby
// backing surfaces with a larger world-space bias. The caller checks against
// its unchanged TMax before tracing again (FLT_MAX advances to +infinity).
float NextFilteredRayTMin(float committedDistance)
{
	return asfloat(asuint(committedDistance) + 1u);
}

#endif
