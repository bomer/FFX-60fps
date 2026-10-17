// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-FTBLINK (diagnostic, default 0, observe only): tests H1 of re/ledger/FT-BLINK.md for the Full Throttle blink
// (magic_0681 sword / fire drawn on logic invocations only). Chr actors reach the HD model list (hdframe+0xE9CC count,
// list +0xE9D4) only through Ch_DrawMain loop 3 (FUN_00449f20 at 0x6DCD98), which runs once per tick; the list is cleared
// in FUN_00464e80 (0x46500D, every invocation) when the state block [0xDCED88] has [0] != 1 or byte +0x338 / +0x33C set.
// While a listed magic DLL is loaded (X2-DIAG-FTBLINK-DLL, default "681 80": Full Throttle and a plain attack for
// comparison) every invocation logs, at its end: kind, the list count, state [0], +0x338, +0x33C (max 240 lines per load).
// H1 holds if, for 0681 only, the state clears the list each invocation and RENDER counts drop below LOGIC counts.
#include "../x2.h"
#include <stdlib.h>
#include <string.h>

namespace x2 {

static uint8_t** g_state = nullptr;              // &DAT_00dced88 (pointer to the state block)
static uint8_t** g_hdframe = nullptr;            // &DAT_00dcda04
static int  g_list[8], g_nlist = 0;
static uint8_t* g_watch = nullptr;               // loaded watched DLL base (one at a time)
static char  g_name[24];
static volatile LONG g_active = 0;
static int   g_lines = 0;
extern Row row_DIAGFTBLINK;

static void on_load(uint8_t* base, uint32_t, const char* name) {
    const char* p = strstr(name, "magic_");
    int n = p ? atoi(p + 6) : -1;
    for (int i = 0; i < g_nlist; i++)
        if (g_list[i] == n && !g_watch) {
            g_watch = base;
            strncpy_s(g_name, name, _TRUNCATE);
            g_lines = 0;
            InterlockedExchange(&g_active, 1);
            log_line("X2-DIAG-FTBLINK: %s loaded - logging HD list count and clear state per invocation", name);
        }
}
static void on_unload(uint8_t* base) {
    if (base == g_watch) { InterlockedExchange(&g_active, 0); g_watch = nullptr; }
}

static void post() {
    if (!g_active || g_lines >= 240) return;
    int32_t cnt = -1, st = -1, b338 = -1, b33c = -1;
    __try {
        uint8_t* hf = *g_hdframe;
        if (hf) cnt = *(int32_t*)(hf + 0xE9CC);
        uint8_t* s = *g_state;
        if (s) { st = *(int32_t*)s; b338 = s[0x338]; b33c = s[0x33C]; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    static const char* K[] = {"LOGIC", "RENDER", "BACKLOG", "STOCK"};
    log_line("FTBLINK %s #%d %s list=%d state=%d b338=%d b33c=%d", g_name, g_lines, K[pacing_kind() & 3], cnt, st, b338, b33c);
    g_lines++;
}

static bool install() {
    uint8_t* t = sig_find("X2-DIAG-FTBLINK HD list clear test",
        "A3 ?? ?? ?? ?? 80 B8 38 03 00 00 00 75 0E 80 B8 3C 03 00 00 00 75 05 83 38 01 74 07 8B CE E8", 0x464FEF);
    uint8_t* a = sig_find("X2-DIAG-FTBLINK loop-3 model add",
        "FF B6 44 08 00 00 E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 08 E8 ?? ?? ?? ?? 3D 2B 02 00 00", 0x6DCD92);
    uint8_t* thunk = a ? call_target(a + 6) : nullptr;
    if (!t || !thunk || !sig_match(thunk, "55 8B EC FF 75 08 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 5D C3")) {
        log_line("  X2-DIAG-FTBLINK: sites not as proven - off");
        return false;
    }
    uint8_t* add = call_target(thunk + 12);
    if (!add || !sig_match(add, "55 8B EC FF 81 CC E9 00 00")) { log_line("  X2-DIAG-FTBLINK: list add +0xE9CC not proven - off"); return false; }
    g_state = (uint8_t**)abs_operand("HD state ptr", t + 1);
    g_hdframe = (uint8_t**)abs_operand("hdframe ptr", thunk + 8);
    if (!g_state || !g_hdframe) return false;
    char buf[128];
    config_str("rows", "X2-DIAG-FTBLINK-DLL", buf, sizeof(buf), "681 80");
    for (char* p = buf; *p && g_nlist < 8;) {
        char* e = nullptr; long v = strtol(p, &e, 10);
        if (e == p) { p++; continue; }
        g_list[g_nlist++] = (int)v; p = e;
    }
    if (!magic_mods_subscribe("X2-DIAG-FTBLINK", on_load, on_unload)) return false;
    log_line("  X2-DIAG-FTBLINK: state ptr gva=0x%08X (expected 0x00DCED88), hdframe ptr gva=0x%08X (expected 0x00DCDA04)",
             rt_to_gva(g_state), rt_to_gva(g_hdframe));
    return true;
}

Row row_DIAGFTBLINK = {
    "X2-DIAG-FTBLINK",
    "diagnostic: HD model list count / clear state per invocation while magic_0681 (or listed DLLs) is loaded",
    0, install, nullptr, post, 0
};

} // namespace x2
