// SPDX-License-Identifier: Apache-2.0
// MAG-020: family-X magic DLLs walk their effect nodes from the draw, so node state handlers run twice
// per logic tick at 60 fps (Bad Breath soft-locks). Render-only frames now only draw the nodes.
// Details: docs/fps60/rows/MAG-020.md
#include "../fps60.h"

typedef void(__cdecl* walk_t)(uint8_t* head);
typedef void(__cdecl* node_fn_t)(uint8_t* node);

static walk_t  g_walk = nullptr;
static volatile LONG g_draw_only = 0;

static void __cdecl h_walk(uint8_t* head) {
    if (!g_disabled && pacing_kind() == INV_BUILD) {
        int guard = 0;
        for (uint8_t* nd = head; nd && guard < 4096; nd = *(uint8_t**)(nd + 8), guard++) {
            uint32_t* u = (uint32_t*)nd;
            node_fn_t cb = (node_fn_t)(uintptr_t)u[0x2C];
            if (cb && !(u[0] & 4)) mag026_draw_record(head, nd, (void*)cb);   // MAG-026 off: plain cb(nd)
        }
        InterlockedIncrement(&g_draw_only);
        return;
    }
    g_walk(head);
}

static bool install() {
    // FUN_00925440: call the node walk, then the "walk complete" check and the battle signal
    uint8_t* fn = sig_find_unique("MAG-020 FUN_00925440 node walk call",
        "8B 47 0C 8D 77 1C 56 A3 ?? ?? ?? ?? E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 20 85 C0 74 0B FF 77 04 E8", 0x925498);
    if (!fn) return false;
    uint8_t* site = fn + 0x0C;                               // CALL node walk FUN_00930300
    uint8_t* walk = call_target(site);
    if (!walk || rt_to_gva_exe(walk) != 0x930300) { log_line("  MAG-020: CALL target %p is not FUN_00930300 - refused", walk); return false; }
    g_walk = (walk_t)walk;
    if (!patch_call_rel32(site, walk, (void*)&h_walk)) { log_line("  MAG-020: CALL patch failed"); return false; }
    log_line("  MAG-020: node walk CALL at gva=0x%08X (FUN_00930300 gva=0x%08X): render-only invocations draw nodes only",
             rt_to_gva_exe(site), rt_to_gva_exe(walk));
    return true;
}

static void post() {
    static uint32_t tick = 0;
    static LONG seen = 0;
    if (++tick % 600 == 0 && g_draw_only != seen) {
        seen = g_draw_only;
        log_line("MAG-020: family-X node walks drawn only on render-only invocations so far: %ld", seen);
    }
}

Row row_MAG020 = {
    "MAG-020",
    "family-X effect node walk (FUN_00925440): render-only invocations draw nodes only; state handlers at logic rate",
    1, install, nullptr, post, 0
};
