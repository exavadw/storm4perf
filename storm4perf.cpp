// storm4perf: xinput9_1_0.dll proxy with CPU fixes for Naruto Shippuden: Ultimate Ninja Storm 4 (PC, Steam 1.09,
// NSUNS4.exe sha256 147e52a3...bc257). Background and measurements: storm4-performance.md, sections 1.5, 3b and 4.
//
// Every fix checks the original bytes at each site before it writes anything; a site that differs is skipped and logged.
// All patches are written once in DllMain, before the game's own code runs. Each hook reads its on/off flag on every
// call, so with [debug] live=1 the fixes can be switched while the game runs (used for interleaved A/B tests).
// XInput calls are forwarded to System32\xinput9_1_0.dll, or to the DLL named in [xinput] forward=.
// Log: storm4perf.log next to the exe (falls back to %TEMP%). Config: storm4perf.ini next to the exe (optional).
// GPU options ([gpu] shadows=game|potato|off, also in the game's Options > Shadow Quality): storm4perf_gpu.cpp and
// storm4perf_ui.cpp, section 4c of the doc.
// Replays ([replay], on by default): save and watch Free Battle VS replays from a native list on the Free Battle menu.
// The replay code is C++20 and lives in replay/ (storm4perf_replay.cpp boots it); it only hooks game functions.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#pragma comment(lib, "synchronization.lib")  // WaitOnAddress / WakeByAddressAll
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002  // Windows 10 1803+
#endif

#define S4P_VERSION "1.12.3"

// ---------------------------------------------------------------- logging
static HANDLE g_log = INVALID_HANDLE_VALUE;
static SRWLOCK g_loglock = SRWLOCK_INIT;
static void s4log(const char *fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[1024]; SYSTEMTIME st; GetLocalTime(&st);
    int n = _snprintf_s(buf, sizeof buf, _TRUNCATE, "%02u:%02u:%02u.%03u [%5lu] ", st.wHour, st.wMinute, st.wSecond,
                        st.wMilliseconds, GetCurrentThreadId());
    if (n < 0) n = 0;
    va_list ap; va_start(ap, fmt);
    int m = _vsnprintf_s(buf + n, sizeof buf - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    n += m < 0 ? (int)strlen(buf + n) : m;
    buf[n++] = '\r'; buf[n++] = '\n';
    AcquireSRWLockExclusive(&g_loglock);
    DWORD w; WriteFile(g_log, buf, n, &w, NULL);
    ReleaseSRWLockExclusive(&g_loglock);
}
// first 10 occurrences, then every 1000th
static bool log_limited(volatile LONG *counter) { LONG c = InterlockedIncrement(counter); return c <= 10 || c % 1000 == 0; }
static void hex(char *out, size_t cap, const uint8_t *p, size_t n) {
    out[0] = 0; for (size_t i = 0; i < n && i * 3 + 3 < cap; i++) sprintf_s(out + i * 3, cap - i * 3, "%02X ", p[i]);
}

// ---------------------------------------------------------------- config
struct Flags { volatile LONG gputimer, skinwait, flipwait, timeraffinity, vsyncsleep, bulletunpin, presentpace, inputdelay, anim60fix, guardfix, audiocrash, qpctimer, fps60; };
enum { NFIX = 13 };
static Flags g_on;                   // what the hooks do right now
static int g_live, g_installed_any;
static wchar_t g_dir[MAX_PATH], g_ini[MAX_PATH], g_forward[MAX_PATH];
static const char *const kFixNames[NFIX] = { "gputimer", "skinwait", "flipwait", "timeraffinity", "vsyncsleep", "bulletunpin", "presentpace", "inputdelay", "anim60fix", "guardfix", "audiocrash", "qpctimer", "fps60" };
static const int kFixDefaults[NFIX] = { 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1 };  // bulletunpin: no measurable effect; presentpace: see storm4perf.ini; inputdelay: diagnostic only; anim60fix: measured inert in battle, opt-in; guardfix/audiocrash/qpctimer: game-bug fixes verified in game (see storm4perf.ini); fps60: 60 FPS timing fixes, identical at 30
static volatile LONG *flag_ptr(int i) { return &g_on.gputimer + i; }

static volatile LONG g_pace_margin_us, g_pace_boost;
static void read_fix_flags(LONG out[NFIX]) {
    g_pace_margin_us = (LONG)GetPrivateProfileIntW(L"fixes", L"presentpace_margin_us", 2000, g_ini);
    g_pace_boost = (LONG)GetPrivateProfileIntW(L"fixes", L"presentpace_boost", 1, g_ini);
    for (int i = 0; i < NFIX; i++) {
        wchar_t key[32]; MultiByteToWideChar(CP_ACP, 0, kFixNames[i], -1, key, 32);
        out[i] = GetPrivateProfileIntW(L"fixes", key, kFixDefaults[i], g_ini) ? 1 : 0;
    }
}

// ---------------------------------------------------------------- exe access and patching
static uint8_t *g_exe; static size_t g_exe_size; static HMODULE g_self;
static uint8_t *VA(uint64_t va) { return g_exe + (va - 0x140000000ull); }
static bool in_exe(uint64_t va, size_t n) { return va >= 0x140000000ull && va - 0x140000000ull + n <= g_exe_size; }

static bool check_bytes(const char *fix, uint64_t va, const uint8_t *expect, size_t n) {
    if (!in_exe(va, n)) { s4log("SKIP %s: 0x%llX is outside the exe image (not Storm 4 1.09?)", fix, va); return false; }
    if (memcmp(VA(va), expect, n) == 0) return true;
    char e[100], f[100]; hex(e, sizeof e, expect, n); hex(f, sizeof f, VA(va), n);
    s4log("SKIP %s: bytes at 0x%llX differ. expected %s found %s(other exe version, or another mod patched it)", fix, va, e, f);
    return false;
}
static bool write_bytes(const char *fix, uint8_t *p, const void *src, size_t n) {
    DWORD old;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) {
        s4log("ERROR %s: VirtualProtect(%p) failed, error %lu", fix, p, GetLastError()); return false;
    }
    memcpy(p, src, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}
static void abs_jmp(uint8_t *b, const void *target, bool call) {  // mov rax, imm64 ; jmp/call rax (12 bytes)
    b[0] = 0x48; b[1] = 0xB8; memcpy(b + 2, &target, 8); b[10] = 0xFF; b[11] = call ? 0xD0 : 0xE0;
}
// one executable page within +-2 GB of the exe, for rel32 call sites
static uint8_t *g_stub; static size_t g_stub_used;
static uint8_t *alloc_stub(size_t n) {
    if (!g_stub) {
        for (uint64_t a = ((uint64_t)g_exe - 0x10000) & ~0xFFFFull; a > (uint64_t)g_exe - 0x70000000ull; a -= 0x10000)
            if ((g_stub = (uint8_t *)VirtualAlloc((void *)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))) break;
        if (!g_stub) { s4log("ERROR: no free page within 2 GB of the exe for call stubs"); return NULL; }
    }
    if (g_stub_used + n > 0x1000) return NULL;
    uint8_t *p = g_stub + g_stub_used; g_stub_used = (g_stub_used + n + 15) & ~(size_t)15;
    return p;
}
static bool rel32_to(uint8_t *site, size_t insn_len, const void *target, int32_t *rel) {
    int64_t d = (int64_t)target - (int64_t)(site + insn_len);
    if (d < INT32_MIN || d > INT32_MAX) return false;
    *rel = (int32_t)d; return true;
}
// RIP-relative target of `call qword ptr [rip+rel32]` (FF 15) at va, or 0
static uint8_t *ff15_slot(uint64_t va) {
    if (!in_exe(va, 6)) return NULL;
    uint8_t *p = VA(va); if (p[0] != 0xFF || p[1] != 0x15) return NULL;
    int32_t rel; memcpy(&rel, p + 2, 4); return p + 6 + rel;
}
static void **find_iat(const char *dll, const char *fn) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_exe; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_exe + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(g_exe + d.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char *)(g_exe + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA64 *nameT = (IMAGE_THUNK_DATA64 *)(g_exe + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *addrT = (IMAGE_THUNK_DATA64 *)(g_exe + imp->FirstThunk);
        for (; nameT->u1.AddressOfData; nameT++, addrT++) {
            if (IMAGE_SNAP_BY_ORDINAL64(nameT->u1.Ordinal)) continue;
            if (strcmp(((IMAGE_IMPORT_BY_NAME *)(g_exe + nameT->u1.AddressOfData))->Name, fn) == 0) return (void **)&addrT->u1.Function;
        }
    }
    return NULL;
}

static LARGE_INTEGER g_qpf;
// ticks * 1e6 overflows int64 after ~10.7 days of uptime at a 10 MHz QPC, so split into seconds and remainder
static int64_t qpc_to_us(int64_t t) { int64_t f = g_qpf.QuadPart; return t / f * 1000000 + t % f * 1000000 / f; }
static int64_t qpc_us(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return qpc_to_us(t.QuadPart); }

// ================================================================ fix 1: GPU frame timer without the blocking readback
// The render manager (vtable 0x1412F8F60) brackets each frame with two TIMESTAMP queries and a DISJOINT query:
//   +0x138 FUN_14019C690 begin: Begin(disjoint @+0x180), End(ts0 @+0x188), [+0x198]=1
//   +0x140 FUN_14019C6E0 end:   End(ts1 @+0x190), End(disjoint), then FUN_14019F400 spins on GetData until all three are
//                               ready (a full CPU<->GPU sync every frame) and writes the GPU time in us to *out.
// The value only feeds the load meter (sys+0x464). Replacement: a ring of query sets, read without blocking
// (D3D11_ASYNC_GETDATA_DONOTFLUSH) one or more frames later; *out keeps the newest finished value.
typedef void (*GpuBeginFn)(uint8_t *rm);
typedef void (*GpuEndFn)(uint8_t *rm, uint64_t *out);
static GpuBeginFn orig_gpu_begin; static GpuEndFn orig_gpu_end;
enum { QN = 8 };  // the iGPU can run 3-4 frames behind at max frame latency 3; 8 leaves headroom
struct QSet { ID3D11Query *dj, *t0, *t1; int state; uint32_t frame; };  // state: 0 free, 1 begun, 2 waiting for the GPU
static QSet g_q[QN]; static int g_qw, g_qr, g_qcur; static ID3D11DeviceContext *g_qctx;
static bool g_frame_mine, g_qbroken; static uint32_t g_qframe; static uint64_t g_gpu_us;
static volatile LONG64 st_q_read, st_q_dropped, st_q_disjoint, st_q_lag_sum; static volatile LONG st_q_lag_max, st_q_err;

static void q_release(void) {
    for (int i = 0; i < QN; i++) {
        if (g_q[i].dj) g_q[i].dj->Release();
        if (g_q[i].t0) g_q[i].t0->Release();
        if (g_q[i].t1) g_q[i].t1->Release();
        g_q[i] = QSet{};
    }
    g_qw = g_qr = 0; g_qctx = NULL;
}
static bool q_init(ID3D11DeviceContext *ctx) {
    q_release();
    ID3D11Device *dev = NULL; ctx->GetDevice(&dev);
    if (!dev) { s4log("ERROR gputimer: ID3D11DeviceContext::GetDevice returned NULL"); return false; }
    D3D11_QUERY_DESC dd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, td = { D3D11_QUERY_TIMESTAMP, 0 };
    HRESULT hr = S_OK;
    for (int i = 0; i < QN && SUCCEEDED(hr); i++) {
        hr = dev->CreateQuery(&dd, &g_q[i].dj);
        if (SUCCEEDED(hr)) hr = dev->CreateQuery(&td, &g_q[i].t0);
        if (SUCCEEDED(hr)) hr = dev->CreateQuery(&td, &g_q[i].t1);
    }
    dev->Release();
    if (FAILED(hr)) { s4log("ERROR gputimer: CreateQuery failed, hr=0x%08lX", (unsigned long)hr); q_release(); return false; }
    g_qctx = ctx;
    s4log("gputimer: %d query sets created on context %p", QN, ctx);
    return true;
}
static void q_drain(ID3D11DeviceContext *ctx) {
    while (g_q[g_qr].state == 2) {
        QSet &s = g_q[g_qr];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a = 0, b = 0;
        HRESULT hr = ctx->GetData(s.dj, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) hr = ctx->GetData(s.t0, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) hr = ctx->GetData(s.t1, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) break;  // not finished yet: try again next frame
        if (FAILED(hr)) {
            if (log_limited(&st_q_err)) s4log("ERROR gputimer: GetData failed, hr=0x%08lX (count %ld)", (unsigned long)hr, st_q_err);
        } else if (dj.Disjoint || !dj.Frequency || b < a) {
            InterlockedIncrement64(&st_q_disjoint);
        } else {
            g_gpu_us = (uint64_t)((double)(b - a) * 1e6 / (double)dj.Frequency);
            LONG lag = (LONG)(g_qframe - s.frame);
            InterlockedIncrement64(&st_q_read); InterlockedAdd64(&st_q_lag_sum, lag);
            if (lag > st_q_lag_max) st_q_lag_max = lag;
        }
        s.state = 0; g_qr = (g_qr + 1) % QN;
    }
}
static void hk_gpu_begin(uint8_t *rm) {
    g_frame_mine = false;
    if (g_on.gputimer && !g_qbroken) {
        ID3D11DeviceContext *ctx = *(ID3D11DeviceContext **)(rm + 0x178);
        if (ctx && ctx != g_qctx && !q_init(ctx)) {
            g_qbroken = true; s4log("ERROR gputimer: falling back to the game's own (blocking) timer");
        } else if (ctx) {
            QSet &s = g_q[g_qw];
            if (s.state == 2) { s.state = 0; g_qr = (g_qr + 1) % QN; InterlockedIncrement64(&st_q_dropped); }  // GPU is QN frames behind
            ctx->Begin(s.dj); ctx->End(s.t0);
            s.state = 1; s.frame = g_qframe; g_qcur = g_qw;
            *(int *)(rm + 0x198) = 1;
            g_frame_mine = true;
            return;
        }
    }
    orig_gpu_begin(rm);
}
static void hk_gpu_end(uint8_t *rm, uint64_t *out) {
    if (!g_frame_mine) { orig_gpu_end(rm, out); return; }  // this frame was begun by the game's code: end it the same way
    g_frame_mine = false;
    ID3D11DeviceContext *ctx = g_qctx; QSet &s = g_q[g_qcur];
    ctx->End(s.t1); ctx->End(s.dj);
    s.state = 2; g_qw = (g_qcur + 1) % QN; g_qframe++;
    *(int *)(rm + 0x198) = 0;
    q_drain(ctx);
    *out = g_gpu_us;
    *(int *)(rm + 0x19C) = 0;
}
static bool install_gputimer(void) {
    static const uint8_t begin_e[] = { 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0x89,0x78,0x01,0x00,0x00,0x48,0x8B,0x01,
                                       0x48,0x8B,0x93,0x80,0x01,0x00,0x00,0xFF,0x90,0xD8,0x00,0x00,0x00 };
    static const uint8_t end_e[] = { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8D,0xB1,0x70,0x01,
                                     0x00,0x00,0x48,0x8B,0xFA,0x48,0x8B,0x4E,0x08,0x48,0x8B,0x56,0x20 };
    static const uint8_t wait_e[] = { 0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x40,0x83,0x7A,0x1C,0x00 };  // FUN_14019F400
    if (!check_bytes("gputimer", 0x14019C690, begin_e, sizeof begin_e) || !check_bytes("gputimer", 0x14019C6E0, end_e, sizeof end_e) ||
        !check_bytes("gputimer", 0x14019F400, wait_e, sizeof wait_e) || !in_exe(0x1412F9098, 16)) return false;
    void **vt = (void **)VA(0x1412F9098);
    if (vt[0] != VA(0x14019C690) || vt[1] != VA(0x14019C6E0)) {
        s4log("SKIP gputimer: render-manager vtable +0x138/+0x140 = %p/%p, expected %p/%p", vt[0], vt[1], VA(0x14019C690), VA(0x14019C6E0));
        return false;
    }
    orig_gpu_begin = (GpuBeginFn)vt[0]; orig_gpu_end = (GpuEndFn)vt[1];
    void *repl[2] = { (void *)hk_gpu_begin, (void *)hk_gpu_end };
    return write_bytes("gputimer", (uint8_t *)vt, repl, sizeof repl);
}

// ================================================================ fix 2: nummSkinningThreadGPU::WaitForEnd (0x1409F45D0)
// Original: `mov eax,[rcx+68h]; test eax,eax; jz` back to itself, then clears three fields. Same body, backoff wait.
static void backoff_until_nonzero(volatile int *f) {
    int64_t t0 = 0;
    for (unsigned n = 0; *f == 0; n++) {
        if (n < 64) { _mm_pause(); continue; }
        if (!t0) t0 = qpc_us();
        if (qpc_us() - t0 < 1000) { if (!SwitchToThread()) _mm_pause(); }
        else Sleep(1);
    }
}
static uint64_t skin_wait(uint8_t *o) {
    volatile int *f = (volatile int *)(o + 0x68);
    if (g_on.skinwait) backoff_until_nonzero(f);
    else while (*f == 0) {}
    *(uint64_t *)(o + 0x10) = *(uint64_t *)(o + 8); *(uint64_t *)(o + 0x18) = 0; *(uint64_t *)(o + 0x30) = 0;
    return 0;
}
static bool install_skinwait(void) {
    static const uint8_t e[] = { 0x66,0x90,0x8B,0x41,0x68,0x85,0xC0,0x74,0xF9,0x48,0x8B,0x41,0x08,0x48,0x89,0x41,0x10,0x33,0xC0,
                                 0x48,0x89,0x41,0x18,0x48,0x89,0x41,0x30,0xC3 };
    if (!check_bytes("skinwait", 0x1409F45D0, e, sizeof e)) return false;
    uint8_t b[12]; abs_jmp(b, (void *)skin_wait, false);
    return write_bytes("skinwait", VA(0x1409F45D0), b, sizeof b);
}

// ================================================================ fix 3: WaitVSync -> Flip wait (loop at 0x1409E1C60)
// Original: `do {} while (!mmDrawCommandManager::Flip(mgr))`; Flip fails while mgr+0x88 is set, which the draw-command
// thread clears at 0x14018C8D4 just before LeaveCriticalSection at 0x14018C8E2. That call is redirected to
// leave_and_wake, so the main thread can sleep in WaitOnAddress on the flag instead of spinning. The same site is the end
// of each render job (the frame is fully drawn there): storm4perf_lowlat.cpp's early present hooks in through g_jobend_cb.
typedef char (*FlipFn)(void *);
typedef void (WINAPI *LeaveCsFn)(LPCRITICAL_SECTION);
static FlipFn g_flip; static void **g_flip_mgr; static LeaveCsFn *g_leave_slot; static bool g_wake_ok, g_jobend_ok;
static void (*g_jobend_cb)(void);  // render thread, job done, right after it released the job critical section
static volatile LONG64 st_flip_waits;
static void leave_and_wake(LPCRITICAL_SECTION cs) {
    (*g_leave_slot)(cs);  // through the exe's IAT slot, so other hooks on it still run
    if (g_jobend_cb) g_jobend_cb();
    void *mgr = g_flip_mgr ? *g_flip_mgr : NULL;
    if (mgr) WakeByAddressAll((uint8_t *)mgr + 0x88);
}
// Redirect the render job's LeaveCriticalSection to leave_and_wake, once (flipwait and lowlat share it). The site is the
// same code in the Storm 1-4 builds of the engine: `mov byte [rbx+88h],0; mov rcx,[rbx+90h]; call [LeaveCriticalSection]`
// (1.09: clear at 0x14018C8D4, call at 0x14018C8E2; Storm 1 Steam: 0x14010D694 / 0x14010D6A2).
static bool install_jobend_redirect(uint64_t clear_va, uint64_t call_va) {
    static bool tried; if (tried) return g_jobend_ok; tried = true;
    static const uint8_t w[] = { 0xC6,0x83,0x88,0x00,0x00,0x00,0x00,0x48,0x8B,0x8B,0x90,0x00,0x00,0x00,0xFF,0x15 };
    if (!check_bytes("render job end", clear_va, w, sizeof w) || call_va != clear_va + 14) return false;
    if (ff15_slot(call_va) != (uint8_t *)find_iat("KERNEL32.dll", "LeaveCriticalSection")) { s4log("SKIP render job end: not a LeaveCriticalSection call"); return false; }
    uint8_t *site = VA(call_va), *stub = alloc_stub(12); int32_t rel;
    if (!stub || !rel32_to(site, 5, stub, &rel)) { s4log("SKIP render job end: no call stub in range"); return false; }
    g_leave_slot = (LeaveCsFn *)ff15_slot(call_va);
    uint8_t sb[12]; abs_jmp(sb, (void *)leave_and_wake, false); memcpy(stub, sb, sizeof sb);
    uint8_t b[6] = { 0xE8 }; memcpy(b + 1, &rel, 4); b[5] = 0x90;
    return g_jobend_ok = write_bytes("render job end", site, b, sizeof b);
}
static void wait_flip(void) {
    if (!g_on.flipwait) { while (!g_flip(*g_flip_mgr)) {} return; }
    int64_t t0 = 0;
    for (unsigned n = 0; !g_flip(*g_flip_mgr); n++) {
        if (n < 32) { _mm_pause(); continue; }
        volatile char *flag = (volatile char *)*g_flip_mgr + 0x88; char v = *flag;
        if (!v) continue;
        if (g_wake_ok) { WaitOnAddress((void *)flag, &v, 1, 4); continue; }  // 4 ms timeout only as a safety net
        if (!t0) t0 = qpc_us();
        if (qpc_us() - t0 < 1000) { if (!SwitchToThread()) _mm_pause(); } else Sleep(1);
    }
    InterlockedIncrement64(&st_flip_waits);
}
static bool install_flipwait(void) {
    static const uint8_t e[] = { 0x48,0x8B,0x0D,0x71,0x13,0x35,0x08,0xE8,0x74,0xBB,0x7A,0xFF,0x84,0xC0,0x74,0xF0 };
    static const uint8_t flip_e[] = { 0x40,0x53,0x48,0x83,0xEC,0x20,0x80,0xB9,0x88,0x00,0x00,0x00,0x00 };
    if (!check_bytes("flipwait", 0x1409E1C60, e, sizeof e) || !check_bytes("flipwait", 0x14018D7E0, flip_e, sizeof flip_e)) return false;
    g_flip = (FlipFn)VA(0x14018D7E0); g_flip_mgr = (void **)VA(0x148D32FD8);
    // wake-up site (optional: without it the wait falls back to pause/yield/sleep)
    g_wake_ok = install_jobend_redirect(0x14018C8D4, 0x14018C8E2);
    if (g_wake_ok) s4log("flipwait: render thread wakes the main thread (WaitOnAddress)");
    else s4log("flipwait: no wake-up site; using pause/yield/sleep instead");
    uint8_t b[16]; abs_jmp(b, (void *)wait_flip, true); memset(b + 12, 0x90, 4);
    return write_bytes("flipwait", VA(0x1409E1C60), b, sizeof b);
}

// ================================================================ fix 5: VSyncCallbackThread sleep (call at 0x1409E0656)
// Original: Sleep(elapsed_us / 2000) until 16666 us have passed since the start time in RBX, i.e. Sleep(0) spins for the
// first 2 ms and overshoots by up to ~8 ms later. Replacement sleeps toward the deadline. A stub passes RBX along.
typedef uint64_t (*TimerUsFn)(void);
typedef void (*SleepFn)(DWORD);
static TimerUsFn g_timer_us; static SleepFn g_sleep_thunk;
static void vsync_sleep(DWORD game_ms, uint64_t start_us) {
    if (!g_on.vsyncsleep) { g_sleep_thunk(game_ms); return; }
    int64_t rem = 16666 - (int64_t)(g_timer_us() - start_us);
    g_sleep_thunk(rem > 2000 ? (DWORD)(rem / 1000 - 1) : 1);
}
static bool install_vsyncsleep(void) {
    static const uint8_t e[] = { 0x48,0xD1,0xE9,0xE8,0xB5,0xC0,0xFF,0xFF,0xE8,0x20,0xDB,0x79,0xFF,0x48,0x8B,0xC8,0x48,0x2B,0xCB,
                                 0x48,0x81,0xF9,0x1A,0x41,0x00,0x00 };
    static const uint8_t sleep_e[] = { 0xE9 };  // 0x1409DC710: jmp to the Sleep import thunk
    if (!check_bytes("vsyncsleep", 0x1409E0653, e, sizeof e) || !check_bytes("vsyncsleep", 0x1409DC710, sleep_e, 1)) return false;
    g_timer_us = (TimerUsFn)VA(0x14017E180); g_sleep_thunk = (SleepFn)VA(0x1409DC710);
    uint8_t *site = VA(0x1409E0656), *stub = alloc_stub(15); int32_t rel;
    if (!stub || !rel32_to(site, 5, stub, &rel)) { s4log("SKIP vsyncsleep: no call stub in range"); return false; }
    stub[0] = 0x48; stub[1] = 0x89; stub[2] = 0xDA;  // mov rdx, rbx (period start, us)
    abs_jmp(stub + 3, (void *)vsync_sleep, false);
    uint8_t b[5] = { 0xE8 }; memcpy(b + 1, &rel, 4);
    return write_bytes("vsyncsleep", site, b, sizeof b);
}

// ================================================================ fix 7: vsync-on Present pacing (FUN_14019D900, Sleep(0) at 0x14019DA69)
// With in-game vsync on, the present function calls Present(1) and then spins with Sleep(0) until (interval+1) * 16666 us
// have passed since the previous present. At the Sleep(0) call R15 holds that target and RAX the elapsed time, both in us.
// A stub passes target - elapsed; the hook sleeps on a high-resolution waitable timer to ~0.5 ms before the deadline and
// spins only the rest, so the present time is not pushed later (an overshoot here would lengthen the frame).
static volatile LONG64 st_pace_waits, st_pace_sleeps, st_pace_late; static volatile LONG st_pace_late_max_us;
static void pace_sleep(int64_t remaining_us) {
    if (!g_on.presentpace) { g_sleep_thunk(0); return; }
    InterlockedIncrement64(&st_pace_waits);
    const int64_t margin = g_pace_margin_us;
    static __declspec(thread) HANDLE timer; static __declspec(thread) bool tried;
    if (!tried) {
        tried = true;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) s4log("presentpace: no high-resolution timer (error %lu); using Sleep(1) steps", GetLastError());
    }
    if (remaining_us > margin + 1000) {
        int64_t t0 = qpc_us(); bool slept = false;
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -(remaining_us - margin) * 10;  // relative, 100 ns units
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
                // time-critical only while asleep: when the timer fires the thread gets a core at once instead of
                // queueing behind other ready threads (measured late wake-ups of up to 57 ms under load without it)
                int prio = g_pace_boost ? GetThreadPriority(GetCurrentThread()) : THREAD_PRIORITY_ERROR_RETURN;
                if (prio != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                WaitForSingleObject(timer, 100);
                if (prio != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), prio);
                slept = true;
            }
        }
        if (!slept && remaining_us > margin + 2000) { g_sleep_thunk((DWORD)((remaining_us - margin - 1000) / 1000)); slept = true; }
        if (slept) {
            InterlockedIncrement64(&st_pace_sleeps);
            LONG late = (LONG)(qpc_us() - t0 - remaining_us);  // > 0: woke after the deadline
            if (late > 0) { InterlockedIncrement64(&st_pace_late); if (late > st_pace_late_max_us) st_pace_late_max_us = late; }
            return;
        }
    }
    if (!SwitchToThread()) _mm_pause();  // last stretch: the game loop re-checks the time after every call
}
static bool install_presentpace(void) {
    static const uint8_t e[] = { 0x49,0x2B,0x86,0x30,0x01,0x00,0x00,0x49,0x3B,0xC7,0x73,0x0D,0x33,0xC9,0xFF,0x15,0xDF,0xF6,0xD0,0x00,
                                 0xE9,0xEB,0xFE,0xFF,0xFF };
    static const uint8_t sleep_e[] = { 0xE9 };
    if (!check_bytes("presentpace", 0x14019DA5D, e, sizeof e) || !check_bytes("presentpace", 0x1409DC710, sleep_e, 1)) return false;
    if (ff15_slot(0x14019DA6B) != (uint8_t *)find_iat("KERNEL32.dll", "Sleep")) { s4log("SKIP presentpace: 0x14019DA6B is not a Sleep call"); return false; }
    g_sleep_thunk = (SleepFn)VA(0x1409DC710);
    uint8_t *site = VA(0x14019DA69), *stub = alloc_stub(18); int32_t rel;
    if (!stub || !rel32_to(site, 5, stub, &rel)) { s4log("SKIP presentpace: no call stub in range"); return false; }
    static const uint8_t pre[] = { 0x4C,0x89,0xF9, 0x48,0x29,0xC1 };  // mov rcx, r15 ; sub rcx, rax  (remaining us)
    memcpy(stub, pre, sizeof pre); abs_jmp(stub + sizeof pre, (void *)pace_sleep, false);
    uint8_t b[8] = { 0xE8 }; memcpy(b + 1, &rel, 4); b[5] = b[6] = b[7] = 0x90;  // replaces xor ecx,ecx ; call [Sleep]
    return write_bytes("presentpace", site, b, sizeof b);
}

// ================================================================ fix 8: inputdelay — force frame-delay buffer to 30 (0x1e)
// The frame-delay byte lives at *(uint8_t*)(nuccSys + 0x942). nuccSys is a global pointer at DAT_1416663c8 in 1.09.
// Static analysis shows 1.07 and 1.09 have identical setter code (clamp to 0x3c, default 0x1e), but users report
// higher perceived input delay in 1.08/1.09. This fix continuously forces the value to 30 every frame from the
// background thread, overriding any hidden runtime change introduced in 1.08+. Enable with [fixes] inputdelay=1.
static volatile LONG st_id_samples, st_id_overrides;
static uint8_t *g_nuccsys_ptr;  // address of the global pointer variable (DAT_1416663c8)
static volatile LONG st_id_calls;
static void enforce_input_delay(void) {
    if (!g_on.inputdelay) return;
    LONG call_n = InterlockedIncrement(&st_id_calls);
    // Late-resolve nuccSys: the global pointer is NULL at DllMain time and only set during framework init.
    if (!g_nuccsys_ptr) {
        static const uint64_t nuccsys_va = 0x1416663c8;
        if (!in_exe(nuccsys_va, 8)) { if (call_n <= 3) s4log("inputdelay: call #%ld nuccSys VA outside exe", call_n); return; }
        g_nuccsys_ptr = VA(nuccsys_va);
        if (call_n <= 3) s4log("inputdelay: call #%ld set g_nuccsys_ptr=%p", call_n, g_nuccsys_ptr);
    }
    uintptr_t base = *(volatile uintptr_t *)g_nuccsys_ptr;
    if (base < 0x10000 || base > 0x7FFFFFFFFFFFull) {
        if (call_n <= 5 || call_n % 600 == 0)
            s4log("inputdelay: call #%ld base=0x%llX invalid (nuccSys not yet init)", call_n, (unsigned long long)base);
        return;
    }
    volatile uint8_t *slot = (volatile uint8_t *)(base + 0x942);
    uint8_t val = *slot;
    LONG n = InterlockedIncrement(&st_id_samples);
    if (val != 0x1e) {
        *slot = 0x1e;
        InterlockedIncrement(&st_id_overrides);
        if (n <= 20 || n % 300 == 0)
            s4log("inputdelay: sample #%ld OVERRIDE frame_delay %u -> 30 (base=0x%llX)", n, val, (unsigned long long)base);
    } else {
        if (n <= 5 || n % 300 == 0)
            s4log("inputdelay: sample #%ld frame_delay=%u OK (base=0x%llX)", n, val, (unsigned long long)base);
    }
}
static bool install_inputdelay(void) {
    static const uint64_t nuccsys_va = 0x1416663c8;
    if (!in_exe(nuccsys_va, 8)) { s4log("SKIP inputdelay: nuccSys VA outside exe image"); return false; }
    g_nuccsys_ptr = VA(nuccsys_va);
    uintptr_t p = *(uintptr_t *)g_nuccsys_ptr;
    if (p < 0x10000 || p > 0x7FFFFFFFFFFFull) {
        s4log("inputdelay: nuccSys pointer 0x%llX not yet valid at startup (will retry from bg thread)", (unsigned long long)p);
    } else {
        uint8_t v = *(uint8_t *)(p + 0x942);
        s4log("inputdelay: nuccSys=0x%llX, initial frame_delay=%u (target=30)", (unsigned long long)p, v);
    }
    return true;
}

// ================================================================ fix 9: anim60fix — opt-in, OFF by default since 1.8.0
// Measured 2026-10-17 (Free Battle, full battles at 30 and 60 FPS, patch off): neither site below ever executes in
// battle. DAT_1415da7d0 and its init flag DAT_1415da7d4 stay 0 (the param_2==7 path never runs), and the site B gate
// nuccSys+0x590 stays 0. nuccSys+0x942 holds the fps value (30/60). So these patches do not change battle animation;
// the cause of choppy animation at 60 FPS is still open. Not measured in story mode or cutscenes, so the keys stay.
// Original rationale, kept for reference:
// Bug A: FUN_1403f6820 (param_2==7) uses a global static timer (DAT_1415da7d0) to throttle character
//   state machine updates. At 60fps the threshold doubles but the accumulator races ahead, causing
//   uneven update cadence and dropped keyframes. Debug 1.06 FUN_14044c8c0 has no such throttle.
//   Fix: NOP the JL at 0x1403f68d6 (0F 8C A7 04 00 00) so the update always fires.
// Bug B: FUN_140a0aa00 (nuccSys inner update) calls FUN_140a09830 — an extra animation time advance
//   pass gated on DAT_1416663c8+0x590. This pass runs outside the normal frame-skip cadence and
//   advances animation time a third time per frame at 60fps, skipping keyframes. Debug 1.06 has
//   no equivalent function or call site. Original fix attempt (commit dc374c5) was reverted without
//   explanation; combining with Bug A fix here under the same flag.
//   Fix: NOP the CALL at 0x140a0ad14 (E8 17 EB FF FF). Safe at 30fps (flag doesn't trigger there).
// Both patches are deterministic (no wall-clock or thread-timing dependence), safe for replays.
static bool install_anim60fix(void) {
    // Bug A: NOP the JL throttle in character state machine
    static const uint8_t ea[] = { 0x0F, 0x8C, 0xA7, 0x04, 0x00, 0x00 }; // JL 0x1403f6d83
    if (!check_bytes("anim60fix:A", 0x1403f68d6, ea, sizeof ea)) return false;
    static const uint8_t nop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    if (!write_bytes("anim60fix:A", VA(0x1403f68d6), nop6, sizeof nop6)) return false;
    // Bug B: NOP the extra animation pass call in nuccSys inner update, only with [fixes] anim60fix_extra=1 too.
    if (!GetPrivateProfileIntW(L"fixes", L"anim60fix_extra", 0, g_ini)) { s4log("off  anim60fix_extra (site B; [fixes] anim60fix_extra=0)"); return true; }
    static const uint8_t eb[] = { 0xE8, 0x17, 0xEB, 0xFF, 0xFF }; // CALL FUN_140a09830
    if (!check_bytes("anim60fix:B", 0x140a0ad14, eb, sizeof eb)) return false;
    static const uint8_t nop5[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    return write_bytes("anim60fix:B", VA(0x140a0ad14), nop5, sizeof nop5);
}

// ================================================================ fix 10: audiocrash — prevent crash on audio device unplug
// When an audio device is unplugged, XAudio2's OnCriticalError callback (FUN_140026e3c / FUN_140026e60) sets
// DAT_148a83b28 = 1. The next voice tick in FUN_1400271ac checks that flag and calls vtable+0xb0 on the now-destroyed
// IXAudio2SourceVoice COM object, causing an access violation. NOP'ing the indirect call at 0x1400271CE prevents the
// crash; the subsequent GetState call at vtable+0xc8 is safe because XAudio2 returns XAUDIO2_E_DEVICE_INVALIDATED for
// destroyed voices, and the game's existing error path handles it. Audio does not recover (game has no reinit path),
// but the game stays alive — matching the behaviour of other titles that lose their audio device mid-session.
static volatile LONG64 st_audio_nops;
static bool install_audiocrash(void) {
    // CALL qword ptr [RAX + 0xb0] at RVA 0x1400271CE inside FUN_1400271ac:
    //   FF 90 B0 00 00 00
    static const uint8_t e[] = { 0xFF, 0x90, 0xB0, 0x00, 0x00, 0x00 };
    if (!check_bytes("audiocrash", 0x1400271CE, e, sizeof e)) return false;
    static const uint8_t nop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    if (!write_bytes("audiocrash", VA(0x1400271CE), nop6, sizeof nop6)) return false;
    InterlockedIncrement64(&st_audio_nops);
    return true;
}

// ================================================================ fix 11: qpctimer — a game clock that cannot freeze
// mmTimer::GetMicroSecond (0x14017E180), GetMilliSecond (0x14017E050) and two inlined copies (present pacing loop
// 0x14019D900, timer object ctor 0x1401B6830) are the old OGRE Win32 timer: pin to one CPU, read QPC, compare with
// GetTickCount and correct jumps of more than 100 ms. The correction zero-extends the signed 32-bit offset
// (mov ecx,edx at 0x14017E225), so when QPC trails GetTickCount by more than 100 ms (the thread was delayed between
// the two reads) the "adjustment" becomes the time since the previous read: the clock returns its previous value, the
// gap only grows, and it stays stuck for the rest of the session (the reset runs only at start-up). Waits on it (VSync
// callback thread 0x1409E05C0, present pacing) then never end. storm4-performance.md section 1.7.
// This returns plain QPC time from the game's own base and frequency: no CPU pin, no GetTickCount check, and no shared
// state written, so it is also safe when several threads read the clock at once.
static volatile int32_t *g_tm_ok; static volatile int64_t *g_tm_base, *g_tm_freq; static volatile uint32_t *g_tm_tick0;
static uint64_t qt_elapsed(uint64_t mul) {  // (QPC - base) * mul / freq without overflow
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    uint64_t d = (uint64_t)(t.QuadPart - *g_tm_base), f = (uint64_t)*g_tm_freq;
    return d / f * mul + d % f * mul / f;
}
static uint64_t qt_us(void) { return *g_tm_ok ? qt_elapsed(1000000) : (uint32_t)((GetTickCount() - *g_tm_tick0) * 1000u); }
static uint64_t qt_ms(void) { return *g_tm_ok ? (uint32_t)qt_elapsed(1000) : (uint32_t)(GetTickCount() - *g_tm_tick0); }

static uint8_t *emit(uint8_t *p, const void *b, size_t n) { memcpy(p, b, n); return p + n; }
static uint8_t *emit_mov(uint8_t *p, uint8_t reg, const void *imm) {  // mov rax/rcx, imm64
    p[0] = 0x48; p[1] = (uint8_t)(0xB8 + reg); memcpy(p + 2, &imm, 8); return p + 10;
}
// mov rax, &g_on.qpctimer ; cmp dword [rax], 0 ; je +off  (off = bytes of the "on" path that follow)
static uint8_t *emit_flag_test(uint8_t *p, uint8_t off) {
    p = emit_mov(p, 0, (const void *)&g_on.qpctimer);
    const uint8_t t[] = { 0x83, 0x38, 0x00, 0x74, off }; return emit(p, t, sizeof t);
}
static bool jmp_to_stub(const char *what, uint64_t va, size_t len, uint8_t *stub) {
    uint8_t b[16]; int32_t rel;
    if (!rel32_to(VA(va), 5, stub, &rel)) { s4log("SKIP qpctimer %s: no stub in range", what); return false; }
    b[0] = 0xE9; memcpy(b + 1, &rel, 4); memset(b + 5, 0x90, len - 5);
    return write_bytes("qpctimer", VA(va), b, len);
}
static bool qt_hook_entry(const char *what, uint64_t va, const void *fn) {  // mmTimer::Get*Second entry
    static const uint8_t prologue[] = { 0x40,0x53, 0x55, 0x48,0x83,0xEC,0x28 };  // push rbx ; push rbp ; sub rsp, 28h
    if (!check_bytes("qpctimer", va, prologue, sizeof prologue)) return false;
    uint8_t *stub = alloc_stub(48), *p = stub; if (!stub) return false;
    p = emit_flag_test(p, 12);
    p = emit_mov(p, 0, fn); *p++ = 0xFF; *p++ = 0xE0;          // on:  jmp qt_*: returns straight to the caller
    p = emit(p, prologue, sizeof prologue);                     // off: the game's own timer
    p = emit_mov(p, 0, VA(va + sizeof prologue)); *p++ = 0xFF; *p++ = 0xE0;
    if (!jmp_to_stub(what, va, sizeof prologue, stub)) return false;
    s4log("qpctimer: %s hooked", what); return true;
}
static bool install_qpctimer(void) {
    // the four readers of the timer state (0x148D31B10/B18/AEC); nothing else touches it
    static const uint8_t pace_e[] = { 0x83,0x3D,0x98,0x41,0xB9,0x08,0x00, 0x0F,0x84,0xDD,0x00,0x00,0x00 };  // cmp [ok],0 ; jz
    static const uint8_t pace_next[] = { 0x49,0x2B,0x86,0x30,0x01,0x00,0x00 };     // 0x14019DA5D: sub rax, [r14+130h]
    static const uint8_t ctor_e[] = { 0x48,0x89,0x5C,0x24,0x50 };                  // 0x1401B688B: mov [rsp+50h], rbx
    static const uint8_t ctor_store[] = { 0x41,0x89,0xBE,0xDC,0x00,0x00,0x00, 0x49,0x8B,0xC6 };  // mov [r14+0DCh], edi ; mov rax, r14
    if (!check_bytes("qpctimer", 0x14019D961, pace_e, sizeof pace_e) || !check_bytes("qpctimer", 0x14019DA5D, pace_next, sizeof pace_next) ||
        !check_bytes("qpctimer", 0x1401B688B, ctor_e, sizeof ctor_e) || !check_bytes("qpctimer", 0x1401B6985, ctor_store, sizeof ctor_store) ||
        !in_exe(0x148D31AEC, 0x30)) return false;
    g_tm_ok = (int32_t *)VA(0x148D31B00); g_tm_base = (int64_t *)VA(0x148D31B10);
    g_tm_freq = (int64_t *)VA(0x148D31B08); g_tm_tick0 = (uint32_t *)VA(0x148D31AEC);
    if (!qt_hook_entry("GetMicroSecond 0x14017E180", 0x14017E180, (void *)qt_us) ||
        !qt_hook_entry("GetMilliSecond 0x14017E050", 0x14017E050, (void *)qt_ms)) return false;
    // present pacing loop: the loop head reads the clock inline into RAX, then continues at 0x14019DA5D
    uint8_t *s = alloc_stub(80), *p = s; if (!s) return false;
    p = emit_flag_test(p, 24);
    p = emit_mov(p, 0, (void *)qt_us); *p++ = 0xFF; *p++ = 0xD0;                // call qt_us (RSP is 16-aligned here)
    p = emit_mov(p, 1, VA(0x14019DA5D)); *p++ = 0xFF; *p++ = 0xE1;
    p = emit_mov(p, 0, (void *)g_tm_ok);                                        // off: cmp [ok],0 ; jz/jmp as the game did
    { const uint8_t t[] = { 0x83, 0x38, 0x00, 0x75, 0x0C }; p = emit(p, t, sizeof t); }
    p = emit_mov(p, 0, VA(0x14019DA4B)); *p++ = 0xFF; *p++ = 0xE0;
    p = emit_mov(p, 0, VA(0x14019D96E)); *p++ = 0xFF; *p++ = 0xE0;
    if (!jmp_to_stub("pacing loop", 0x14019D961, sizeof pace_e, s)) return false;
    // timer object ctor: stores the millisecond time at [r14+0DCh] after the "QPC available" check
    s = alloc_stub(64); p = s; if (!s) return false;
    p = emit_flag_test(p, 31);
    p = emit_mov(p, 0, (void *)qt_ms); *p++ = 0xFF; *p++ = 0xD0;                // call qt_ms
    { const uint8_t t[] = { 0x41, 0x89, 0x86, 0xDC, 0x00, 0x00, 0x00 }; p = emit(p, t, sizeof t); }  // mov [r14+0DCh], eax
    p = emit_mov(p, 1, VA(0x1401B698C)); *p++ = 0xFF; *p++ = 0xE1;              // the ctor's epilogue
    p = emit(p, ctor_e, sizeof ctor_e);                                         // off: the game's code
    p = emit_mov(p, 0, VA(0x1401B6890)); *p++ = 0xFF; *p++ = 0xE0;
    if (!jmp_to_stub("timer ctor", 0x1401B688B, sizeof ctor_e, s)) return false;
    s4log("qpctimer: pacing loop 0x14019D961 and timer ctor 0x1401B688B hooked"); return true;
}

// ================================================================ fixes 4 and 6: SetThreadAffinityMask (IAT hook)
// timeraffinity: the QPC timer helpers pin the calling thread to CPU 0 around every read and restore it afterwards.
//   Only those call sites (by return address) on the current-thread pseudo-handle are skipped; QPC itself is unchanged.
// bulletunpin: the Bullet worker pool (0x140E0C590) pins its 4 workers to logical CPUs 0-3; the call is skipped.
typedef DWORD_PTR (WINAPI *SetAffFn)(HANDLE, DWORD_PTR);
static SetAffFn real_setaff; static uintptr_t g_timer_ret[12]; static int g_ntimer_ret; static uintptr_t g_bullet_ret;
static DWORD_PTR g_procmask; static volatile LONG64 st_aff_skipped, st_bullet_skipped;
struct BulletThread { HANDLE h; DWORD_PTR mask; };
static BulletThread g_bullet[8]; static volatile LONG g_nbullet;
static DWORD_PTR WINAPI hk_setaff(HANDLE h, DWORD_PTR m) {
    uintptr_t ra = (uintptr_t)_ReturnAddress();
    if (g_on.timeraffinity && h == GetCurrentThread())
        for (int i = 0; i < g_ntimer_ret; i++)
            if (ra == g_timer_ret[i]) { InterlockedIncrement64(&st_aff_skipped); return g_procmask; }
    if (ra == g_bullet_ret && g_bullet_ret) {
        // remember each worker (own handle + the game's mask) so a live toggle can re-pin or unpin it later
        LONG i = InterlockedIncrement(&g_nbullet) - 1;
        if (i < 8 && DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &g_bullet[i].h, THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, 0))
            g_bullet[i].mask = m;
        s4log("bulletunpin: Bullet worker %lu, game mask 0x%llX, %s", GetThreadId(h), (unsigned long long)m, g_on.bulletunpin ? "left unpinned" : "pinned");
        if (g_on.bulletunpin) { InterlockedIncrement64(&st_bullet_skipped); return g_procmask; }
    }
    return real_setaff(h, m);
}
static void bullet_repin(bool unpin) {
    for (LONG i = 0; i < g_nbullet && i < 8; i++)
        if (g_bullet[i].h && !real_setaff(g_bullet[i].h, unpin ? g_procmask : g_bullet[i].mask))
            s4log("ERROR bulletunpin: SetThreadAffinityMask on worker %lu failed, error %lu", GetThreadId(g_bullet[i].h), GetLastError());
}
static bool install_affinity(bool timer, bool bullet) {
    void **slot = find_iat("KERNEL32.dll", "SetThreadAffinityMask");
    if (!slot) { s4log("SKIP timeraffinity/bulletunpin: SetThreadAffinityMask import not found"); return false; }
    // pin/restore pairs in mmTimer::GetMilliSecond, GetMicroSecond, the count reset, the timer object ctor, and the present pacing loop
    static const uint64_t timer_sites[] = { 0x14017E081, 0x14017E0A2, 0x14017E1B1, 0x14017E1D2, 0x14017DFFA, 0x14017E020,
                                            0x1401B68A8, 0x1401B68C9, 0x14019D981, 0x14019D9A2 };
    if (timer)
        for (uint64_t s : timer_sites) {
            if (ff15_slot(s) == (uint8_t *)slot) g_timer_ret[g_ntimer_ret++] = (uintptr_t)VA(s) + 6;
            else s4log("SKIP timeraffinity site 0x%llX: not a SetThreadAffinityMask call", s);
        }
    if (bullet) {
        if (ff15_slot(0x140E0C8FB) == (uint8_t *)slot) g_bullet_ret = (uintptr_t)VA(0x140E0C8FB) + 6;
        else s4log("SKIP bulletunpin: 0x140E0C8FB is not a SetThreadAffinityMask call");
    }
    if (!g_ntimer_ret && !g_bullet_ret) return false;
    DWORD_PTR sm; GetProcessAffinityMask(GetCurrentProcess(), &g_procmask, &sm);
    real_setaff = (SetAffFn)*slot;
    void *hk = (void *)hk_setaff;
    if (!write_bytes("affinity(iat)", (uint8_t *)slot, &hk, sizeof hk)) { real_setaff = NULL; return false; }
    if (timer) s4log("timeraffinity: %d of %d timer call sites verified", g_ntimer_ret, (int)(sizeof timer_sites / sizeof *timer_sites));
    return true;
}


#include "storm4perf_gpu.cpp"
#include "storm4perf_ui.cpp"
#include "storm4perf_guardfix.cpp"
#include "storm4perf_fps60.cpp"
#include "storm4perf_lowlat.cpp"
extern "C" bool s4p_replay_attach(const wchar_t *dir, const wchar_t *ini);  // replay/storm4perf_replay.cpp

// ================================================================ crash and error logging
static volatile LONG st_exc;
static bool in_range(uintptr_t a, const void *base, size_t n) { return a >= (uintptr_t)base && a < (uintptr_t)base + n; }
static size_t g_self_size;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_ILLEGAL_INSTRUCTION && c != EXCEPTION_PRIV_INSTRUCTION &&
        c != EXCEPTION_STACK_OVERFLOW && c != EXCEPTION_INT_DIVIDE_BY_ZERO && c != EXCEPTION_IN_PAGE_ERROR && c != 0xC0000374)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    bool mine = in_range(a, g_self, g_self_size) || (g_stub && in_range(a, g_stub, 0x1000));
    if (!mine && InterlockedIncrement(&st_exc) > 8) return EXCEPTION_CONTINUE_SEARCH;
    char where[300] = "?";
    if (c != EXCEPTION_STACK_OVERFLOW) {
        HMODULE m = NULL; char name[MAX_PATH];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) &&
            GetModuleFileNameA(m, name, MAX_PATH)) {
            const char *b = strrchr(name, '\\'); b = b ? b + 1 : name;
            if (m == (HMODULE)g_exe) sprintf_s(where, "%s VA 0x%llX", b, (unsigned long long)(a - (uintptr_t)g_exe + 0x140000000ull));
            else sprintf_s(where, "%s+0x%llX", b, (unsigned long long)(a - (uintptr_t)m));
        }
    }
    ULONG_PTR *info = ep->ExceptionRecord->ExceptionInformation;
    s4log("%s exception 0x%08lX at %s (%s), access=%llu addr=0x%llX. If the game crashed, send this log.",
         mine ? "ERROR: storm4perf code raised" : "note: first-chance", c, where, mine ? "inside storm4perf" : "not in storm4perf",
         (unsigned long long)info[0], (unsigned long long)info[1]);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void log_stats(const char *when) {
    LONG64 r = st_q_read;
    s4log("stats (%s): gputimer read=%lld lag_avg=%.2f lag_max=%ld dropped=%lld disjoint=%lld errors=%ld | flip waits=%lld | "
         "timer pins skipped=%lld | bullet pins skipped=%lld | present pacing calls=%lld sleeps=%lld late=%lld late_max=%ldus", when, r,
         r ? (double)st_q_lag_sum / r : 0.0, st_q_lag_max, st_q_dropped, st_q_disjoint, st_q_err, st_flip_waits, st_aff_skipped,
         st_bullet_skipped, st_pace_waits, st_pace_sleeps, st_pace_late, st_pace_late_max_us);
    s4log("stats (%s): shadow frames forced off=%lld", when, st_shadow_frames_off);
    s4log("stats (%s): guardfix stale guard results skipped=%lld; fps60 menu repeated pulses dropped=%lld, late pulses delivered=%lld, bounces dropped=%lld, spring solves rescaled=%lld", when, st_guard_skipped, st_menu_dup, st_menu_late, st_menu_bounce, st_spring_calls);
}

// ---------------------------------------------------------------- XInput forwarding
typedef DWORD (WINAPI *XGetStateFn)(DWORD, void *);
typedef DWORD (WINAPI *XSetStateFn)(DWORD, void *);
typedef DWORD (WINAPI *XGetCapsFn)(DWORD, DWORD, void *);
typedef DWORD (WINAPI *XGetGuidsFn)(DWORD, GUID *, GUID *);
static XGetStateFn x_get; static XSetStateFn x_set; static XGetCapsFn x_caps; static XGetGuidsFn x_guids;
static INIT_ONCE g_xonce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK x_load(PINIT_ONCE, PVOID, PVOID *) {
    wchar_t path[MAX_PATH]; HMODULE h = NULL;
    if (g_forward[0]) {
        if (wcschr(g_forward, L'\\') || wcschr(g_forward, L'/')) wcscpy_s(path, g_forward);
        else swprintf_s(path, L"%s\\%s", g_dir, g_forward);
        h = LoadLibraryW(path);
        if (!h) s4log("ERROR xinput: cannot load forward=%ls (error %lu); using the system XInput", path, GetLastError());
        else if (h == g_self) { s4log("ERROR xinput: forward= points at storm4perf itself; using the system XInput"); FreeLibrary(h); h = NULL; }
    }
    if (!h) {
        GetSystemDirectoryW(path, MAX_PATH); wcscat_s(path, L"\\xinput9_1_0.dll");
        h = LoadLibraryW(path);
        if (!h) { s4log("ERROR xinput: cannot load %ls (error %lu); controllers will not work", path, GetLastError()); return TRUE; }
    }
    x_get = (XGetStateFn)GetProcAddress(h, "XInputGetState"); x_set = (XSetStateFn)GetProcAddress(h, "XInputSetState");
    x_caps = (XGetCapsFn)GetProcAddress(h, "XInputGetCapabilities");
    x_guids = (XGetGuidsFn)GetProcAddress(h, "XInputGetDSoundAudioDeviceGuids");
    s4log("xinput: forwarding to %ls%s", path, x_get && x_set ? "" : " (ERROR: XInputGetState/XInputSetState missing)");
    return TRUE;
}
static void x_ensure(void) { InitOnceExecuteOnce(&g_xonce, x_load, NULL, NULL); }
// Virtual pad: inject START+A press on pad 0 for the first ~10 seconds after load so headless
// test instances advance past the title screen and initialise nuccSys. Controlled by
// [xinput] virtual_pad=1 in storm4perf.ini (read once at startup via g_virtual_pad).
static volatile LONG g_virtual_pad; // 0 or 1
static DWORD g_load_tick;           // GetTickCount() when DLL loaded
static volatile LONG g_getstate_calls; // diagnostic: how many times GetState was called
// Set by the replays (replay/input.cpp): sees every pad read after the system XInput and may change it (virtual pad 0
// presses, buttons reserved for the replay controls). Returns the result to give the game.
extern "C" DWORD (*s4p_pad_filter)(DWORD pad, void *state, DWORD result) = NULL;
extern "C" __declspec(dllexport) DWORD WINAPI XInputGetState(DWORD i, void *s) {
    x_ensure();
    LONG call_n = InterlockedIncrement(&g_getstate_calls);
    if (g_ll_frames_from_xinput) ll_on_pad_read();  // storm4perf_lowlat.cpp: frame timing (and the latent wait) before the read
    DWORD r = g_ll_block ? ll_blocked_pad(i, s) : x_get ? x_get(i, s) : ERROR_DEVICE_NOT_CONNECTED;
    if (call_n <= 3 || call_n % 600 == 0)
        s4log("xinput: GetState call #%ld pad=%u ret=%lu", call_n, i, r);
    if (g_virtual_pad && i == 0 && s && r == ERROR_SUCCESS) {
        DWORD elapsed = GetTickCount() - g_load_tick;
        if (elapsed < 10000) {
            // XINPUT_GAMEPAD layout: wButtons is the first WORD after dwPacketNumber (DWORD)
            uint16_t *buttons = (uint16_t *)((uint8_t *)s + 4);
            *buttons |= 0x0010 | 0x1000; // XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_A
            if (call_n <= 5) s4log("xinput: virtual_pad injected START+A at %lu ms", elapsed);
        }
    }
    if (s4p_pad_filter) r = s4p_pad_filter(i, s, r);
    return r;
}
extern "C" __declspec(dllexport) DWORD WINAPI XInputSetState(DWORD i, void *v) { x_ensure(); return x_set ? x_set(i, v) : ERROR_DEVICE_NOT_CONNECTED; }
extern "C" __declspec(dllexport) DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD f, void *c) { x_ensure(); return x_caps ? x_caps(i, f, c) : ERROR_DEVICE_NOT_CONNECTED; }
extern "C" __declspec(dllexport) DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD i, GUID *r, GUID *c) { x_ensure(); return x_guids ? x_guids(i, r, c) : ERROR_DEVICE_NOT_CONNECTED; }

// ---------------------------------------------------------------- start-up and the background thread
static bool g_hooked[NFIX];
static void apply_flags(const LONG want[NFIX], bool initial) {
    char msg[240] = ""; size_t k = 0;
    for (int i = 0; i < NFIX; i++) {
        LONG v = g_hooked[i] ? want[i] : 0;
        if (!initial && *flag_ptr(i) == v) continue;
        InterlockedExchange(flag_ptr(i), v);
        if (i == 5 && !initial) bullet_repin(v != 0);
        k += sprintf_s(msg + k, sizeof msg - k, " %s=%ld", kFixNames[i], v);
    }
    if (k) s4log("%s:%s", initial ? "active" : "live change", msg);
}
static DWORD WINAPI bg_thread(void *) {
    x_ensure();
    FILETIME last = {}; WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_ini, GetFileExInfoStandard, &fa)) last = fa.ftLastWriteTime;
    DWORD last_stats = GetTickCount();
    for (;;) {
        Sleep(g_live ? 250 : 5000);
        if (g_live && GetFileAttributesExW(g_ini, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &last) != 0) {
            last = fa.ftLastWriteTime; LONG want[NFIX]; read_fix_flags(want); apply_flags(want, false); gpu_live_change();
        }
        enforce_input_delay();
        lowlat_tick();
        if (GetTickCount() - last_stats >= 300000) { last_stats = GetTickCount(); log_stats("5 min"); }
    }
}
static void open_log(void) {
    wchar_t p[MAX_PATH]; swprintf_s(p, L"%s\\storm4perf.log", g_dir);
    g_log = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE) {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t); swprintf_s(p, L"%sstorm4perf.log", t);
        g_log = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
}
static void startup(void) {
    g_exe = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    g_exe_size = nt->OptionalHeader.SizeOfImage;
    { IMAGE_NT_HEADERS64 *sn = (IMAGE_NT_HEADERS64 *)((uint8_t *)g_self + ((IMAGE_DOS_HEADER *)g_self)->e_lfanew); g_self_size = sn->OptionalHeader.SizeOfImage; }
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcscpy_s(g_dir, exe); wchar_t *sl = wcsrchr(g_dir, L'\\'); if (sl) *sl = 0;
    swprintf_s(g_ini, L"%s\\storm4perf.ini", g_dir);
    int log_on = GetPrivateProfileIntW(L"debug", L"log", 1, g_ini);
    if (log_on) open_log();
    QueryPerformanceFrequency(&g_qpf);
    AddVectoredExceptionHandler(0, veh);
    g_live = GetPrivateProfileIntW(L"debug", L"live", 0, g_ini);
    GetPrivateProfileStringW(L"xinput", L"forward", L"", g_forward, MAX_PATH, g_ini);
    g_virtual_pad = GetPrivateProfileIntW(L"xinput", L"virtual_pad", 0, g_ini);
    g_load_tick = GetTickCount();

    s4log("storm4perf %s, exe %ls, image size 0x%zX, PE timestamp 0x%08lX, live=%d", S4P_VERSION, exe, g_exe_size,
         nt->FileHeader.TimeDateStamp, g_live);
    if (g_virtual_pad) s4log("xinput: virtual_pad=1 (START+A press injected for first 10s on pad 0)");
    wchar_t cfg[MAX_PATH]; swprintf_s(cfg, L"%s\\config.ini", g_dir);
    int fps = GetPrivateProfileIntW(L"screenSetting", L"fps", 30, cfg);
    if (fps != 30) s4log("note: config.ini fps=%d. The game logic is built for 30; 60 has known menu/combat timing bugs.", fps);

    LONG want[NFIX]; read_fix_flags(want);
    // Every fix below, the replays and the options menu are for Storm 4 1.09 (some hook fixed addresses); on any other exe
    // (Storm 4 1.07/1.08, Storm 1) only the XInput forwarding and the lowlat module (its own per-exe table) run.
    bool s4_109 = nt->FileHeader.TimeDateStamp == 0x5EF2F684 && g_exe_size == 0x8F1D000;
    if (!s4_109) { s4log("not Storm 4 1.09: Storm 4 fixes, replays and the options menu stay off"); for (int i = 0; i < NFIX; i++) want[i] = 0; }
    bool any_aff = false;
    for (int i = 0; i < NFIX; i++) {
        if (!s4_109) continue;
        if (!want[i] && !g_live) { s4log("off  %s (off in storm4perf.ini, or off by default)", kFixNames[i]); continue; }
        bool ok = false;
        switch (i) {
        case 0: ok = install_gputimer(); break;
        case 1: ok = install_skinwait(); break;
        case 2: ok = install_flipwait(); break;
        case 3: case 5:
            if (!any_aff) {
                any_aff = true;
                bool t = want[3] || g_live, b = want[5] || g_live;
                ok = install_affinity(t, b);
                g_hooked[3] = ok && g_ntimer_ret > 0; g_hooked[5] = ok && g_bullet_ret != 0;
            }
            ok = g_hooked[i];
            break;
        case 4: ok = install_vsyncsleep(); break;
        case 6: ok = install_presentpace(); break;
        case 7: ok = install_inputdelay(); break;
        case 8: ok = install_anim60fix(); break;
        case 9: ok = install_guardfix(); break;
        case 10: ok = install_audiocrash(); break;
        case 11: ok = install_qpctimer(); break;
        case 12: ok = install_fps60(); break;
        }
        g_hooked[i] = ok;
        s4log("%s %s", ok ? "OK  " : "FAIL", kFixNames[i]);
        if (ok) g_installed_any = 1;
    }
    if (s4_109) install_gpu();
    install_lowlat(!s4_109 || g_hooked[0]);  // early present needs the gputimer fix on 1.09 (Storm 1: s1o's gpu_timer, on by default)
    if (!s4_109) {}
    else if (GetPrivateProfileIntW(L"replay", L"enabled", 1, g_ini)) {
        bool rp = s4p_replay_attach(g_dir, g_ini);
        s4log("%s replays (log: StormReplay\\logs\\replay.log)", rp ? "OK  " : "FAIL");
    } else s4log("off  replays ([replay] enabled=0)");
    if (!s4_109) {}
    else if (GetPrivateProfileIntW(L"gpu", L"options_menu", 1, g_ini)) {
        bool ui = install_ui();
        s4log("%s options menu (Shadow Quality: OFF / VERY LOW / LOW / MEDIUM / HIGH)", ui ? "OK  " : "FAIL");
    } else s4log("off  options menu ([gpu] options_menu=0)");
    if (g_stub) { DWORD old; VirtualProtect(g_stub, 0x1000, PAGE_EXECUTE_READ, &old); }
    apply_flags(want, true);
    if (!g_installed_any) s4log("no fix was applied: this does not look like Storm 4 1.09 (XInput still works)");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_self = h;
        startup();
        HANDLE t = CreateThread(NULL, 0, bg_thread, NULL, 0, NULL);
        if (t) CloseHandle(t); else s4log("ERROR: CreateThread failed, error %lu", GetLastError());
    } else if (reason == DLL_PROCESS_DETACH) {
        log_stats(reserved ? "process exit" : "unload");
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    }
    return TRUE;
}
