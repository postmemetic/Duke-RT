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

	float4 localScalar, localVelocity, localOptical, localDynamics;
	SmokeGridSampleFields(worldPosition, inputPing, localScalar, localVelocity, localOptical, localDynamics);
	bool cflEvent, clampEvent;
	const float3 backtrace = SmokeGridBacktrace(worldPosition, localVelocity.xyz, deltaTime, cflEvent, clampEvent);
	if (cflEvent)
		InterlockedAdd(gSmokeGridControl[0].CflClamps, 1u);
	if (clampEvent)
		InterlockedAdd(gSmokeGridControl[0].BacktraceClamps, 1u);

	float4 scalar, velocity, optical, dynamics;
	SmokeGridSampleFields(backtrace, inputPing, scalar, velocity, optical, dynamics);
	const float mass = max(scalar.x, 0.0);
	const float inverseMass = mass > 1e-8 ? rcp(mass) : 0.0;
	const float thermalBuoyancy = max(scalar.y * inverseMass, 0.0);
	const float styleTurbulence = max(dynamics.z * inverseMass, 0.0);
	const float styleDrag = max(dynamics.w * inverseMass, 0.0);
	const float styleTurbulenceScale = max(abs(SmokeSourceFinite(velocity.w * inverseMass,
		gSmokeGridConstants.CellSize)), 0.0001);
	float3 advectedVelocity = velocity.xyz;
	const float damping = max(gSmokeGridConstants.VelocityDamping + styleDrag, 0.0);
	advectedVelocity *= exp(-damping * deltaTime);
	const float windBlend = 1.0 - exp(-max(gSmokeGridConstants.WindCoupling, 0.0) * deltaTime);
	advectedVelocity = lerp(advectedVelocity, gSmokeGridConstants.Wind, saturate(windBlend));
	advectedVelocity += float3(0.0, 1.0, 0.0) *
		(max(gSmokeGridConstants.Buoyancy, 0.0) * thermalBuoyancy * deltaTime);
	advectedVelocity += SmokeSourceWorldCurl(worldPosition, styleTurbulenceScale,
		gSmokeGridConstants.CurlTime, gSmokeGridConstants.CurlEvolution) *
		(styleTurbulence * deltaTime);

	advectedVelocity = SmokeSourceLimitVelocity(advectedVelocity, gSmokeGridConstants.MaxVelocity);
	if (!all(isfinite(advectedVelocity)))
	{
		advectedVelocity = gSmokeGridConstants.Wind;
		InterlockedAdd(gSmokeGridControl[0].NanRejects, 1u);
	}
	const float densityRate = max(SmokeSourceFinite(dynamics.x * inverseMass, 0.0), 0.0) /
		max(SmokeSourceFinite(gSmokeGridConstants.DensityHalfLifeScale, 0.001), 0.001);
	const float scaleMoment = mass > 1e-8 ?
		max(SmokeSourceFinite(velocity.w, 0.0), 0.0) * exp(-densityRate * deltaTime) : 0.0;
	SmokeGridStoreVelocity(outputPing, cellIndex, float4(advectedVelocity, scaleMoment));

	float4 vorticity = 0.0;
	if (gSmokeGridConstants.VorticityConfinement > 0.0)
	{
		const int3 cell = SmokeGridCellCoordinate(brick.Coordinate, groupThreadId);
		const float3 velocityX0 = SmokeGridLoadCellVelocity(cell - int3(1, 0, 0), inputPing);
		const float3 velocityX1 = SmokeGridLoadCellVelocity(cell + int3(1, 0, 0), inputPing);
		const float3 velocityY0 = SmokeGridLoadCellVelocity(cell - int3(0, 1, 0), inputPing);
		const float3 velocityY1 = SmokeGridLoadCellVelocity(cell + int3(0, 1, 0), inputPing);
		const float3 velocityZ0 = SmokeGridLoadCellVelocity(cell - int3(0, 0, 1), inputPing);
		const float3 velocityZ1 = SmokeGridLoadCellVelocity(cell + int3(0, 0, 1), inputPing);
		const float inverseDiameter = 0.5 / max(gSmokeGridConstants.CellSize, 0.0001);
		const float3 omega = float3(
			velocityY1.z - velocityY0.z - velocityZ1.y + velocityZ0.y,
			velocityZ1.x - velocityZ0.x - velocityX1.z + velocityX0.z,
			velocityX1.y - velocityX0.y - velocityY1.x + velocityY0.x) * inverseDiameter;
		if (all(isfinite(omega)))
			vorticity = float4(omega, length(omega));
		else
			InterlockedAdd(gSmokeGridControl[0].NanRejects, 1u);
	}
	gSmokeGridVorticity[cellIndex] = vorticity;
}
