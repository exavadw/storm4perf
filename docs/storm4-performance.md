# Storm 4 PC performance: research notes

Date: 2026-10-16. Scope: research only. Nothing was run, patched or installed.
Target: Steam `NSUNS4.exe` 1.09, SHA-256 `147e52a3…bc257` (same hash as the Ghidra project `NSUNS4_release.exe`).
Addresses are VAs (image base 0x140000000). Function names come from the nucc-symbols package where it has them.

## 1. What the exe actually does (verified in Ghidra)

### 1.1 Settings live in `config.ini` next to the exe

`[screenSetting]` is read by `FUN_14052fe5c` (`GetPrivateProfileIntW`) and applied by `FUN_140530174`.

| key | default if missing | what the code does with it |
|---|---|---|
| `width` / `height` | 1280 / 720 | back-buffer size (`nuccSys::InitRender` 0x140a0a150) |
| `windowed` | 1 | 0 sets the fullscreen flag (0x2000) in the render init flags |
| `vsync` | 0 | 0 sets flag 0x80 (no vsync); also stored at present-object+0x128 |
| `fps` | **30** | `nuccSys::ResetFrame` 0x140a0c290: must divide 60 and be ≤ 60, else forced to 60. Stores the fps byte at sys+0x942 and the present interval `60/fps` at sys+0x4a0 |
| `shadowQuality` | 1 | see 1.3 |
| `motionBlur` | 1 | postfx+0x34; also forced off per stage when the stage disables it |
| `glare` | 1 | postfx+0x3c (bloom/glare pass) |
| `fxaa` | 1 | postfx+0xe0 AA mode; stage data can raise it to mode 2 |
| `msaa` | 1 | sample count; `>1` enables MSAA (render+0x590/0x594) |
| `ssaa` | 0 | read and stored, **never written by the options menu**; consumer not traced yet |
| `aspectx` / `aspecty` | 0 | read only; used for the fullscreen aspect/letterbox path; not written by the menu |

The in-game options screen (`FUN_1406a9e10`, loop at 0x1406ac55d) writes exactly ten keys back: fxaa, msaa, shadowQuality, motionBlur, glare, windowed, vsync, width, height, **fps**. So frame rate is a normal menu option; `ssaa` and the aspect keys are ini-only.

### 1.2 The game is frame-locked, but the 30 FPS mode is a real, speed-correct mode

- Main loop limiter `nuccSys::Update` (0x140a0c580): target = `interval*1000/60` ms (integer: **16 ms at 60 FPS**, 33 ms at 30 FPS). It measures with `timeGetTime`, `Sleep`s the remainder, and keeps a 15-frame rolling budget. There is **no frame skipping**: if a frame takes longer than the budget, the game simply runs slower (slow-motion), it does not drop frames. This is why weak PCs see "slowdown" instead of low FPS with correct speed.
- `timeBeginPeriod(1)` is called at start-up (0x14017df00, 0x14087be50), so `Sleep` is 1 ms accurate; the limiter itself is fine.
- **1,772 functions read the fps byte** (sys+0x942) and scale their frame counts and per-frame steps by `30/fps` (e.g. `FUN_14020cfcc`, `FUN_140372228`, UI step `FUN_140b08aa0`). That is the engine-wide support for running the simulation at 30 Hz with doubled steps. So `fps=30` halves CPU and GPU work per second and the game should still run at normal speed. This is the single biggest lever for a low-end PC, and it is already in the game.
- Side note: at `fps=60` with vsync off, the 16 ms integer target gives about 62.5 FPS, so the game runs about 4% fast. With vsync on a 60 Hz panel, the present call paces it to 60. On 120/144 Hz panels with vsync on, Present(1) runs at the panel rate while the limiter sleeps toward 62.5, which produces uneven pacing. An external 60 FPS cap (RTSS or the driver) with in-game vsync off is the cleaner setup there.
- Untested risk: values below 30 (20, 15) pass the validation, but the `30/fps` scaling then becomes non-integer (1.5×, 2×). 20 FPS may cause rounding errors in frame counts. Online play probably needs both sides on the same fps; this has not been checked.

### 1.3 Shadows: what the three levels really are

`FUN_140530174` → `FUN_140a40150` (cascade count at shadow+0x588) and `FUN_140a40060` (atlas tile sizes):

| `shadowQuality` | cascades | tile sizes (px) |
|---|---|---|
| 0 | 3 | 1536, 1536, 768 |
| 1 (default) | 4 | 1536, 1536, 768, 768 |
| 2 | 4 | 2048, 2048, 1024, 1024 |
| any other value | unchanged (engine default) | unchanged |

"Low" never turns shadows off; it only drops one cascade. The cascade count is an immediate (`MOV EDX,3` at 0x1405301ef, `MOV EDX,4` at 0x1405301e3). The sizes are `MOVSS` loads from a shared .rdata table at 0x141018fc4–0x141018fd0 (768/1024/1536/2048). Do not patch that table in place, because other code may read the same constants. Hook or redirect the loads instead. A runtime hook in the existing `steam_api64.dll` mod could add a real "potato" level (1–2 cascades at 512–768 px). This only changes the shadow-pass cost; the gain still has to be measured.

### 1.4 Threads

- Bullet physics ("collision" pool, `ccBulletManager.cpp`; Bullet's `Win32ThreadSupport`, 0x140e0c590): **4 worker threads**, priority HIGHEST (corrected), each hard-pinned with `SetThreadAffinityMask(1<<i)` to **logical CPUs 0–3**. On a CPU with Hyper-Threading, CPUs 0/1 and 2/3 are the same physical cores, and CPU 0 also takes most interrupts/DPCs. On 2-core/4-thread laptops, all four workers compete with the main and render threads on the same cores. On 2-thread CPUs the masks for CPUs 2–3 fail, and those threads stay unpinned.
- Other thread creation (`mmThread_Impl::Start` 0x14017d590, CRI threads at 0x14087b5a0) uses configurable masks/priorities.
- The QPC timer code re-pins the calling thread to one CPU around every `QueryPerformanceCounter` call (0x14017e050, 0x14017e180). That pattern dates from old multi-core TSC bugs and costs two syscalls per call. This is a minor cost, not a bottleneck.
- `FUN_14019d900` is a spin-wait (`Sleep(0)` loop until a 16.666 ms tick). Its owner is not identified yet. If it runs on its own thread during gameplay, it keeps one core busy, which matters on 2–4 thread CPUs. **Unverified**: needs one runtime check (thread CPU time in Process Explorer).

### 1.5 CPU waste: engine bugs and unoptimized code

**Found by profiling (section 4), and the two biggest:**
- **G. Render thread: blocking GPU-timer readback every frame.** `FUN_14019C6E0` (render-manager vtable +0x140, called from the draw-command thread at `0x14018C868`) issues `End` on two TIMESTAMP queries, then `FUN_14019F400` does `do {} while (GetData(q, &v, 8, 0) != S_OK)` three times: start, end, disjoint. That forces a full CPU↔GPU sync each frame and spins inside the driver. The result (GPU µs) goes only into `nuccSys` +0x464 as a load ratio.
- **K. Main thread: `nummSkinningThreadGPU::WaitForEnd` `0x1409F45D0`** is `mov eax,[rcx+68h]; test eax,eax; jz` back to itself, called from `WaitVSync` before the `Flip` spin. It is not covered by the `+0xE8` spin counter.


These were found statically. Each one is a real code pattern; how much it costs per frame still needs a profiler run (section 4).

**A. The main thread busy-spins every frame while it waits for the render thread.** Severity: high.
`nummRender::WaitVSync` (0x1409e1c10) is called from `nuccSys::MainEnd` every frame. At 0x1409e1c60–0x1409e1c6e it runs:
```
loop: call mmDrawCommandManager::Flip   ; 0x14018d7e0
      test al,al
      jz   loop                          ; no pause, no yield, no wait
```
`Flip` returns false straight away while the render thread is still busy: it reads one byte (+0x88), which `FrameEnd`/kick at 0x14018d740 sets and the render thread clears. The kick already uses an event (`SetEvent`), so the render thread sleeps properly. Only the main thread spins.
- If the render thread is slow (GPU-bound, or blocked in Present by vsync), the main thread burns 100% of a core for the whole wait. This is why Storm 4 always shows one core pegged, even on strong PCs.
- On 2–4 thread CPUs that spinning core is taken away from the render thread, the 4 Bullet workers and the audio/streaming threads. With Hyper-Threading, a spin on the sibling logical core also slows the render thread down directly. A spin loop without `pause` is the worst case for that.
- Fix (runtime hook): replace the loop with `while(!Flip()) { _mm_pause(); … SwitchToThread()/WaitOnAddress on the +0x88 byte }`. Better still, have the render thread signal an event that the main thread waits on. Behavior stays the same; only the waiting changes.

**B. Every timer read forces the calling thread onto CPU 0.** Severity: medium-high; may cause stutter on any CPU.
`mmTimer::GetMicroSecond` (0x14017e180) and `mmTimer::GetMilliSecond` (0x14017e050) do `SetThreadAffinityMask(self, 1)` → `QueryPerformanceCounter` → restore the old mask. The pin mask `DAT_1415afc38` = 1, i.e. CPU 0 (start-up code 0x14017df00 only moves it if CPU 0 is not in the process mask). That is 2 extra syscalls and **a forced migration to CPU 0 on every call**. Callers include `WaitVSync` (4×/frame on the main thread), the timer helpers `nummTimerGetCount`/`ResetCount`, the VSync callback thread (many calls per period, see C), and `FUN_14019d900`.
- CPU 0 is also where **Bullet worker #0 is hard-pinned at ABOVE_NORMAL priority** (1.4), and where Windows sends most interrupts/DPCs. So a timer read on the main thread can have to wait for CPU 0 to be free. That gives a stall of unpredictable length during physics-heavy moments.
- QPC has been safe across cores since invariant-TSC CPUs (Windows Vista+ handles this itself). Fix: hook both functions to call QPC without touching affinity. Low risk.

**C. "numm VSyncCallbackThread" sleeps in a wasteful way.** Severity: low-medium.
Thread proc 0x1409e0270 → loop 0x1409e05c0 (started by `nummRender::VSyncCallbackThread::Start`). It runs a callback every 16.666 ms (`0x411a` µs). The wait in between is `Sleep(elapsed_µs / 2000)` (0x1409e0640–0x1409e0656), which sleeps for half of the time already elapsed, not the time remaining:
- For the first 2 ms of every period it calls `Sleep(0)` in a tight loop. Each iteration also does the CPU-0 migration from B. That is about 12% of a core and thousands of context switches per second.
- It then sleeps 1, 1, 2, 3, 4, 7… ms and overshoots the 16.67 ms target by up to about 8 ms, so callbacks arrive irregularly. Fix: sleep `(16666 − elapsed)/1000` ms or use a high-resolution waitable timer.

**D. Spin-locks built on `Sleep(0)` in the dlmalloc heap.** Severity: unknown, possible priority inversion.
The `Nucc::Memory` heap (0x140aa7xxx–0x140aaaxxx; it prints dlmalloc's `max system bytes` stats) uses `USE_SPIN_LOCKS` with a bare `LOCK cmpxchg` + `Sleep(0)` loop (`FUN_140aa9730`, `Nucc::Memory::GetExclusion` 0x140aa7bd0). `Sleep(0)` only yields to threads of **equal or higher** priority. An ABOVE_NORMAL thread, such as a Bullet worker, spinning on a lock held by a NORMAL thread can therefore starve the holder on a busy core. Whether this happens depends on which threads allocate from this heap during battles. A profiler would show it as `Sleep`/`NtYieldExecution` hot spots.

**E. Physics workers are pinned to logical CPUs 0–3** (1.4). Removing the pin (`SetThreadAffinityMask` in 0x140e0c590), or moving it off CPU 0 and off HT siblings, lets Windows balance them. Low risk. The benefit on 2C/4T machines is probably larger than on 6+ core desktops.

**F. Not a bug, but a cost: no frame skipping.** The simulation and the render submission run in lock-step on the main thread (1.2). Any CPU spike becomes slow-motion. `fps=30` is the supported way to halve the per-second CPU cost.

Things checked that are **fine**: `timeBeginPeriod(1)` is set; the main-loop limiter sleeps rather than spins. XInput: one `XInputGetState` per controller object per update (`mmController_Win::Update` 0x1401b5620), with no back-off for a disconnected pad. Polling empty slots every frame is known to be slow in some XInput versions. Whether Storm 4 polls all 4 slots was **not verified**.

### 1.6 Rendering pipeline facts

- D3D11 only (`D3D11CreateDevice`); 2 back buffers; present interval = `60/fps`.
- Stage lighting/post setup `FUN_14085ba68` applies per-stage post parameters (AA mode, motion blur, glare, about 80 post constants) and loads `data/effect/1efcmn.xfbin` noise textures. Post-processing is data-driven per stage, so stage xfbin edits can also disable passes.

## 1b. Same engine, same bugs: Storm 1 / 2 / 3 HD / Full Burst / 4 / Connections (static, 2026-10-16)

Checked in Ghidra against all PC builds; names come from the Switch Trilogy symbols. "P" = present in the same form; addresses are the function in that build.

| # | Issue | Storm 1 | Storm 2 | Storm 3 HD | Full Burst (2014, x86, D3D9) | Storm 4 1.09 | Connections |
|---|---|---|---|---|---|---|---|
| G | **GPU frame timer read back with a blocking `GetData` spin every frame** (render thread) | P `0x1401218d0` | P `0x140175d00` | P `0x14014f960` | not present | P `0x14019f400` | P `0x141417570`, plus a second copy `0x141416620` |
| K | **Main thread spins on `nummSkinningThreadGPU::WaitForEnd`** (`mov eax,[rcx+68h]; test; jz` to itself) | P `0x1404738a0` | P `0x140574250` | P `0x1406783c0` | not present | P `0x1409f45d0` | P `0x141253640` (flag at +0x78) |
| 1a | Main thread spins on `mmDrawCommandManager::Flip` in `WaitVSync` | P `0x14044f8a0` | P `0x14055f140` | P `0x140662f00` | changed (no mm layer) | P `0x1409e1c10` | P `0x14121b570` |
| 1b | Present pacing: `Present(1)` then a `Sleep(0)` spin to 16.667 ms (S4: only with in-game vsync on) | P `0x14011fdd0` | P `0x140174200` | P `0x14014de60` | not present | P `0x14019d900` | **worse** `0x141417370`: spins even with vsync off |
| 2 | Timer pins the caller to CPU 0 around every QPC read | P `0x1400ffc50` | P `0x140158520` | P `0x14012d760` | not present (plain QPC) | P `0x14017e180` | P `0x1413d7490` |
| 3 | Bullet pool: 4 workers, priority **HIGHEST**, pinned to CPUs 0–3 | P `0x1405e3710` | P `0x140704ff0` | P `0x14080a1c0` | not present | P `0x140e0c590` | P `0x1415e2420` |
| 4 | VSync callback thread `Sleep(elapsed/2000)` | P `0x14044e230` | P `0x14055daa0` | P `0x140661890` | changed (`GetRasterStatus` poll) | P `0x1409e05c0` | P `0x141218950` |
| 5 | dlmalloc `Sleep(0)` spinlock | P `0x14052f350` | P `0x14062a2a0` | P `0x14072dc20` | not present | P `0x140aa9730` | P `0x141397cb0` |
| 8 | One global `CRITICAL_SECTION` around every TLSF allocation | P | ? | P | ? | P | **worse** (3 call layers + JMC) |
| 10 | `/JMC` debug checks compiled into release (2,531 functions) | – | – | – | – | – | **new** |

- **Full Burst** (2014) predates the `mm`/`numm` engine layer and has none of these.
- From Storm 1 HD (2017) onward, every build carries the same code. Connections (2021) fixed only the old `nuccSys::Update` limiter. It made three things worse: the pacing spin now runs with vsync off, allocations go through more layers, and `/JMC` code is compiled in.
- So the same hook-DLL fixes apply to every game, each with its own addresses.

Corrections to section 1:
- Bullet priority is `THREAD_PRIORITY_HIGHEST` (2), not ABOVE_NORMAL.
- `mmShaderEffectCompiler::Compile` does **not** compile HLSL at runtime. Shaders load as precompiled `.fxo` through `CreateFromBinaryFile`, so this is not a source of hitches.

## 2. Community knowledge (web pass, 2026-10-16)

Tags: [2+] = two or more independent sources, [1] = single source. Full source list is in section 5.

- **Slow motion = frame-locked logic [2+].** Players describe the game as "frame dependent". Below about 50–60 FPS it slows down instead of dropping frames. This matches 1.2.
- **30 FPS was the launch mode [2+].** 60 FPS came in a 2017 patch (DSOGaming, Digital Foundry). Known side effects at 60 [2+]: menus read inputs twice, some combos break, and substitution refills faster. None of these happen at 30. This fits the code: the engine is built for 30 and *scales up* to 60 through the 1,772 `30/fps` sites, and a few places were missed. **30 is the more correct mode, not a degraded one.**
- **Online:** both players must use the same fps setting, or a substitution can disconnect you [1].
- **fps values:** above 60 is reverted to 60 [1], which matches `ResetFrame`. "fps=20 slows down perfectly" [1]: ambiguous, and it fits the rounding risk noted in 1.2. Do not recommend values below 30.
- **MSAA is the most-blamed setting [2+].** Example: GTX 1050 laptop, MSAA 2x → 45–50 FPS and slow motion; MSAA off fixes it. The guide also says MSAA crashes after an update.
- **Resolution is the biggest lever on iGPUs [2+].** Intel HD needs 800×600 or 480p–720p to hold 60 or 30.
- **Laptop running on the iGPU is the #1 cause of "slow motion" reports [2+].** Even RTX 3070 laptops hit it. Fix: Windows Graphics settings → high-performance GPU. Also: High Performance power plan, monitor cable in the graphics card, in-game vsync off (it is "more aggressive") with driver adaptive vsync instead [2+].
- **Config key conflict.** One video shows `smaa` and `mipmap` keys, and the Storm 1 guide shows `textureQuality`/`framelimit`. The 1.09 reader (`FUN_14052fe5c`) reads **none of these**, so they belong to other games/versions or launchers. They do nothing in 1.09.
- **Mods:** the "Tormunds FPS fix" (hex-blanking shadow/particle strings) is disowned by its own author since 1.07 because it crashes and leaks; avoid it. Storm 4 Evolution's launcher exposes settings, and its fixes are gameplay changes. Nexus "Storm 4 Native 120 FPS" (HenDGS) uses a **proxy `D3DCompiler_47.dll`** and is Free Battle only, which shows that frame-rate hooks are practical. No low-res texture or "potato" packs exist.
- **DXVK:** one report says it "fixed all slowdown/stutter since NUNS 4" (Connections thread); another crashed when leaving windowed mode [1]. Proton is Gold, and `WINE_FULLSCREEN_FSR=1` upscaling works.
- **Not found anywhere:** profiling data, HDD-vs-SSD numbers, a Special K/dgVoodoo/AFMF report, Steam-overlay or mouse-polling bugs, or official notes for the 1.07–1.09 performance changes. **Nobody has publicly identified the CPU issues in 1.5.**

## 3. Ranked options

### 3a. User-side, no code (safe, try in this order)
1. Dedicated GPU selected (Windows Graphics settings / vendor panel), High Performance power plan, monitor on the graphics card.
2. **`fps=30`.** This is the mode the game logic was written for. 60 FPS was patched in later, and it has real logic bugs that the user has seen and the community reports: menus double-step or read inputs twice, combos change, substitution refills faster. In the profiling below, menu presses moved two entries at 60 FPS under load and exactly one at 30. So 30 is the correct mode, not a fallback. It also halves CPU and GPU work, and online opponents must match.
3. `msaa=1` (MSAA off; values `>1` enable it), `shadowQuality=0`, `motionBlur=0`, `glare=0`, `vsync=0`; keep `fxaa=1` as a cheap AA. **MSAA is the most expensive GPU setting by far:** with it, the engine resolves the whole frame ~30 times per frame, which is half the GPU time at 1080p. On the UHD 770, 1080p went from 11 to 29 FPS with `msaa=1` (4c).
4. Lower `width`/`height` (1280×720 → 960×540 → 800×600 on iGPUs), `windowed=0`.
5. External frame cap (RTSS/driver) at the target with in-game vsync off, for even pacing (1.2). In-game vsync also turns on the `Sleep(0)` pacing spin (1b, row 1b).
6. Optional: DXVK (reversible, one report), Lossless Scaling upscaling or frame-gen on top of 30 (no Storm 4 numbers).

### 3b. Mod-side CPU fixes (runtime hook DLL), ranked by what was **measured**
None of these change gameplay or online determinism; they only change waiting and scheduling. Addresses are for Storm 4 1.09; see 1b for the other games.

| # | fix | where (S4 1.09) | measured effect | risk |
|---|---|---|---|---|
| 1 | **GPU timer: stop the blocking `GetData` spin.** Prototype: return "no result" (`xor eax,eax; ret`). **storm4perf (4b):** replaces vtable +0x138/+0x140 (`0x14019C690` begin, `0x14019C6E0` end) with a ring of 8 query sets read with `D3D11_ASYNC_GETDATA_DONOTFLUSH`; sys+0x464 keeps getting the newest finished value, 3–4 frames late. | `0x14019F400` (called from the render thread via render-manager vtable +0x140; result only feeds the load meter at sys+0x464) | **−0.9 to −1.1 cores** in every config. 60 FPS prototype on 2 threads: 48.8 → 58.8 FPS, slow motion gone. **30 FPS release: 2 threads 25.8 → 29.3 FPS; heavy settings p95 no longer worse** | low |
| 2 | Skinning wait: `pause` (64×), then `SwitchToThread` for 1 ms, then `Sleep(1)` | `nummSkinningThreadGPU::WaitForEnd` `0x1409F45D0` (whole body re-implemented) | most of its cost disappears once fix 1 is in, because the render thread stops lagging. Fixes 2–5 together: −1 to −15 points of CPU on top of fix 1 at 30 FPS | low |
| 3 | `WaitVSync`/`Flip` wait: `WaitOnAddress` on mgr+0x88; the draw-command thread's `LeaveCriticalSection` right after it clears the flag (`0x14018C8E2`) is redirected to also `WakeByAddressAll` | loop at `0x1409E1C60` | small on its own (spin was 0.3 ms/frame mean in the light config) | low |
| 4 | Timer QPC without the CPU-0 affinity dance | IAT hook on `SetThreadAffinityMask`, pseudo-handle only, and only for the 10 pin/restore call sites (by return address) in `0x14017E050`, `0x14017E180`, `0x14017DFD0`, `0x1401B6830`, `0x14019D900` | part of the "all" set; the soak skipped ~3,100 pin calls/s | low |
| 5 | VSyncCallbackThread: sleep toward the 16,666 µs deadline (`remaining/1000 − 1` ms, then 1 ms); a stub passes the period start from RBX | call at `0x1409E0656` | that thread used ~6% of a core, 5% of it kernel time, ~1,100 context switches/s; fixed as part of "all" | low |
| 6 | Unpin the Bullet workers (storm4perf `bulletunpin`, **off by default**) | `SetThreadAffinityMask` call at `0x140E0C8FB` in `0x140E0C590` (pool created once at start-up; storm4perf records the 4 workers so a live toggle re-pins them) | **no measurable effect** (4b): CPUs 0–3: p95 36.12 vs 36.05 ms, CPU 32 vs 33%; CPUs 0–1: p95 36.54 vs 36.51 ms | low |
| 6b | vsync-on `Present` pacing spin (storm4perf `presentpace`, **off by default**) | `Sleep(0)` call at `0x14019DA69` in `FUN_14019D900`; a stub passes `R15 − RAX` (target − elapsed, µs) | with in-game vsync on, the render thread falls from ~90% to 9–21% of a core and process CPU from 107–124% to 22–53%. FPS was unchanged in 5 of 8 runs, but **1–3 FPS lower in 3 runs**, all in one window with outside load (4b) | medium (pacing) |
| 7 | Potato / off shadow levels (**storm4perf 1.2.0**, `[gpu] shadows`, also in Options > Shadow Quality) | calls `0x14085BC55`/`0x14085BCAD` in `ccGameDrawEnv::BeginScreen` (4d) | GPU: −5.4 ms (VERY LOW) / −5.9 ms (OFF) at 1080p on the UHD 770 | low (visual, user-chosen) |
| — | `SetMaximumFrameLatency(1)` together with fix 1 | `IDXGIDevice1` | **worse** (11.2 vs 12.6 FPS GPU-bound): do not use | — |

Prototype: `tools/xinput_proxy.cpp` next to this doc (deployed at `C:\dev\storm-perf\tools` on the desktop). The profiler (`stormprof.cpp`), the analysis scripts and the raw A/B runs are in `tools/` and `runs/`. It is an `xinput9_1_0.dll` proxy that checks the original bytes before every patch and toggles each fix live through `C:\dev\storm-perf\fixes.txt`, suspending other threads first so no patch is applied mid-instruction. **Release module: `storm4perf/` next to this doc** (standalone `xinput9_1_0.dll`, chosen by the user over the story fork's `steam_api64.dll`). Results are in section 4b. The vsync-on `Present` pacing spin (`0x14019D900`, row 1b in 1b) is fix 6b, which is opt-in. Its timer pins are always removed, through fix 4.

### 3c. Mod-side GPU savings (measured, see 4c)
Ranked by gain ÷ visual cost on the UHD 770: potato shadows (2 cascades at 768/512, −2.4 ms at 720p, −5.3 ms at 1080p); keep only the first per-frame depth copy (−2.8 to −8.7 ms, needs per-effect checks); render scale 0.75 with native UI (−8 ms at 1080p, only with glare and motion blur off, set at launch); shadow-clear de-dup (−0.1 to −0.6 ms, lossless). A texture LOD/mip skip gave no measurable GPU gain. Prototype: `gpu/stormgpu.cpp`. **Shipped in storm4perf 1.2.0 (4d):** VERY LOW (potato) and OFF shadow levels, engine-side, chosen in the game's own Options > Shadow Quality row and saved to `storm4perf.ini`. Not shipped: depth-copy skip, render scale, clear de-dup (reasons in 4d).

## 4. Profiling results (2026-10-16, Windows desktop, separate vanilla copy)

**Setup.**
- Copy at `C:\dev\storm-perf\game\NSUNS4_perf.exe`, with `OWNER.txt`; `data`/`dlc` are junctions to the Steam install.
- Forced onto the **Intel UHD 770 iGPU** (per-exe GPU preference, removed afterwards).
- CPU affinity of 4 logical CPUs (`F00` = 2 P-cores with HT) or 2 logical CPUs (`300` = 1 core with HT).
- Free Battle COM vs COM on Hidden Leaf Village (After the War), team match.
- Input came from my scripted `xinput9_1_0.dll` proxy; the game was kept running unfocused by patching the focus-lost handler `0x140AB8880` to return 0.
- Sampler `stormprof.exe` (on separate E-cores) recorded:
  - per-thread CPU time and context switches every 100 ms;
  - RIP samples at 500 Hz, plus optional stack walks;
  - the game's own frame counter and spin timers from `nummRender` (`+0xD0`, `+0xE0`, `+0xE8`).
- Fixes were toggled **live within the same match** in 20 s segments: off / all / off / GPU-timer-only, 3 cycles each.

**Where the time goes (baseline, 60 FPS, light settings, 4 threads, 58.7 FPS):**
- Render thread: **85% of a core**. 70% of its samples sit in a loop at `0x14019F470` calling `ID3D11DeviceContext::GetData` until the GPU timestamp queries finish. That is a full CPU↔GPU sync every frame, spinning inside the Intel driver (`igd10um64xe.dll`).
- Main thread: **44%**. About 39% is sleeping in the limiter; **31% of samples are the `WaitForEnd` skinning spin**; `WaitVSync`/`Flip` adds about 0.4 ms/frame.
- VSyncCallbackThread: 6% (mostly kernel: `ZwDelayExecution` plus `ZwSetInformationThread`, i.e. the CPU-0 affinity calls).
- XInput: all **4 slots are polled every frame**. Empty slots cost about 64 µs/frame in total (~0.4% of a frame), so this is not worth fixing.

| config | set | FPS | p95 frame | slow frames | process CPU | busiest thread |
|---|---|---|---|---|---|---|
| 60 FPS, light, 4 thr | off | 58.4 | 20.1 ms | 4.1% >20 ms | **159%** | 84% |
| | gputimer | 58.8 | 19.0 ms | 1.4% | **69%** | 16% |
| | all | 58.2 | 19.4 ms | 2.7% | 67% | 17% |
| 60 FPS, light, **2 thr** | off | **48.8** (slow motion, 81% speed) | 26.5 ms | **52.6%** | 165% | 81% |
| | gputimer | **58.8** (full speed) | 21.5 ms | 10.3% | 87% | 25% |
| | all | 58.9 | 21.9 ms | 10.3% | 86% | 26% |
| **30 FPS**, light, 4 thr | off | 29.4 | 36.5 ms | – | **109%** | 69% |
| | gputimer | 29.4 | 36.8 ms | – | 48% | 10% |
| | all | 29.4 | 36.1 ms | – | **42%** | 10% |
| 30 FPS, **heavy** (1080p, MSAA, shadow 2, blur, glare), 4 thr | off | 12.0 | 99.6 ms | – | 209% | 96% |
| | gputimer | 12.6 | 122.9 ms | – | 96% | 66% |
| | gputimer + latency 1 | 11.2 | 106.9 ms | – | 114% | 81% |

**Reading the table.**
- The GPU-timer spin is the biggest CPU cost in the engine. It wastes about one full core in every configuration, and it forces the CPU to wait for the GPU every frame.
- On a CPU with few cores, removing it turns a slow-motion game (48.8 FPS, half the frames late) into a full-speed one.
- When the GPU itself is saturated, FPS stays GPU-bound, but CPU use still halves. On laptops, where the CPU and iGPU share one power budget, that could let the iGPU clock higher; this is **untested** here (desktop CPU).
- Not measured: the vsync-on pacing spin (1b), Bullet unpinning, and real low-end hardware (old dual-cores, real laptops).

### 4b. Release module `storm4perf` at 30 FPS (2026-10-16)

**What it is.** `storm4perf/storm4perf.cpp` builds to `xinput9_1_0.dll`. The game imports only `XInputGetState`/`XInputSetState`. The proxy forwards all four XInput 9.1.0 exports to System32, or to `[xinput] forward=`.
- In `DllMain`, before game code runs, it checks the original bytes at every site and patches only the fixes that match. Anything else is logged as `SKIP` with the expected and found bytes.
- Each hook reads an on/off flag on every call. With `[debug] live=1` the fixes switch live through `storm4perf.ini`, so no code is rewritten while the game runs. This replaces the prototype's thread-suspend patching.
- `storm4perf.log` records patch results, D3D query errors, XInput load errors, crashes in storm4perf code (vectored handler), the first 8 fatal first-chance exceptions anywhere, and counters every 5 minutes.
- Test-only `stormharness.dll` (scripted pad, focus patch, screenshots) is chained through `forward=`, so the tests ran the release binary unchanged.
  - The 30 FPS tables, the soak and the iGPU runs used 1.0.0 (SHA-256 `9c4e9429…a1c1fe`).
  - The Bullet, pacing and 60 FPS runs used 1.1.0 builds. They add `presentpace` and Bullet worker recording and change no default-fix code.
  - Final 1.1.0: `f2838460…e439f5`.

**Setup.** Same copy, iGPU and match as section 4, `fps=30`, vsync off, windowed.
- light = 1280×720, msaa 2, shadow 0, blur/glare off; heavy = 1920×1080, msaa 2, shadow 2, blur and glare on.
- Each run interleaves 20 s segments live in one match (off / all / off / gputimer-only), 3 cycles, 7 cycles for light 2 threads. `tools/ab.sh` screenshots after every segment. Segments that end on the post-match menu are discarded, and the driver picks Rematch.
- Raw runs: `runs/l4_*`, `l2_*`, `l2b_*`, `h4_*`, `h2_*`. Table values are the **median over segments** (`tools/compare.py --median`). `runs/s30l4_*` is an earlier light-4 run contaminated by a match end and is not used.
- **Noise:** other sessions' Storm instances (`agent-rev`, `storm-replay`) ran on the same desktop during these runs, and the iGPU sat at ~95%. Heavy runs are GPU-bound, and p95 varies about ±40% with the scene even inside "off".

| config | set | FPS | p95 | p99 | process CPU | busiest thread |
|---|---|---|---|---|---|---|
| light, 4 thr (3 cycles) | off | 28.4 | 40.9 ms | 44.6 ms | **194%** | 94% |
| | gputimer | 28.0 | 40.1 ms | 50.5 ms | 47% | 10% |
| | **all** | **29.4** | **38.1 ms** | **40.6 ms** | **44%** | 12% |
| light, **2 thr** (7 cycles) | off | **25.8** (slow motion) | 46.5 ms | 51.0 ms | 147% | 86% |
| | gputimer | 29.3 | 39.0 ms | 43.0 ms | 62% | 15% |
| | **all** | **29.1** (full speed) | 39.3 ms | 44.6 ms | **52%** | 15% |
| heavy, 4 thr | off | 8.1 | 153 ms | 173 ms | 166% | 75% |
| | gputimer | 8.9 | 131 ms | 143 ms | 47% | 9% |
| | all | 8.9 | 146 ms | 167 ms | 47% | 11% |
| heavy, 2 thr | off | 9.9 | 116 ms | 125 ms | 165% | 91% |
| | gputimer | 11.1 | 105 ms | 123 ms | 32% | 8% |
| | all | 12.0 | 123 ms | 142 ms | 29% | 7% |

**Reading it.**
- **CPU:** the default set ("all") cuts process CPU to **a quarter to a third** in every config: 194→44%, 147→52%, 166→47%, 165→29%. The busiest thread drops from 75–94% of a core to 7–15%.
- **2 threads, light:** the unpatched game falls into slow motion at 30 FPS (25.8 FPS; 36% of frames over 40 ms). With the fixes it holds 29.1–29.3.
- **Pacing:**
  - Light configs: "all" beat the mean of its two neighbouring "off" segments on p95 in 7 of 10 cycles. Of the 3 losses:
    - one was within 1% (44.4 vs 44.0 ms);
    - one was a GPU-heavy team ultimate that slowed every set in that cycle to 11–19 FPS;
    - one (`l2_c2_all`, 26.1 FPS, p95 60 ms) had the main thread blocked in a `win32u` GPU wait rather than in storm4perf code, so it is probably the scene, but this is not proven.
  - Heavy configs: no consistent direction, within scene noise. FPS went up in both.
  - The prototype's p95 regression under GPU saturation (99.6 → 122.9 ms) does **not** appear with the non-blocking read. Heavy 4 threads: 153 → 131 ms for gputimer-only.
  - gputimer-only shows one p99 outlier in light 4 threads (69.6 ms, a single segment). "all" has none.
- **GPU timer:** finished results come back 3.5–3.7 frames late on average, max 4. 0 drops and 0 errors over the soak, 1 disjoint sample.

**10-minute soak** (`tools/soak.sh`, release config `live=0`, default fixes, light, 4 threads):
- No crash. The process stayed alive at all 20 checks, and one match ended and rematched normally.
- `storm4perf.log` has no errors and no exceptions.
- 20 screenshots, no corruption. One is a flat purple frame with the HUD intact, which matches Sasuke's Susanoo transition flash; the frames after it are normal.
- Counters after 10 min: 13,781 GPU timer reads, 13,782 flip waits, 934,686 skipped timer pins (~3,100/s).

**Follow-up runs (1.1.0, RTX 4080 instead of the iGPU, because a GPU-profiling session needed the iGPU idle; light settings; 20 s segments, median):**

| test | config | sets | FPS | p95 | p99 | process CPU |
|---|---|---|---|---|---|---|
| Bullet unpin | fps 30, CPUs 0–3, 4 cycles | all / all + bulletunpin | 29.4 / 29.4 | 36.12 / 36.05 ms | 37.31 / 37.45 ms | 32% / 33% |
| | fps 30, CPUs 0–1, 4 cycles | all / all + bulletunpin | 29.4 / 29.4 | 36.54 / 36.51 ms | 38.09 / 38.27 ms | 37% / 36% |
| present pacing (vsync=1, 60 Hz) | quiet, 4 cycles | base / + presentpace | 30.0 / 30.0 | 34.84 / 34.90 ms | 36.35 / 35.76 ms | 124% / 53% |
| | quiet, 3 cycles, wake margin | base / 0.5 / 2 / 4 ms | 30.0 all | 34.68 / 34.92 / 34.77 / 34.71 ms | 35.2–35.4 ms | 117% / 36 / 43 / 42% |
| | + 2 busy threads on its 4 CPUs | base / 0.5 / 2 / 4 ms | 30.0 all | 34.70 / 35.04 / 34.77 / 34.81 ms | 35.2–35.5 ms | 120% / 38 / 40 / 46% |
| | + 4 busy threads, 4 cycles | base / no boost / time-critical boost | 30.0 all | 34.68 / 34.80 / 34.75 ms | 35.2–35.6 ms | 107% / 22 / 23% |
| | **outside load** (other sessions busy), 3 cycles each | off / base / all / gputimer | 29.7 / 29.7 / **28.7** / 29.7 | 35.3 / 35.6 / **41.2** / 35.2 ms | 41–42 / **47.6** ms | 117 / 109 / 51 / 116% |
| 60 FPS, vsync off | 3 cycles | off / all / gputimer | 58.9 / 58.9 / 58.9 | 19.52 / **19.04** / 19.95 ms | 22.62 / **20.00** / 24.00 ms | 79% / **53%** / 61% |

- **Bullet unpinning makes no difference**, either when the pins land on 4 logical CPUs or on 2. On this desktop, Windows' scheduling of pinned workers was not the bottleneck. It stays off.
- **Present pacing** removes the vsync-on spin, the largest remaining CPU cost with in-game vsync on.
  - In 5 of 8 runs it matched the game's frame times exactly, including with 2 or 4 busy threads added on the same CPUs.
  - Three runs (`v4s`, `v4t`, `v4u`) fell in one ~30-minute window while other sessions' games were loading and running. In all three, adding it cost 1–3 FPS and about 6 ms of p95, consistently across segments, while the interleaved sets without it were fine.
  - Its counters show rare very late wake-ups (worst 57 ms). A time-critical priority boost during the wait was added afterwards. That window could not be reproduced, so the boost is unverified against it.
  - It ships **off**; turn it on with `presentpace=1`. In-game vsync off with an external cap (3a) avoids the spin entirely.
- **60 FPS:** the release module saves 26 points of process CPU on the dGPU (the GPU-timer spin is cheaper there than on the iGPU) and improves p99 and slow frames (3.7% → 1.0% over 20 ms). 60 FPS keeps the game's own logic bugs: during these tests the scripted menu inputs double-stepped into the in-game card shop (nothing bought; all entries sold out).
- **Not done:** real low-end hardware (old dual-cores, laptops with a shared CPU/iGPU power budget), and the potato shadow level (7).


### 4c. GPU: where the frame goes on the iGPU, and what is cheap to cut (2026-10-16)

**Setup.**
- Own copy `C:\dev\storm-gpu\game\NSUNS4_gpu.exe` (same 1.09 hash, vanilla `steam_api64.dll`, `OWNER.txt`, junctions to the Steam `data`/`dlc`). Forced onto the **Intel UHD 770** with a per-exe `UserGpuPreferences` entry (removed afterwards). Process pinned to CPUs 12–15 at above-normal priority.
- Profiler and prototypes: `gpu/stormgpu.cpp`, built as `xinput9_1_0.dll` (scripted pad on slot 0, focus patch, back-buffer screenshots, live settings apply, and the prototype's `gputimer` patch from section 4, `FUN_14019F400` → "no result", so the render thread never waits on the GPU).
- **Per-pass GPU time:** D3D11 vtable hooks on `OMSetRenderTargets(AndUnorderedAccessViews)` cut the frame into passes; `Present` closes the frame. Each cut ends a `TIMESTAMP` query inside one `TIMESTAMP_DISJOINT` query per frame. A ring of 8 frames is read back 3–7 frames later and never waits. A pass is labelled by its bound targets (size, format, sample count, depth). `gpu/passes.py` groups passes into phases, and `gpu/compare.py` aggregates interleaved A/B segments.
- Free Battle COM vs COM, Hidden Leaf (After the War), `fps=30` unless noted. Prototypes are switched **live in the same match** in 12–15 s segments: every set once per cycle, 3 cycles (`gpu/ab.sh`). Same-moment screenshots: the main thread is held in the `WaitVSync` hook and stepped frame by frame (`gpu/vpair.sh`), so an off/on pair is 3–20 game frames apart.
- Settings: **light** = 1280×720, msaa off, shadow 0, blur/glare off. **heavy** = 1920×1080, msaa 2, shadow 2, blur, glare. **mid** = heavy with msaa off.

**Measurement pitfalls found on the way (they matter for anyone repeating this):**
- On this Intel driver, timestamp queries read with `D3D11_ASYNC_GETDATA_DONOTFLUSH` from the presenting thread **never complete**; reading with flags 0 (still non-blocking) works. Storm 4 issues draws on its draw-command thread and calls `Present` from another thread. The engine serializes the two, so they never use the immediate context at the same time.
- d3d11 gives each context a heap-allocated vtable and **rewrites the draw and clear slots itself** (state-dependent fast paths). Hooks on those slots have to be re-applied, and draw counts are a lower bound.
- The Steam overlay re-hooks the swapchain vtable when the game creates its swapchain. A `Present` hook installed earlier calls itself in a loop (stack overflow), so it is installed 4 s after device creation.
- **The UHD 770 clocks down when it is not saturated.** At light settings and 30 FPS the GPU is ~50% busy: the same full-screen pass took 93 µs in one segment and 195 µs in another, and the busy time per frame went from 12.7 ms (GPU clocked up, at fps=60) to 15.8–23.6 ms (30 FPS). Below full load the per-frame wait for the CPU (~20 ms) also lands inside whichever pass is open. So:
  - light numbers below are measured with **fps=60** (measurement only; the frame's work is the same, and the GPU stays clocked up);
  - every phase is the **median over frames** (drops the idle gap), and "busy" is the sum of the phase medians;
  - a saving at partial load is partly eaten by the clock going down (in the light runs the untouched shadow phase got 20–26% slower when other work was removed). Fixed-clock savings are given per phase. They are what a GPU-bound weak iGPU would get.
- The desktop was shared: a 26-thread batch job and two other sessions' game instances (on the RTX 4080) ran throughout, and the CPU-fix session's instance used the iGPU only before these runs. Heavy settings were GPU-bound (span = busy, stable clock proxy); scene content still moves heavy frames by ±15%, because every extra effect layer on screen adds a resolve and a depth copy.

**What a frame is made of** (same order every frame):
1. **Shadows:** 4–5 clears of a fixed **2048×2048** depth buffer (even at shadow 0), then per cascade: the casters' depth pass, and a full-screen **filter pass into an RG16F 2048×5120 atlas** (moments, i.e. VSM-style soft shadows). The atlas is allocated at the shadow-2 size for every level.
2. **Scene in layers:** before each layer (sky, stage, characters, then one layer per active effect group) a full-screen pass **copies scene depth into an R32F target** (13–15 copies per frame). Each layer then draws into 2 colour targets + depth (MRT).
3. **With MSAA on:** after each MSAA layer, a full-screen **shader resolve** writes colour ×2 + depth into non-MSAA targets: ~30 such passes per frame (nearly all of them resolves), each reading the whole 1080p frame.
4. **Post:** ~30 full-screen RGBA8 passes at full resolution (scene copies and AA/edge/tone passes). Glare uses 8 small RGBA16F passes; motion blur adds ~7 full-res passes.
5. **Final + UI** on the back buffer (~100 binds, 0.4–0.6 ms).

**GPU ms per frame by phase (UHD 770, median of 3 segments each):**

| phase | light 720p (fps=60 clock) | share | mid 1080p, msaa off | share | heavy 1080p, msaa 2 | share |
|---|---|---|---|---|---|---|
| shadow depth (clears + casters) | 1.28 | 10% | 2.16 | 8% | 2.49 | 3% |
| shadow filter (RG16F atlas) | 2.41 | 19% | 4.46 | 16% | 4.52 | 5% |
| depth copies (R32F, ×13–15) | 1.29 | 10% | 2.97 | 11% | 12.96 | 14% |
| scene layers (MRT) | 5.29 | 42% | 11.89 | 43% | 19.56 | 21% |
| **MSAA shader resolves (×30)** | – | – | – | – | **46.28** | **50%** |
| post (full-res passes) | 2.05 | 16% | 5.25 | 19% | 6.72 | 7% |
| final + UI | 0.36 | 3% | 0.61 | 2% | 0.58 | 1% |
| **busy total** | **12.7** | | **27.4** | | **93.2** | |
| FPS (fps=30 setting) | 29.4 (GPU ~50% busy) | | 29.4 | | **11.0** (slow motion) | |

Raw: `gpu/runs/L60_*`, `M_*`, `H_*` (`.txt` = per-key summary and last-frame sequence, `.seq` = 90 frame sequences); `L1_*` = light at 30 FPS (clock-floating, not used for numbers).

**Reading it.**
- **MSAA is half of the heavy frame.** The engine resolves the whole frame after every scene layer, ~30 times, at ~1.5 ms each at 1080p. It also makes each depth copy 4× more expensive (0.6–0.9 ms instead of 0.2), because the copy reads the MSAA depth. `msaa=1` takes heavy from 93 to 28 ms and **11 → 29 FPS** (measured below). This is why "MSAA 2x → slow motion" is the most common community report (section 2).
- **Shadows cost 29% at light settings.** The filter into the RG16F atlas costs more than drawing the casters, and shadow quality 0 still clears the full 2048² depth buffer 4–5 times.
- The R32F depth copy before every layer costs 10–14%. The copies repeat even when the layer in between drew nothing that writes depth.
- Glare and motion blur are cheap (≤0.5 ms and ~1.1–1.6 ms at 1080p). Texture sampling is not a bottleneck (see texture LOD below).

**Prototypes** (all live toggles in `C:\dev\storm-gpu\gpu.txt` except render scale; gains are GPU ms per frame, phase-targeted at a fixed clock unless noted).

| # | prototype | how | light 720p | mid 1080p | heavy 1080p msaa 2 | visual cost | screenshots |
|---|---|---|---|---|---|---|---|
| a | **MSAA off** (existing option `msaa=1`) | ini + live apply (the apply re-reads `msaa`) | – | – | **−65 ms total (93→28), 11.0→29.1 FPS** | jagged edges; FXAA stays on | `gpu/shots/doc/nomsaa.jpg` |
| b | **render scale** 0.75 / 0.5, UI native | scene targets with the back-buffer size that are also sampled are created smaller (`CreateTexture2D`), and viewports and scissors are scaled while they are bound. The back buffer and its depth stay native, so the UI stays sharp, and the game's final pass stretches the scene. **Launch-time only** | not validly measured (live-toggle runs, image broken) | with blur + glare off, separate launches: **0.75: −8.0** (scene phases 17.6→9.5); **0.5: −12.1** (→5.5); raw busy 24.6→21.1/21.7 because the GPU clocked down | heavy: image broken (MSAA + glare/blur), not usable | 0.75 soft; 0.5 blocky (the stretch is unfiltered). UI native. **Image breaks into tiles when glare or motion blur is on, or after a live toggle** | `gpu/shots/doc/rscale.jpg` |
| c | **"potato" shadows**: 2 cascades at 768/512 px | calls `FUN_140a40150`/`FUN_140a40060` with own values from the main thread; the .rdata table is untouched | **−2.4** (shadow 3.69→1.29) | **−5.3** (6.62→1.29; total −5.4) | −5.6 (7.01→1.44) | softer, slightly blockier character shadows; still grounded | `gpu/shots/doc/potato.jpg` |
| c′ | 1 cascade at 768 | same | not timed | not timed | not timed | shadows only near the camera | `gpu/shots/doc/potato1.jpg` |
| c″ | **no shadow pass** | zero viewport while the shadow targets are bound, skip the 2048² depth clears | −3.7 | −6.6 | −7.0 | no shadows at all, characters look pasted on | `gpu/shots/doc/noshadow.jpg` |
| d | **keep only the first depth copy** per frame (`dcopy1`) | zero viewport for the 2nd+ R32F pass | ≤−1.3 (upper bound, all copies skipped) | **−2.8** (2.97→0.21) | **−8.7** (12.96→4.28) | none seen in the sampled frames; soft-particle depth no longer includes geometry drawn in later layers (needs a per-effect check) | `gpu/shots/doc/dcopy1.jpg` |
| e | motion blur off (existing option) | live apply | – | −1.2 total | post −1.45 | small | `gpu/shots/doc/noblur.jpg` |
| e′ | glare off (existing option) | live apply | – | ≤0.5 | ≤0.5 | loses bloom on effects | `gpu/shots/doc/noglare.jpg` |
| f | shadow clear de-duplication | skip a clear of the same 2048-wide shadow view when nothing was drawn since | −0.14 | −0.26 | −0.3 to −0.6 | none (identical by construction) | – |
| g | **texture top-mip skip**, live variant `SetResourceMinLOD(1)` on all 543 asset textures | tracked at `CreateTexture2D` | −0.65 ±1 (noise) | +3.5 (noise) | within noise | LOD 1 hard to see; LOD 2 visibly blurs walls | `gpu/shots/doc/lod1.jpg`, `gpu/shots/doc/lod2.jpg` |
| g′ | texture top-mip skip at creation (`mipskip`) | `CreateTexture2D` drops mip 0, SRV mip ranges clamped | implemented, **not measured** (needs a match reload; g shows sampling cost is not the bottleneck; would save texture memory, which the iGPU shares with the CPU) | | | | |

Ranked by gain ÷ visual cost (for a GPU-bound weak iGPU at 30 FPS):
1. **MSAA off** (existing `msaa=1`): −65 ms at 1080p, 11 → 29 FPS; costs only edge quality, since FXAA stays on. Nothing else comes close.
2. **Potato shadows** (2 cascades, 768/512): −2.4 ms at 720p, −5.3 to −5.6 ms at 1080p (19–20% of the mid frame), and the shadows are still there.
3. **First depth copy only:** −2.8 ms (mid) to −8.7 ms (heavy), with no change visible in the sampled frames. The correctness risk is untested, so it needs per-effect checks before release.
4. **Render scale 0.75:** −8 ms (−33%) at mid at a fixed clock, but the image is softer, and it only works with glare and blur off and set at launch.
5. **Motion blur off** (existing): −1.2 to −1.5 ms; small visual change.
6. **No shadow pass:** −3.7 / −6.6 / −7.0 ms, but characters lose their contact shadows. Potato gets 65–80% of this gain.
7. **Render scale 0.5:** −12 ms (−49%) at mid, but blocky.
8. **Shadow clear de-dup:** −0.1 to −0.6 ms, lossless, nearly free to ship.
9. Glare off (≤0.5 ms) and **texture LOD/mip skip** (no measurable GPU gain): not worth it for GPU time.

**Render scale caveats.**
- It works only when both **glare and motion blur are off**: their shaders turn pixel position into texture coordinates with the full-screen size, so with scaled targets the image comes out as misaligned tiles (`gpu/shots/doc/rscale-broken.jpg`). Keeping the glare RGBA16F targets native did not fix it.
- A live switch goes through the game's settings apply, which recreates only part of the targets. Scaled and native targets then mix, and the image breaks the same way. So render scale is a launch option.
- MSAA + render scale was also broken. The light and heavy render-scale numbers from the live-toggle runs (`L60_rs*`, `H_rs*`) are therefore **not valid** and are not used. Valid runs: `gpu/runs/N100_*`, `N75_*`, `N50_*`, `N100b_*` (mid, blur and glare off, 4 × 12 s per launch; the two native launches agree, 24.65 vs 24.49 ms).
- Clock: at 0.75 and 0.5 the untouched shadow phases took 1.28× and 1.75× longer, because the UHD 770 clocked down at the lower load. The savings above are scaled back to the native clock with that factor. On a GPU that stays saturated, that is the gain to expect; on this desktop at 30 FPS the GPU simply idles more.

**What the prototypes would need for a release** (in the `storm4perf`/story-fork runtime):
- (a) needs nothing: it is the existing `msaa=1`. It deserves to be the first line of any low-end guide, ahead of resolution.
- (c) is safe as is: two engine calls with byte checks at the call sites. Expose a `shadowQuality=-1` (potato) and `-2` (off). Off should skip the passes engine-side rather than through the viewport trick, so that the casters are not submitted at all.
- (d) needs a check of which effect layers really read the R32F copy. A safe version re-copies only when the previous layer wrote depth (track `OMSetDepthStencilState` write masks).
- (b) needs the glare and motion-blur shaders' screen-size constants patched (or those passes run at native size). An FSR1 or bilinear upscale pass in place of the stretch was not built.
- (g) is not worth shipping for GPU time. It may still help 2–4 GB laptops through memory, but that was not measured.

### 4d. Release: GPU options in `storm4perf` 1.2.0 (2026-10-16)

**What shipped.** `storm4perf/xinput9_1_0.dll` 1.2.0 (sha256 `e4cd3944…d33460`), built from `storm4perf.cpp` + `storm4perf_gpu.cpp` + `storm4perf_ui.cpp`. Everything from 4b is unchanged. New:
- **`[gpu] shadows = game | potato | off`** in `storm4perf.ini`, applied engine-side, not through D3D tricks. `ccGameDrawEnv::BeginScreen` (`0x14085BA68`, main thread, every frame) copies the stage's shadow settings into `nuccShadowProjection`. Two of its calls are redirected:
  - `0x14085BC55` → `FUN_14047D960` (writes the enable flag at obj+0x10). **off** writes 0, so `nuccLayerShadow::Draw` skips the whole pass: no caster depth, no RG16F filter, no 2048² clears. The receivers see shadows disabled, exactly as on a stage without shadows.
  - `0x14085BCAD` → `FUN_140A3FF10` (per-cascade blur width at obj+0x550[i]). **potato** caps it at `shadow_blur`, and sets the cascade count (obj+0x588) and tile sizes (`FUN_140A40060`) to `shadow_cascades` / `shadow_size` (default 2 cascades, 768/512, blur 2).
  - Switching back to game restores the values the settings apply uses for the current `shadowQuality` (q0: 3 cascades 1536/768; q2: 4 cascades 2048/1024). The `.rdata` size table is not touched. All five sites are byte-checked.
- **In-game Options menu** (the user asked for the options to be in the game's own settings and saved to a file in the game folder). Options > Shadow Quality offers **OFF, VERY LOW, LOW, MEDIUM, HIGH**. "Settings Complete" saves OFF/VERY LOW to `storm4perf.ini` (`[gpu] shadows=off|potato`) and LOW/MEDIUM/HIGH to `config.ini` as before (`shadows=game`); OFF and VERY LOW also store `shadowQuality=0` in `config.ini`. It applies live. How it works (`storm4perf_ui.cpp`):
  - The screen is Scaleform AS3 `data\ui\flash\OTHER\option\option.gfx`. The game's file opener (vtable slot `0x1410FB430` → `0x1409A7608`) is wrapped: for that one file storm4perf reads the original (must be 89,857 bytes), applies 5 byte-verified same-length patches to `all_nut/updateMain` (method 115: arrow limit 2→4, two `lookupswitch` targets so values 3/4 get both arrows and a label) and returns a memory file. **No game file is changed.** Patch derivation: `gpu/re/gfx/patch_option.py` (AVM2 parser `gpu/re/gfx/avm2.py`).
  - Native side, in the options handler `FUN_1406A9E10`: the row's initial value (`0x1406A7AA3`, now 0–4 from `[gpu] shadows` + `shadowQuality`), the right-arrow limit (`0x1406AC126`, 2→4), Return to Default (`0x1406AAB9E`, → HIGH = 4), and the confirm call `0x1406AC3FC` (→ `FUN_140A44CA0`), where the 0–4 value is split into `[gpu] shadows` + `shadowQuality` before the original call writes `config.ini`.
  - Labels: the message lookup `FUN_140ABD020` is detoured. For `PC_option_004/005/006` it returns the text for the current level: OFF reuses the game's own "OFF" string (`networkmode_057`), and VERY LOW is the only new string.
  - `[gpu] options_menu=0` leaves the screen untouched. If the `.gfx` differs (another mod), the menu keeps 3 levels, the log says why, and the ini still works.
- Screenshots: `gpu/shots/doc/menu_options.jpg` (screen), `gpu/shots/doc/menu_shadow_levels.jpg` (the row at HIGH → OFF, arrows hide at the ends).

**Measured** (UHD 770, mid 1080p msaa off, shadow 2, interleaved live segments, run `gpu/runs/S1_*`; shadow phases = clears + casters + filter):

| `[gpu] shadows` | shadow phases (ms) | saved |
|---|---|---|
| game (HIGH) | 6.43 | – |
| potato (VERY LOW, 2 × 768/512, blur 2) | 1.04 | −5.4 |
| potato, blur 1 | 1.01 | same as blur 2 (blur is not the cost) |
| potato, 1 cascade 768 | 0.52 | −5.9 |
| off | ~0.54 (only the receivers' sampling left) | −5.9; whole frame −6.5 |

Off shows no artifacts: the receivers' shaders take the "no shadow" path, unlike the viewport trick in 4c (c″), which left stale shadows.

**Menu test** (heavy 1080p, pad scripted with 120 ms presses at 30 FPS): one step per press; labels and arrows correct at all five levels; Settings Complete saved potato and off to `storm4perf.ini` (with `config.ini shadowQuality=0`) and applied live; the choice survived reopening the screen and a restart; Return to Default → HIGH; B (cancel) saved nothing. The release config (`live=0`, no `forward=`) logged OK for every install, including `options menu`.

**Looked at, not shipped:**
- `emptylayers` (skip layers with nothing to draw): a no-op. The base `nuccLayer::Draw` is never called for the scene layers, so there was nothing to skip. Removed.
- Shadow-clear de-dup (4c f): −0.1 to −0.6 ms, but off/potato already remove most clears, and it needs D3D-level hooks (per-context vtables that d3d11 rewrites) in the release DLL. Not worth that risk for <1 ms.
- First depth copy only (4c d): still untested per effect (which effect layers read the R32F copy is not known). Needs a "copy only when the previous layer wrote depth" variant and visual checks per effect. Not shipped.
- Render scale (4c b): breaks with glare/motion blur and on live switches. Not shipped.
- The CPU fixes from 4b stay ini-only (on by default, nothing to choose in a menu).

**Debug build → release map** (from the parent session's debug 1.06 findings, mapped by name through nucc-symbols; release VA = 0x140000000 + RVA):

| function | debug 1.06 RVA | release 1.09 RVA | notes |
|---|---|---|---|
| `nuccShadowProjection::Initialize` | 0xbc7540 | 0xa3fd10 | always allocates the 2048×5120 RG16F atlas + 2048² depth |
| `nuccShadowProjection::CreateShadowTexture` | 0xbc7800 | 0xa3f5f0 | clears the full 2048² depth per cascade (0.12–0.2 ms each); the atlas clear is a fast clear (~0.02 ms) |
| `nuccShadowProjection::SetLight` | 0xbc78e0 | 0xa3ffd0 | |
| `nuccShadowProjection::GetDistanceSplitFrustum` | 0xbc7190 | 0xa3f990 | split far distances at obj+0x450.., cascade count obj+0x588 (1–4) |
| `nuccShadowProjection::GetDefaultBlurWidth` | 0xbc7150 | 0xa3f980 | |
| `nuccShadowProjection::UpdateMatrix` | 0xbc6840 | 0xa40160 | |
| `nuccShadowProjection::calcLightViewLiSPSM` | 0xbc7f70 | 0xa40870 | |
| atlas viewport setter (asserts c0 ≤ 2048, c0+c1+c2 ≤ 5120) | 0xbc7b10 | 0xa40060 | used by potato |
| `nuccLayerShadow::Draw` | 0xc0ef60 | 0xa7a820 | skips the pass when obj+0x10 == 0 (measured: shadow phases gone with `off`) |
| `ccGameDrawEnv::BeginScreen` | – | 0x85ba68 | hook sites 0x85bc55 / 0x85bcad |
| `ccGameDrawEnv::SetShadowEnable` | 0x909a1c | 0x8601c4 | per-slot flag at env+(slot+9)*0x60; 24 callers (events, stages) |
| `ccGameDrawEnv::GetShadowParamPtr` | 0x906c24 | 0x85d614 | |
| `ccGameDrawEnv::ResetShadowParam` | 0x90829c | 0x85ea78 | |
| `ccGameDrawEnv::SetShadowDircParam` | 0x9099d4 | 0x86017c | |

The per-slot `SetShadowEnable` flag was not used for "off": events and stages call it and would turn shadows back on. Forcing the value BeginScreen copies into the shadow object every frame wins over all of them.

## 5. Sources (web)

- Steam guide "Solutions to (Almost) Every Storm 4 Problem": https://steamcommunity.com/sharedfiles/filedetails/?id=955102242
- Steam FPS Fix guide (Tormunds, author disowns it): https://steamcommunity.com/sharedfiles/filedetails/?id=705569819 ; https://steamcommunity.com/app/349040/discussions/0/352792037327323008/
- PCGamingWiki: https://www.pcgamingwiki.com/wiki/Naruto_Shippuden:_Ultimate_Ninja_Storm_4
- ProtonDB: https://www.protondb.com/app/349040
- Reddit: https://www.reddit.com/r/NarutoNinjaStorm/comments/jy76jd/ , /mzz1kf/ , /1ax55v7/ , /1gtg3rr/
- Steam threads: https://steamcommunity.com/app/349040/discussions/0/133256240744480950/ , /3053985636036062372/ , /3372656531453043821/ ; Connections (DXVK): https://steamcommunity.com/app/1020790/discussions/0/5715692378861185011/
- DSOGaming 30 FPS lock (2016) and 60 FPS patch (2017): https://www.dsogaming.com/news/naruto-shippuden-ultimate-ninja-storm-4-is-locked-at-30fps/ ; https://www.dsogaming.com/news/naruto-shippuden-ultimate-ninja-storm-4-now-supports-60fps-on-the-pc/
- Nexus 120 FPS mod: https://www.nexusmods.com/narutoultimateninjastorm4/mods/672 ; Storm 4 Evolution: https://www.nexusmods.com/narutoultimateninjastorm4/mods/508
