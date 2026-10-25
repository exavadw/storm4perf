# Guardfix Research: Guard-Hit Cancels Other Players' Jutsu

## 1. Root Cause (Ghidra Analysis)

### The Bug
When a projectile (kunai, shuriken, item) hits Player A's guard, Player B's active running jutsu (e.g., Rasengan mid-dash) can end prematurely. This is a cross-player state contamination bug with two independent mechanisms.

### Mechanism A: Global State Race (DAT_141666ebc)

`FUN_14077e64c` writes the defending player's ID (as float) to global `DAT_141666ebc` during guard-hit processing, then resets it to `0xff7fffff`. Other players' jutsu state machines can read this global in the window between write and reset, causing their active jutsu to cancel prematurely.

```c
// FUN_14077e64c (RVA 0x77E64C)
if (iVar2 != 0) {
    DAT_141666ebc = (undefined4)param_1[0x13];  // WRITE: player ID as float
    lVar1 = *(longlong *)param_1[0x41];
    (**(code **)(lVar1 + 0x18))((longlong *)param_1[0x41],1,1,lVar1,in_stack_00000028);
    FUN_140557220(param_1[0x41]);
    DAT_141666ebc = 0xff7fffff;                  // RESET: sentinel value
    FUN_14077c6d0(param_1);
}
```

The SSE instructions at the binary level:
- **WRITE** at RVA `0x14077E6B6`: `F3 0F 11 05 FE 87 EE 00` — `movss [rip+0xEE87FE], xmm0`
- **RESET** at RVA `0x14077E6E6`: `F3 0F 11 05 CE 87 EE 00` — `movss [rip+0xEE87CE], xmm0`

### Mechanism B: Mis-targeted FUN_1407ebec8 Calls

Guard-hit response handlers (`FUN_1407aebf8`, `FUN_1407af4c4`) call `FUN_1407ebec8(opponent)` where the argument is NOT `param_1` (the defender being hit), but rather the result of `FUN_14085b588(player_id, index-1)` — an **opponent lookup**.

**Site 1:** `FUN_1407aebf8` at RVA `0x7AEFB1`
```
1407aefae  MOV RCX,RAX          ; RCX = opponent player object
1407aefb1  CALL FUN_1407ebec8   ; E8 12 CF 03 00
```

**Site 2:** `FUN_1407af4c4` at RVA `0x7AF59D`
```
1407af59a  MOV RCX,RAX          ; RCX = opponent player object
1407af59d  CALL FUN_1407ebec8   ; E8 26 C9 03 00
```

### What FUN_1407ebec8 Does

```c
undefined8 FUN_1407ebec8(longlong *param_1) {
    // param_1 = OPPONENT player object (attacker whose jutsu might be cancelled)
    longlong *opponent_sub = (**(code **)(*param_1 + 0xd88))();
    if (opponent_sub == NULL) return 0;
    
    if (*(int *)(param_1 + 0x15504) == 6) {  // guard state check
        for (int i = 0; i < 2; i++) {
            longlong *sub = FUN_14079bda4(opponent_sub, i);
            if (sub != NULL && (int)sub[0x198] == 0xe3) {  // running jutsu type
                uVar1 = (**(code **)(*sub + 0x18))(sub);
                FUN_140764210(param_1, uVar1);  // WRITE to param_1+0x8e4
                return 1;
            }
        }
    }
    // Fallback path
    uVar1 = (**(code **)(*opponent_sub + 0x18))(opponent_sub);
    FUN_140764210(param_1, uVar1);  // WRITE to param_1+0x8e4
    return 0;
}
```

### What FUN_140764210 Does

```c
void FUN_140764210(longlong *param_1, int param_2) {
    int current = (**(code **)(*param_1 + 0x18))();
    if (current != param_2) {
        *(int *)(param_1 + 0x8e4) = param_2;  // ACTION STATE FIELD
    }
}
```

This writes an action state value to `player+0x8e4`. When called on the attacker during the defender's guard-hit processing, it overwrites the attacker's current action state with a value derived from the guard context, prematurely ending any active jutsu.

Additionally, multiple internal guard-hit handler functions also call `FUN_140764210` directly with the wrong player target, creating secondary contamination paths.

## 2. Three-Tier Fix Implementation

The fix is implemented in `storm4perf_guardfix.cpp` with three independent tiers, matching main branch storm4perf 1.6.0. Each tier addresses a different layer of the bug.

### Tier 1: NOP DAT_141666ebc Global Write/Reset

**Target:** `FUN_14077e64c` SSE float stores to `DAT_141666ebc`

| Site | RVA | Expected Bytes | Instruction |
|------|-----|----------------|-------------|
| write | `0x14077E6B6` | `F3 0F 11 05 FE 87 EE 00` | `movss [rip+0xEE87FE], xmm0` |
| reset | `0x14077E6E6` | `F3 0F 11 05 CE 87 EE 00` | `movss [rip+0xEE87CE], xmm0` |

Both 8-byte instructions are replaced with NOP sleds (`0x90 × 8`). This prevents the transient player ID from ever reaching the shared global variable. The virtual dispatch immediately after still runs correctly because it uses `param_1[0x41]`, not the global.

### Tier 2: NOP 6 Internal FUN_140764210 Calls

**Target:** Direct `FUN_140764210` calls within guard-hit handler functions that pass the guarding player instead of the jutsu user.

| Tag | RVA | Expected Bytes | Context |
|-----|-----|----------------|---------|
| call1 | `0x1407EBF26` | `E8 E5 82 F7 FF` | `FUN_1407ebec8` matched-path call |
| call2 | `0x1407EBF4B` | `E8 C0 82 F7 FF` | `FUN_1407ebec8` fallback call |
| call3 | `0x1407EB6B7` | `E8 54 8B F7 FF` | `FUN_1407eb670` guard-hit handler |
| call4 | `0x1407E8864` | `E8 A7 B9 F7 FF` | `FUN_1407e86e0` projectile-guard handler |
| call5 | `0x14078854F` | `E8 BC BC FD FF` | `FUN_140787a84` collision type 0x12 (opponent→self) |
| call6 | `0x140788572` | `E8 99 BC FD FF` | `FUN_140787a84` collision type 0x1E (self→opponent) |

Each 5-byte CALL instruction is replaced with `0x90 × 5`. These sites use a soft-fail pattern: if bytes don't match, the site is skipped with a log message rather than aborting the entire fix. This allows partial application if some sites have already been patched or differ across builds.

### Tier 3: NOP 2 FUN_1407ebec8 Entry Point Calls

**Target:** The root cause entry points where guard-hit handlers invoke `FUN_1407ebec8(opponent)`.

| Site | RVA | Expected Bytes | Caller |
|------|-----|----------------|--------|
| ebec8_a | `0x1407AEFB1` | `E8 12 CF 03 00` | `FUN_1407aebf8` |
| ebec8_b | `0x1407AF59D` | `E8 26 C9 03 00` | `FUN_1407af4c4` |

NOPping internal `FUN_140764210` calls alone wasn't sufficient because the `vtable+0x18` dispatch inside `FUN_1407ebec8` also corrupts state. NOPping the entire `FUN_1407ebec8` call at both caller sites eliminates all corruption paths through this function.

These sites also use soft-fail (skip on mismatch).

### Why Three Tiers?

The tiers are defense-in-depth:
- **Tier 1** eliminates the global race condition (Mechanism A)
- **Tier 2** eliminates direct `FUN_140764210` mis-targeting in handler internals
- **Tier 3** eliminates the `FUN_1407ebec8` entry points entirely (covers both the vtable dispatch and any future internal calls added by game updates)

All three tiers are needed because the bug manifests through multiple independent code paths. Testing showed that Tier 3 alone (the original single-tier fix) was insufficient — the vtable+0x18 dispatch inside `FUN_1407ebec8` could still corrupt state even when `FUN_140764210` was blocked internally.

## 3. In-Game Reproduction Recipe

### Setup
- **Mode:** Local 2P (pad 0 + pad 1) or CPU vs CPU with scripted inputs
- **Characters:** Any two characters with running jutsu (e.g., Naruto: Rasengan, Sasuke: Chidori)
- **Stage:** Any stage with flat ground for easy positioning

### Steps (2P Scriptable)
1. Start VS battle, both players at neutral distance (~3-4 body widths apart)
2. **Pad 1 (P2):** Hold Y (guard) and remain stationary
3. **Pad 0 (P1):** Input running jutsu command (directional + attack combo, e.g., →→+B for Rasengan dash)
4. **While P1 is mid-dash:** Throw a kunai at P2's guard using Pad 0 item slot (typically LB or RB depending on config), OR have a third-party projectile source (support character, trap item)
5. **Observe:** P1's Rasengan dash ends abruptly before reaching P2, even though the kunai hit P2's guard, not P1

### Timing Window
The bug occurs during the ~10-15 frame window when:
- The defender is in guard state (`player+0x15504 == 6`)
- The attacker has an active running jutsu (action type `0xe3` at sub-object+0x198)
- A projectile collision triggers the guard-hit handler

## 4. Memory Values for Verification

### Action State Field
- **Address:** `player_base + 0x8e4`
- **Type:** `int32`
- **Meaning:** Current action state ID. During a running jutsu, this holds the jutsu-specific action ID. When the bug fires, this value changes to a guard-context value.

### Global Variable
- **Address:** `DAT_141666ebc` (RVA `0x1266EBC`)
- **Type:** `float` (stored via SSE `movss`)
- **Normal value:** `0xff7fffff` (sentinel / invalid marker)
- **During guard-hit:** Briefly set to defending player's ID (`param_1[0x13]` cast to float)

### Supporting Addresses
| Offset | Purpose |
|--------|---------|
| `+0x13` | Player ID (used in global state write) |
| `+0x41` | Character/model object pointer |
| `+0x8e4` | Action state (set by `FUN_140764210`) |
| `+0xc8c` | Current action ID |
| `+0xd88` | Vtable offset for sub-object accessor |
| `+0x15504` | Guard state flag (6 = actively guarding) |
| sub+`0x198` | Action type (`0xe3` = running jutsu variant) |

## 5. Side Effects of NOP Patch

### Legitimate Behavior Lost
Based on Ghidra xref analysis, `FUN_1407ebec8` is ONLY called from these two guard-hit sites (plus one DATA reference at `0x148DAEA90`). There are no other callers in legitimate gameplay paths.

Potential edge cases:
1. **Guard-break counter-play:** If the game intended for hitting a guard to interrupt the attacker's follow-up pressure, removing this call eliminates that mechanic. Community reports suggest this was never intentional behavior.
2. **Specific jutsu interactions:** Some jutsu may have been designed to cancel on guard contact. No evidence of this in decompiled code or community knowledge.
3. **AI decision-making:** CPU AI may factor in this cancellation for combo routing. Removing it could subtly change CPU behavior in guard-heavy matchups.

### Determinism Concern
The existing Tier-3-only NOP patch produced 128 gameplay mismatches in replay playback testing, indicating it changes deterministic battle outcomes. The three-tier fix may produce different determinism characteristics since it blocks additional code paths.

**Recommendation:** Test with fresh baseline recording (guardfix OFF) vs playback (guardfix ON, all three tiers) to isolate whether mismatches are from the fix itself or from test methodology.

## 6. Fix Status

### Implementation
Branch `fix/guardfix` implements all three tiers in `storm4perf_guardfix.cpp`, included from root `storm4perf.cpp`. Byte patterns verified against NSUNS4 Steam 1.09 (`NSUNS4_release.exe-75912b`, sha256 `147e52a3...bc257`).

Config key `[fixes] guardfix=1` added to `storm4perf.ini` (default on).

### Patch Summary
| Tier | Sites | Patch Size | Fail Mode |
|------|-------|------------|-----------|
| 1 | 2 (write + reset) | 8 bytes each | Hard fail (return false) |
| 2 | 6 (call1–call6) | 5 bytes each | Soft fail (skip + log) |
| 3 | 2 (ebec8_a + ebec8_b) | 5 bytes each | Soft fail (skip + log) |

Total: 10 NOP sites, 56 bytes patched.

### Open Questions
1. Does the full three-tier NOP break any legitimate guard-break mechanics? (Needs focused in-game testing)
2. Can we narrow the patch to only skip calls when `param_1` is truly the wrong target? (Current analysis suggests all calls are cross-player, but edge cases may exist)
3. Why did replay determinism fail with Tier 3 alone? (Needs clean A/B test with proper baseline)
4. Does the three-tier fix improve or worsen replay determinism compared to Tier 3 alone?

### Next Steps
1. Build storm4perf DLL with updated `storm4perf_guardfix.cpp`
2. Deploy to test instance (`F:\dev\agent-<name>\game`)
3. Reproduce the bug using the recipe above (Section 3) with guardfix OFF
4. Enable guardfix and verify the bug is gone
5. Test guard functionality (block stun, chakra drain, guard break) still works normally
6. Run replay determinism test: record OFF, play back ON, check for IDENTICAL verdict