// storm4perf_gpu.cpp: GPU-side options for storm4perf, included by storm4perf.cpp (one translation unit; it uses the
// helpers defined there: VA, check_bytes, write_bytes, alloc_stub, abs_jmp, rel32_to, s4log, g_ini).
// Measurements and how these were found: storm4-performance.md, section 4c ("What the release module does").
//
// [gpu] shadows = game | potato | off
//   ccGameDrawEnv::BeginScreen (0x14085BA68, main thread, every frame) copies the stage's shadow settings into the shadow
//   object nuccShadowProjection (DAT_1416B6030): the enable flag (+0x10) through FUN_14047D960 at call 0x14085BC55, and
//   one filter blur width per cascade (+0x550[i]) through FUN_140A3FF10 at call 0x14085BCAD. Both calls are redirected.
//   off:    the enable flag is written as 0, so nuccLayerShadow::Draw skips the whole shadow pass (no caster depth, no
//           RG16F filter) and the receivers see shadows disabled, exactly as on a stage without shadows.
//   potato: cascade count (+0x588, the game allows 1-4) and tile sizes (FUN_140A40060) are set to [gpu] shadow_cascades /
//           shadow_size, and the per-cascade blur width is capped at shadow_blur. The shared size constants in .rdata
//           (0x141018FC4..) are not touched. Switching back to game restores the values for the current shadowQuality.
// The game's Options > Shadow Quality row offers the same choice (storm4perf_ui.cpp).

struct GpuFlags { volatile LONG shadows; volatile LONG cascades, size_near, size_far, blur; };
enum { SHADOWS_GAME = 0, SHADOWS_POTATO = 1, SHADOWS_OFF = 2 };
static GpuFlags g_gpu;
static bool g_gpu_shadow_hooked;
static volatile LONG64 st_shadow_frames_off;

static void read_gpu_flags(void) {
    wchar_t v[32];
    GetPrivateProfileStringW(L"gpu", L"shadows", L"game", v, 32, g_ini);
    LONG s = _wcsicmp(v, L"potato") == 0 ? SHADOWS_POTATO : _wcsicmp(v, L"off") == 0 ? SHADOWS_OFF : SHADOWS_GAME;
    if (s == SHADOWS_GAME && _wcsicmp(v, L"game") != 0) s4log("note: [gpu] shadows=%ls is not game/potato/off; using game", v);
    LONG c = (LONG)GetPrivateProfileIntW(L"gpu", L"shadow_cascades", 2, g_ini);
    if (c < 1 || c > 4) { s4log("note: [gpu] shadow_cascades=%ld out of 1-4; using 2", c); c = 2; }
    int n = 768, f = 512;
    GetPrivateProfileStringW(L"gpu", L"shadow_size", L"768,512", v, 32, g_ini);
    if (swscanf_s(v, L"%d,%d", &n, &f) < 1 || n < 128 || n > 2048) { n = 768; f = 512; }
    if (f < 128 || f > n) f = n;
    LONG b = (LONG)GetPrivateProfileIntW(L"gpu", L"shadow_blur", 2, g_ini);
    if (b < 0 || b > 8) b = 2;
    InterlockedExchange(&g_gpu.cascades, c); InterlockedExchange(&g_gpu.size_near, n); InterlockedExchange(&g_gpu.size_far, f);
    InterlockedExchange(&g_gpu.blur, b);
    InterlockedExchange(&g_gpu.shadows, g_gpu_shadow_hooked ? s : SHADOWS_GAME);
}

// ---------------------------------------------------------------- shadows (main thread, inside BeginScreen)
typedef void (*SetSizesFn)(void *obj, float c0, float c1, float c2, float c3);  // FUN_140A40060: tile rects in the atlas
static LONG g_shadow_applied = -1, g_shadow_applied_c, g_shadow_applied_n, g_shadow_applied_f;
static void shadow_apply_layout(uint8_t *obj, LONG mode) {
    int *count = (int *)(obj + 0x588);
    float *tile0_w = (float *)(obj + 0x478);   // rect 0 of 4 at +0x470: x, y, w, h
    SetSizesFn set_sizes = (SetSizesFn)VA(0x140A40060);
    if (mode == SHADOWS_POTATO) {
        LONG c = g_gpu.cascades, n = g_gpu.size_near, f = g_gpu.size_far;
        if (g_shadow_applied == mode && *count == c && *tile0_w == (float)n && c == g_shadow_applied_c && n == g_shadow_applied_n && f == g_shadow_applied_f) return;
        *count = c; set_sizes(obj, (float)n, (float)n, (float)f, (float)f);   // count first: set_sizes lays out `count` tiles
        g_shadow_applied_c = c; g_shadow_applied_n = n; g_shadow_applied_f = f;
        s4log("shadows: potato, %ld cascade(s) at %ld/%ld px, blur <= %ld", c, n, f, g_gpu.blur);
    } else if (g_shadow_applied == SHADOWS_POTATO) {
        // back to the values FUN_140530174 (settings apply) uses for the current shadowQuality (settings struct +0x24)
        int q = (int)((uint32_t *)VA(0x141561108))[9];
        if (q >= 0 && q <= 2) {
            *count = q == 0 ? 3 : 4;
            float a = q == 2 ? 2048.0f : 1536.0f, b = q == 2 ? 1024.0f : 768.0f;
            set_sizes(obj, a, a, b, b);
            s4log("shadows: restored for shadowQuality %d", q);
        }
    }
    g_shadow_applied = mode;
}
static void hk_shadow_enable(uint8_t *obj, LONG enable) {          // replaces FUN_14047D960(obj, enable): obj+0x10 = enable
    LONG mode = g_gpu.shadows;
    if (mode == SHADOWS_OFF && enable) { enable = 0; st_shadow_frames_off++; }
    *(LONG *)(obj + 0x10) = enable;
    if (mode != g_shadow_applied || mode == SHADOWS_POTATO) shadow_apply_layout(obj, mode);
}
static void hk_shadow_blur(uint8_t *obj, uint8_t cascade, LONG width) {   // replaces FUN_140A3FF10: obj+0x550[i] = width
    if (g_gpu.shadows == SHADOWS_POTATO && width > g_gpu.blur) width = g_gpu.blur;
    *(LONG *)(obj + 0x550 + (size_t)cascade * 4) = width;
}
static bool redirect_call(const char *fix, uint64_t site, uint64_t expect_target, void *hook) {
    uint8_t *p = VA(site);
    if (p[0] != 0xE8) { s4log("SKIP %s: no call at 0x%llX", fix, site); return false; }
    int32_t rel; memcpy(&rel, p + 1, 4);
    if ((uint64_t)(p + 5 + rel) != (uint64_t)VA(expect_target)) { s4log("SKIP %s: call at 0x%llX does not go to 0x%llX", fix, site, expect_target); return false; }
    uint8_t *stub = alloc_stub(12); if (!stub) return false;
    abs_jmp(stub, hook, false);
    uint8_t b[5] = { 0xE8 }; if (!rel32_to(p, 5, stub, &rel)) return false;
    memcpy(b + 1, &rel, 4);
    return write_bytes(fix, p, b, 5);
}
static bool install_gpu_shadows(void) {
    // call sites in BeginScreen, with the instructions that set up their arguments
    static const uint8_t en_e[] = { 0x42, 0x8B, 0x14, 0x3B, 0x48, 0x8B, 0xC8, 0xE8, 0x06, 0x1D, 0xC2, 0xFF };          // 0x14085BC4E
    static const uint8_t bl_e[] = { 0x45, 0x8B, 0x06, 0x8A, 0xD3, 0x48, 0x8B, 0xC8, 0xE8, 0x5E, 0x42, 0x1E, 0x00 };    // 0x14085BCA5
    static const uint8_t en_fn[] = { 0x89, 0x51, 0x10, 0xC3 };                                                          // 0x14047D960
    static const uint8_t bl_fn[] = { 0x0F, 0xB6, 0xC2, 0x44, 0x89, 0x84, 0x81, 0x50, 0x05, 0x00, 0x00, 0xC3 };          // 0x140A3FF10
    static const uint8_t sz_fn[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x0F, 0x29, 0x74, 0x24, 0x50, 0x48, 0x8B, 0xD9 };  // 0x140A40060
    if (!check_bytes("shadows", 0x14085BC4E, en_e, sizeof en_e) || !check_bytes("shadows", 0x14085BCA5, bl_e, sizeof bl_e) ||
        !check_bytes("shadows", 0x14047D960, en_fn, sizeof en_fn) || !check_bytes("shadows", 0x140A3FF10, bl_fn, sizeof bl_fn) ||
        !check_bytes("shadows", 0x140A40060, sz_fn, sizeof sz_fn))
        return false;
    return redirect_call("shadows", 0x14085BC55, 0x14047D960, (void *)hk_shadow_enable) &&
           redirect_call("shadows", 0x14085BCAD, 0x140A3FF10, (void *)hk_shadow_blur);
}

static void install_gpu(void) {
    g_gpu_shadow_hooked = install_gpu_shadows();
    s4log("%s shadows (game / potato / off)", g_gpu_shadow_hooked ? "OK  " : "FAIL");
    if (g_gpu_shadow_hooked) g_installed_any = 1;
    read_gpu_flags();
    static const char *const sn[] = { "game", "potato", "off" };
    s4log("active: shadows=%s (potato: %ld cascades %ld/%ld blur %ld)", sn[g_gpu.shadows], g_gpu.cascades, g_gpu.size_near, g_gpu.size_far, g_gpu.blur);
}
static void gpu_live_change(void) {
    LONG s = g_gpu.shadows, c = g_gpu.cascades, n = g_gpu.size_near, f = g_gpu.size_far, b = g_gpu.blur;
    read_gpu_flags();
    static const char *const sn[] = { "game", "potato", "off" };
    if (s != g_gpu.shadows || c != g_gpu.cascades || n != g_gpu.size_near || f != g_gpu.size_far || b != g_gpu.blur)
        s4log("live change: shadows=%s (%ld cascades %ld/%ld blur %ld)", sn[g_gpu.shadows], g_gpu.cascades, g_gpu.size_near,
              g_gpu.size_far, g_gpu.blur);
}
