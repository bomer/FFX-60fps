// SPDX-License-Identifier: Apache-2.0
// Diagnostic DIAG-BTLSIG (default off): logs every battle effect signal FUN_00788920(arg) with the invocation
// kind and the caller, plus a periodic dump of family-X effect nodes. Written for the Bad Breath soft-lock.
// Details: docs/fps60/rows/DIAG-BTLSIG.md
#include "../fps60.h"
#include <string.h>
#include <intrin.h>
#include <stdio.h>

typedef void(__cdecl* sig_t)(uint32_t arg);

static sig_t    g_orig = nullptr;
static uint32_t g_tick = 0;
static volatile LONG g_lines = 0;

static void describe(uint8_t* ret, char* out, size_t n) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ret, &m) && m && GetModuleFileNameA(m, name, MAX_PATH)) {
        const char* b = strrchr(name, '\\');
        b = b ? b + 1 : name;
        uint32_t base = (_stricmp(b, "FFX.exe") == 0) ? 0x400000u : 0x10000000u;
        _snprintf_s(out, n, _TRUNCATE, "%s gva 0x%08X", b, base + (uint32_t)(ret - (uint8_t*)m));
    } else {
        _snprintf_s(out, n, _TRUNCATE, "%p", ret);
    }
}

static void __cdecl h_signal(uint32_t arg) {
    uint8_t* ret = (uint8_t*)_ReturnAddress();
    if (InterlockedIncrement(&g_lines) <= 600) {
        static const char* const K[] = { "LOGIC", "BUILD", "BACKLOG", "STOCK" };
        int k = pacing_kind();
        char who[96];
        describe(ret, who, sizeof(who));
        log_line("DIAG-BTLSIG: %-5s tick=%u arg=0x%08X case=0x%02X chr=%u from %s",
                 (k >= 0 && k < 4) ? K[k] : "?", g_tick, arg, (arg >> 8) & 0xFF, arg & 0xFF, who);
    }
    g_orig(arg);
}

// Node dump: wraps the effect node walk and logs each node's flags, state and handler every 30 logic ticks.
// Node layout: docs/fps60/rows/DIAG-BTLSIG.md.
typedef void(__cdecl* walk_t)(uint8_t* head);
static walk_t g_walk = nullptr;
static uint32_t g_dump_tick = 0;

// DIAG-BTLSIG-XREC: on every logic walk (after it), log each node whose state handler, state level, wait count
// or particle program (node+0x280 bit 0 = alive, +0x298 = age) changed, plus births and deaths. Timeline input
// for stock vs 60 comparisons (Vorban's Mortar, magic_0360).
static int g_xrec = 0;
struct XSeen { uint8_t* nd; uint32_t h, st, w, prog, cb; uint32_t gen; };
static XSeen g_xs[64];
static uint32_t g_xgen = 0;
static volatile LONG g_xlines = 0;

static void xrec_log(uint8_t* head) {
    if (g_xlines > 4000) return;
    g_xgen++;
    __try {
        uint8_t* tail = head + 0xBC;
        int n = 0;
        for (uint8_t* nd = *(uint8_t**)(head + 8); nd && nd != tail && n < 32; nd = *(uint8_t**)(nd + 8), n++) {
            uint32_t* u = (uint32_t*)nd;
            uint32_t st = u[0x2B];
            uint32_t h = st < 8 ? u[3 + st] : 0;
            uint32_t w = st < 8 ? u[0xB + st * 4] : 0;
            uint32_t prog = (u[0xA0] & 1) ? (u[0xA6] | 0x80000000u) : 0;   // +0x280 flags, +0x298 age
            XSeen* s = nullptr;
            for (XSeen& x : g_xs) if (x.nd == nd) { s = &x; break; }
            bool born = !s;
            if (!s) for (XSeen& x : g_xs) if (!x.nd || x.gen + 1 < g_xgen) { s = &x; break; }
            if (!s) continue;
            bool chg = born || s->h != h || s->st != st || (s->w != w && s->w != w + 1) ||       // a wait set, not a countdown step
                       ((s->prog ^ prog) & 0x80000000u) || s->cb != u[0x2C];
            if (chg) {
                char hs[96];
                describe((uint8_t*)(uintptr_t)h, hs, sizeof(hs));
                InterlockedIncrement(&g_xlines);
                log_line("XREC tick=%u %s node=%p lvl=%u h=%s wait=%d prog=%s age=0x%X pos=%.1f,%.1f,%.1f",
                         g_tick, born ? "BORN" : "chg ", nd, st, hs, (int)w, (prog & 0x80000000u) ? "on " : "off",
                         prog & 0x7FFFFFFFu, *(float*)(nd + 0xD0), *(float*)(nd + 0xD4), *(float*)(nd + 0xD8));
            }
            s->nd = nd; s->h = h; s->st = st; s->w = w; s->prog = prog; s->cb = u[0x2C]; s->gen = g_xgen;
        }
        for (XSeen& x : g_xs) if (x.nd && x.gen != g_xgen) {
            InterlockedIncrement(&g_xlines);
            log_line("XREC tick=%u GONE node=%p", g_tick, x.nd);
            x.nd = nullptr;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { log_line("XREC: read fault"); }
}

static void __cdecl h_walk(uint8_t* head) {
    if (g_xrec && pacing_kind() != INV_BUILD) { g_walk(head); xrec_log(head); return; }
    if (pacing_kind() != INV_BUILD && g_tick - g_dump_tick >= 30 && InterlockedCompareExchange(&g_lines, 0, 0) < 1500) {
        g_dump_tick = g_tick;
        __try {
            int n = 0;
            for (uint8_t* nd = *(uint8_t**)(head + 8); nd && n < 16; nd = *(uint8_t**)(nd + 8), n++) {
                uint32_t* u = (uint32_t*)nd;
                uint32_t st = u[0x2B];
                char h[96], c[96];
                describe(st < 64 ? (uint8_t*)(uintptr_t)u[3 + st] : nullptr, h, sizeof(h));
                describe((uint8_t*)(uintptr_t)u[0x2C], c, sizeof(c));
                InterlockedIncrement(&g_lines);
                log_line("DIAG-BTLSIG node: tick=%u head=%p #%d node=%p flags=%08X state=%u handler=%s cb=%s w=%08X %08X %08X %08X",
                         g_tick, head, n, nd, u[0], st, h, c, u[0x2D], u[0x2E], u[0x2F], u[0x30]);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("DIAG-BTLSIG node: read fault"); }
    }
    g_walk(head);
}

static bool install() {
    g_xrec = config_row("DIAG-BTLSIG-XREC", 0);
    if (g_xrec) log_line("  DIAG-BTLSIG-XREC: family-X node transitions logged on every logic walk (XREC lines)");
    // The battle effect signal handler FUN_00788920(arg)
    uint8_t* fn = sig_find_unique("DIAG-BTLSIG FUN_00788920",
        "55 8B EC 53 E8 ?? ?? ?? ?? 8B 5D 08 85 C0 74 10 0F B6 C3 50 E8", 0x788920);
    if (!fn) return false;
    g_orig = (sig_t)fn;
    // The three CALL sites, located relative to the EXE base and verified to target FUN_00788920.
    static const uint32_t SITES[] = { 0x9254B9, 0x7FE7DE, 0x80C4C3 };
    uint8_t* base = exe_base();
    int ok = 0;
    for (uint32_t gva : SITES) {
        uint8_t* site = base + (gva - 0x400000);
        if (call_target(site) != fn) { log_line("  DIAG-BTLSIG: CALL at gva=0x%08X does not target it - skipped", gva); continue; }
        if (patch_call_rel32(site, fn, (void*)&h_signal)) ok++;
    }
    // MagicFile_Start: pushes the magic service table (slot +0x358 holds the handler)
    uint8_t* at = sig_find_unique("DIAG-BTLSIG MagicFile_Start table push",
        "0F 94 C0 50 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? FF 35", 0x9DA816);
    if (at) {
        uint32_t* slot = (uint32_t*)((uint8_t*)read_abs(at + 0x0A) + 0x358);
        if (*slot == (uint32_t)(uintptr_t)fn) {
            uint32_t v = (uint32_t)(uintptr_t)&h_signal;
            if (patch_write(slot, &v, 4)) ok++;
        } else {
            log_line("  DIAG-BTLSIG: table+0x358 = 0x%08X, not FUN_00788920 - slot skipped", *slot);
        }
    }
    uint8_t* ws = base + (0x9254A4 - 0x400000);
    uint8_t* wt = call_target(ws);
    if (wt && patch_call_rel32(ws, wt, (void*)&h_walk)) {      // chains MAG-020's hook when it is on
        g_walk = (walk_t)wt;
        log_line("  DIAG-BTLSIG: node walk CALL at gva=0x009254A4 wrapped (node dump every 30 logic ticks)");
    }
    log_line("  DIAG-BTLSIG: %d of 4 entry points redirected to the logger", ok);
    return ok > 0;
}

static void pre() { if (pacing_kind() != INV_BUILD) g_tick++; }

Row row_DIAG_BTLSIG = {
    "DIAG-BTLSIG",
    "diagnostic: log battle effect signals FUN_00788920 (arg, caller, invocation kind)",
    0, install, pre, nullptr, 0
};
