# storm4perf

A drop-in `xinput9_1_0.dll` for **Naruto Shippuden: Ultimate Ninja Storm 4** on PC (Steam 1.09,
`NSUNS4.exe` SHA-256 `147e52a3…bc257`). It removes busy-waits from the engine and leaves gameplay unchanged, adds
two cheaper shadow levels to the game's own Options screen, and adds **replays**: every Free Battle VS battle is recorded,
and you can watch, save and delete replays from a list on the Free Battle menu, all in the game's own menus.
Measured at 30 FPS on an Intel UHD 770: the game uses a quarter to a third of the CPU it used before, and on a
2-thread CPU it runs at full speed instead of in slow motion. Details and numbers: [`docs/storm4-performance.md`](docs/storm4-performance.md), sections 3b and 4.

## Install

1. Download `storm4perf-<version>.zip` from [Releases](https://github.com/exavadw/storm4perf/releases).
2. Open the game folder: in Steam, right-click **NARUTO SHIPPUDEN: Ultimate Ninja STORM 4** > **Manage** >
   **Browse local files**. It is the folder with `NSUNS4.exe`.
3. Copy `xinput9_1_0.dll` and `storm4perf.ini` from the zip into that folder. If the folder already has an
   `xinput9_1_0.dll` from another mod, see [Compatibility](#compatibility) first.
4. Start the game. `storm4perf.log` appears next to the exe and lists every fix as `OK`, `SKIP` or `FAIL`, and
   `OK replays` when replays are on.

Settings live in `storm4perf.ini` (optional; missing keys use the defaults shown in it).

Uninstall: delete `xinput9_1_0.dll` and `storm4perf.ini`. Steam's "verify files" does not remove them, because they are
not game files. Your replays stay in the `StormReplay` folder until you delete it.

## Replays

Every **VS Battle** in **Free Battle** (against the CPU or local 2P) is recorded automatically. Nothing to set up.

- **Open the list:** on the Free Battle menu, press **Y** (the button guide shows *Replays*).
- **Tabs:** **LB / RB** switch between **Saved** and **Recent**. Recent keeps the last 20 battles (oldest removed first);
  Saved keeps up to 150 until you delete them. The highlighted replay's stage, rounds, winner, length and date show in
  the help bar at the bottom; ★ marks the winner.
- **Actions:** **A** on a replay: **Watch**, **Save** (Recent only; moves it to Saved), **Delete**, **Back**.
- **Import:** put `.nsreplay` files (from a friend, or another PC) in `StormReplay\replays\import` next to the exe, then
  pick **Import Replays** at the bottom of the Saved tab. Each file is checked; good ones move to Saved, duplicates and
  unusable files are reported and left out.
- **While watching:** the button guide at the bottom right shows the controls. **LB / RB** change the playback speed
  (1/4x to 4x) and **Y** turns frame advance on. In frame advance, **RB** steps one frame and **Y** resumes playback;
  you don't need the menu. **Start** opens the pause menu with **Restart Replay**, **Playback Speed**, **Frame Advance**,
  **HUD Display**, **Save Replay** (for Recent replays, hidden once saved), and **Return to Replays**. Restarting with
  frame advance on works. Your controller does not affect the replay.
- **Two controllers:** both control the same playback: either one's Start opens the one pause menu, and both move
  its cursor and use the speed and frame-advance buttons. Unplugging (or not having) a second controller doesn't
  pause the replay.
- **After a replay:** **Watch Again** or **Return to Replays**.
- **After a battle you played:** the end menu has a **Save Replay** item, so you can keep it right away. Once saved,
  a message confirms it and the end menu returns without the Save Replay item.

Files: `StormReplay\replays\recent` and `StormReplay\replays\saved` next to the exe (`.nsreplay`, about 15–130 KB each),
log `StormReplay\logs\replay.log`. Settings, in `storm4perf.ini` under `[replay]`: `enabled` (1), `auto_record` (1),
`keep_recent` (20).

How it works: a replay stores the battle setup and both players' inputs, and the game replays them, so a replay is
the real battle again, not a video. Every frame is checked against the recording; `replay.log` says `IDENTICAL` when a
replay matched to the end. A replay also stores both players' button layouts (Options > Controller Settings), so it
plays the same moves when it is watched with a different layout, and the frame each round let the players move (the
game times that on its menu clock, which can land a frame earlier or later from one run to the next).

Known limits:
- Replays play on the same game version and FPS setting they were recorded with (Steam 1.09).
- Online battles are not recorded yet.
- Replays recorded with an older version of this mod may drift from the original battle in the first seconds.
- Replays recorded before 1.12.0 do not store the button layouts or the item-effects option (see 1.12.0 below): watch
  them with the same Controller Settings layout and the same option they were recorded with, or they can drift.
- The pause between rounds (KO, then the next round's intro) runs on the game's menu clock. If it takes a frame or two
  longer or shorter than when the replay was recorded, the replay can drift from the start of the next round (seen in 2
  of 4 viewings of a 60 FPS two-round replay; the first round matched in all 4).
- Sometimes a replay diverges from its very first frame (seen in 2 of 17 test viewings of 60 FPS replays); the same
  replay matched when watched again. During the battle intro the CPU makes a different number of random rolls from one
  viewing to the next, and occasionally that carries into the fight. Not fixed yet.
- Send `StormReplay\logs\replay.log` and the replay file if you see a replay that does not match the battle.

## Lower input lag (optional)

Off by default. To turn it on, set in-game **VSync OFF** (Options), then in `storm4perf.ini` under `[lowlat]` set
`mode=early` (or `mode=elatent`, see below).

The game draws each frame, then leaves the finished image waiting until the start of its *next* frame before it shows
it: a whole frame (33 ms at 30 FPS) of delay on every button press. `early` shows each frame the moment it is drawn,
the way Slippi's visual-buffer codes do for Melee. Nothing about the game's timing or simulation changes, only when the
finished image goes to the screen.

Measured in a Practice battle (30 FPS, 960x540 window, 60 Hz screen, about 275 simulated presses per setting, from the
press to the screen starting to draw the frame that shows it):

| in-game vsync | `[lowlat] mode` | press to screen |
|---|---|---|
| off | `off` (the game's own timing) | 65 ms |
| off | `early` | **37 ms (-30 ms)** |
| off | `elatent` | **34 ms (-33 ms)** |
| on | `off` | 79 ms |
| on | `early` | 81 ms (no gain: the frame still waits for the same refresh) |

`elatent` is `early` plus a late frame start: each frame starts just late enough to be done right before the screen
refreshes, and the controller is read at that later start. It changes the game's timing, so **replays do not always
match across it**: the showcase replay drifts from frame 222 on with `elatent`. Use `early` if you care about replays;
`elatent` if you want the last 3 ms.

So: vsync off plus `early` (or `elatent`). In a window, vsync off does not tear (Windows shows each frame on a refresh
anyway).

- Needs the `gputimer` fix (on by default): with the game's own GPU-timer wait, early showing could hang the game, so
  `early`/`elatent` stay off when `gputimer=0` (the log says so).
- Replays: with `off` (the default) nothing changes (the showcase replay played back identically 4 of 4 times). With
  `early`, 4 of 5 playbacks were identical and 1 drifted from frame 222, the same timing-sensitive spot where
  `elatent` drifts. So `early` is very close but not guaranteed to match a replay recorded without it.
- Tested windowed, on the title screen, in Practice battles and while loading replays. Fullscreen, story mode and online
  play are not tested yet.
- `storm4perf.log` shows `OK   lowlat: early present at the render job end` when it is active.

Method, all numbers and the test-only settings: [`docs/lowlat.md`](docs/lowlat.md).

## What it changes

| fix | what the game does | what storm4perf does |
|---|---|---|
| `gputimer` | render thread spins on `GetData` every frame until the GPU finishes (a full CPU/GPU sync) | reads the GPU timer one to a few frames later, without waiting |
| `skinwait` | main thread spins on the GPU-skinning flag | pause, then yield, then 1 ms sleeps |
| `flipwait` | main thread spins in `WaitVSync` until the render thread is free | sleeps on the flag; the render thread wakes it |
| `timeraffinity` | every timer read moves the thread to CPU 0 and back | timer reads stay on the current CPU |
| `vsyncsleep` | VSync callback thread spins with `Sleep(0)` for 2 ms every 16.7 ms | sleeps toward its deadline |
| `bulletunpin` (off by default) | pins the 4 physics workers to CPUs 0–3 | leaves them to Windows (no measurable effect, so it stays off) |
| `presentpace` (off by default) | with in-game vsync on, spins with `Sleep(0)` after every Present until the frame time is up | sleeps on a high-resolution timer, wakes 2 ms early at time-critical priority, then spins the rest. Saves ~0.8 core with vsync on, but cost 1–3 FPS in 3 of 8 runs, all in one 30-minute window while other heavy programs ran |

### Game-bug fixes

Under `[fixes]` in `storm4perf.ini`. `guardfix`, `audiocrash`, `qpctimer` and `fps60` are **on by default**; the two
`anim60fix` keys are experimental and off.

| key | bug | what storm4perf does |
|---|---|---|
| `guardfix` (on) | a shuriken you threw is guarded while your next jutsu runs (e.g. Rasengan), and the jutsu ends without reaching the opponent | the guard-hit code marks the thrower's current attack as guarded; with the fix only when the guarded hit came from that attack. Verified in game; replays stay identical |
| `qpctimer` (on) | if a game thread is delayed for more than 100 ms in the middle of a timer read, the game clock stops for the rest of the session and the frame pacing that waits on it never finishes | replaces the game's timer (`mmTimer::GetMicroSecond`/`GetMilliSecond` and two inlined copies) with plain `QueryPerformanceCounter` time |
| `audiocrash` (on) | unplugging or switching the audio device mid-game crashes the game | skips the call on the dead voice. Verified in game: the game shows its own XAudio2 error message and keeps running, silent until restarted |
| `fps60` (on) | at 60 FPS some game code counts frames instead of time and runs twice as fast: the substitution (warp dodge) slow-motion lasts 117 ms instead of 238 ms, some camera moves and fades run at double speed | rescales those spots to the current FPS (no game data changed; at 30 FPS every value is the game's own). Verified in game: the substitution slow-motion is 205 ms at 60 (the rest is the engine's normal 2-frame state change, which is shorter at 60); 30 FPS unchanged. The camera and fade spots are fixed by the same rules but not measured one by one. Groups: `fps60_substitution`, `fps60_camera`, `fps60_effects` |
| `fps60_menu` (on, part of `fps60`) | at 60 FPS a short menu press sometimes moves the cursor two entries, and now and then a press is lost (Flash menus; measured on Game Mode Select) | the Flash menus read the pad only on the frames their 30 FPS movie ticks, and the game keeps last frame's pad state for them, so a press is read twice when the movie ticks on two frames in a row and lost when it skips both. storm4perf keeps every press until the next frame a menu reads (up to 4 frames) and delivers it on that frame only, and ignores a new press of a button released for under `fps60_menu_neutral` frames (default 2) so a bouncing d-pad or stick does not step twice. Verified in game at 60: 60 taps, 60 pad presses, the game alone would have read 69 (9 doubles), storm4perf delivered 60 and the cursor moved 60 entries; re-presses after a 1-frame release are dropped, after 3+ frames kept. Character select uses another path and was already correct. 30 FPS untouched |
| `fps60_springs` (on, part of `fps60`) | at 60 FPS hair, cloth and accessory spring bones settle twice as fast and trail half as far behind | rescales each spring node's per-frame follow / blend fractions to the frame rate while it is solved (`1 - (1 - c)^(30/fps)`); 30 FPS untouched. Not measured visually yet |
| `anim60fix` (off) | choppy animation at 60 FPS | skips a Retail-only animation throttle (JL at `0x1403f68d6`). Measured in game: that code never runs in a battle at 30 or 60 FPS, so it changes nothing there. On by default until 1.7.0 |
| `anim60fix_extra` (off) | choppy animation at 60 FPS (second part) | with `anim60fix=1`, also skips a Retail-only extra animation pass (`0x140a0ad14`); measured the same way, it never runs in a battle |

1.7.0 also had an `inputfix` key (fast jutsu / chakra-dash inputs dropped). It only changed a block the CPU AI uses, so it
could not affect player input, and it is removed.

## Shadow levels in the Options screen

Options > Shadow Quality offers **OFF, VERY LOW, LOW, MEDIUM, HIGH** (the game has LOW, MEDIUM, HIGH). "Settings
Complete" saves the choice: LOW/MEDIUM/HIGH go to `config.ini` as before, and the extra levels to `storm4perf.ini`
(`[gpu] shadows = off | potato | game`). The ini can also be edited by hand.

| level | what it does | GPU time saved (1080p, Intel UHD 770) |
|---|---|---|
| VERY LOW | 2 shadow cascades at 768/512 px instead of the game's 3–4 at 1536–2048 px | ~5 ms per frame |
| OFF | no shadow pass at all, as on a stage without shadows | ~6 ms per frame |

On a weak GPU the biggest saving is still the game's own **MSAA: OFF** (half the frame at 1080p).
The menu is patched in memory from the game's own `data\ui\flash\OTHER\option\option.gfx`; no game file is
changed. With another mod that replaces that file, the menu keeps its three levels (the log says why) and
`storm4perf.ini` still works. `[gpu] options_menu=0` leaves the Options screen untouched.

Each fix checks the exact original bytes at each site first. On a different exe version, or when another mod
has already patched a site, that fix is skipped and the log says why. Controller input goes to Windows' own
XInput, or to another XInput DLL named in `[xinput] forward=`. That lets it chain with a controller mod.

## Errors

Send `storm4perf.log` with any report. It records skipped or failed patches (with expected and found bytes),
Direct3D query errors, XInput load failures, crashes inside storm4perf code, and the first few fatal exceptions
anywhere in the game, with module and address. Every 5 minutes, and on exit, it writes counters.

## Compatibility

- Storm 4 **Steam 1.09** only. On any other exe every fix is skipped (see the log) and the game runs unmodified.
- Gameplay logic is not changed: only how the engine waits, plus the optional shadow levels and the replay feature (which
  records and replays inputs; with `[replay] enabled=0` it is off entirely). Online play has **not been
  tested** yet; if you try it, both players using the same FPS setting matters more than this mod. **30 FPS** (`fps=30` in `config.ini`, or in-game) is the mode the game logic was written for.
- Other mods that also ship `xinput9_1_0.dll` (controller or keyboard mods): rename theirs (e.g. `xinput_other.dll`) and set
  `[xinput] forward=xinput_other.dll` in `storm4perf.ini`.
- Mods using `steam_api64.dll` or `d3dcompiler_47.dll` proxies don't conflict. Replays hook the game through the
  `SteamAPI_RunCallbacks` export of whichever `steam_api64.dll` is installed (Valve's or another mod's proxy).

## Build

`build.cmd` (MSVC x64 Build Tools) produces `xinput9_1_0.dll` and its `.pdb`. Set `VCVARS` to your `vcvars64.bat` if the
Build Tools are not in `C:\BuildTools`.

`storm4perf.cpp` holds the performance and shadow code. `replay/` holds the replay feature (C++20), built into the same
DLL: `storm4perf_replay.cpp` boots it, and the rest is the replay core from the StormReplay project, copied in with
its `tools/sync-storm4perf.sh`. `replay/minhook` is [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4
(BSD 2-clause, `replay/minhook/LICENSE.txt`).

## Bug Fix History

### 1.12.0

- **Replays keep the button layouts.** A replay stores both players' button layouts (Options > Controller Settings) and
  plays the recorded moves even when it is watched with a different layout, the way online battles convert the other
  player's buttons. Checked in game: a replay recorded with one layout, watched with two buttons swapped, plays back
  identically.
- **Fixed: replays at 60 FPS could drift from the first seconds.** The frame on which each round lets the players move
  is timed by the game's menu clock, so it could land a frame earlier or later than in the recording, and every input
  after it shifted. Replays now open the players' input on the recorded frame. This also applies to replays recorded
  with older versions. This was the "battle started through Character Select drifted" case listed under Known limits.
- **Replays and `fps60`.** A replay records which `fps60` groups shaped its battle and is watched with the same ones,
  then your `storm4perf.ini` settings come back. Replays recorded before `fps60` was released (1.10.0, 2026-10-27 10:54
  UTC) are watched without it, so 60 FPS replays from 1.9.0 and older play back as recorded again (the 1.10.0 note
  below). Replays from 1.10.0 and 1.11.0 do not say which groups were on and use yours, as before. Only the battle groups are applied (substitution, camera, effects); with `fps60=0` in `storm4perf.ini` a
  replay recorded with `fps60` on cannot be matched and the log says so.
- **Fixed: an Options setting changed replays.** With the setting that shows items obtained in battle turned on, each
  Ninja Treasure drop and pickup plays an effect at a random spot, and that spot comes from the battle's own random
  numbers. A replay watched with the setting different from when it was recorded drifted from the first drop. Replays
  now store the setting and use the recorded value for those effects only; your setting is not changed.
- Checked in game at 60 FPS (1P vs COM team battle): fresh replays played back identically in 15 of 17 viewings with this
  version, including with two buttons swapped and with the setting changed; the other two diverged from their first
  frame (see Known limits). 1.9.0 diverged in 1 of 2 viewings of the same file. A
  two-round 1.8.0 replay that drifted from its first seconds now plays its first round identically when watched with
  the layout and setting it was recorded with (both rounds in 2 of 4 viewings; see Known limits). The 1.8.1 showcase
  replay still plays back identically. A replay recorded with `fps60` on, watched with its battle groups off in
  `storm4perf.ini`, plays back identically (without switching them, it diverged at the first substitution); one
  recorded without `fps60`, watched with it on, too.

### 1.11.0

- Lower input lag, optional (`[lowlat] mode=early` or `elatent`, with in-game vsync off): each frame is shown as soon
  as the render thread has drawn it instead of at the start of the next game frame. Measured in a Practice battle:
  press to screen 65 ms -> 37 ms (`early`) / 34 ms (`elatent`). Off by default. Both can change replay playback (see
  the section); the default `off` does not.
- storm4perf's microsecond clock overflowed after about 10.7 days of Windows uptime (QPC ticks x 1,000,000 in 64 bits);
  fixed.
- On any exe other than Steam 1.09, the Storm 4 fixes, replays and options menu now stay off instead of only skipping
  the fixes whose bytes differ (some hooks were not byte-checked).

### 1.10.0

- New fix `fps60` (on by default) for games set to 60 FPS (config.ini `fps=60`). Some game code counts frames where it
  should count time, so at 60 it runs twice as fast; storm4perf rescales those spots to the current FPS. At 30 FPS
  every value is the game's own. No game data files are changed. Groups (all on, each can be turned off):
  - `fps60_menu`: a short press in a menu sometimes moved the cursor two entries, and now and then a press was lost.
    The Flash menus read the pad only on the frames their 30 FPS movie ticks, and the game keeps last frame's pad
    state for them, so one press was read twice when the movie ticked on two frames in a row and lost when it skipped
    both. Every press is now kept until the next frame a menu reads and given to it once. `fps60_menu_neutral`
    (default 2 frames) also ignores a new press of a button released for less than that, so a bouncing d-pad or a
    stick flicking past its threshold does not step twice. Verified in game at 60: 96 taps, the game alone read 106
    presses, storm4perf delivered 96 and the cursor moved 96 entries. Character select was already correct.
  - `fps60_substitution`: the slow-motion when a substitution triggers lasted 117 ms at 60 instead of 238 ms; now
    205 ms (the rest is the engine's normal 2-frame state change).
  - `fps60_camera`: the duel camera hold and several camera smoothing steps (hell rescue, team ultimate, camera
    manipulator, view blend) ran twice as fast.
  - `fps60_effects`: a per-character fade and one character's wing/spine model blend ran twice as fast.
  - `fps60_springs`: hair, cloth and accessory spring bones settled twice as fast and trailed half as far behind.
- Replays recorded at 60 FPS with 1.9.0 or older may play back differently with `fps60` on (substitution timing
  changed). 30 FPS replays are unaffected.

### 1.9.0

- New fix `qpctimer` (on by default): the game's timer could stop for the rest of the session, or jump by days, if a
  game thread was delayed for more than 100 ms in the middle of a timer read (heavy load, a debugger, a suspended
  process). With vsync off the game's animation clock then stops; with vsync on the frame pacing can hang. The game's
  timer, and its two inlined copies, now read plain `QueryPerformanceCounter` time. Verified in game: a 150 ms stall
  froze the clock without the fix and changes nothing with it; FPS and CPU use are unchanged; replays still play back
  identically (the 1.8.1 showcase replay: 548/548 inputs, 0 mismatches). Works alongside StormStory's own clock fix.
- Replays: both controllers control the same pause menu (either Start opens it, both move the cursor and use the
  speed and frame-advance buttons). A missing or unplugged second controller no longer pauses the replay. The pause
  menu no longer lists Command List and Combo List.

### 1.8.1

- The replay feature's helper code (exe addresses, hooks, log, controller access, game properties, message text and
  the game's select window) is rewritten from scratch for storm4perf; earlier versions contained pieces derived from
  another project's runtime, whose license does not allow public redistribution. Replays work as before (verified in
  game: list, watch, playback controls, end menu; the 1.8.0 showcase replay plays back identically).
- The `storm-replay/` development folder is no longer part of this repository.

### 1.8.0

- `guardfix` (now on by default) is a new, verified fix: a shuriken you threw that is guarded while your next jutsu
  runs (e.g. Sage Naruto's air shuriken, then Rasengan on landing) no longer ends that jutsu without contact. The
  guard-hit code (`0x140788B4C`) marked the thrower's current attack as guarded (`SetAttackResult 0x1407824B8`, result
  at `+0x14054`); it now does that only when the guarded hit's attack id matches the thrower's current attack. The old
  guardfix patches (calls to `0x1407EBEC8`) never ran in these battles and are gone. Replays stay identical. Showcase
  replay: `docs/replays/guardfix-showcase.nsreplay`.
- `audiocrash` is on by default (verified in game).
- `inputfix` is removed: it only changed the CPU AI's request block, never player input.
- `anim60fix` is off by default: measured in game, its code never runs in a battle at 30 or 60 FPS.

### 1.7.0

- Replays: after saving from the end menu, the menu no longer offers "Replay Saved" (selecting it used to repeat the
  prompt); it comes back without the Save item.
- Replays: frame advance can be left with **Y** without opening the pause menu; restarting a replay with frame advance
  on no longer crashes; the button guide shows the speed and frame-advance controls while watching.
- Replays: **Import Replays** on the Saved tab.
- Experimental, off by default: `guardfix`, `inputfix`, `audiocrash`, `anim60fix_extra` (see above).

### anim60fix (1.3.0; off by default since 1.8.0)

`anim60fix` NOPs the `JL` at VA `0x1403f68d6`, a Retail-only throttle (`DAT_1415da7d0`) in the character state machine
`FUN_1403f6820` (Debug 1.06's `FUN_14044c8c0` has none); `anim60fix_extra` NOPs the call at `0x140a0ad14`. Both were
meant to fix choppy animation at 60 FPS. Measured in game (Free Battle, full battles at 30 and 60 FPS, patches off):
neither code path ever runs in a battle (`DAT_1415da7d0`/`DAT_1415da7d4` and the gate `nuccSys+0x590` stay 0), so
they don't change battle animation, and the cause at 60 FPS is still open. They remain as opt-in keys because story
mode and cutscenes were not measured.

## References

- Storm KB: `storm-kb/memory/storm4-performance-research.md`
- Storm KB: `storm-kb/memory/storm4-test-instance-setup.md`
- Ghidra project: Switch Trilogy `main`, NSUNS4 release 1.09 + debug 1.06
- storm4perf repo: https://github.com/exavadw/storm4perf
