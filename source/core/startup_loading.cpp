#include "startup_loading.h"
#include "v_video.h"
#include "printf.h"

#ifdef _WIN32
#include "i_mainwindow.h"
#endif

StartupLoadingScreen* StartupLoadingScreen::sCurrent = nullptr;

StartupLoadingScreen::StartupLoadingScreen(const char* title, const char* fontId, bool showWindow, bool trace)
	: mSession(title, fontId, showWindow, trace)
{
	sCurrent = this;
#ifdef _WIN32
	// If the auxiliary window could not be created, retain ordinary startup.
	mainwindow.BeginStartupPresentation(StartupProgress::IsWindowAvailable());
#endif
}

StartupLoadingScreen::~StartupLoadingScreen()
{
	sCurrent = nullptr;
	// Session closes its own window. Failure/cancel suppression deliberately
	// survives this object until the framebuffer destructor has finished.
}

void StartupLoadingScreen::SetStage(const char* label)
{
	StartupProgress::CheckCancellation();
#ifdef _WIN32
	if (sCurrent != nullptr && mainwindow.IsStartupHidden() && !StartupProgress::IsWindowAvailable())
	{
		// Losing the optional host must not leave startup with no visible window.
		// This runs at an engine-thread boundary, never from the native UI thread.
		Printf("Startup progress window closed unexpectedly; restoring game-window presentation.\n");
		mainwindow.RevealStartupGameView(StartupProgress::WasWindowShown());
	}
#endif
	StartupProgress::SetStage(label);
}

void StartupLoadingScreen::BeforeFirstPresent()
{
	if (sCurrent == nullptr || sCurrent->mComplete || sCurrent->mRevealed) return;
	StartupProgress::CheckCancellation();
	StartupProgress::SetStage("Opening game");
#ifdef _WIN32
	mainwindow.RevealStartupGameView(StartupProgress::WasWindowShown());
#endif
	sCurrent->mRevealed = true;
}

void StartupLoadingScreen::AfterFirstPresent()
{
	if (sCurrent == nullptr || sCurrent->mComplete || !sCurrent->mRevealed) return;
	StartupProgress::CheckCancellation();
	if (screen == nullptr || !screen->HasPresentedFrame()) return;
	sCurrent->mSession.Complete();
	sCurrent->mComplete = true;
	sCurrent = nullptr;
}

void StartupLoadingScreen::ResetAfterGraphicsShutdown()
{
#ifdef _WIN32
	mainwindow.ResetStartupPresentation();
#endif
}
