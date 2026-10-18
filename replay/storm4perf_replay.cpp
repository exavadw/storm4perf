// storm4perf_replay.cpp: save and watch replays (the StormReplay core) inside storm4perf's xinput9_1_0.dll.
// Every VS battle in Free Battle is recorded to StormReplay\replays\recent (inputs, battle setup, RNG); Y on the Free
// Battle menu opens the native replay list. Development, tests and research happen in a separate private project.
//
// Boot: storm4perf's DllMain calls s4p_replay_attach, which hooks the SteamAPI_RunCallbacks export of whatever
// steam_api64.dll is loaded (Valve's or another mod's proxy). The game calls it once per frame on its main thread; the
// first call installs the game hooks, every call runs the replay UI. The rest of the folder is the StormReplay core,
// copied from that repo (runtime/src); game, hooks, log, input and props are storm4perf's own small helpers.
#include <windows.h>

#include <filesystem>
#include <functional>

#include "browser.h"
#include "game.h"
#include "hooks.h"
#include "input.h"
#include "launch.h"
#include "log.h"
#include "playback_ui.h"
#include "replay.h"
#include "text.h"
#include "ui_window.h"

namespace ssr {
namespace {
DWORD g_game_tid = 0;
bool g_ready = false;  // game hooks installed (supported exe, replays enabled)
replay::Config g_cfg;

void first_frame() {
  g_game_tid = GetCurrentThreadId();
  log::info("game thread = t{}", g_game_tid);
  game::verify();
  if (!game::info().supported) {
    log::warn("unsupported NSUNS4.exe build: replays are off (they need Storm 4 1.09)");
    return;
  }
  input::install();
  text::install();
  replay::install(g_cfg);
  playback_ui::install();
  browser::install();
  launch::install();
  browser::set_launcher(&launch::start);
  g_ready = true;
}

using RunCallbacksFn = void(__cdecl*)();
RunCallbacksFn o_run_callbacks = nullptr;
void __cdecl hk_run_callbacks() {
  static bool started = false;
  if (!started) {
    started = true;
    first_frame();
  }
  if (g_ready) {
    playback_ui::tick();
    ui::tick();
    launch::tick();
    browser::tick();
  }
  o_run_callbacks();
}
}  // namespace

}  // namespace ssr

// Called from storm4perf's DllMain. dir = game folder, ini = storm4perf.ini. Returns false if replays could not start.
extern "C" bool s4p_replay_attach(const wchar_t* dir, const wchar_t* ini) {
  using namespace ssr;
  const std::filesystem::path root = std::filesystem::path(dir) / L"StormReplay";
  game::init();
  log::init(root / L"logs" / L"replay.log");
  log::info("storm4perf replays starting");
  g_cfg.root = root;
  g_cfg.auto_record = GetPrivateProfileIntW(L"replay", L"auto_record", 1, ini) != 0;
  g_cfg.keep_recent = static_cast<int>(GetPrivateProfileIntW(L"replay", L"keep_recent", 20, ini));
  if (g_cfg.keep_recent < 1) g_cfg.keep_recent = 1;
  HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
  void* run = steam ? reinterpret_cast<void*>(GetProcAddress(steam, "SteamAPI_RunCallbacks")) : nullptr;
  if (!run) {
    log::error("steam_api64.dll / SteamAPI_RunCallbacks not found: replays are off");
    return false;
  }
  if (!hooks::init()) return false;
  return hooks::install("SteamAPI_RunCallbacks", run, reinterpret_cast<void*>(&hk_run_callbacks), reinterpret_cast<void**>(&o_run_callbacks));
}
