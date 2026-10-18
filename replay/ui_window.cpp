#include "ui_window.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <deque>
#include <format>
#include <optional>

#include "game.h"
#include "input.h"

// How the game builds this window (1.09; see ccSceneBattleEndBase's end menu at 0x1406D1F64 and the pause menu at
// 0x14067F8DC): allocate 0x210 bytes (0x140A017F0), construct (0x1406A3CC0), set items (0x1406A4B94, or 0x1406A4E34 for
// items with help-bar descriptions) or a yes/no (0x1406A59FC), title (0x1406A572C), resize (0x1406A5708), key help
// (clear 0x140621AA4, add 0x140621838, show 0x1406A4B5C), then start it (0x1406A4A10) and push input focus (0x14065FF7C).
// Window fields: +0x1C8 result, +0x1CC state (3 = decided or cancelled), +0x1D8 cancel button mask, +0x1F4 open,
// +0x1F8 show item descriptions. Result 0x1406A4894 (-1 = cancelled), close 0x1406A4B2C, focus pop 0x14065FFE8, and the
// object is deleted (vtable[0](win, 1)) only a few frames after closing, when its animation no longer uses it.
namespace ssr::ui {
namespace {
namespace va {
constexpr uintptr_t Alloc = 0x140A017F0, Construct = 0x1406A3CC0, Items = 0x1406A4B94, ItemsWithHelp = 0x1406A4E34,
                    YesNo = 0x1406A59FC, Title = 0x1406A572C, Resize = 0x1406A5708, HelpBar = 0x1406A5884,
                    ShowItemHelp = 0x1406A55CC, KeysClear = 0x140621AA4, KeysAdd = 0x140621838, KeysShow = 0x1406A4B5C,
                    Start = 0x1406A4A10, Result = 0x1406A4894, Close = 0x1406A4B2C, FocusPush = 0x14065FF7C,
                    FocusPop = 0x14065FFE8, FocusMgr = 0x14161C8D0, StrInit = 0x1401CA8F8, StrSet = 0x1401CB71C,
                    ItemFree = 0x1404E9D3C, SoundPlay = 0x14073D860, SoundMgr = 0x141610160;
}
template <class F> F call(uintptr_t a) { return game::at<F>(a); }
int& field(uint8_t* win, int off) { return *reinterpret_cast<int*>(win + off); }

struct Request {
  bool yes_no = false;
  std::vector<std::string> items;
  Options opts;
  std::function<void(int)> done;
};

// One window's life: queued -> open (taking input) -> closing (animation) -> settle (a few quiet frames) -> next.
struct Window {
  uint8_t* obj = nullptr;
  std::vector<std::string> strings;  // item ids and title, alive as long as the window
  std::vector<const char*> item_ptrs;
  std::function<void(int)> done;
  int rows_to_move = 0, move_delay = 0;
};
std::deque<Request> g_queue;
std::optional<Window> g_open;
uint8_t* g_closing = nullptr;
int g_closing_frames = 0, g_settle_frames = 0;

void set_items_with_help(uint8_t* win, const std::vector<std::string>& ids, const std::vector<std::string>& help) {
  // The pause menu's item array: per item two of the game's 0x38-byte strings (label id, help id), 0x70 bytes, 16-aligned.
  const size_t n = ids.size();
  std::vector<uint8_t> raw(n * 0x70 + 15);
  auto* items = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(raw.data()) + 15) & ~uintptr_t{15});
  for (size_t i = 0; i < n; ++i) {
    uint8_t* label = items + i * 0x70;
    uint8_t* desc = label + 0x38;
    call<void(__fastcall*)(void*)>(va::StrInit)(label);
    call<void(__fastcall*)(void*)>(va::StrInit)(desc);
    call<void(__fastcall*)(void*, const char*, size_t)>(va::StrSet)(label, ids[i].c_str(), ids[i].size());
    call<void(__fastcall*)(void*, const char*, size_t)>(va::StrSet)(desc, help[i].c_str(), help[i].size());
  }
  int flags[4] = {0, 0x10, 0x40, 2};
  call<void(__fastcall*)(void*, int*, void*, unsigned, int, int)>(va::ItemsWithHelp)(win, flags, items, static_cast<unsigned>(n), 1, -1);
  for (size_t i = 0; i < n; ++i) call<void(__fastcall*)(void*)>(va::ItemFree)(items + i * 0x70);
  field(win, 0x1F8) = 1;                                   // update the help bar when the cursor moves
  call<void(__fastcall*)(void*, int)>(va::HelpBar)(win, 0);  // the pause menu calls this after its items too
}

void show(Request r) {
  Window w;
  w.done = std::move(r.done);
  w.strings = std::move(r.items);
  w.strings.reserve(w.strings.size() + 1);  // the title goes last; no reallocation may move the strings afterwards
  w.strings.push_back(r.opts.title_id);
  for (size_t i = 0; i + 1 < w.strings.size(); ++i) w.item_ptrs.push_back(w.strings[i].c_str());
  const char* title = w.strings.back().c_str();
  const bool has_help = !r.opts.desc_ids.empty() && r.opts.desc_ids.size() == w.item_ptrs.size();

  void* mem = call<void*(__fastcall*)(size_t, const char*, int)>(va::Alloc)(0x210, "storm4perf/replay/ui_window.cpp", __LINE__);
  uint8_t* win = call<uint8_t*(__fastcall*)(void*, const char*, int)>(va::Construct)(mem, "ReplayMenu", 0);
  if (r.yes_no) {
    call<void(__fastcall*)(void*, const char*, int)>(va::YesNo)(win, title, 1);
  } else {
    if (has_help) {
      std::vector<std::string> ids(w.strings.begin(), w.strings.end() - 1);
      set_items_with_help(win, ids, r.opts.desc_ids);
    } else {
      int flags[4] = {0, 0x10, 0x40, 2};  // the end menu's values
      call<void(__fastcall*)(void*, int*, const char**, int, int, int)>(va::Items)(win, flags, w.item_ptrs.data(),
                                                                                  static_cast<int>(w.item_ptrs.size()), 1, -1);
    }
    if (*title) call<void(__fastcall*)(void*, const char*, int)>(va::Title)(win, title, 1);
  }
  call<void(__fastcall*)(void*)>(va::Resize)(win);
  call<void(__fastcall*)(void*)>(va::KeysClear)(win);
  if (r.opts.cancel) call<void(__fastcall*)(void*, int)>(va::KeysAdd)(win, 1);  // B Cancel
  call<void(__fastcall*)(void*, int)>(va::KeysAdd)(win, 0);                       // A Confirm
  call<void(__fastcall*)(void*)>(va::KeysShow)(win);
  field(win, 0x1F4) = 1;
  if (!r.opts.cancel) field(win, 0x1D8) = 0;  // no cancel button
  call<void(__fastcall*)(void*)>(va::Start)(win);
  call<void(__fastcall*)(void*, int)>(va::FocusPush)(*game::at<void**>(va::FocusMgr), 4);
  if (has_help) call<void(__fastcall*)(void*, int)>(va::ShowItemHelp)(win, 0);
  // Setting the cursor directly does not move the highlight, so the start row is reached with Down presses after the
  // opening animation.
  w.rows_to_move = std::max(0, r.opts.start_row);
  w.move_delay = 20;
  w.obj = win;
  g_open = std::move(w);
}

void finish_open_window() {
  Window w = std::move(*g_open);
  g_open.reset();
  const int choice = call<int(__fastcall*)(void*)>(va::Result)(w.obj);
  call<void(__fastcall*)(void*)>(va::Close)(w.obj);
  call<void(__fastcall*)(void*)>(va::FocusPop)(*game::at<void**>(va::FocusMgr));
  g_closing = w.obj;
  g_closing_frames = 3;
  if (w.done) w.done(choice);  // may queue the next window
}
}  // namespace

void open(std::vector<std::string> item_ids, std::function<void(int)> on_choice, Options opts) {
  g_queue.push_back({false, std::move(item_ids), std::move(opts), std::move(on_choice)});
}

void yes_no(std::string title_id, std::function<void(bool)> on_answer) {
  Options o;
  o.title_id = std::move(title_id);
  g_queue.push_back({true, {}, std::move(o), [f = std::move(on_answer)](int c) {
                       if (f) f(c == 0);
                     }});
}

void notice(std::string title_id, std::function<void()> on_ok) {
  Options o;
  o.title_id = std::move(title_id);
  g_queue.push_back({false, {"MSG_OK"}, std::move(o), [f = std::move(on_ok)](int) {
                       if (f) f();
                     }});
}

bool is_open() { return g_open.has_value(); }
bool busy() { return g_open || g_closing || g_settle_frames > 0 || !g_queue.empty(); }

void dismiss(int code) {
  if (!g_open) return;
  field(g_open->obj, 0x1C8) = code;
  field(g_open->obj, 0x1CC) = 3;
}

std::string debug() {
  if (!g_open) return "win=none";
  uint8_t* w = g_open->obj;
  return std::format("win={} result={} state={} cancel_mask={:x} open={} help={}", static_cast<void*>(w), field(w, 0x1C8),
                     field(w, 0x1CC), field(w, 0x1D8), field(w, 0x1F4), field(w, 0x1F8));
}

void play_se(int id) {
  if (void* mgr = *game::at<void**>(va::SoundMgr))
    call<void(__fastcall*)(void*, int, int, float, int, int)>(va::SoundPlay)(mgr, id, 0, 1.0f, 0, 1);
}

void tick() {
  if (g_closing) {
    if (--g_closing_frames > 0) return;
    using DeleteFn = void(__fastcall*)(void*, int);
    reinterpret_cast<DeleteFn>((*reinterpret_cast<void***>(g_closing))[0])(g_closing, 1);
    g_closing = nullptr;
    g_settle_frames = 6;
    return;
  }
  if (g_open) {
    Window& w = *g_open;
    if (w.rows_to_move > 0 && --w.move_delay <= 0) {
      input::press(XINPUT_GAMEPAD_DPAD_DOWN, 50);
      --w.rows_to_move;
      w.move_delay = 5;
    }
    if (field(w.obj, 0x1CC) == 3) finish_open_window();
    return;
  }
  if (g_settle_frames > 0) {
    --g_settle_frames;
    return;
  }
  if (!g_queue.empty()) {
    Request r = std::move(g_queue.front());
    g_queue.pop_front();
    show(std::move(r));
  }
}
}  // namespace ssr::ui
