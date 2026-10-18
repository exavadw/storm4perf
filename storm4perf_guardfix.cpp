// storm4perf_guardfix.cpp: a guarded projectile ends the thrower's running jutsu (e.g. a shuriken thrown just before a
// Rasengan hits the opponent's guard while the Rasengan runs: the Rasengan then ends without touching the opponent).
//
// Root cause (NSUNS4 Steam 1.09, measured in game 2026-10-17; 1.07 has the same code):
//   The defender's guard-hit handler 0x140788B4C(defender) starts by telling the attacker "your attack was guarded":
//   it looks the attacker up from defender+0xFB70/+0xFB74 (0x14085B574) and calls SetAttackResult 0x1407824B8(attacker,
//   3) from 0x140788BA8, for EVERY guarded hit. The result lives in attacker+0x14054 and is cleared when the attacker
//   starts a new action. A shuriken guarded after the attacker started the Rasengan writes 3 into the Rasengan's
//   result, so when the Rasengan's dash begins it reads "contact" and jumps straight to its end section.
//   Normal guarded hits come from the attacker's current attack: the hit's attack id (defender+0xFB7C, u16) equals the
//   attacker's current attack id (attacker+0x140A0, low 16 bits). A leftover projectile does not (0x7C vs 0xFFFF).
// Fix: skip that one SetAttackResult call when the two ids differ. Everything else in the guard-hit handler (the
//   defender's guard reaction, damage, effects) runs unchanged. Deterministic, so replays stay valid.
//
// Config: [fixes] guardfix=1. Earlier versions NOP'd two calls to 0x1407EBEC8 instead; that code never runs in these
// battles, so those patches are gone.
#include "replay/minhook/include/MinHook.h"
#include <intrin.h>

typedef void (__fastcall *GuardHitFn)(uint8_t *);
typedef void (__fastcall *SetResultFn)(uint8_t *, int);
typedef uint8_t *(__fastcall *GetPlayerFn)(int, int);
static GuardHitFn o_guard_hit;
static SetResultFn o_set_result;
static uint8_t *g_guard_defender;  // set while the guard-hit handler runs (game thread only)
static volatile LONG64 st_guard_skipped;

static void __fastcall hk_guard_hit(uint8_t *def) {
    g_guard_defender = def;
    o_guard_hit(def);
    g_guard_defender = NULL;
}

static void __fastcall hk_set_result(uint8_t *attacker, int result) {
    uint8_t *def = g_guard_defender;
    if (g_on.guardfix && def && attacker && (uintptr_t)_ReturnAddress() == (uintptr_t)VA(0x140788BAD)) {
        uint16_t hit = *(uint16_t *)(def + 0xFB7C), cur = *(uint16_t *)(attacker + 0x140A0);
        if (hit != cur) { InterlockedIncrement64(&st_guard_skipped); return; }
    }
    o_set_result(attacker, result);
}

static bool install_guardfix(void) {
    // Prologue of the guard-hit handler and the call site, so a different exe never gets hooked.
    static const uint8_t eh[] = { 0x4C, 0x8B, 0xF1, 0x0F, 0xB7, 0x89, 0x7C, 0xFB, 0x00, 0x00 };  // mov r14,rcx; movzx ecx,[rcx+FB7C]
    static const uint8_t ec[] = { 0xBA, 0x03, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xC8, 0xE8 };        // mov edx,3; mov rcx,rax; call
    if (!check_bytes("guardfix", 0x140788B65, eh, sizeof eh)) return false;
    if (!check_bytes("guardfix", 0x140788BA0, ec, sizeof ec)) return false;
    MH_STATUS r = MH_Initialize();
    if (r != MH_OK && r != MH_ERROR_ALREADY_INITIALIZED) { s4log("FAIL guardfix: MH_Initialize %d", (int)r); return false; }
    if (MH_CreateHook((void *)VA(0x140788B4C), (void *)hk_guard_hit, (void **)&o_guard_hit) != MH_OK ||
        MH_CreateHook((void *)VA(0x1407824B8), (void *)hk_set_result, (void **)&o_set_result) != MH_OK ||
        MH_EnableHook((void *)VA(0x140788B4C)) != MH_OK || MH_EnableHook((void *)VA(0x1407824B8)) != MH_OK) {
        s4log("FAIL guardfix: could not hook the guard-hit handler / SetAttackResult");
        return false;
    }
    return true;
}
