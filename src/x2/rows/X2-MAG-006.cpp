// SPDX-License-Identifier: Apache-2.0
// X2-MAG-006 (port of FFX MAG-006 v3+v4): particle programs (Ppp_ProgramStep FUN_007fed00) step on every draw, so
// at 60 fps they run 2x. On render-only invocations DLL callers (service table +0xB00) step with the engine's pause
// flag 0x01CF579C set: the particles draw but their clock, age and ops don't advance. EXE callers keep stock
// behaviour (pausing them crashed in FFX). A program that never advanced (+8 == 0 && +0xC == 0) steps unpaused
// (FFX v4 FRESH: a paused first step draws unset state).
// FFX-2 only: the step's first call FUN_006ba090 (E8 at entry+8) can clear the pause flag for the loaded magic
// overlay (DAT_00e20b24 && DAT_00e10398); it is skipped while this row pauses a step.
// Open (H, PORT-B §2): FFX-2's start-delay clock prog+0x78 (FUN_00804870) runs 2x for FRESH programs.
// Evidence: outer repo re/ledger/PORT-B.md §2 (S).
#include "../x2.h"
#include <intrin.h>

namespace x2 {

typedef int(__cdecl* ppp_step_t)(void* prog, uint32_t flags);   // bool cdecl(prog, flags), plain RET
typedef void(__cdecl* override_t)(void* prog);

static uint8_t*   g_entry = nullptr;
static ppp_step_t g_tramp = nullptr;
static override_t g_override = nullptr;
static volatile int32_t* g_pause = nullptr;
static bool       g_patched = false;
static int        g_fresh_on = 1;                // [rows] X2-MAG-006-FRESH
static int        g_pausing = 0;                 // game thread: this row is inside a paused step
static volatile LONG g_paused_calls = 0, g_exe_calls = 0, g_fresh_calls = 0, g_override_skipped = 0;
extern Row row_MAG006;

static void __cdecl h_override(void* prog) {
    if (g_pausing) { InterlockedIncrement(&g_override_skipped); return; }
    g_override(prog);
}

static int __cdecl h_ppp_step(void* prog, uint32_t flags) {
    if (!row_MAG006.on || g_disabled || !render_only() || g_pausing) return g_tramp(prog, flags);
    uint8_t* ret = (uint8_t*)_ReturnAddress();
    if (in_image(ret)) {                         // EXE caller: stock
        InterlockedIncrement(&g_exe_calls);
        return g_tramp(prog, flags);
    }
    if (g_fresh_on && *(int32_t*)((uint8_t*)prog + 8) == 0 && *(int32_t*)((uint8_t*)prog + 0xC) == 0) {
        InterlockedIncrement(&g_fresh_calls);
        return g_tramp(prog, flags);
    }
    InterlockedIncrement(&g_paused_calls);
    int32_t saved = *g_pause;                    // values written by event scripts survive (FFX MAG-004)
    *g_pause = 1;
    g_pausing = 1;
    int r = g_tramp(prog, flags);
    g_pausing = 0;
    *g_pause = saved;
    return r;
}

static bool install() {
    // Ppp_ProgramStep: PUSH ESI; CALL override(prog); ADD ESP,4; CMP [..],0; JNZ; PUSH ESI; CALL start-delay
    g_entry = sig_find("X2-MAG-006 Ppp_ProgramStep",
        "55 8B EC 56 8B 75 08 56 E8 ?? ?? ?? ?? 83 C4 04 83 3D ?? ?? ?? ?? 00 75 20 56 E8", 0x7FED00);
    if (!g_entry) return false;
    // the step's own pause test: CMP dword [flag],0 at entry+0x1BF
    if (!sig_match(g_entry + 0x1BF, "83 3D ?? ?? ?? ?? 00 5F 75 3F 80 7E 1C 00")) {
        log_line("  X2-MAG-006: pause CMP not at entry+0x1BF - refused");
        return false;
    }
    g_pause = (volatile int32_t*)abs_operand("particle pause flag", g_entry + 0x1BF + 2);
    if (!g_pause) return false;
    // override FUN_006ba090: if (DAT_00e20b24) flag &= ~-(DAT_00e10398 != 0)
    uint8_t* ov = call_target(g_entry + 8);
    if (!ov || !in_image(ov)) return false;
    g_override = (override_t)ov;
    g_fresh_on = config_row("X2-MAG-006-FRESH", 1);
    log_line("  X2-MAG-006: Ppp_ProgramStep gva=0x%08X (entry + override CALL patched on the game thread), pause flag gva=0x%08X "
             "(expected 0x01CF579C), override gva=0x%08X skipped while paused, FRESH %s",
             rt_to_gva(g_entry), rt_to_gva((const void*)g_pause), rt_to_gva(ov), g_fresh_on ? "ON" : "OFF");
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    // both patches on the game thread, between invocations: the step never runs while they are written
    if (!patch_call_rel32(g_entry + 8, (void*)g_override, (void*)&h_override)) {
        log_line("X2-MAG-006: override CALL patch failed - row off");
        row_MAG006.on = 0;
        return;
    }
    if (!hook_entry("X2-MAG-006", g_entry, (void*)&h_ppp_step, (void**)&g_tramp)) {
        log_line("X2-MAG-006: entry hook failed - row off (the override stub is pass-through alone)");
        row_MAG006.on = 0;
    }
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_paused_calls > 0) {
        reported = 1;
        log_line("X2-MAG-006: first paused DLL particle step on a render-only invocation (EXE callers passed %ld, fresh %ld)",
                 g_exe_calls, g_fresh_calls);
    }
    static LONG ov_seen = 0;
    if (g_override_skipped && !ov_seen) {
        ov_seen = 1;
        log_line("X2-MAG-006: pause override FUN_006ba090 skipped inside a paused step (first time)");
    }
}

Row row_MAG006 = {
    "X2-MAG-006",
    "DLL particle programs: pause flag on render-only invocations (draw still runs); EXE callers untouched",
    1, install, pre, post, 0
};

} // namespace x2
