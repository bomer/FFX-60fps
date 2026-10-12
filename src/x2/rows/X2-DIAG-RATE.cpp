// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-RATE (diagnostic, default 0): which code steps how often per second. Every ~5 s it logs the rate of
// Sg_MainLoop invocations by kind, plus calls of a few functions counted by naked entry hooks (any convention):
//   actor update FUN_006e10a0 (exec loop, expected LOGIC), battle process FUN_00790f00 (its own entry counter
//   [0x016E782C] is read too), system layer FUN_00466ef0 and FUN_0042f8d0 (once per host frame, outside Sg_MainLoop).
// Used for the title-screen / battle 2x report (U 12/10/2026). Each target is checked against its prologue bytes
// (decomp export) before it is hooked; a mismatch skips that target.
#include "../x2.h"
#include <stdio.h>

namespace x2 {

struct Probe {
    const char*   name;
    uint32_t      gva;
    const char*   prologue;
    void*         tramp;
    volatile LONG calls;
    LONG          last;
};

static Probe g_p[4] = {
    { "actor update FUN_006e10a0",  0x6E10A0, "55 8B EC 83 EC 10 E8" },
    { "battle process FUN_00790f00", 0x790F00, "FF 05 ?? ?? ?? ?? 53 56 57" },
    { "system layer FUN_00466ef0",  0x466EF0, "55 8B EC 6A FF 68" },
    { "host frame FUN_0042f8d0",    0x42F8D0, "56 8B F1 80 BE AC 03 00 00 00 74" },
};

// One naked detour per probe: count, then continue in the trampoline (registers and stack untouched).
#define PROBE_STUB(i) \
    __declspec(naked) static void stub##i() { __asm { lock inc dword ptr [g_p + i * TYPE Probe + 16] } \
                                              __asm { jmp dword ptr [g_p + i * TYPE Probe + 12] } }
PROBE_STUB(0)
PROBE_STUB(1)
PROBE_STUB(2)
PROBE_STUB(3)
static void (*const g_stubs[4])() = { stub0, stub1, stub2, stub3 };

static volatile uint32_t* g_btl_counter = nullptr;
static uint32_t g_btl_last = 0;
static bool     g_hooked = false;
static LARGE_INTEGER g_t0, g_f;
static uint64_t g_kind[4], g_kind_last[4];
extern Row row_DIAGRATE;

static bool install() {
    static_assert(offsetof(Probe, tramp) == 12 && offsetof(Probe, calls) == 16, "stub offsets");
    for (auto& p : g_p) {
        uint8_t* at = exe_base() + (p.gva - 0x400000);
        if (!sig_match(at, p.prologue)) { log_line("  X2-DIAG-RATE: %s prologue mismatch - not counted", p.name); p.gva = 0; }
    }
    if (g_p[1].gva) g_btl_counter = (volatile uint32_t*)abs_operand("battle entry counter", exe_base() + (0x790F00 - 0x400000) + 2);
    QueryPerformanceFrequency(&g_f);
    QueryPerformanceCounter(&g_t0);
    return true;
}

static void pre() {
    g_kind[pacing_kind()]++;
    if (g_hooked) return;
    g_hooked = true;
    for (int i = 0; i < 4; i++) {
        if (!g_p[i].gva) continue;
        uint8_t* at = exe_base() + (g_p[i].gva - 0x400000);
        if (!hook_entry(g_p[i].name, at, (void*)g_stubs[i], &g_p[i].tramp)) g_p[i].gva = 0;
    }
    if (g_btl_counter) g_btl_last = *g_btl_counter;
    log_line("X2-DIAG-RATE: probes hooked");
}

static void post() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    double s = (double)(t.QuadPart - g_t0.QuadPart) / (double)g_f.QuadPart;
    if (s < 5.0) return;
    g_t0 = t;
    char line[600];
    int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "X2-DIAG-RATE /s: inv LOGIC %.1f BUILD %.1f BACKLOG %.1f STOCK %.1f |",
                        (g_kind[0] - g_kind_last[0]) / s, (g_kind[1] - g_kind_last[1]) / s,
                        (g_kind[2] - g_kind_last[2]) / s, (g_kind[3] - g_kind_last[3]) / s);
    for (int i = 0; i < 4; i++) g_kind_last[i] = g_kind[i];
    for (auto& p : g_p) {
        if (!p.gva) continue;
        LONG c = p.calls;
        k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %s %.1f;", p.name, (c - p.last) / s);
        p.last = c;
    }
    if (g_btl_counter) {
        uint32_t c = *g_btl_counter;
        k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " battle counter %.1f", (c - g_btl_last) / s);
        g_btl_last = c;
    }
    log_line("%s", line);
}

Row row_DIAGRATE = {
    "X2-DIAG-RATE",
    "diagnostic: per-second rates of invocation kinds and key update functions",
    0, install, pre, post, 0
};

} // namespace x2
