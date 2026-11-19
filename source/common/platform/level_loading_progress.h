#pragma once

#include <cstdint>

namespace LevelLoadingProgress
{
enum class Stage { Level, World, Actors, Materials, FirstFrame };
struct Stats
{
	uint32_t shows = 0;
	uint32_t paints = 0;
	uint64_t maxPaintGapMs = 0;
};

#ifdef _WIN32
void Begin();
void SetStage(Stage);
void Presented();
Stats End();
#else
inline void Begin() {}
inline void SetStage(Stage) {}
inline void Presented() {}
inline Stats End() { return {}; }
#endif
}
