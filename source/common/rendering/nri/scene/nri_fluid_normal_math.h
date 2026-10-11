#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace nri_scene
{
constexpr uint32_t FluidSmoothNormalTag = 0xc0000000u;

inline std::array<double, 3> FluidCarrierNormalFromBuildHeightDerivatives(double dx, double dy)
{
	// Height is renderer Y, while Build XY maps to renderer X,-Z.
	const double length = std::sqrt(dx * dx + dy * dy + 1.0);
	return { -dx / length, 1.0 / length, dy / length };
}

inline uint32_t PackFluidCarrierNormal(double x, double y, double z)
{
	const double length = std::abs(x) + std::abs(y) + std::abs(z);
	if (!(length > 1.0e-12) || !std::isfinite(length)) return UINT32_MAX;
	x /= length; y /= length; z /= length;
	if (z < 0.0)
	{
		const double oldX = x;
		x = (1.0 - std::abs(y)) * (oldX < 0.0 ? -1.0 : 1.0);
		y = (1.0 - std::abs(oldX)) * (y < 0.0 ? -1.0 : 1.0);
	}
	return (uint32_t)std::lround((std::clamp(x, -1.0, 1.0) * 0.5 + 0.5) * 255.0) |
		((uint32_t)std::lround((std::clamp(y, -1.0, 1.0) * 0.5 + 0.5) * 255.0) << 8u);
}

inline bool HasFluidCarrierNormals(const uint32_t packed[2])
{
	return (packed[1] & 0xffff0000u) == FluidSmoothNormalTag;
}

inline bool HasSupportedMapCarrierNormals(const uint32_t packed[2])
{
	return (packed[0] == 0u && packed[1] == 0u) || HasFluidCarrierNormals(packed);
}

inline uint32_t FluidCarrierCornerNormal(const uint32_t packed[2], uint32_t corner)
{
	return corner == 0u ? packed[0] & 0xffffu :
		corner == 1u ? packed[0] >> 16u : packed[1] & 0xffffu;
}

inline void StoreFluidCarrierNormals(uint32_t out[2], uint32_t a, uint32_t b, uint32_t c)
{
	if (a > 0xffffu || b > 0xffffu || c > 0xffffu) return;
	out[0] = a | (b << 16u);
	out[1] = c | FluidSmoothNormalTag;
}

inline void RemapFluidCarrierNormals(const uint32_t source[2], const uint32_t corners[3], uint32_t out[2])
{
	out[0] = out[1] = 0u;
	if (HasFluidCarrierNormals(source)) StoreFluidCarrierNormals(out,
		FluidCarrierCornerNormal(source, corners[0]), FluidCarrierCornerNormal(source, corners[1]),
		FluidCarrierCornerNormal(source, corners[2]));
}

inline bool FluidFloorEdgeIsContinuous(double a0, double a1, double b0, double b1, double normalDot)
{
	// Match entire edges, not just a coincident corner: steps and crossing planes
	// must retain their own carrier normals. The tolerance is 1/32 renderer unit.
	return std::isfinite(a0) && std::isfinite(a1) && std::isfinite(b0) && std::isfinite(b1) &&
		std::abs(a0 - b0) <= 0.03125 && std::abs(a1 - b1) <= 0.03125 && normalDot >= 0.70710678118;
}
}
