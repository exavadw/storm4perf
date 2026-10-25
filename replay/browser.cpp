// Replay browser. Everything on screen is the game's: the select window (ui_window), its title and help bar, the
// button guide (ccUiFlashKeyHelp *0x14161C8E8) and system sounds. Only the row text is ours.
//   Lists: Saved and Recent (L1/R1), newest first, pages of kPageRows with Next/Previous Page rows; the highlighted row's
//   details (stage, rounds, winner, length, date) in the native help bar; ★ marks the winning leader.
//   Row → Watch / Save (Recent only; Saved holds kSavedCap and never trims itself) / Delete / Back.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "browser.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "game.h"
#include "input.h"
#include "launch.h"
#include "log.h"
#include "replay.h"
#include "replay_data.h"
#include "text.h"
#include "ui_window.h"

namespace ssr::browser {
namespace {
constexpr int kPageRows = 7;
constexpr uintptr_t kKeyHelp = 0x14161C8E8;  // ccUiFlashKeyHelp*
constexpr uintptr_t kKhClear = 0x14066AB78, kKhAddTable = 0x14066AAEC, kKhAddCustom = 0x14066A9A0, kKhPush = 0x14066AE94,
                    kKhPop = 0x14066AC9C;
constexpr uintptr_t kSceneTable = 0x1414F9300;  // {u32 id; u32 pad; void* create; char name[0x80]}, stride 0x90
constexpr uintptr_t kSceneMgr = 0x1416BDA18, kInStack = 0x140AB6630;  // int InStack(mgr, id)
uint32_t g_host_id = 0xFFFFFFFF;    // ccSceneBattleModeSelect: the Free Battle top menu
std::vector<uint32_t> g_child_ids;  // scenes that sit on top of it (VS battle, survival, tournament, league, practice …)

// While the browser is up the Free Battle menu underneath must not react to the pad (it ignores the input-focus layer
// the window pushes). The game's pause stack freezes it the same way the battle pause menu freezes the battle, and the
// select window keeps working under it.
constexpr uintptr_t kTaskMgr = 0x1416BDA20, kPushPause = 0x140ABE450, kPopPause = 0x140ABE480, kPauseTop = 0x140ABE4D0;
bool g_paused = false;
void hold_menu(bool on) {
  void* mgr = *game::at<void**>(kTaskMgr);
  if (!mgr || on == g_paused) return;
  if (on) {
    const uint32_t top = game::at<uint32_t(__fastcall*)(void*)>(kPauseTop)(mgr);
    game::at<void(__fastcall*)(void*, uint32_t)>(kPushPause)(mgr, top | 0x80001);
  } else {
    game::at<void(__fastcall*)(void*)>(kPopPause)(mgr);
  }
  g_paused = on;
}

bool in_stack(uint32_t id) {
  void* mgr = *game::at<void**>(kSceneMgr);
  return mgr && game::at<int(__fastcall*)(void*, uint32_t)>(kInStack)(mgr, id) != 0;
}
bool on_host_screen() {
  if (g_host_id == 0xFFFFFFFF || !in_stack(g_host_id)) return false;
  for (uint32_t id : g_child_ids)
    if (in_stack(id)) return false;
  return true;
}

struct Entry {
  std::filesystem::path path;
  replay::Meta meta;
  int chara[2]{-1, -1}, costume[2]{-1, -1};  // starting leaders (fighter 0 of each side block)
  int support[2][2]{{-1, -1}, {-1, -1}}, support_costume[2][2]{};  // fighters 1–2 (chara <= 0: empty, single battle)
  uint32_t stage = 0;                          // CRC-32 of the stage id (BattleSelectParam+0x418)
  bool ok = false;
};

// Names as character / stage select show them, in the current text language. Message entries carry two texts:
// ccUiGetMessage 0x140AB9320 returns +0x10 (character names, stage descriptions), 0x140AB93D0 returns +8 (stage names,
// what stage select's banner shows).
const char* message(const char* id) { return game::at<const char*(__fastcall*)(const char*)>(0x140AB9320)(id); }
const char* message_name(const char* id) { return game::at<const char*(__fastcall*)(const char*)>(0x140AB93D0)(id); }
std::string chara_text(int chara, int costume) {
  auto* base = *game::at<uint8_t**>(0x14161C890);
  void* psp = base ? *reinterpret_cast<void**>(base + 0x100) : nullptr;
  if (!psp || chara < 0) return "?";
  // (psp, &chara, &costume, variant): -1 = any variant (the 4th argument is compared when != -1)
  const int preset = game::at<int(__fastcall*)(void*, const int*, const int*, int)>(0x1406C7154)(psp, &chara, &costume, -1);
  auto* ps = preset ? game::at<uint8_t*(__fastcall*)(void*, int)>(0x1406C7258)(psp, preset) : nullptr;
  if (!ps) return "?";
  const char* id = *reinterpret_cast<const char**>(ps + 0x20);  // c_cha_a: the character's name
  const char* text = id && *id ? message(id) : nullptr;
  return text && *text ? text : "?";
}
std::string stage_text(const Entry& e) {
  std::string id = e.meta.stage_name_id;  // c_sta_NN, stored at recording time
  if (id.empty()) {                       // older files: the stage select list, if it is loaded right now
    if (auto* list = *game::at<uint8_t**>(0x14161C938)) {
      auto* head = *reinterpret_cast<uint8_t**>(list + 8);
      using Crc = uint32_t(__fastcall*)(const char*);
      for (auto* n = head ? *reinterpret_cast<uint8_t**>(head) : nullptr; n && n != head; n = *reinterpret_cast<uint8_t**>(n))
        if (game::at<Crc>(0x140A01EB0)(reinterpret_cast<const char*>(n + 0x38)) == e.stage) id = reinterpret_cast<const char*>(n + 0x18);
    }
  }
  if (id.empty()) return {};
  const char* t = message_name(id.c_str());
  return t ? t : "";
}

enum Tab { Saved = 0, Recent = 1 };
const char* kTabDir[] = {"saved", "recent"};
std::vector<Entry> g_entries[2];
int g_tab = Saved, g_page[2]{};
std::filesystem::path g_last;  // last replay opened from the list: the list comes back to it
bool g_list_open = false;
std::function<bool()> g_launch;
bool g_kh_pushed = false;
int g_host_frames = 0;
uint16_t g_prev_buttons = 0;
const char* kWinMark = "\xE2\x98\x85";  // ★ before the winning leader

template <class F> F fn(uintptr_t va) { return game::at<F>(va); }

std::string local_time(const std::string& iso_utc) {  // "2026-10-16T23:26:21Z" → "10/16 16:26" local
  SYSTEMTIME u{};
  if (std::sscanf(iso_utc.c_str(), "%hu-%hu-%huT%hu:%hu:%hu", &u.wYear, &u.wMonth, &u.wDay, &u.wHour, &u.wMinute, &u.wSecond) != 6) return iso_utc;
  SYSTEMTIME l{};
  if (!SystemTimeToTzSpecificLocalTime(nullptr, &u, &l)) l = u;
  return std::format("{}/{} {:02}:{:02}", l.wMonth, l.wDay, l.wHour, l.wMinute);
}

std::string row_text(const Entry& e) {
  if (!e.ok) return e.path.filename().string() + " (unreadable)";
  const bool p1 = e.meta.result == 2, p2 = e.meta.result == 3;
  return std::format("{}{} vs {}{}", p1 ? kWinMark : "", chara_text(e.chara[0], e.costume[0]), p2 ? kWinMark : "",
                     chara_text(e.chara[1], e.costume[1]));
}
// Team battle: either side brought supports. The fighter on screen can change mid-battle (switching), so names here
// are the starting leaders and the winner is a team.
bool team_battle(const Entry& e) {
  for (auto& side : e.support)
    for (int c : side)
      if (c > 0) return true;
  return false;
}
std::string team_text(const Entry& e, int side) {
  std::string out = chara_text(e.chara[side], e.costume[side]);
  for (int k = 0; k < 2; ++k)
    if (e.support[side][k] > 0) out += ", " + chara_text(e.support[side][k], e.support_costume[side][k]);
  return out;
}
// Native help bar under the list; the actions window title puts the stage on its own line (stage_sep "\n").
std::string detail_text(const Entry& e, const char* stage_sep = "   ") {
  if (!e.ok) return {};
  const auto& m = e.meta;
  std::string out = stage_text(e);
  const char* sep = out.empty() ? "" : stage_sep;
  auto add = [&](const std::string& part) {
    if (part.empty()) return;
    out += sep;
    out += part;
    sep = "   ";
  };
  const bool team = team_battle(e);
  add(team ? "Team Battle" : "Single Battle");
  add(std::format("{} round{}", m.rounds, m.rounds == 1 ? "" : "s"));
  if (m.result == 2 || m.result == 3)
    add(chara_text(e.chara[m.result - 2], e.costume[m.result - 2]) + (team ? "'s team wins" : " wins"));
  if (m.fps) add(std::format("{}:{:02}", m.ticks / m.fps / 60, m.ticks / m.fps % 60));
  add(local_time(m.created));
  return out;
}

void scan() {
  for (int tab : {Saved, Recent}) {
    g_entries[tab].clear();
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto& f : std::filesystem::directory_iterator(replay::folder(kTabDir[tab]), ec))
      if (f.path().extension() == L".nsreplay") files.push_back(f.path());
    std::sort(files.rbegin(), files.rend());  // names are UTC timestamps: newest first
    for (auto& f : files) {
      Entry e;
      e.path = f;
      replay::Recording r;
      std::string err;
      e.ok = replay::read_file(f.wstring(), r, &err);
      e.meta = r.meta;
      if (r.bsp.size() == replay::kBspSize) {
        for (int side = 0; side < 2; ++side) {
          const auto* fighter = reinterpret_cast<const int32_t*>(r.bsp.data() + 0xC0 + side * 0x1AC);
          e.chara[side] = fighter[0];
          e.costume[side] = fighter[1];
          for (int k = 0; k < 2; ++k) {
            const auto* sup = reinterpret_cast<const int32_t*>(r.bsp.data() + 0xC0 + side * 0x1AC + (k + 1) * 0x3C);
            e.support[side][k] = sup[0];
            e.support_costume[side][k] = sup[1];
          }
        }
        std::memcpy(&e.stage, r.bsp.data() + 0x418, 4);
      }
      g_entries[tab].push_back(std::move(e));
    }
  }
}

void show_page(int start_row = 0);

// Copy a Recent replay into Saved. Saved never trims itself: when it is full, the player decides.
void save_entry(const Entry& e, std::function<void()> then) {
  auto copy = [path = e.path, then] {
    std::error_code ec;
    const auto dest = replay::folder("saved") / path.filename();
    std::filesystem::copy_file(path, dest, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return ui::notice("rpl_err_write", then);
    log::info("browser: saved {}", dest.filename().string());
    scan();
    ui::notice("rpl_replay_saved", then);
  };
  if (replay::count("saved") < replay::kSavedCap) return copy();
  ui::yes_no("rpl_saved_full_q", [copy, then](bool yes) {
    if (!yes) return then();
    if (!g_entries[Saved].empty()) {  // oldest saved replay = last in the newest-first list
      std::error_code ec;
      std::filesystem::remove(g_entries[Saved].back().path, ec);
    }
    copy();
  });
}

void show_actions(int tab, size_t index) {
  const Entry& e = g_entries[tab][index];
  g_last = e.path;
  std::string title = std::format("{}\n{}", row_text(e), detail_text(e, "\n"));
  if (team_battle(e)) title += std::format("\n{}\nvs\n{}", team_text(e, 0), team_text(e, 1));
  text::set("rpl_act_title", title);
  std::vector<std::string> ids{"rpl_act_play"};
  if (tab == Recent) ids.push_back("rpl_act_save");
  ids.push_back("rpl_act_delete");
  ids.push_back("MSG_Back");
  ui::Options o;
  o.title_id = "rpl_act_title";
  for (const auto& id : ids) o.desc_ids.push_back(id == "MSG_Back" ? "rpl_act_back_desc" : id + "_desc");
  const int row = static_cast<int>(index) % kPageRows;
  ui::open(ids, [tab, index, ids, row](int c) {
    if (c < 0 || ids[c] == "MSG_Back") return show_page(row);
    const Entry& e = g_entries[tab][index];
    if (ids[c] == "rpl_act_play") {
      const std::string err = replay::arm_file(e.path);
      if (!err.empty()) return ui::notice(err, [row] { show_page(row); });
      hold_menu(false);                    // the menu must run again to take the launch
      if (g_launch && g_launch()) return;  // the battle loads; the browser is done
      ui::notice("rpl_start_vs");          // no direct launch from here: the next VS battle loads as the replay
      return;
    }
    if (ids[c] == "rpl_act_save") return save_entry(e, [row] { show_page(row); });
    if (ids[c] == "rpl_act_delete") {
      return ui::yes_no("rpl_delete_q", [tab, index, row](bool yes) {
        if (yes) {
          std::error_code ec;
          std::filesystem::remove(g_entries[tab][index].path, ec);
          log::info("browser: deleted {}", g_entries[tab][index].path.filename().string());
          scan();
        }
        show_page(yes ? 0 : row);
      });
    }
  }, o);
}

void show_page(int start_row) {
  auto& list = g_entries[g_tab];
  const int pages = std::max(1, (static_cast<int>(list.size()) + kPageRows - 1) / kPageRows);
  int& page = g_page[g_tab];
  page = std::clamp(page, 0, pages - 1);
  std::vector<std::string> ids, descs;
  std::vector<int> map;  // item → entry index, or -1 next, -2 previous, -3 nothing
  for (int i = page * kPageRows; i < std::min<int>(static_cast<int>(list.size()), (page + 1) * kPageRows); ++i) {
    const std::string id = std::format("rpl_row_{}", ids.size()), desc = std::format("rpl_desc_{}", ids.size());
    text::set(id.c_str(), row_text(list[i]));
    text::set(desc.c_str(), detail_text(list[i]));
    ids.push_back(id);
    descs.push_back(desc);
    map.push_back(i);
  }
  if (list.empty()) ids.push_back("rpl_none"), descs.push_back("rpl_none_desc"), map.push_back(-3);
  if (page + 1 < pages) ids.push_back("rpl_next_page"), descs.push_back("rpl_next_page_desc"), map.push_back(-1);
  if (page > 0) ids.push_back("rpl_prev_page"), descs.push_back("rpl_prev_page_desc"), map.push_back(-2);
  const int count = static_cast<int>(list.size());
  const std::string counter = g_tab == Saved ? std::format("{}/{}", count, replay::kSavedCap) : std::format("{}/{}", count, replay::keep_recent());
  const std::string pager = pages > 1 ? std::format("   Page {}/{}", page + 1, pages) : std::string();
  text::set("rpl_browser_title", std::format("<icon btn_l1 />  {} Replays  {}  <icon btn_r1 />{}", g_tab == Saved ? "Saved" : "Recent", counter, pager));
  ui::Options o;
  o.title_id = "rpl_browser_title";
  o.desc_ids = descs;
  o.start_row = std::clamp(start_row, 0, static_cast<int>(ids.size()) - 1);
  g_list_open = true;
  ui::open(ids, [map](int c) {
    g_list_open = false;
    if (c == -2 || c == -3) {  // L1 / R1: the other list
      g_tab = g_tab == Saved ? Recent : Saved;
      ui::play_se(0x17);
      return show_page();
    }
    if (c < 0) return;  // B: close the browser
    if (map[c] == -1) return ++g_page[g_tab], show_page();
    if (map[c] == -2) return --g_page[g_tab], show_page();
    if (map[c] == -3) return show_page();
    show_actions(g_tab, static_cast<size_t>(map[c]));
  }, o);
}

// Opens the list on the replay last opened from it (after "Return to Replays"), else on Recent's newest.
void open_at_last() {
  for (int tab : {Saved, Recent})
    for (size_t i = 0; i < g_entries[tab].size(); ++i)
      if (!g_last.empty() && g_entries[tab][i].path == g_last) {
        g_tab = tab;
        g_page[tab] = static_cast<int>(i) / kPageRows;
        return show_page(static_cast<int>(i) % kPageRows);
      }
  show_page();
}

void push_key_help() {
  void* kh = *game::at<void**>(kKeyHelp);
  if (!kh || g_kh_pushed) return;
  fn<void(__fastcall*)(void*)>(kKhClear)(kh);
  fn<void(__fastcall*)(void*, int)>(kKhAddTable)(kh, 0);  // Confirm
  fn<void(__fastcall*)(void*, int)>(kKhAddTable)(kh, 7);  // Back
  fn<void(__fastcall*)(void*, const char*, const char*)>(kKhAddCustom)(kh, "<icon btn_4 />", "Replays");
  fn<void(__fastcall*)(void*, int)>(kKhPush)(kh, 0);
  g_kh_pushed = true;
}
void pop_key_help() {
  void* kh = *game::at<void**>(kKeyHelp);
  if (!kh || !g_kh_pushed) return;
  fn<void(__fastcall*)(void*)>(kKhPop)(kh);
  g_kh_pushed = false;
}
}  // namespace

void install() {
  for (auto* e = game::at<const uint8_t*>(kSceneTable); *reinterpret_cast<const uint32_t*>(e) != 0xFFFFFFFF; e += 0x90) {
    const uint32_t id = *reinterpret_cast<const uint32_t*>(e);
    const std::string name = reinterpret_cast<const char*>(e + 0x10);
    if (name == "ccSceneBattleModeSelect") g_host_id = id;
    else if (name.find("FreeBattle") != std::string::npos || name.find("Survival") != std::string::npos ||
             name.find("Tournament") != std::string::npos || name.find("League") != std::string::npos ||
             name.find("Practice") != std::string::npos)
      g_child_ids.push_back(id);
  }
  log::info("browser: host scene {} with {} child scenes", g_host_id, g_child_ids.size());
  text::set("rpl_act_play", "Watch");
  text::set("rpl_act_save", "Save");
  text::set("rpl_act_delete", "Delete");
  text::set("rpl_act_play_desc", "Watch this replay.");
  text::set("rpl_act_save_desc", "Keep this replay in Saved Replays.");
  text::set("rpl_act_delete_desc", "Delete this replay.");
  text::set("rpl_act_back_desc", "Return to the replay list.");
  text::set("rpl_next_page", "Next Page");
  text::set("rpl_prev_page", "Previous Page");
  text::set("rpl_next_page_desc", "Show older replays.");
  text::set("rpl_prev_page_desc", "Show newer replays.");
  text::set("rpl_none", "No Replays");
  text::set("rpl_none_desc", "Battles you fight in VS Battle are recorded here.");
  text::set("rpl_saved_full_q", "Saved replays are full. Delete the oldest saved replay to make room?");
  text::set("rpl_err_saved_full", "Saved replays are full. Delete one in Replays first.");
  text::set("rpl_saved_full_item", "Saved Replays Full");
  text::set("rpl_delete_q", "Delete this replay?");
  text::set("rpl_empty", "There are no replays yet.");
  text::set("rpl_save_replay", "Save Replay");
  text::set("rpl_replay_saved", "Replay Saved");
  text::set("rpl_start_vs", "Start a VS Battle. It will load as this replay.");
  text::set("rpl_err_busy", "A replay is already in progress.");
  text::set("rpl_err_read", "This replay could not be read.");
  text::set("rpl_err_version", "This replay was recorded on a different version of the game.");
  text::set("rpl_err_fps", "This replay was recorded with a different frame rate setting.");
  text::set("rpl_err_write", "The replay could not be saved.");
  text::set("rpl_err_nothing", "There is no battle to save.");
}

void set_launcher(std::function<bool()> launch) { g_launch = std::move(launch); }

void open() {
  hold_menu(true);
  scan();
  ui::play_se(0x18);
  open_at_last();
}

void tick() {
  if (g_paused && !ui::busy()) hold_menu(false);  // browser closed (or launched a replay)
  const bool on_host = on_host_screen();
  g_host_frames = on_host && launch::menu_settled() ? g_host_frames + 1 : 0;  // menu fully in (see launch.h)
  const bool show = g_host_frames > 30 && !ui::busy() && !launch::active() && !replay::viewing();
  if (show && !g_kh_pushed) push_key_help();
  if (!on_host && g_kh_pushed) pop_key_help();
  const uint16_t buttons = static_cast<uint16_t>(input::real_buttons() | input::virtual_buttons());
  const uint16_t pressed = buttons & ~g_prev_buttons;
  g_prev_buttons = buttons;
  if (g_list_open && ui::is_open()) {  // L1 / R1 switch between Saved and Recent
    if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) ui::dismiss(-2);
    else if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) ui::dismiss(-3);
  }
  if (show && replay::take_left_to_menu()) open();  // back from "Exit Replay": the list again
  else if (show && (pressed & XINPUT_GAMEPAD_Y)) open();
}

std::string dump(bool recent) {  // dev: raw leader ids per entry
  scan();
  std::string o = "ok";
  for (const auto& e : g_entries[recent ? Recent : Saved])
    o += std::format(" ;; {} c0={}/{} c1={}/{} {}", e.path.filename().string(), e.chara[0], e.costume[0], e.chara[1], e.costume[1],
                     row_text(e));
  return o;
}

std::string status() {
  return std::format("ok host_frames={} key_help={} tab={} saved={} recent={} ui_busy={} {}", g_host_frames, g_kh_pushed,
                     g_tab == Saved ? "saved" : "recent", g_entries[Saved].size(), g_entries[Recent].size(), ui::busy(), ui::debug());
}

}  // namespace ssr::browser
