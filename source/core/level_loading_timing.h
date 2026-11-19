#pragma once

#include <algorithm>
#include <cstdint>

struct LevelLoadingTiming
{
	bool active = false;
	double start = 0;
	double previousEventPump = 0;
	double previousNativeService = 0;
	double maxNativeServiceGap = 0;
	uint32_t nativeServices = 0;
	double previousScreenUpdate = 0;
	double maxEventPumpGap = 0;
	double maxScreenUpdateGap = 0;
	double maxPreloadTick = 0;
	uint32_t screenUpdates = 0;

	void Begin(bool trace, double now)
	{
		*this = {};
		active = trace;
		start = previousEventPump = previousNativeService = previousScreenUpdate = now;
	}

	void NativeService(double now)
	{
		maxNativeServiceGap = std::max(maxNativeServiceGap, now - previousNativeService);
		previousNativeService = now;
		++nativeServices;
	}

	void EventPump(double now)
	{
		maxEventPumpGap = std::max(maxEventPumpGap, now - previousEventPump);
		previousEventPump = now;
	}

	void ScreenUpdate(double now)
	{
		maxScreenUpdateGap = std::max(maxScreenUpdateGap, now - previousScreenUpdate);
		previousScreenUpdate = now;
		++screenUpdates;
	}
};
