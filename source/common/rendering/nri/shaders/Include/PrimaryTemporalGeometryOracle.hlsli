#ifndef RAZE_NRI_PRIMARY_TEMPORAL_GEOMETRY_ORACLE_HLSLI
#define RAZE_NRI_PRIMARY_TEMPORAL_GEOMETRY_ORACLE_HLSLI

#if NRI_SHADER_DIAGNOSTICS
// Compare before mirror reflection. These legacy resolvers deliberately keep
// their independent equations; the oracle never replaces candidate outputs.
uint ComparePrimaryTemporalGeometryWithLegacy(
	HitData hit,
	float3 currentPosition,
	float3 previousPosition,
	float3 currentGeometricNormal,
	float3 previousGeometricNormal,
	uint4 temporalIdentity)
{
	uint mismatches = 0u;
	mismatches |= any(asuint(currentPosition) != asuint(ResolveHitVertexPosition(hit, false))) ? 1u : 0u;
	mismatches |= any(asuint(previousPosition) != asuint(ResolveHitVertexPosition(hit, true))) ? 2u : 0u;
	mismatches |= any(asuint(currentGeometricNormal) != asuint(ResolveHitGeometricNormal(hit, false))) ? 4u : 0u;
	mismatches |= any(asuint(previousGeometricNormal) != asuint(ResolveHitGeometricNormal(hit, true))) ? 8u : 0u;
	const PrimitiveData primitive = GetPrimitiveData(hit.dataSource, hit.primitiveIndex);
	const uint4 legacyIdentity = uint4(primitive.temporalSurfaceId, primitive.temporalGeneration, primitive.temporalFlags);
	mismatches |= any(temporalIdentity != legacyIdentity) ? 16u : 0u;
	return mismatches;
}

void RecordPrimaryTemporalGeometryOracle(
	HitData hit,
	float3 currentPosition,
	float3 previousPosition,
	float3 currentGeometricNormal,
	float3 previousGeometricNormal,
	uint4 temporalIdentity)
{
	if (!TraceShaderStatsEnabled())
	{
		return;
	}
	const uint mismatches = ComparePrimaryTemporalGeometryWithLegacy(
		hit, currentPosition, previousPosition, currentGeometricNormal, previousGeometricNormal, temporalIdentity);
	TraceShaderStatAdd(TRACE_STAT_PRIMARY_GEOMETRY_ORACLE_COMPARISONS, 1u);
	if (mismatches != 0u)
	{
		TraceShaderStatAdd(TRACE_STAT_PRIMARY_GEOMETRY_ORACLE_MISMATCH_PIXELS, 1u);
		if ((mismatches & 1u) != 0u) TraceShaderStatAdd(TRACE_STAT_PRIMARY_GEOMETRY_ORACLE_CURRENT_POSITION, 1u);
		if ((mismatches & 2u) != 0u) TraceShaderStatAdd(TRACE_STAT_PRIMARY_GEOMETRY_ORACLE_PREVIOUS_POSITION, 1u);
		if ((mismatches & 4u) != 0u) TraceShaderStatAdd(TRACE_STAT_PRIMARY_GEOMETRY_ORACLE_CURRENT_NORMAL, 1u);
		if ((mismatches & 8u) != 0u) TraceShaderStatAdd(TRACE_STAT_PRIMARY_GEOMETRY_ORACLE_PREVIOUS_NORMAL, 1u);
		if ((mismatches & 16u) != 0u) TraceShaderStatAdd(TRACE_STAT_PRIMARY_GEOMETRY_ORACLE_IDENTITY, 1u);
	}
}
#endif

#endif
