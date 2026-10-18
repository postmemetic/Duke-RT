#ifndef NRI_SMOKE_PROJECTION_HLSLI
#define NRI_SMOKE_PROJECTION_HLSLI

struct SmokeProjectedAxisBounds
{
	float Minimum;
	float Maximum;
};

// Bounds of the sphere's enclosing camera-space box along one projected axis.
// The caller handles spheres crossing the camera plane, so viewDepth > radius.
// For an entirely off-axis interval the endpoint nearest the screen center is
// attained at the FAR depth. Using near depth for both endpoints can reject
// visible smoke or leave holes in the compact froxel selection.
SmokeProjectedAxisBounds SmokeProjectSphereAxis(float center, float radius,
	float viewDepth, float tanHalfFov)
{
	const float nearDepth = viewDepth - radius;
	const float farDepth = viewDepth + radius;
	const float minimum = center - radius;
	const float maximum = center + radius;
	const float safeTan = max(tanHalfFov, 1e-4);
	SmokeProjectedAxisBounds bounds;
	bounds.Minimum = (minimum / (minimum < 0.0 ? nearDepth : farDepth)) / safeTan;
	bounds.Maximum = (maximum / (maximum > 0.0 ? nearDepth : farDepth)) / safeTan;
	return bounds;
}

#endif
