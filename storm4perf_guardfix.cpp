// storm4perf_guardfix.cpp: fix for guard-hit cancelling other players' jutsu.
// Root cause: FUN_1407ebec8 (jutsu cancel check) is called on the opponent during
// guard-hit processing in FUN_1407aebf8 and FUN_1407af4c4. This function calls
// FUN_140764210 which sets action state at +0x8e4, prematurely ending active jutsu.
// The call should only happen when the defender is actually breaking guard, not on
// every guard-hit event. Fix: NOP the two CALL instructions to FUN_1407ebec8 in the
// guard-hit path so jutsu cancellation only occurs through legitimate guard-break paths.
//
// Sites (NSUNS4 Steam 1.09, NSUNS4.exe sha256 147e52a3...bc257):
// 0x1407AEFB1: E8 12 CF 03 00 (call FUN_1407ebec8 from FUN_1407aebf8)
// 0x1407AF59D: E8 26 C9 03 00 (call FUN_1407ebec8 from FUN_1407af4c4)
//
// Config: [fixes] guardfix=1 (default on)

static bool install_guardfix() {
    // Site 1: FUN_1407aebf8 -> FUN_1407ebec8 at RVA 0x7AEFB1
    static const uint8_t site1_expect[] = { 0xE8, 0x12, 0xCF, 0x03, 0x00 };
    static const uint8_t site1_patch[] = { 0x90, 0x90, 0x90, 0x90, 0x90 }; // NOP sled
    if (!check_bytes("guardfix", 0x1407AEFB1ULL, site1_expect, sizeof(site1_expect))) return false;

    // Site 2: FUN_1407af4c4 -> FUN_1407ebec8 at RVA 0x7AF59D
    static const uint8_t site2_expect[] = { 0xE8, 0x26, 0xC9, 0x03, 0x00 };
    static const uint8_t site2_patch[] = { 0x90, 0x90, 0x90, 0x90, 0x90 }; // NOP sled
    if (!check_bytes("guardfix", 0x1407AF59DULL, site2_expect, sizeof(site2_expect))) return false;

    // Both sites verified; apply patches using VA() to handle ASLR
    if (!write_bytes("guardfix", VA(0x1407AEFB1), site1_patch, sizeof(site1_patch))) return false;
    if (!write_bytes("guardfix", VA(0x1407AF59D), site2_patch, sizeof(site2_patch))) return false;

    s4log("OK guardfix: NOP'd FUN_1407ebec8 calls at 0x1407AEFB1 and 0x1407AF59D");
    return true;
}