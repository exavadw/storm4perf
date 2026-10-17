# storm4perf v1.4.0-guardfix — Bug 1 Fix Release

**Date:** 2026-10-17
**Target:** NSUNS4 PC Steam 1.09 (`NSUNS4_release.exe`, SHA256 `147E52A3E0D7382501A6777E158B37908E0B69D4E902079DC6E62DCAE69BC257`)

## What this fixes

**Bug 1: Guard-hit cancels other players' active jutsu**

When a kunai, shuriken, or item projectile hits a player who is guarding, any *other* player currently performing a jutsu (e.g., Rasengan, Chidori) has that jutsu prematurely cancelled. This affects all multiplayer modes and is especially disruptive in team battles.

### Root cause

`FUN_14077e64c` (RVA `0x77E64C`) uses the global `DAT_141666ebc` (RVA `0x1266EBC`) as scratch storage for the "hit player ID" during guard-effect processing. Two `movss` instructions write to and reset this global without any synchronization:

- **Write:** RVA `0x77E6B6` — stores the hit-target player ID into the shared global
- **Reset:** RVA `0x77E6E6` — clears the global back to zero

Because these are SSE float stores (`movss`) operating on a shared global with no locking, concurrent access from another thread processing jutsu state reads a stale or incorrect player ID, causing the jutsu-cancel check in `FUN_1407ebec8` to fire spuriously.

### Patch

Both `movss` instructions are NOPed (8 bytes each), eliminating the shared-global side effect. The guard-hit logic still functions correctly because the player ID is already available in a register; the global store was redundant debug/state-tracking code.

## Installation

1. Copy `xinput9_1_0.dll` to your NSUNS4 game directory (next to `NSUNS4.exe`)
2. Ensure `storm4perf.ini` has `guardfix=1` (enabled by default)
3. Launch the game normally

## Verification

After launch, check `storm4perf.log` for:
```
[guardfix] OK: patched RVA 0x77E6B6 (8 bytes)
[guardfix] OK: patched RVA 0x77E6E6 (8 bytes)
```

## Checksums

```
SHA256: 934e69e650315a5db0a9e0bbe8714afe309cab796d26944b54cc5f725964203a  xinput9_1_0.dll
```

## Other patches included

| Patch | Default | Description |
|-------|---------|-------------|
| `gputimer` | off | GPU timer workaround |
| `skinwait` | off | Skin loading wait fix |
| `flipwait` | off | Flip model wait fix |
| `timeraffinity` | off | Timer affinity fix |
| `vsyncsleep` | off | VSync sleep optimization |
| `bulletunpin` | off | Bullet unpin fix |
| `presentpace` | off | Present pacing |
| **`guardfix`** | **on** | **This release's fix** |
| `audiocrash` | on | Audio device unplug crash prevention |
| `inputfix` | on | Input buffer persistence (Bug 2 — **not yet working**) |

## Source

Built from commit `577402e` on the `feature/support-health` branch of the storm4-bugfix repository.

## Known issues

- `inputfix` (Bug 2) is enabled by default but currently fails to locate its target pattern in the release binary. See `re-bug2-input-drop.md` for ongoing RE status.