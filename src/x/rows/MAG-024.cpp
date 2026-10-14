// SPDX-License-Identifier: Apache-2.0
// A thrown item's arc (grenade and a few others) is stepped by a per-draw node, so it flies at 2x. On
// render-only frames the node returns at once; the item is drawn where the last logic tick put it.
// Details: docs/fps60/rows/MAG-024.md
#include "../fps60.h"
#include <string.h>
#include <utility>

// Arc node entry: decrement the launch countdown, then fly/smoke/impact
static const char SIG[] = "55 8B EC 51 A1 ?? ?? ?? ?? 85 C0 7E ?? 48 A3 ?? ?? ?? ?? 85 C0 0F 8F";
static bool same_countdown(const uint8_t* p) { return read_abs(p + 5) == read_abs(p + 15); }   // reads and writes [cd]

typedef void(__fastcall* node_t)(void* node);

enum { MAX_MODS = 16 };
struct Mod {
    uint8_t* base; uint32_t size; char name[24];
    bool dead, seen;                             // seen: pre() has looked at it
    uint8_t* entry;                              // hooked entry (null = not hooked)
    node_t orig;                                 // MinHook trampoline
};
static Mod g_mods[MAX_MODS];
static volatile uint8_t g_ro = 0;               // 1 while a render-only invocation runs

template <int I> static void __fastcall h_node(void* node) {
    if (g_ro) return;                            // render-only: no step, no smoke spawn
    g_mods[I].orig(node);
}
template <int... I> static void* const* detours(std::integer_sequence<int, I...>) {
    static void* const list[] = { (void*)&h_node<I>... };
    return list;
}
static void* const* const DETOURS = detours(std::make_integer_sequence<int, MAX_MODS>{});

static void patch(int idx) {
    Mod& m = g_mods[idx];
    m.seen = true;
    int hits = 0;
    uint8_t* e = sig_scan_module(m.base, m.size, SIG, &hits, same_countdown);
    if (hits != 1) { if (hits > 1) log_line("MAG-024: %s has %d arc nodes - not handled", m.name, hits); return; }
    if (!hook_entry("MAG-024", e, DETOURS[idx], (void**)&m.orig)) return;
    m.entry = e;
    log_line("MAG-024: %s arc node at gva 0x%08X held on render-only invocations", m.name, 0x10000000u + (uint32_t)(e - m.base));
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
        if (m.entry) hook_remove("MAG-024", m.entry);
        m.entry = nullptr;
        m.base = nullptr;
        m.dead = true;
    }
}

static void pre() {
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (m.dead) { memset(&m, 0, sizeof(m)); continue; }
        if (!m.base || m.seen) continue;
        __try { patch(i); }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("MAG-024: fault 0x%08lX patching %s - left alone", GetExceptionCode(), m.name); }
    }
    g_ro = (!g_disabled && pacing_kind() == INV_BUILD) ? 1 : 0;
}

static void post() { g_ro = 0; }

static bool install() {
    if (!magic_mods_subscribe("MAG-024", on_load, on_unload)) return false;
    log_line("  MAG-024: watching magic DLL loads for the thrown-item arc node (magic_0242, 0651, 0654, 0655, 0657)");
    return true;
}

Row row_MAG024 = {
    "MAG-024",
    "thrown-item arc node (grenade, magic_0242 FUN_10003AA0 and 4 more DLLs) steps only on logic invocations",
    1, install, pre, post, 0
};
