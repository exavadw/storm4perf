# Lower input lag (`[lowlat]`): method and measurements

Code: `storm4perf_lowlat.cpp`. Measured 2026-10-26/27 on the test PC (RTX 4080, 60 Hz display, Windows 11), Storm 4
Steam 1.09, 30 FPS, 960x540 window, in private test copies.

## Where the delay is

One game frame ("tick") on the main thread, from a trace of every pad read, render kick, back-buffer bind and Present
(`[lowlat] trace=1`):

1. pad read (`XInputGetState`, right after the frame limiter);
2. update and draw-command recording for this tick;
3. Present (main thread) of the image drawn during the previous tick;
4. kick of this tick's draw commands to the render thread (`mmDrawCommandManager` FrameEnd `0x14018D740`, from
   `WaitVSync`);
5. the render thread draws them into the back buffer at once;
6. the limiter sleeps until the next tick.

So a drawn frame waits in the back buffer for the next tick's Present: one tick, 33.3 ms at 30 FPS. Melee has a
similar "visual buffer", which Slippi's lag-reduction codes remove. Storm 4's pad read is already as late as the engine
allows, so there is no Melee-style polling drift to fix.

## The modes

- `early`: the render thread presents at the end of its job (right after the `LeaveCriticalSection` that follows the
  busy-flag clear at `+0x88`, `0x14018C8E2`, the site `flipwait` also uses), and the main thread's next Present is
  skipped. Since 1.12.2:
  - a stub at the busy-flag clear (`0x14018C8D4`) marks the drawn image as ready first, and whichever thread takes it
    under the present lock presents it, so every image goes out exactly once. Before, when the main thread reached its
    Present first (its Flip only waits for the busy flag), the render thread presented the same back buffer again and
    the next real image was then skipped (about once in 6,300 frames);
  - the render thread presents with `DXGI_PRESENT_DO_NOT_WAIT`: if the GPU's present queue is full the image goes back
    to the main thread instead of the render thread sleeping in Present while the next frame's draw list waits;
  - it presents only with the game's vsync off (sync interval 0; vsync on gains nothing) and a windowed swapchain
    (checked every few seconds): from a fullscreen Present, DXGI may `SendMessage` to the game window, whose thread can
    be waiting for that Present's locks.
- `latent` (test only, changes gameplay): the start of each tick is held inside the tick's first pad read, so the
  Present lands `latent_offset_us` before a refresh on an exact 2-refresh cadence; the pad read happens after the wait.
  Needs in-game vsync off. Replays can diverge under it (see Replays); on its own it is test only.
- `elatent` (opt-in, changes gameplay timing): both.
- `latency1`: `SetMaximumFrameLatency(1)` on the game's DXGI device. No effect windowed (no queue builds up there).
- In fullscreen, `latent` presents with vsync (a no-vsync present 5 ms before the refresh would tear about 70% down the
  screen). Not measured.

## How it was measured (`measure=1`)

The module times virtual taps through the whole chain without pressing anything in the game:

- a tap at a random time; the first pad read after it is the tick that would see it;
- the Present that carries that tick's image (the frame of the last kick, from the kick hook) paints a 16x16 marker in the
  top-left corner, white/black flipping once per tap;
- a Desktop Duplication thread finds the first desktop image showing the flip (`LastPresentTime`, when DWM took it,
  about 2 ms after the Present); the scanout starts at the next refresh (`IDXGIOutput::WaitForVBlank` clock).

Per tap: tap to read, read to Present, in Present, Present to DWM, DWM to refresh. Excludes the monitor's own
processing. Rows go to `lowlat.csv`.

## Results

Practice battle (Naruto vs COM), storm4perf default fixes, in-game vsync off, interleaved 20 s segments, 274-276 taps
each, still in battle at the end:

| mode | press to screen mean / p95 | read to screen | vs `off` (95% CI) |
|---|---|---|---|
| `off` | 65.3 / 86.3 ms | 49.7 ms | |
| `early` | 37.2 / 53.6 ms | 19.7 ms | -30.0 ms (-30.9 .. -29.0) |
| `elatent` | 33.6 / 57.1 ms | 16.5 ms | -33.2 ms (-34.3 .. -32.1) |

The render thread presented 6313 of 6314 frames; once the render job was late and the game's own Present went through.
`elatent` missed its refresh on 1.6% of frames (each such frame shows one refresh, 16.7 ms, later).

In-game vsync on, same battle: `off` 79.0 ms, `early` 81.2 ms; read to screen 62.7 vs 63.2 ms (no gain: the vsync'd
Present still waits for the same refresh).

Vsync costs, title screen, read to the Present-carrying image: Present to DWM 8.7 vs 2.1 ms and DWM to refresh 14.9 vs
8.4 ms (vsync on vs off).

1.07 and 1.09 (vanilla copies, same data and settings, about 270-360 taps each) have the same latency within 1.2 ms in
every setting.

Title-screen numbers measured before 2026-10-27 timed the first Present after the read, which is one tick before the
Present that carries the image; their absolute values are 33.3 ms low and their differences are unaffected.

## A hang, and why `gputimer` is required

Loading a four-character battle with `early` once crashed the NVIDIA driver inside the early Present and hung the game:
other threads (resource updates while loading) take the draw manager's lock at `+0x78` before using the D3D immediate
context, and the early Present did not. It now takes that lock (after releasing the job lock, so the locks are never
held together); the same load then ran clean twice.

Without storm4perf's `gputimer` fix, `elatent` hung the game within seconds in battle: the render thread was spinning in
the game's GPU-timer readback (`0x14019F400`, `GetData` until the GPU answers) and the main thread in
`nummSkinningThreadGPU::WaitForEnd`. With `gputimer` alone, `elatent` ran 5 minutes of battle without a stall; with
the default fixes, 7.5 and 6 minutes. `early`/`elatent` therefore stay off when `gputimer` is off. A watchdog logs both
threads' stacks if the pad reads stop for 3 s.

## 1.12.2 check

Practice battle, 5 minutes, default fixes, `early`, vsync off, windowed (2026-10-27): 16,799 images presented by the
render thread, 15 by the main thread (14 of them handed back because the present queue was full), no stall; read to
Present 5.35 ms mean (p95 7.75). The guardfix showcase replay with `early`: IDENTICAL 3 of 3 (1.11.0: 4 of 5, one drifted
at tick 222; 1.12.0's replay fixes came in between, so this does not say which change cured it). The same hand-off ships in the Storm 1 online client (storm1-online
`docs/design/early-present.md`), where it also passed rollback and netplay checks.

## Replays

Each replay checks every frame against the recording; `replay.log` says `IDENTICAL` when it matches to the end.

| replay | `off` | `early` | `elatent` |
|---|---|---|---|
| guardfix showcase (568 inputs), 30 FPS | IDENTICAL 4/4 | IDENTICAL 4/5, 1 DIVERGED at tick 222 | DIVERGED at tick 222 (346 ticks) |
| 2026-10-18 Team Battle, 60 FPS | DIVERGED at tick 76 (recorded with an older mod version) | DIVERGED at tick 76 and 55 | not run |

`early` only moves the Present, yet one of five playbacks drifted at exactly the tick where `elatent` drifts, so some
simulation input in that replay depends on frame timing. Holding the start of a tick (`latent`) changes it reliably: the
simulation reads something from the clock (the engine's 60 Hz VSync callback thread feeds per-tick counters into gameplay;
Storm 1's code is known to multiply by such a counter), so `elatent` ships as an opt-in with that warning and `latent` alone stays test only.

## Storm 1

The engine code is the same in Storm 1 (Steam, PE timestamp `0x58DB8286`): post-frame function `0x1404A3B30` (Storm 1
polls XInput only for connected pads, so ticks come from there), kick `0x14010E500`, job end `0x14010D694` / call
`0x14010D6A2`, draw manager `0x140B8E598`. With the s1o client, set its `[perf] flip_wait=0` so the job-end site is free.

Measured 2026-10-27 in two offline s1o bot battles (30 FPS, windowed, about 3,600 frames per mode):

- Same order per tick as Storm 4 (trace, medians): pad read, game Present +0.15 ms (previous image), kick +0.28 ms,
  render thread draws at once, next Present at +33.5 ms.
- `early`: 2,722 of 2,723 frames presented by the render thread (one late job); read to the image's Present 4.0 ms
  (p99 6.8) instead of about 33.7 ms (the next tick's Present): about 30 ms less, the same as Storm 4.
- `elatent` missed its refresh on 1.6-1.8% of frames.
- The on-screen measurement was blocked (Windows Task View covered the window for both runs), so the Storm 1 figure
  comes from the in-game timing, not from screen capture. Replays and s1o rollback were not checked with `early`.

## Test-only settings

Under `[lowlat]` (not in the shipped ini): `measure=1` (taps, marker, `lowlat.csv`), `ab=off,early,elatent@-5000` and
`ab_seconds` (interleaved A/B), `trace=1`, `wait_start=1` (taps start when `llstart.req` appears next to the exe),
`block_real_input=1`, `no_focus=1`, `window_x`/`window_y` (the measured window goes topmost without activation so the
marker is on screen). `llshot.req` next to the exe (holding a `.bmp` path) dumps the back buffer while `measure=1`.
