// Playback controls built from the game's own pieces (1.09 VAs, verified by decompile):
//   Pause menu ccUiFlashPauseMenuFreeBattle: CreateItems 0x1406807A0(menu) fills named slots menu+0x108..+0x150 (freed by
//   ClearItems 0x140680C14) and appends them with 0x14067F16C(menu, item). Items: base ctor 0x14068399C; +0x30 label id,
//   +0x38 desc id (0x140683CF0 stores the pointers), +0x48 confirm window, +0x50 state, +0x54 action, +0x70 pad side;
//   vtbl [3] InitSelectData, [4] OnDecide, [5] Ctrl. Retry item 0x1406811A0 (action 2 = restart the battle) and
//   "Return to Free Battle Menu" 0x140680F58 (action 8) are reused as Restart Replay / Exit Replay under replay text.
//   Our choice items are native "Change Command Pop-Up Display" items (ctor 0x140681090) with a cloned vtable whose
//   [3]/[4]/[5] open the game's select window ("confirm", ctor 0x1406A3CC0) with our options.
//   Speed: nuccSys::Update 0x140A0C580 presents, then sleeps to the frame target; we scale its frame divisor for the
//   duration of that call (never the engine speed float 0x14161A334, which changes the simulation).
//   Frame advance: the task-manager pause stack (*0x1416BDA20: push 0x140ABE450(mgr, flags), pop 0x140ABE480(mgr),
//   depth mgr+0x1438), with the pause menu's own flags (top | 0x80001).
//   HUD: property "scene_battle" → hide 0x1406D9230(scene) / show 0x1406D924C(scene).
#include "playback_ui.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "game.h"
#include "hooks.h"
#include "input.h"
#include "log.h"
#include "props.h"
#include "replay.h"
#include "runtime.h"
#include "text.h"

namespace ssr::playback_ui {
namespace {
constexpr uintptr_t kAlloc = 0x140A017F0;          // void* (size, file, line)
constexpr uintptr_t kCreateItems = 0x1406807A0;    // ccUiFlashPauseMenuFreeBattle::CreateItems(menu)
constexpr uintptr_t kAppendItem = 0x14067F16C;     // (menu, item)
constexpr uintptr_t kItemRetry = 0x1406811A0, kItemExit = 0x140680F58, kItemCmdList = 0x1406810B4, kItemComboList = 0x140681064,
                    kItemPopup = 0x140681090;     // ctors(mem) → item
constexpr uintptr_t kItemPopupVtbl = 0x141079C50;
constexpr uintptr_t kItemSetText = 0x140683CF0;    // (item, {label, desc, flags})
constexpr uintptr_t kItemSetState = 0x140683A5C;   // (item, state)
constexpr uintptr_t kItemBack = 0x140683D60;       // (item): close the confirm window, back to the menu
constexpr uintptr_t kWinCtor = 0x1406A3CC0, kWinPadSide = 0x1406A56FC, kWinSetItems = 0x1406A4B94, kWinTitle = 0x1406A572C,
                    kWinResize = 0x1406A5708, kWinStart = 0x1406A4A10, kWinResult = 0x1406A4894, kWinClose = 0x1406A4B2C,
                    kKeyHelpClear = 0x140621AA4, kKeyHelpAdd = 0x140621838, kKeyHelpShow = 0x1406A4B5C;
constexpr uintptr_t kBattleMode = 0x141611F70;
constexpr uintptr_t kNuccUpdate = 0x140A0C580;     // nuccSys::Update(sys)
constexpr uintptr_t kNuccSys = 0x1416663C8;
constexpr uintptr_t kTaskMgr = 0x1416BDA20, kPushPause = 0x140ABE450, kPopPause = 0x140ABE480, kPauseTop = 0x140ABE4D0;
constexpr uintptr_t kHudHide = 0x1406D9230, kHudShow = 0x1406D924C;
constexpr uintptr_t kSceneMgr = 0x1416BDA18, kSceneUpdate = 0x140AB5810, kTaskExecute = 0x140ABE320;

template <class F> F fn(uintptr_t va) { return game::at<F>(va); }

// ---- playback state ----
const float kSpeeds[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
const char* kSpeedNames[] = {"1/4x", "1/2x", "1x", "2x", "4x"};
int g_speed = 2;  // index into kSpeeds
bool g_step = false, g_hud_hidden = false;
int g_our_pauses = 0;
bool g_step_once = false;  // popped for one tick; push again next frame
bool g_was_viewing = false;
uint16_t g_prev_buttons = 0;
int g_hud_refresh = 0;
bool g_hud_applied = false;

void* task_mgr() { return *game::at<void**>(kTaskMgr); }
int64_t pause_depth() {
  auto* m = static_cast<uint8_t*>(task_mgr());
  return m ? *reinterpret_cast<int64_t*>(m + 0x1438) : 0;
}
void push_pause() {
  if (!task_mgr()) return;
  const uint32_t top = fn<uint32_t(__fastcall*)(void*)>(kPauseTop)(task_mgr());
  // Use only the logic-pause bit (0x1). The 0x80000 bit freezes the render pipeline and causes a black screen
  // when frame advance is active; the pause menu's own push uses it because the menu draws over the scene,
  // but our step-mode pause must keep rendering alive so the frozen frame is visible.
  fn<void(__fastcall*)(void*, uint32_t)>(kPushPause)(task_mgr(), top | 0x1);
  ++g_our_pauses;
}
void pop_pause() {
  if (!task_mgr() || g_our_pauses <= 0) return;
  fn<void(__fastcall*)(void*)>(kPopPause)(task_mgr());
  --g_our_pauses;
}

void set_hud(bool hidden) {
  void* scene = props::storage("scene_battle");
  if (!scene) return;
  fn<void(__fastcall*)(void*)>(hidden ? kHudHide : kHudShow)(scene);
}

// Watching a Recent replay: the pause menu offers to keep it (Saved never trims itself; when full, nothing is replaced
// from here — the browser asks).
enum class SaveState { Hidden, Offer, Saved, Full };
SaveState g_save = SaveState::Hidden;
void refresh_labels();  // forward decl: called from save_watched before definition
void save_watched() {
  const auto file = replay::playing_file();
  if (file.empty() || g_save != SaveState::Offer) return;
  if (replay::count("saved") >= replay::kSavedCap) {
    g_save = SaveState::Full;
    refresh_labels();
    return;
  }
  std::error_code ec;
  std::filesystem::copy_file(file, replay::folder("saved") / file.filename(), std::filesystem::copy_options::overwrite_existing, ec);
  g_save = ec ? SaveState::Offer : SaveState::Saved;
  log::info("playback: saved {} ({})", file.filename().string(), ec ? ec.message() : "ok");
  refresh_labels();
}

void refresh_labels() {
  text::set("rpl_pause_save", g_save == SaveState::Saved ? "Replay Saved" : g_save == SaveState::Full ? "Saved Replays Full" : "Save Replay");
  text::set("rpl_speed", std::format("Playback Speed: {}", kSpeedNames[g_speed]));
  text::set("rpl_step", std::format("Frame Advance: {}", g_step ? "On" : "Off"));
  text::set("rpl_hud", std::format("HUD Display: {}", g_hud_hidden ? "Off" : "On"));
}

void set_speed(int idx) {
  g_speed = std::clamp(idx, 0, 4);
  refresh_labels();
  log::info("playback: speed {}", kSpeedNames[g_speed]);
}
void set_step(bool on) {
  g_step = on;
  refresh_labels();
  log::info("playback: frame advance {}", on ? "on" : "off");
}

// ---- choice items (native popup item with a cloned vtable) ----
struct Choice {
  const char* label;
  const char* desc;
  std::vector<const char*> options;
  int (*current)();
  void (*apply)(int);
};
const Choice kSpeedChoice{"rpl_speed", "rpl_speed_desc", {"rpl_opt_s025", "rpl_opt_s050", "rpl_opt_s100", "rpl_opt_s200", "rpl_opt_s400"},
                          [] { return g_speed; }, [](int i) { set_speed(i); }};
const Choice kStepChoice{"rpl_step", "rpl_step_desc", {"rpl_opt_on", "rpl_opt_off"}, [] { return g_step ? 0 : 1; },
                         [](int i) { set_step(i == 0); }};
const Choice kHudChoice{"rpl_hud", "rpl_hud_desc", {"rpl_opt_show", "rpl_opt_hide"}, [] { return g_hud_hidden ? 1 : 0; },
                        [](int i) {
                          g_hud_hidden = i == 1;  // applied when the pause menu closes (the HUD is hidden behind it anyway)
                          refresh_labels();
                        }};
const Choice kSaveChoice{"rpl_pause_save", "rpl_pause_save_desc", {"rpl_save_replay", "MSG_Back"}, [] { return 0; },
                         [](int i) {
                           if (i == 0 && g_save == SaveState::Offer) save_watched();
                           // After saving (or if already saved/full), do nothing on re-select — the label is the feedback.
                         }};
std::map<void*, const Choice*> g_items;
void* g_choice_vtbl[8]{};

void __fastcall choice_init(uint8_t* item, int) {
  const Choice* c = g_items.count(item) ? g_items[item] : nullptr;
  if (!c) return;
  struct {
    const char* label;
    const char* desc;
    uint64_t flags;
  } t{c->label, c->desc, 0};
  fn<void(__fastcall*)(void*, void*)>(kItemSetText)(item, &t);
}

void __fastcall choice_decide(uint8_t* item) {
  const Choice* c = g_items.count(item) ? g_items[item] : nullptr;
  if (!c) return;
  void* mem = fn<void*(__fastcall*)(size_t, const char*, int)>(kAlloc)(0x210, "StormReplay/playback_ui.cpp", __LINE__);
  auto* win = fn<uint8_t*(__fastcall*)(void*, const char*, int)>(kWinCtor)(mem, "confirm", 0);
  *reinterpret_cast<uint8_t**>(item + 0x48) = win;
  fn<void(__fastcall*)(void*, int)>(kWinPadSide)(win, *reinterpret_cast<int*>(item + 0x70));
  int flags[4] = {0, 0x10, 0x40, 2};
  fn<void(__fastcall*)(void*, int*, const char**, int, int, int)>(kWinSetItems)(
      win, flags, const_cast<const char**>(c->options.data()), static_cast<int>(c->options.size()), 1, -1);
  fn<void(__fastcall*)(void*, const char*, int)>(kWinTitle)(win, c->label, 1);
  fn<void(__fastcall*)(void*)>(kWinResize)(win);
  *reinterpret_cast<int*>(win + 500) = 1;
  fn<void(__fastcall*)(void*)>(kWinStart)(win);
  fn<void(__fastcall*)(void*)>(kKeyHelpClear)(win);
  fn<void(__fastcall*)(void*, int)>(kKeyHelpAdd)(win, 1);
  fn<void(__fastcall*)(void*, int)>(kKeyHelpAdd)(win, 0);
  fn<void(__fastcall*)(void*)>(kKeyHelpShow)(win);
  fn<void(__fastcall*)(void*, int)>(kItemSetState)(item, 1);
}

void __fastcall choice_ctrl(uint8_t* item) {
  const Choice* c = g_items.count(item) ? g_items[item] : nullptr;
  auto* win = *reinterpret_cast<uint8_t**>(item + 0x48);
  if (!c || !win || *reinterpret_cast<int*>(win + 0x1CC) != 3) return;
  fn<void(__fastcall*)(void*)>(kWinClose)(win);
  const int choice = fn<int(__fastcall*)(void*)>(kWinResult)(win);
  if (choice >= 0 && choice < static_cast<int>(c->options.size())) c->apply(choice);
  fn<void(__fastcall*)(void*)>(kItemBack)(item);
}

using CreateItemsFn = void(__fastcall*)(uint8_t*);
CreateItemsFn o_create_items = nullptr;

void __fastcall hk_create_items(uint8_t* menu) {
  if (!replay::viewing() || *game::at<const uint32_t*>(kBattleMode) != 0) return o_create_items(menu);
  auto alloc = fn<void*(__fastcall*)(size_t, const char*, int)>(kAlloc);
  auto make = [&](uintptr_t slot, size_t size, uintptr_t ctor) {
    void* mem = alloc(size, "StormReplay/playback_ui.cpp", static_cast<int>(slot));
    auto* item = fn<uint8_t*(__fastcall*)(void*)>(ctor)(mem);
    *reinterpret_cast<uint8_t**>(menu + slot) = item;  // ClearItems frees the named slots
    return item;
  };
  auto choice = [&](uintptr_t slot, const Choice& c) {
    auto* item = make(slot, 0x80, kItemPopup);
    *reinterpret_cast<void***>(item) = g_choice_vtbl;
    g_items[item] = &c;
    return item;
  };
  refresh_labels();
  std::vector<uint8_t*> items{make(0x130, 0x80, kItemRetry), choice(0x120, kSpeedChoice), choice(0x118, kStepChoice),
                              choice(0x128, kHudChoice)};
  if (g_save != SaveState::Hidden) items.push_back(choice(0x138, kSaveChoice));
  items.push_back(make(0x108, 0x90, kItemCmdList));
  items.push_back(make(0x110, 0x90, kItemComboList));
  items.push_back(make(0x140, 0x80, kItemExit));
  for (auto* it : items) fn<void(__fastcall*)(void*, void*)>(kAppendItem)(menu, it);
}

// ---- speed: pace around nuccSys::Update ----
using UpdateFn = uintptr_t(__fastcall*)(uint8_t*);
UpdateFn o_update = nullptr;
// Slow motion: nuccSys+0x4A0 = 60 / logic fps (2 at 30 fps) is the frame limiter's divisor; during this call only it is
// scaled up, so each logic tick stays on screen longer. The simulation never sees a different value.
// Fast forward: the main loop (0x140AB8930) runs scene update 0x140AB5810(*0x1416BDA18) + logic phases
// Execute(task mgr, 2, 0x4F, 1) + render phases, then this Update. Rendering caps the frame rate, so 2x/4x run 1/3 extra
// logic passes after the present instead (same scene and task calls, no extra render).
uintptr_t __fastcall hk_update(uint8_t* sys) {
  const float speed = replay::in_transition() ? 1.0f : kSpeeds[g_speed];
  if (!replay::viewing() || speed == 1.0f || pause_depth() > 0) return o_update(sys);
  if (speed < 1.0f) {
    int& div = *reinterpret_cast<int*>(sys + 0x4A0);
    const int base = div;
    div = static_cast<int>(base / speed + 0.5f);
    const uintptr_t r = o_update(sys);
    div = base;
    return r;
  }
  const uintptr_t r = o_update(sys);
  void* scenes = *game::at<void**>(kSceneMgr);
  for (int extra = static_cast<int>(speed) - 1; extra > 0 && scenes && task_mgr() && replay::viewing() && pause_depth() == 0; --extra) {
    fn<void(__fastcall*)(void*)>(kSceneUpdate)(scenes);
    fn<void(__fastcall*)(void*, int, int, int)>(kTaskExecute)(task_mgr(), 2, 0x4F, 1);
  }
  return r;
}

void install_text() {
  text::set("rpl_speed_desc", "Change how fast the replay plays.");
  text::set("rpl_step_desc", "Freeze the replay and advance one frame at a time with RB.");
  text::set("rpl_hud_desc", "Show or hide the battle HUD while watching.");
  text::set("rpl_pause_save_desc", "Keep this replay in Saved Replays. Recent replays are replaced as you play.");
  text::set("rpl_opt_s025", "1/4x");
  text::set("rpl_opt_s050", "1/2x");
  text::set("rpl_opt_s100", "1x");
  text::set("rpl_opt_s200", "2x");
  text::set("rpl_opt_s400", "4x");
  text::set("rpl_opt_on", "On");
  text::set("rpl_opt_off", "Off");
  text::set("rpl_opt_show", "Show");
  text::set("rpl_opt_hide", "Hide");
  text::set_override("battlestartmenu_007", "Restart Replay");
  text::set_override("battlestartmenu_001", "Watch the replay again from the start.");
  text::set_override("battlestartmenu_009", "Restart the replay?");
  text::set_override("battlestartmenu_046", "Exit Replay");
  text::set_override("battlestartmenu_047", "Stop watching and return to the Free Battle menu.");
  text::set_override("battlestartmenu_097", "Exit the replay?");
  text::set_override("MSG_Rematch", "Watch Again");
}
}  // namespace

void install() {
  install_text();
  refresh_labels();
  std::memcpy(g_choice_vtbl, game::at<void*>(kItemPopupVtbl), sizeof g_choice_vtbl);
  g_choice_vtbl[3] = reinterpret_cast<void*>(&choice_init);
  g_choice_vtbl[4] = reinterpret_cast<void*>(&choice_decide);
  g_choice_vtbl[5] = reinterpret_cast<void*>(&choice_ctrl);
  hooks::install("PauseMenuFreeBattle::CreateItems", game::at(kCreateItems), reinterpret_cast<void*>(&hk_create_items),
                 reinterpret_cast<void**>(&o_create_items));
  hooks::install("nuccSys::Update", game::at(kNuccUpdate), reinterpret_cast<void*>(&hk_update), reinterpret_cast<void**>(&o_update));
}

void tick() {
  const bool viewing = replay::viewing();
  if (viewing != g_was_viewing) {
    g_was_viewing = viewing;
    if (viewing) {
      // Reset pause state at the start of every new viewing session. A prior session that ended abnormally
      // (e.g. Restart Replay while frame advance was active) can leave stale g_our_pauses; popping them here
      // prevents the task-manager pause stack from accumulating across restarts and crashing the game.
      while (g_our_pauses > 0) pop_pause();
      g_step_once = false;
      // A replay watched from the list goes back to the list (the browser reopens on it); Watch Again stays native.
      const auto file = replay::playing_file();
      const bool from_list = !file.empty();
      text::set_override("battlestartmenu_046", from_list ? "Return to Replays" : "Exit Replay");
      text::set_override("battlestartmenu_047", from_list ? "Stop watching and go back to the replay list." :
                                                            "Stop watching and return to the Free Battle menu.");
      text::set_override("battlestartmenu_097", from_list ? "Return to the replay list?" : "Exit the replay?");
      const bool recent = from_list && file.parent_path() == replay::folder("recent");
      std::error_code ec;
      g_save = recent && !std::filesystem::exists(replay::folder("saved") / file.filename(), ec) ? SaveState::Offer : SaveState::Hidden;
      refresh_labels();
    }
    text::enable_overrides(viewing);
    if (!viewing) {  // battle gone: give everything back
      while (g_our_pauses > 0) pop_pause();
      g_step = g_step_once = false;
      g_speed = 2;
      g_hud_hidden = g_hud_applied = false;
      input::set_reserved(0);
      refresh_labels();
      log::info("playback: controls released");
    } else {
      log::info("playback: controls active");
    }
  }
  if (!viewing) return;

  const bool menu_open = pause_depth() > g_our_pauses;
  // The viewer's pad must not reach the battle (it would skip intros/KOs and change the tick count); Start still opens
  // the pause menu, which then gets the full pad. LB/RB are our speed / frame-advance keys.
  // Once the recorded inputs are used up (end menu, Watch Again / Exit Replay) the pad is the viewer's again.
  input::set_reserved(menu_open || !replay::replaying() ? 0 : static_cast<uint16_t>(0xFFFF & ~XINPUT_GAMEPAD_START));
  const uint16_t buttons = static_cast<uint16_t>(input::real_buttons() | input::virtual_buttons());
  const uint16_t pressed = buttons & ~g_prev_buttons;
  g_prev_buttons = buttons;

  const bool transition = replay::in_transition();  // KO / round intro: no frame advance, HUD changes wait
  if (g_step_once) {  // one tick ran: freeze again
    g_step_once = false;
    if (g_step && !menu_open && !transition) push_pause();
  }
  if (!menu_open) {
    const bool hold = g_step && !transition;
    if (hold && g_our_pauses == 0) push_pause();
    if (!hold && g_our_pauses > 0) pop_pause();
    if (hold && (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) && g_our_pauses > 0) {
      pop_pause();
      g_step_once = true;
    }
    if (!g_step && (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)) set_speed(g_speed + 1);
    if (!g_step && (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)) set_speed(g_speed - 1);
    // The game re-shows the HUD at each round start, so a hidden HUD is re-applied every half second.
    if (transition) {
      // leave the HUD as it is until the round is under way
    } else if (g_hud_hidden && (!g_hud_applied || ++g_hud_refresh >= 30)) {
      g_hud_refresh = 0;
      g_hud_applied = true;
      set_hud(true);
    } else if (!g_hud_hidden && g_hud_applied) {
      g_hud_applied = false;
      set_hud(false);
    }
  }
}

std::string command(const std::string& args) {
  std::istringstream in(args);
  std::string sub, v;
  in >> sub >> v;
  if (sub == "speed") {
    for (int i = 0; i < 5; ++i)
      if (v == kSpeedNames[i]) {
        set_speed(i);
        return "ok";
      }
    return "error speeds: 1/4x 1/2x 1x 2x 4x";
  }
  if (sub == "step") {
    set_step(v == "on");
    return "ok";
  }
  if (sub == "hud") {
    g_hud_hidden = v == "off";
    refresh_labels();
    return "ok";
  }
  return std::format("ok viewing={} speed={} step={} hud={} our_pauses={} pause_depth={}", replay::viewing(), kSpeedNames[g_speed], g_step,
                     g_hud_hidden ? "off" : "on", g_our_pauses, pause_depth());
}

}  // namespace ssr::playback_ui
