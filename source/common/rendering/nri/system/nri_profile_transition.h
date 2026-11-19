#pragma once

#include <chrono>
#include <functional>

// One user-selected preset, through its first visible path-traced output.
namespace nri_profile
{
	bool Active();
	bool InPhase();
	void Begin(const char* preset);
	void Reset();
	void OnPresent(bool pathTracedOutput);

	class Phase
	{
	public:
		explicit Phase(const char* name);
		~Phase();
		Phase(const Phase&) = delete;
		Phase& operator=(const Phase&) = delete;
	private:
		const char* mName;
		std::chrono::steady_clock::time_point mStart;
	};

	// Only pass native work whose device/command list remains exclusively
	// owned by the caller until this function returns.
	void RunNative(const char* phase, const std::function<void()>& work);
	// At a loading boundary, service the owning window while a single native
	// call completes. False means the worker could not start and work did not run.
	bool TryRunWithLoadingWindow(const std::function<void()>& work);
	bool DeferWorkerMessage(const char* type, const char* file, unsigned line, const char* text);
}
