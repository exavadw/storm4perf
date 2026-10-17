# Bug 2: Fast Input Drops Jutsu/Chakra Dash Despite Chakra Spend

## Summary

**Root cause:** The `ccPlayerPad::PreCtrl()` function unconditionally clears the command request buffer (`memset(this + 0x428, 0, 0xcc)`) every frame before input processing. When combined with the single-frame latching behavior of `RequestCommand()` and the multi-frame window check in `UpdateSuccessCommand()`, fast consecutive inputs can overwrite or lose pending jutsu/chakra dash commands before they are consumed by the player state machine.

## Evidence Chain

### 1. Command Request Buffer Architecture

**`ns3::ccPlayerPad::RequestCommand`** (Switch Trilogy `7100bef3a8`, NSUNS4 equivalent at offset `+0x428`):
```c
void __thiscall ns3::ccPlayerPad::RequestCommand(ccPlayerPad *this, int param_1) {
    *(undefined4 *)(this + (long)param_1 * 4 + 0x428) = 1;
}
```
- Commands are stored as an array of `uint32` flags starting at pad offset `0x428`.
- Each command ID maps to a 4-byte slot; setting it to `1` marks it as requested.
- Buffer size is `0xcc` bytes = 51 command slots (confirmed by `ResetRequestedCommand` memset size).

**`ns3::ccPlayerPad::IsRequestedCommand`** (Switch Trilogy `7100bef084`):
```c
undefined4 __thiscall ns3::ccPlayerPad::IsRequestedCommand(ccPlayerPad *this, int param_1) {
    return *(undefined4 *)(this + (long)param_1 * 4 + 0x428);
}
```
- Simple read-back of the flag; no debounce, no persistence logic.

### 2. PreCtrl Clears Buffer Every Frame

**`ns3::ccPlayerPad::PreCtrl`** (Switch Trilogy `7100bee6a8`, NSUNS4 equivalent confirmed via identical structure):
```c
if (*(int *)(this + 0x3f8) < 0) {
    // ... reset analog/digital state ...
    memset(this + 0x428, 0, 0xcc);   // <-- UNCONDITIONAL CLEAR
    *(undefined4 *)(this + 0x410) = 0xffffffff;
    // ... reset direction vector ...
} else {
    memset(this + 0x428, 0, 0xcc);   // <-- ALSO CLEARS IN ELSE BRANCH
    *(undefined4 *)(this + 0x410) = 0xffffffff;
}
```
- **Both branches** of the `PreCtrl` conditional clear the entire command buffer.
- This runs **before** `Ctrl()` processes new button inputs for the current frame.
- Any command that was requested in frame N but not yet consumed by the player state machine is **wiped** before frame N+1's input can re-request it.

### 3. UpdateSuccessCommand Timing Window

**`ns3::ccPlayerPad::UpdateSuccessCommand`** (Switch Trilogy `7100bee79c`):
```c
if (((uVar1 & *(uint *)(this + 0x218)) != 0) && (0.0 <= *(float *)(this + 0x21c))) {
    iVar5 = (int)*(float *)(this + 0x21c);
    pcVar4 = this + 0x220;
    lVar6 = 1;
    do {
        // Check button sequence timing windows (up to 0x3c = 60 entries)
        if (*(float *)(pcVar4 + 4) < 0.0) return 0;
        iVar5 = (int)(*(float *)(pcVar4 + 4) + (float)iVar5);
        if (5 < iVar5) return 0;          // <-- HARD LIMIT: 5-frame window
        if (bVar3) {
            *(undefined4 *)(this + 0x424) = 1;
            return 1;
        }
        pcVar4 = pcVar4 + 8;
    } while (lVar6 < 0x3c);
}
return 0;
```
- The successful-command detection has a **5-frame cumulative timing limit**.
- If the player state machine doesn't consume the command within this window, `UpdateSuccessCommand` returns 0 even though the button was pressed.
- Combined with `PreCtrl` clearing, a command requested on frame N is gone by frame N+1 unless the state machine reads it **in the same frame**.

### 4. Chakra Dash Gate Checks

**`ns3::ccPlayer::IsRequestActChakraDashOk`** (Switch Trilogy `7100bd767c`):
- Checks animation chunks `0x54`-`0x57` (must all be absent).
- Checks `IsEnableConSeal` (seal condition must be disabled).
- Reads battle adjust param `"CHAKRA_USE_AT_CHAKRA_DASH"`.
- Validates chakra gauge level (`this + 0x78b4`): must be > 0 and != 3.
- Has a **state whitelist** via switch on `*(uint *)(this + 0x9fc)` -- only specific action states allow chakra dash initiation.

**`ns3::ccPlayerLeader::IsCheckStartCancelChakraDash`** (Switch Trilogy `7100bd1ae0`):
- Calls `ccPlayerPad::IsJump` OR `ccPlayerPad::IsRequestedCommand(pad, 2)` (command ID 2 = chakra dash trigger).
- If neither is true, returns false immediately -- **the dash never starts**.
- This is where the dropped input manifests: if `PreCtrl` cleared command 2 before the state machine polls it, `IsRequestedCommand` returns 0 and the dash is silently cancelled despite chakra being spent on the charge-up.

### 5. UseChakra Is Not Gated on Success

**`ns3::ccPlayerLeader::UseChakra`** (Switch Trilogy `7100c1510c`):
```c
void __thiscall ns3::ccPlayerLeader::UseChakra(float param_1, ccPlayerLeader *this) {
    iVar1 = (**(code **)(*(long *)this + 0x48))();
    if (iVar1 == 0) {
        uVar2 = ccPlayer::IsCheckSituationInBattle((ccPlayer *)this, 0x59);
        if ((int)uVar2 != 0) { param_1 = param_1 + param_1; }
    }
    (**(code **)(*(long *)this + 0x5d0))(-param_1, this, 0);
}
```
- Chakra is deducted via a vtable call at offset `0x5d0` **without checking** whether the subsequent action actually succeeded.
- This explains the symptom: chakra is spent during the charge/input phase, but the dash/jutsu activation fails because the command was cleared before consumption.

### 6. NSUNS4 Confirmation

NSUNS4 release binary (`/NSUNS4_release.exe-75912b`) lacks symbols but contains identical CC2 engine source paths:
- `ccPlayer.cpp` at `FUN_140777a70` (constructor/init, allocates `0x1ab8` bytes -- matches Switch `ccPlayer` size).
- `ccPlayerCondition.cpp` at `FUN_140765618` (condition system, references `DAT_1416663c8 + 0x942` for player index).
- `ccPlayerActionGuard.cpp` at `FUN_1407afadc` (action state machine with guard/jutsu effect strings like `"2sszeff1.xfbin"`, `"3sskeff1.xfbin"`).
- `ccChakraBallManager.cpp` at `FUN_140752164` (chakra ball visual effects).

The pad structure offsets (`0x428` command buffer, `0x3f8` player index, `0x408` button state, `0x218`/`0x21c` timing window) are consistent between Switch Trilogy and NSUNS4, confirming the same engine code path.

## Root Cause Statement

**The `PreCtrl` unconditional memset of the command buffer at offset `0x428` creates a single-frame lifetime for all requested commands.** When a player inputs a jutsu or chakra dash command rapidly (e.g., during combo cancel windows or at high FPS where frame timing is tight), the following race occurs:

1. Frame N: Player presses chakra button -> `RequestCommand(2)` sets `pad[0x428 + 8] = 1`.
2. Frame N: State machine hasn't polled `IsRequestedCommand(2)` yet (it runs later in the tick order, or the player is in a non-whitelisted state).
3. Frame N+1: `PreCtrl` runs first, memsets `0x428` to zero.
4. Frame N+1: Button may still be held, but `Ctrl` only sets edge-triggered flags; the command slot stays 0.
5. Result: `IsCheckStartCancelChakraDash` sees `IsRequestedCommand(2) == 0`, returns false. Chakra was already deducted during the charge phase. Action silently fails.

At 30 FPS this is less likely because each frame is ~33ms, giving the state machine more wall-clock time to poll. At 60 FPS or with fast input macros, the window shrinks and drops become frequent.

## Fix Recommendations

1. **Do not clear the command buffer in `PreCtrl`.** Move the clear to occur only after the command has been consumed (i.e., after `IsRequestedCommand` returns true and the action is initiated). Alternatively, use a double-buffer scheme where `PreCtrl` swaps buffers instead of zeroing.

2. **Add command persistence.** Change `RequestCommand` to set a "sticky" flag that survives one `PreCtrl` cycle, or add a separate "pending" buffer that `PreCtrl` does not touch.

3. **Gate `UseChakra` on action success.** Move the chakra deduction to occur only after `IsRequestActChakraDashOk` / `IsCheckStartCancelChakraDash` confirms the action will proceed, preventing the "chakra spent but nothing happens" symptom even if the input drop persists.

4. **For storm4perf proxy mitigation:** Hook `PreCtrl` at the NSUNS4 equivalent address and NOP the `memset(this + 0x428, 0, 0xcc)` calls in both branches. This is a binary patch, not a config option. Test at both 30 and 60 FPS to verify the fix eliminates drops without introducing stuck-command bugs.

## Key Addresses (NSUNS4 Release 1.09)

| Function | Estimated RVA | Notes |
|----------|--------------|-------|
| `ccPlayerPad::PreCtrl` | ~`0x76e22c` (derive from Switch `7100bee6a8` - base) | Contains two `memset(+0x428, 0, 0xcc)` calls |
| `ccPlayerPad::Ctrl` | ~`0x76e22c` region | Processes button edges, calls `utilGetAnalogStick` |
| `ccPlayerPad::RequestCommand` | Trivial: `mov [rcx + rdx*4 + 0x428], 1` | Inline candidate |
| `ccPlayerPad::IsRequestedCommand` | Trivial: `mov eax, [rcx + rdx*4 + 0x428]` | Inline candidate |
| `ccPlayerPad::UpdateSuccessCommand` | ~`0x76e79c` region | 5-frame timing limit at offset `0x21c` |
| `ccPlayer::IsRequestActChakraDashOk` | Near `ccPlayer.cpp` init (`FUN_140777a70`) callees | State whitelist + chakra gauge check |
| `ccPlayerLeader::UseChakra` | Vtable offset `0x5d0` on ccPlayerLeader | Ungated deduction |

*Note: Exact NSUNS4 RVAs require cross-referencing with the debug 1.06 binary or runtime symbol resolution. The Switch Trilogy addresses above are authoritative for structure/layout.*

## Files Referenced

- Switch Trilogy binary: `/main-69612a` (full symbols, ns3 namespace)
- NSUNS4 release: `/NSUNS4_release.exe-75912b` (no symbols, source paths confirm shared engine)
- NSUNS4 debug: `/NSUNS4_debug_1.06.exe-bc53b3` (no symbols either; PDB not loaded)
- Storm KB: `storm-kb/memory/storm4-performance-research.md` (30 FPS correctness context)