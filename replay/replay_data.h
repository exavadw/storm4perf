#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// In-memory replay and the .nsreplay file (nsreplay.cpp).
namespace ssr::replay {
inline constexpr size_t kMtWords = 624;
inline constexpr size_t kBspSize = 0x11E0;                // BattleSelectParam (ccBattleSelResult)
inline constexpr uintptr_t kGlobalsBegin = 0x141611A88;   // duel globals written from BattleSelectParam …
inline constexpr uintptr_t kGlobalsEnd = 0x141612090;     // … and by the battle loaders (snapshot for diagnostics)

// Button layout (ccSaveData's per-player key config, Options > Controller Settings): the 8 battle actions' button masks
// for sides 0 and 1, as ccPlayerPad::Init caches them. Recorded pad frames hold raw buttons in this layout.
inline constexpr size_t kKeyActions = 8;
struct PadFrame {
  uint32_t w[9];  // ccPlayerPad::Ctrl stack frame: on, off, press, release, repeat, analog u16[8]
};
struct TickHash {
  uint64_t mt = 0, pos = 0, life = 0, judge = 0;
  bool operator==(const TickHash&) const = default;
};
struct Segment {  // one per round (battle-main init)
  std::array<uint32_t, kMtWords> mt{};
  uint32_t mt_index = 0;
  uint64_t lcg = 0;
  uint32_t tick0 = 0;
  uint32_t cursor0[2]{};
};
// The round timer starting (judge+0xD4 0 → 1): "Fight!" and the end of jutsu cinematics. What runs before it (intros,
// banners, cutscenes, KO tails) is timed by the UI, so playback snaps tick, pad cursors and RNG to each anchor.
struct Anchor {
  uint32_t seg = 0;
  uint32_t tick = 0;
  uint32_t cursor0[2]{};
  std::array<uint32_t, kMtWords> mt{};
  uint32_t mt_index = 0;
  uint64_t lcg = 0;
};
using JudgeRaw = std::array<uint32_t, 18>;  // judge+0x78..+0xC0; [17] = +0xBC round result (2 P1 win, 3 P2 win)

struct Meta {
  std::string created;     // UTC, ISO 8601
  std::string exe_sha256;  // NSUNS4.exe the replay was recorded on
  uint32_t fps = 0;        // nuccSys+0x942
  uint32_t mode = 0;       // battle mode global 0x141611F70 (0 free battle, 3 online)
  uint32_t stage = 0;      // BattleSelectParam+0x418: CRC-32 of the stage id
  std::string stage_name_id;  // stage select list "nameid" (resolves to the stage description), "" if unknown
  std::string start;          // "fight": tick 0 is round 1's timer start ("Fight"); "" (older files): battle-main init
  int32_t chara[2]{-1, -1}, preset[2]{-1, -1};  // leaders, from the side blocks
  uint32_t ticks = 0, rounds = 0, result = 0;   // result = last round's judge result
};

struct Recording {
  Meta meta;
  std::vector<uint8_t> bsp;      // BattleSelectParam at battle start (kBspSize)
  std::vector<uint8_t> globals;  // kGlobalsBegin..kGlobalsEnd after the loaders applied it
  std::vector<Segment> segs;
  std::vector<Anchor> anchors;  // every start of the round timer, in order
  // RNG on the battle's first duel tick (0 or 1 entries). The battle-begin intro runs a fixed number of ticks before
  // "Fight", and the CPU AI already draws during it (its countdowns carry into the fight).
  std::vector<Segment> pre;
  // Each CPU's decision countdown (ai+0x1A58) at its first decision after "Fight", in call order. The AI object is
  // reused across battles and this countdown is not reset, so it carries in from earlier battles.
  std::vector<float> ai_timers;
  // Each CPU's 8 timers (ai+0x1A40) at its first think from the battle's first duel tick, in call order, taken after
  // its task was dropped there: the intro's CPU rolls start from them. Empty before 1.12.3.
  std::vector<float> ai_pre;
  std::vector<PadFrame> pads[2];
  std::vector<uint32_t> keys;  // 2 * kKeyActions masks (side 0 then side 1); empty in files from before 1.12
  // Tick on which each round's opening scene let the players move (one per round start, in order). That scene ends on
  // UI time, so the tick varies between runs; playback opens input on the recorded tick. Empty before 1.12.
  std::vector<uint32_t> opens;
  // Options that change the battle: [0] = ccSaveData+0x120 (show items obtained in battle: with it on, each Ninja
  // Treasure drop and pickup plays an effect at a random spot drawn from the gameplay RNG); [1] = storm4perf fps60
  // groups active at 60 FPS (bit g = group g, 0x80000000 = recorded). Empty before 1.12.
  std::vector<uint32_t> opts;
  std::vector<uint32_t> pad_tick[2];  // tick each pad frame was taken on (diagnostic)
  std::vector<TickHash> hashes;
  std::vector<JudgeRaw> judge_raw;  // diagnostic
};

bool write_file(const std::wstring& path, const Recording& r, std::string* err);
bool read_file(const std::wstring& path, Recording& r, std::string* err);
}  // namespace ssr::replay
