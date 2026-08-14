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
	const uint cellIndex = SmokeGridCellIndex(brickIndex, groupThreadId);
	const int4 q0 = gSmokeGridDeposit0[cellIndex];
	const int4 q1 = gSmokeGridDeposit1[cellIndex];
	const int4 q2 = gSmokeGridDeposit2[cellIndex];
	const int4 q3 = gSmokeGridDeposit3[cellIndex];
	if (!any(q0 != 0) && !any(q1 != 0) && !any(q2 != 0) && !any(q3 != 0))
		return;
	const uint fieldPing = min(gSmokeGridConstants.FieldPing, 1u);
	float4 scalar = SmokeGridLoadScalar(fieldPing, cellIndex);
	float4 velocity = SmokeGridLoadVelocity(fieldPing, cellIndex);
	float4 optical = SmokeGridLoadOptical(fieldPing, cellIndex);
	float4 dynamics = SmokeGridLoadDynamics(fieldPing, cellIndex);
	const float inverseMassQ = rcp(max(gSmokeGridConstants.MassQuantization, 0.0001));
	const float inverseMomentumQ = rcp(max(gSmokeGridConstants.MomentumQuantization, 0.0001));
	const float addedMass = max((float)q0.x * inverseMassQ, 0.0);
	const float newMass = scalar.x + addedMass;
	if (newMass > 1e-8)
	{
		velocity.xyz = (velocity.xyz * scalar.x + (float3)q1.xyz * inverseMomentumQ) / newMass;
	}
	// Velocity.xyz is a mass-weighted average. Velocity.w deliberately remains
	// the additive turbulence-scale moment so trilinear sampling can divide it
	// by the matching sampled mass without shrinking scale at smoke boundaries.
	velocity.w += (float)q3.w * inverseMassQ;
	scalar += (float4)q0 * inverseMassQ;
	optical += (float4)q2 * inverseMassQ;
	dynamics += float4((float)q1.w, (float)q3.x, (float)q3.y, (float)q3.z) * inverseMassQ;
	scalar = max(scalar, float4(0.0, 0.0, 0.0, -3.402823466e+38));
	optical = max(optical, 0.0);
	dynamics = max(dynamics, 0.0);
	if (!all(isfinite(scalar)) || !all(isfinite(velocity)) || !all(isfinite(optical)) || !all(isfinite(dynamics)))
	{
		scalar = 0.0;
		velocity = float4(gSmokeGridConstants.Wind, 0.0);
		optical = 0.0;
		dynamics = 0.0;
		InterlockedAdd(gSmokeGridControl[0].NanRejects, 1u);
	}
	SmokeGridStoreScalar(fieldPing, cellIndex, scalar);
	SmokeGridStoreVelocity(fieldPing, cellIndex, velocity);
	SmokeGridStoreOptical(fieldPing, cellIndex, optical);
	SmokeGridStoreDynamics(fieldPing, cellIndex, dynamics);
	if (any(abs(optical) > 0.0))
		InterlockedOr(gSmokeGridBricks[brickIndex].Flags, NRI_SMOKE_GRID_BRICK_OPTICAL_CONTENT);
	gSmokeGridDeposit0[cellIndex] = 0;
	gSmokeGridDeposit1[cellIndex] = 0;
	gSmokeGridDeposit2[cellIndex] = 0;
	gSmokeGridDeposit3[cellIndex] = 0;
}
