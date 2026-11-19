
#include "i_mainwindow.h"
#include "../loading_window.h"

#include "resource.h"
#include "startupinfo.h"
#include "gstrings.h"
#include "palentry.h"
#include "st_start.h"
#include "i_input.h"
#include "version.h"
#include "utf8.h"
#include "v_font.h"
#include "i_net.h"
#include "engineerrors.h"
#include "common/widgets/errorwindow.h"
#include "common/widgets/netstartwindow.h"
#include <richedit.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

MainWindow mainwindow;

void LoadingWindow::Service()
{
	// Never steal posted input or a pending WM_QUIT from the outer loop.
	// Checking for sent messages lets Windows service synchronous queries
	// even when the loading screen is between GPU submissions.
	static thread_local ULONGLONG nextUpdate = 0;
	const ULONGLONG now = GetTickCount64();
	if (now < nextUpdate) return;
	nextUpdate = now + 32;
	const HWND hwnd = mainwindow.GetHandle();
	if (!hwnd || GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) return;
	MSG message;
	PeekMessageW(&message, hwnd, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
	UpdateWindow(hwnd);
	LoadingWindow::ObservedService();
}

void MainWindow::Create(const FString& caption, int x, int y, int width, int height)
{
	static const WCHAR WinClassName[] = L"MainWindow";

	HINSTANCE hInstance = GetModuleHandle(0);

	WNDCLASS WndClass;
	WndClass.style = 0;
	WndClass.lpfnWndProc = LConProc;
	WndClass.cbClsExtra = 0;
	WndClass.cbWndExtra = 0;
	WndClass.hInstance = hInstance;
	WndClass.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
	WndClass.hCursor = LoadCursor(NULL, IDC_ARROW);
	WndClass.hbrBackground = CreateSolidBrush(RGB(0,0,0));
	WndClass.lpszMenuName = NULL;
	WndClass.lpszClassName = WinClassName;

	/* register this new class with Windows */
	if (!RegisterClass((LPWNDCLASS)&WndClass))
	{
		MessageBoxA(nullptr, "Could not register window class", "Fatal", MB_ICONEXCLAMATION | MB_OK);
		exit(-1);
	}

	std::wstring wcaption = caption.WideString();
	Window = CreateWindowExW(
		WS_EX_APPWINDOW,
		WinClassName,
		wcaption.c_str(),
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		x, y, width, height,
		(HWND)NULL,
		(HMENU)NULL,
		hInstance,
		NULL);

	if (!Window)
	{
		MessageBoxA(nullptr, "Unable to create main window", "Fatal", MB_ICONEXCLAMATION | MB_OK);
		exit(-1);
	}

	uint32_t bordercolor = RGB(51, 51, 51);
	uint32_t captioncolor = RGB(33, 33, 33);
	uint32_t textcolor = RGB(226, 223, 219);

	// Don't error check these as they only exist on Windows 11, and if they fail then that is OK.
	DwmSetWindowAttribute(Window, 34/*DWMWA_BORDER_COLOR*/, &bordercolor, sizeof(uint32_t));
	DwmSetWindowAttribute(Window, 35/*DWMWA_CAPTION_COLOR*/, &captioncolor, sizeof(uint32_t));
	DwmSetWindowAttribute(Window, 36/*DWMWA_TEXT_COLOR*/, &textcolor, sizeof(uint32_t));
}

// Sets the main WndProc, hides all the child windows, and starts up in-game input.
void MainWindow::ShowGameView()
{
	if (StartupHidden) return;
	if (GetWindowLongPtr(Window, GWLP_USERDATA) == 0)
	{
		SetWindowLongPtr(Window, GWLP_USERDATA, 1);
		SetWindowLongPtr(Window, GWLP_WNDPROC, (LONG_PTR)WndProc);
		I_InitInput(Window);
	}
}

void MainWindow::BeginStartupPresentation(bool hideGameWindow)
{
	StartupHidden = hideGameWindow;
	StartupShowCommand = SW_SHOW;
	StartupMaximizePending = false;
	StartupForegroundWindow = GetForegroundWindow();
	LASTINPUTINFO lastInput = { sizeof(lastInput) };
	StartupLastInputValid = GetLastInputInfo(&lastInput) != FALSE;
	StartupLastInputTime = lastInput.dwTime;
	if (StartupHidden) ShowWindow(Window, SW_HIDE);
}

void MainWindow::ShowGameWindow(int command)
{
	if (StartupHidden)
	{
		StartupShowCommand = command;
		return;
	}
	if (command == SW_SHOWNORMAL || command == SW_RESTORE)
		StartupMaximizePending = false;
	ShowWindow(Window, command);
}

void MainWindow::RevealStartupGameView(bool splashWasShown)
{
	if (!StartupHidden) return;
	const HWND foregroundWindow = GetForegroundWindow();
	DWORD foregroundProcess = 0;
	GetWindowThreadProcessId(foregroundWindow, &foregroundProcess);
	LASTINPUTINFO lastInput = { sizeof(lastInput) };
	const bool launchUnchanged = !splashWasShown && foregroundWindow == StartupForegroundWindow &&
		StartupLastInputValid && GetLastInputInfo(&lastInput) && lastInput.dwTime == StartupLastInputTime;
	const bool activate = foregroundProcess == GetCurrentProcessId() || launchUnchanged;
	StartupMaximizePending = StartupShowCommand == SW_MAXIMIZE;
	StartupHidden = false;
	ShowGameView();
	// Show without activation first, and let Windows decide whether our request
	// for foreground is allowed. Maximizing would itself activate the window.
	ShowWindow(Window, StartupShowCommand == SW_SHOWNORMAL || StartupShowCommand == SW_RESTORE
		? SW_SHOWNOACTIVATE : SW_SHOWNA);
	if (activate) SetForegroundWindow(Window);
	if (GetForegroundWindow() == Window)
	{
		OnGameWindowActivated();
		SetFocus(Window);
	}
	// The startup window procedure did not maintain activation state, and a
	// nonactivating reveal need not generate a new WM_ACTIVATEAPP notification.
	// Refresh the normal input, sound, and scheduling policy after installing it.
	SendMessageW(Window, WM_ACTIVATEAPP, GetForegroundWindow() == Window ? TRUE : FALSE, 0);
}

void MainWindow::OnGameWindowActivated()
{
	if (StartupHidden || !StartupMaximizePending || GetForegroundWindow() != Window) return;
	// Clear before ShowWindow, which can synchronously reenter the window proc.
	StartupMaximizePending = false;
	if ((GetWindowLong(Window, GWL_STYLE) & WS_POPUP) == 0)
		ShowWindow(Window, SW_MAXIMIZE);
}

void MainWindow::ResetStartupPresentation()
{
	StartupHidden = false;
	StartupShowCommand = SW_SHOW;
	StartupMaximizePending = false;
	StartupForegroundWindow = nullptr;
	StartupLastInputTime = 0;
	StartupLastInputValid = false;
}

// Returns the main window to its startup state.
void MainWindow::RestoreConView()
{
	I_ShutdownInput();		// Make sure the mouse pointer is available.
	ShowWindow(Window, SW_HIDE);

	// Make sure the progress bar isn't visible.
	DeleteStartupScreen();
}

// Shows an error message, preferably in the main window, but it can use a normal message box too.
void MainWindow::ShowErrorPane(const char* text)
{
	if (StartWindow)	// Ensure that the network pane is hidden.
	{
		I_NetDone();
	}

	// PrintStr(text);

	size_t totalsize = 0;
	for (const FString& line : bufferedConsoleStuff)
		totalsize += line.Len();

	std::string alltext;
	alltext.reserve(totalsize);
	for (const FString& line : bufferedConsoleStuff)
		alltext.append(line.GetChars(), line.Len());

	restartrequest = ErrorWindow::ExecModal(text, alltext);
}

void MainWindow::ShowNetStartPane(const char* message, int maxpos)
{
	NetStartWindow::ShowNetStartPane(message, maxpos);
}

void MainWindow::HideNetStartPane()
{
	NetStartWindow::HideNetStartPane();
}

void MainWindow::CloseNetStartPane()
{
	NetStartWindow::NetClose();
}

void MainWindow::SetNetStartProgress(int pos)
{
	NetStartWindow::SetNetStartProgress(pos);
}

bool MainWindow::RunMessageLoop(bool (*timer_callback)(void*), void* userdata)
{
	return NetStartWindow::RunMessageLoop(timer_callback, userdata);
}

bool MainWindow::CheckForRestart()
{
	bool result = restartrequest;
	restartrequest = false;
	return result;
}

// The main window's WndProc during startup. During gameplay, the WndProc in i_input.cpp is used instead.
LRESULT MainWindow::LConProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

void MainWindow::PrintStr(const char* cp)
{
	bufferedConsoleStuff.Push(cp);
}

void MainWindow::GetLog(std::function<bool(const void* data, uint32_t size, uint32_t& written)> writeData)
{
	for (const FString& line : bufferedConsoleStuff)
	{
		size_t pos = 0;
		size_t len = line.Len();
		while (pos < len)
		{
			uint32_t size = (uint32_t)std::min(len - pos, 0x0fffffffULL);
			uint32_t written = 0;
			if (!writeData(&line[pos], size, written))
				return;
			pos += written;
		}
	}
}

// each platform has its own specific version of this function.
void MainWindow::SetWindowTitle(const char* caption)
{
	std::wstring widecaption;
	if (!caption)
	{
		FStringf default_caption("" GAMENAME " %s (%s)", GetVersionString(), GetGitTime());
		widecaption = default_caption.WideString();
	}
	else
	{
		widecaption = WideString(caption);
	}
	SetWindowText(Window, widecaption.c_str());
}
