// SPDX-License-Identifier: Apache-2.0
// LOOP-004: the camera finalize steps tweens, shakes and battle RNG stream 9 on every draw, so at 60 fps
// they run 2x. Fix: roll those spans back after each render-only invocation.
// Details: docs/fps60/rows/LOOP-004.md
#include "../fps60.h"

static bool install() {
    uint8_t* tween = sym_rt("cam_tween_slots");
    if (!tween) { log_line("  LOOP-004: cam_tween_slots unresolved - refused"); return false; }
    uint8_t* block = tween - 0xDC;
    // Btl_Rand(stream): loads, multiplies and stores the per-stream seed table entry
    uint8_t* rng = sig_find_unique("LOOP-004 Btl_Rand stream seeds (FUN_007988f0)",
        "55 8B EC 8B 55 08 8B 0C 95 ?? ?? ?? ?? 0F AF 0C 95 ?? ?? ?? ?? 0F B7 04 55", 0x7988F0);
    if (!rng) return false;
    uint8_t* seeds = (uint8_t*)read_abs(rng + 0x11);
    // The seed store (+0x29) must use the same table base as the load.
    if (rng[0x29] != 0x89 || rng[0x2A] != 0x04 || rng[0x2B] != 0x95 || *(uint32_t*)(rng + 0x11) != *(uint32_t*)(rng + 0x2C)) {
        log_line("  LOOP-004: seed load/store bases differ - refused");
        return false;
    }
    log_line("  LOOP-004: viewport blocks gva=0x%08X (expected 0x01137000), stream 9 gva=0x%08X (expected 0x01135F04)",
             rt_to_gva_exe(block), rt_to_gva_exe(seeds + 9 * 4));
    return rollback_add_static("LOOP-004", block, 3 * 0x1C0) &&
           rollback_add_static("LOOP-004", seeds + 9 * 4, 4);
}

Row row_LOOP004 = {
    "LOOP-004",
    "viewport camera tween/shake blocks + battle RNG stream 9 rolled back on render-only invocations",
    1, install, nullptr, nullptr, 0
};
