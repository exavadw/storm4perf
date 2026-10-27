// storm4perf_lowlat.cpp: low-latency frame pacing and an input-to-display measurement for storm4perf, included by
// storm4perf.cpp (one translation unit; it uses s4log, qpc_us, g_ini, g_dir, g_self from there).
//
// Where the latency is (storm4-input-research.md section 5): the game reads the pads right after its frame limiter,
// just before the update, so the read is already as late as the engine allows (Slippi's "polling drift" problem does
// not exist here). What remains is on the display side:
//   - with in-game vsync on, the limiter's integer 33 ms target runs slightly faster than two 60 Hz refreshes (33.33 ms),
//     so frames pile up in front of the display until Present blocks: the oldest queued frame is what you see;
//   - with vsync off, frames are not tied to the refresh at all, so the wait for the next refresh drifts 0-1 refresh.
//
// [lowlat] mode = off | latency1 | latent
//   latency1  the game's DXGI device gets SetMaximumFrameLatency(1) (default 3): at most one frame queued.
//   latent    (needs in-game vsync=0) each frame is started at a fixed point before a refresh, chosen so its Present
//             finishes latent_offset_us before that refresh: the frame start is held back inside the first pad read of
//             the frame (XInputGetState after the limiter), so the read is also as fresh as possible. Frames stay on an
//             exact 2-refresh cadence at 30 FPS. Same idea as Special K's Latent Sync and Slippi's visual-buffer codes.
//
// [lowlat] measure = 1 (test copies only): the module times virtual taps through the whole chain without pressing
// anything in the game. A tap k happens at a random time t; the first frame whose pad read comes after t is the frame
// that would show it. That frame's Present paints a 16x16 marker in the top-left corner (white/black flips once per
// tap), and a Desktop Duplication thread timestamps the first desktop image that shows the flip (DXGI LastPresentTime:
// when DWM took the update, measured ~2 ms after Present at any refresh phase, so not the scanout). The image goes out
// at the next refresh after that (from the WaitForVBlank clock). Per tap: tap->read, read->Present call, Present
// call->return, return->DWM, DWM->refresh, and the total to the start of scanout. Rows go to lowlat.csv next to the
// exe. Excludes the monitor's own processing and where on the screen the marker is.
// [lowlat] ab = mode list ("off,latent@-2000,latent@1000") switches modes every ab_seconds for interleaved A/B runs.
// [lowlat] block_real_input = 1: the copy ignores the real controller and keyboard (pad 0 reads as connected and idle).

#include <dxgi1_2.h>
#include <psapi.h>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")

enum { LL_OFF = 0, LL_LATENCY1 = 1, LL_LATENT = 2 };
static const char *const kLlNames[] = { "off", "latency1", "latent" };
struct LlMode { int mode; int offset_us; };
static LlMode g_ll_ab[8]; static int g_ll_nab, g_ll_ab_ms;
static volatile LONG g_ll_mode, g_ll_offset_us = -2000;     // current mode (the A/B thread may change it)
static int g_ll_measure, g_ll_block, g_ll_fps = 30;
static bool g_ll_vsync_on;

// ---------------------------------------------------------------- refresh clock (IDXGIOutput::WaitForVBlank thread)
static IDXGIOutput1 *g_ll_out;                // output that shows the game window
static volatile LONG64 g_ll_vblank_us;       // last refresh, QPC us
static volatile LONG64 g_ll_period_x1000 = 16666667;  // refresh period in ns (measured)
static DWORD WINAPI ll_vblank_thread(void *) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    int64_t prev = 0;
    for (;;) {
        if (FAILED(g_ll_out->WaitForVBlank())) { Sleep(16); continue; }
        int64_t t = qpc_us();
        if (prev) {
            int64_t d = (t - prev) * 1000;  // ns
            int64_t p = g_ll_period_x1000;
            for (int n = 1; n <= 4; n++)    // tolerate missed wake-ups: fold the delta onto 1-4 periods
                if (llabs(d / n - p) < p / 10) { InterlockedExchange64(&g_ll_period_x1000, p + (d / n - p) / 64); break; }
        }
        prev = t; InterlockedExchange64(&g_ll_vblank_us, t);
    }
}
static int64_t ll_period_us(void) { return g_ll_period_x1000 / 1000; }

// ---------------------------------------------------------------- per-frame records
// Frame f = the f-th pad read of the main thread. The engine does not start frame N+1's update before frame N's
// render work is flipped (WaitVSync), so the first Present that starts after read f presents frame f; checked at run
// time (st_ll_dup_present / st_ll_no_present count the frames where that does not hold).
#define LL_RING 256
struct LlFrame { int64_t read_us, pres0_us, pres1_us; LONG mode; LONG offset; };
static LlFrame g_ll_frames[LL_RING];
static volatile LONG g_ll_frame;             // last frame whose pad read happened
static DWORD g_ll_main_tid; static int64_t g_ll_last_read_us;
static volatile LONG64 st_ll_waits, st_ll_wait_us, st_ll_late, st_ll_dup_present, st_ll_no_present;
static int64_t g_ll_work_us[32]; static int g_ll_work_i;  // read -> Present return of recent frames
static int64_t g_ll_target_us;                // latent: time the current frame's Present should be done by
static IDXGISwapChain *g_ll_sc; static HWND g_ll_hwnd; static UINT g_ll_bw = 960, g_ll_bh = 540;

static int64_t ll_work_estimate(void) {       // max of the last 32 frames: a late frame costs a whole refresh
    int64_t m = 0; for (int i = 0; i < 32; i++) if (g_ll_work_us[i] > m) m = g_ll_work_us[i];
    return m ? m : 4000;
}
static void ll_sleep_until(int64_t t_us) {  // high-resolution timer to ~0.7 ms before, then spin
    static __declspec(thread) HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int64_t now = qpc_us();
    if (t_us - now > 1500 && timer) {
        LARGE_INTEGER due; due.QuadPart = -(t_us - now - 700) * 10;  // 100 ns units, relative
        if (SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0)) WaitForSingleObject(timer, (DWORD)((t_us - now) / 1000 + 50));
    } else if (t_us - now > 1500) Sleep((DWORD)((t_us - now - 1000) / 1000));
    while (qpc_us() < t_us) _mm_pause();
}

// latent: hold the frame start so that start + work estimate lands latent_offset_us before a refresh. Frames keep an
// exact (60 / fps)-refresh cadence; a frame that cannot make its refresh slips to the next one instead of piling up.
static void ll_latent_wait(void) {
    int64_t v = g_ll_vblank_us, per = ll_period_us(); if (!v || per < 4000) return;
    int64_t step = per * (60 / g_ll_fps), work = ll_work_estimate(), now = qpc_us(), off = g_ll_offset_us;
    int64_t target = g_ll_target_us ? g_ll_target_us + step : 0;
    if (!target || now + work > target) {     // first frame or late: the earliest refresh this frame can still make
        if (target) InterlockedIncrement64(&st_ll_late);
        int64_t k = (now + work - off - v + per - 1) / per;
        target = v + k * per + off;
    }
    while (target - now > step + work) target -= step;  // after a long stall, do not wait more than one cadence step
    g_ll_target_us = target;
    int64_t start = target - work;
    if (start > now) { ll_sleep_until(start); InterlockedIncrement64(&st_ll_waits); InterlockedAdd64(&st_ll_wait_us, start - now); }
}

// ---------------------------------------------------------------- virtual taps (measure=1)
struct LlTap { int64_t t_us; LONG frame; LONG mode, offset; };
#define LL_TAPS 4096
static LlTap g_ll_taps[LL_TAPS];
static volatile LONG g_ll_ntaps;              // taps issued
static volatile LONG g_ll_marked;             // taps whose frame has been presented (marker parity = this & 1)
static volatile LONG g_ll_tapped;             // taps that have a frame
static FILE *g_ll_csv;
// The tap is seen by the first pad read at or after its time; called by that read (after any latent wait).
static void ll_assign_tap(LONG f, int64_t read_us) {
    LONG k = g_ll_tapped;
    if (k < g_ll_ntaps && g_ll_taps[k % LL_TAPS].t_us <= read_us) { g_ll_taps[k % LL_TAPS].frame = f; InterlockedExchange(&g_ll_tapped, k + 1); }
}

// called from XInputGetState before the real read
static void ll_on_pad_read(void) {
    DWORD tid = GetCurrentThreadId();
    if (!g_ll_main_tid) { g_ll_main_tid = tid; s4log("lowlat: pad reads come from thread %lu", tid); }
    if (tid != g_ll_main_tid) return;
    int64_t now = qpc_us();
    bool new_frame = now - g_ll_last_read_us > 4000;  // a frame reads all pads back to back; frames are >= 16 ms apart
    g_ll_last_read_us = now;
    if (!new_frame) return;
    if (g_ll_mode == LL_LATENT && !g_ll_vsync_on) { ll_latent_wait(); now = qpc_us(); g_ll_last_read_us = now; }
    LONG f = g_ll_frame + 1;
    LlFrame &r = g_ll_frames[f % LL_RING];
    r.read_us = now; r.pres0_us = r.pres1_us = 0; r.mode = g_ll_mode; r.offset = g_ll_offset_us;
    if (g_ll_sc && !g_ll_frames[(f - 1) % LL_RING].pres0_us) InterlockedIncrement64(&st_ll_no_present);
    ll_assign_tap(f, now);
    InterlockedExchange(&g_ll_frame, f);
}

// ---------------------------------------------------------------- Present hook (render thread)
typedef HRESULT (STDMETHODCALLTYPE *LlPresentFn)(IDXGISwapChain *, UINT, UINT);
static LlPresentFn ll_real_present;
static ID3D11Device *g_ll_dev; static ID3D11DeviceContext *g_ll_ctx; static ID3D11Texture2D *g_ll_mark[2];
static volatile LONG g_ll_latency_set = -1;   // frame latency applied to the game device
static __declspec(thread) int t_ll_in_present;
static LONG g_ll_last_presented = 0;

static void ll_apply_latency(void) {
    LONG want = g_ll_mode == LL_LATENCY1 ? 1 : 3;
    if (!g_ll_dev || g_ll_latency_set == want) return;
    IDXGIDevice1 *d = NULL;
    if (SUCCEEDED(g_ll_dev->QueryInterface(__uuidof(IDXGIDevice1), (void **)&d)) && d) {
        HRESULT hr = d->SetMaximumFrameLatency((UINT)want); d->Release();
        s4log("lowlat: SetMaximumFrameLatency(%ld) -> 0x%08lX", want, (unsigned long)hr);
    }
    g_ll_latency_set = want;
}
static void ll_make_markers(IDXGISwapChain *sc) {
    ID3D11Texture2D *bb = NULL;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)) || !bb) { s4log("lowlat: no back buffer"); return; }
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d); bb->Release();
    s4log("lowlat: back buffer %ux%u format %u samples %u", d.Width, d.Height, d.Format, d.SampleDesc.Count);
    if (d.SampleDesc.Count != 1) { s4log("lowlat: multisampled back buffer, no marker"); return; }
    D3D11_TEXTURE2D_DESC m = d; m.Width = m.Height = 16; m.MipLevels = m.ArraySize = 1; m.Usage = D3D11_USAGE_IMMUTABLE;
    m.BindFlags = D3D11_BIND_SHADER_RESOURCE; m.CPUAccessFlags = 0; m.MiscFlags = 0;
    static uint32_t px[2][256];
    for (int i = 0; i < 256; i++) { px[0][i] = 0xFF000000u; px[1][i] = 0xFFFFFFFFu; }  // 8-bit RGBA/BGRA: black, white
    for (int c = 0; c < 2; c++) {
        D3D11_SUBRESOURCE_DATA s = { px[c], 16 * 4, 0 };
        if (FAILED(g_ll_dev->CreateTexture2D(&m, &s, &g_ll_mark[c]))) { s4log("lowlat: marker texture failed (format %u)", d.Format); return; }
    }
}
static HRESULT STDMETHODCALLTYPE ll_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (t_ll_in_present || (g_ll_sc && sc != g_ll_sc)) return ll_real_present(sc, sync, flags);
    t_ll_in_present = 1;
    if (!g_ll_sc) {
        DXGI_SWAP_CHAIN_DESC sd; sc->GetDesc(&sd); g_ll_bw = sd.BufferDesc.Width; g_ll_bh = sd.BufferDesc.Height;
        s4log("lowlat: game swapchain %p, window %p, %ux%u windowed=%d buffers=%u swap effect %d, Present(sync=%u)", (void *)sc,
              (void *)g_ll_hwnd, sd.BufferDesc.Width, sd.BufferDesc.Height, sd.Windowed, sd.BufferCount, (int)sd.SwapEffect, sync);
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&g_ll_dev)) && g_ll_dev) {
            g_ll_dev->GetImmediateContext(&g_ll_ctx);
            if (g_ll_measure) ll_make_markers(sc);
        }
        g_ll_sc = sc; g_ll_hwnd = sd.OutputWindow;
    }
    ll_apply_latency();
    LONG f = g_ll_frame; int64_t t0 = qpc_us();
    LlFrame &r = g_ll_frames[f % LL_RING];
    bool first = f != g_ll_last_presented;
    if (!first) InterlockedIncrement64(&st_ll_dup_present);
    if (first) r.pres0_us = t0;
    if (first && g_ll_measure) {  // taps whose frame is f or earlier are on screen from this frame on
        LONG n = g_ll_ntaps, m = g_ll_marked;
        while (m < n && g_ll_taps[m % LL_TAPS].frame && g_ll_taps[m % LL_TAPS].frame <= f) m++;
        g_ll_marked = m;
    }
    if (g_ll_measure && g_ll_mark[0]) {
        ID3D11Resource *bb = NULL;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Resource), (void **)&bb)) && bb) {
            g_ll_ctx->CopySubresourceRegion(bb, 0, 0, 0, 0, g_ll_mark[g_ll_marked & 1], 0, NULL); bb->Release();
        }
    }
    HRESULT hr = ll_real_present(sc, sync, flags);
    if (first) {
        int64_t t1 = qpc_us(); r.pres1_us = t1; g_ll_last_presented = f;
        g_ll_work_us[g_ll_work_i++ & 31] = t1 - r.read_us;
    }
    t_ll_in_present = 0;
    return hr;
}
// Present is IDXGISwapChain vtable slot 8, shared by every swapchain of dxgi.dll's class: patch it through a throwaway
// WARP swapchain. Done a few seconds after the game started reading pads (its swapchain exists by then): an overlay that
// re-hooks the vtable when the game's swapchain is created would otherwise call back into a hook installed before it.
static bool ll_hook_present(void) {
    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "s4p_lowlat";
    RegisterClassA(&wc);
    HWND hw = CreateWindowA("s4p_lowlat", "", WS_OVERLAPPED, 0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC sd = {}; sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hw; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    IDXGISwapChain *sc = NULL; ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; bool ok = false;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx))) {
        void **vt = *(void ***)sc; DWORD old;
        if (VirtualProtect(&vt[8], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
            ll_real_present = (LlPresentFn)vt[8]; vt[8] = (void *)ll_present; VirtualProtect(&vt[8], sizeof(void *), old, &old); ok = true;
        }
        sc->Release(); dev->Release(); ctx->Release();
    }
    DestroyWindow(hw);
    s4log("%s lowlat: Present hook", ok ? "OK  " : "FAIL");
    return ok;
}

// ---------------------------------------------------------------- display side: output, Desktop Duplication
static IDXGIAdapter1 *g_ll_adapter; static RECT g_ll_out_rect;
// physical pixels: the game is not DPI aware, so threads that map its window onto the desktop image opt in themselves
static void ll_thread_dpi_aware(void) {
    typedef DPI_AWARENESS_CONTEXT (WINAPI *SetCtxFn)(DPI_AWARENESS_CONTEXT);
    SetCtxFn set_ctx = (SetCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    if (set_ctx) set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}
static bool ll_find_output(void) {  // the output whose desktop area holds the game window's centre
    RECT wr; if (!GetWindowRect(g_ll_hwnd, &wr)) return false;
    POINT c = { (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };
    IDXGIFactory1 *fac = NULL; if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac))) return false;
    IDXGIAdapter1 *a = NULL;
    for (UINT i = 0; !g_ll_out && fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        IDXGIOutput *o = NULL;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC od; o->GetDesc(&od);
            if (PtInRect(&od.DesktopCoordinates, c) && SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput1), (void **)&g_ll_out))) {
                g_ll_adapter = a; a->AddRef(); g_ll_out_rect = od.DesktopCoordinates;
                DXGI_ADAPTER_DESC1 ad; a->GetDesc1(&ad);
                s4log("lowlat: window on output %ls (%ld,%ld)-(%ld,%ld), adapter %ls", od.DeviceName, od.DesktopCoordinates.left,
                      od.DesktopCoordinates.top, od.DesktopCoordinates.right, od.DesktopCoordinates.bottom, ad.Description);
                o->Release(); break;
            }
            o->Release();
        }
        a->Release();
    }
    fac->Release();
    return g_ll_out != NULL;
}

static volatile LONG64 st_ll_occluded, st_ll_dup_frames, st_ll_dup_lost;
static void ll_write_tap(LONG k, int64_t shown_us);
static DWORD WINAPI ll_capture_thread(void *) {
    ll_thread_dpi_aware();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    if (FAILED(D3D11CreateDevice(g_ll_adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
        s4log("ERROR lowlat: capture device failed"); return 0;
    }
    D3D11_TEXTURE2D_DESC sd = {}; sd.Width = sd.Height = 1; sd.MipLevels = sd.ArraySize = 1; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stage = NULL; dev->CreateTexture2D(&sd, NULL, &stage);
    IDXGIOutputDuplication *dup = NULL; int parity = -1; LONG shown = -1;
    for (;;) {
        if (!dup) {
            HRESULT hr = g_ll_out->DuplicateOutput(dev, &dup);
            if (FAILED(hr)) { s4log("lowlat: DuplicateOutput -> 0x%08lX, retrying", (unsigned long)hr); Sleep(1000); continue; }
            s4log("lowlat: desktop duplication on");
        }
        DXGI_OUTDUPL_FRAME_INFO fi; IDXGIResource *res = NULL;
        HRESULT hr = dup->AcquireNextFrame(100, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) { dup->Release(); dup = NULL; InterlockedIncrement64(&st_ll_dup_lost); Sleep(100); continue; }
        if (fi.LastPresentTime.QuadPart) {
            InterlockedIncrement64(&st_ll_dup_frames);
            // sample the marker's centre pixel (backbuffer (8,8)) where the window shows on this output
            RECT cr; POINT p = { 0, 0 }; GetClientRect(g_ll_hwnd, &cr); ClientToScreen(g_ll_hwnd, &p);
            int sx = p.x + (int)(8.0 * cr.right / g_ll_bw + 0.5), sy = p.y + (int)(8.0 * cr.bottom / g_ll_bh + 0.5);
            POINT q = { sx, sy }; HWND top = WindowFromPoint(q), root = GetAncestor(g_ll_hwnd, GA_ROOT);
            bool occ = top && GetAncestor(top, GA_ROOT) != root;
            if (occ) InterlockedIncrement64(&st_ll_occluded);
            static int diag; bool log_px = diag < 4 || (occ && diag < 8);
            if (log_px) { diag++; s4log("lowlat: sample at (%d,%d), client %ldx%ld, window at point %p (root %p, game %p root %p)%s", sx, sy, cr.right, cr.bottom,
                                         (void *)top, top ? (void *)GetAncestor(top, GA_ROOT) : NULL, (void *)g_ll_hwnd, (void *)root, occ ? " OCCLUDED" : ""); }
            ID3D11Texture2D *tex = NULL;
            bool inside = sx >= g_ll_out_rect.left && sy >= g_ll_out_rect.top && sx < g_ll_out_rect.right && sy < g_ll_out_rect.bottom;
            if (inside && !occ && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) && tex) {
                D3D11_BOX b = { (UINT)(sx - g_ll_out_rect.left), (UINT)(sy - g_ll_out_rect.top), 0, 0, 0, 1 };
                b.right = b.left + 1; b.bottom = b.top + 1;
                ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, tex, 0, &b); tex->Release();
                D3D11_MAPPED_SUBRESOURCE mp;
                if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mp))) {
                    uint32_t px = *(uint32_t *)mp.pData; ctx->Unmap(stage, 0);
                    int g = (px >> 8) & 0xFF, now = g > 128 ? 1 : 0;
                    if (log_px) s4log("lowlat: marker pixel 0x%08X", px);
                    LONG m = g_ll_marked;  // the flip shows tap m-1 (taps are >= 400 ms apart, so no later one is marked yet)
                    if (parity < 0) parity = now;
                    else if (now != parity) {
                        parity = now;
                        int64_t t = qpc_to_us(fi.LastPresentTime.QuadPart);
                        if (m > 0 && (m & 1) == now && m - 1 != shown) { shown = m - 1; ll_write_tap(shown, t); }
                        else InterlockedIncrement64(&st_ll_dup_lost);
                    }
                }
            }
        }
        res->Release(); dup->ReleaseFrame();
    }
}

// ---------------------------------------------------------------- measurement thread: taps, A/B switching, report
struct LlAcc { int n; double sum[6]; int64_t mn, mx; };
static LlAcc g_ll_acc[8]; static int g_ll_cur_ab = -1;
static void ll_write_tap(LONG k, int64_t shown_us) {
    LlTap &t = g_ll_taps[k % LL_TAPS]; LlFrame &r = g_ll_frames[t.frame % LL_RING];
    if (!r.pres1_us || r.read_us < t.t_us) return;  // ring entry overwritten or inconsistent
    int64_t v = g_ll_vblank_us, per = ll_period_us(), scan = shown_us;
    if (v && per > 4000) { int64_t d = shown_us - v; int64_t k = d >= 0 ? d / per + 1 : -((-d) / per); scan = v + k * per; }
    int64_t c[6] = { r.read_us - t.t_us, r.pres0_us - r.read_us, r.pres1_us - r.pres0_us, shown_us - r.pres1_us, scan - shown_us, scan - t.t_us };
    if (c[3] < 0 || c[5] > 500000) return;
    char name[32]; sprintf_s(name, "%s@%ld", kLlNames[t.mode], t.offset);
    if (g_ll_csv) { fprintf(g_ll_csv, "%ld,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n", k, name, c[0] / 1000.0, c[1] / 1000.0, c[2] / 1000.0, c[3] / 1000.0, c[4] / 1000.0, c[5] / 1000.0); fflush(g_ll_csv); }
    int a = 0; for (int i = 0; i < g_ll_nab; i++) if (g_ll_ab[i].mode == t.mode && g_ll_ab[i].offset_us == t.offset) a = i;
    LlAcc &s = g_ll_acc[a]; if (!s.n) { s.mn = c[5]; s.mx = c[5]; }
    s.n++; for (int i = 0; i < 6; i++) s.sum[i] += c[i]; if (c[5] < s.mn) s.mn = c[5]; if (c[5] > s.mx) s.mx = c[5];
}
static void ll_report(const char *when) {
    for (int i = 0; i < g_ll_nab; i++) {
        LlAcc &s = g_ll_acc[i]; if (!s.n) continue;
        s4log("lowlat %s: %s@%d n=%d tap->scanout avg %.1f ms (min %.1f max %.1f) = tap->read %.1f + read->Present %.1f + in Present %.1f + Present->DWM %.1f + DWM->refresh %.1f",
              when, kLlNames[g_ll_ab[i].mode], g_ll_ab[i].offset_us, s.n, s.sum[5] / s.n / 1000, s.mn / 1000.0, s.mx / 1000.0,
              s.sum[0] / s.n / 1000, s.sum[1] / s.n / 1000, s.sum[2] / s.n / 1000, s.sum[3] / s.n / 1000, s.sum[4] / s.n / 1000);
    }
    s4log("lowlat %s: waits=%lld (avg %.2f ms) late=%lld dup_present=%lld no_present=%lld | capture frames=%lld lost=%lld occluded=%lld | refresh %.3f ms, work est %.2f ms",
          when, st_ll_waits, st_ll_waits ? st_ll_wait_us / 1000.0 / st_ll_waits : 0.0, st_ll_late, st_ll_dup_present, st_ll_no_present,
          st_ll_dup_frames, st_ll_dup_lost, st_ll_occluded, g_ll_period_x1000 / 1e6, ll_work_estimate() / 1000.0);
}
// measure=1: the duplicated desktop must show the marker, so the copy's window goes into the topmost band without being
// activated (HWND_TOP cannot lift a background window above the active one), optionally moved to [lowlat]
// window_x/window_y (physical pixels). It never takes focus or input; if it is covered anyway, the next A/B segment
// raises it again and the covered samples are dropped.
static int g_ll_win_x = -1, g_ll_win_y = -1;
static void ll_raise_window(const char *why) {
    HWND root = GetAncestor(g_ll_hwnd, GA_ROOT);
    UINT fl = SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOOWNERZORDER | (g_ll_win_x < 0 ? SWP_NOMOVE : 0);
    BOOL ok = SetWindowPos(root, HWND_TOPMOST, g_ll_win_x, g_ll_win_y, 0, 0, fl);
    RECT r = {}; GetWindowRect(root, &r);
    s4log("lowlat: window raised (%s) -> %d, now (%ld,%ld)-(%ld,%ld)", why, ok, r.left, r.top, r.right, r.bottom);
}
static void ll_set_ab(int i) {
    g_ll_cur_ab = i; InterlockedExchange(&g_ll_offset_us, g_ll_ab[i].offset_us); InterlockedExchange(&g_ll_mode, g_ll_ab[i].mode);
    g_ll_target_us = 0;
    s4log("lowlat: mode %s@%d", kLlNames[g_ll_ab[i].mode], g_ll_ab[i].offset_us);
}
static DWORD WINAPI ll_thread(void *) {
    ll_thread_dpi_aware();
    while (g_ll_frame < 30) Sleep(250);       // the game's main loop (and swapchain) is running
    Sleep(4000);
    if (!ll_hook_present()) return 0;
    while (!g_ll_hwnd) Sleep(100);
    if (!ll_find_output()) { s4log("ERROR lowlat: no output for the game window; latent and measure are off"); InterlockedExchange(&g_ll_mode, LL_OFF); return 0; }
    HANDLE t = CreateThread(NULL, 0, ll_vblank_thread, NULL, 0, NULL); if (t) CloseHandle(t);
    if (!g_ll_measure) return 0;
    wchar_t p[MAX_PATH]; swprintf_s(p, L"%s\\lowlat.csv", g_dir);
    if (_wfopen_s(&g_ll_csv, p, L"w") == 0 && g_ll_csv) fprintf(g_ll_csv, "tap,mode,tap_to_read_ms,read_to_present_ms,in_present_ms,present_to_dwm_ms,dwm_to_refresh_ms,total_ms\n");
    ll_raise_window("start");
    t = CreateThread(NULL, 0, ll_capture_thread, NULL, 0, NULL); if (t) CloseHandle(t);
    Sleep(2000);
    srand(GetTickCount());
    int64_t seg_end = 0, last_report = qpc_us(); LONG64 occ_seen = 0;
    if (g_ll_nab) ll_set_ab(0);
    for (;;) {
        // one tap at a time: random gap 400-466 ms (two frames' worth of phase spread), next only once it is on screen
        int64_t now = qpc_us();
        if (g_ll_nab > 1 && now >= seg_end) {
            if (seg_end) ll_set_ab((g_ll_cur_ab + 1) % g_ll_nab);
            if (st_ll_occluded != occ_seen) { occ_seen = st_ll_occluded; ll_raise_window("covered"); }
            seg_end = now + g_ll_ab_ms * 1000LL; Sleep(1000);
        }
        ll_sleep_until(qpc_us() + 400000 + rand() % 66667);
        LONG k = g_ll_ntaps;
        if (k > 0 && g_ll_marked < k) { Sleep(50); continue; }  // previous tap not presented yet
        LlTap &tp = g_ll_taps[k % LL_TAPS]; tp.frame = 0; tp.mode = g_ll_mode; tp.offset = g_ll_offset_us;
        tp.t_us = qpc_us();
        InterlockedExchange(&g_ll_ntaps, k + 1);  // the next pad read takes it (ll_assign_tap)
        if (now - last_report > 30000000) { last_report = now; ll_report("30 s"); }
    }
}

// ---------------------------------------------------------------- real input blocking for automated copies
static BOOL WINAPI ll_RegisterRawInputDevices(PCRAWINPUTDEVICE, UINT, UINT) { return TRUE; }
static UINT WINAPI ll_GetRawInputData(HRAWINPUT, UINT, LPVOID, PUINT pcb, UINT) { if (pcb) *pcb = 0; return (UINT)-1; }
static UINT WINAPI ll_GetRawInputBuffer(PRAWINPUT, PUINT pcb, UINT) { if (pcb) *pcb = 0; return 0; }
static SHORT WINAPI ll_GetKeyState(int) { return 0; }
static SHORT WINAPI ll_GetAsyncKeyState(int) { return 0; }
static BOOL WINAPI ll_GetKeyboardState(PBYTE k) { if (k) memset(k, 0, 256); return TRUE; }
static int ll_patch_iat(HMODULE m) {
    static const struct { const char *name; void *stub; } R[] = {
        { "RegisterRawInputDevices", (void *)ll_RegisterRawInputDevices }, { "GetRawInputData", (void *)ll_GetRawInputData },
        { "GetRawInputBuffer", (void *)ll_GetRawInputBuffer }, { "GetKeyState", (void *)ll_GetKeyState },
        { "GetAsyncKeyState", (void *)ll_GetAsyncKeyState }, { "GetKeyboardState", (void *)ll_GetKeyboardState } };
    HMODULE u32 = GetModuleHandleW(L"user32.dll"); if (!u32) return 0;
    void *real[6]; for (int i = 0; i < 6; i++) real[i] = (void *)GetProcAddress(u32, R[i].name);
    BYTE *b = (BYTE *)m; IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)b; if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dir.VirtualAddress) return 0;
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dir.VirtualAddress); d->Name; d++)
        for (IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(b + d->FirstThunk); t->u1.Function; t++)
            for (int i = 0; i < 6; i++) if (real[i] && (void *)t->u1.Function == real[i]) {
                DWORD old;
                if (VirtualProtect(&t->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                    t->u1.Function = (ULONG_PTR)R[i].stub; VirtualProtect(&t->u1.Function, sizeof(void *), old, &old); n++;
                }
            }
    return n;
}
static void ll_block_keyboard(const char *when) {
    HMODULE mods[512]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    int total = 0; for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; i++) if (mods[i] != g_self) total += ll_patch_iat(mods[i]);
    s4log("lowlat: real keyboard/mouse blocked (%s): %d import entries patched", when, total);
}
// pad reads with block_real_input=1: pad 0 connected and idle, the others absent
static DWORD ll_blocked_pad(DWORD i, void *s) {
    if (i != 0 || !s) return ERROR_DEVICE_NOT_CONNECTED;
    memset(s, 0, 16); return ERROR_SUCCESS;  // XINPUT_STATE: packet number + XINPUT_GAMEPAD
}

static void install_lowlat(void) {
    wchar_t v[256];
    GetPrivateProfileStringW(L"lowlat", L"mode", L"off", v, 256, g_ini);
    LONG mode = _wcsicmp(v, L"latency1") == 0 ? LL_LATENCY1 : _wcsicmp(v, L"latent") == 0 ? LL_LATENT : LL_OFF;
    g_ll_offset_us = (LONG)(int)GetPrivateProfileIntW(L"lowlat", L"latent_offset_us", (UINT)-2000, g_ini);
    g_ll_measure = GetPrivateProfileIntW(L"lowlat", L"measure", 0, g_ini);
    g_ll_block = GetPrivateProfileIntW(L"lowlat", L"block_real_input", 0, g_ini);
    g_ll_ab_ms = GetPrivateProfileIntW(L"lowlat", L"ab_seconds", 20, g_ini) * 1000;
    g_ll_win_x = (int)GetPrivateProfileIntW(L"lowlat", L"window_x", (UINT)-1, g_ini);
    g_ll_win_y = (int)GetPrivateProfileIntW(L"lowlat", L"window_y", (UINT)-1, g_ini);
    GetPrivateProfileStringW(L"lowlat", L"ab", L"", v, 256, g_ini);
    for (wchar_t *ctx = NULL, *tok = wcstok_s(v, L", ", &ctx); tok && g_ll_nab < 8; tok = wcstok_s(NULL, L", ", &ctx)) {
        wchar_t *at = wcschr(tok, L'@'); int off = g_ll_offset_us; if (at) { *at = 0; off = _wtoi(at + 1); }
        int m = _wcsicmp(tok, L"latency1") == 0 ? LL_LATENCY1 : _wcsicmp(tok, L"latent") == 0 ? LL_LATENT : LL_OFF;
        g_ll_ab[g_ll_nab].mode = m; g_ll_ab[g_ll_nab].offset_us = off; g_ll_nab++;
    }
    if (!g_ll_nab) { g_ll_ab[0].mode = mode; g_ll_ab[0].offset_us = g_ll_offset_us; g_ll_nab = 1; }
    g_ll_mode = g_ll_ab[0].mode; g_ll_offset_us = g_ll_ab[0].offset_us;
    wchar_t cfg[MAX_PATH]; swprintf_s(cfg, L"%s\\config.ini", g_dir);
    g_ll_vsync_on = GetPrivateProfileIntW(L"screenSetting", L"vsync", 0, cfg) != 0;
    g_ll_fps = (int)GetPrivateProfileIntW(L"screenSetting", L"fps", 30, cfg);
    if (g_ll_fps != 30 && g_ll_fps != 60) g_ll_fps = 30;
    bool uses_latent = false; for (int i = 0; i < g_ll_nab; i++) uses_latent |= g_ll_ab[i].mode == LL_LATENT;
    if (uses_latent && g_ll_vsync_on) s4log("note lowlat: latent needs in-game vsync=0 (config.ini); it stays off while vsync=1");
    if (g_ll_block) ll_block_keyboard("load");
    bool any = g_ll_measure || g_ll_nab > 1 || g_ll_mode != LL_OFF;
    s4log("%s lowlat: mode %s@%d%s measure=%d block_real_input=%d (vsync=%d fps=%d)", any ? "OK  " : "off ", kLlNames[g_ll_mode],
          g_ll_offset_us, g_ll_nab > 1 ? " (A/B list)" : "", g_ll_measure, g_ll_block, g_ll_vsync_on, g_ll_fps);
    if (!any) return;
    HANDLE t = CreateThread(NULL, 0, ll_thread, NULL, 0, NULL); if (t) CloseHandle(t);
}
static void lowlat_tick(void) {  // background thread: patch modules loaded after start-up too
    static int n;
    if (g_ll_block && n < 2) { n++; ll_block_keyboard(n == 1 ? "first tick" : "second tick"); }
}
