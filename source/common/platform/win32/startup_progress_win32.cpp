#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "startup_progress_win32.h"
#include "../startup_progress.h"
#include "../startup_font.h"
#include "resource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>

namespace StartupProgress
{
namespace
{
constexpr wchar_t WindowClass[] = L"DukeRTStartupProgress";
constexpr UINT StopMessage = WM_APP + 37;
// Keep the engine's older SDK target while handling per-monitor DPI messages
// delivered by newer Windows versions.
constexpr UINT DpiChangedMessage = 0x02e0;
constexpr UINT_PTR RefreshTimer = 1;
constexpr int DesignWidth = 620;
constexpr int DesignHeight = 310;

std::wstring WideText(const char* text)
{
	if (!text || !*text)
		return {};
	const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	if (count < 1)
		return {};
	std::wstring result(count, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text, -1, &result[0], count);
	result.resize(count - 1);
	return result;
}

UINT WindowDpi(HWND window)
{
	using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
	auto getDpi = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
	if (getDpi)
		return getDpi(window);
	HDC dc = GetDC(window);
	const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
	if (dc)
		ReleaseDC(window, dc);
	return dpi > 0 ? (UINT)dpi : 96;
}
}

struct WindowState
{
	std::mutex readyMutex;
	std::condition_variable readyCondition;
	bool ready = false;
	std::atomic<bool> stop{ false };
	std::atomic<bool> available{ false };
	std::atomic<HWND> window{ nullptr };
	std::wstring title;
	std::string fontId;
	const StartupFont::Font* font = nullptr;
	std::atomic<bool> shown{ false };
	bool closeAcknowledged = false;
	ULONGLONG startTicks = GetTickCount64();
	ULONGLONG lastPaint = 0;
	WindowPaintStats paintStats;
	HWND launchForeground = nullptr;
	DWORD launchInputTick = 0;
	bool launchInputKnown = false;
	UINT dpi = 96;
	HDC bitmapDC = nullptr;
	HBITMAP bitmap = nullptr;
	HGDIOBJ oldBitmap = nullptr;
	uint32_t* pixels = nullptr;
	int bitmapWidth = 0;
	int bitmapHeight = 0;
	HFONT titleFont = nullptr;
	HFONT stageFont = nullptr;
	HFONT bodyFont = nullptr;

	void SignalReady()
	{
		{
			std::lock_guard<std::mutex> lock(readyMutex);
			ready = true;
		}
		readyCondition.notify_one();
	}

	void ReleaseBitmap()
	{
		if (bitmapDC && oldBitmap)
			SelectObject(bitmapDC, oldBitmap);
		if (bitmap)
			DeleteObject(bitmap);
		if (bitmapDC)
			DeleteDC(bitmapDC);
		bitmapDC = nullptr;
		bitmap = nullptr;
		oldBitmap = nullptr;
		pixels = nullptr;
		bitmapWidth = bitmapHeight = 0;
	}

	void ReleaseFonts()
	{
		if (titleFont) DeleteObject(titleFont);
		if (stageFont) DeleteObject(stageFont);
		if (bodyFont) DeleteObject(bodyFont);
		titleFont = stageFont = bodyFont = nullptr;
	}

	void SetDpi(UINT value)
	{
		dpi = value ? value : 96;
		ReleaseFonts();
		auto makeFont = [this](int height, int weight)
		{
			return CreateFontW(-MulDiv(height, (int)dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
				DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
		};
		titleFont = makeFont(27, FW_SEMIBOLD);
		stageFont = makeFont(21, FW_SEMIBOLD);
		bodyFont = makeFont(15, FW_NORMAL);
	}

	bool EnsureBitmap(HDC dc, int width, int height)
	{
		if (pixels && bitmapWidth == width && bitmapHeight == height)
			return true;
		ReleaseBitmap();
		if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
			return false;
		BITMAPINFO info = {};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		bitmapDC = CreateCompatibleDC(dc);
		bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
		if (!bitmapDC || !bitmap || !pixels)
		{
			ReleaseBitmap();
			return false;
		}
		oldBitmap = SelectObject(bitmapDC, bitmap);
		bitmapWidth = width;
		bitmapHeight = height;
		return true;
	}

	int Scale(int value) const { return MulDiv(value, (int)dpi, 96); }

	void Fill(int x, int y, int width, int height, uint32_t color)
	{
		const int right = std::min(x + width, bitmapWidth);
		const int bottom = std::min(y + height, bitmapHeight);
		for (int row = std::max(y, 0); row < bottom; ++row)
			for (int col = std::max(x, 0); col < right; ++col)
				pixels[row * bitmapWidth + col] = color | 0xff000000u;
	}

	void Text(const char* text, int y, int height, uint32_t color)
	{
		if (!text || !*text)
			return;
		const int width = font ? StartupFont::MeasureText(*font, text) : -1;
		const int availableWidth = std::max(1, bitmapWidth - Scale(48));
		if (font && width > 0 && font->lineHeight > 0)
		{
			double scale = (double)Scale(height) / font->lineHeight;
			scale = std::min(scale, (double)availableWidth / width);
			// Very long/unfamiliar labels use the native font with ellipsis.
			if (scale >= 0.6)
			{
				double pen = (bitmapWidth - width * scale) * 0.5;
				for (const unsigned char* c = (const unsigned char*)text; *c; ++c)
				{
					StartupFont::Glyph glyph = {};
					if (!StartupFont::GetGlyph(*font, *c, glyph))
						continue;
					const int left = (int)std::floor(pen - glyph.leftOffset * scale);
					const int top = Scale(y) - (int)std::round(glyph.topOffset * scale);
					const int drawWidth = (int)std::ceil(glyph.width * scale);
					const int drawHeight = (int)std::ceil(glyph.height * scale);
					for (int dy = 0; dy < drawHeight; ++dy)
					{
						const int py = top + dy;
						if (py < 0 || py >= bitmapHeight) continue;
						const int sy = std::min((int)(dy / scale), glyph.height - 1);
						for (int dx = 0; dx < drawWidth; ++dx)
						{
							const int px = left + dx;
							if (px < 0 || px >= bitmapWidth) continue;
							const int sx = std::min((int)(dx / scale), glyph.width - 1);
							const unsigned shade = glyph.pixels[sy * glyph.width + sx];
							if (!shade) continue;
							const uint32_t r = ((color >> 16) & 255) * shade / 255;
							const uint32_t g = ((color >> 8) & 255) * shade / 255;
							const uint32_t b = (color & 255) * shade / 255;
							pixels[py * bitmapWidth + px] = 0xff000000u | (r << 16) | (g << 8) | b;
						}
					}
					pen += glyph.advance * scale;
				}
				return;
			}
		}
		const std::wstring wide = WideText(text);
		HFONT selected = height >= 25 ? titleFont : height >= 20 ? stageFont : bodyFont;
		HGDIOBJ old = SelectObject(bitmapDC, selected ? selected : GetStockObject(DEFAULT_GUI_FONT));
		SetBkMode(bitmapDC, TRANSPARENT);
		SetTextColor(bitmapDC, RGB((color >> 16) & 255, (color >> 8) & 255, color & 255));
		RECT rect = { Scale(24), Scale(y), bitmapWidth - Scale(24), Scale(y + height + 8) };
		DrawTextW(bitmapDC, wide.c_str(), (int)wide.size(), &rect, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		SelectObject(bitmapDC, old);
		// GDI and direct DIB writes share this buffer. Flush before the next
		// CPU write so queued text operations cannot overwrite a newer frame.
		GdiFlush();
	}

	void Paint(HWND hwnd)
	{
		const Snapshot snapshot = GetSnapshot();
		struct PaintScope
		{
			HWND window;
			PAINTSTRUCT paint = {};
			HDC dc;
			explicit PaintScope(HWND hwnd) : window(hwnd), dc(BeginPaint(hwnd, &paint)) {}
			~PaintScope() { EndPaint(window, &paint); }
		} scope(hwnd);
		HDC dc = scope.dc;
		RECT client = {};
		GetClientRect(hwnd, &client);
		if (EnsureBitmap(dc, client.right, client.bottom))
		{
			std::fill(pixels, pixels + (size_t)bitmapWidth * bitmapHeight, 0xff000000u);
			const bool closing = snapshot.cancellationRequested;
			Text(snapshot.title.c_str(), 34, 30, 0xe7bb6c);
			Text(closing ? "Closing" : snapshot.stage.c_str(), 92, 23, 0xe9e1d3);
			Text(closing ? "Waiting for the current task to finish..." : snapshot.task.c_str(), 129, 17, 0xb9b1a5);
			const int x = Scale(54);
			const int y = Scale(177);
			const int width = std::max(1, bitmapWidth - 2 * x);
			const int height = Scale(14);
			const int inset = std::max(1, Scale(2));
			Fill(x, y, width, height, 0x685633);
			Fill(x + inset, y + inset, width - 2 * inset, height - 2 * inset, 0x15110b);
			const int innerWidth = std::max(0, width - 2 * inset);
			if (snapshot.total)
			{
				const double fraction = std::min(1.0, (double)snapshot.completed / snapshot.total);
				Fill(x + inset, y + inset, (int)(innerWidth * fraction), height - 2 * inset, 0xc69748);
			}
			else if (innerWidth > 0)
			{
				const int segment = std::max(1, innerWidth / 5);
				const double phase = std::fmod(snapshot.elapsedSeconds * 0.55, 2.0);
				const double travel = phase <= 1.0 ? phase : 2.0 - phase;
				Fill(x + inset + (int)((innerWidth - segment) * travel), y + inset, segment, height - 2 * inset, 0xc69748);
			}
			char count[96] = {};
			if (snapshot.total)
				snprintf(count, sizeof(count), "Tasks completed: %u / %u", snapshot.completed, snapshot.total);
			Text(count, 203, 14, 0xb9b1a5);
			const auto seconds = (unsigned long long)std::max(0.0, snapshot.elapsedSeconds);
			char elapsed[96];
			snprintf(elapsed, sizeof(elapsed), "Elapsed: %02llu:%02llu", seconds / 60, seconds % 60);
			Text(elapsed, 231, 14, 0xb9b1a5);
			const int dot = (int)(snapshot.elapsedSeconds * 6.0) % 6;
			for (int i = 0; i < 6; ++i)
				Fill(bitmapWidth / 2 - Scale(27) + Scale(i * 10), Scale(255), Scale(4), Scale(4), i == dot ? 0xe7bb6c : 0x483c27);
			Text("This may take longer after an update.", 280, 13, 0x91887a);
			BitBlt(dc, 0, 0, bitmapWidth, bitmapHeight, bitmapDC, 0, 0, SRCCOPY);
			GdiFlush();
		}
		else
		{
			FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, RGB(235, 225, 205));
			const std::wstring text = WideText(snapshot.stage.c_str());
			DrawTextW(dc, text.c_str(), (int)text.size(), &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		}
	}

	// This path deliberately avoids snapshots, std::strings and new GDI
	// objects. A failed optional font/buffer allocation must not freeze the
	// window or remove the only startup UI while a driver call is blocked.
	void PaintFallback(HWND hwnd) noexcept
	{
		InvalidateRect(hwnd, nullptr, FALSE);
		PAINTSTRUCT paint = {};
		HDC dc = BeginPaint(hwnd, &paint);
		RECT client = {};
		GetClientRect(hwnd, &client);
		FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
		HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(235, 225, 205));
		RECT textRect = client;
		textRect.top = client.bottom / 3;
		const wchar_t* text = closeAcknowledged ? L"Closing: waiting for the current task to finish..." : L"Preparing graphics. Please wait...";
		DrawTextW(dc, text, -1, &textRect, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
		const ULONGLONG elapsedMs = GetTickCount64() - startTicks;
		wchar_t elapsed[64];
		swprintf_s(elapsed, L"Elapsed: %02llu:%02llu", elapsedMs / 60000, (elapsedMs / 1000) % 60);
		textRect.top += Scale(34);
		DrawTextW(dc, elapsed, -1, &textRect, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
		const int dot = (int)((elapsedMs / 160) % 6);
		for (int i = 0; i < 6; ++i)
		{
			const int x = client.right / 2 - Scale(27) + Scale(i * 10);
			RECT rect = { x, textRect.top + Scale(35), x + Scale(4), textRect.top + Scale(39) };
			FillRect(dc, &rect, (HBRUSH)GetStockObject(i == dot ? LTGRAY_BRUSH : DKGRAY_BRUSH));
		}
		SelectObject(dc, old);
		EndPaint(hwnd, &paint);
	}
};

namespace
{
LRESULT CALLBACK ProgressWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	auto state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (message == WM_NCCREATE)
	{
		state = static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
	}
	if (!state)
		return DefWindowProcW(hwnd, message, wparam, lparam);
	try
	{
		switch (message)
		{
		case WM_ERASEBKGND: return 1;
		case WM_PAINT:
			try { state->Paint(hwnd); }
			catch (...) { state->PaintFallback(hwnd); }
			{
				const auto now = GetTickCount64();
				if (state->lastPaint)
					state->paintStats.longestGapMs = std::max(state->paintStats.longestGapMs, now - state->lastPaint);
				state->lastPaint = now;
				++state->paintStats.paints;
			}
			return 0;
		case WM_CLOSE:
			state->closeAcknowledged = true;
			RequestCancellation();
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		case WM_TIMER:
			if (wparam != RefreshTimer) break;
			if (state->stop.load())
				DestroyWindow(hwnd);
			else
			{
				if (!state->shown.load() && GetTickCount64() - state->startTicks >= 250)
				{
					const HWND foreground = GetForegroundWindow();
					DWORD foregroundProcess = 0;
					if (foreground)
						GetWindowThreadProcessId(foreground, &foregroundProcess);
					LASTINPUTINFO input = {};
					input.cbSize = sizeof(input);
					const bool unchangedInput = state->launchInputKnown && GetLastInputInfo(&input) && input.dwTime == state->launchInputTick;
					const bool mayActivate = foregroundProcess == GetCurrentProcessId() ||
						(foreground == state->launchForeground && unchangedInput);
					// Respect a user who switched tasks during the display delay.
					ShowWindow(hwnd, mayActivate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
					state->shown.store(true);
				}
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			return 0;
		case StopMessage:
			DestroyWindow(hwnd);
			return 0;
		case DpiChangedMessage:
		{
			state->SetDpi(HIWORD(wparam));
			const auto rect = reinterpret_cast<const RECT*>(lparam);
			SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOACTIVATE | SWP_NOZORDER);
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		case WM_DESTROY:
			state->available.store(false);
			KillTimer(hwnd, RefreshTimer);
			PostQuitMessage(0);
			return 0;
		case WM_NCDESTROY:
			state->window.store(nullptr);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
			break;
		}
	}
	catch (...)
	{
		// No exception may cross the native callback boundary. Failure of
		// optional presentation must not call the engine's error UI here.
		state->stop.store(true);
		state->available.store(false);
		PostMessageW(hwnd, StopMessage, 0, 0);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

void RunWindow(const std::shared_ptr<WindowState>& state) noexcept
{
	try
	{
		state->font = StartupFont::Find(state->fontId.c_str());
		const HINSTANCE instance = GetModuleHandleW(nullptr);
		WNDCLASSEXW windowClass = {};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.lpfnWndProc = ProgressWindowProc;
		windowClass.hInstance = instance;
		windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_ICON1));
		windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		windowClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
		windowClass.lpszClassName = WindowClass;
		const bool registered = RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
		if (registered && !state->stop.load())
		{
			const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
			HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, WindowClass, state->title.c_str(), style,
				CW_USEDEFAULT, CW_USEDEFAULT, DesignWidth, DesignHeight, nullptr, nullptr, instance, state.get());
			if (hwnd)
			{
				state->window.store(hwnd);
				state->SetDpi(WindowDpi(hwnd));
				RECT bounds = { 0, 0, state->Scale(DesignWidth), state->Scale(DesignHeight) };
				AdjustWindowRectEx(&bounds, style, FALSE, WS_EX_APPWINDOW);
				POINT cursor = {};
				GetCursorPos(&cursor);
				MONITORINFO monitor = {};
				monitor.cbSize = sizeof(monitor);
				GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
				const int width = bounds.right - bounds.left;
				const int height = bounds.bottom - bounds.top;
				SetWindowPos(hwnd, nullptr,
					monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
					monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2,
					width, height, SWP_NOACTIVATE | SWP_NOZORDER);
				if (!state->stop.load() && SetTimer(hwnd, RefreshTimer, 50, nullptr))
				{
					state->available.store(true);
					state->SignalReady();
					MSG message = {};
					while (GetMessageW(&message, nullptr, 0, 0) > 0)
					{
						TranslateMessage(&message);
						DispatchMessageW(&message);
					}
				}
			}
		}
	}
	catch (...) {}
	state->available.store(false);
	if (HWND hwnd = state->window.load())
		DestroyWindow(hwnd);
	state->ReleaseBitmap();
	state->ReleaseFonts();
	state->SignalReady();
}
}

WindowHost::WindowHost() = default;
WindowHost::~WindowHost() { Close(); }

bool WindowHost::Start(const char* title, const char* fontId)
{
	mState = std::make_shared<WindowState>();
	mState->title = WideText(title ? title : "Starting");
	mState->fontId = fontId ? fontId : "";
	mState->launchForeground = GetForegroundWindow();
	LASTINPUTINFO input = {};
	input.cbSize = sizeof(input);
	mState->launchInputKnown = GetLastInputInfo(&input) != FALSE;
	mState->launchInputTick = input.dwTime;
	mThread = std::thread(RunWindow, mState);
	std::unique_lock<std::mutex> lock(mState->readyMutex);
	if (!mState->readyCondition.wait_for(lock, std::chrono::seconds(2), [this]() { return mState->ready; }))
	{
		// A late host must never appear after the engine selected fallback
		// presentation. Its state is retained until orderly session cleanup.
		mState->stop.store(true);
		return false;
	}
	return mState->available.load();
}

bool WindowHost::IsAvailable() const noexcept
{
	return mState && !mState->stop.load() && mState->available.load();
}

bool WindowHost::WasShown() const noexcept
{
	return mState && mState->shown.load();
}

WindowPaintStats WindowHost::Close() noexcept
{
	if (mState)
	{
		mState->stop.store(true);
		if (HWND hwnd = mState->window.load())
			PostMessageW(hwnd, StopMessage, 0, 0);
	}
	if (mThread.joinable())
		mThread.join();
	return mState ? mState->paintStats : WindowPaintStats{};
}
}
