// SPDX-License-Identifier: Apache-2.0
// X2-MAG-008 (port of FFX MAG-008): on render-only invocations Sg_MainLoop's exec==0 fallback runs the effect
// dispatch FUN_00616660(0) (FFX MsBattleScene_DrawDispatch): every queued effect / magic overlay updates again,
// so effects run 2x. The CALL at 0x60582D goes to a stub that skips it on render-only invocations.
// Evidence: outer repo re/ledger/PORT-A.md §1 (S), re/MAINLOOP.md §9.4.
#include "../x2.h"

namespace x2 {

typedef void(__cdecl* dispatch_t)(int);         // cdecl int, plain RET 0x61675D (PORT-A §1)

static dispatch_t g_dispatch = nullptr;
static volatile LONG g_skipped = 0, g_passed = 0;
extern Row row_MAG008;

static void __cdecl h_fallback_dispatch(int pass) {
    if (row_MAG008.on && !g_disabled && render_only()) { InterlockedIncrement(&g_skipped); return; }
    InterlockedIncrement(&g_passed);
    g_dispatch(pass);
}

static bool install() {
    // Sg_MainLoop: if exec==0 { FUN_00616660(0); if (flag) { camera pair } }
    uint8_t* at = sig_find("X2-MAG-008 exec==0 fallback",
        "E8 ?? ?? ?? ?? 85 C0 75 1C 50 E8 ?? ?? ?? ?? 83 C4 04 80 3D ?? ?? ?? ?? 00 74 0A E8 ?? ?? ?? ?? E8", 0x605823);
    if (!at) return false;
    if (!g_get_exec || call_target(at) != g_get_exec) {
        log_line("  X2-MAG-008: first CALL is not Sg_GetCurExecFrames - refused");
        return false;
    }
    uint8_t* site = at + 0x0A;
    uint8_t* target = call_target(site);
    if (!sig_match(target, "55 8B EC 51 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 53 8B 5D 08 8B C3 83 E8 00")) {
        log_line("  X2-MAG-008: CALL target %p is not the effect dispatch (prologue mismatch) - refused", target);
        return false;
    }
    g_dispatch = (dispatch_t)target;
    if (!patch_call_rel32(site, target, (void*)&h_fallback_dispatch)) {
        log_line("  X2-MAG-008: patch failed");
        return false;
    }
    log_line("  X2-MAG-008: CALL at gva=0x%08X -> stub (dispatch gva=0x%08X skipped on render-only invocations)",
             rt_to_gva(site), rt_to_gva(target));
    return true;
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_skipped > 0) {
        reported = 1;
        log_line("X2-MAG-008: first skip of the exec==0 dispatch (skipped=%ld passed=%ld)", g_skipped, g_passed);
    }
}

Row row_MAG008 = {
    "X2-MAG-008",
    "skip the exec==0 effect dispatch FUN_00616660(0) on render-only invocations",
    1, install, nullptr, post, 0
};

} // namespace x2
