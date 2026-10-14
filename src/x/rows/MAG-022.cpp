// SPDX-License-Identifier: Apache-2.0
// Hellfire: the rock, embers and enemy-lift nodes step on every draw, so the rock shatters before it is
// thrown and enemies spin too long. On render-only frames the rock only draws, and the embers and targets
// nodes are skipped. Found by pattern in any magic DLL (Hellfire ships in several builds, e.g. 0307, 0605).
// Details: docs/fps60/rows/MAG-022.md
#include "../fps60.h"
#include <string.h>

typedef void(__cdecl* node_t)(void* node);

// Rock node: loads the work struct, switches on the rock state, steps that state, then jumps to the draw.
static const char ROCK[] =
    "A1 ?? ?? ?? ?? 8B 80 ?? ?? 00 00 83 E8 00 74 1A 48 74 0D 48 75 19 "
    "E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? E8 ?? ?? ?? ?? E9";
// Rock draw (the three JMP targets): aligned frame.
static const char DRAW[] = "55 8B EC 83 E4 F8 83 EC 64";
// Targets node: stack cookie, then `if (target count == 0)`.
static const char TARGETS[] = "55 8B EC 83 EC 7C A1 ?? ?? ?? ?? 33 C5 89 45 FC 83 3D ?? ?? ?? ?? 00";
// Embers node: stack cookie, then increments node word +0x14 and compares word +0x16 with 0x5A.
static const char EMBERS[] = "55 8B EC 83 EC 58 A1 ?? ?? ?? ?? 33 C5 89 45 FC 8B 55 08 56 66 FF 42 14 66 83 7A 16 5A";

enum { MAX_PENDING = 8 };
static uint8_t* volatile g_pending[MAX_PENDING];   // loaded, not yet scanned (set under the loader lock)
static uint32_t g_pending_size[MAX_PENDING];
static char     g_pending_name[MAX_PENDING][24];
static uint8_t* g_mod = nullptr;                   // the hooked Hellfire module (one at a time)
static char     g_name[24];
static uint8_t* g_hooked[3];                       // hooked entries (removed at unload)
static int      g_nhooked = 0;
static node_t   g_rock_tramp = nullptr, g_rock_draw = nullptr, g_emb_tramp = nullptr, g_tgt_tramp = nullptr;
static int      g_embers_on = 1, g_targets_on = 1;
static volatile LONG g_rock_held = 0, g_emb_held = 0, g_tgt_held = 0;

static void __cdecl h_rock(void* node) {
    if (row_MAG022.on && !g_disabled && pacing_kind() == INV_BUILD) {
        InterlockedIncrement(&g_rock_held);
        g_rock_draw(node);                         // draw at the logic-tick state, no step
        return;
    }
    g_rock_tramp(node);
}

static void __cdecl h_embers(void* node) {
    if (row_MAG022.on && !g_disabled && pacing_kind() == INV_BUILD) {
        InterlockedIncrement(&g_emb_held);
        return;
    }
    g_emb_tramp(node);
}

static void __cdecl h_targets(void* node) {
    if (row_MAG022.on && !g_disabled && pacing_kind() == INV_BUILD) {
        InterlockedIncrement(&g_tgt_held);
        return;
    }
    g_tgt_tramp(node);
}

static bool hook(const char* name, uint8_t* at, void* fn, node_t* tramp) {
    if (!hook_entry(name, at, fn, (void**)tramp)) return false;
    g_hooked[g_nhooked++] = at;
    return true;
}

static uint32_t gva(uint8_t* mod, uint8_t* p) { return 0x10000000u + (uint32_t)(p - mod); }

// A node that occurs exactly once in the module, or null.
static uint8_t* find_one(uint8_t* mod, uint32_t size, const char* pat) {
    int hits = 0;
    uint8_t* p = sig_scan_module(mod, size, pat, &hits);
    return hits == 1 ? p : nullptr;
}

static void try_patch(uint8_t* mod, uint32_t size, const char* name) {
    uint8_t* r = find_one(mod, size, ROCK);
    if (!r) return;                                // not a Hellfire build
    if (g_mod) { log_line("MAG-022: %s has the Hellfire rock node but %s is still hooked - left alone", name, g_name); return; }
    uint8_t* draw = jmp_target(r + 0x1B);
    uint8_t* work = read_abs(r + 1);
    bool ok = draw && jmp_target(r + 0x25) == draw && jmp_target(r + 0x2F) == draw && sig_match(draw, DRAW) &&
              work >= mod && work < mod + size;
    if (!ok) { log_line("MAG-022: %s rock node at gva 0x%08X has an unexpected shape - not hooked", name, gva(mod, r)); return; }
    g_rock_draw = (node_t)draw;
    if (!hook("MAG-022 rock", r, (void*)&h_rock, &g_rock_tramp)) return;
    g_mod = mod;
    memcpy(g_name, name, sizeof(g_name));
    g_rock_held = g_emb_held = g_tgt_held = 0;
    log_line("MAG-022: %s rock node hooked (gva 0x%08X, draw 0x%08X): render-only invocations draw without stepping",
             name, gva(mod, r), gva(mod, draw));

    uint8_t* t = g_targets_on ? find_one(mod, size, TARGETS) : nullptr;
    if (t && read_abs(t + 18) >= mod && read_abs(t + 18) < mod + size &&
        hook("MAG-022 targets", t, (void*)&h_targets, &g_tgt_tramp))
        log_line("MAG-022: %s targets node hooked (gva 0x%08X): skipped on render-only invocations", name, gva(mod, t));
    else
        log_line("MAG-022: %s targets node %s", name, g_targets_on ? "not found (this build has none)" : "left as is (MAG-022-TARGETS=0)");

    uint8_t* e = g_embers_on ? find_one(mod, size, EMBERS) : nullptr;
    if (e && hook("MAG-022 embers", e, (void*)&h_embers, &g_emb_tramp))
        log_line("MAG-022: %s embers node hooked (gva 0x%08X): skipped on render-only invocations", name, gva(mod, e));
    else
        log_line("MAG-022: %s embers node %s", name, g_embers_on ? "not found" : "left as is (MAG-022-EMBERS=0)");
}

// ---------------------------------------------------------------- module tracking (magic_mods.cpp, loader lock)
static void on_load(uint8_t* base, uint32_t size, const char* name) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pending[i]) continue;
        g_pending_size[i] = size;
        memset(g_pending_name[i], 0, sizeof(g_pending_name[i]));
        strncpy(g_pending_name[i], name, sizeof(g_pending_name[i]) - 1);
        g_pending[i] = base;                     // publish last
        return;
    }
}

static void on_unload(uint8_t* base) {
    for (int i = 0; i < MAX_PENDING; i++) if (g_pending[i] == base) g_pending[i] = nullptr;
    if (base != g_mod) return;
    log_line("MAG-022: %s unloaded (render-only calls held: rock %ld, embers %ld, targets %ld)", g_name, g_rock_held, g_emb_held, g_tgt_held);
    for (int i = 0; i < g_nhooked; i++) hook_remove("MAG-022", g_hooked[i]);
    g_nhooked = 0;
    g_mod = nullptr;
}

static bool install() {
    g_embers_on = config_row("MAG-022-EMBERS", 1);
    g_targets_on = config_row("MAG-022-TARGETS", 1);
    if (!magic_mods_subscribe("MAG-022", on_load, on_unload)) return false;
    log_line("  MAG-022: watching magic DLL loads for the Hellfire rock node (0307, 0563, 0605, 0612 on disk; embers %s)",
             g_embers_on ? "too" : "off");
    return true;
}

static void pre() {
    for (int i = 0; i < MAX_PENDING; i++) {
        uint8_t* m = g_pending[i];
        if (!m) continue;
        char name[24];
        memcpy(name, g_pending_name[i], sizeof(name));
        g_pending[i] = nullptr;
        __try { try_patch(m, g_pending_size[i], name); }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("MAG-022: fault 0x%08lX while hooking %s - left unhooked", GetExceptionCode(), name); }
    }
}

Row row_MAG022 = {
    "MAG-022",
    "Hellfire rock node (any magic DLL build): render-only invocations draw without stepping; embers and targets nodes skipped",
    1, install, pre, nullptr, 0
};
