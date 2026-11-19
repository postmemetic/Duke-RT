#include "nri_profile_transition.h"

#include "printf.h"
#include "../../../platform/loading_window.h"

#ifdef _WIN32
#include <future>
#include <string>
#include <vector>
#endif

namespace nri_profile
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		const char* preset = nullptr;
		unsigned serial = 0;
		unsigned phaseDepth = 0;
		bool firstPresent = false;
		Clock::time_point started;

		double ElapsedMs(Clock::time_point then)
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - then).count();
		}

#ifdef _WIN32
		struct Message
		{
			std::string type, file, text;
			unsigned line;
		};
		thread_local std::vector<Message>* workerMessages = nullptr;
#endif
	}

	bool Active() { return preset != nullptr; }
	bool InPhase() { return phaseDepth != 0; }

	void Begin(const char* name)
	{
		if (Active())
			Printf("NRI profile transition: serial=%u superseded_ms=%.3f\n", serial, ElapsedMs(started));
		preset = name;
		++serial;
		firstPresent = false;
		started = Clock::now();
		Printf("NRI profile transition: serial=%u preset=\"%s\" event=begin\n", serial, preset);
	}

	void Reset() { preset = nullptr; }

	void OnPresent(bool pathTracedOutput)
	{
		if (!Active()) return;
		if (!firstPresent)
		{
			firstPresent = true;
			Printf("NRI profile transition: serial=%u first_present_ms=%.3f\n", serial, ElapsedMs(started));
		}
		if (pathTracedOutput)
		{
			Printf("NRI profile transition: serial=%u first_pt_output_ms=%.3f\n", serial, ElapsedMs(started));
			Reset();
		}
	}

	Phase::Phase(const char* name) : mName(Active() ? name : nullptr)
	{
		if (mName)
		{
			++phaseDepth;
			mStart = Clock::now();
		}
	}

	Phase::~Phase()
	{
		if (mName)
		{
			--phaseDepth;
			Printf("NRI profile transition: serial=%u phase=%s ms=%.3f\n", serial, mName, ElapsedMs(mStart));
		}
	}

	bool DeferWorkerMessage(const char* type, const char* file, unsigned line, const char* text)
	{
#ifdef _WIN32
		if (workerMessages)
		{
			workerMessages->push_back({ type, file ? file : "", text, line });
			return true;
		}
#endif
		return false;
	}

	void RunNative(const char* name, const std::function<void()>& work)
	{
		Phase phase(name);
#ifdef _WIN32
		if (Active())
		{
			// The game thread never starts another frame here. The worker
			// exclusively uses the native resources until joined, including
			// recording provider initialization on the original command list.
			std::vector<Message> messages;
			std::future<void> done;
			try
			{
				done = std::async(std::launch::async, [&] {
					workerMessages = &messages;
					try { work(); }
					catch (...) { workerMessages = nullptr; throw; }
					workerMessages = nullptr;
				});
			}
			catch (const std::system_error&)
			{
				work();
				return;
			}
			while (done.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready)
				LoadingWindow::Service();
			done.get();
			for (const auto& message : messages)
			{
				if (message.file.empty()) Printf("NRI %s: %s\n", message.type.c_str(), message.text.c_str());
				else Printf("NRI %s: %s (%s:%u)\n", message.type.c_str(), message.text.c_str(), message.file.c_str(), message.line);
			}
			return;
		}
#endif
		work();
	}
}
