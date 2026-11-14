#pragma once

#include <cstdint>
#include <string>

// Startup producers run on the engine thread. The native window only reads
// copied snapshots and requests cancellation; it never enters engine code.
namespace StartupProgress
{
struct Snapshot
{
	std::string title;
	std::string stage;
	std::string task;
	uint32_t completed = 0;
	uint32_t total = 0; // Zero means this stage has no known work count.
	double elapsedSeconds = 0.0;
	bool active = false;
	bool taskRunning = false;
	bool failed = false;
	bool succeeded = false;
	bool cancellationRequested = false;
};

class Session
{
public:
	Session(const char* title, const char* fontId, bool showWindow, bool trace);
	~Session();
	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	// Called after a usable initial presentation. Window/input visibility is
	// owned by the startup integration, including during failed teardown.
	void Complete();

private:
	void Close(bool completed) noexcept;
	uint64_t mGeneration = 0;
};

bool IsActive();
bool IsTracing();
bool IsWindowAvailable();
bool WasWindowShown();
bool IsCancellationRequested();
void RequestCancellation() noexcept;
void CheckCancellation(); // Throws CExitEvent on the engine thread only.
void SetStage(const char* stage);
void BeginTasks(const char* stage, uint32_t total);
void BeginTask(const char* humanLabel);
void EndTask(bool success);
Snapshot GetSnapshot();

// Diagnostic names stay in logs; pass human-readable text to BeginTask.
void TraceDetail(const char* format, ...);
class Operation
{
public:
	Operation(const char* category, const char* name);
	~Operation();
	Operation(const Operation&) = delete;
	Operation& operator=(const Operation&) = delete;
	void Finish(bool success, int64_t result = 0);

private:
	std::string mCategory;
	std::string mName;
	uint64_t mGeneration = 0;
	uint64_t mSequence = 0;
	double mStartSeconds = 0.0;
	bool mFinished = false;
};
}
