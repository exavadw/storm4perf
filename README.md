# storm4perf

A drop-in `xinput9_1_0.dll` for **Naruto Shippuden: Ultimate Ninja Storm 4** on PC (Steam 1.09,
`NSUNS4.exe` SHA-256 `147e52a3…bc257`). It removes busy-waits from the engine and leaves gameplay unchanged, and it adds
two cheaper shadow levels to the game's own Options screen.
Measured at 30 FPS on an Intel UHD 770: the game uses a quarter to a third of the CPU it used before, and on a
2-thread CPU it runs at full speed instead of in slow motion. Details and numbers: [`docs/storm4-performance.md`](docs/storm4-performance.md), sections 3b and 4.

## Install

1. Download `storm4perf-<version>.zip` from Releases and copy `xinput9_1_0.dll` (and optionally `storm4perf.ini`) next to
   `NSUNS4.exe` (Steam: right-click the game > Manage > Browse local files).
2. Start the game. `storm4perf.log` appears next to the exe and lists every fix as `OK`, `SKIP` or `FAIL`.

Uninstall: delete the two files. Steam's "verify files" does not remove them, because they are not game files.

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
- Gameplay logic is not changed: only how the engine waits, plus the optional shadow levels. Online play has **not been
  tested** yet; if you try it, both players using the same FPS setting matters more than this mod. **30 FPS** (`fps=30` in `config.ini`, or in-game) is the mode the game logic was written for.
- Other mods that also ship `xinput9_1_0.dll` (controller or keyboard mods): rename theirs (e.g. `xinput_other.dll`) and set
  `[xinput] forward=xinput_other.dll` in `storm4perf.ini`.
- Mods using `steam_api64.dll` or `d3dcompiler_47.dll` proxies don't conflict.

## Build

`build.cmd` (MSVC x64 Build Tools) produces `xinput9_1_0.dll` and its `.pdb`. Set `VCVARS` to your `vcvars64.bat` if the
Build Tools are not in `C:\BuildTools`.
