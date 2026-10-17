# Bug 2 RE Report v2: Fast Input Drops Jutsu/Chakra Dash Despite Chakra Spend

## Executive Summary

**Root cause confirmed**: `ccPlayerPad::PreCtrl` (`FUN_1407c0fd4`) unconditionally clears the command request buffer (`memset(this + 0x428, 0, 0x15c)`) every frame BEFORE input processing occurs. Combined with the single-frame latching behavior of `RequestCommand` and state machine guards that reject inputs during recovery frames, fast consecutive inputs are silently dropped even though chakra has already been deducted.

**Current patch is WRONG**: The existing NOP at VA `0x1407C0363` targets `FUN_1407c039c` (button sequence timing tracker), NOT the PreCtrl memset. This patch breaks combo/command sequence detection without fixing the input drop bug.

**Correct fix target**: NOP the two `memset` calls inside `FUN_1407c0fd4` at VAs `0x1407c100a` (if-branch) and `0x1407c102e` (else-branch), OR redirect them to clear only non-command regions.

---

## Detailed Analysis

### 1. Command Buffer Architecture (Verified)

**Buffer location**: `ccPlayerPad + 0x428`, size `0x15c` (348 bytes = 87 uint32 slots)

The prior analysis incorrectly stated the buffer size was `0xcc` (51 slots). The actual memset in NSUNS4 uses `0x15c`, covering a larger region that includes:
- Command request flags: `+0x428` to `+0x4F4` (estimated 51 slots based on Switch Trilogy)
- Success command flags: `+0x424` (cleared separately at start of `FUN_1407c039c`)
- Additional pad state through `+0x583`

**RequestCommand equivalent**: Not found as a standalone function in NSUNS4 release binary. Likely inlined as:
```c
*(uint32_t*)(pad + cmd_id * 4 + 0x428) = 1;
```
This matches the Switch Trilogy pattern at `7100bef3a8`.

**IsRequestedCommand equivalent**: Also inlined. Reads from same offset without debounce or persistence.

### 2. PreCtrl Clears Buffer Every Frame (Confirmed Root Cause)

**Function**: `FUN_1407c0fd4` (NSUNS4 RVA `0x7c0fd4`)

Decompiled code shows BOTH branches clear the command buffer:
```c
void FUN_1407c0fd4(longlong param_1) {
    if (*(int *)(param_1 + 0x3f8) < 0) {
        // Reset analog/digital state for disconnected pad
        *(undefined4 *)(param_1 + 0x404) = 0;
        *(undefined4 *)(param_1 + 0x408) = 0;
        *(undefined4 *)(param_1 + 0x40c) = 0;
        // ... reset stick vectors ...
        FUN_1407c039c(param_1);  // Reset button sequence tracker
        memset((void *)(param_1 + 0x428), 0, 0x15c);  // <-- CLEAR #1 @ 0x1407c100a
        *(undefined4 *)(param_1 + 0x410) = 0xffffffff;
        *(undefined4 *)(param_1 + 0x420) = 0;
        // ... copy timestamp ...
    } else {
        memset((void *)(param_1 + 0x428), 0, 0x15c);  // <-- CLEAR #2 @ 0x1407c102e
        *(undefined4 *)(param_1 + 0x410) = 0xffffffff;
    }
}
```

**Callers** (all call PreCtrl BEFORE Ctrl):
- `FUN_1404948dc`: Calls `PreCtrl(pad)` then `Ctrl(pad)` — main battle input path
- `FUN_140495004`: Same pattern — alternate battle input path
- `FUN_1407e10e0`: Player leader update — calls `PreCtrl(&this[0x43])` then conditionally `Ctrl`
- `FUN_1407e7738`: Support character update — calls `PreCtrl(this + 0x218)` then conditionally resets

**Execution order per frame**:
1. `PreCtrl` runs → **wipes all pending commands**
2. `Ctrl` runs → reads physical buttons, sets NEW commands via inlined RequestCommand
3. Player state machine polls `IsRequestedCommand` → only sees commands set in step 2

**The race condition**: If the player state machine is in a recovery/transition state during step 3 and doesn't consume the command, that command is lost forever when step 1 runs next frame. There is NO persistence mechanism.

### 3. Why "Fast" Inputs Are Dropped

The bug manifests when:
1. Player inputs jutsu/chakra dash command during recovery frames (e.g., after previous action)
2. Command is set in buffer during `Ctrl` (frame N)
3. State machine checks `IsRequestedCommand` but rejects it due to state guard (e.g., `IsRequestActChakraDashOk` whitelist at player offset `+0x9fc`)
4. Next frame (N+1): `PreCtrl` wipes the command before state machine can retry
5. Result: Command silently dropped, but chakra was already deducted

**State machine guards** (from Switch Trilogy cross-reference):
- `IsRequestActChakraDashOk`: Checks animation chunks `0x54-0x57`, seal condition, chakra gauge level, and has a **state whitelist** via switch on `player + 0x9fc`
- `IsCheckStartCancelChakraDash`: Requires `IsJump` OR `IsRequestedCommand(pad, 2)` — if command was cleared, returns false immediately

### 4. Chakra Deduction Is Ungated (Compounding Factor)

From the prior analysis (confirmed by code structure):
- `UseChakra` deducts chakra via vtable call at offset `0x5d0` BEFORE action confirmation
- No rollback mechanism exists if the action fails to initiate
- This explains "chakra spent but nothing happens" symptom

### 5. The Current Patch Is Wrong

**Patched address**: `0x1407C0363`

Bytes at this address: `e8 1a f2 37 00 83 8b 10 04 00 00 ff e8 3c 49 24 00...`

This is a `call` instruction (`e8 1a f2 37 00` = call to `0x140a3f582`) followed by float operations. This address is inside `FUN_1407c039c`, the button sequence timing tracker that maintains combo input windows.

**What FUN_1407c039c does**:
```c
void FUN_1407c039c(longlong param_1, int param_2) {
    *(undefined4 *)(param_1 + 0x424) = 0;  // Clear success flag
    if (param_2 == *(int *)(param_1 + 0x218)) {
        *(int *)(param_1 + 0x21c) += 1;  // Increment timing counter
    } else {
        // Shift timing window history (0x3b entries)
        // Reset counter and store new button mask
    }
    FUN_1407c1234();  // Check for successful command sequences
}
```

NOPing code here would break combo/command sequence detection (jutsu input patterns like ↓↘→+attack), NOT fix the single-button input drop bug.

### 6. UpdateSuccessCommand Timing Window

`FUN_1407c1234` (called from `FUN_1407c039c`) implements the command sequence checker with a **frame-based timing limit**:
```c
// Simplified from decompilation
if ((button_mask & stored_sequence[i]) != 0) {
    cumulative_frames += timing_window[i+1];
    if (cumulative_frames > threshold) return 0;  // Too slow
    if (sequence_complete) {
        *(pad + 0x424) = 1;  // Set success flag
        return 1;
    }
}
```

The threshold calculation uses `30.0 / fps_byte` scaling, confirming the game expects 30 FPS timing. At higher framerates without compensation, windows shrink proportionally.

---

## Fix Recommendations

### Option A: NOP Both Memset Calls in PreCtrl (Recommended)

**Target**: `FUN_1407c0fd4`

| Location | VA | Bytes | Action |
|----------|-----|-------|--------|
| Clear #1 (if-branch) | `0x1407c100a` | `e8 XX XX XX XX` (call memset) | NOP (5 bytes) or redirect |
| Clear #2 (else-branch) | `0x1407c102e` | `e8 XX XX XX XX` (call memset) | NOP (5 bytes) or redirect |

**Risk**: Commands may persist across frames, causing "stuck" inputs. Mitigation: Add explicit clear AFTER command consumption in the state machine (requires hooking `IsRequestedCommand` consumers).

### Option B: Reduce Memset Size to Preserve Commands

Change `0x15c` to a smaller value that clears only non-command fields. Requires identifying exact command buffer boundaries within the `+0x428` to `+0x583` region. Higher risk of side effects.

### Option C: Double-Buffer Scheme

Modify `PreCtrl` to swap between two command buffers instead of clearing. Most robust but requires significant code cave space or DLL injection.

### Option D: Gate Chakra Deduction on Action Success

Separate fix for the "chakra spent but nothing happens" symptom. Hook `UseChakra` to defer deduction until after `IsRequestActChakraDashOk` / `IsCheckStartCancelChakraDash` confirms the action will proceed. Does NOT fix the input drop itself but prevents resource waste.

**Recommendation**: Implement Option A + Option D together. Option A fixes the input drop; Option D prevents chakra waste for any remaining edge cases.

---

## Key Addresses (NSUNS4 Release 1.09, Binary NSUNS4_release.exe-75912b)

| Function | VA | Purpose |
|----------|-----|---------|
| `ccPlayerPad::PreCtrl` | `0x1407c0fd4` | Clears command buffer every frame (ROOT CAUSE) |
| `ccPlayerPad::Ctrl` | `0x1407c03f0` | Processes button inputs, sets commands |
| Button sequence tracker | `0x1407c039c` | Maintains combo timing windows (NOT the bug) |
| Command sequence checker | `0x1407c1234` | Validates jutsu input patterns |
| Pad reset/init | `0x1407c02dc` | Full pad initialization (larger memset 0x15c) |
| Player leader update | `0x1407e10e0` | Calls PreCtrl+Ctrl for leader character |
| Support char update | `0x1407e7738` | Calls PreCtrl+Ctrl for support character |
| Battle input path A | `0x1404948dc` | Calls PreCtrl then Ctrl |
| Battle input path B | `0x140495004` | Calls PreCtrl then Ctrl |

**WRONG PATCH ADDRESS**: `0x1407C0363` — inside button sequence tracker, NOT PreCtrl memset

---

## Confidence Level

**HIGH (90%)** for root cause identification:
- PreCtrl structure matches Switch Trilogy exactly
- Both branches clear the command buffer unconditionally
- Call graph confirms PreCtrl runs before Ctrl every frame
- No command persistence mechanism exists

**MEDIUM (70%)** for fix approach:
- NOPing memsets may cause stuck-command side effects
- Testing required at both 30 and 60 FPS
- May need additional clear-after-consume logic

**UNCERTAINTIES**:
- Exact RequestCommand/IsRequestedCommand inlining locations not identified (would require runtime debugging or debug symbols)
- State machine guard conditions not fully mapped in NSUNS4 binary
- Whether 0x15c memset covers additional critical state beyond commands

---

## Files Referenced

- Ghidra binary: `NSUNS4_release.exe-75912b`
- Prior analysis: `/home/hbarorhp/ghost-brain/workspace/storm4-bugfix/re-bug2-input-drop.md`
- Storm KB: `storm-kb/memory/storm4-performance-research.md` (30 FPS correctness context)
- Switch Trilogy binary: `/main-69612a` (full symbols, used for cross-reference)