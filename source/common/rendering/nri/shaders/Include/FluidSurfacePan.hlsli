#ifndef NRI_FLUID_SURFACE_PAN_HLSLI
#define NRI_FLUID_SURFACE_PAN_HLSLI

#include "FluidSurfaceContracts.hlsli"

// Material advection is separate from geometric motion. Keep the domain frame
// world-aligned, including across animated slopes; UV flips/rotation/scale are
// used only to translate the field. No additional rays or primitive payload.
struct FluidPanEvaluation
{
	float2 currentOffset;
	float2 previousOffset;
	float2 currentDomainRate;
	float2 previousDomainRate;
	float currentPhaseSeconds;
	float previousPhaseSeconds;
	uint flags;
};

FluidPanEvaluation FluidResolvePan(HitData hit, MaterialData material,
	FluidBindingData binding, FluidFrameData frame)
{
	FluidPanEvaluation result = (FluidPanEvaluation)0;
	result.currentPhaseSeconds = frame.currentGameplaySeconds;
	result.previousPhaseSeconds = frame.previousGameplaySeconds;
	[branch]
	if ((binding.flags & NRI_FLUID_BINDING_FLAG_FOLLOW_PAN) == 0u ||
		(material.flags & MATERIAL_FLAG_FLAT) == 0u ||
		(binding.styleFlags & NRI_FLUID_STYLE_FALL) != 0u)
		return result;

	uint count, stride;
	gFluidPans.GetDimensions(count, stride);
	if (material.sectorIndex >= count / 2u)
		return result;
	const PrimitiveData primitive = GetPrimitiveData(hit.dataSource, hit.primitiveIndex);
	const uint plane = primitive.normal.y < 0.0 ? 1u : 0u;
	const FluidPanData pan = gFluidPans[material.sectorIndex * 2u + plane];
	if (pan.flags == 0u)
		return result;

	const float2 uv1 = primitive.uv1 - primitive.uv0;
	const float2 uv2 = primitive.uv2 - primitive.uv0;
	const float determinant = uv1.x * uv2.y - uv1.y * uv2.x;
	// Relative conditioning guard, so valid very small/large texture scales do
	// not accidentally switch off inheritance. Degenerate mappings fail closed.
	if (!isfinite(determinant) || abs(determinant) <=
		1.0e-6 * max(length(uv1) * length(uv2), 1.0e-12))
		return result;
	float3 p0 = GetVertexData(hit.dataSource, primitive.indices.x).position;
	float3 p1 = GetVertexData(hit.dataSource, primitive.indices.y).position;
	float3 p2 = GetVertexData(hit.dataSource, primitive.indices.z).position;
	if (hit.instanceId != 0xffffffffu)
	{
		const SceneInstanceData instanceData = GetSceneInstanceData(hit.instanceId);
		p0 = TransformSceneInstancePoint(instanceData, p0, false);
		p1 = TransformSceneInstancePoint(instanceData, p1, false);
		p2 = TransformSceneInstancePoint(instanceData, p2, false);
	}
	const float3 edge1 = p1 - p0;
	const float3 edge2 = p2 - p0;
	const float3 dPdu = (edge1 * uv2.y - edge2 * uv1.y) / determinant;
	const float3 dPdv = (edge2 * uv1.x - edge1 * uv2.x) / determinant;
	// A positive UV offset moves the visible texture in -dPdu/-dPdv. Sampling
	// a fixed field at world - displacement therefore ADDS this Jacobian term.
	const float3 currentShift = dPdu * pan.currentPan.x + dPdv * pan.currentPan.y;
	const float3 previousShift = dPdu * pan.previousPan.x + dPdv * pan.previousPan.y;
	const float3 currentRate = dPdu * pan.currentPanRate.x + dPdv * pan.currentPanRate.y;
	const float3 previousRate = dPdu * pan.previousPanRate.x + dPdv * pan.previousPanRate.y;
	const float scale = max(abs(binding.domainOriginScale.w), 1.0e-5);
	const float2 currentOffset = float2(currentShift.x, -currentShift.z) * scale;
	const float2 previousOffset = float2(previousShift.x, -previousShift.z) * scale;
	if (!all(isfinite(currentOffset)) || !all(isfinite(previousOffset)))
		return result;
	result.flags = pan.flags;
	if ((pan.flags & NRI_FLUID_PAN_CURRENT_ACTIVE) != 0u)
	{
		result.currentOffset = currentOffset;
		const float2 domainRate = float2(currentRate.x, -currentRate.z) * scale;
		result.currentDomainRate = all(isfinite(domainRate)) ? domainRate : 0.0;
		result.currentPhaseSeconds = pan.currentPhaseSeconds;
	}
	if ((pan.flags & NRI_FLUID_PAN_PREVIOUS_ACTIVE) != 0u)
	{
		result.previousOffset = previousOffset;
		const float2 domainRate = float2(previousRate.x, -previousRate.z) * scale;
		result.previousDomainRate = all(isfinite(domainRate)) ? domainRate : 0.0;
		result.previousPhaseSeconds = pan.previousPhaseSeconds;
	}
	return result;
}

float FluidPanReactive(FluidPanEvaluation pan, FluidBindingData binding)
{
	return saturate(length(pan.currentOffset - pan.previousOffset) *
		max(abs(binding.spectrum.y), 1.0e-4) * clamp(binding.motion.z, 0.2, 5.0));
}

#endif
