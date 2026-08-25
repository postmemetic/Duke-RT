#pragma once

#include <cstdint>
#include <string>

namespace nri_scene
{
enum class MotionProjectionStatus : uint32_t
{
	Valid = 0,
	BehindCamera,
	NonFinite,
};

struct MotionProjectionResult
{
	MotionProjectionStatus status = MotionProjectionStatus::NonFinite;
	bool inViewport = false;
	double uv[2] = {};
};

MotionProjectionResult ProjectMotionPointRaw(
	const double worldPosition[3],
	const double worldToClip[16]);

bool RunNRIMotionProjectionSelfTests(std::string* failureReason = nullptr);
}
