// Addresses (1.09) and the sequence: docs/research/direct-launch.md §2.1.
#include "launch.h"

#include <cstring>

#include "game.h"
#include "hooks.h"
#include "log.h"
#include "replay.h"

namespace ssr::launch {
namespace {
constexpr uintptr_t kMenuBg = 0x14161C9C0;         // mode-select 3D background stage
constexpr uintptr_t kBgUnload = 0x14066F3B8, kBgReset = 0x140670738, kBgLight = 0x140670054;
constexpr uintptr_t kBgLoaded = 0x14066FE40;       // (bg): both resource groups loaded
constexpr uintptr_t kDropHeader = 0x1406D6A64;     // (BattleModeSelect scene)
constexpr uintptr_t kBgm = 0x140721128;            // (cmd, arg): 4 = what MatchSelect does before character select
constexpr uintptr_t kPropManager = 0x1416BDA28, kPropFind = 0x140AB7930;
constexpr uintptr_t kModeSelectTag = 0x1415FE838;  // "BattleModeSelectScene" property type
constexpr uintptr_t kRootStep = 0x14073261C;       // FreeBattleRoot sequence step (root, idx, msg)
constexpr uintptr_t kRootSetSeq = 0x1406D6014;     // (root, idx): root+0x1070
constexpr int kSeqVsLoading = 3;                   // {410 MatchSelect, 409 CharSelect, 453 StageSelect, 411 VSLoading, 402}

// Unloading: the menu background was just unloaded/reset; the decision waits until its groups have settled.
enum class State { Idle, Unloading, Pushed };
constexpr int kUnloadSettleFrames = 15;  // frames the background must stay settled before VSLoading may request files
constexpr int kUnloadTimeoutFrames = 300;
State g_state = State::Idle;
int g_wait = 0, g_settled = 0;
uint8_t* g_menu = nullptr;  // the Free Battle menu object (decision fields +0x4C/+0x50)
std::function<bool()> g_write_setup;  // puts the battle setup in place on the root's first step

template <class F> F fn(uintptr_t va) { return game::at<F>(va); }

uint8_t* prop(const char* name, uintptr_t tag) {
  void* mgr = *game::at<void**>(kPropManager);
  if (!mgr) return nullptr;
  auto* holder = fn<uint8_t*(__fastcall*)(void*, const char*)>(kPropFind)(mgr, name);
  auto* value = holder ? *reinterpret_cast<uint8_t**>(holder + 8) : nullptr;
  if (!value) return nullptr;
  using TagFn = void*(__fastcall*)(void*);
  if (reinterpret_cast<TagFn>((*reinterpret_cast<void***>(value))[1])(value) != game::at<void*>(tag)) return nullptr;
  return *reinterpret_cast<uint8_t**>(value + 0x10);
}

using StepFn = void(__fastcall*)(void*, int, void*);
StepFn o_step = nullptr;
void __fastcall hk_step(uint8_t* root, int idx, void* msg) {
  if (g_state == State::Pushed && idx == 0) {
    g_state = State::Idle;
    // What MatchSelect / character select would have done on the way forward (the background was already dropped in
    // start_with): the header and the BGM.
    if (uint8_t* scene = prop("BattleModeSelectScene", kModeSelectTag)) fn<void(__fastcall*)(void*)>(kDropHeader)(scene);
    fn<void(__fastcall*)(int, uint64_t)>(kBgm)(4, 0);
    if (g_write_setup && g_write_setup()) {
      fn<void(__fastcall*)(void*, int)>(kRootSetSeq)(root, kSeqVsLoading);
      idx = kSeqVsLoading;
      log::info("launch: FreeBattleRoot → VSLoading with the prepared setup");
    } else {
      log::warn("launch: no BattleSelectParam on the root; falling back to character select");
    }
  }
  o_step(root, idx, msg);
}
}  // namespace

void install() {
  hooks::install("FreeBattleRoot::Step", game::at(kRootStep), reinterpret_cast<void*>(&hk_step), reinterpret_cast<void**>(&o_step));
}

bool active() { return g_state != State::Idle; }

bool menu_settled() {
  auto* bg = *game::at<uint8_t**>(kMenuBg);
  return bg && fn<int(__fastcall*)(void*)>(kBgLoaded)(bg) != 0 && *reinterpret_cast<int*>(bg + 0x154) == 0;
}

// The menu's own "VS Battle" decision (what AS3 SetSelect(2) does): the scene's state machine (0x1406D6394) then
// pushes FreeBattleRoot 408 once no fade is running, and the root's first step (hk_step) jumps to VSLoading.
bool start() {
  if (!replay::armed()) return false;
  return start_with(&replay::write_armed_setup);
}

bool start_with(std::function<bool()> write_setup) {
  if (g_state != State::Idle) return false;
  if (!menu_settled()) {
    log::warn("launch: menu background still loading");
    return false;
  }
  uint8_t* scene = prop("BattleModeSelectScene", kModeSelectTag);
  auto* ctrl = scene ? *reinterpret_cast<uint8_t**>(scene + 0x128) : nullptr;
  auto* menu = ctrl ? *reinterpret_cast<uint8_t**>(ctrl + 0x68) : nullptr;
  if (!menu) {
    log::warn("launch: Free Battle menu object not found");
    return false;
  }
  // MatchSelect drops the mode-select 3D background on its way forward (unload, then reset: releases group +0x60 and
  // re-requests group +0x20), and character/stage select give the loader thread seconds to finish. Launching VSLoading
  // in the same frame made the loader destroy pages while VSLoading's requests came in (crash in the page destructor
  // 0x140A11AC0 at 0x140A11CB9, 1–3 in 10 fresh launches). So: drop it now, decide once it has settled (tick).
  void* bg = *game::at<void**>(kMenuBg);
  fn<void(__fastcall*)(void*)>(kBgUnload)(bg);
  fn<void(__fastcall*)(void*)>(kBgReset)(bg);
  fn<void(__fastcall*)(void*)>(kBgLight)(bg);
  g_menu = menu;
  g_write_setup = std::move(write_setup);
  g_state = State::Unloading;
  g_wait = g_settled = 0;
  log::info("launch: menu background dropped; waiting for it to settle");
  return true;
}

void tick() {
  if (g_state == State::Unloading) {
    ++g_wait;
    g_settled = menu_settled() ? g_settled + 1 : 0;
    if (g_settled < kUnloadSettleFrames && g_wait < kUnloadTimeoutFrames) return;
    if (g_settled < kUnloadSettleFrames) log::warn("launch: menu background did not settle in {} frames; deciding anyway", g_wait);
    *reinterpret_cast<int*>(g_menu + 0x50) = 2;  // table 0x141098388[2] = 408 FreeBattleRoot
    *reinterpret_cast<int*>(g_menu + 0x4C) = 1;  // decided
    g_state = State::Pushed;
    log::info("launch: VS Battle decided ({} frames after dropping the background)", g_wait);
    g_wait = 0;
    return;
  }
  if (g_state == State::Pushed && ++g_wait > 600) {  // 20 s without the root's first step: the menu didn't take it
    log::warn("launch: FreeBattleRoot never started");
    g_state = State::Idle;
  }
}

}  // namespace ssr::launch
