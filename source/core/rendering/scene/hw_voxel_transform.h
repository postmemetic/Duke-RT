#pragma once

#include "vectors.h"

// Actor-owned inputs must be copied before entering snapshot-only preparation.
// Pitch and roll remain available here, but the legacy voxel path uses only yaw.
struct HWVoxelActorInputs
{
	DRotator modelRotation = {};
	DVector3 modelPositionOffset = { 0, 0, 0 };
	int cstat = 0;
	float alpha = 0;
};

// No actor, game callback, texture lookup or mutable renderer state is needed
// to turn an already resolved voxel recipe into its placement transform.
struct HWVoxelTransformInputs
{
	DVector3 position = { 0, 0, 0 };
	DVector3 modelPositionOffset = { 0, 0, 0 };
	DVector2 spriteScale = { 1, 1 };
	DAngle spriteYaw = DAngle::fromDeg(0);
	DAngle modelYaw = DAngle::fromDeg(0);
	FVector3 pivot = { 0, 0, 0 };
	int height = 0;
	int xOffset = 0;
	int yOffset = 0;
	float voxelScale = 1;
	float baseScale = 1;
	float zAdd = 0;
	int rotationClock = 0;
	bool rotate = false;
	bool wallAlignedOwner = false;
	bool alignedOwner = false;
	bool xFlip = false;
	bool yFlip = false;
	bool yCenter = false;
	bool slabAligned = false;
};

struct HWVoxelTransform
{
	float x = 0, y = 0, z = 0;
	double yawDegrees = 0;
	FVector3 translation;
	FVector3 scale;
	FVector3 pivot;
};

inline HWVoxelTransform BuildHWVoxelTransform(const HWVoxelTransformInputs& input)
{
	auto ang = input.spriteYaw + input.modelYaw;
	if (input.rotate) ang += DAngle::fromBuild(input.rotationClock);

	FVector3 scalevec = { input.voxelScale, input.voxelScale, input.voxelScale };
	FVector3 translatevec = { 0, 0, input.zAdd * input.voxelScale };
	float basescale = input.baseScale;
	float sprxscale = (float)input.spriteScale.X * 0.8f * basescale;
	if (input.wallAlignedOwner)
	{
		sprxscale *= 1.25f;
		auto rvec = input.modelYaw.ToVector();
		translatevec.Y -= input.xOffset * rvec.X / 64;
		translatevec.X += input.xOffset * rvec.Y / 64;
	}
	if (input.yFlip)
	{
		scalevec.Z = -scalevec.Z;
		translatevec.Z = -translatevec.Z;
	}
	if (input.xFlip)
	{
		scalevec.X = -scalevec.X;
		translatevec.X = -translatevec.X;
		translatevec.Y = -translatevec.Y;
	}
	scalevec.X *= sprxscale;
	translatevec.X *= sprxscale;
	scalevec.Y *= sprxscale;
	translatevec.Y *= sprxscale;
	float sprzscale = (float)input.spriteScale.Y * basescale;
	scalevec.Z *= sprzscale;
	translatevec.Z *= sprzscale;

	float zpos = (float)(input.position.Z + input.modelPositionOffset.Z);
	float zscale = (input.yFlip && input.alignedOwner) ? -1.f : 1.f;
	zpos -= (input.yOffset * input.spriteScale.Y) * zscale * basescale;
	HWVoxelTransform result;
	result.x = (float)(input.position.X + input.modelPositionOffset.X);
	result.z = -zpos;
	result.y = (float)-(input.position.Y + input.modelPositionOffset.Y);

	float zoff = input.height * .5f;
	if (!input.yCenter)
		zoff += input.pivot.Z;
	else if (!input.slabAligned)
	{
		zoff += input.pivot.Z;
		zoff -= input.height * .5f;
	}
	if (input.yFlip) zoff = input.height - zoff;

	result.yawDegrees = ang.Degrees() - 90.;
	result.translation = { result.x + translatevec.X, result.z - translatevec.Z, result.y - translatevec.Y };
	result.scale = { scalevec.X, scalevec.Z, scalevec.Y };
	result.pivot = { -input.pivot.X, zoff, input.pivot.Y };
	return result;
}

template<class Matrix>
inline void ApplyHWVoxelTransform(Matrix& matrix, const HWVoxelTransform& transform)
{
	matrix.loadIdentity();
	matrix.translate(transform.translation.X, transform.translation.Y, transform.translation.Z);
	matrix.rotate(transform.yawDegrees, 0, 1, 0);
	matrix.scale(transform.scale.X, transform.scale.Y, transform.scale.Z);
	// Apply pivot last, matching the live raster/voxel path.
	matrix.translate(transform.pivot.X, transform.pivot.Y, transform.pivot.Z);
}
