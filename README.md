# Storm 4 Bugfix Project

Fixes for two community-reported bugs in Naruto Shippuden: Ultimate Ninja Storm 4 (PC, Steam 1.09).

## Test Instance

- **Location:** `C:\dev\agent-bugfix\game` on henry-desktop
- **Config:** 960×540 windowed, 30 FPS, vsync off
- **Patch:** storm4perf v1.2.0 (`xinput9_1_0.dll`)
- **Assets:** junctions to Steam install (data/dlc)

## Bugs

### Bug 1: Guard-hit cancels active jutsu

**Report:** A kunai projectile or item hitting a player's guard can cause other players' running jutsu (e.g. Rasengan) to prematurely end.

**Hypothesis:** The guard-hit handler may be broadcasting a global "interrupt" or "hit confirm" event that doesn't properly scope to the defending player, causing unrelated active jutsu state machines on other players to reset.

**RE targets:**
- Projectile/guard collision handler
- Jutsu state machine transition logic
- Multiplayer event dispatch for hit confirmation

### Bug 2: Fast input drops jutsu / chakra dash

**Report:** Inputting a jutsu or chakra dash too fast can cause the action to not happen even though chakra was consumed.

**Hypothesis:** The input buffer or command recognition window may be too tight, or there's a race between chakra deduction and action commitment where the action gets cancelled but the resource spend isn't refunded. Could also be an animation cancel window issue where a new input overrides a pending action before it fully starts.

**RE targets:**
- Input buffer / command queue processing
- Chakra consumption vs action commit ordering
- Animation state machine transition guards
- Jutsu activation sequence (charge → commit → execute)

## Approach

1. Reproduce each bug in the test instance
2. Identify relevant functions via Ghidra (NSUNS4 1.09 release + 1.06 debug)
3. Patch via storm4perf proxy DLL or standalone patch
4. Verify fix at 30 FPS

## References

- Storm KB: `storm-kb/memory/storm4-performance-research.md`
- Storm KB: `storm-kb/memory/storm4-test-instance-setup.md`
- Ghidra project: Switch Trilogy `main`, NSUNS4 release 1.09 + debug 1.06
- storm4perf repo: https://github.com/exavadw/storm4perf