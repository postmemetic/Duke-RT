#pragma once

namespace LevelLoadingProgress
{
enum class Stage { None, Level, World, Actors, Materials };

// The engine thread owns both the map preload and its scripted loading screen.
void Begin(bool show);
void SetStage(Stage stage);
void HoldLastCaption();
void End();
}
