#pragma once

#include <cmath>
#include <cstdint>
#include <type_traits>

struct SectorPanMotionSample
{
	double x;
	double y;
	bool hasMoved;
};

// Gameplay-owned displacement, independent of renderer visibility/history.
// Initial authored offsets are not motion. Temporary interpolation writes to
// the wrapped fields are observed by GetPresented, never accumulated again.
struct SectorPanMotionState
{
	enum Axis : uint32_t { FloorX, FloorY, CeilingX, CeilingY, AxisCount };
	double accumulated[AxisCount];
	float authoredWrapped[AxisCount];
	uint32_t initializedMask;
	uint32_t movedMask;

	void ResetAxis(Axis axis, float wrapped)
	{
		const uint32_t bit = 1u << axis;
		accumulated[axis] = 0.0;
		authoredWrapped[axis] = wrapped;
		initializedMask = std::isfinite(wrapped) ? initializedMask | bit : initializedMask & ~bit;
		movedMask &= ~bit;
	}

	void Assign(Axis axis, float& wrapped, float value)
	{
		wrapped = value;
		ResetAxis(axis, wrapped);
	}

	void Set(Axis axis, float& wrapped, float value)
	{
		// Retain the engine's existing wrapping behavior, including its float
		// precision and handling of large negative absolute values.
		Assign(axis, wrapped, std::fmod(value + 512.0f, 256.0f));
	}

	void Add(Axis axis, float& wrapped, float amount)
	{
		const float oldWrapped = wrapped;
		wrapped = std::fmod(wrapped + amount + 512.0f, 256.0f);
		const uint32_t bit = 1u << axis;
		if ((initializedMask & bit) == 0u) ResetAxis(axis, oldWrapped);
		const double next = accumulated[axis] + (double)amount;
		if (!std::isfinite(amount) || !std::isfinite(oldWrapped) ||
			!std::isfinite(wrapped) || !std::isfinite(next))
		{
			ResetAxis(axis, wrapped);
			return;
		}
		accumulated[axis] = next;
		authoredWrapped[axis] = wrapped;
		initializedMask |= bit;
		if (amount != 0.0f) movedMask |= bit;
	}

	double GetPresented(Axis axis, float wrapped) const
	{
		const uint32_t bit = 1u << axis;
		if ((initializedMask & movedMask & bit) == 0u || !std::isfinite(wrapped) ||
			!std::isfinite(authoredWrapped[axis]) || !std::isfinite(accumulated[axis])) return 0.0;
		double correction = (double)wrapped - authoredWrapped[axis];
		correction -= std::round(correction / 256.0) * 256.0;
		return accumulated[axis] + correction;
	}

	SectorPanMotionSample GetPlane(bool ceiling, float x, float y) const
	{
		const Axis axisX = ceiling ? CeilingX : FloorX;
		const Axis axisY = ceiling ? CeilingY : FloorY;
		return { GetPresented(axisX, x), GetPresented(axisY, y),
			(initializedMask & movedMask & (3u << axisX)) != 0u };
	}

	// Old saves leave totals/movedMask zero; new saves restore only those
	// durable fields. Reconstruct references from the restored authored pans.
	void SeedRestoredReferences(float floorX, float floorY, float ceilingX, float ceilingY)
	{
		const float wrapped[AxisCount] = { floorX, floorY, ceilingX, ceilingY };
		initializedMask = 0u;
		movedMask &= (1u << AxisCount) - 1u;
		for (uint32_t index = 0; index < AxisCount; ++index)
		{
			const Axis axis = (Axis)index;
			if (!std::isfinite(wrapped[index]) || !std::isfinite(accumulated[index]))
			{
				ResetAxis(axis, wrapped[index]);
				continue;
			}
			authoredWrapped[index] = wrapped[index];
			initializedMask |= 1u << index;
			if ((movedMask & (1u << index)) == 0u) accumulated[index] = 0.0;
		}
	}
};

static_assert(std::is_trivial<SectorPanMotionState>::value &&
	std::is_standard_layout<SectorPanMotionState>::value,
	"Sector pan state must support zero-initialized map arrays and sector copies.");
