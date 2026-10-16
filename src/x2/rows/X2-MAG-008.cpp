// SPDX-License-Identifier: Apache-2.0
// X2-MAG-008 (port of FFX MAG-008): on render-only invocations Sg_MainLoop's exec==0 fallback runs the effect
// dispatch FUN_00616660(0) (FFX MsBattleScene_DrawDispatch): every queued effect / magic overlay updates again,
// so effects run 2x. The CALL at 0x60582D goes to a stub that skips it on render-only invocations.
// Evidence: outer repo re/ledger/PORT-A.md §1 (S), re/MAINLOOP.md §9.4.
// X2-MAG-008-PASS (default 1; port of FFX MAG-008-PASS / the MAG-021 symptom): the 63 "cross fade" magic DLLs
// (dressphere change and others, magic_0313 .. 0990, string "kohno corss fade begin") draw parts of their effect only
// right after their update, which this dispatch runs: skipped on render-only invocations, the character model and
// effects were drawn every second frame (U 16/10/2026: transformation blink, gone with X2-MAG-008 off via F8). While
// such a DLL is loaded the dispatch runs on render-only invocations too; their update steps only when the exec getter
// (+0x400) is non-zero (S, magic_0646 FUN_10002b90 / FUN_10003fb0). Ledger: re/ledger/XFADE-BLINK.md.
#include "../x2.h"
#include <string.h>

namespace x2 {

typedef void(__cdecl* dispatch_t)(int);         // cdecl int, plain RET 0x61675D (PORT-A §1)

static dispatch_t g_dispatch = nullptr;
static volatile LONG g_skipped = 0, g_passed = 0, g_ro_passed = 0;
static int  g_pass_on = 1;                       // [rows] X2-MAG-008-PASS
static volatile LONG g_pass_loaded = 0;          // cross-fade DLLs loaded now
extern Row row_MAG008;

static bool has_xfade(const uint8_t* base, uint32_t size) {
    static const char k[] = "kohno corss fade begin";
    const uint32_t n = sizeof(k) - 1;
    __try {
        for (uint32_t i = 0; i + n <= size; i++)
            if (base[i] == 'k' && memcmp(base + i, k, n) == 0) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
static uint8_t* g_xbase[8];
static void on_load(uint8_t* base, uint32_t size, const char* name) {
    if (!has_xfade(base, size)) return;
    for (int i = 0; i < 8; i++)
        if (!g_xbase[i]) { g_xbase[i] = base; InterlockedIncrement(&g_pass_loaded); break; }
    log_line("X2-MAG-008-PASS: %s is a cross-fade DLL - exec==0 dispatch runs on render-only invocations while loaded", name);
}
static void on_unload(uint8_t* base) {
    for (int i = 0; i < 8; i++)
        if (g_xbase[i] == base) { g_xbase[i] = nullptr; InterlockedDecrement(&g_pass_loaded); }
}

static void __cdecl h_fallback_dispatch(int pass) {
    if (row_MAG008.on && !g_disabled && render_only()) {
        if (!(g_pass_on && g_pass_loaded > 0)) { InterlockedIncrement(&g_skipped); return; }
        InterlockedIncrement(&g_ro_passed);
    }
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
    g_pass_on = config_row("X2-MAG-008-PASS", 1);
    if (g_pass_on && !magic_mods_subscribe("X2-MAG-008", on_load, on_unload)) g_pass_on = 0;
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
    static LONG ro_seen = 0;
    if (g_ro_passed && !ro_seen) {
        ro_seen = 1;
        log_line("X2-MAG-008-PASS: first render-only dispatch run for a cross-fade DLL");
    }
}

Row row_MAG008 = {
    "X2-MAG-008",
    "skip the exec==0 effect dispatch FUN_00616660(0) on render-only invocations",
    1, install, nullptr, post, 0
};

} // namespace x2
