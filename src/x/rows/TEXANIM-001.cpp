// SPDX-License-Identifier: Apache-2.0
// TEXANIM-001: the character texture-animation stepper (UV scroll / flipbook, e.g. the water on Brotherhood) is
// called from Ch_SkeletonJob on every invocation, so it runs 2x. Fix: step it only on logic invocations.
// Details: docs/fps60/rows/TEXANIM-001.md
#include "../fps60.h"

typedef void(__cdecl* texanim_t)(int, int, int);
static texanim_t g_step = nullptr;
static uint8_t* g_site = nullptr;
static LONG g_skipped = 0, g_passed = 0;

// Runs on the skeleton job worker threads; pacing_kind() is set by pre() on the game thread before the dispatch.
static void __cdecl h_step(int a, int b, int c) {
    if (row_TEXANIM001.on && !g_disabled && pacing_kind() == INV_BUILD) { InterlockedIncrement(&g_skipped); return; }
    InterlockedIncrement(&g_passed);
    g_step(a, b, c);
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_skipped > 0) {
        reported = 1;
        log_line("TEXANIM-001: first texture-anim step skipped on a render-only invocation (passed %ld)", g_passed);
    }
}

static bool install() {
    // Ch_SkeletonJob: PUSH [ESI+0x698]; PUSH [ESI-0x194]; PUSH [ESI+0x568]; CALL stepper; ADD ESP,0xC
    uint8_t* a = sig_find_unique("TEXANIM-001 texanim step (Ch_SkeletonJob)",
        "FF B6 98 06 00 00 FF B6 6C FE FF FF FF B6 68 05 00 00 E8 ?? ?? ?? ?? 83 C4 0C", 0x83361D);
    if (!a) return false;
    g_site = a + 0x12;
    uint8_t* fn = call_target(g_site);
    if (!fn) return false;
    g_step = (texanim_t)fn;
    if (!patch_call_rel32(g_site, fn, (void*)&h_step)) {
        log_line("  TEXANIM-001: patch failed - refused");
        return false;
    }
    log_line("  TEXANIM-001: texanim step gva=0x%08X (expected 0x0077C8D0), CALL gva=0x%08X -> stub: skipped on render-only invocations",
             rt_to_gva_exe(fn), rt_to_gva_exe(g_site));
    return true;
}

Row row_TEXANIM001 = {
    "TEXANIM-001",
    "character texture animation (FUN_0077c8d0: UV scroll/flipbook, e.g. Brotherhood water) only on logic invocations",
    1, install, nullptr, post, 0
};
