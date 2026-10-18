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
  **HUD Display**, **Save Replay** (for Recent replays, hidden once saved), the command and combo lists, and
  **Return to Replays**. Restarting with frame advance on works. Your controller does not affect the replay.
- **After a replay:** **Watch Again** or **Return to Replays**.
- **After a battle you played:** the end menu has a **Save Replay** item, so you can keep it right away. Once saved,
  a message confirms it and the end menu returns without the Save Replay item.

Files: `StormReplay\replays\recent` and `StormReplay\replays\saved` next to the exe (`.nsreplay`, about 15–130 KB each),
log `StormReplay\logs\replay.log`. Settings, in `storm4perf.ini` under `[replay]`: `enabled` (1), `auto_record` (1),
`keep_recent` (20).

How it works: a replay stores the battle setup and both players' inputs, and the game replays them, so a replay is
the real battle again, not a video. Every frame is checked against the recording; `replay.log` says `IDENTICAL` when a
replay matched to the end.

Known limits:
- Replays play on the same game version and FPS setting they were recorded with (Steam 1.09).
- Online battles are not recorded yet.
- Replays recorded with an older version of this mod may drift from the original battle in the first seconds.
- In testing, one battle started through Character Select drifted from its first seconds when watched (replays from
  the list's Watch Again and battles started directly matched exactly). Send `StormReplay\logs\replay.log` and the
  replay file if you see a replay that does not match the battle.

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

Under `[fixes]` in `storm4perf.ini`. `guardfix` and `audiocrash` are verified in game and **on by default**; the two
`anim60fix` keys are experimental and off.

| key | bug | what storm4perf does |
|---|---|---|
| `guardfix` (on) | a shuriken you threw is guarded while your next jutsu runs (e.g. Rasengan), and the jutsu ends without reaching the opponent | the guard-hit code marks the thrower's current attack as guarded; with the fix only when the guarded hit came from that attack. Verified in game; replays stay identical |
| `audiocrash` (on) | unplugging or switching the audio device mid-game crashes the game | skips the call on the dead voice. Verified in game: the game shows its own XAudio2 error message and keeps running, silent until restarted |
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

`storm-replay/` is the replay feature's development project, kept here with its history: a separate `steam_api64.dll`
dev runtime with pipe automation and test tools (scripted inputs, memory watch/trace/snapshots, simulated audio loss),
Windows test scripts, and the research notes in `storm-replay/docs/research/`. It is not part of the release build; its
CMake build also needs the storm-revival-story fork snapshot (`STORM_REVIVAL_DIR`).

## Bug Fix History

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
