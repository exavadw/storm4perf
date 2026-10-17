#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace ssr {

// Game thread = the thread that calls SteamAPI_RunCallbacks every frame (the game's main loop).
// Work that touches engine state is queued here and runs at the start of the next RunCallbacks.
void post_to_game_thread(std::function<void()> fn);
void run_game_thread_tasks();
bool on_game_thread();
uint32_t game_thread_id();

void on_steam_init();       // first game-thread entry: install game hooks
void on_steam_shutdown();
void on_game_frame();      // after queued tasks, every RunCallbacks (game thread)

// Lua discovery (lua_discovery.cpp)
void install_lua_discovery();
void* lua_function(const std::string& name);  // native address registered for a cc* Lua name, or nullptr

// Overlay (overlay.cpp)
void install_overlay();
void request_screenshot(const std::wstring& path);

// Automation pipe (pipe.cpp)
void start_pipe_server();

}  // namespace ssr
