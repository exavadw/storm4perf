// storm4perf_guardfix.cpp: fix for guard-hit cancelling other players' jutsu (Bug 1).
//
// Three-tier fix matching main branch storm4perf 1.6.0:
//   Tier 1: NOP DAT_141666ebc global write/reset in FUN_14077e64c (SSE float stores)
//   Tier 2: NOP 6 internal FUN_140764210 calls that mis-target the guarding player
//   Tier 3: NOP 2 FUN_1407ebec8 calls from guard-hit handlers (root cause entry points)
//
// Root cause: when a projectile hits Player A's guard, FUN_14077e64c writes Player A's
// ID to global DAT_141666ebc as a float, then resets it. During this window, other
// players' jutsu state machines can read the stale global and cancel prematurely.
// Additionally, guard-hit handlers call FUN_1407ebec8(opponent) which propagates
// opponent state into the attacker's +0x8e4 field via FUN_140764210, ending active jutsu.
//
// Binary: NSUNS4 Steam 1.09 release (NSUNS4_release.exe-75912b)
// Config: [fixes] guardfix=1 (default on)

static volatile LONG64 st_guard_nops;

static bool install_guardfix() {
    // ================================================================
    // Tier 1: NOP DAT_141666ebc global write and reset in FUN_14077e64c
    // ================================================================
    // WRITE at RVA 0x14077E6B6 (FUN_14077e64c + 0x6A):
    //   F3 0F 11 05 FE 87 EE 00   movss [rip+0xEE87FE], xmm0  ; store player ID to DAT_141666ebc
    // RESET at RVA 0x14077E6E6 (FUN_14077e64c + 0x9A):
    //   F3 0F 11 05 CE 87 EE 00   movss [rip+0xEE87CE], xmm0  ; reset DAT_141666ebc to 0xff7fffff
    static const uint8_t write_e[] = { 0xF3, 0x0F, 0x11, 0x05, 0xFE, 0x87, 0xEE, 0x00 };
    static const uint8_t reset_e[] = { 0xF3, 0x0F, 0x11, 0x05, 0xCE, 0x87, 0xEE, 0x00 };
    if (!check_bytes("guardfix(write)", 0x14077E6B6ULL, write_e, sizeof(write_e))) return false;
    if (!check_bytes("guardfix(reset)", 0x14077E6E6ULL, reset_e, sizeof(reset_e))) return false;

    uint8_t nops8[8]; memset(nops8, 0x90, sizeof(nops8));
    if (!write_bytes("guardfix(write)", VA(0x14077E6B6), nops8, sizeof(nops8))) return false;
    if (!write_bytes("guardfix(reset)", VA(0x14077E6E6), nops8, sizeof(nops8))) return false;
    s4log("guardfix: NOPped DAT_141666ebc write at 0x14077E6B6 and reset at 0x14077E6E6 (8B each)");

    // ================================================================
    // Tier 2: NOP 6 internal FUN_140764210 calls in guard-hit handlers
    // ================================================================
    // Multiple guard-hit handlers call FUN_140764210(param_1, ...) where param_1 is the
    // GUARDING player, not the jutsu user. This overwrites the guarding player's action
    // state (+0x8e4) with the opponent's jutsu-end animation ID, causing the jutsu
    // attacker to skip to post-jutsu recovery animation. NOP all 5-byte call sites:
    struct { const char *tag; uint64_t rva; uint8_t bytes[5]; } calls[] = {
        { "call1", 0x1407EBF26ULL, { 0xE8, 0xE5, 0x82, 0xF7, 0xFF } },  // FUN_1407ebec8 matched-path
        { "call2", 0x1407EBF4BULL, { 0xE8, 0xC0, 0x82, 0xF7, 0xFF } },  // FUN_1407ebec8 fallback
        { "call3", 0x1407EB6B7ULL, { 0xE8, 0x54, 0x8B, 0xF7, 0xFF } },  // FUN_1407eb670 guard-hit handler
        { "call4", 0x1407E8864ULL, { 0xE8, 0xA7, 0xB9, 0xF7, 0xFF } },  // FUN_1407e86e0 projectile-guard handler
        { "call5", 0x14078854FULL, { 0xE8, 0xBC, 0xBC, 0xFD, 0xFF } },  // FUN_140787a84 collision 0x12 opponent->self
        { "call6", 0x140788572ULL, { 0xE8, 0x99, 0xBC, 0xFD, 0xFF } },  // FUN_140787a84 collision 0x1E self->opponent
    };
    for (int i = 0; i < 6; i++) {
        char label[32]; _snprintf_s(label, sizeof(label), _TRUNCATE, "guardfix(%s)", calls[i].tag);
        if (check_bytes(label, calls[i].rva, calls[i].bytes, 5)) {
            uint8_t nops5[5]; memset(nops5, 0x90, sizeof(nops5));
            if (!write_bytes(label, VA(calls[i].rva), nops5, sizeof(nops5))) return false;
            s4log("guardfix: NOPped FUN_140764210 %s at 0x%llX", calls[i].tag, calls[i].rva);
        } else {
            s4log("guardfix: SKIP %s NOP (bytes differ at 0x%llX)", calls[i].tag, calls[i].rva);
        }
    }

    // ================================================================
    // Tier 3: NOP FUN_1407ebec8 calls from guard-hit response handlers
    // ================================================================
    // The root cause entry points: FUN_1407aebf8 and FUN_1407af4c4 call
    // FUN_1407ebec8(opponent) which propagates opponent state into the jutsu
    // attacker's +0x8e4 field via FUN_140764210. NOPping internal FUN_140764210
    // calls alone wasn't sufficient because the vtable+0x18 dispatch inside
    // FUN_1407ebec8 also corrupts state. NOP the entire call at both sites:
    static const uint8_t ebec8_a[] = { 0xE8, 0x12, 0xCF, 0x03, 0x00 };
    static const uint8_t ebec8_b[] = { 0xE8, 0x26, 0xC9, 0x03, 0x00 };
    if (check_bytes("guardfix(ebec8_a)", 0x1407AEFB1ULL, ebec8_a, sizeof(ebec8_a))) {
        uint8_t nops5[5]; memset(nops5, 0x90, sizeof(nops5));
        if (!write_bytes("guardfix(ebec8_a)", VA(0x1407AEFB1), nops5, sizeof(nops5))) return false;
        s4log("guardfix: NOPped FUN_1407ebec8 call in FUN_1407aebf8 at 0x1407AEFB1");
    } else {
        s4log("guardfix: SKIP ebec8_a NOP (bytes differ at 0x1407AEFB1)");
    }
    if (check_bytes("guardfix(ebec8_b)", 0x1407AF59DULL, ebec8_b, sizeof(ebec8_b))) {
        uint8_t nops5[5]; memset(nops5, 0x90, sizeof(nops5));
        if (!write_bytes("guardfix(ebec8_b)", VA(0x1407AF59D), nops5, sizeof(nops5))) return false;
        s4log("guardfix: NOPped FUN_1407ebec8 call in FUN_1407af4c4 at 0x1407AF59D");
    } else {
        s4log("guardfix: SKIP ebec8_b NOP (bytes differ at 0x1407AF59D)");
    }

    InterlockedIncrement64(&st_guard_nops);
    return true;
}