// SPDX-License-Identifier: Apache-2.0
// LOOP-005: the exec==0 fallback in Sg_MainLoop eases the camera again on render-only invocations, so the
// camera moves 2x. Fix: clear its gate flag for those invocations and restore it after.
// Details: docs/fps60/rows/LOOP-005.md
#include "../fps60.h"

static volatile uint8_t* g_flag = nullptr;
static uint8_t g_saved = 0;
static bool    g_cleared = false;
static volatile LONG g_skips = 0;

static bool install() {
    // Sg_MainLoop exec==0 fallback: exec check, then `if (flag) { spherical camera; battle camera }`
    uint8_t* at = sig_find_unique("LOOP-005 exec==0 camera fallback (Sg_MainLoop)",
        "E8 ?? ?? ?? ?? 85 C0 75 1C 50 E8 ?? ?? ?? ?? 83 C4 04 80 3D ?? ?? ?? ?? 00 74 0A E8 ?? ?? ?? ?? E8", 0x8210A2);
    if (!at) return false;
    if (call_target(at) != (uint8_t*)g_get_exec) {
        log_line("  LOOP-005: first CALL is not Sg_GetCurExecFrames - refused");
        return false;
    }
    g_flag = (volatile uint8_t*)read_abs(at + 0x14);
    log_line("  LOOP-005: fallback camera flag gva=0x%08X (expected 0x012FBBB0) = %u, camera calls gva=0x%08X / 0x%08X",
             rt_to_gva_exe((const void*)g_flag), *g_flag, rt_to_gva_exe(call_target(at + 0x1B)), rt_to_gva_exe(call_target(at + 0x20)));
    return true;
}

static void pre() {
    g_cleared = false;
    if (g_disabled || pacing_kind() != INV_BUILD) return;
    g_saved = *g_flag;
    if (!g_saved) return;
    *g_flag = 0;
    g_cleared = true;
    InterlockedIncrement(&g_skips);
}

static void post() {
    if (g_cleared && *g_flag == 0) *g_flag = g_saved;
    g_cleared = false;
    static LONG reported = 0;
    if (!reported && g_skips > 0) {
        reported = 1;
        log_line("LOOP-005: first render-only invocation with the fallback camera ease suppressed");
    }
}

Row row_LOOP005 = {
    "LOOP-005",
    "exec==0 camera fallback (FUN_0083f2e0 + battle camera FUN_007be110) suppressed on render-only invocations",
    1, install, pre, post, 0
};
