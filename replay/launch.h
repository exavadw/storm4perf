#pragma once
#include <functional>

// Direct replay launch from the Free Battle menu (no character/stage select), the game's own way:
// the menu's "VS Battle" decision (the game pushes FreeBattleRoot 408) → on the root's first sequence step,
// MatchSelect's forward side effects, the replay's BattleSelectParam + round count, and the sequence jumps to
// VSLoading (411). From there the flow is native (loading, battle, end menu: Watch Again / Character Select / Menu).
namespace ssr::launch {
void install();
bool start();  // game thread, on the Free Battle top menu with a replay armed; false if not possible here
// Same launch with any setup writer (game thread, on the root's first step; true = setup written, jump to VSLoading).
bool start_with(std::function<bool()> write_setup);
void tick();   // game thread, every frame
bool active();
// The mode-select 3D background (*0x14161C9C0) has both resource groups loaded and no background op pending. The menu
// takes input before that; launching earlier unloads the background under the loader thread (crash at 0x140A11CB9).
bool menu_settled();
}  // namespace ssr::launch
