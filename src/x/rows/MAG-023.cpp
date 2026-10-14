// SPDX-License-Identifier: Apache-2.0
// Many magic DLLs drive the caster's motion with a script called from a draw node, so the throw runs at
// 2x. On render-only frames the script returns its current result without stepping.
// Details: docs/fps60/rows/MAG-023.md
#include "../fps60.h"
#include <string.h>
#include <utility>

// Motion script entry: return 1 if done, else tick its wait countdown and command index
static const char SIG[] =
    "83 3D ?? ?? ?? ?? 01 75 06 B8 01 00 00 00 C3 66 A1 ?? ?? ?? ?? 66 85 C0 75 ?? 66 FF 05 ?? ?? ?? ?? 66 83 3D ?? ?? ?? ?? 00";

typedef int(__cdecl* script_t)();

enum { MAX_MODS = 16 };
struct Mod {
    uint8_t* base; uint32_t size; char name[24];
    bool dead, seen;                             // seen: pre() has looked at it
    uint8_t* entry;                              // hooked entry (null = not hooked)
    script_t orig;                               // MinHook trampoline
    volatile int32_t* done;                      // the script's done flag (relocated)
};
static Mod g_mods[MAX_MODS];
static volatile uint8_t g_ro = 0;               // 1 while a render-only invocation runs
static LONG g_patched = 0;

// One detour per slot: the slot index is the only per-DLL state the hook needs.
template <int I> static int __cdecl h_script() {
    Mod& m = g_mods[I];
    if (g_ro) return *m.done;                    // render-only: current result, no step
    return m.orig();
}
template <int... I> static void* const* detours(std::integer_sequence<int, I...>) {
    static void* const list[] = { (void*)&h_script<I>... };
    return list;
}
static void* const* const DETOURS = detours(std::make_integer_sequence<int, MAX_MODS>{});

static void patch(int idx) {
    Mod& m = g_mods[idx];
    m.seen = true;
    int hits = 0;
    uint8_t* e = sig_scan_module(m.base, m.size, SIG, &hits);
    if (hits != 1) { if (hits > 1) log_line("MAG-023: %s has %d motion scripts - not handled", m.name, hits); return; }
    m.done = (volatile int32_t*)read_abs(e + 2);                         // CMP dword [done],1 (relocated)
    if (!hook_entry("MAG-023", e, DETOURS[idx], (void**)&m.orig)) return;
    m.entry = e;
    InterlockedIncrement(&g_patched);
    log_line("MAG-023: %s motion script at gva 0x%08X (done flag gva 0x%08X) held on render-only invocations",
             m.name, 0x10000000u + (uint32_t)(e - m.base), 0x10000000u + (uint32_t)((uint8_t*)m.done - m.base));
}

// ---------------------------------------------------------------- module tracking (magic_mods.cpp, loader lock)
static void on_load(uint8_t* base, uint32_t size, const char* name) {
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (m.base || m.dead) continue;
        memset(&m, 0, sizeof(m));
        m.size = size;
        strncpy(m.name, name, sizeof(m.name) - 1);
        m.base = base;                           // publish last
        return;
    }
}

static void on_unload(uint8_t* base) {
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (m.base != base) continue;
        if (m.entry) hook_remove("MAG-023", m.entry);
        m.entry = nullptr;
        m.base = nullptr;
        m.dead = true;
    }
}

// ---------------------------------------------------------------- per invocation
static void pre() {
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (m.dead) { memset(&m, 0, sizeof(m)); continue; }     // unloaded: the slot is free
        if (!m.base || m.seen) continue;
        __try { patch(i); }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("MAG-023: fault 0x%08lX patching %s - left alone", GetExceptionCode(), m.name); }
    }
    g_ro = (!g_disabled && pacing_kind() == INV_BUILD) ? 1 : 0;
}

static void post() { g_ro = 0; }

static bool install() {
    if (!magic_mods_subscribe("MAG-023", on_load, on_unload)) return false;
    log_line("  MAG-023: watching magic DLL loads for the actor motion script (118 DLLs on disk carry it)");
    return true;
}

Row row_MAG023 = {
    "MAG-023",
    "magic-DLL actor motion/throw script (118 DLLs, e.g. magic_0242 FUN_100013E0) steps only on logic invocations",
    1, install, pre, post, 0
};
