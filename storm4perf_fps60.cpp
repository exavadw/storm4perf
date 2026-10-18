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
// Config: [fixes] fps60=1, with groups fps60_substitution / fps60_camera / fps60_effects (all 1 by default).

static volatile LONG64 st_fps60_changes;

enum F60Kind { STEP, SMOOTH, DECAYL };
enum F60Group { G_SUB, G_CAM, G_FX, NGROUP };
static const wchar_t *const kF60Group[NGROUP] = { L"fps60_substitution", L"fps60_camera", L"fps60_effects" };

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
    s4log("fps60: %d/%d constants, %d/%d counters, %d/%d statics; groups substitution=%d camera=%d effects=%d",
          nf, NF60, nc, NC60, ns, NS60, g_f60_group_on[G_SUB], g_f60_group_on[G_CAM], g_f60_group_on[G_FX]);
    g_f60_fps = 30;
    HANDLE t = CreateThread(NULL, 0, f60_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else { s4log("ERROR fps60: CreateThread failed, error %lu", GetLastError()); return false; }
    return nf + nc + ns > 0;
}
