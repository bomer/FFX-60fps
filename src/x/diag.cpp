// SPDX-License-Identifier: Apache-2.0
// Breadcrumbs for the spell-load hang, observe only: g_where (where the game thread is in the invocation)
// and ldr_* (which loader-notification callback runs, on which thread, for which module, at which step).
// The DIAG-HANGDUMP watchdog prints both. Details: docs/fps60/rows/DIAG-HANGDUMP.md, re/ledger/HANG-SPELLLOAD.md
#include "fps60.h"
#include <stdio.h>

volatile LONG  g_where = W_BETWEEN | (0xFF << 8);

static const char* volatile g_ldr_row = nullptr;
static volatile DWORD g_ldr_tid = 0;
static volatile DWORD g_ldr_since = 0;           // GetTickCount at enter
static volatile LONG  g_ldr_in = 0;              // 1 between enter and leave
static volatile LONG  g_ldr_step = 0;
static volatile ULONG g_ldr_reason = 0;
static const void* volatile g_ldr_base = nullptr;
static volatile LONG  g_ldr_calls = 0;

void ldr_enter(const char* row, ULONG reason, const void* base) {
    g_ldr_in = 0;
    g_ldr_row = row;
    g_ldr_tid = GetCurrentThreadId();
    g_ldr_reason = reason;
    g_ldr_base = base;
    g_ldr_step = 0;
    g_ldr_since = GetTickCount();
    g_ldr_in = 1;
    InterlockedIncrement(&g_ldr_calls);
}

void ldr_step(int step) { g_ldr_step = step; }

void ldr_leave() { g_ldr_in = 0; }

void ldr_text(char* out, size_t cap) {
    const char* row = g_ldr_row;
    _snprintf_s(out, cap, _TRUNCATE, "loader callback %s: row %s, step %ld, reason %lu (%s), module %p, thread %lu, %lu ms ago, "
                "%ld callbacks so far",
                g_ldr_in ? "INSIDE" : "last left", row ? row : "-", g_ldr_step, g_ldr_reason,
                g_ldr_reason == 1 ? "load" : g_ldr_reason == 2 ? "unload" : "?", g_ldr_base, g_ldr_tid,
                GetTickCount() - g_ldr_since, g_ldr_calls);
}

void where_text(LONG w, char* out, size_t cap) {
    static const char* const phases[] = { "between invocations", "fp_pre start", "pacing_pre", "rollback_pre", "rows_pre",
                                          "inside Sg_MainLoop", "rows_post", "rollback_post", "pacing_post" };
    int ph = w & 0xFF, ri = (w >> 8) & 0xFF, sub = (w >> 16) & 0xFF;
    const Row* r = (ri != 0xFF) ? row_at(ri) : nullptr;
    const char* sn = "";
    switch (sub) {                               // set inside Sg_MainLoop by MAG-008 and SMOOTH-003
    case 0x21: sn = " (MAG-008 skip: MAG-021 DLL update replay)"; break;
    case 0x22: sn = " (after the MAG-021 replay)"; break;
    case 0x23: sn = " (MAG-008: battle scene draw dispatch)"; break;
    case 0x24: sn = " (after the battle scene draw dispatch)"; break;
    case 0x31: sn = " (SMOOTH-003 render-only walk)"; break;
    case 0x32: sn = " (SMOOTH-003 skeleton job sync)"; break;
    case 0x33: sn = " (SMOOTH-003 skeleton job dispatch)"; break;
    case 0x34: sn = " (original Ch_NoSkelPass)"; break;
    case 0x35: sn = " (SMOOTH-003 cloth fix-up)"; break;
    case 0x36: sn = " (after Ch_NoSkelPass)"; break;
    }
    _snprintf_s(out, cap, _TRUNCATE, "where=0x%06lX: %s%s%s, sub-step 0x%02X%s", w,
                ph < (int)(sizeof(phases) / sizeof(phases[0])) ? phases[ph] : "?",
                r ? " / row " : "", r ? r->id : "", sub, sn);
}
