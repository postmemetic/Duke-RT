#pragma once

#include "common/platform/startup_progress.h"

// Owns the initial loading presentation only. Game data and renderer resources
// keep their normal initialization order and thread ownership.
class StartupLoadingScreen
{
public:
	StartupLoadingScreen(const char* title, const char* fontId, bool showWindow, bool trace);
	~StartupLoadingScreen();
	StartupLoadingScreen(const StartupLoadingScreen&) = delete;
	StartupLoadingScreen& operator=(const StartupLoadingScreen&) = delete;

	static void BeforeFirstPresent();
	static void AfterFirstPresent();
	static void ResetAfterGraphicsShutdown();
	static void SetStage(const char* label);

private:
	StartupProgress::Session mSession;
	bool mRevealed = false;
	bool mComplete = false;
	static StartupLoadingScreen* sCurrent;
};
