// SPDX-License-Identifier: Apache-2.0
// DIAG-DYNGEO (diagnostic, observe only): tests whether particles drawn on render-only invocations upload an empty
// mesh (re/ledger/FX-RENDERCACHE.md item 3). The dynamic-geometry manager alternates two buffer segments per frame;
// the upload reader FUN_006732a0 takes a sub-mesh's counts from the current segment's slot (+0x2C/+0x30 + seg*0x1C)
// when its pointer +0x34 is set, else from the live counts +4/+8, and turns a 0 count into a 4/6 placeholder.
// Counts per invocation kind and segment: sub-meshes read, fallbacks, placeholders; and FUN_0063fdb0 (segment
// alloc) returning 0 (segment full), with its first callers. Logged every 600 invocations.
#include "../fps60.h"
#include <intrin.h>
#include <stdio.h>

typedef void(__thiscall* reader_t)(void* self, int geo);
typedef int(__cdecl* alloc_t)(int, int, int, int, int, int, int, int);
typedef int(__cdecl* seg_t)(void);

static uint8_t*  g_reader_fn = nullptr;
static uint8_t*  g_alloc_fn = nullptr;
static reader_t  g_reader = nullptr;
static alloc_t   g_alloc = nullptr;
static seg_t     g_seg = nullptr;
static volatile int32_t* g_seg_enabled = nullptr;   // DAT_00c32694 (reader uses segments only when != 0)
static bool      g_hooked = false;

// [kind 0..3][seg -1/0/1 -> 0..2]
static volatile LONG g_sub[4][3], g_fallback[4][3], g_placeholder[4][3], g_alloc_fail[4][3], g_calls[4][3];
static uint32_t g_fail_ret[8];
static volatile LONG g_fail_ret_n = 0;

static int seg_index(int s) { return s < 0 ? 0 : (s > 1 ? 2 : s + 1); }

static void __fastcall h_reader(void* self, void* /*edx*/, int geo) {
    g_reader(self, geo);
    int k = pacing_kind() & 3;
    int s = g_seg();
    int si = seg_index(s);
    InterlockedIncrement(&g_calls[k][si]);
    __try {
        int n = *(int*)(geo + 0x90);
        uint8_t* rec = *(uint8_t**)(geo + 0x94);
        for (int i = 0; i < n && i < 256; i++, rec += 0x6C) {
            InterlockedIncrement(&g_sub[k][si]);
            bool fb = s < 0 || *g_seg_enabled == 0 || *(int*)(rec + 0x34 + s * 0x1C) == 0;
            if (fb) {
                InterlockedIncrement(&g_fallback[k][si]);
                if (*(int*)(rec + 4) == 0) InterlockedIncrement(&g_placeholder[k][si]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static int __cdecl h_alloc(int a, int b, int c, int d, int e, int f, int g, int h) {
    int r = g_alloc(a, b, c, d, e, f, g, h);
    if (r == 0) {
        int k = pacing_kind() & 3;
        int si = seg_index(g_seg());
        InterlockedIncrement(&g_alloc_fail[k][si]);
        LONG i = InterlockedIncrement(&g_fail_ret_n) - 1;
        if (i < 8) g_fail_ret[i] = rt_to_gva_exe(_ReturnAddress());
    }
    return r;
}

static bool install() {
    // FUN_006732a0 (thiscall geo; RET 4): PUSH EBP..MOV EBX,[ESI+0x90]; MOV EDI,ECX; MOV [EBP-4],EBX; CALL seg getter
    g_reader_fn = sig_find_unique("DIAG-DYNGEO upload reader FUN_006732a0",
        "55 8B EC 83 EC 10 53 56 8B 75 08 57 8B 9E 90 00 00 00 8B F9 89 5D FC E8", 0x6732A0);
    // FUN_0063fdb0 (cdecl; plain RET 0x63FE1B): segment alloc
    g_alloc_fn = sig_find_unique("DIAG-DYNGEO segment alloc FUN_0063fdb0",
        "55 8B EC 83 EC 1C 8B 4D 10 53 56 8D 04 49 57 8D 1C 85 FF FF FF FF 8B 45 14", 0x63FDB0);
    if (!g_reader_fn || !g_alloc_fn) return false;
    g_seg = (seg_t)call_target(g_reader_fn + 0x17);
    // FUN_0063ff50: MOV ECX,[mgr]; JMP FUN_006de9f0
    if (!g_seg || !sig_match((void*)g_seg, "8B 0D ?? ?? ?? ?? E9")) {
        log_line("  DIAG-DYNGEO: segment getter not found - refused");
        return false;
    }
    if (rt_to_gva_exe(g_reader_fn) != 0x6732A0) {
        log_line("  DIAG-DYNGEO: reader not at its expected VA (other build) - refused");
        return false;
    }
    g_seg_enabled = (volatile int32_t*)(exe_base() + (0x00C32694 - 0x400000));
    log_line("  DIAG-DYNGEO: reader gva=0x%08X, alloc gva=0x%08X, segment getter gva=0x%08X",
             rt_to_gva_exe(g_reader_fn), rt_to_gva_exe(g_alloc_fn), rt_to_gva_exe((void*)g_seg));
    return true;
}

static void pre() {
    if (g_hooked) return;
    g_hooked = true;
    if (!hook_entry("DIAG-DYNGEO reader", g_reader_fn, (void*)&h_reader, (void**)&g_reader) ||
        !hook_entry("DIAG-DYNGEO alloc", g_alloc_fn, (void*)&h_alloc, (void**)&g_alloc)) {
        hook_remove("DIAG-DYNGEO reader", g_reader_fn);
        log_line("DIAG-DYNGEO: hook failed - row off");
        row_DIAG_DYNGEO.on = 0;
        return;
    }
    log_line("DIAG-DYNGEO: hooked on the game thread");
}

static void post() {
    static uint32_t tick = 0;
    if (!g_hooked || ++tick % 600) return;
    static const char* kn[4] = {"logic", "render-only", "backlog", "stock"};
    log_line("DIAG-DYNGEO: last 600 invocations (kind/seg: reader calls, sub-meshes, fallbacks, placeholders 4/6, alloc fails):");
    for (int k = 0; k < 4; k++)
        for (int s = 0; s < 3; s++) {
            if (!g_calls[k][s] && !g_alloc_fail[k][s]) continue;
            log_line("  %-11s seg %2d: calls %ld, sub %ld, fallback %ld, placeholder %ld, alloc fail %ld", kn[k], s - 1,
                     g_calls[k][s], g_sub[k][s], g_fallback[k][s], g_placeholder[k][s], g_alloc_fail[k][s]);
        }
    LONG nf = g_fail_ret_n;
    if (nf) {
        char line[160]; int o = 0;
        for (int i = 0; i < nf && i < 8; i++) o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " %08X", g_fail_ret[i]);
        log_line("  alloc fail callers (first 8 ever):%s", line);
    }
    for (int k = 0; k < 4; k++)
        for (int s = 0; s < 3; s++)
            g_calls[k][s] = g_sub[k][s] = g_fallback[k][s] = g_placeholder[k][s] = g_alloc_fail[k][s] = 0;
}

Row row_DIAG_DYNGEO = {
    "DIAG-DYNGEO",
    "diagnostic: dynamic-geometry upload per invocation kind and segment (fallbacks, 4/6 placeholders, alloc fails)",
    0, install, pre, post, 0
};
