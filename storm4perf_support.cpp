// storm4perf_support.cpp: Revolution-style support health for Storm 4 1.09.
// Hooks ccPlayerSupport::ClacCrashPointVal (damage), RecoverFromDead (KO recovery),
// and ThinkSummon (summon gate) to layer a parallel HP system on top of the native
// crash-point mechanics. Drives gauge_sprt.swf via the native Flash bridge
// (FUN_140607ba0) using SetGaugeRateL/R, goToAndStopDead, goToAndPlayRecover.
// All RVAs verified against nucc-symbols CSV sha 8324cc6d (Storm 4 1.09 release).
// Included by storm4perf.cpp after storm4perf_ui.cpp.

// ---------------------------------------------------------------- config
struct SupportConfig {
    float maxHp;           // default 100.0
    float koCooldownSec;   // default 10.0
    float regenRatePerSec; // default 20.0
    int enabled;           // 1 = on
};
static SupportConfig g_supCfg = { 100.0f, 10.0f, 20.0f, 1 };
static volatile LONG g_supInstalled = 0;

// ---------------------------------------------------------------- per-support state (side-car, max 4 supports)
struct SupState {
    uintptr_t ptr;
    float hp;
    float koTimer;
    int isKo;     // 1 = KO, waiting for cooldown
    int isRegen;  // 1 = regenerating HP
};
#define MAX_SUP 4
static SupState g_supStates[MAX_SUP];
static volatile LONG g_supCount = 0;
static SRWLOCK g_supLock = SRWLOCK_INIT;

static SupState *sup_get_or_create(uintptr_t p) {
    AcquireSRWLockExclusive(&g_supLock);
    for (int i = 0; i < g_supCount; i++) {
        if (g_supStates[i].ptr == p) { ReleaseSRWLockExclusive(&g_supLock); return &g_supStates[i]; }
    }
    if (g_supCount >= MAX_SUP) { ReleaseSRWLockExclusive(&g_supLock); return NULL; }
    SupState *s = &g_supStates[g_supCount++];
    s->ptr = p; s->hp = g_supCfg.maxHp; s->koTimer = 0; s->isKo = 0; s->isRegen = 0;
    ReleaseSRWLockExclusive(&g_supLock);
    return s;
}

// ---------------------------------------------------------------- native Flash bridge (FUN_140607ba0)
typedef void (__fastcall *FlashBridge_t)(uint64_t ui, const char *inst, const char *path,
    const char *method, uint64_t unk, void *args, uint32_t nArgs);
static FlashBridge_t g_flashBridge = NULL;

static void flash_call_double(const char *path, const char *method, double val) {
    if (!g_flashBridge) return;
    struct { uint64_t typeDesc; double v; } arg;
    arg.typeDesc = 0xffffffffffffffffull; arg.v = val;
    g_flashBridge(0xffffffffffffffffull, "supportGauge", path, method, 0, &arg, 1);
}

static void flash_call_noargs(const char *path, const char *method) {
    if (!g_flashBridge) return;
    g_flashBridge(0xffffffffffffffffull, "supportGauge", path, method, 0, NULL, 0);
}

// ---------------------------------------------------------------- hook targets (RVAs from nucc-symbols 8324cc6d)
// ccPlayerSupport::ClacCrashPointVal @ 0x5C8FE4
// ccPlayerSupport::RecoverFromDead   @ 0x5E3064
// ccPlayerSupport::ThinkSummon       @ 0x365244
// Flash bridge FUN_140607ba0         @ 0x607BA0
typedef void (__fastcall *CrashFn_t)(uintptr_t, float);
typedef void (__fastcall *RecoverFn_t)(uintptr_t);
typedef int  (__fastcall *ThinkSummonFn_t)(uintptr_t);

static CrashFn_t      g_origCalcCrash   = NULL;
static RecoverFn_t    g_origRecover     = NULL;
static ThinkSummonFn_t g_origThinkSummon = NULL;

// Trampoline buffers (32 bytes each: original instructions + abs_jmp back)
static uint8_t g_crashTramp[32];
static uint8_t g_recoverTramp[32];
static uint8_t g_summonTramp[32];

// --- ClacCrashPointVal hook: intercept damage to support ---
static void __fastcall hk_calc_crash(uintptr_t supPtr, float damage) {
    if (g_supCfg.enabled && supPtr && damage > 0) {
        SupState *s = sup_get_or_create(supPtr);
        if (s && !s->isKo && !s->isRegen) {
            s->hp -= damage;
            if (s->hp <= 0.0f) {
                s->hp = 0.0f;
                s->isKo = 1;
                s->koTimer = g_supCfg.koCooldownSec;
                s->isRegen = 0;
                s4log("support KO! cooldown=%.1fs", g_supCfg.koCooldownSec);
            }
        }
    }
    if (g_origCalcCrash) g_origCalcCrash(supPtr, damage);
}

// --- RecoverFromDead hook: sync native KO recovery ---
static void __fastcall hk_recover(uintptr_t supPtr) {
    if (g_supCfg.enabled && supPtr) {
        SupState *s = sup_get_or_create(supPtr);
        if (s && s->isKo && s->koTimer <= 0.0f) {
            s->isRegen = 1;
            s4log("support entering regen phase");
        }
    }
    if (g_origRecover) g_origRecover(supPtr);
}

// --- ThinkSummon hook: block summon while KO or low HP ---
static int __fastcall hk_think_summon(uintptr_t supPtr) {
    if (g_supCfg.enabled && supPtr) {
        AcquireSRWLockShared(&g_supLock);
        for (int i = 0; i < g_supCount; i++) {
            if (g_supStates[i].ptr == supPtr) {
                const SupState *s = &g_supStates[i];
                if (s->isKo || (s->isRegen && s->hp < g_supCfg.maxHp * 0.25f)) {
                    ReleaseSRWLockShared(&g_supLock);
                    return 0;
                }
                break;
            }
        }
        ReleaseSRWLockShared(&g_supLock);
    }
    if (g_origThinkSummon) return g_origThinkSummon(supPtr);
    return 0;
}

// ---------------------------------------------------------------- per-frame tick (called from bg_thread or Present hook)
static void support_tick(float dt) {
    if (!g_supCfg.enabled || !g_supInstalled) return;
    AcquireSRWLockExclusive(&g_supLock);
    for (int i = 0; i < g_supCount; i++) {
        SupState *s = &g_supStates[i];
        if (s->isKo) {
            s->koTimer -= dt;
            if (s->koTimer <= 0.0f) { s->koTimer = 0; s->isRegen = 1; }
        } else if (s->isRegen) {
            s->hp += g_supCfg.regenRatePerSec * dt;
            if (s->hp >= g_supCfg.maxHp) { s->hp = g_supCfg.maxHp; s->isRegen = 0; }
        }
    }
    ReleaseSRWLockExclusive(&g_supLock);
}

// ---------------------------------------------------------------- native Flash UI drive (called per frame)
static void support_drive_ui(void) {
    if (!g_supCfg.enabled || !g_supInstalled || !g_flashBridge) return;
    static const char *paths[] = { "_root.all_nut.all_sprt_1p", "_root.all_nut.all_sprt_2p" };
    AcquireSRWLockShared(&g_supLock);
    for (int i = 0; i < g_supCount && i < MAX_SUP; i++) {
        const SupState *s = &g_supStates[i];
        const char *path = paths[i < 2 ? i : i - 2];
        const char *rateMethod = (i < 2) ? "SetGaugeRateL" : "SetGaugeRateR";
        double rate = (g_supCfg.maxHp > 0) ? (s->hp / g_supCfg.maxHp) : 0.0;
        if (rate < 0.0) rate = 0.0; if (rate > 1.0) rate = 1.0;
        flash_call_double(path, rateMethod, rate);
        if (s->isKo) flash_call_noargs(path, "goToAndStopDead");
        else if (s->isRegen) flash_call_noargs(path, "goToAndPlayRecover");
    }
    ReleaseSRWLockShared(&g_supLock);
}

// ---------------------------------------------------------------- install helper: build trampoline from original bytes + abs_jmp back
static bool make_trampoline(const char *name, uint64_t rva, uint8_t *tramp, size_t trampCap,
                            void *hookFn, void **origOut, size_t origLen) {
    if (!in_exe(rva, origLen)) { s4log("FAIL %s: RVA 0x%llX out of range", name, rva); return false; }
    uint8_t *site = VA(rva);
    // Save original bytes into trampoline
    if (origLen + 12 > trampCap) { s4log("FAIL %s: trampoline too small", name); return false; }
    memcpy(tramp, site, origLen);
    // Add abs_jmp back to site+origLen
    abs_jmp(tramp + origLen, (void *)(site + origLen), false);
    *origOut = (void *)tramp;
    // Now overwrite the original site with abs_jmp to our hook
    uint8_t jmpBuf[12];
    abs_jmp(jmpBuf, hookFn, false);
    if (!write_bytes(name, site, jmpBuf, 12)) return false;
    return true;
}

// ---------------------------------------------------------------- install
static bool install_support_health(void) {
    // Read config
    g_supCfg.enabled = GetPrivateProfileIntW(L"support", L"enabled", 1, g_ini);
    g_supCfg.maxHp = (float)GetPrivateProfileIntW(L"support", L"max_hp", 100, g_ini);
    g_supCfg.koCooldownSec = (float)GetPrivateProfileIntW(L"support", L"ko_cooldown_sec", 10, g_ini);
    g_supCfg.regenRatePerSec = (float)GetPrivateProfileIntW(L"support", L"regen_rate", 20, g_ini);
    if (!g_supCfg.enabled) { s4log("off  support_health ([support] enabled=0)"); return false; }

    // Resolve Flash bridge
    g_flashBridge = (FlashBridge_t)VA(0x140607BA0);
    if (!in_exe(0x140607BA0, 1)) { s4log("FAIL support_health: Flash bridge RVA out of range"); return false; }

    // Hook ClacCrashPointVal @ 0x5C8FE4 (overwrite first 12 bytes)
    if (!make_trampoline("support(CrashPoint)", 0x1405C8FE4, g_crashTramp, sizeof(g_crashTramp),
                         (void *)hk_calc_crash, (void **)&g_origCalcCrash, 12)) return false;

    // Hook RecoverFromDead @ 0x5E3064
    if (!make_trampoline("support(Recover)", 0x1405E3064, g_recoverTramp, sizeof(g_recoverTramp),
                         (void *)hk_recover, (void **)&g_origRecover, 12)) return false;

    // Hook ThinkSummon @ 0x365244
    if (!make_trampoline("support(ThinkSummon)", 0x140365244, g_summonTramp, sizeof(g_summonTramp),
                         (void *)hk_think_summon, (void **)&g_origThinkSummon, 12)) return false;

    InterlockedExchange(&g_supInstalled, 1);
    s4log("OK   support_health (maxHp=%.0f ko=%.1fs regen=%.0f/s)", g_supCfg.maxHp, g_supCfg.koCooldownSec, g_supCfg.regenRatePerSec);
    return true;
}
