// Input replay core.
//
// Native pieces (1.09, verified by decompile):
//   ccPlayerPad::Ctrl 0x1407C03F0 — after choosing local pad vs online buffer, if the global ccBattleRecord*
//   0x1415F82E0 is set: rec+0x4B0 == 1 → Record(rec, padFrame*, side) 0x140524D6C; == 2 → Play(...) 0x140524DE8.
//   side = ccPlayerPad+0x3F8 (controller side). CPU leaders never reach this tap (they run CtrlCpu).
//   ccGame::Finalize calls DeleteInstance 0x140524DC0: vtbl[0](rec, 1), then nulls the global.
// The release exe never creates a recorder, so we install our own object (only +0x00 vtbl and +0x4B0 mode are read
// outside Record/Play) and hook Record/Play so the streams are sequential per side rather than frame-indexed.
//   Battle-main init 0x1406D4FC4(scene) runs once per round; the duel's win counters tell a new battle from a round.
//   0x14074031C(BattleSelectParam*) copies the battle setup into the duel globals (VSLoading calls it before the loader
//   requests fighters); playback from a file writes the recorded setup into it first and empties the fighter slots
//   character select preloaded, so the battle loads the replay's fighters and stage.
#include "replay.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <intrin.h>
#include <cstring>
#include <format>
#include <sstream>
#include <thread>

#include "game.h"
#include "hooks.h"
#include "log.h"
#include "replay_data.h"

namespace ssr::replay {
namespace {
constexpr uintptr_t kRecorderGlobal = 0x1415F82E0;
constexpr uintptr_t kRecord = 0x140524D6C;
constexpr uintptr_t kPlay = 0x140524DE8;
constexpr uintptr_t kBattleMainInit = 0x1406D4FC4;  // void(scene*): battle-main base Initialize; passes RCX on to 0x1406D4BF8
// Round 1's timer starts in the Begin scene: "Fight" 0x1405B8088(begin scene) sets duel+0x244 = 1 (HUD on, timer runs)
// 20–28 ticks before battle-main init, a gap whose length is wall-clock timed (Begin waits for its UI). Recording and
// playback of a new battle start here, so those ticks are recorded and the RNG is snapshotted at the timer start.
constexpr uintptr_t kFight = 0x1405B8088;
constexpr uintptr_t kJudgeUpdate = 0x14074D248;     // ccDuelJudge::Update(judge), once per duel tick
constexpr uintptr_t kApplyBattleParam = 0x14074031C;  // (BattleSelectParam*) → duel globals
constexpr uintptr_t kMtState = 0x1416B7240;         // MT19937 u32[624] (gameplay RNG)
constexpr uintptr_t kMtIndex = 0x141561598;         // u32
constexpr uintptr_t kAsyncLcg = 0x1416660C0;        // u64 cosmetic LCG
constexpr uintptr_t kGetPlayerLeader = 0x14085B548;  // void* (int side)
constexpr uintptr_t kVecLoad = 0x140A04690;          // (float out[3], const void* src) — used by the net sync check
constexpr uintptr_t kDuelManager = 0x14161C8C8;      // ccDuelManager*; +0xF0 feeds the net sync check
constexpr uintptr_t kDuelWins = 0x12249C;            // duel+: u32 wins P1, +4 wins P2 (round = sum + 1)
constexpr uintptr_t kBattleMode = 0x141611F70;       // 0 free battle, 3 online, 5 practice, …
constexpr uintptr_t kNuccSys = 0x1416663C8;          // nuccSys*; +0x942 u8 logic fps
constexpr uintptr_t kEndMenuState = 0x1406D1F64;    // ccSceneBattleEndBase menu state (scene*, sub); action → scene+0x134
constexpr uintptr_t kPauseAction = 0x14067F380;     // int (pause menu*): action of the decided item (2 retry, 7 char select, 8 menu)
constexpr uintptr_t kLoaderSlots = 0x250, kLoaderSlotSize = 0x8F48;  // duel+: ccDuelPlayerLoader[6] (side*3 + fighter); +0x08 char id
constexpr uintptr_t kLoaderUnload = 0x140750908;     // ccDuelPlayerLoader::Unload(slot)
constexpr uintptr_t kPropManager = 0x1416BDA28, kPropFind = 0x140AB7930;  // ccGameProperty
constexpr uintptr_t kBspTypeTag = 0x1415BF188;      // value->vt[1]() of a BattleSelectParam property
constexpr size_t kBspConditionsBegin = 0x18, kBspConditionsEnd = 0xC0;  // story condition pointers: never copied

enum class Mode { Idle, ArmedRec, Recording, ArmedPlay, Playing };
const char* mode_name(Mode m) {
  switch (m) {
    case Mode::Idle: return "idle";
    case Mode::ArmedRec: return "armed_rec";
    case Mode::Recording: return "recording";
    case Mode::ArmedPlay: return "armed_play";
    case Mode::Playing: return "playing";
  }
  return "?";
}

Config g_cfg;
Mode g_mode = Mode::Idle;
Recording g_rec;   // current or last finished recording
Recording g_play;  // replay being watched
std::string g_play_name;
std::filesystem::path g_play_path;
bool g_decided = false;     // playback: this round's result is decided
bool g_timer_running = false;
size_t g_round_start = 0;  // playback tick where the current round began
constexpr size_t kSettleTicks = 180;  // ~6 s at 30 fps: round intro and "Fight!" banner  // judge+0xD4 on the previous duel tick
size_t g_next_anchor = 0;
bool g_viewing = false, g_play_done = false, g_norestore = false, g_globals_checked = false;
std::vector<uint8_t> g_applied_bsp;  // the BattleSelectParam the loaders last applied (this battle's setup)
size_t g_seg = 0, g_tick = 0, g_cursor[2]{}, g_overrun = 0, g_mismatches = 0, g_gameplay_mismatches = 0, g_tail_extra = 0;
int64_t g_first_mismatch = -1, g_first_pad_tick_skew = -1, g_resync_drift = 0;
std::string g_first_mismatch_what, g_verdict, g_last_saved;
uint32_t g_inits = 0, g_deletes = 0, g_restarts = 0;
uint32_t g_pre_fight_ticks = 0;  // duel ticks since the battle setup was applied (Begin runs a fixed count before Fight)
Segment g_pre_snap;              // RNG on this battle's first duel tick (recorded with the battle; see Recording::pre)
bool g_pre_have = false;
bool g_started_at_fight = false;  // this battle's recording/playback began at "Fight"; battle-main init must not restart it

// Our recorder object. Only vtbl (+0x00) and mode (+0x4B0) are read outside the hooked Record/Play.
alignas(16) uint8_t g_obj[0x4D8]{};
void* __fastcall obj_delete(void* self, int);
void* g_vtbl[4] = {reinterpret_cast<void*>(&obj_delete), nullptr, nullptr, nullptr};

void** recorder_global() { return game::at<void**>(kRecorderGlobal); }
void install_object(int mode) {
  std::memset(g_obj, 0, sizeof g_obj);
  *reinterpret_cast<void***>(g_obj) = g_vtbl;
  *reinterpret_cast<int32_t*>(g_obj + 0x4B0) = mode;
  *recorder_global() = g_obj;
}
void remove_object() {
  if (*recorder_global() == g_obj) *recorder_global() = nullptr;
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
  auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

TickHash compute(const void* judge) {
  TickHash t;
  t.mt = fnv(game::at<const void*>(kMtState), kMtWords * 4);
  t.mt = fnv(game::at<const void*>(kMtIndex), 4, t.mt);
  using GetLeader = void*(__fastcall*)(int);
  using VecLoad = void(__fastcall*)(float*, const void*);
  using GetF = float(__fastcall*)(void*);
  t.pos = t.life = 1469598103934665603ull;
  for (int side = 0; side < 2; ++side) {
    void* leader = game::at<GetLeader>(kGetPlayerLeader)(side);
    if (!leader) continue;
    alignas(16) float v[4]{};
    game::at<VecLoad>(kVecLoad)(v, static_cast<uint8_t*>(leader) + 0x70);
    t.pos = fnv(v, 12, t.pos);
    void** vt = *reinterpret_cast<void***>(leader);
    const float life = reinterpret_cast<GetF>(vt[0xC98 / 8])(leader);
    t.life = fnv(&life, 4, t.life);
  }
  if (void* duel = *game::at<void**>(kDuelManager)) t.pos = fnv(static_cast<uint8_t*>(duel) + 0xF0, 4, t.pos);
  t.judge = fnv(static_cast<const uint8_t*>(judge) + 0x78, 0x48);  // time left, per-round frame counters, result
  return t;
}

Segment snapshot(const Recording& r) {
  Segment s;
  std::memcpy(s.mt.data(), game::at<const void*>(kMtState), kMtWords * 4);
  s.mt_index = *game::at<const uint32_t*>(kMtIndex);
  s.lcg = *game::at<const uint64_t*>(kAsyncLcg);
  s.tick0 = static_cast<uint32_t>(r.hashes.size());
  s.cursor0[0] = static_cast<uint32_t>(r.pads[0].size());
  s.cursor0[1] = static_cast<uint32_t>(r.pads[1].size());
  return s;
}
void restore(const Segment& s) {
  std::memcpy(game::at<void*>(kMtState), s.mt.data(), kMtWords * 4);
  *game::at<uint32_t*>(kMtIndex) = s.mt_index;
  *game::at<uint64_t*>(kAsyncLcg) = s.lcg;
}

// BattleSelectParam as the battle loaders fetch it (0x14073F058): holder = Find(mgr, name); value = holder+8;
// value->vt[1]() must be the ccBattleSelResult type tag; the object is value+0x10.
uint8_t* battle_select_param() {
  void* mgr = *game::at<void**>(kPropManager);
  if (!mgr) return nullptr;
  auto* holder = game::at<uint8_t*(__fastcall*)(void*, const char*)>(kPropFind)(mgr, "BattleSelectParam");
  auto* value = holder ? *reinterpret_cast<uint8_t**>(holder + 8) : nullptr;
  if (!value) return nullptr;
  using TagFn = void*(__fastcall*)(void*);
  if (reinterpret_cast<TagFn>((*reinterpret_cast<void***>(value))[1])(value) != game::at<void*>(kBspTypeTag)) return nullptr;
  return *reinterpret_cast<uint8_t**>(value + 0x10);
}

void write_replay_setup(uint8_t* bsp, const char* where) {
  std::memcpy(bsp, g_play.bsp.data(), kBspConditionsBegin);
  std::memcpy(bsp + kBspConditionsEnd, g_play.bsp.data() + kBspConditionsEnd, kBspSize - kBspConditionsEnd);
  log::info("replay: battle setup replaced with the replay's at {}", where);
}

// The round count (MatchSelect "Rounds" + 1) lives in its own global, not in BattleSelectParam; SetupDuelCondition
// 0x14074E808 copies it into the duel at loader state 5, after 0x14074031C.
constexpr uintptr_t kRoundsGlobal = 0x141611F78;
void restore_rounds() {
  const size_t off = kRoundsGlobal - kGlobalsBegin;
  if (g_play.globals.size() < off + 4) return;
  *game::at<uint32_t*>(kRoundsGlobal) = *reinterpret_cast<const uint32_t*>(g_play.globals.data() + off);
}

// Stage select's list (*0x14161C938, std::list of nodes: +0x18 "nameid" — it resolves to the stage description — and
// +0x38 stage id) maps the stage hash to that id; the list is only loaded around stage select, so it is stored with
// the recording.
std::string stage_name_id(uint32_t hash) {
  auto* list = *game::at<uint8_t**>(0x14161C938);
  if (!list) return {};
  auto* head = *reinterpret_cast<uint8_t**>(list + 8);
  using Crc = uint32_t(__fastcall*)(const char*);
  for (auto* n = head ? *reinterpret_cast<uint8_t**>(head) : nullptr; n && n != head; n = *reinterpret_cast<uint8_t**>(n))
    if (game::at<Crc>(0x140A01EB0)(reinterpret_cast<const char*>(n + 0x38)) == hash) return reinterpret_cast<const char*>(n + 0x18);
  return {};
}

uint32_t battle_mode() { return *game::at<const uint32_t*>(kBattleMode); }
uint32_t logic_fps() {
  auto* sys = *game::at<uint8_t**>(kNuccSys);
  return sys ? sys[0x942] : 0;
}
int duel_wins() {
  auto* duel = *game::at<uint8_t**>(kDuelManager);
  if (!duel) return -1;
  return *reinterpret_cast<int*>(duel + kDuelWins) + *reinterpret_cast<int*>(duel + kDuelWins + 4);
}

std::string utc_now(bool for_file) {
  SYSTEMTIME t;
  GetSystemTime(&t);
  return for_file ? std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond)
                  : std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

std::filesystem::path dir(const char* sub) {
  auto d = g_cfg.root / L"replays" / sub;
  std::error_code ec;
  std::filesystem::create_directories(d, ec);
  return d;
}

void trim_recent() {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (auto& e : std::filesystem::directory_iterator(dir("recent"), ec))
    if (e.path().extension() == L".nsreplay") files.push_back(e.path());
  std::sort(files.begin(), files.end());  // names are UTC timestamps
  while (static_cast<int>(files.size()) > g_cfg.keep_recent) {
    std::filesystem::remove(files.front(), ec);
    files.erase(files.begin());
  }
}

void write_async(Recording r, std::filesystem::path path, bool trim) {
  std::thread([r = std::move(r), path = std::move(path), trim] {
    std::string err;
    if (write_file(path.wstring(), r, &err)) log::info("replay: wrote {} ({} ticks)", path.filename().string(), r.meta.ticks);
    else log::error("replay: writing {} failed: {}", path.string(), err);
    if (trim) trim_recent();
  }).detach();
}

std::string summary() {
  std::string s = std::format("mode={} inits={} deletes={} restarts={} rec_segs={} rec_ticks={} rec_pads={}/{}", mode_name(g_mode), g_inits,
                              g_deletes, g_restarts, g_rec.segs.size(), g_rec.hashes.size(), g_rec.pads[0].size(), g_rec.pads[1].size());
  if (!g_play.segs.empty())
    s += std::format(" | play={} viewing={} play_seg={} play_tick={}/{} cursor={}/{} of {}/{} overrun={} mismatches={} first_mismatch={} {} "
                     "pad_tick_skew={} resync_drift={} gameplay_mismatches={} unlive_ticks={} verdict={}",
                     g_play_name.empty() ? "memory" : g_play_name, g_viewing, g_seg, g_tick, g_play.hashes.size(), g_cursor[0],
                     g_cursor[1], g_play.pads[0].size(), g_play.pads[1].size(), g_overrun, g_mismatches, g_first_mismatch,
                     g_first_mismatch_what, g_first_pad_tick_skew, g_resync_drift, g_gameplay_mismatches, g_tail_extra,
                     g_verdict.empty() ? "-" : g_verdict);
  return s;
}

// ---- recording ----
void start_recording() {
  g_rec = {};
  auto& m = g_rec.meta;
  m.created = utc_now(false);
  m.exe_sha256 = game::info().sha256;
  m.fps = logic_fps();
  m.mode = battle_mode();
  m.start = g_started_at_fight ? "fight" : "";
  if (g_started_at_fight && g_pre_have) g_rec.pre.push_back(g_pre_snap);
  if (g_applied_bsp.size() == kBspSize) {
    g_rec.bsp = g_applied_bsp;
    const uint8_t* bsp = g_rec.bsp.data();
    m.stage = *reinterpret_cast<const uint32_t*>(bsp + 0x418);
    m.stage_name_id = stage_name_id(m.stage);
    for (int side = 0; side < 2; ++side) {
      const uint8_t* leader = bsp + 0xC0 + side * 0x1AC;
      m.chara[side] = *reinterpret_cast<const int32_t*>(leader + 0x00);
      m.preset[side] = *reinterpret_cast<const int32_t*>(leader + 0x08);
    }
  } else {
    log::warn("replay: no battle setup was applied before this battle; this replay can only be played via Rematch");
  }
  auto* g = game::at<const uint8_t*>(kGlobalsBegin);
  g_rec.globals.assign(g, g + (kGlobalsEnd - kGlobalsBegin));
  g_rec.segs.push_back(snapshot(g_rec));
  g_timer_running = false;
  install_object(1);
  g_mode = Mode::Recording;
  log::info("replay: recording started (mode={} fps={} stage={} chara={}/{} mt_index={})", m.mode, m.fps, m.stage, m.chara[0], m.chara[1],
            g_rec.segs.back().mt_index);
}

void finish_recording(const char* why) {
  if (g_mode != Mode::Recording) return;
  auto& m = g_rec.meta;
  m.ticks = static_cast<uint32_t>(g_rec.hashes.size());
  m.rounds = static_cast<uint32_t>(g_rec.segs.size());
  m.result = g_rec.judge_raw.empty() ? 0 : g_rec.judge_raw.back()[17];
  g_mode = Mode::Idle;
  remove_object();
  log::info("replay: recording finished ({}): {} ticks, {} rounds, pads {}/{}", why, m.ticks, m.rounds, g_rec.pads[0].size(),
            g_rec.pads[1].size());
  if (m.ticks > 0) write_async(g_rec, dir("recent") / (utc_now(true) + ".nsreplay"), true);
}

// ---- playback ----
void finish_playback() {
  const bool complete = g_seg + 1 >= g_play.segs.size() && (g_decided || g_tick >= g_play.hashes.size());
  g_verdict = !complete ? "INCOMPLETE" : g_mismatches == 0 ? "IDENTICAL" : g_gameplay_mismatches == 0 ? "GAMEPLAY-IDENTICAL" : "DIVERGED";
  g_play_done = true;
  log::info("replay: playback finished: {} -> {}", summary(), g_verdict);
}

void start_playback() {
  g_mode = Mode::Playing;
  g_viewing = true;
  g_play_done = false;
  g_seg = 0;
  g_tick = 0;
  g_cursor[0] = g_cursor[1] = 0;
  g_overrun = g_mismatches = g_gameplay_mismatches = g_tail_extra = 0;
  g_first_mismatch = g_first_pad_tick_skew = -1;
  g_resync_drift = 0;
  g_first_mismatch_what.clear();
  g_verdict.clear();
  g_decided = false;
  g_timer_running = false;
  g_next_anchor = 0;
  g_round_start = 0;
  if (!g_norestore) restore(g_play.segs[0]);
  install_object(2);
  log::info("replay: playback started ({} ticks, {} rounds{})", g_play.hashes.size(), g_play.segs.size(), g_norestore ? ", no RNG restore" : "");
}

void next_segment() {
  if (++g_seg >= g_play.segs.size()) {
    log::warn("replay: round {} beyond the recorded {}", g_seg + 1, g_play.segs.size());
    return;
  }
  const auto& s = g_play.segs[g_seg];
  log::info("replay: round {} at tick {} (recorded {}), cursors {}/{} (recorded {}/{})", g_seg + 1, g_tick, s.tick0, g_cursor[0], g_cursor[1],
            s.cursor0[0], s.cursor0[1]);
  if (!g_norestore) restore(s);
  // Round transitions take a wall-clock-dependent number of ticks; resync to where this round began.
  const int64_t drift = static_cast<int64_t>(g_tick) - static_cast<int64_t>(s.tick0);
  g_resync_drift += drift < 0 ? -drift : drift;
  g_tick = s.tick0;
  g_cursor[0] = s.cursor0[0];
  g_cursor[1] = s.cursor0[1];
  g_round_start = g_tick;
  g_decided = false;
  g_timer_running = false;
  while (g_next_anchor < g_play.anchors.size() && g_play.anchors[g_next_anchor].seg < g_seg) ++g_next_anchor;
}

// The round timer starts: what ran before it is UI-timed, so tick, pad cursors and RNG snap to the recording's next
// anchor in this round.
void snap_to_anchor() {
  if (g_next_anchor >= g_play.anchors.size() || g_play.anchors[g_next_anchor].seg != g_seg) return;
  const Anchor& a = g_play.anchors[g_next_anchor++];
  const int64_t drift = static_cast<int64_t>(g_tick) - static_cast<int64_t>(a.tick);
  g_resync_drift += drift < 0 ? -drift : drift;
  if (drift || g_cursor[0] != a.cursor0[0] || g_cursor[1] != a.cursor0[1])
    log::info("replay: timer starts at tick {} (recorded {}), cursors {}/{} (recorded {}/{})", g_tick, a.tick, g_cursor[0], g_cursor[1],
              a.cursor0[0], a.cursor0[1]);
  if (!g_norestore) {
    std::memcpy(game::at<void*>(kMtState), a.mt.data(), kMtWords * 4);
    *game::at<uint32_t*>(kMtIndex) = a.mt_index;
    *game::at<uint64_t*>(kAsyncLcg) = a.lcg;
  }
  g_tick = a.tick;
  g_cursor[0] = a.cursor0[0];
  g_cursor[1] = a.cursor0[1];
}

void end_viewing(const char* why) {
  if (g_mode == Mode::Playing && !g_play_done) finish_playback();
  if (g_viewing) log::info("replay: viewing ended ({})", why);
  g_viewing = false;
  g_mode = Mode::Idle;
  remove_object();
}

void* __fastcall obj_delete(void* self, int) {
  ++g_deletes;
  log::info("replay: recorder DeleteInstance (ccGame::Finalize)");
  if (g_mode == Mode::Recording) finish_recording("battle finalize");
  else if (g_mode == Mode::Playing) end_viewing("battle finalize");
  return self;
}

// ---- hooks ----
using RecFn = void(__fastcall*)(void*, uint32_t*, uint32_t);
RecFn o_record = nullptr, o_play = nullptr;
using InitFn = uintptr_t(__fastcall*)(void*);
InitFn o_battle_init = nullptr;
using JudgeFn = uintptr_t(__fastcall*)(void*);
JudgeFn o_judge = nullptr;
using ApplyFn = uintptr_t(__fastcall*)(uint8_t*);
ApplyFn o_apply = nullptr;

void __fastcall hk_record(void* rec, uint32_t* frame, uint32_t side) {
  if (rec != g_obj) return o_record(rec, frame, side);
  if (g_mode != Mode::Recording || side > 1) return;
  PadFrame f;
  std::memcpy(f.w, frame, sizeof f.w);
  g_rec.pads[side].push_back(f);
  g_rec.pad_tick[side].push_back(static_cast<uint32_t>(g_rec.hashes.size()));
}

void __fastcall hk_play(void* rec, uint32_t* frame, uint32_t side) {
  if (rec != g_obj) return o_play(rec, frame, side);
  if (g_mode != Mode::Playing || g_play_done || side > 1) return;
  auto& c = g_cursor[side];
  if (c >= g_play.pads[side].size()) {
    ++g_overrun;
    return;
  }
  if (g_first_pad_tick_skew < 0 && g_play.pad_tick[side][c] != g_tick) {
    g_first_pad_tick_skew = static_cast<int64_t>(g_tick);
    log::warn("replay: side {} pad #{} recorded on tick {}, consumed on tick {}", side, c, g_play.pad_tick[side][c], g_tick);
  }
  const PadFrame& f = g_play.pads[side][c++];
  for (int i = 0; i < 9; ++i)
    if (i != 4) frame[i] = f.w[i];  // repeat (w[4]) is wall-clock based and not part of the native record either
}

void on_round_start(bool new_battle) {
  switch (g_mode) {
    case Mode::Idle:
      if (g_cfg.auto_record && battle_mode() == 0 && new_battle) start_recording();
      break;
    case Mode::ArmedRec:
      start_recording();
      break;
    case Mode::Recording:
      if (new_battle) {
        finish_recording("new battle");
        if (g_cfg.auto_record && battle_mode() == 0) start_recording();
      } else {
        g_rec.segs.push_back(snapshot(g_rec));
        g_timer_running = false;
        log::info("replay: round {} at tick {}", g_rec.segs.size(), g_rec.hashes.size());
      }
      break;
    case Mode::ArmedPlay:
      start_playback();
      break;
    case Mode::Playing:
      if (new_battle) {  // Retry / Rematch / Restart Replay: watch again from the start
        ++g_restarts;
        if (!g_play_done) log::info("replay: restarted at tick {}", g_tick);
        start_playback();
      } else {
        next_segment();
      }
      break;
  }
}

uintptr_t __fastcall hk_battle_init(void* scene) {
  ++g_inits;
  const int wins = duel_wins();
  const bool new_battle = wins <= 0;
  log::info("replay: battle init #{} (mode {}, wins {}, {})", g_inits, battle_mode(), wins, new_battle ? "new battle" : "next round");
  if (new_battle && g_started_at_fight) g_started_at_fight = false;  // already started when the timer started
  else on_round_start(new_battle);
  return o_battle_init(scene);
}

// Dev: which code draws the gameplay MT (genrand 0x140A58800), on which tick and thread, from "Fight" on.
constexpr uintptr_t kGenrand = 0x140A58800;
struct RngDraw { uint32_t tick; uint32_t tid; void* frames[5]; };
constexpr size_t kRngTraceCap = 60000;
RngDraw* g_rng_trace = nullptr;
std::atomic<size_t> g_rng_n{0};
std::atomic<bool> g_rng_on{false}, g_rng_armed{false};
uint32_t g_rng_tick = 0;  // duel ticks since "Fight" (counted in hk_judge while tracing)
using GenrandFn = uint32_t(__fastcall*)();
GenrandFn o_genrand = nullptr;
uint32_t __fastcall hk_genrand() {
  if (g_rng_on.load(std::memory_order_relaxed)) {
    const size_t i = g_rng_n.fetch_add(1, std::memory_order_relaxed);
    if (i < kRngTraceCap) {
      RngDraw& d = g_rng_trace[i];
      d = {g_rng_tick, GetCurrentThreadId(), {}};
      RtlCaptureStackBackTrace(1, 5, d.frames, nullptr);
    }
  }
  return o_genrand();
}

// CPU decision 0x14075E728(ai): its countdown ai+0x1A58 decides when the CPU rolls for its next move (it reaches 0 →
// genrand). The AI object is reused across battles and the countdown is not reset, so its value at "Fight" depends on
// what ran before in this process. It is recorded at each CPU's first decision after Fight and written back on playback.
constexpr uintptr_t kAiDecide = 0x14075E728, kAiCountdown = 0x1A58;
std::vector<uint8_t*> g_ai_seen;  // CPUs that made their first decision since Fight
bool g_ai_track = false;          // set at Fight (g_started_at_fight is cleared again at battle-main init)
using AiDecideFn = uint64_t(__fastcall*)(uint8_t*);
AiDecideFn o_ai_decide = nullptr;
uint64_t __fastcall hk_ai_decide(uint8_t* ai) {
  if (g_ai_track && std::find(g_ai_seen.begin(), g_ai_seen.end(), ai) == g_ai_seen.end()) {
    const size_t i = g_ai_seen.size();
    g_ai_seen.push_back(ai);
    auto& t = *reinterpret_cast<float*>(ai + kAiCountdown);
    if (g_mode == Mode::Recording) g_rec.ai_timers.push_back(t);
    else if (g_mode == Mode::Playing && !g_play_done && i < g_play.ai_timers.size()) t = g_play.ai_timers[i];
  }
  return o_ai_decide(ai);
}

using FightFn = void(__fastcall*)(void*);
FightFn o_fight = nullptr;
void __fastcall hk_fight(void* begin) {
  if (g_rng_armed.exchange(false)) {
    g_rng_n = 0;
    g_rng_tick = 0;
    g_rng_on = true;
    log::info("replay: rng trace started at Fight");
  }
  // Replays recorded before tick 0 moved here start at battle-main init, as they were recorded.
  const bool old_file = (g_mode == Mode::ArmedPlay || g_mode == Mode::Playing) && g_play.meta.start != "fight";
  if (!g_started_at_fight && !old_file && duel_wins() <= 0) {
    log::info("replay: Fight (new battle, mode {}, {} duel ticks since setup)", battle_mode(), g_pre_fight_ticks);
    g_started_at_fight = true;
    g_ai_seen.clear();
    g_ai_track = true;
    on_round_start(true);
  }
  o_fight(begin);
}

uintptr_t __fastcall hk_apply(uint8_t* bsp) {
  g_pre_fight_ticks = 0;
  g_pre_have = false;
  const bool overwrite = (g_mode == Mode::ArmedPlay || g_mode == Mode::Playing) && g_play.bsp.size() == kBspSize;
  if (overwrite) {
    write_replay_setup(bsp, "apply");
    // Character select preloads its picks into the fighter load slots and the loader skips filled slots, so the
    // replay's fighters only load if those slots are emptied first (the same Unload char select uses on cancel).
    if (auto* dm = *game::at<uint8_t**>(kDuelManager)) {
      int unloaded = 0;
      for (int i = 0; i < 6; ++i) {
        uint8_t* slot = dm + kLoaderSlots + i * kLoaderSlotSize;
        if (*reinterpret_cast<int*>(slot + 8) == 0) continue;
        game::at<void(__fastcall*)(void*)>(kLoaderUnload)(slot);
        ++unloaded;
      }
      if (unloaded) log::info("replay: unloaded {} preloaded fighter slot(s)", unloaded);
    }
  }
  g_applied_bsp.assign(bsp, bsp + kBspSize);
  const uintptr_t r = o_apply(bsp);
  if (overwrite) restore_rounds();
  if (overwrite && !g_globals_checked && g_play.globals.size() == kGlobalsEnd - kGlobalsBegin) {
    g_globals_checked = true;
    auto* g = game::at<const uint8_t*>(kGlobalsBegin);
    std::string diff;
    int ranges = 0;
    for (size_t i = 0; i < g_play.globals.size();) {
      if (g[i] == g_play.globals[i]) {
        ++i;
        continue;
      }
      size_t j = i;
      while (j < g_play.globals.size() && g[j] != g_play.globals[j]) ++j;
      if (++ranges <= 12) diff += std::format(" 0x{:X}+{}", kGlobalsBegin + i, j - i);
      i = j;
    }
    log::info("replay: duel globals after apply vs recording: {} differing ranges{}", ranges, diff);
  }
  return r;
}

using EndMenuFn = uintptr_t(__fastcall*)(uint8_t*, int);
EndMenuFn o_end_menu = nullptr;
using PauseActionFn = int(__fastcall*)(void*);
PauseActionFn o_pause_action = nullptr;

bool g_left_to_menu = false;
void note_action(int action, const char* where) {
  if (action != 7 && action != 8) return;  // 2 = Retry / Rematch: handled at the next battle init
  if (g_mode == Mode::Recording) finish_recording(where);
  if (g_viewing && action == 8) g_left_to_menu = true;
  if (g_viewing) end_viewing(where);  // the battle is over or being left: controls, HUD and input go back now
}

// The VS end menu (ccSceneBattleEndBase, 0x1406D1F64(scene, sub)) only runs once the match is decided: the recording
// is complete, so it is written before the menu shows. Sub 0 builds the "EndMenu" window at scene+0x158 with Rematch /
// Return to Character Select / Return to Free Battle Menu; sub 1 maps the decided index to an action (scene+0x134).
// After a recorded battle the window gets a fourth item, Save Replay, at index 1: choosing it saves and reopens the
// menu with "Replay Saved" in its place; the other indices are mapped back before the game reads them.
// After watching a replay it has two: Watch Again / Return to Replays (or Exit Replay); index 1 maps back to the game's 2.
const char* kEndItemsSave[] = {"MSG_Rematch", "rpl_save_replay", "battlestartmenu_006", "battlestartmenu_046"};
const char* kEndItemsSaved[] = {"MSG_Rematch", "rpl_replay_saved", "battlestartmenu_006", "battlestartmenu_046"};
const char* kEndItemsFull[] = {"MSG_Rematch", "rpl_saved_full_item", "battlestartmenu_006", "battlestartmenu_046"};
bool g_end_fresh = false;  // the recording just finished at this end menu
uint8_t* g_end_win = nullptr;
const char* kEndItemsView[] = {"MSG_Rematch", "battlestartmenu_046"};
bool g_end_saved = false, g_end_full = false, g_end_view = false;

void end_menu_items(uint8_t* win, const char** items, int n = 4) {
  int flags[4] = {0, 0x10, 0x40, 2};
  game::at<void(__fastcall*)(void*, int*, const char**, int, int, int)>(0x1406A4B94)(win, flags, items, n, 1, -1);
  game::at<void(__fastcall*)(void*)>(0x1406A5708)(win);  // resize
}

uintptr_t __fastcall hk_end_menu(uint8_t* scene, int sub) {
  if (g_mode == Mode::Recording) {
    finish_recording("match end");
    g_end_fresh = battle_mode() == 0 && !g_rec.hashes.empty();
  }
  auto* win = *reinterpret_cast<uint8_t**>(scene + 0x158);
  if (sub == 1 && win && win == g_end_win && *reinterpret_cast<int*>(win + 0x1CC) == 3) {
    const int idx = game::at<int(__fastcall*)(void*)>(0x1406A4894)(win);
    if (g_end_view) {
      if (idx == 1) *reinterpret_cast<int*>(win + 0x1C8) = 2;  // Return to Replays = the game's Return to Free Battle Menu
    } else if (idx == 1) {  // Save Replay: save, then show the menu again (state back to open)
      if (!g_end_saved) {
        std::filesystem::path written;
        const std::string err = save_last(&written);
        g_end_saved = err.empty();
        g_end_full = err == "rpl_err_saved_full";
        game::at<void(__fastcall*)(void*, int, int, float, int, int)>(0x14073D860)(*game::at<void**>(0x141610160),
                                                                                g_end_saved ? 0x14 : 0x0F, 0, 1.0f, 0, 1);
      }
      end_menu_items(win, g_end_saved ? kEndItemsSaved : g_end_full ? kEndItemsFull : kEndItemsSave);
      game::at<void(__fastcall*)(void*)>(0x1406A4A10)(win);  // start: +0x1CC = 0, result −1
    } else {
      *reinterpret_cast<int*>(win + 0x1C8) = idx <= 0 ? idx : idx - 1;  // back to the game's 3-item indices
    }
  }
  const uintptr_t r = o_end_menu(scene, sub);
  win = *reinterpret_cast<uint8_t**>(scene + 0x158);
  if (sub == 0 && win && win != g_end_win) {
    g_end_win = win;
    g_end_saved = g_end_full = false;
    g_end_view = g_viewing;
    if (g_viewing) end_menu_items(win, kEndItemsView, 2);  // no Return to Character Select after a replay
    else if (g_end_fresh) end_menu_items(win, kEndItemsSave);
    else g_end_win = nullptr;  // plain native menu
    g_end_fresh = false;
  }
  if (sub == 2) g_end_win = nullptr;  // window closed and deleted
  note_action(*reinterpret_cast<int*>(scene + 0x134), "end menu");
  return r;
}

int __fastcall hk_pause_action(void* menu) {
  const int action = o_pause_action(menu);
  note_action(action, "quit from pause");
  return action;
}


uintptr_t __fastcall hk_judge(void* judge) {
  if (++g_pre_fight_ticks == 1) {  // the battle's first duel tick: the intro (and the CPU AI) start drawing from here
    if ((g_mode == Mode::ArmedPlay || g_mode == Mode::Playing) && !g_play.pre.empty() && !g_norestore) {
      restore(g_play.pre[0]);
      log::info("replay: RNG restored at the battle's first duel tick");
    } else {
      g_pre_snap = snapshot(g_rec);
      g_pre_have = true;
    }
  }
  if (g_rng_on && ++g_rng_tick > 300) g_rng_on = false;  // tick k's draws are tagged k-1 (hook runs before o_judge)
  const uintptr_t r = o_judge(judge);
  const bool running = *reinterpret_cast<const int*>(static_cast<const uint8_t*>(judge) + 0xD4) != 0;  // round timer counts
  const bool timer_starts = running && !g_timer_running;
  g_timer_running = running;
  if (g_mode == Mode::Recording) {
    if (timer_starts) {
      Anchor& a = g_rec.anchors.emplace_back();
      a.seg = static_cast<uint32_t>(g_rec.segs.size() - 1);
      a.tick = static_cast<uint32_t>(g_rec.hashes.size());
      a.cursor0[0] = static_cast<uint32_t>(g_rec.pads[0].size());
      a.cursor0[1] = static_cast<uint32_t>(g_rec.pads[1].size());
      std::memcpy(a.mt.data(), game::at<const void*>(kMtState), kMtWords * 4);
      a.mt_index = *game::at<const uint32_t*>(kMtIndex);
      a.lcg = *game::at<const uint64_t*>(kAsyncLcg);
    }
    g_rec.hashes.push_back(compute(judge));
    JudgeRaw raw;
    std::memcpy(raw.data(), static_cast<const uint8_t*>(judge) + 0x78, sizeof raw);
    g_rec.judge_raw.push_back(raw);
  } else if (g_mode == Mode::Playing && !g_play_done) {
    // Only ticks with the round timer running are compared, until the round is decided. Intros, banners, cinematics and
    // KO slow-motion run on UI time; the timer starting after them snaps to the recording (snap_to_anchor).
    if (timer_starts && !g_play.anchors.empty()) snap_to_anchor();
    const bool live = !g_decided && (g_play.anchors.empty() || running);
    if (live && g_tick < g_play.hashes.size()) {
      const TickHash now = compute(judge);
      const TickHash& want = g_play.hashes[g_tick];
      if (!(now == want)) {
        ++g_mismatches;
        if (now.mt != want.mt || now.pos != want.pos || now.life != want.life) ++g_gameplay_mismatches;
        if (g_first_mismatch < 0) {
          g_first_mismatch = static_cast<int64_t>(g_tick);
          g_first_mismatch_what = std::format("[{}{}{}{}]", now.mt != want.mt ? "mt " : "", now.pos != want.pos ? "pos " : "",
                                              now.life != want.life ? "life " : "", now.judge != want.judge ? "judge" : "");
          log::warn("replay: first mismatch at tick {} {}", g_tick, g_first_mismatch_what);
        }
      }
    } else if (!live) {
      ++g_tail_extra;
    }
    const uint32_t result = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(judge) + 0xBC);  // 0 = round running
    if (!g_decided && result != 0) {
      g_decided = true;
      log::info("replay: round {} decided at tick {} (result {})", g_seg + 1, g_tick, result);
      if (g_seg + 1 >= g_play.segs.size()) finish_playback();
    }
    ++g_tick;
    if (!g_play_done && g_play.anchors.empty() && g_tick == g_play.hashes.size()) finish_playback();  // files without anchors
  }
  return r;
}

std::string list_files() {
  std::string out;
  for (const char* sub : {"saved", "recent"}) {
    std::vector<std::string> names;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir(sub), ec))
      if (e.path().extension() == L".nsreplay") names.push_back(e.path().filename().string());
    std::sort(names.rbegin(), names.rend());
    out += std::format(" {}={}", sub, names.size());
    for (auto& n : names) out += " " + std::string(sub) + "/" + n;
  }
  return out;
}

std::filesystem::path resolve(const std::string& arg) {
  std::filesystem::path p(std::u8string(arg.begin(), arg.end()));
  if (p.is_relative()) p = g_cfg.root / L"replays" / p;
  return p;
}
}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  hooks::install("ccBattleRecord::Record", game::at(kRecord), reinterpret_cast<void*>(&hk_record), reinterpret_cast<void**>(&o_record));
  hooks::install("ccBattleRecord::Play", game::at(kPlay), reinterpret_cast<void*>(&hk_play), reinterpret_cast<void**>(&o_play));
  hooks::install("BattleMain::Initialize", game::at(kBattleMainInit), reinterpret_cast<void*>(&hk_battle_init),
                 reinterpret_cast<void**>(&o_battle_init));
  hooks::install("CpuAi::Decide", game::at(kAiDecide), reinterpret_cast<void*>(&hk_ai_decide), reinterpret_cast<void**>(&o_ai_decide));
  hooks::install("MT::genrand", game::at(kGenrand), reinterpret_cast<void*>(&hk_genrand), reinterpret_cast<void**>(&o_genrand));
  hooks::install("BattleBegin::Fight", game::at(kFight), reinterpret_cast<void*>(&hk_fight), reinterpret_cast<void**>(&o_fight));
  hooks::install("ccDuelJudge::Update", game::at(kJudgeUpdate), reinterpret_cast<void*>(&hk_judge), reinterpret_cast<void**>(&o_judge));
  hooks::install("BattleEnd::MenuState", game::at(kEndMenuState), reinterpret_cast<void*>(&hk_end_menu),
                 reinterpret_cast<void**>(&o_end_menu));
  hooks::install("PauseMenu::SelectedAction", game::at(kPauseAction), reinterpret_cast<void*>(&hk_pause_action),
                 reinterpret_cast<void**>(&o_pause_action));
  hooks::install("ApplyBattleSelectParam", game::at(kApplyBattleParam), reinterpret_cast<void*>(&hk_apply),
                 reinterpret_cast<void**>(&o_apply));
  log::info("replay: auto_record={} keep_recent={} dir={}", cfg.auto_record, cfg.keep_recent, (cfg.root / L"replays").string());
}

bool viewing() { return g_viewing; }
bool replaying() { return g_mode == Mode::Playing && !g_play_done; }
std::filesystem::path folder(const char* sub) { return dir(sub); }

std::string arm_file(const std::filesystem::path& path) {
  if (g_mode == Mode::Recording || (g_mode == Mode::Playing && !g_play_done)) return "rpl_err_busy";
  Recording r;
  std::string err;
  if (!read_file(path.wstring(), r, &err)) {
    log::warn("replay: cannot read {}: {}", path.string(), err);
    return "rpl_err_read";
  }
  if (r.meta.exe_sha256 != game::info().sha256) return "rpl_err_version";
  if (r.meta.fps != logic_fps()) return "rpl_err_fps";
  if (r.bsp.size() != kBspSize) return "rpl_err_read";
  g_play = std::move(r);
  g_play_name = path.filename().string();
  g_play_path = path;
  g_norestore = false;
  g_globals_checked = false;
  g_mode = Mode::ArmedPlay;
  log::info("replay: armed {} ({} ticks, {} rounds)", g_play_name, g_play.meta.ticks, g_play.meta.rounds);
  return {};
}

bool take_left_to_menu() {
  const bool r = g_left_to_menu;
  g_left_to_menu = false;
  return r;
}

bool end_menu_has_save(const void* window) { return window && window == g_end_win && !g_end_view; }
bool end_menu_is_replay(const void* window) { return window && window == g_end_win && g_end_view; }

bool armed() { return g_mode == Mode::ArmedPlay && g_play.bsp.size() == kBspSize; }

bool write_armed_setup() {
  if (!armed()) return false;
  uint8_t* bsp = battle_select_param();
  if (!bsp) return false;
  write_replay_setup(bsp, "FreeBattleRoot");
  restore_rounds();
  return true;
}

int count(const char* sub) {
  int n = 0;
  std::error_code ec;
  for (auto& e : std::filesystem::directory_iterator(dir(sub), ec))
    if (e.path().extension() == L".nsreplay") ++n;
  return n;
}

int keep_recent() { return g_cfg.keep_recent; }

std::filesystem::path playing_file() { return g_viewing ? g_play_path : std::filesystem::path{}; }

std::string save_last(std::filesystem::path* written) {
  if (g_mode == Mode::Recording || g_rec.hashes.empty()) return "rpl_err_nothing";
  if (count("saved") >= kSavedCap) return "rpl_err_saved_full";
  const auto path = dir("saved") / (utc_now(true) + ".nsreplay");
  std::string err;
  if (!write_file(path.wstring(), g_rec, &err)) {
    log::error("replay: save failed: {}", err);
    return "rpl_err_write";
  }
  g_last_saved = path.string();
  log::info("replay: saved {}", g_last_saved);
  if (written) *written = path;
  return {};
}

bool in_transition() { return replaying() && (g_decided || g_tick < g_round_start + kSettleTicks); }

std::string command(const std::string& args) {
  std::istringstream in(args);
  std::string sub, rest;
  in >> sub;
  std::getline(in, rest);
  if (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
  const bool busy = g_mode == Mode::Recording || (g_mode == Mode::Playing && !g_play_done);
  if (sub.empty() || sub == "status") return "ok " + summary();
  if (sub == "list") return "ok" + list_files();
  if (sub == "auto") {
    g_cfg.auto_record = rest != "off";
    return std::format("ok auto_record={}", g_cfg.auto_record);
  }
  if (sub == "rec") {
    if (busy) return "error busy " + summary();
    g_mode = Mode::ArmedRec;
    return "ok armed: recording starts at the next battle init";
  }
  if (sub == "play") {  // replay the last recording (in-process, e.g. via Rematch)
    if (busy) return "error busy " + summary();
    if (g_rec.segs.empty()) return "error nothing recorded";
    g_play = g_rec;
    g_play_name.clear();
    g_play_path.clear();
    g_norestore = rest == "norestore";
    g_globals_checked = false;
    g_mode = Mode::ArmedPlay;
    return std::string("ok armed: playback starts at the next battle init") + (g_norestore ? " (no RNG restore)" : "");
  }
  if (sub == "playfile") {  // playfile <path | recent/x.nsreplay | saved/x.nsreplay>
    const auto path = resolve(rest);
    const std::string err = arm_file(path);
    if (!err.empty()) return "error " + err;
    return std::format("ok armed {}: start any Free Battle VS battle; it loads as the replay ({} ticks, {} rounds)", g_play_name,
                       g_play.meta.ticks, g_play.meta.rounds);
  }
  if (sub == "save") {  // save [path]: the last finished recording
    if (g_mode == Mode::Recording) return "error the battle is still being recorded";
    if (g_rec.hashes.empty()) return "error nothing recorded";
    const auto path = rest.empty() ? dir("saved") / (utc_now(true) + ".nsreplay") : resolve(rest);
    std::string err;
    if (!write_file(path.wstring(), g_rec, &err)) return "error " + err;
    g_last_saved = path.string();
    return "ok saved " + g_last_saved;
  }
  if (sub == "bspdump") {  // dev: side blocks of the replay's, the live and the duel's battle setup (u32 words per fighter)
    auto sides = [](const uint8_t* base) {
      std::string o;
      for (int side = 0; side < 2; ++side) {
        o += std::format(" s{}[", side);
        for (int f = 0; f < 3; ++f) {
          const auto* w = reinterpret_cast<const int32_t*>(base + side * 0x1AC + f * 0x3C);
          o += std::format("{}{}/{}/{}/{}", f ? " " : "", w[0], w[1], w[2], w[3]);
        }
        o += "]";
      }
      return o;
    };
    std::string out = "ok";
    if (g_play.bsp.size() == kBspSize) out += " replay:" + sides(g_play.bsp.data() + 0xC0) + std::format(" +418={:#x}", *reinterpret_cast<const uint32_t*>(g_play.bsp.data() + 0x418));
    if (uint8_t* bsp = battle_select_param()) out += " live:" + sides(bsp + 0xC0) + std::format(" +418={:#x}", *reinterpret_cast<uint32_t*>(bsp + 0x418));
    out += " duel:" + sides(game::at<const uint8_t*>(0x141611A8C)) + std::format(" stage={:#x}", *game::at<const uint32_t*>(0x141611A88));
    return out;
  }
  if (sub == "rngtrace") {  // dev: rngtrace arm | dump <file>
    if (!g_rng_trace) g_rng_trace = new RngDraw[kRngTraceCap];
    if (rest == "arm") {
      g_rng_armed = true;
      return "ok armed: traces genrand callers from the next Fight for 300 ticks";
    }
    const std::string name = rest.starts_with("dump ") ? rest.substr(5) : "rngtrace.txt";
    const auto path = g_cfg.root / L"logs" / std::filesystem::path(name);
    std::string out;
    const size_t n = std::min(g_rng_n.load(), kRngTraceCap);
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (size_t i = 0; i < n; ++i) {
      const auto& d = g_rng_trace[i];
      out += std::format("{} {}", d.tick, d.tid);
      for (void* f : d.frames) {
        const auto a = reinterpret_cast<uintptr_t>(f);
        out += a >= base && a < base + 0x10000000 ? std::format(" 0x{:x}", a - base + 0x140000000) : std::string(" -");
      }
      out += "\n";
    }
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
      fwrite(out.data(), 1, out.size(), f);
      fclose(f);
    }
    return std::format("ok {} draws -> {}", n, path.string());
  }
  if (sub == "stop") {
    if (g_mode == Mode::Recording) finish_recording("stop command");
    else if (g_mode == Mode::Playing || g_mode == Mode::ArmedPlay) end_viewing("stop command");
    else g_mode = Mode::Idle;
    return "ok " + summary();
  }
  return "error usage: replay status|list|auto on|off|rec|play [norestore]|playfile <file>|save [file]|stop";
}

}  // namespace ssr::replay
