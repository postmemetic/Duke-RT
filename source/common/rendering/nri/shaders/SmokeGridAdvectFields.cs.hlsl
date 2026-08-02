#include "Include/SmokeGridResources.hlsli"

[numthreads(8, 8, 8)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
	const uint activeIndex = groupId.x;
	if (activeIndex >= min(SmokeGridActiveCount(), gSmokeGridConstants.BrickCapacity))
		return;
	const uint brickIndex = SmokeGridActiveBrick(activeIndex);
	if (brickIndex >= gSmokeGridConstants.BrickCapacity)
		return;
	const SmokeGridBrick brick = gSmokeGridBricks[brickIndex];
	if (brick.State != NRI_SMOKE_GRID_RESIDENT)
		return;

	const uint inputPing = min(gSmokeGridConstants.FieldPing, 1u);
	const uint outputPing = 1u - inputPing;
	const uint cellIndex = SmokeGridCellIndex(brickIndex, groupThreadId);
	const float3 worldPosition = SmokeGridCellCenter(brick.Coordinate, groupThreadId,
		max(gSmokeGridConstants.CellSize, 0.0001));
	const float deltaTime = max(gSmokeGridConstants.DeltaTime * gSmokeGridConstants.TimeScale, 0.0);
	float4 velocity = SmokeGridLoadVelocity(outputPing, cellIndex);
	float3 traceVelocity = velocity.xyz;
	if (gSmokeGridConstants.VorticityConfinement > 0.0)
	{
		const float localMass = max(SmokeGridLoadScalar(inputPing, cellIndex).x, 0.0);
		if (localMass > max(gSmokeGridConstants.ActiveThreshold, 1e-8))
		{
			const int3 cell = SmokeGridCellCoordinate(brick.Coordinate, groupThreadId);
			const float magnitudeX0 = SmokeGridLoadCellVorticityMagnitude(cell - int3(1, 0, 0));
			const float magnitudeX1 = SmokeGridLoadCellVorticityMagnitude(cell + int3(1, 0, 0));
			const float magnitudeY0 = SmokeGridLoadCellVorticityMagnitude(cell - int3(0, 1, 0));
			const float magnitudeY1 = SmokeGridLoadCellVorticityMagnitude(cell + int3(0, 1, 0));
			const float magnitudeZ0 = SmokeGridLoadCellVorticityMagnitude(cell - int3(0, 0, 1));
			const float magnitudeZ1 = SmokeGridLoadCellVorticityMagnitude(cell + int3(0, 0, 1));
			const float inverseDiameter = 0.5 / max(gSmokeGridConstants.CellSize, 0.0001);
			const float3 gradient = float3(magnitudeX1 - magnitudeX0,
				magnitudeY1 - magnitudeY0, magnitudeZ1 - magnitudeZ0) * inverseDiameter;
			const float gradientLengthSquared = dot(gradient, gradient);
			if (gradientLengthSquared > 1e-12 && all(isfinite(gradient)))
			{
				const float3 normal = gradient * rsqrt(gradientLengthSquared);
				const float3 omega = gSmokeGridVorticity[cellIndex].xyz;
				traceVelocity += cross(normal, omega) *
					(max(gSmokeGridConstants.CellSize, 0.0001) *
					 max(gSmokeGridConstants.VorticityConfinement, 0.0) * deltaTime);
			}

			if (!all(isfinite(traceVelocity)))
			{
				traceVelocity = gSmokeGridConstants.Wind;
				InterlockedAdd(gSmokeGridControl[0].NanRejects, 1u);
			}
			else
			{
				const float maximumVelocity = max(gSmokeGridConstants.MaxVelocity, 0.0);
				if (dot(traceVelocity, traceVelocity) > maximumVelocity * maximumVelocity)
					InterlockedAdd(gSmokeGridControl[0].VorticityClamps, 1u);
				traceVelocity = SmokeSourceLimitVelocity(traceVelocity, maximumVelocity);
			}
			velocity.xyz = traceVelocity;
			SmokeGridStoreVelocity(outputPing, cellIndex, velocity);
		}
	}
	bool ignoredCflEvent, ignoredClampEvent;
	const float3 backtrace = SmokeGridBacktrace(worldPosition, traceVelocity, deltaTime,
		ignoredCflEvent, ignoredClampEvent);

	float4 scalar, ignoredVelocity, optical, dynamics;
	SmokeGridSampleFields(backtrace, inputPing, scalar, ignoredVelocity, optical, dynamics);
	const float mass = max(scalar.x, 0.0);
	const float inverseMass = mass > 1e-8 ? rcp(mass) : 0.0;
	const float densityRate = max(dynamics.x * inverseMass, 0.0) /
		max(gSmokeGridConstants.DensityHalfLifeScale, 0.001);
	const float coolingRate = max(dynamics.y * inverseMass, 0.0) /
		max(gSmokeGridConstants.CoolingScale, 0.001);
	const float densityDecay = exp(-densityRate * deltaTime);
	const float coolingDecay = exp(-coolingRate * deltaTime);

	scalar.x *= densityDecay;
	scalar.y *= densityDecay * coolingDecay;
	scalar.z *= densityDecay;
	scalar.w *= densityDecay;
	optical *= densityDecay;
	dynamics *= densityDecay;
	scalar.xyz = max(scalar.xyz, 0.0);
	optical = max(optical, 0.0);
	dynamics = max(dynamics, 0.0);

	const float previousDenominator = optical.w;
	const float anisotropy = previousDenominator > 1e-8 ?
		clamp(scalar.w / previousDenominator, -0.95, 0.95) : 0.0;
	optical.xyz = min(optical.xyz, scalar.z.xxx);
	optical.w = dot(optical.xyz, float3(0.2126, 0.7152, 0.0722));
	scalar.w = anisotropy * optical.w;

	if (!all(isfinite(scalar)) || !all(isfinite(optical)) || !all(isfinite(dynamics)))
	{
		scalar = 0.0;
		optical = 0.0;
		dynamics = 0.0;
		InterlockedAdd(gSmokeGridControl[0].NanRejects, 1u);
	}
	SmokeGridStoreScalar(outputPing, cellIndex, scalar);
	SmokeGridStoreOptical(outputPing, cellIndex, optical);
	SmokeGridStoreDynamics(outputPing, cellIndex, dynamics);
}
