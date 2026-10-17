#pragma once
#include <string>

// Native UI while a replay plays: the game's own pause menu with replay items, playback speed by frame pacing,
// frame advance, HUD toggle, and an input guard so the viewer's pad cannot skip or steer the battle.
namespace ssr::playback_ui {
void install();
void tick();  // game thread, every frame
std::string command(const std::string& args);  // speed <x> | step on|off | hud on|off | status
}  // namespace ssr::playback_ui
