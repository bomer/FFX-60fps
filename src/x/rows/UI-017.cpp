// SPDX-License-Identifier: Apache-2.0
// UI-017: the pad latch runs every invocation and takes press edges against the previous sample, so a
// press that lands on a render-only frame is lost. Fix: latch only on logic invocations.
// Details: docs/fps60/rows/UI-017.md
#include "../fps60.h"

typedef void(__cdecl* latch_t)(void);
static latch_t g_latch = nullptr;
static uint8_t* g_sites[2] = {};
static LONG g_skipped = 0, g_passed = 0;

static void __cdecl h_latch() {
    if (row_UI017.on && !g_disabled && pacing_kind() == INV_BUILD) { InterlockedIncrement(&g_skipped); return; }
    InterlockedIncrement(&g_passed);
    g_latch();
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_skipped > 0) {
        reported = 1;
        log_line("UI-017: first pad latch skipped on a render-only invocation (passed %ld)", g_passed);
    }
}

static bool install() {
    // Sg_WaitFrameSync: advance the vblank counters, then CALL the pad latch
    uint8_t* a = sig_find_unique("UI-017 pad latch (Sg_WaitFrameSync)",
        "85 C0 74 0E 83 05 ?? ?? ?? ?? 02 83 05 ?? ?? ?? ?? 02 E8 ?? ?? ?? ?? 80 3D", 0x82220D);
    // Sg_MainLoop: CALL Sg_WaitFrameSync, then the pause path's own CALL to the pad latch
    uint8_t* b = sig_find_unique("UI-017 pad latch (Sg_MainLoop pause path)",
        "E8 ?? ?? ?? ?? 83 C4 04 EB 05 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 F8 01", 0x820B99);
    if (!a || !b) return false;
    g_sites[0] = a + 0x12;                       // the two latch CALLs
    g_sites[1] = b + 0x0A;
    uint8_t* fn = call_target(g_sites[0]);
    if (!fn || call_target(g_sites[1]) != fn) {
        log_line("  UI-017: the two latch CALLs do not share a target - refused");
        return false;
    }
    g_latch = (latch_t)fn;
    for (int i = 0; i < 2; i++) {
        if (!patch_call_rel32(g_sites[i], fn, (void*)&h_latch)) {
            if (i == 1) patch_call_rel32(g_sites[0], (void*)&h_latch, fn);
            log_line("  UI-017: patch %d failed - refused", i);
            return false;
        }
    }
    log_line("  UI-017: pad latch gva=0x%08X (expected 0x008893E0), CALLs gva=0x%08X/0x%08X -> stub: skipped on render-only invocations",
             rt_to_gva_exe(fn), rt_to_gva_exe(g_sites[0]), rt_to_gva_exe(g_sites[1]));
    return true;
}

Row row_UI017 = {
    "UI-017",
    "gameplay pad latch (FUN_008893e0) only on logic invocations: presses are not lost on render-only frames (K4)",
    1, install, nullptr, post, 0
};
