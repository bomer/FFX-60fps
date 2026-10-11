// SPDX-License-Identifier: Apache-2.0
// Shared by MAG-006 and FX-010: step a particle program with the engine's pause flag set (it still
// draws, but its clock, age and ops don't advance), and optionally skip spawn, which ignores the flag.
// Details: docs/fps60/rows/MAG-006.md
#include "../fps60.h"
#include <string.h>

volatile int32_t* g_ppp_pause = nullptr;          // DAT_0230fd34 (runtime)
static void (__cdecl* g_spawn)(void*) = nullptr;  // FUN_00712b80
static volatile LONG g_spawn_skipped = 0;

// Particle steps call this around their trampoline on render-only invocations.
// Save/restore per call so values written by E DLLs / event scripts survive (MAG-004).
int ppp_step_paused(int (__cdecl* tramp)(void*, uint32_t), void* prog, uint32_t flags) {
    int32_t saved = *g_ppp_pause;
    *g_ppp_pause = 1;
    int r = tramp(prog, flags);
    *g_ppp_pause = saved;
    return r;
}

static void __cdecl h_spawn(void* prog) {
    if (!g_disabled && pacing_kind() == INV_BUILD) {
        InterlockedIncrement(&g_spawn_skipped);
        return;
    }
    g_spawn(prog);
}

// `cmp_at` points at the step function's `CMP dword [pause flag],0`.
bool ppp_resolve_pause(const char* row, const uint8_t* cmp_at) {
    if (cmp_at[0] != 0x83 || cmp_at[1] != 0x3D || cmp_at[6] != 0x00) {
        log_line("  %s: pause-flag CMP not found at %p - refused", row, cmp_at);
        return false;
    }
    volatile int32_t* f = (volatile int32_t*)read_abs(cmp_at + 2);
    if (g_ppp_pause && g_ppp_pause != f) {
        log_line("  %s: pause flag %p differs from %p - refused", row, f, g_ppp_pause);
        return false;
    }
    g_ppp_pause = f;
    log_line("  %s: particle pause flag at gva=0x%08X (expected 0x0230FD34)", row, rt_to_gva_exe((const void*)f));
    return true;
}

// Re-points one `CALL FUN_00712b80` to the spawn stub.
bool ppp_hook_spawn_call(const char* row, uint8_t* site) {
    uint8_t* target = call_target(site);
    // FUN_00712b80 (particle spawn): prologue
    if (!sig_match(target, "55 8B EC 56 8B 75 08 57 33 FF 39 7E 18")) {
        log_line("  %s: spawn CALL target %p is not FUN_00712b80 - refused", row, target);
        return false;
    }
    if (g_spawn && (uint8_t*)g_spawn != target) return false;
    g_spawn = (void (__cdecl*)(void*))target;
    if (!patch_call_rel32(site, target, (void*)&h_spawn)) {
        log_line("  %s: spawn CALL patch failed", row);
        return false;
    }
    log_line("  %s: spawn CALL at gva=0x%08X -> stub (skipped on render-only invocations)", row, rt_to_gva_exe(site));
    return true;
}
