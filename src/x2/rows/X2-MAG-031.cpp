// SPDX-License-Identifier: Apache-2.0
// X2-MAG-031: magic DLLs that write the particle pause flag 0x1CF579C from their draw as (exec == 0) leave it at 1 when
// their last draw before the unload is a render-only invocation (exec 0 under 60 Hz pacing); at stock exec is never 0 at
// a draw, so the flag ends at 0. Nothing else clears it: every later particle program steps paused, the next effect
// never finishes and the battle waits forever. S: magic_0424 FUN_10001760 `*[svc+0xBF0] = (svc+0x400() == 0)` on every
// draw (magic_0718 has the same shape). U 17/10/2026: battle simulator soft-lock twice (Seymour vs Tidus, auto battle),
// both right after magic_0424 unloaded; D: flag = 1 at the end of every invocation, no pppFpStopStatus call, X2-MAG-006
// not inside a paused step (outer repo re/ledger/HANG-0073.md).
// Fix: when a magic DLL unloads, the next invocation start (game thread) puts a flag of 1 back to 0, unless a battle
// script set it through pppFpStopStatus FUN_00810ef0 after that unload (its own value is kept).
#include "../x2.h"

namespace x2 {

extern Row row_MAG031;

typedef void(__cdecl* set_t)(int);
static uint8_t* g_fn = nullptr;
static set_t    g_orig = nullptr;
static volatile int32_t* g_flag = nullptr;
static volatile LONG g_pending = 0;             // set by a DLL unload (loader thread), consumed in pre()
static volatile LONG g_script_set = 0;          // pppFpStopStatus called since the last unload
static volatile LONG g_cleared = 0;

static void __cdecl h_set(int v) {
    InterlockedExchange(&g_script_set, 1);
    g_orig(v);
}

static void on_load(uint8_t*, uint32_t, const char*) {}
static void on_unload(uint8_t*) {
    InterlockedExchange(&g_script_set, 0);
    InterlockedExchange(&g_pending, 1);
}

static bool install() {
    g_fn = sig_find("X2-MAG-031 pppFpStopStatus",
        "55 8B EC 56 8B 75 08 56 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 89 35 ?? ?? ?? ?? 5E 5D C3", 0x810EF0);
    if (!g_fn) return false;
    g_flag = (volatile int32_t*)abs_operand("particle pause flag", g_fn + 0x17);
    if (!g_flag) return false;
    if (!magic_mods_subscribe("X2-MAG-031", on_load, on_unload)) return false;
    log_line("  X2-MAG-031: pppFpStopStatus gva=0x%08X, flag gva=0x%08X (expected 0x00810EF0 / 0x01CF579C)", rt_to_gva(g_fn),
             rt_to_gva((const void*)g_flag));
    return true;
}

static void pre() {
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        if (!hook_entry("X2-MAG-031", g_fn, (void*)&h_set, (void**)&g_orig))
            log_line("X2-MAG-031: setter hook failed - a script-set flag is not told apart (cleared too)");
    }
    if (!InterlockedExchange(&g_pending, 0)) return;
    if (*g_flag != 1 || g_script_set) return;
    *g_flag = 0;
    if (InterlockedIncrement(&g_cleared) <= 20)
        log_line("X2-MAG-031: particle pause flag left at 1 by an unloaded magic DLL - cleared (%ld so far)", g_cleared);
}

Row row_MAG031 = {
    "X2-MAG-031",
    "particle pause flag left at 1 by a magic DLL's last render-only draw is cleared after its unload (battle soft-lock)",
    1, install, pre, nullptr, 0
};

} // namespace x2
