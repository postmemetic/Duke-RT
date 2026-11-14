#pragma once

#include <memory>
#include <thread>

namespace StartupProgress
{
struct WindowState;

// A separate native window/message loop, with no dependency on game HWNDs.
// The host never calls engine rendering, input, logging or resource services.
class WindowHost
{
public:
	WindowHost();
	~WindowHost();
	WindowHost(const WindowHost&) = delete;
	WindowHost& operator=(const WindowHost&) = delete;
	bool Start(const char* title, const char* fontId);
	bool IsAvailable() const noexcept;
	bool WasShown() const noexcept;
	void Close() noexcept;

private:
	std::shared_ptr<WindowState> mState;
	std::thread mThread;
};
}
