# Bug Analysis: Guard-Hit Event Global Broadcast

## Summary

When a kunai projectile or item hits a player's guard in NSUNS4 multiplayer, OTHER players' active jutsu (e.g., Rasengan) can prematurely end. The root cause appears to be in how the guard-hit event is dispatched - it uses a global state variable (`DAT_141666ebc`) instead of being properly scoped to the defending player.

## Key Functions Identified

### 1. Guard Effect Handler - `FUN_1407afadc` (RVA: 0x7AFADC)
**Source file:** `ccPlayerActionGuard.cpp` (line 0x2E9 = 745)

This function handles guard visual effects and is called when a player successfully blocks an attack. It references the string `"wskn_e_guard00"` for guard sound/effect lookup.

**Key behavior:**
- Takes `param_1` (player object pointer) and `param_2` (guard type: 0=start, 1=loop, 2=end)
- Reads action ID from `*(int *)(param_1 + 0xc8c)` to determine character-specific guard effects
- Creates effect objects via `FUN_140a017f0()` with source file reference
- Calls `FUN_1407b5714()` to spawn the guard effect

### 2. Guard State Machine - `FUN_1407aebf8` (RVA: 0x7AEBF8)

Main guard action handler that processes guard transitions. Called with:
- `param_2 == 0`: Guard start
- `param_2 == 1`: Guard loop/hold  
- `param_2 == 2`: Guard end/break

**Critical code path for bug:**
```c
// At RVA 0x7AED61, 0x7AED9A, 0x7AF37A, 0x7AF49E
FUN_1407afadc(param_1, uVar12);  // Calls guard effect handler
```

### 3. Hit Reaction Handler - `FUN_1403f4a6c` (RVA: 0x3F4A6C)

Handles hit reactions including guard impacts. This is where projectile/item hits are processed.

**Key observations:**
- At offset `+0xfb7c` stores a player/action identifier
- Calls `FUN_14077e64c()` which triggers state transitions
- Uses virtual dispatch table at `*param_1 + 0xf10` for final state change

### 4. State Transition Dispatcher - `FUN_14077e64c` (RVA: 0x77E64C)

**THIS IS THE LIKELY BUG LOCATION.**

```c
int FUN_14077e64c(longlong *param_1)
{
  // ...
  if (iVar2 != 0) {
    DAT_141666ebc = (undefined4)param_1[0x13];  // <-- GLOBAL STATE WRITE
    lVar1 = *(longlong *)param_1[0x41];
    (**(code **)(lVar1 + 0x18))((longlong *)param_1[0x41],1,1,lVar1,in_stack_00000028);
    FUN_140557220(param_1[0x41]);
    DAT_141666ebc = 0xff7fffff;  // <-- GLOBAL STATE RESET
    FUN_14077c6d0(param_1);
  }
  return iVar2;
}
```

**Bug pattern identified:**
- `DAT_141666ebc` (RVA: 0x1266EBC) is a **global variable** used during state transitions
- When a guard-hit occurs, this global is set to the player's ID (`param_1[0x13]`)
- Other systems reading this global during the transition window may incorrectly associate the event with themselves
- The reset value `0xff7fffff` suggests this is meant as a sentinel/invalid marker

### 5. Jutsu Cancellation Check - `FUN_1407ebec8` (RVA: 0x7EBEC8)

Called from `FUN_1407aebf8` during guard processing. This function checks for and cancels active jutsu:

```c
undefined8 FUN_1407ebec8(longlong *param_1)
{
  plVar2 = (longlong *)(**(code **)(*param_1 + 0xd88))();  // Get opponent/target
  if (plVar2 != NULL) {
    if (*(int *)(param_1 + 0x15504) == 6) {
      // Loop checking for jutsu state 0xe3
      do {
        plVar3 = FUN_14079bda4(plVar2, iVar4);
        if (plVar3 != NULL && (int)plVar3[0x198] == 0xe3) {
          uVar1 = (**(code **)(*plVar3 + 0x18))(plVar3);
          FUN_140764210(param_1, uVar1);  // Cancel/interrupt jutsu
          return 1;
        }
      } while (iVar4 < 2);
    }
    // Fallback: cancel any active jutsu on target
    uVar1 = (**(code **)(*plVar2 + 0x18))(plVar2);
    FUN_140764210(param_1, uVar1);
  }
  return 0;
}
```

**Problem:** This function gets the target via virtual call at `*param_1 + 0xd88`, but if the global state `DAT_141666ebc` was polluted by another player's guard-hit, the wrong target may be returned.

### 6. Action State Setter - `FUN_140764210` (RVA: 0x764210)

Simple function that sets action state on a player object:
```c
void FUN_140764210(longlong *param_1, int param_2)
{
  iVar1 = (**(code **)(*param_1 + 0x18))();
  if (iVar1 != param_2) {
    *(int *)(param_1 + 0x8e4) = param_2;  // Set new action state
  }
}
```

Offset `+0x8e4` is the action state field. If this is called on the wrong player object due to global state pollution, their jutsu ends prematurely.

## Root Cause Analysis

The scoping bug occurs because:

1. **Global state variable `DAT_141666ebc`** at RVA `0x1266EBC` is used as temporary storage during guard-hit processing
2. When Player A's guard is hit by a kunai, `FUN_14077e64c` writes Player A's ID to this global
3. During the brief window before the global is reset to `0xff7fffff`, if Player B is executing a jutsu that checks this global (directly or indirectly through `FUN_1407ebec8`), Player B's jutsu state machine may read stale/wrong data
4. The jutsu cancellation logic in `FUN_1407ebec8` then incorrectly targets Player B's own jutsu for termination

## Patch Targets

### Primary Fix Location
**RVA: 0x77E64C** (`FUN_14077e64c`)

The function should pass the player ID through the call chain rather than using a global. Potential patch approaches:

1. **Thread-local storage**: Move `DAT_141666ebc` to TLS so each thread has its own copy
2. **Parameter passing**: Modify `FUN_14077e64c` signature to accept and forward player ID explicitly
3. **Atomic operation**: Use atomic compare-and-swap to prevent race conditions on the global

### Secondary Verification Points
- **RVA: 0x7EBEC8** (`FUN_1407ebec8`): Add player ID validation before calling `FUN_140764210`
- **RVA: 0x764210** (`FUN_140764210`): Validate that `param_1` matches expected player before modifying state at `+0x8e4`

## Data Structures

### Player Object Offsets (from decompiled code)
| Offset | Purpose |
|--------|---------|
| `+0x13` | Player ID (used in global state) |
| `+0x41` | Character/model object pointer |
| `+0x154` | Float field (possibly damage multiplier) |
| `+0x194` | Guard stance flag |
| `+0x198` | Current action sub-state |
| `+0x199` | Action category |
| `+0x8e4` | Action state (set by `FUN_140764210`) |
| `+0xc8c` | Current action ID |
| `+0xc94` | Action sub-index |
| `+0x15504` | Jutsu state counter |
| `+0x2949` | Active guard effect object |
| `+0x294a` | Guard effect timer |
| `+0x29c3` | Guard-related flag |

### Global Variables
| RVA | Purpose |
|-----|---------|
| `0x1266EBC` | `DAT_141666ebc` - Temporary player ID during state transitions (BUG SOURCE) |
| `0x12663C8+0x942` | Frame timing divisor (30 FPS base) |

## Cross-Reference Map

```
FUN_1407afadc (Guard Effects)
    ^
    |-- FUN_1407aebf8 (Guard State Machine) [4 callsites]
    |-- FUN_1403f4a6c (Hit Reaction) [2 callsites]
    |-- FUN_1407af4c4 [2 callsites]

FUN_14077e64c (State Transition - BUG HERE)
    ^
    |-- FUN_1407aebf8
    |-- FUN_1403f4a6c

FUN_1407ebec8 (Jutsu Cancel Check)
    ^
    |-- FUN_1407aebf8 (during guard break)
```

## Recommendations for Further Investigation

1. **Trace `DAT_141666ebc` readers**: Search for all functions that read this global to identify all potential victims of the race condition
2. **Check network sync**: Verify if guard-hit events are properly tagged with player ID in network packets
3. **Test with debug binary**: The debug 1.06 binary (`NSUNS4_debug_1.06.exe-bc53b3`) may have symbol names that clarify the intended scoping
4. **Frame timing**: The bug may be more reproducible at certain frame rates due to the race window size

## Files Analyzed
- Binary: `NSUNS4_release.exe-75912b` (Steam 1.09 release)
- Ghidra project path: `/home/hbarorhp/ghidra/binaries/pc/NSUNS4_release.exe`