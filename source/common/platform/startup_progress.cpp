#include "startup_progress.h"
#include "engineerrors.h"
#include "printf.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>

#ifdef _WIN32
#include "win32/startup_progress_win32.h"
#endif

namespace StartupProgress
{
namespace
{
using Clock = std::chrono::steady_clock;
std::mutex StateMutex;
Snapshot State;
Clock::time_point StartTime;
Clock::time_point StageStartTime;
uint64_t Generation = 0;
uint64_t OperationSequence = 0;
bool Trace = false;
bool WindowWasShown = false;
std::atomic<bool> CancelRequested{ false };
#ifdef _WIN32
std::shared_ptr<WindowHost> Host;
#endif

double NowSeconds()
{
	return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

double ElapsedSeconds()
{
	return std::chrono::duration<double>(Clock::now() - StartTime).count();
}
}

Session::Session(const char* title, const char* fontId, bool showWindow, bool trace)
{
	{
		std::lock_guard<std::mutex> lock(StateMutex);
		// A nested startup must not replace a live host or its cancellation.
		if (State.active)
			return;
		State = {};
		State.title = title ? title : "Starting";
		State.stage = "Starting";
		State.active = true;
		StartTime = StageStartTime = Clock::now();
		CancelRequested.store(false);
		Trace = trace;
		WindowWasShown = false;
		OperationSequence = 0;
		mGeneration = ++Generation;
	}
	if (trace)
	{
		Printf("STARTUP session begin: title=\"%s\" font=%s window=%u\n", title ? title : "", fontId ? fontId : "fallback", showWindow ? 1u : 0u);
		Printf("STARTUP stage begin: name=\"Starting\" tasks=0\n");
	}
#ifdef _WIN32
	if (showWindow)
	{
		try
		{
			auto host = std::make_shared<WindowHost>();
			const bool available = host->Start(title, fontId);
			{
				std::lock_guard<std::mutex> lock(StateMutex);
				Host = std::move(host);
			}
			if (!available)
				Printf("Startup progress window unavailable; continuing with normal game-window presentation.\n");
		}
		catch (...)
		{
			// Presentation is optional. A failed thread/window allocation must
			// not turn an otherwise usable game startup into a fatal error.
			Printf("Startup progress window unavailable; continuing with normal game-window presentation.\n");
		}
	}
#else
	(void)fontId;
	(void)showWindow;
#endif
}

Session::~Session()
{
	Close(false);
}

void Session::Complete()
{
	CheckCancellation();
	Close(true);
}

void Session::Close(bool completed) noexcept
{
	bool trace = false;
	bool failed = false;
	bool cancelled = false;
	double elapsed = 0.0;
	double stageElapsed = 0.0;
	std::string stage;
#ifdef _WIN32
	std::shared_ptr<WindowHost> host;
#endif
	{
		std::lock_guard<std::mutex> lock(StateMutex);
		if (mGeneration == 0 || mGeneration != Generation || !State.active)
			return;
		cancelled = CancelRequested.load();
		failed = State.failed;
		State.succeeded = completed && !cancelled && !failed;
		State.active = false;
		State.taskRunning = false;
		State.elapsedSeconds = elapsed = ElapsedSeconds();
		trace = Trace;
		if (trace)
		{
			stage.swap(State.stage);
			stageElapsed = std::chrono::duration<double>(Clock::now() - StageStartTime).count();
		}
		Trace = false;
		mGeneration = 0;
#ifdef _WIN32
		host = std::move(Host);
#endif
	}
#ifdef _WIN32
	if (host)
	{
		host->Close();
		std::lock_guard<std::mutex> lock(StateMutex);
		WindowWasShown = host->WasShown();
	}
#endif
	if (trace)
	{
		try
		{
			Printf("STARTUP stage end: name=\"%s\" elapsed_ms=%.3f\n", stage.c_str(), stageElapsed * 1000.0);
			Printf("STARTUP session end: status=%s elapsed_ms=%.3f\n",
				cancelled ? "cancelled" : completed ? (failed ? "completed-with-failures" : "ready") : "aborted", elapsed * 1000.0);
		}
		catch (...) {}
	}
}

bool IsActive()
{
	std::lock_guard<std::mutex> lock(StateMutex);
	return State.active;
}

bool IsTracing()
{
	std::lock_guard<std::mutex> lock(StateMutex);
	return State.active && Trace;
}

bool IsWindowAvailable()
{
#ifdef _WIN32
	std::shared_ptr<WindowHost> host;
	{
		std::lock_guard<std::mutex> lock(StateMutex);
		host = Host;
	}
	return host && host->IsAvailable();
#else
	return false;
#endif
}

bool IsCancellationRequested()
{
	return IsActive() && CancelRequested.load();
}

bool WasWindowShown()
{
#ifdef _WIN32
	std::shared_ptr<WindowHost> host;
	bool wasShown;
	{
		std::lock_guard<std::mutex> lock(StateMutex);
		host = Host;
		wasShown = WindowWasShown;
	}
	return wasShown || (host && host->WasShown());
#else
	return false;
#endif
}

void RequestCancellation() noexcept
{
	CancelRequested.store(true);
}

void CheckCancellation()
{
	if (IsCancellationRequested())
		throw CExitEvent(0);
}

void SetStage(const char* stage)
{
	BeginTasks(stage, 0);
}

void BeginTasks(const char* stage, uint32_t total)
{
	bool trace = false;
	std::string previousStage;
	double previousElapsed = 0.0;
	{
		std::lock_guard<std::mutex> lock(StateMutex);
		if (!State.active)
			return;
		trace = Trace;
		if (trace)
		{
			previousStage = State.stage;
			previousElapsed = std::chrono::duration<double>(Clock::now() - StageStartTime).count();
		}
		State.stage = stage ? stage : "Starting";
		State.task.clear();
		State.completed = 0;
		State.total = total;
		State.taskRunning = false;
		StageStartTime = Clock::now();
	}
	if (trace)
	{
		Printf("STARTUP stage end: name=\"%s\" elapsed_ms=%.3f\n", previousStage.c_str(), previousElapsed * 1000.0);
		Printf("STARTUP stage begin: name=\"%s\" tasks=%u\n", stage ? stage : "Starting", total);
	}
}

void BeginTask(const char* humanLabel)
{
	std::lock_guard<std::mutex> lock(StateMutex);
	if (!State.active)
		return;
	State.task = humanLabel ? humanLabel : "Preparing";
	State.taskRunning = true;
}

void EndTask(bool success)
{
	std::lock_guard<std::mutex> lock(StateMutex);
	if (!State.active || !State.taskRunning)
		return;
	State.taskRunning = false;
	State.failed |= !success;
	if (success && State.completed < State.total)
		++State.completed;
}

Snapshot GetSnapshot()
{
	std::lock_guard<std::mutex> lock(StateMutex);
	Snapshot result = State;
	if (result.active)
		result.elapsedSeconds = ElapsedSeconds();
	result.cancellationRequested = CancelRequested.load();
	return result;
}

void TraceDetail(const char* format, ...)
{
	if (!IsTracing() || !format)
		return;
	char text[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	text[sizeof(text) - 1] = 0;
	Printf("STARTUP detail: %s\n", text);
}

Operation::Operation(const char* category, const char* name)
{
	{
		std::lock_guard<std::mutex> lock(StateMutex);
		if (!State.active || !Trace)
			return;
		mGeneration = Generation;
		mSequence = ++OperationSequence;
	}
	mCategory = category ? category : "startup";
	mName = name ? name : "unnamed";
	Printf("STARTUP operation begin: id=%llu category=\"%s\" name=\"%s\"\n",
		(unsigned long long)mSequence, mCategory.c_str(), mName.c_str());
	mStartSeconds = NowSeconds();
}

Operation::~Operation()
{
	try { Finish(false); }
	catch (...) {}
}

void Operation::Finish(bool success, int64_t result)
{
	if (mGeneration == 0 || mFinished)
		return;
	mFinished = true;
	const double elapsedMs = (NowSeconds() - mStartSeconds) * 1000.0;
	{
		std::lock_guard<std::mutex> lock(StateMutex);
		if (!State.active || !Trace || mGeneration != Generation)
			return;
	}
	Printf("STARTUP operation end: id=%llu category=\"%s\" name=\"%s\" success=%u result=%lld elapsed_ms=%.3f\n",
		(unsigned long long)mSequence, mCategory.c_str(), mName.c_str(), success ? 1u : 0u, (long long)result, elapsedMs);
}
}
