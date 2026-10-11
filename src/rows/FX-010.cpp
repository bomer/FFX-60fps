// SPDX-License-Identifier: Apache-2.0
// Field/map particles (rain, lightning, field lights) step on every draw and run at 2x. On render-only
// frames their step runs with the particle pause flag set, as MAG-006 does for DLL programs. Off by default.
// Details: docs/fps60/rows/FX-010.md
#include "../fps60.h"
#include <string.h>

int      ppp_step_paused(int (__cdecl* tramp)(void*, uint32_t), void* prog, uint32_t flags);
bool     ppp_resolve_pause(const char* row, const uint8_t* cmp_at);
bool     ppp_hook_spawn_call(const char* row, uint8_t* site);

typedef int(__cdecl* fp_step_t)(void* prog, uint32_t flags);

static uint8_t*  g_entry = nullptr;
static fp_step_t g_tramp = nullptr;
static bool      g_patched = false;
static volatile LONG g_paused_calls = 0, g_fresh_calls = 0;

static int __cdecl h_fp_step(void* prog, uint32_t flags) {
    if (row_FX010.on && !g_disabled && pacing_kind() == INV_BUILD) {
        if (*(int32_t*)((uint8_t*)prog + 8) == 0 && *(int32_t*)((uint8_t*)prog + 0xC) == 0) {
            InterlockedIncrement(&g_fresh_calls);
            return g_tramp(prog, flags);
        }
        InterlockedIncrement(&g_paused_calls);
        return ppp_step_paused(g_tramp, prog, flags);
    }
    return g_tramp(prog, flags);
}

static bool install() {
    // FUN_007122f0 (field particle step): prologue through its spawn CALL at +0x2A
    g_entry = sig_find_unique("FX-010 pppFpLoop step FUN_007122f0",
        "55 8B EC 53 8B 5D 0C 56 8B 75 08 89 35 ?? ?? ?? ?? F6 C3 10 74 09 56 E8 ?? ?? ?? ?? 83 C4 04 56 C7 05 ?? ?? ?? ?? 00 00 00 00 E8",
        0x7122F0);
    if (!g_entry) return false;
    if (!ppp_resolve_pause("FX-010", g_entry + 0x72)) return false;
    if (config_row("FX-010-SPAWN", 0)) {
        if (!ppp_hook_spawn_call("FX-010", g_entry + 0x2A)) return false;
    } else {
        log_line("  FX-010: spawn skip OFF (FX-010-SPAWN=0)");
    }
    log_line("  FX-010: FUN_007122f0 gva=0x%08X, entry hooked on the game thread",
             rt_to_gva_exe(g_entry));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("FX-010", g_entry, (void*)&h_fp_step, (void**)&g_tramp)) {
        log_line("FX-010: entry hook failed - row off");
        row_FX010.on = 0;
        return;
    }
    log_line("FX-010: FUN_007122f0 entry hooked on the game thread (tramp %p)", (void*)g_tramp);
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_paused_calls > 0) {
        reported = 1;
        log_line("FX-010: first paused field particle step on a render-only invocation (fresh unpaused: %ld)", g_fresh_calls);
    }
    static uint32_t tick = 0;
    if (++tick % 18000 == 0) log_line("FX-010: paused=%ld fresh=%ld", g_paused_calls, g_fresh_calls);
}

Row row_FX010 = {
    "FX-010",
    "field/map particle programs (FUN_007122f0): pause flag on render-only invocations (draw still runs)",
    0, install, pre, post, 0
};
