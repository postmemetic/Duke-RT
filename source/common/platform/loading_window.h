#pragma once

// The engine thread owns the game window. Long loading operations can
// acknowledge the OS and repaint the client between completed units of work.
// The normal input loop still owns queued game commands, input and shutdown.
namespace LoadingWindow
{
// Main-thread observation of a completed OS message check. Set only while
// collecting a loading trace; the window procedure never enters the renderer.
inline thread_local void (*onService)() = nullptr;
inline void ObservedService() { if (onService) onService(); }
#ifdef _WIN32
void Service();
#else
inline void Service() {}
#endif

// Static scene and voxel builders are shared with ordinary gameplay.
// Only the loading caller enables their periodic window updates.
class Scope
{
public:
	Scope() { ++depth; }
	~Scope() { --depth; }
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
	static void ServiceIfActive() { if (depth) Service(); }

private:
	static inline thread_local unsigned depth = 0;
};
}
