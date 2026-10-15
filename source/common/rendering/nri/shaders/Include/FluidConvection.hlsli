#ifndef NRI_FLUID_CONVECTION_HLSLI
#define NRI_FLUID_CONVECTION_HLSLI

// D3D-only, stateless broad convection. This is a smooth scalar height field,
// not a fluid simulation or nearest-cell/Voronoi boundary effect. The same
// field supplies virtual relief, analytic normals and current/previous crust.
struct FluidConvectionEvaluation
{
	float heightRatio;
	float2 gradientRatio;
	float foldPotential;
};

bool FluidUsesConvection(FluidBindingData binding)
{
	return (binding.flags & NRI_FLUID_BINDING_FLAG_CONVECTION) != 0u;
}

float FluidConvectionCellScale(FluidBindingData binding)
{
	return clamp(binding.convectionParams.x, 8.0, 4096.0);
}

float FluidConvectionDomainToCell(FluidBindingData binding)
{
	return rcp(max(abs(binding.domainOriginScale.w), 1.0e-5) *
		FluidConvectionCellScale(binding));
}

float FluidConvectionFootprint(FluidBindingData binding, float worldFootprint)
{
	return max(worldFootprint, 0.0) * 4.0 / FluidConvectionCellScale(binding);
}

float FluidConvectionFoldStrength(FluidBindingData binding, float amplitudeScale)
{
	// A dimensionless relief gate, independent of the unused wave spectrum.
	return saturate(max(binding.spectrum.x, 0.0) * max(amplitudeScale, 0.0) *
		16.0 / FluidConvectionCellScale(binding));
}

FluidConvectionEvaluation FluidEvaluateConvection(
	FluidBindingData binding,
	float2 domain,
	float gameplaySeconds,
	float worldFootprint)
{
	FluidConvectionEvaluation result = (FluidConvectionEvaluation)0;
	if (!all(isfinite(domain)) || !isfinite(gameplaySeconds))
		return result;

	const float domainToCell = FluidConvectionDomainToCell(binding);
	const uint rotationHash = FluidHashUint(binding.seed ^ 0x632be59bu);
	float2 latticeU = float2(rotationHash & 0xffffu, rotationHash >> 16u) *
		(2.0 / 65535.0) - 1.0;
	const float rotationLengthSq = dot(latticeU, latticeU);
	latticeU = rotationLengthSq > 1.0e-8 ?
		latticeU * rsqrt(rotationLengthSq) : float2(1.0, 0.0);
	const float2 latticeV = float2(-latticeU.y, latticeU.x);
	const float2 p = float2(dot(domain, latticeU), dot(domain, latticeV)) * domainToCell;
	const int2 baseCell = (int2)floor(p);
	const float rollSpeed = clamp(binding.convectionParams.y, 0.0, 2.0);
	const float rollStrength = saturate(binding.convectionParams.z);
	const float inverseRadius = 0.8; // Support radius is exactly 1.25 cells.
	float weightSum = 0.0;
	float weightedHeight = 0.0;
	float weightedFold = 0.0;
	float2 weightGradientSum = 0.0;
	float2 weightedGradient = 0.0;

	// Centers stay in [cell + 0.28, cell + 0.72]. A cell omitted by this
	// 3x3 stencil is at least 1.28 cells away on one axis, outside radius 1.25.
	// The containing cell is at most sqrt(2)*0.72 away, so support never vanishes.
	// Quartic support and its first two derivatives vanish at the boundary;
	// changing the stencil cannot introduce a height/normal seam.
	// Cost: nine bounded candidates, one jitter hash each, and only supported
	// cells pay a second hash and one sincos shared by swelling and rolling.
	[loop]
	for (int cellY = -1; cellY <= 1; ++cellY)
	{
		[loop]
		for (int cellX = -1; cellX <= 1; ++cellX)
		{
			const int2 cell = baseCell + int2(cellX, cellY);
			const uint cellHash = FluidHashUint(binding.seed ^
				((uint)cell.x * 0x8da6b343u) ^ ((uint)cell.y * 0xd8163841u));
			const float2 jitter = (float2(cellHash & 0xffffu, cellHash >> 16u) *
				(2.0 / 65535.0) - 1.0) * 0.22;
			const float2 r = (p - ((float2)cell + 0.5 + jitter)) * inverseRadius;
			const float radiusSq = dot(r, r);
			[branch]
			if (radiusSq >= 1.0)
				continue;

			const uint motionHash = FluidHashUint(cellHash ^ 0x9e3779b9u);
			const float phaseOffset = (float)(motionHash & 0xffffu) * (1.0 / 65535.0);
			const float rate = 0.72 + 0.56 *
				(float)((motionHash >> 16u) & 0xffu) * (1.0 / 255.0);
			const float bias = (float)(motionHash >> 24u) * (2.0 / 255.0) - 1.0;
			const float spinSign = (cellHash & 1u) != 0u ? 1.0 : -1.0;
			const float phase = 6.28318530718 *
				(phaseOffset + gameplaySeconds * rollSpeed * rate);
			float sine, cosine;
			sincos(phase, sine, cosine);
			const float2 rollDirection = float2(cosine, spinSign * sine);

			// Broad rounded lobes with independent heights, slow buoyancy and a
			// rotating local tilt. There is no shared traveling phase or drifting
			// noise coordinate. At zero strength the irregular base relief remains;
			// zero speed instead freezes each cell at its independent initial phase.
			// Each local height is bounded by one, as is their normalized mixture.
			const float localHeight = 0.25 * (1.0 - 2.0 * radiusSq) + 0.25 * bias +
				rollStrength * (0.30 * sine + 0.20 * dot(r, rollDirection));
			const float2 localGradient = inverseRadius *
				(-r + 0.20 * rollStrength * rollDirection);
			const float support = 1.0 - radiusSq;
			const float supportSq = support * support;
			const float weight = supportSq * supportSq;
			const float2 weightGradient =
				-8.0 * inverseRadius * supportSq * support * r;
			weightSum += weight;
			weightedHeight += weight * localHeight;
			weightGradientSum += weightGradient;
			weightedGradient += weight * localGradient + weightGradient * localHeight;

			// Broad cell shoulders/folds, centered on the same tilted quadratic
			// lobe. Blend potentials, never nearest-cell distances or thin outlines.
			const float2 foldedRadius = r - 0.20 * rollStrength * rollDirection;
			const float fold = smoothstep(0.025, 0.32, dot(foldedRadius, foldedRadius)) *
				(0.9 + 0.1 * rollStrength * sine);
			weightedFold += weight * fold;
		}
	}

	if (weightSum <= 1.0e-6)
		return result;
	const float height = weightedHeight / weightSum;
	// Exact quotient derivative, including the support and local lobe terms.
	const float2 latticeGradient =
		(weightedGradient - height * weightGradientSum) / weightSum;
	const float footprint = FluidConvectionFootprint(binding, worldFootprint);
	const float detailWeight = rcp(1.0 + footprint * footprint);
	result.heightRatio = height * detailWeight;
	result.gradientRatio = (latticeU * latticeGradient.x + latticeV * latticeGradient.y) *
		(domainToCell * detailWeight);
	result.foldPotential = saturate(weightedFold / weightSum) * detailWeight;
	if (!isfinite(result.heightRatio) || !all(isfinite(result.gradientRatio)) ||
		!isfinite(result.foldPotential))
		result = (FluidConvectionEvaluation)0;
	return result;
}

#endif
