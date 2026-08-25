#include "nri_motion_projection.h"

#include <cmath>
#include <limits>

namespace nri_scene
{
MotionProjectionResult ProjectMotionPointRaw(
	const double worldPosition[3],
	const double worldToClip[16])
{
	MotionProjectionResult result;
	double clip[4] = {};
	for (uint32_t row = 0; row < 4u; ++row)
	{
		clip[row] =
			worldToClip[row * 4u + 0u] * worldPosition[0] +
			worldToClip[row * 4u + 1u] * worldPosition[1] +
			worldToClip[row * 4u + 2u] * worldPosition[2] +
			worldToClip[row * 4u + 3u];
	}
	if (!std::isfinite(worldPosition[0]) || !std::isfinite(worldPosition[1]) ||
		!std::isfinite(worldPosition[2]) || !std::isfinite(clip[0]) ||
		!std::isfinite(clip[1]) || !std::isfinite(clip[2]) || !std::isfinite(clip[3]))
	{
		return result;
	}
	if (clip[3] <= 1.0e-5)
	{
		result.status = MotionProjectionStatus::BehindCamera;
		return result;
	}
	result.uv[0] = clip[0] / clip[3] * 0.5 + 0.5;
	result.uv[1] = 0.5 - clip[1] / clip[3] * 0.5;
	result.status = MotionProjectionStatus::Valid;
	result.inViewport = result.uv[0] >= 0.0 && result.uv[0] <= 1.0 &&
		result.uv[1] >= 0.0 && result.uv[1] <= 1.0;
	return result;
}

bool RunNRIMotionProjectionSelfTests(std::string* failureReason)
{
	auto fail = [&](const char* reason)
	{
		if (failureReason != nullptr) *failureReason = reason;
		return false;
	};
	const double identity[16] = {
		1.0, 0.0, 0.0, 0.0,
		0.0, 1.0, 0.0, 0.0,
		0.0, 0.0, 1.0, 0.0,
		0.0, 0.0, 0.0, 1.0,
	};
	const double offscreen[3] = { 1.2, 0.0, 0.0 };
	const MotionProjectionResult outside = ProjectMotionPointRaw(offscreen, identity);
	if (outside.status != MotionProjectionStatus::Valid || outside.inViewport ||
		std::fabs(outside.uv[0] - 1.1) > 1.0e-9)
		return fail("finite offscreen UV was discarded");
	double behindMatrix[16];
	for (uint32_t i = 0; i < 16u; ++i) behindMatrix[i] = identity[i];
	behindMatrix[15] = -1.0;
	const double origin[3] = {};
	if (ProjectMotionPointRaw(origin, behindMatrix).status != MotionProjectionStatus::BehindCamera)
		return fail("behind-camera point accepted");
	double nonFinite[3] = { std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0 };
	if (ProjectMotionPointRaw(nonFinite, identity).status != MotionProjectionStatus::NonFinite)
		return fail("non-finite point accepted");
	if (failureReason != nullptr) failureReason->clear();
	return true;
}
}
