// storm4perf_ui.cpp: puts storm4perf's shadow choice into Storm 4's own Options screen, included by storm4perf.cpp after
// storm4perf_gpu.cpp. Options > Shadow Quality offers OFF, VERY LOW, LOW, MEDIUM, HIGH instead of LOW, MEDIUM, HIGH.
// OK / Settings Complete saves LOW/MEDIUM/HIGH to config.ini (shadowQuality 0/1/2, as before) and the storm4perf choice to
// storm4perf.ini [gpu] shadows = off | potato | game. Section 4c of the doc has the background.
//
// The Options screen is ccUiOption (FUN_1406A74C8 open, FUN_1406A9E10 update) driving the Scaleform movie
// data/ui/flash/OTHER/option/option.gfx (a loose file in the game folder). The shadow row's value lives at
// state(+0x100)+0x40; the movie shows it with labels PC_option_004/005/006 fetched through
// ExternalInterface "MessageConvert" (the game's message lookup FUN_140ABD020).
// While the screen is open that value means 0 OFF, 1 VERY LOW, 2 LOW, 3 MEDIUM, 4 HIGH:
//   open    0x1406A7AA3  `mov eax,[r14+24h]; mov [rcx+40h],eax` (shadowQuality -> row) becomes a call to a stub
//   default 0x1406AAB9E  Return to Default stores 2 -> stores 4 (HIGH, the game's default)
//   right   0x1406AC126  `cmp eax,r12d` (r12d = 2) -> `cmp eax,4`; left keeps "> 0"
//   confirm 0x1406AC3FC  call FUN_140A44CA0(settings copy) goes through ui_apply: row -> shadowQuality + [gpu] shadows
//   movie   the game's Scaleform file opener (vtable slot 0x1410FB430 -> FUN_1409A7608) serves option.gfx with five
//           same-length bytecode patches (arrows and labels for five values), applied in memory to the game's own file
//   labels  FUN_140ABD020 returns, for PC_option_004/005/006, the entry of the current level (OFF = networkmode_057,
//           LOW/MEDIUM/HIGH = the game's own localized PC_option_004/005/006; VERY LOW is English text)

static bool g_ui_installed;
static uint8_t *volatile g_ui_state;   // ccUiOption state while the screen is open (set when it opens)

// ---------------------------------------------------------------- level mapping
static LONG ui_level_from_game(int q) {
    LONG s = g_gpu.shadows;
    if (s == SHADOWS_OFF) return 0;
    if (s == SHADOWS_POTATO) return 1;
    return (q >= 0 && q <= 2) ? q + 2 : 4;
}
static int ui_init_value(int q, uint8_t *state) {   // called from the stub at screen open
    g_ui_state = state;
    return (int)ui_level_from_game(q);
}
typedef void (*ApplySettingsFn)(int *s);
static ApplySettingsFn g_apply_settings;   // FUN_140A44CA0: copies the UI's settings struct into the game's
static void ui_apply(int *s) {              // s[9] = shadowQuality slot of the copy (and of what config.ini gets)
    int lvl = s[9];
    if (lvl >= 0 && lvl <= 4) {
        LONG mode = lvl == 0 ? SHADOWS_OFF : lvl == 1 ? SHADOWS_POTATO : SHADOWS_GAME;
        s[9] = lvl >= 2 ? lvl - 2 : 0;    // OFF / VERY LOW keep the game's lowest level underneath
        static const wchar_t *const names[] = { L"game", L"potato", L"off" };
        if (!WritePrivateProfileStringW(L"gpu", L"shadows", names[mode], g_ini))
            s4log("ERROR ui: cannot write [gpu] shadows to storm4perf.ini (error %lu)", GetLastError());
        InterlockedExchange(&g_gpu.shadows, mode);
        s4log("ui: Options saved shadow level %d -> shadowQuality=%d shadows=%ls", lvl, s[9], names[mode]);
    }
    g_apply_settings(s);
}

// ---------------------------------------------------------------- labels (message lookup detour)
typedef uint8_t *(*MsgLookupFn)(void *table, const char *id);
static MsgLookupFn g_msg_orig;   // trampoline
static uint8_t g_fake_entry[5][0x40]; static const char kVeryLow[] = "VERY LOW";
static uint8_t *hk_msg_lookup(void *table, const char *id) {
    uint8_t *e = g_msg_orig(table, id);
    if (!id || strncmp(id, "PC_option_00", 12) != 0 || (id[12] < '4' || id[12] > '6') || id[13]) return e;
    uint8_t *st = g_ui_state; int lvl = -1;
    __try { if (st) lvl = *(volatile int *)(st + 0x40); } __except (EXCEPTION_EXECUTE_HANDLER) { lvl = -1; }
    if (lvl < 0 || lvl > 4) return e;
    static const char *const src_id[] = { "networkmode_057", "PC_option_004", "PC_option_004", "PC_option_005", "PC_option_006" };
    uint8_t *src = g_msg_orig(table, src_id[lvl]);
    if (!src) return e;
    uint8_t *f = g_fake_entry[lvl];
    memcpy(f, src, sizeof g_fake_entry[lvl]);
    if (lvl == 1) *(const char **)(f + 0x10) = kVeryLow;
    return f;
}

// ---------------------------------------------------------------- the movie (served from memory, patched)
struct GfxPatch { uint32_t off; uint32_t n; uint8_t expect[3]; uint8_t repl[3]; };
static const uint32_t kOptionGfxSize = 89857;   // Storm 4 1.09 data/ui/flash/OTHER/option/option.gfx
static const GfxPatch kOptionPatches[] = {      // from gpu/re/gfx/patch_option.py
    { 0xCB2C, 1, { 0x02 }, { 0x04 } },                         // arrows: right arrow hides only at level 4
    { 0xCB48, 3, { 0xBD, 0xFF, 0xFF }, { 0x8F, 0xFF, 0xFF } }, // arrows switch default -> both arrows
    { 0xCB55, 3, { 0xBD, 0xFF, 0xFF }, { 0x8F, 0xFF, 0xFF } }, // arrows switch case 3  -> both arrows
    { 0xCC9A, 3, { 0xBD, 0xFF, 0xFF }, { 0x89, 0xFF, 0xFF } }, // label switch default  -> PC_option_005
    { 0xCCA7, 3, { 0xBD, 0xFF, 0xFF }, { 0x89, 0xFF, 0xFF } }, // label switch case 3   -> PC_option_005
};
static uint8_t *g_gfx; static uint32_t g_gfx_len; static INIT_ONCE g_gfx_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK gfx_load(PINIT_ONCE, PVOID, PVOID *) {
    wchar_t p[MAX_PATH]; swprintf_s(p, L"%s\\data\\ui\\flash\\OTHER\\option\\option.gfx", g_dir);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { s4log("ERROR ui: cannot open %ls (error %lu); Options keeps 3 shadow levels", p, GetLastError()); return TRUE; }
    DWORD sz = GetFileSize(h, NULL), got = 0;
    uint8_t *b = sz == kOptionGfxSize ? (uint8_t *)HeapAlloc(GetProcessHeap(), 0, sz) : NULL;
    bool ok = b && ReadFile(h, b, sz, &got, NULL) && got == sz;
    CloseHandle(h);
    if (!ok) { s4log("SKIP ui: option.gfx is %lu bytes, expected %lu (other version or a UI mod)", sz, kOptionGfxSize); if (b) HeapFree(GetProcessHeap(), 0, b); return TRUE; }
    for (const GfxPatch &q : kOptionPatches)
        if (memcmp(b + q.off, q.expect, q.n) != 0) { s4log("SKIP ui: option.gfx bytes differ at 0x%X", q.off); HeapFree(GetProcessHeap(), 0, b); return TRUE; }
    for (const GfxPatch &q : kOptionPatches) memcpy(b + q.off, q.repl, q.n);
    g_gfx = b; g_gfx_len = sz;
    s4log("ui: option.gfx patched in memory (%u bytes)", sz);
    return TRUE;
}
typedef void *(*OpenFileFn)(void *self, const char *url, int flags, int mode);
typedef void *(*MemFileCtorFn)(void *mem, const char *url, const void *data, uint32_t len);   // FUN_14089F710
static OpenFileFn g_open_orig;
static void *hk_open_file(void *self, const char *url, int flags, int mode) {
    size_t n = url ? strlen(url) : 0;
    if (g_ui_installed && n >= 17 && (_stricmp(url + n - 17, "option/option.swf") == 0 || _stricmp(url + n - 17, "option/option.gfx") == 0)) {
        InitOnceExecuteOnce(&g_gfx_once, gfx_load, NULL, NULL);
        void **heap = *(void ***)VA(0x14165FD88);
        if (g_gfx && heap) {
            typedef void *(*AllocFn)(void *, size_t, size_t);
            void *mem = ((AllocFn)(*(void ***)heap)[0x50 / 8])(heap, 0x30, 0);
            if (mem) return ((MemFileCtorFn)VA(0x14089F710))(mem, url, g_gfx, g_gfx_len);
        }
    }
    return g_open_orig(self, url, flags, mode);
}

// ---------------------------------------------------------------- install
static bool ui_redirect_call(uint64_t site, uint64_t expect_target, void *hook) {
    uint8_t *p = VA(site); int32_t rel;
    if (p[0] != 0xE8) return false;
    memcpy(&rel, p + 1, 4);
    if ((uint64_t)(p + 5 + rel) != (uint64_t)VA(expect_target)) return false;
    uint8_t *stub = alloc_stub(12); if (!stub) return false;
    abs_jmp(stub, hook, false);
    uint8_t b[5] = { 0xE8 }; if (!rel32_to(p, 5, stub, &rel)) return false;
    memcpy(b + 1, &rel, 4);
    return write_bytes("ui", p, b, 5);
}
static bool install_ui(void) {
    if (!g_gpu_shadow_hooked) { s4log("SKIP ui: needs the shadows switch"); return false; }
    static const uint8_t open_e[] = { 0x48, 0x8B, 0x8E, 0x00, 0x01, 0x00, 0x00, 0x41, 0x8B, 0x46, 0x24, 0x89, 0x41, 0x40 };  // 0x1406A7A9C
    static const uint8_t def_e[] = { 0x44, 0x89, 0x60, 0x40 };                                                       // 0x1406AAB9E
    static const uint8_t right_e[] = { 0x48, 0x8B, 0x8F, 0x00, 0x01, 0x00, 0x00, 0x8B, 0x41, 0x40, 0x41, 0x3B, 0xC4, 0x7D, 0x4A };  // 0x1406AC11C
    static const uint8_t left_e[] = { 0x48, 0x8B, 0x8F, 0x00, 0x01, 0x00, 0x00, 0x8B, 0x41, 0x40, 0x85, 0xC0, 0x7E, 0x32 };      // 0x1406ABC39
    static const uint8_t conf_e[] = { 0xE8, 0x9F, 0x88, 0x39, 0x00 };                                                // 0x1406AC3FC
    static const uint8_t msg_e[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0xCA, 0xE8 }; // 0x140ABD020
    if (!check_bytes("ui", 0x1406A7A9C, open_e, sizeof open_e) || !check_bytes("ui", 0x1406AAB9E, def_e, sizeof def_e) ||
        !check_bytes("ui", 0x1406AC11C, right_e, sizeof right_e) || !check_bytes("ui", 0x1406ABC39, left_e, sizeof left_e) ||
        !check_bytes("ui", 0x1406AC3FC, conf_e, sizeof conf_e) || !check_bytes("ui", 0x140ABD020, msg_e, sizeof msg_e) ||
        !in_exe(0x1410FB430, 8))
        return false;
    void **open_slot = (void **)VA(0x1410FB430);
    if (*open_slot != VA(0x1409A7608)) { s4log("SKIP ui: file opener slot = %p, expected %p", *open_slot, VA(0x1409A7608)); return false; }

    // open: `mov eax,[r14+24h]; mov [rcx+40h],eax` -> call stub (eax = ui_init_value(q, state); [rcx+40h] = eax)
    uint8_t *stub = alloc_stub(48); if (!stub) return false;
    {
        uint8_t s[] = { 0x41, 0x8B, 0x46, 0x24,             // mov eax,[r14+24h]
                        0x51,                               // push rcx
                        0x48, 0x83, 0xEC, 0x20,             // sub rsp,20h
                        0x48, 0x8B, 0xD1,                   // mov rdx,rcx (state)
                        0x8B, 0xC8,                         // mov ecx,eax (shadowQuality)
                        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, ui_init_value
                        0xFF, 0xD0,                         // call rax
                        0x48, 0x83, 0xC4, 0x20,             // add rsp,20h
                        0x59,                               // pop rcx
                        0x89, 0x41, 0x40,                   // mov [rcx+40h],eax
                        0xC3 };                             // ret
        void *fn = (void *)ui_init_value; memcpy(s + 16, &fn, 8);
        memcpy(stub, s, sizeof s);
    }
    uint8_t *site = VA(0x1406A7AA3); int32_t rel;
    if (!rel32_to(site, 5, stub, &rel)) return false;
    uint8_t callb[7] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90 }; memcpy(callb + 1, &rel, 4);

    // labels: detour the message lookup (12 prologue bytes to a trampoline)
    uint8_t *msite = VA(0x140ABD020), *tramp = alloc_stub(32); if (!tramp) return false;
    memcpy(tramp, msite, 12); abs_jmp(tramp + 12, msite + 12, false);
    g_msg_orig = (MsgLookupFn)tramp;
    uint8_t mj[12]; abs_jmp(mj, (void *)hk_msg_lookup, false);

    g_apply_settings = (ApplySettingsFn)VA(0x140A44CA0);
    g_open_orig = (OpenFileFn)*open_slot;
    static const uint8_t def_r[] = { 0xC6, 0x40, 0x40, 0x04 };    // mov byte ptr [rax+40h],4
    static const uint8_t right_r[] = { 0x83, 0xF8, 0x04 };        // cmp eax,4
    void *open_repl = (void *)hk_open_file;
    bool ok = write_bytes("ui", site, callb, sizeof callb) && write_bytes("ui", VA(0x1406AAB9E), def_r, sizeof def_r) &&
              write_bytes("ui", VA(0x1406AC126), right_r, sizeof right_r) &&
              ui_redirect_call(0x1406AC3FC, 0x140A44CA0, (void *)ui_apply) && write_bytes("ui", msite, mj, sizeof mj) &&
              write_bytes("ui", (uint8_t *)open_slot, &open_repl, sizeof open_repl);
    g_ui_installed = ok;
    return ok;
}
