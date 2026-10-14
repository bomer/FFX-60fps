// SPDX-License-Identifier: Apache-2.0
// UI-SPH-001: the Sphere Grid runs at divider 2, so at 60 Hz its per-frame update FUN_00a53570 (task 0x13, called
// once per Sg_MainLoop) also ran on render-only invocations: stick cursor steps and the grid's own call-counted
// d-pad repeat went 2x. The update is skipped on render-only invocations; it draws nothing (the draw is
// FUN_008e2720 -> FUN_00a54560). Details: re/ledger/UI-SPHERE.md
#include "../fps60.h"

typedef void(__cdecl* update_t)(void);

static uint8_t*  g_entry = nullptr;
static update_t  g_tramp = nullptr;
static bool      g_patched = false;
static volatile LONG g_skipped = 0, g_passed = 0;

// void(void), plain RET (0xA53DD9): returning early is stack-safe.
static void __cdecl h_update() {
    if (!g_disabled && pacing_kind() == INV_BUILD) {
        if (InterlockedIncrement(&g_skipped) == 1) log_line("UI-SPH-001: first Sphere Grid update skipped on a render-only invocation");
        return;
    }
    InterlockedIncrement(&g_passed);
    g_tramp();
}

static bool install() {
    // Sphere Grid update FUN_00a53570: prologue, then sets a global to 1 and tests the grid state byte +0x115B8
    g_entry = sig_find_unique("UI-SPH-001 Sphere Grid update FUN_00a53570",
        "55 8B EC 83 EC 18 A1 ?? ?? ?? ?? 33 C5 89 45 FC 8B 0D ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00 80 B9 B8 15 01 00",
        0xA53570);
    if (!g_entry) return false;
    log_line("  UI-SPH-001: FUN_00a53570 at gva=0x%08X (entry hooked on the game thread)", rt_to_gva_exe(g_entry));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("UI-SPH-001", g_entry, (void*)&h_update, (void**)&g_tramp)) {
        log_line("UI-SPH-001: entry hook failed - row off");
        row_UISPH001.on = 0;
        return;
    }
    log_line("UI-SPH-001: FUN_00a53570 entry hooked on the game thread (tramp %p)", (void*)g_tramp);
}

Row row_UISPH001 = {
    "UI-SPH-001",
    "Sphere Grid update FUN_00a53570 (stick cursor, grid d-pad repeat) only on logic invocations",
    1, install, pre, nullptr, 0
};
