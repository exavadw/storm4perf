// Select window helper, same call sequence as ccSceneBattleEndBase's "EndMenu" (0x1406D1F64) and StormStory's endmenu:
// alloc 0x210 → ctor 0x1406A3CC0(mem, name, 0) → items 0x1406A4B94 / yes-no 0x1406A59FC → title 0x1406A572C → key help
// 0x140621AA4/0x140621838/0x1406A4B5C → +500 = 1, +0x1D8 = 0 if B must not cancel (it is the cancel button mask; +0x1D4
// decide) → start 0x1406A4A10 → input focus push.
// +0x1CC == 3 once decided or cancelled; result 0x1406A4894 (−1 = cancel). Close 0x1406A4B2C, focus pop, and the
// deleting dtor a few frames later (deleting at once crashes the closing animation).
#include "ui_window.h"

#include <format>

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <deque>
#include <memory>

#include "game.h"
#include "input.h"
#include "log.h"

namespace ssr::ui {
namespace {
constexpr uintptr_t kAlloc = 0x140A017F0, kWinCtor = 0x1406A3CC0, kWinSetItems = 0x1406A4B94, kWinYesNo = 0x1406A59FC,
                    kWinTitle = 0x1406A572C, kWinResize = 0x1406A5708, kWinStart = 0x1406A4A10, kWinResult = 0x1406A4894,
                    kWinClose = 0x1406A4B2C, kKeyHelpClear = 0x140621AA4, kKeyHelpAdd = 0x140621838, kKeyHelpShow = 0x1406A4B5C,
                    kFocusPush = 0x14065FF7C, kFocusPop = 0x14065FFE8, kFocusMgr = 0x14161C8D0, kSePlay = 0x14073D860,
                    kSeMgr = 0x141610160;
// Items with help-bar descriptions (the pause menu's way, 0x14067F8DC): an array of 0x70-byte items, each two of the
// game's 0x38-byte strings (label id, description id), built with its own string ctor/assign/dtor; then
// 0x1406A4E34(win, flags, items, n, isId, -1). The window shows the highlighted item's description when +0x1F8 == 1
// (0x1406A55CC(win, row) on cursor moves).
constexpr uintptr_t kWinSetItemsDesc = 0x1406A4E34, kStrCtor = 0x1401CA8F8, kStrAssign = 0x1401CB71C, kItemDtor = 0x1404E9D3C,
                    kWinShowDesc = 0x1406A55CC;
template <class F> F fn(uintptr_t va) { return game::at<F>(va); }

struct Request {
  enum Kind { List, YesNo } kind = List;
  std::vector<std::string> ids;
  Options opts;
  std::function<void(int)> cb;
};
std::deque<Request> g_queue;
uint8_t* g_win = nullptr;
std::function<void(int)> g_cb;
std::vector<std::string> g_ids;  // keep the id strings alive while the window exists
std::vector<const char*> g_id_ptrs;
std::string g_title;
uint8_t* g_dying = nullptr;
int g_dying_frames = 0, g_quiet_frames = 0;
int g_rows_down = 0, g_row_wait = 0;  // start_row: Down presses still to send once the window has opened

void open_now(Request r) {
  g_ids = std::move(r.ids);
  g_id_ptrs.clear();
  for (auto& s : g_ids) g_id_ptrs.push_back(s.c_str());
  g_title = r.opts.title_id;
  g_cb = std::move(r.cb);
  void* mem = fn<void*(__fastcall*)(size_t, const char*, int)>(kAlloc)(0x210, "StormReplay/ui_window.cpp", __LINE__);
  g_win = fn<uint8_t*(__fastcall*)(void*, const char*, int)>(kWinCtor)(mem, "ReplayMenu", 0);
  if (r.kind == Request::YesNo) {
    fn<void(__fastcall*)(void*, const char*, int)>(kWinYesNo)(g_win, g_title.c_str(), 1);
  } else if (r.opts.desc_ids.size() == g_ids.size()) {
    int flags[4] = {0, 0x10, 0x40, 2};
    const size_t n = g_ids.size();
    std::vector<uint8_t> items(n * 0x70 + 16);
    auto* base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(items.data()) + 15) & ~uintptr_t{15});
    for (size_t i = 0; i < n; ++i) {
      uint8_t* it = base + i * 0x70;
      fn<void(__fastcall*)(void*)>(kStrCtor)(it);
      fn<void(__fastcall*)(void*)>(kStrCtor)(it + 0x38);
      fn<void(__fastcall*)(void*, const char*, size_t)>(kStrAssign)(it, g_ids[i].c_str(), g_ids[i].size());
      const auto& d = r.opts.desc_ids[i];
      fn<void(__fastcall*)(void*, const char*, size_t)>(kStrAssign)(it + 0x38, d.c_str(), d.size());
    }
    fn<void(__fastcall*)(void*, int*, void*, unsigned, int, int)>(kWinSetItemsDesc)(g_win, flags, base, static_cast<unsigned>(n), 1, -1);
    for (size_t i = 0; i < n; ++i) fn<void(__fastcall*)(void*)>(kItemDtor)(base + i * 0x70);
    *reinterpret_cast<int*>(g_win + 0x1F8) = 1;  // show descriptions on cursor moves
    fn<void(__fastcall*)(void*, int)>(0x1406A5884)(g_win, 0);  // as the pause menu does after its items (help bar, +0x1FC)
    if (!g_title.empty()) fn<void(__fastcall*)(void*, const char*, int)>(kWinTitle)(g_win, g_title.c_str(), 1);
  } else {
    int flags[4] = {0, 0x10, 0x40, 2};  // as the end menu passes them
    fn<void(__fastcall*)(void*, int*, const char**, int, int, int)>(kWinSetItems)(g_win, flags, g_id_ptrs.data(),
                                                                                   static_cast<int>(g_id_ptrs.size()), 1, -1);
    if (!g_title.empty()) fn<void(__fastcall*)(void*, const char*, int)>(kWinTitle)(g_win, g_title.c_str(), 1);
  }
  fn<void(__fastcall*)(void*)>(kWinResize)(g_win);
  fn<void(__fastcall*)(void*)>(kKeyHelpClear)(g_win);
  if (r.opts.cancel) fn<void(__fastcall*)(void*, int)>(kKeyHelpAdd)(g_win, 1);
  fn<void(__fastcall*)(void*, int)>(kKeyHelpAdd)(g_win, 0);
  fn<void(__fastcall*)(void*)>(kKeyHelpShow)(g_win);
  *reinterpret_cast<int*>(g_win + 500) = 1;
  if (!r.opts.cancel) *reinterpret_cast<int*>(g_win + 0x1D8) = 0;  // cancel button mask (the ctor sets the game's B)
  fn<void(__fastcall*)(void*)>(kWinStart)(g_win);
  fn<void(__fastcall*)(void*, int)>(kFocusPush)(*game::at<void**>(kFocusMgr), 4);
  if (r.opts.desc_ids.size() == g_ids.size() && !g_ids.empty()) fn<void(__fastcall*)(void*, int)>(kWinShowDesc)(g_win, 0);
  // The cursor setter does not move the selection (StormStory endmenu note); the row is reached with Down presses
  // once the open animation has finished.
  g_rows_down = std::max(0, r.opts.start_row);
  g_row_wait = 20;
}
}  // namespace

void open(std::vector<std::string> item_ids, std::function<void(int)> on_choice, Options opts) {
  g_queue.push_back(Request{Request::List, std::move(item_ids), std::move(opts), std::move(on_choice)});
}

void yes_no(std::string title_id, std::function<void(bool)> on_answer) {
  Options o;
  o.title_id = std::move(title_id);
  g_queue.push_back(Request{Request::YesNo, {}, std::move(o), [cb = std::move(on_answer)](int c) {
                              if (cb) cb(c == 0);
                            }});
}

void notice(std::string title_id, std::function<void()> on_ok) {
  Options o;
  o.title_id = std::move(title_id);
  g_queue.push_back(Request{Request::List, {"MSG_OK"}, std::move(o), [cb = std::move(on_ok)](int) {
                              if (cb) cb();
                            }});
}

bool is_open() { return g_win != nullptr; }

void dismiss(int code) {
  if (!g_win) return;
  *reinterpret_cast<int*>(g_win + 0x1C8) = code;
  *reinterpret_cast<int*>(g_win + 0x1CC) = 3;
}

std::string debug() {
  if (!g_win) return "win=none";
  auto i = [](int off) { return *reinterpret_cast<int*>(g_win + off); };
  return std::format("win={:p} 1c8={} 1cc={} 1d0={} 1d4={:x} 1d8={:x} 1dc={} 1f4={} 1f8={} 1fc={} 200={}", static_cast<void*>(g_win), i(0x1C8),
                     i(0x1CC), i(0x1D0), i(0x1D4), i(0x1D8), i(0x1DC), i(0x1F4), i(0x1F8), i(0x1FC), i(0x200));
}

bool busy() { return g_win || g_dying || g_quiet_frames > 0 || !g_queue.empty(); }

void play_se(int id) {
  void* mgr = *game::at<void**>(kSeMgr);
  if (mgr) fn<void(__fastcall*)(void*, int, int, float, int, int)>(kSePlay)(mgr, id, 0, 1.0f, 0, 1);
}

void tick() {
  if (g_dying && --g_dying_frames <= 0) {
    reinterpret_cast<void(__fastcall*)(void*, int)>((*reinterpret_cast<void***>(g_dying))[0])(g_dying, 1);
    g_dying = nullptr;
    g_quiet_frames = 6;
  } else if (!g_dying && !g_win && g_quiet_frames > 0) {
    --g_quiet_frames;
  }
  if (!g_win && !g_dying && g_quiet_frames == 0 && !g_queue.empty()) {
    Request r = std::move(g_queue.front());
    g_queue.pop_front();
    open_now(std::move(r));
    return;
  }
  if (g_win && g_rows_down > 0 && --g_row_wait <= 0) {
    input::press(XINPUT_GAMEPAD_DPAD_DOWN, 50);
    --g_rows_down;
    g_row_wait = 5;
  }
  if (!g_win || *reinterpret_cast<int*>(g_win + 0x1CC) != 3) return;
  const int choice = fn<int(__fastcall*)(void*)>(kWinResult)(g_win);
  g_rows_down = 0;
  fn<void(__fastcall*)(void*)>(kWinClose)(g_win);
  fn<void(__fastcall*)(void*)>(kFocusPop)(*game::at<void**>(kFocusMgr));
  g_dying = g_win;
  g_dying_frames = 3;
  g_win = nullptr;
  if (auto cb = std::move(g_cb)) cb(choice);
}

}  // namespace ssr::ui
