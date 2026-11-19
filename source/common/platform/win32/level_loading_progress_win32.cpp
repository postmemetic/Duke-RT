#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "level_loading_progress_win32.h"
#include "../level_loading_progress.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

namespace LevelLoadingProgress
{
namespace
{
constexpr wchar_t PanelClass[] = L"DukeRTLevelProgress";
constexpr unsigned ShowAfterMs = 350;
constexpr unsigned RefreshMs = 50;

const wchar_t* Caption(Stage stage)
{
	switch (stage)
	{
	case Stage::World: return L"Preparing level geometry";
	case Stage::Actors: return L"Preparing voxel actors";
	case Stage::Materials: return L"Preparing materials";
	case Stage::FirstFrame: return L"Preparing first frame";
	default: return L"Loading level";
	}
}

class ProgressPanel
{
public:
	explicit ProgressPanel(HWND game) : owner(game), lastPresent(GetTickCount64()) {}
	~ProgressPanel() { Stop(); }

	bool Start()
	{
		stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!stopEvent) return false;
		thread = std::thread([this]() { Run(); });
		return true;
	}

	Stats Stop()
	{
		if (stopEvent) SetEvent(stopEvent);
		if (thread.joinable()) thread.join();
		if (stopEvent) { CloseHandle(stopEvent); stopEvent = nullptr; }
		return stats;
	}

	std::atomic<Stage> stage{Stage::Level};
	std::atomic<ULONGLONG> lastPresent;

private:
	static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
	{
		if (message == WM_NCCREATE)
			SetWindowLongPtrW(hwnd, GWLP_USERDATA,
				reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
		auto* self = reinterpret_cast<ProgressPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (self)
		{
			switch (message)
			{
			case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
			case WM_NCHITTEST: return HTTRANSPARENT;
			case WM_ERASEBKGND: return TRUE;
			case WM_PAINT: self->Paint(hwnd); return 0;
			case WM_CLOSE:
				self->closing = true;
				ShowWindow(hwnd, SW_HIDE);
				PostMessageW(self->owner, WM_CLOSE, 0, 0);
				return 0;
			}
		}
		return DefWindowProcW(hwnd, message, wp, lp);
	}

	void Paint(HWND hwnd)
	{
		PAINTSTRUCT paint = {};
		HDC dc = BeginPaint(hwnd, &paint);
		RECT area;
		GetClientRect(hwnd, &area);
		const ULONGLONG now = GetTickCount64();
		const auto brush = (HBRUSH)GetStockObject(DC_BRUSH);
		SetDCBrushColor(dc, RGB(32, 31, 29));
		FillRect(dc, &area, brush);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(244, 236, 217));
		auto old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
		RECT text = area;
		text.bottom -= 5;
		DrawTextW(dc, Caption(stage.load()), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dc, old);
		RECT track = {0, area.bottom - 4, area.right, area.bottom};
		SetDCBrushColor(dc, RGB(85, 70, 49));
		FillRect(dc, &track, brush);
		const int width = area.right / 4;
		const int range = area.right - width;
		const int position = int((now / 4) % (2 * std::max(1, range)));
		track.left = position > range ? 2 * range - position : position;
		track.right = track.left + width;
		SetDCBrushColor(dc, RGB(232, 186, 107));
		FillRect(dc, &track, brush);
		EndPaint(hwnd, &paint);
		if (previousPaint) stats.maxPaintGapMs = std::max(stats.maxPaintGapMs, now - previousPaint);
		previousPaint = now;
		++stats.paints;
	}

	void Refresh(HWND panel)
	{
		RECT client;
		// A separate, non-activating popup keeps its own input queue. It is
		// visible only above the foreground game while presentation is stalled.
		const auto presented = lastPresent.load();
		const bool visible = !closing && GetForegroundWindow() == owner &&
			IsWindowVisible(owner) && !IsIconic(owner) &&
			GetTickCount64() - presented >= ShowAfterMs &&
			GetClientRect(owner, &client) && client.right >= 110 && client.bottom >= 60;
		if (!visible)
		{
			if (IsWindowVisible(panel)) ShowWindow(panel, SW_HIDE);
			previousPaint = 0;
			return;
		}
		POINT origin = {};
		if (!ClientToScreen(owner, &origin)) return;
		const int width = std::min<LONG>(260, client.right - 16);
		const int height = 33;
		const bool wasVisible = IsWindowVisible(panel) != FALSE;
		SetWindowPos(panel, HWND_TOPMOST, origin.x + (client.right - width) / 2,
			origin.y + client.bottom - height - 12, width, height,
			SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
		if (!wasVisible && IsWindowVisible(panel)) ++stats.shows;
		InvalidateRect(panel, nullptr, FALSE);
		UpdateWindow(panel);
	}

	void Run()
	{
		HINSTANCE instance = GetModuleHandleW(nullptr);
		WNDCLASSW wc = {};
		wc.hInstance = instance;
		wc.lpfnWndProc = Proc;
		wc.lpszClassName = PanelClass;
		if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
		const HWND panel = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
			PanelClass, L"Loading level", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
		if (!panel) return;
		SetLayeredWindowAttributes(panel, 0, 250, LWA_ALPHA);
		auto nextRefresh = GetTickCount64() + RefreshMs;
		while (IsWindow(owner))
		{
			const auto now = GetTickCount64();
			const DWORD wait = nextRefresh > now ? DWORD(nextRefresh - now) : 0;
			const DWORD wake = MsgWaitForMultipleObjects(1, &stopEvent, FALSE, wait, QS_ALLINPUT);
			if (wake == WAIT_OBJECT_0 || wake == WAIT_FAILED) break;
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
				DispatchMessageW(&msg);
			if (GetTickCount64() >= nextRefresh)
			{
				Refresh(panel);
				nextRefresh = GetTickCount64() + RefreshMs;
			}
		}
		DestroyWindow(panel);
	}

	HWND owner;
	HANDLE stopEvent = nullptr;
	std::thread thread;
	// Only the panel thread writes these. Stop joins before reading stats.
	ULONGLONG previousPaint = 0;
	Stats stats;
	bool closing = false;
};

// All accesses to this owner are on the engine thread.
std::unique_ptr<ProgressPanel> current;
}

void BeginForWindow(HWND owner)
{
	End();
	if (!owner) return;
	try
	{
		auto panel = std::make_unique<ProgressPanel>(owner);
		if (panel->Start()) current = std::move(panel);
	}
	catch (...) {} // The in-game loading screen also works without a native panel.
}

void Presented() { if (current) current->lastPresent = GetTickCount64(); }
void SetStage(Stage stage) { if (current) current->stage = stage; }
Stats End()
{
	if (!current) return {};
	const Stats result = current->Stop();
	current.reset();
	return result;
}
}
