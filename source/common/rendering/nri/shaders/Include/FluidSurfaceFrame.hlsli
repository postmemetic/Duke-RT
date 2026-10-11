#ifndef NRI_FLUID_SURFACE_FRAME_HLSLI
#define NRI_FLUID_SURFACE_FRAME_HLSLI

// Shared by DXIL and the SPIR-V compatibility field. Horizontal material
// coordinates must not rotate with each independently animated sector plane.
void FluidBuildDomainAxes(MaterialData material, FluidBindingData binding,
	float3 carrierNormal, out float3 axisU, out float3 axisV)
{
	if ((material.flags & MATERIAL_FLAG_FLAT) != 0u &&
		(binding.styleFlags & NRI_FLUID_STYLE_FALL) == 0u)
	{
		axisU = float3(1.0, 0.0, 0.0);
		axisV = float3(0.0, 0.0, -1.0);
		return;
	}
	float3 requestedU = binding.domainAxisUFlowU.xyz;
	requestedU -= carrierNormal * dot(requestedU, carrierNormal);
	if (dot(requestedU, requestedU) <= 1.0e-8)
	{
		const float3 fallbackUp = abs(carrierNormal.z) < 0.95 ?
			float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
		requestedU = cross(fallbackUp, carrierNormal);
	}
	axisU = normalize(requestedU);
	float3 requestedV = binding.domainAxisVFlowV.xyz;
	requestedV -= carrierNormal * dot(requestedV, carrierNormal) +
		axisU * dot(requestedV, axisU);
	axisV = dot(requestedV, requestedV) > 1.0e-8 ?
		normalize(requestedV) : normalize(cross(carrierNormal, axisU));
	if (dot(cross(axisU, axisV), carrierNormal) < 0.0)
		axisV = -axisV;
}

float3 FluidComposeShadingNormal(float3 carrierNormal, float3 carrierShadingNormal,
	float3 axisU, float3 axisV, float2 gradient, float strength)
{
	float3 smoothCarrier = FluidSafeNormalize(carrierShadingNormal, carrierNormal);
	if (dot(smoothCarrier, carrierNormal) < 0.0)
		smoothCarrier = -smoothCarrier;
	float3 tangentGradient = gradient.x * axisU + gradient.y * axisV;
	tangentGradient -= smoothCarrier * dot(tangentGradient, smoothCarrier);
	return FluidSafeNormalize(smoothCarrier - strength * tangentGradient, smoothCarrier);
}

// Geometry motion is still authoritative. This is a confidence signal for
// changing material features, with profile time scaling and pause respected.
float FluidAnimationReactive(FluidFrameData frame, FluidBindingData binding, float timeScale)
{
	float2 domainVelocity = 0.0;
	if ((binding.styleFlags & NRI_FLUID_STYLE_FLOW) != 0u)
		domainVelocity = float2(binding.domainAxisUFlowU.w, binding.domainAxisVFlowV.w);
	if ((binding.styleFlags & NRI_FLUID_STYLE_FALL) != 0u)
		domainVelocity.y += binding.domainAxisVFlowV.w * 1.146;
	const float phaseRate = abs(binding.motion.x) + length(domainVelocity) *
		max(abs(binding.spectrum.y), 1.0e-4) * clamp(binding.motion.z, 0.2, 5.0);
	const float deltaTime = abs((frame.currentGameplaySeconds - frame.previousGameplaySeconds) * timeScale);
	return saturate(deltaTime * phaseRate);
}

#endif
