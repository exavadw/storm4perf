// storm4perf_fps60.cpp: make 60 FPS play like 30 FPS where the game counts frames instead of time.
//
// The engine's own fps-correct idiom (NSUNS4 Steam 1.09): timers start at -(3000/fps) and advance 3000/fps per frame,
// frame counters compare against N*fps/30, per-frame steps are scaled by 30/fps. The sites below skip that scaling, so
// at 60 FPS they run twice as fast in real time. Each was traced set -> advance -> compare in the 1.09 binary and
// cross-checked against Storm Connections and the Evolution exe (research repo fps60.git, SYNTHESIS.md).
//
// How it is fixed: nothing in the game's data changes. Each float site is an SSE instruction with a RIP-relative
// constant; its displacement is pointed at our own slot (a page within 2 GB of the exe), and the slot holds the
// constant for the current fps. Integer sites get their immediate rewritten. A watcher thread reads the fps byte
// (nuccSys+0x942) and recomputes everything when it changes. At 30 FPS every slot and immediate holds the original
// value, so 30 FPS behaves exactly as without the fix.
//
// Scale s = 30/fps. Per-site rules:
//   STEP   constant added or subtracted once per frame       c' = c * s
//   SMOOTH x += (target - x) * c once per frame            c' = 1 - (1 - c)^s
//   DECAYL x *= (30/fps) * c once per frame (linear-scaled)  c' = c^s / s   (so the product is c^s)
//   COUNT  frame counter compared against N                N' = N / s
//
// Config: [fixes] fps60=1, with groups fps60_substitution / fps60_camera / fps60_effects / fps60_menu / fps60_springs
// (all 1 by default).

static volatile LONG64 st_fps60_changes;

enum F60Kind { STEP, SMOOTH, DECAYL };
enum F60Group { G_SUB, G_CAM, G_FX, G_MENU, G_SPRING, NGROUP };
static const wchar_t *const kF60Group[NGROUP] = { L"fps60_substitution", L"fps60_camera", L"fps60_effects", L"fps60_menu", L"fps60_springs" };

struct F60Float {
    uint64_t va; uint8_t len; uint8_t kind; uint8_t group; float orig; const char *what; uint8_t bytes[9];
};
// Expected bytes are the whole instruction from the 1.09 exe; the displacement is always the last 4 bytes.
static const F60Float kF60Floats[] = {
    // ccPlayer::CtrlCounter (0x1403fe5a0): x *= (30/fps)*0.85 per frame on +0x15950/+0x15954 (exponential fade)
    { 0x1403fe976, 8, DECAYL, G_FX, 0.85f, "CtrlCounter decay", { 0xf3,0x0f,0x59,0x0d,0xba,0xeb,0xb1,0x00 } },
    // ccPlayerMain3TYO::CtrlCounter (0x1406c4360): wing blend +0x15750 ramps up 0.08/frame, down 0.04 or 0.08/frame
    { 0x1406c43bb, 8, STEP, G_FX, 0.08f, "3TYO wing blend up", { 0xf3,0x0f,0x58,0x05,0x65,0x4a,0x83,0x00 } },
    { 0x1406c43e0, 8, STEP, G_FX, 0.04f, "3TYO wing blend down", { 0xf3,0x0f,0x10,0x35,0x8c,0xec,0x8b,0x00 } },
    { 0x1406c43f1, 8, STEP, G_FX, 0.08f, "3TYO wing blend down (fast)", { 0xf3,0x0f,0x10,0x35,0x2f,0x4a,0x83,0x00 } },
    // hell rescue camera (0x140747ad4): +0x1d0/+0x1cc/+0x1d4 approach their targets by 0.12 / 0.2 per frame
    { 0x140747dce, 8, SMOOTH, G_CAM, 0.12f, "rescue cam +0x1d0", { 0xf3,0x0f,0x59,0x0d,0xb6,0x30,0xbb,0x00 } },
    { 0x140747dee, 8, SMOOTH, G_CAM, 0.2f, "rescue cam +0x1cc", { 0xf3,0x0f,0x59,0x05,0x7e,0x16,0x7b,0x00 } },
    { 0x140747e20, 8, SMOOTH, G_CAM, 0.2f, "rescue cam +0x1cc (b)", { 0xf3,0x0f,0x59,0x0d,0x4c,0x16,0x7b,0x00 } },
    { 0x140747e40, 8, SMOOTH, G_CAM, 0.12f, "rescue cam +0x1d4", { 0xf3,0x0f,0x59,0x05,0x44,0x30,0xbb,0x00 } },
    // ccCameraManipulator::Update (0x140854610): +0xc/+0x10 approach their targets by 0.02 per frame
    { 0x1408546ae, 8, SMOOTH, G_CAM, 0.02f, "manipulator +0xc", { 0xf3,0x0f,0x59,0x25,0xb6,0x52,0x6a,0x00 } },
    { 0x1408546b6, 8, SMOOTH, G_CAM, 0.02f, "manipulator +0x10", { 0xf3,0x0f,0x59,0x1d,0xae,0x52,0x6a,0x00 } },
    // RenkeiOugi::CalcCameraPos (0x1407469b8): team ultimate camera follows its target by 0.25 per frame
    { 0x140746ce8, 8, SMOOTH, G_CAM, 0.25f, "team ultimate cam", { 0xf3,0x0f,0x10,0x35,0xa8,0x41,0xbb,0x00 } },
    // ccCmnCameraOperator::InterpView (0x140ac91f0): view blend +0x19c advances 0.002 per frame
    { 0x140ac944f, 8, STEP, G_CAM, 0.002f, "InterpView blend", { 0xf3,0x0f,0x58,0x05,0xb1,0x1d,0x4e,0x00 } },
};
enum { NF60 = sizeof kF60Floats / sizeof kF60Floats[0] };

struct F60Count { uint64_t va; uint8_t len; uint8_t imm_off; uint8_t group; uint8_t orig; const char *what; uint8_t bytes[10]; };
// Frame counters compared against (or started at) a small constant. Rewritten: the immediate's low byte (all fit 0..255).
static const F60Count kF60Counts[] = {
    // ccPlayerLeader::CtrlAct_PL_ACT_DODGE_TO_WARP_UPPER (0x1407cbb28): substitution warp. State 0 sets the global
    // battle speed to 0.4; state 1 does `inc [rdi+0xfcb8]` each frame (when FUN_1407cf38c returns 0) and leaves at 3.
    { 0x1407cbc66, 7, 6, G_SUB, 3, "substitution warp frames", { 0x83,0xbf,0xb8,0xfc,0x00,0x00,0x03 } },
    // ccCameraOperatorDuel::Activated (0x1407421bc) and its reset (0x14074412c) start +0x1ec at 80; Ctrl (0x14074413c)
    // decrements it once per frame and the camera holds while it is > 0.
    { 0x1407422e3, 10, 6, G_CAM, 0x50, "duel cam hold (Activated)", { 0xc7,0x87,0xec,0x01,0x00,0x00,0x50,0x00,0x00,0x00 } },
    { 0x14074412c, 10, 6, G_CAM, 0x50, "duel cam hold (reset)", { 0xc7,0x83,0xec,0x01,0x00,0x00,0x50,0x00,0x00,0x00 } },
};
enum { NC60 = sizeof kF60Counts / sizeof kF60Counts[0] };

// Values the game computes once, on first use, from the fps at that moment, guarded by an init bit. We preset the value
// and set the bit at load (before the game runs), so the game never computes them; float ones follow the fps.
struct F60Static { uint64_t va, guard; uint8_t bit; uint8_t group; int ival; float orig; const char *what; };
static const F60Static kF60Statics[] = {
    // Substitution warp states: the state timer (+0xcd0, the usual -(3000/fps) timer) ends the state when it passes
    // 6000/fps: 200 units at 30 but 100 at 60. Measured in game (COM vs COM, 13-14 windows per run): the common 0.4
    // slowdown (state 0x70 -> 0x75) was 7 frames at both fps (238 ms at 30, 117 ms at 60); with 200 it is 12 frames =
    // 205 ms at 60, and 30 is unchanged (7 frames, 238 ms). The rest of the gap is the 2-frame state-change latency every
    // state has (entry frame + exit frame), which is 66 ms at 30 and 33 ms at 60. Correct value: 200 at every fps.
    { 0x141614e08, 0x141614e00, 4, G_SUB, 200, 0, "warp state 0x70 (FUN_1407cb72c) exit" },
    { 0x141614de8, 0x141614de4, 2, G_SUB, 200, 0, "warp state 0x6d (FUN_1407cbb28) exit" },
    { 0x141614e14, 0x141614e10, 2, G_SUB, 200, 0, "warp state (FUN_1407cba0c) exit" },
    // FUN_1403249a4 / FUN_140324d20: x *= (30/fps)*0.85 per frame on +0x128/+0x12c, the factor cached in a static.
    { 0x1415cf900, 0x1415cf904, 1, G_FX, 0, 0.85f, "event fade (FUN_1403249a4)" },
    { 0x1415cf910, 0x1415cf90c, 2, G_FX, 0, 0.85f, "event fade (FUN_140324d20)" },
};
enum { NS60 = sizeof kF60Statics / sizeof kF60Statics[0] };
static bool g_f60_static_ok[NS60];

static float *g_f60_slot;              // NF60 floats, in our own read-write page near the exe
static bool g_f60_float_ok[NF60], g_f60_count_ok[NC60];
static int g_f60_group_on[NGROUP];
static volatile LONG g_f60_fps;        // fps the slots currently hold (30 = original values)

static float f60_value(const F60Float &f, double s) {
    double c = f.orig;
    switch (f.kind) {
    case STEP: return (float)(c * s);
    case SMOOTH: return (float)(1.0 - pow(1.0 - c, s));
    default: return (float)(pow(c, s) / s);
    }
}
static void f60_apply(int fps) {
    double s = 30.0 / fps;
    for (int i = 0; i < NF60; i++)
        if (g_f60_float_ok[i]) {
            const F60Float &f = kF60Floats[i];
            float v = g_f60_group_on[f.group] ? f60_value(f, s) : f.orig;
            InterlockedExchange((volatile LONG *)&g_f60_slot[i], *(LONG *)&v);
        }
    for (int i = 0; i < NC60; i++)
        if (g_f60_count_ok[i]) {
            const F60Count &c = kF60Counts[i];
            int n = g_f60_group_on[c.group] ? (c.orig * fps + 15) / 30 : c.orig;
            int cap = c.len == 7 ? 127 : 255;  // the cmp takes a sign-extended imm8
            uint8_t b = (uint8_t)(n > cap ? cap : n);
            if (*VA(c.va + c.imm_off) != b) write_bytes("fps60", VA(c.va + c.imm_off), &b, 1);
        }
    for (int i = 0; i < NS60; i++)
        if (g_f60_static_ok[i] && !kF60Statics[i].ival) {  // float statics: x *= (30/fps)*k, value = (30/fps)*k at 30
            const F60Static &st = kF60Statics[i];
            float v = (float)(g_f60_group_on[st.group] ? pow(st.orig, s) : s * st.orig);
            InterlockedExchange((volatile LONG *)VA(st.va), *(LONG *)&v);
        }
    InterlockedExchange(&g_f60_fps, fps);
}
static DWORD WINAPI f60_thread(void *) {
    for (;;) {
        Sleep(250);
        int fps = 30;
        uintptr_t ns = *(volatile uintptr_t *)VA(0x1416663c8);
        if (g_on.fps60 && ns >= 0x10000 && ns <= 0x7FFFFFFFFFFFull) {
            int v = *(volatile uint8_t *)(ns + 0x942);
            if (v >= 10 && v <= 60 && 60 % v == 0) fps = v;
        }
        if (fps != g_f60_fps) {
            s4log("fps60: fps %ld -> %d, %s", g_f60_fps, fps, fps == 30 ? "original values" : "timings scaled to match 30 FPS");
            f60_apply(fps); InterlockedIncrement64(&st_fps60_changes);
        }
    }
}

// ---------------------------------------------------------------- replays
// A replay records which groups shaped its battle (s4p_fps60_mask) and is watched with the same ones (s4p_fps60_force):
// substitution timing and camera smoothing change what happens in the battle, so a 60 FPS replay recorded without fps60
// drifts when watched with it, and the other way round. Only the battle groups are forced (substitution, camera,
// effects); menus and spring bones stay as they are. The statics are preset here too, like at load: with a group off in
// storm4perf.ini the game would compute them itself on first use (6000/fps for the substitution exits) and keep that
// value. Forced off, the substitution exits get the game's own 6000/fps; forced on, fps60's 200.
static int g_f60_ini_group[NGROUP];
static bool g_f60_installed;
static const int kF60BattleGroups = (1 << G_SUB) | (1 << G_CAM) | (1 << G_FX);
extern "C" int s4p_fps60_mask(void) {  // bit g: group g is on and the game runs above 30 FPS with fps60 on
    if (!g_f60_installed || !g_on.fps60) return 0;
    int m = 0;
    for (int g = 0; g < NGROUP; g++) if (g_f60_group_on[g]) m |= 1 << g;
    return m;
}
static void f60_statics(int fps) {  // before f60_apply, which then sets the float ones for the groups
    for (int i = 0; i < NS60; i++) {
        const F60Static &st = kF60Statics[i];
        if (!in_exe(st.va, 4) || !in_exe(st.guard, 4)) continue;
        if (st.ival) *(volatile int32_t *)VA(st.va) = g_f60_group_on[st.group] ? st.ival : 6000 / fps;
        else if (!g_f60_static_ok[i]) *(volatile float *)VA(st.va) = (float)(30.0 / fps * st.orig);
        _InterlockedOr((volatile LONG *)VA(st.guard), st.bit);
        g_f60_static_ok[i] = true;
    }
}
// mask < 0: back to storm4perf.ini. Returns false when fps60 is not running (off in storm4perf.ini, or not installed).
extern "C" bool s4p_fps60_force(int mask) {
    if (!g_f60_installed || !g_on.fps60) return false;
    for (int g = 0; g < NGROUP; g++)
        if (kF60BattleGroups & (1 << g)) g_f60_group_on[g] = mask < 0 ? g_f60_ini_group[g] : (mask >> g) & 1;
    int fps = g_f60_fps > 0 ? (int)g_f60_fps : 30;
    f60_statics(fps);
    f60_apply(fps);
    return true;
}

// ---------------------------------------------------------------- menus (Flash UI pad input)
// The Flash menus read the pad through four ActionScript callbacks (registered at 0x140106a40): GetPadOn / GetPadPress
// / GetPadRepeat / GetPadRelease -> 0x14065ddc4 / ddf4 / de54 / de24, each `pad->vtbl(...) | ui[+0x2d0/2d8/2e0/2dc]`.
// The second term is a copy of the pad (+0x20 on, +0x28 press, +0x2c release, +0x30 repeat) that the task
// ccUiFlashManager::StorePadInput (0x1406706b8) takes once per frame, only when fps > 30. CC2 added it because the
// menus are 30 FPS Flash movies that read the pad only on the game frames where the movie ticks, roughly every other
// frame at 60, so a 1-frame press pulse could fall between two ticks. The copy keeps a press visible for one extra
// frame, which goes wrong both ways (measured on Game Mode Select at 60, never at 30):
//   - the movie ticks on two consecutive frames: the same press is read twice and the cursor moves two entries
//     (3 of 61 taps in one run);
//   - the movie skips both frames (a frame-time hitch): the press is never read (1 of 30 taps, and a missed A).
// Fix (fps > 30 only; at 30 nothing here changes what the game sees): the two getters return storm4perf's own view.
// Every press / repeat pulse the pad reports is latched until the next frame on which a menu reads, and is delivered
// on that frame only (all reads within one frame get the same answer); a latched pulse older than 4 frames is dropped
// so a press made outside any menu cannot leak into the next one. The game's one-frame copy is not used.
// Neutral gate (the "wait for neutral" rule): a new press of a button only counts once that button was released for
// fps60_menu_neutral frames (default 2 = 33 ms at 60), so a d-pad that bounces or a stick that dips out of its
// threshold for a frame does not step twice; the repeat pulse that comes with such a press is dropped too.
// Frames are counted by the StorePadInput task (once per game frame, also at 30). Those two getters are only called
// from the Flash callbacks. Character select does not use them (task charSel_PadCtrl 0x14064aae8 reads its own pad
// copy and pushes SetPadInfo): measured at 60, each tap arrives once and a held direction scrolls as far as at 30.
typedef void (__fastcall *StorePadFn)(uint8_t *);
typedef uint32_t (__fastcall *PadGetFn)(void);
static StorePadFn o_store_pad;
static PadGetFn o_pad_press, o_pad_repeat;
static volatile LONG g_menu_frame;     // task count; reads between task k and task k+1 see k
static int g_menu_neutral = 2, g_menu_log, g_menu_observe;
static volatile LONG64 st_menu_dup, st_menu_late, st_menu_bounce;
enum { MENU_LATCH_FRAMES = 4 };

// Per getter (press = pad +0x28, repeat = pad +0x30). Game thread only: the task and the Flash callbacks both run there.
struct MenuLatch {
    uint32_t off;                // pad field
    uint32_t pending;            // pulses not yet delivered
    LONG pend_frame[32];         // when each pending pulse was sampled
    LONG read_frame;             // read period of `delivered`
    uint32_t delivered, bounced;  // what this read period got; presses the neutral gate dropped in it
};
static MenuLatch g_ml_press = { 0x28 }, g_ml_repeat = { 0x30 };
static LONG g_held_last[32], g_hold_start[32], g_release_len[32];  // pad +0x20 history, sampled by the task
static uint32_t g_was_on;

static uint8_t *menu_pad(void) {
    uintptr_t m = *(volatile uintptr_t *)VA(0x1416bda78);
    if (m < 0x10000 || m > 0x7FFFFFFFFFFFull) return NULL;
    return *(uint8_t **)(m + 0x1a8);
}
static bool menu_active(void) { return g_on.fps60 && g_f60_group_on[G_MENU] && g_f60_fps > 30; }

// End of a frame's UI update: latch this frame's pulses that the menus did not already get (when the menus read
// before this task in the same game frame, they saw them in the pad directly).
static void menu_latch(MenuLatch &m, uint8_t *pad, LONG f) {
    uint32_t p = pad ? *(volatile uint32_t *)(pad + m.off) : 0;
    uint32_t already = m.read_frame == f - 1 ? m.delivered : 0;
    uint32_t add = p & ~already;
    for (int b = 0; b < 32; b++) if (add >> b & 1) m.pend_frame[b] = f;
    m.pending |= add;
    for (int b = 0; b < 32; b++)
        if ((m.pending >> b & 1) && f - m.pend_frame[b] > MENU_LATCH_FRAMES) m.pending &= ~(1u << b);
}
static void __fastcall hk_store_pad(uint8_t *ui) {
    o_store_pad(ui);
    LONG f = InterlockedIncrement(&g_menu_frame);
    uint8_t *pad = menu_pad();
    uint32_t on = pad ? *(volatile uint32_t *)(pad + 0x20) : 0;
    for (int b = 0; b < 32; b++) {
        if (!(on >> b & 1)) continue;
        if (!(g_was_on >> b & 1)) { g_release_len[b] = g_held_last[b] ? f - g_held_last[b] - 1 : 0x7FFF; g_hold_start[b] = f; }
        g_held_last[b] = f;
    }
    g_was_on = on;
    if (g_menu_log >= 2 && pad && *(volatile uint32_t *)(pad + 0x28))
        s4log("fps60 menu pad press frame %ld: 0x%08X (task)", f, *(volatile uint32_t *)(pad + 0x28));
    menu_latch(g_ml_press, pad, f);
    menu_latch(g_ml_repeat, pad, f);
}
// Frames button b was released before its current press (0x7FFF if never held before).
static LONG menu_release_len(int b, LONG f) {
    if (g_hold_start[b] == f && f) return g_release_len[b];  // the task already saw this press
    return g_held_last[b] ? f - g_held_last[b] : 0x7FFF;      // read before the task: off since the last held sample
}

static uint32_t menu_read(MenuLatch &m, uint32_t game, bool press) {
    LONG f = g_menu_frame;
    if (m.read_frame != f) {  // first read of this frame decides for every read of this frame
        uint8_t *pad = menu_pad();
        uint32_t now = pad ? *(volatile uint32_t *)(pad + m.off) : 0;
        uint32_t prev = m.read_frame == f - 1 ? m.delivered : 0;
        uint32_t out = (now & ~prev) | m.pending;   // a pulse delivered last frame is not delivered again
        if (m.pending & ~now) InterlockedIncrement64(&st_menu_late);
        // neutral gate: presses, and repeat pulses that start together with a press (pad press now or latched)
        uint32_t gate = press ? out : out & ((pad ? *(volatile uint32_t *)(pad + 0x28) : 0) | g_ml_press.pending |
                                             (g_ml_press.read_frame == f ? g_ml_press.bounced : 0));
        uint32_t bounced = 0;
        if (g_menu_neutral > 0)
            for (int b = 0; b < 32; b++)
                if ((gate >> b & 1) && menu_release_len(b, f) < g_menu_neutral) bounced |= 1u << b;
        if (press && bounced) InterlockedIncrement64(&st_menu_bounce);
        out &= ~bounced;
        if (game & ~out & prev) InterlockedIncrement64(&st_menu_dup);
        m.pending = 0;
        m.delivered = out; m.bounced = bounced; m.read_frame = f;
        if (g_menu_log && (game | out))
            s4log("fps60 menu %s frame %ld: game 0x%08X -> 0x%08X%s", press ? "press " : "repeat", f, game, out, g_menu_observe ? " (observe only)" : "");
    }
    return g_menu_observe ? game : m.delivered;
}
static uint32_t __fastcall hk_pad_press(void) { uint32_t v = o_pad_press(); return menu_active() ? menu_read(g_ml_press, v, true) : v; }
static uint32_t __fastcall hk_pad_repeat(void) { uint32_t v = o_pad_repeat(); return menu_active() ? menu_read(g_ml_repeat, v, false) : v; }

static bool install_fps60_menu(void) {
    // getter bodies: push rbx; sub rsp,20; mov rax,[pad mgr]; mov rbx,[ui]; mov rcx,[rax+1a8]; mov rax,[rcx]; call [rax+N]; or eax,[rbx+M]
    static const uint8_t press[] = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x05,0x77,0xfc,0x05,0x01,0x48,0x8b,0x1d,0xb8,0xeb,0xfb,0x00,
                                     0x48,0x8b,0x88,0xa8,0x01,0x00,0x00,0x48,0x8b,0x01,0xff,0x50,0x38,0x0b,0x83,0xd8,0x02,0x00,0x00 };
    static const uint8_t repeat[] = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x05,0x17,0xfc,0x05,0x01,0x48,0x8b,0x1d,0x58,0xeb,0xfb,0x00,
                                      0x48,0x8b,0x88,0xa8,0x01,0x00,0x00,0x48,0x8b,0x01,0xff,0x50,0x40,0x0b,0x83,0xe0,0x02,0x00,0x00 };
    // StorePadInput: ... movups xmm0,[rdx+20]; movups [rdi+2d0],xmm0
    static const uint8_t store[] = { 0x0f,0x10,0x42,0x20,0x0f,0x11,0x87,0xd0,0x02,0x00,0x00 };
    if (!check_bytes("fps60 menu", 0x14065ddf4, press, sizeof press) || !check_bytes("fps60 menu", 0x14065de54, repeat, sizeof repeat) ||
        !check_bytes("fps60 menu", 0x1406706ef, store, sizeof store)) return false;
    g_menu_neutral = GetPrivateProfileIntW(L"fixes", L"fps60_menu_neutral", 2, g_ini);
    if (g_menu_neutral < 0) g_menu_neutral = 0; if (g_menu_neutral > 8) g_menu_neutral = 8;
    g_menu_log = GetPrivateProfileIntW(L"fixes", L"fps60_menu_log", 0, g_ini);          // 1: every press/repeat the menus read, 2: + raw pad presses
    g_menu_observe = GetPrivateProfileIntW(L"fixes", L"fps60_menu_observe", 0, g_ini);  // decide and log, but return the game's value
    MH_STATUS r = MH_Initialize();
    if (r != MH_OK && r != MH_ERROR_ALREADY_INITIALIZED) { s4log("FAIL fps60 menu: MH_Initialize %d", (int)r); return false; }
    void *t[3] = { VA(0x1406706b8), VA(0x14065ddf4), VA(0x14065de54) };
    if (MH_CreateHook(t[0], (void *)hk_store_pad, (void **)&o_store_pad) != MH_OK ||
        MH_CreateHook(t[1], (void *)hk_pad_press, (void **)&o_pad_press) != MH_OK ||
        MH_CreateHook(t[2], (void *)hk_pad_repeat, (void **)&o_pad_repeat) != MH_OK ||
        MH_EnableHook(t[0]) != MH_OK || MH_EnableHook(t[1]) != MH_OK || MH_EnableHook(t[2]) != MH_OK) {
        s4log("FAIL fps60 menu: hooks"); return false;
    }
    s4log("fps60 menu: on (neutral gate %d frames, log %d, observe only %d)", g_menu_neutral, g_menu_log, g_menu_observe);
    return true;
}

// ---------------------------------------------------------------- spring bones (hair, cloth, accessories)
// nuccCdSpringGrp::CalcSpring (0x140a9f4e0) solves each spring node with FUN_140aab680 / FUN_140aabd50, once per frame.
// The step the callers pass is (3000/fps)/(3000/fps) = 1.0 at any fps, and the solvers move each node toward its target
// by fixed per-frame fractions read from the node: +0x90 (length correction), +0x94 (follow) and 1 - +0x98 (blend).
// At 60 a node therefore settles in half the real time and lags half as far behind (measured on Kushina's hair:
// +0x90 = 1.0, +0x94 = 0.02, +0x98 = 0.6; same swing period, faster settling). Fix: for the duration of each solver
// call the node holds the fractions for the current fps, c' = 1 - (1 - c)^(30/fps) (so +0x98' = +0x98^(30/fps)),
// then gets its own values back. At 30 nothing is touched.
typedef void (__fastcall *SpringAFn)(uint8_t *, void *, float, int, float);
typedef void (__fastcall *SpringBFn)(uint8_t *, void *, void *, void *, int, float);
static SpringAFn o_spring_a;
static SpringBFn o_spring_b;
static volatile LONG64 st_spring_calls;

static bool spring_active(void) { return g_on.fps60 && g_f60_group_on[G_SPRING] && g_f60_fps > 30; }
static float spring_frac(float c, double s) { return c > 0.0f && c < 1.0f ? (float)(1.0 - pow(1.0 - c, s)) : c; }
struct SpringSave { float k90, k94, k98; };
static void spring_enter(uint8_t *node, SpringSave &sv) {
    double s = 30.0 / g_f60_fps;
    float *k = (float *)(node + 0x90);
    sv.k90 = k[0]; sv.k94 = k[1]; sv.k98 = k[2];
    k[0] = spring_frac(sv.k90, s); k[1] = spring_frac(sv.k94, s);
    if (sv.k98 > 0.0f && sv.k98 < 1.0f) k[2] = (float)pow(sv.k98, s);
}
static void spring_leave(uint8_t *node, const SpringSave &sv) { float *k = (float *)(node + 0x90); k[0] = sv.k90; k[1] = sv.k94; k[2] = sv.k98; }
static void __fastcall hk_spring_a(uint8_t *node, void *a2, float a3, int a4, float a5) {
    if (!spring_active() || !node) { o_spring_a(node, a2, a3, a4, a5); return; }
    SpringSave sv; spring_enter(node, sv); o_spring_a(node, a2, a3, a4, a5); spring_leave(node, sv);
    InterlockedIncrement64(&st_spring_calls);
}
static void __fastcall hk_spring_b(uint8_t *node, void *a2, void *a3, void *a4, int a5, float a6) {
    if (!spring_active() || !node) { o_spring_b(node, a2, a3, a4, a5, a6); return; }
    SpringSave sv; spring_enter(node, sv); o_spring_b(node, a2, a3, a4, a5, a6); spring_leave(node, sv);
    InterlockedIncrement64(&st_spring_calls);
}
static bool install_fps60_springs(void) {
    static const uint8_t pro[] = { 0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18,0x55 };  // both solvers
    // the two call sites in CalcSpring, so another exe never gets hooked
    static const uint8_t ca[] = { 0xe8 }, cb[] = { 0xe8 };
    if (!check_bytes("fps60 springs", 0x140aab680, pro, sizeof pro) || !check_bytes("fps60 springs", 0x140aabd50, pro, sizeof pro) ||
        !check_bytes("fps60 springs", 0x140a9f6aa, ca, 1) || !check_bytes("fps60 springs", 0x140a9f6c6, cb, 1)) return false;
    MH_STATUS r = MH_Initialize();
    if (r != MH_OK && r != MH_ERROR_ALREADY_INITIALIZED) { s4log("FAIL fps60 springs: MH_Initialize %d", (int)r); return false; }
    if (MH_CreateHook(VA(0x140aab680), (void *)hk_spring_a, (void **)&o_spring_a) != MH_OK ||
        MH_CreateHook(VA(0x140aabd50), (void *)hk_spring_b, (void **)&o_spring_b) != MH_OK ||
        MH_EnableHook(VA(0x140aab680)) != MH_OK || MH_EnableHook(VA(0x140aabd50)) != MH_OK) {
        s4log("FAIL fps60 springs: hooks"); return false;
    }
    return true;
}

static bool install_fps60(void) {
    for (int g = 0; g < NGROUP; g++) g_f60_group_on[g] = GetPrivateProfileIntW(L"fixes", kF60Group[g], 1, g_ini) ? 1 : 0;
    // one read-write data page within 2 GB of every site (the call-stub page becomes read-only after start-up)
    for (uint64_t a = ((uint64_t)g_exe - 0x20000) & ~0xFFFFull; !g_f60_slot && a > (uint64_t)g_exe - 0x70000000ull; a -= 0x10000)
        g_f60_slot = (float *)VirtualAlloc((void *)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_f60_slot) { s4log("SKIP fps60: no free page within 2 GB of the exe"); return false; }
    int nf = 0, nc = 0;
    for (int i = 0; i < NF60; i++) {
        const F60Float &f = kF60Floats[i];
        if (!check_bytes("fps60", f.va, f.bytes, f.len)) continue;
        uint8_t *site = VA(f.va); int32_t disp; memcpy(&disp, site + f.len - 4, 4);
        float cur; memcpy(&cur, site + f.len + disp, 4);
        if (cur != f.orig) { s4log("SKIP fps60 %s: constant at 0x%llX is %g, expected %g", f.what, f.va + f.len + disp, cur, f.orig); continue; }
        g_f60_slot[i] = f.orig;
        int32_t rel;
        if (!rel32_to(site, f.len, &g_f60_slot[i], &rel)) { s4log("SKIP fps60 %s: slot out of rel32 range", f.what); continue; }
        uint8_t b[9]; memcpy(b, site, f.len); memcpy(b + f.len - 4, &rel, 4);
        if (!write_bytes("fps60", site, b, f.len)) continue;
        g_f60_float_ok[i] = true; nf++;
    }
    for (int i = 0; i < NC60; i++)
        if (check_bytes("fps60", kF60Counts[i].va, kF60Counts[i].bytes, kF60Counts[i].len)) { g_f60_count_ok[i] = true; nc++; }
    int ns = 0;
    for (int i = 0; i < NS60; i++) {
        const F60Static &st = kF60Statics[i];
        if (!g_f60_group_on[st.group] || !in_exe(st.va, 4) || !in_exe(st.guard, 4)) continue;
        volatile LONG *guard = (volatile LONG *)VA(st.guard);
        if (*guard & st.bit) { s4log("SKIP fps60 %s: already initialised by the game", st.what); continue; }
        if (st.ival) *(volatile int32_t *)VA(st.va) = st.ival; else *(volatile float *)VA(st.va) = st.orig;
        _InterlockedOr(guard, st.bit);
        g_f60_static_ok[i] = true; ns++;
    }
    bool menu = g_f60_group_on[G_MENU] && install_fps60_menu();
    bool springs = g_f60_group_on[G_SPRING] && install_fps60_springs();
    s4log("fps60: %d/%d constants, %d/%d counters, %d/%d statics, menu %s, springs %s; groups substitution=%d camera=%d effects=%d menu=%d springs=%d",
          nf, NF60, nc, NC60, ns, NS60, menu ? "hooked" : "off", springs ? "hooked" : "off", g_f60_group_on[G_SUB], g_f60_group_on[G_CAM],
          g_f60_group_on[G_FX], g_f60_group_on[G_MENU], g_f60_group_on[G_SPRING]);
    g_f60_fps = 30;
    for (int g = 0; g < NGROUP; g++) g_f60_ini_group[g] = g_f60_group_on[g];
    g_f60_installed = true;
    HANDLE t = CreateThread(NULL, 0, f60_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else { s4log("ERROR fps60: CreateThread failed, error %lu", GetLastError()); return false; }
    return nf + nc + ns > 0 || menu || springs;
}
