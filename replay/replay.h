#pragma once
#include <filesystem>
#include <string>

// Input replay core. Records the human pad frames at CyberConnect2's own record/play tap in ccPlayerPad::Ctrl,
// snapshots the gameplay RNG at every round's battle-main init, keeps BattleSelectParam so a replay can be relaunched
// in a fresh process, and hashes game state once per duel tick so playback proves it is frame-identical.
namespace ssr::replay {
struct Config {
  std::filesystem::path root;  // <game>/StormReplay
  bool auto_record = true;     // record every Free Battle VS battle into replays/recent
  int keep_recent = 20;
};
void install(const Config&);
std::string command(const std::string& args);  // game thread
bool viewing();  // a replay battle is on screen: from playback start until that battle is torn down (game thread)
bool replaying();  // the recorded inputs are still being played (the viewer's pad must not reach the battle)
// Round transition (KO → next round's first seconds): UI-timed and sensitive to frame pacing, so playback controls
// stay at 1x / no frame advance here; changing the tick count of a transition can make the next round diverge.
bool in_transition();

std::filesystem::path folder(const char* sub);  // <root>/replays/<sub> ("recent", "saved"), created if missing
// Load a replay for watching: the next Free Battle VS battle loads as this replay. Empty = ok, else a message id.
std::string arm_file(const std::filesystem::path& file);
bool armed();                // a replay is loaded and waits for its battle
bool take_left_to_menu();    // a replay was just left with "Exit Replay" (consumes the flag)
bool write_armed_setup();    // put the armed replay's battle setup + round count in place (direct launch)
bool end_menu_has_save(const void* window);  // that end-menu window carries the Save Replay item (4 items)
bool end_menu_is_replay(const void* window);  // the end menu after a replay: Watch Again / Return to Replays (2 items)
inline constexpr int kSavedCap = 150;  // as Storm Generations: My Replays held 150
int count(const char* sub);             // replays in replays/<sub>
int keep_recent();                      // Recent keeps this many (oldest trimmed after each battle)
std::filesystem::path playing_file();   // file being watched ("" = in-memory recording / none)
// Write the last finished recording to replays/saved; empty = ok, else a message id.
std::string save_last(std::filesystem::path* written = nullptr);
}  // namespace ssr::replay
