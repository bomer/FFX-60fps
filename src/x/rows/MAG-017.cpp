// SPDX-License-Identifier: Apache-2.0
// MAG-017: Meteor Strike (magic_0305) steps Ifrit's move script, sound countdowns and the fireball's flight
// from its draw, so they run 2x at 60 fps. Render-only frames now draw the records and the trail without stepping.
// Details: docs/fps60/rows/MAG-017.md
#include "../fps60.h"
#include <string.h>

typedef void(__cdecl* node_t)(void* node);
typedef void(__cdecl* driver_v_t)(void);

enum { NODE_RVA = 0x1BF0, SCRIPT_CALL = 0x7C, DRIVER_CALL = 0x83, SCRIPT_RVA = 0x2680, DRIVER_RVA = 0x3F60 };

static volatile uint8_t* g_pending = nullptr;   // loaded, not yet patched
static uint8_t*  g_mod = nullptr;               // patched module
static uint8_t*  g_node = nullptr;              // hooked entries (removed at unload)
static uint8_t*  g_mover = nullptr;
static node_t    g_tramp = nullptr;
static driver_v_t g_driver = nullptr;
static volatile LONG g_build_calls = 0, g_logic_calls = 0;

enum { MOVER_RVA = 0x2E30, SAVE_OFF = 0xD0, SAVE_LEN = 0x3A0 - 0xD0 };
static node_t    g_mtramp = nullptr;
static volatile LONG g_mover_held = 0;

static void __cdecl h_mover(void* node) {
    if (row_MAG017.on && !g_disabled && pacing_kind() == INV_BUILD) {
        uint8_t* rec = *(uint8_t**)((uint8_t*)node + 0x14);
        uint8_t save[SAVE_LEN];
        memcpy(save, rec + SAVE_OFF, SAVE_LEN);
        memset(rec + 0x368, 0, 16);                   // velocity: no motion, no arrival
        *(uint32_t*)(rec + 0x37C) = 0;                // per-step callback (spark spawn)
        g_mtramp(node);
        memcpy(rec + SAVE_OFF, save, SAVE_LEN);
        InterlockedIncrement(&g_mover_held);
        return;
    }
    g_mtramp(node);
}

static void __cdecl h_node(void* node) {
    if (row_MAG017.on && !g_disabled && pacing_kind() == INV_BUILD) {
        InterlockedIncrement(&g_build_calls);
        g_driver();                               // draw the records only
        return;
    }
    InterlockedIncrement(&g_logic_calls);
    g_tramp(node);
}

static void try_patch(uint8_t* mod) {
    uint8_t* n = mod + NODE_RVA;
    // Draw node: starts by testing the first sound countdown; calls the move script and the record driver
    if (!sig_match(n, "55 8B EC A1 ?? ?? ?? ?? 85 C0 7E") || read_abs(n + 4) != mod + 0x225AE8 ||
        call_target(n + SCRIPT_CALL) != mod + SCRIPT_RVA || call_target(n + DRIVER_CALL) != mod + DRIVER_RVA) {
        log_line("MAG-017: magic_0305 node at +0x%X does not match (wrong build?) - not hooked", NODE_RVA);
        return;
    }
    g_driver = (driver_v_t)(mod + DRIVER_RVA);    // through MAG-016's hook, if installed
    if (!hook_entry("MAG-017 node", n, (void*)&h_node, (void**)&g_tramp)) return;
    g_node = n;
    g_mod = mod;
    g_build_calls = g_logic_calls = 0;
    log_line("MAG-017: magic_0305 draw node hooked (gva 0x%08X): render-only invocations draw records only",
             0x10000000u + NODE_RVA);

    // Throw mover: loads its record (node+0x14) and the record's countdown (+0x38C)
    uint8_t* m = mod + MOVER_RVA;
    if (!sig_match(m, "55 8B EC 83 E4 F8 83 EC 5C A1 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 8B 5D 08 ?? 8B 73 14 ?? 8B 86 8C 03 00 00")) {
        log_line("MAG-017: magic_0305 throw mover at +0x%X does not match - not hooked", MOVER_RVA);
        return;
    }
    if (!hook_entry("MAG-017 mover", m, (void*)&h_mover, (void**)&g_mtramp)) return;
    g_mover = m;
    g_mover_held = 0;
    log_line("MAG-017: magic_0305 throw mover hooked (gva 0x%08X): render-only invocations redraw without moving",
             0x10000000u + MOVER_RVA);
}

// ---------------------------------------------------------------- module tracking (magic_mods.cpp, loader lock)
static void on_load(uint8_t* base, uint32_t, const char* name) {
    if (_stricmp(name, "magic_0305.dll") == 0) g_pending = base;
}

static void on_unload(uint8_t* base) {
    if (base != g_mod && base != g_pending) return;
    if (g_mod) log_line("MAG-017: magic_0305 unloaded (render-only node calls held: %ld, logic calls: %ld, mover calls held: %ld)",
                        g_build_calls, g_logic_calls, g_mover_held);
    if (g_node) hook_remove("MAG-017 node", g_node);
    if (g_mover) hook_remove("MAG-017 mover", g_mover);
    g_node = g_mover = nullptr;
    g_mod = nullptr; g_pending = nullptr;
}

static bool install() {
    if (!magic_mods_subscribe("MAG-017", on_load, on_unload)) return false;
    log_line("  MAG-017: waiting for magic_0305.dll (Meteor Strike) to hook its draw node +0x%X", NODE_RVA);
    return true;
}

static void pre() {
    uint8_t* m = (uint8_t*)g_pending;
    if (!m) return;
    g_pending = nullptr;
    __try { try_patch(m); }
    __except (EXCEPTION_EXECUTE_HANDLER) { log_line("MAG-017: fault 0x%08lX while hooking - left unhooked", GetExceptionCode()); }
}

Row row_MAG017 = {
    "MAG-017",
    "magic_0305 (Meteor Strike) draw node: render-only invocations draw records only (move script/SE countdowns held)",
    1, install, pre, nullptr, 0
};
