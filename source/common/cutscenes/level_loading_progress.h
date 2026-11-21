#pragma once

namespace LevelLoadingProgress
{
enum class Stage { None, Level, World, Actors, Materials, FirstFrame };

// The engine thread owns both the map preload and its scripted loading screen.
void Begin(bool show);
void SetStage(Stage stage);
void End();
}
