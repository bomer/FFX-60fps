// SPDX-License-Identifier: Apache-2.0
// Field/map particles (rain, lightning, field lights) step on every draw and run at 2x. On render-only
// frames their step runs with the particle pause flag set, as MAG-006 does for DLL programs. Off by default.
// Details: docs/fps60/rows/FX-010.md
#include "../fps60.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int      ppp_step_paused(int (__cdecl* tramp)(void*, uint32_t), void* prog, uint32_t flags);
bool     ppp_resolve_pause(const char* row, const uint8_t* cmp_at);
bool     ppp_hook_spawn_call(const char* row, uint8_t* site);
extern volatile int32_t* g_ppp_pause;

typedef int(__cdecl* fp_step_t)(void* prog, uint32_t flags);

static uint8_t*  g_entry = nullptr;
static fp_step_t g_tramp = nullptr;
static bool      g_patched = false;
static volatile LONG g_paused_calls = 0, g_fresh_calls = 0;


// ---- DIAG-FX010SUBMIT (diagnostic): counts draw submits FUN_006458a0 made inside field particle steps, per caller
// (return address) and per program index, split by unpaused / paused step. A caller or program that submits when
// unpaused but not when paused is what flickers under FX-010.
// hooked draw-side functions: submit FUN_006458a0, twin FUN_00645ba0, FUN_00645aa0, FUN_00645400, packet alloc
// FUN_007e6a00 (each op that builds a packet calls it)
enum { NSUB = 7 };
static const char* const k_sub_name[NSUB] = {"0x6458A0", "0x645BA0", "0x645AA0", "0x645400", "0x7E6A00", "0x641460", "0x7166D0"};
static uint8_t* g_submit_fn[NSUB] = {};
static void*    g_submit_tramp[NSUB] = {};
static volatile int   g_step_mode = 0;          // 0 outside a step, 1 unpaused step, 2 paused step
static DWORD          g_step_tid = 0;
static int            g_step_submits = 0;
struct SubmitCaller { uint32_t ret; int fn; uint32_t n[3]; };
static SubmitCaller g_sc[64];
static int          g_sc_count = 0;
struct ProgStat { uint32_t calls[3], with_submit[3]; };
static ProgStat     g_ps[128];

static void __cdecl submit_record(uint32_t ret, int fn) {
    int m = g_step_mode;
    if (!m || GetCurrentThreadId() != g_step_tid) return;
    g_step_submits++;
    // once per (caller, mode): log the call stack (Ghidra VAs) so the dispatcher of a draw op can be found
    static uint32_t traced[16][2];
    static int traced_n = 0;
    bool seen = false;
    for (int i = 0; i < traced_n; i++) if (traced[i][0] == ret && traced[i][1] == (uint32_t)m) seen = true;
    if (!seen && traced_n < 16) {
        traced[traced_n][0] = ret; traced[traced_n][1] = (uint32_t)m; traced_n++;
        void* fr[12] = {};
        USHORT k = RtlCaptureStackBackTrace(1, 12, fr, nullptr);
        char line[300]; int o = 0;
        for (USHORT j = 0; j < k && o < 280; j++) {
            uint32_t g = rt_to_gva_exe(fr[j]);
            o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " %08X", g ? g : (uint32_t)(uintptr_t)fr[j]);
        }
        line[o] = 0;
        log_line("DIAG-FX010SUBMIT: stack for caller 0x%08X (%s):%s", rt_to_gva_exe((void*)(uintptr_t)ret),
                 m == 2 ? "paused" : "unpaused", line);
    }
    for (int i = 0; i < g_sc_count; i++) if (g_sc[i].ret == ret && g_sc[i].fn == fn) { g_sc[i].n[m]++; return; }
    if (g_sc_count < 64) { g_sc[g_sc_count].ret = ret; g_sc[g_sc_count].fn = fn; g_sc[g_sc_count].n[m] = 1; g_sc_count++; }
}

#define SUBMIT_THUNK(I) __declspec(naked) static void h_submit##I() {     __asm mov eax, [esp]     __asm push I     __asm push eax     __asm call submit_record     __asm add esp, 8     __asm jmp dword ptr [g_submit_tramp + I * 4] }
SUBMIT_THUNK(0)
SUBMIT_THUNK(1)
SUBMIT_THUNK(2)
SUBMIT_THUNK(3)
SUBMIT_THUNK(4)
SUBMIT_THUNK(5)
SUBMIT_THUNK(6)
static void* const k_sub_thunk[NSUB] = {(void*)&h_submit0, (void*)&h_submit1, (void*)&h_submit2, (void*)&h_submit3, (void*)&h_submit4,
                                         (void*)&h_submit5, (void*)&h_submit6};

static int prog_index(void* prog);

static void diag_step_begin(int mode) { g_step_tid = GetCurrentThreadId(); g_step_submits = 0; g_step_mode = mode; }
static void diag_step_end(void* prog, int mode) {
    g_step_mode = 0;
    int i = prog_index(prog);
    if (i < 0 || i >= 128) return;
    g_ps[i].calls[mode]++;
    if (g_step_submits) g_ps[i].with_submit[mode]++;
}

static void diag_dump() {
    log_line("DIAG-FX010SUBMIT: submit callers inside field particle steps (gva: unpaused / paused):");
    for (int i = 0; i < g_sc_count; i++)
        log_line("  caller 0x%08X -> %s: %u / %u", rt_to_gva_exe((void*)(uintptr_t)g_sc[i].ret),
                 k_sub_name[g_sc[i].fn], g_sc[i].n[1], g_sc[i].n[2]);
    log_line("DIAG-FX010SUBMIT: programs (index: unpaused steps with submit/steps; paused ...):");
    for (int i = 0; i < 128; i++)
        if (g_ps[i].calls[1] || g_ps[i].calls[2])
            log_line("  prog %3d: unpaused %u/%u, paused %u/%u", i, g_ps[i].with_submit[1], g_ps[i].calls[1],
                     g_ps[i].with_submit[2], g_ps[i].calls[2]);
    memset(g_sc, 0, sizeof(g_sc)); g_sc_count = 0;
    memset(g_ps, 0, sizeof(g_ps));
}
static bool g_diag_submit = false;
static uint32_t g_find_gva = 0;
static bool find_at_accept(const uint8_t* m) { return rt_to_gva_exe(m) == g_find_gva; }
static uint8_t* diag_find_at(const char* pat, uint32_t gva) {   // diagnostic only: the match at this VA
    int hits = 0;
    g_find_gva = gva;
    uint8_t* m = sig_scan_module(exe_base(), 0, pat, &hits, find_at_accept);
    if (!m) log_line("  DIAG-FX010SUBMIT: no match at gva=0x%08X", gva);
    return m;
}
// diagnostic only: program table DAT_00d2c260 by VA (FX-010 proved the build at its expected VA)
static int prog_index(void* prog) {
    uint8_t* b = exe_base() + (0x00D2C260 - 0x400000);
    intptr_t d = (uint8_t*)prog - b;
    return (d >= 0 && d % 0x80 == 0) ? (int)(d / 0x80) : -1;
}

// FX-010-PROGS=a-b (test switch, bisect): pause only programs whose slot index is in [a, b]; default all.
static int g_prog_lo = 0, g_prog_hi = 0x7FFFFFFF;
static volatile LONG g_prog_seen[128];

// DIAG-FX010-OPS: log a selected program's groups and ops (FUN_00717010 layout: prog +0x18 group count, +0x3C groups
// stride 0x10 {+0 group def, +4 first particle, +0xC u16 particle count}; group def +0xC flags, +0x26 op count, +0x28
// ops stride 0x10 {+0 op def, +4 u16 data offset}; op def +4 / +8 / +0xC functions). Handles are plain pointers.
static bool g_diag_ops = false;
static uint32_t gv(uint32_t p) { uint32_t g = p ? rt_to_gva_exe((void*)(uintptr_t)p) : 0; return g ? g : p; }
static void log_prog_ops(int idx, uint8_t* prog) {
    __try {
        int ng = *(int*)(prog + 0x18);
        uint8_t* groups = *(uint8_t**)(prog + 0x3C);
        log_line("DIAG-FX010-OPS: program %d: %d groups", idx, ng);
        for (int g = 0; g < ng && g < 32 && groups; g++) {
            uint8_t* grp = groups + g * 0x10;
            uint8_t* def = *(uint8_t**)grp;
            if (!def) continue;
            int nop = *(int16_t*)(def + 0x26);
            log_line("  group %d: def %p flags 0x%X, particles %u, ops %d", g, def, *(uint8_t*)(def + 0xC),
                     *(uint16_t*)(grp + 0xC), nop);
            for (int o = 0; o < nop && o < 32; o++) {
                uint8_t* opd = *(uint8_t**)(def + 0x28 + o * 0x10);
                if (!opd) { log_line("    op %d: null", o); continue; }
                log_line("    op %d: +4 0x%08X +8 0x%08X +C 0x%08X (data +0x%X)", o, gv(*(uint32_t*)(opd + 4)),
                         gv(*(uint32_t*)(opd + 8)), gv(*(uint32_t*)(opd + 0xC)), *(uint16_t*)(def + 0x28 + o * 0x10 + 4));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { log_line("DIAG-FX010-OPS: program %d: walk faulted", idx); }
}

static bool prog_selected(void* prog) {
    int i = prog_index(prog);
    if (i >= 0 && i < 128 && !InterlockedExchange(&g_prog_seen[i], 1)) {
        log_line("FX-010: field program %d first stepped (%s)", i,
                 (i >= g_prog_lo && i <= g_prog_hi) ? "paused on render-only" : "NOT paused (FX-010-PROGS)");
        if (g_diag_ops && i >= g_prog_lo && i <= g_prog_hi) log_prog_ops(i, (uint8_t*)prog);
    }
    return i < 0 || (i >= g_prog_lo && i <= g_prog_hi);
}

// DIAG-FX010-WRITES: for programs in FX-010-PROGS, snapshot the program entry (0x80) and each particle record
// (first WR_REC bytes; groups +0x3C, first particle +4, count u16 +0xC, next = *particle) before every step, diff
// after it, and count changed dwords per (group, offset) and mode (1 unpaused, 2 paused). Logged every 600.
enum { WR_REC = 0x400, WR_MAXP = 64, WR_KEYS = 512 };
static bool     g_diag_writes = false;
static uint8_t  g_wr_buf[WR_MAXP + 1][WR_REC];
static uint8_t* g_wr_ptr[WR_MAXP + 1];
static int      g_wr_grp[WR_MAXP + 1];
static int      g_wr_n = 0;
struct WrKey { int grp; int off; uint32_t n[3]; uint32_t last[3]; };
static WrKey    g_wr[WR_KEYS];
static int      g_wr_keys = 0;
static uint32_t g_wr_steps[3];

static void wr_snapshot(uint8_t* prog) {
    g_wr_n = 0;
    __try {
        g_wr_ptr[g_wr_n] = prog; g_wr_grp[g_wr_n] = -1; memcpy(g_wr_buf[g_wr_n], prog, 0x80); g_wr_n++;
        int ng = *(int*)(prog + 0x18);
        uint8_t* groups = *(uint8_t**)(prog + 0x3C);
        for (int g = 0; g < ng && g < 32 && groups; g++) {
            uint8_t* grp = groups + g * 0x10;
            uint8_t* part = *(uint8_t**)(grp + 4);
            int cnt = *(uint16_t*)(grp + 0xC);
            for (int k = 0; k < cnt && part && g_wr_n <= WR_MAXP; k++) {
                g_wr_ptr[g_wr_n] = part; g_wr_grp[g_wr_n] = g;
                memcpy(g_wr_buf[g_wr_n], part, WR_REC); g_wr_n++;
                part = *(uint8_t**)part;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void wr_count(int grp, int off, int mode, uint32_t val) {
    for (int i = 0; i < g_wr_keys; i++)
        if (g_wr[i].grp == grp && g_wr[i].off == off) { g_wr[i].n[mode]++; g_wr[i].last[mode] = val; return; }
    if (g_wr_keys < WR_KEYS) { g_wr[g_wr_keys] = {grp, off, {0, 0, 0}, {0, 0, 0}}; g_wr[g_wr_keys].n[mode] = 1; g_wr[g_wr_keys].last[mode] = val; g_wr_keys++; }
}

static void wr_diff(int mode) {
    g_wr_steps[mode]++;
    __try {
        for (int i = 0; i < g_wr_n; i++) {
            int len = g_wr_grp[i] < 0 ? 0x80 : WR_REC;
            for (int o = 0; o < len; o += 4)
                if (*(uint32_t*)(g_wr_ptr[i] + o) != *(uint32_t*)(g_wr_buf[i] + o))
                    wr_count(g_wr_grp[i], o, mode, *(uint32_t*)(g_wr_ptr[i] + o));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void wr_dump() {
    log_line("DIAG-FX010-WRITES: steps unpaused %u, paused %u; changed dwords (group -1 = program entry): group+offset: unpaused / paused (last values)",
             g_wr_steps[1], g_wr_steps[2]);
    for (int i = 0; i < g_wr_keys; i++)
        log_line("  g%d +0x%03X: %u / %u (0x%08X / 0x%08X)", g_wr[i].grp, g_wr[i].off, g_wr[i].n[1], g_wr[i].n[2],
                 g_wr[i].last[1], g_wr[i].last[2]);
    g_wr_keys = 0; g_wr_steps[1] = g_wr_steps[2] = 0;
}

// Per-phase diff on paused steps of selected programs: pre callbacks FUN_00712e80, particle ops FUN_00717010, post
// callbacks FUN_00712e00 (CALLs at step +0x17, +0x53, +0x6A). Counts changed dwords per phase: all, and the
// particle matrix block +0x10..+0x4C (seen re-initialised on paused steps).
typedef void(__cdecl* phase_t)(void*);
typedef int(__cdecl* opfn_t)(int, int, int, int);
enum { NPH = 13, NOPS = NPH - 3 };                 // 0..2 phases, 3.. ungated ops (river program, or DIAG-FX010-WRITES-OPS)
static void*    g_ph_tramp[NPH] = {};
static uint8_t* g_ph_fn[NPH] = {};
static volatile int g_ph_active = 0;              // 1 while a selected paused step runs (game thread)
static int      g_ph_depth = 0;                   // only the outermost hooked call snapshots
static uint8_t  g_ph_buf[2][WR_MAXP + 1][WR_REC];   // [0] phases, [1] ops (nested inside phase 1)
static uint8_t* g_ph_ptr[2][WR_MAXP + 1];
static int      g_ph_grp[2][WR_MAXP + 1];
static int      g_ph_n[2] = {};
static uint32_t g_ph_all[NPH], g_ph_mat[NPH], g_ph_calls[NPH];
static uint32_t g_ph_first[NPH][4];
static uint32_t g_ph_offs[NPH][12];             // first distinct changed group<<16|offset per phase / op
static int      g_ph_offs_n[NPH];
static int      g_ph_first_n[NPH];

static void ph_before(int B) {
    g_ph_n[B] = 0;
    __try {
        for (int i = 0; i < g_wr_n; i++) {
            g_ph_ptr[B][g_ph_n[B]] = g_wr_ptr[i]; g_ph_grp[B][g_ph_n[B]] = g_wr_grp[i];
            memcpy(g_ph_buf[B][g_ph_n[B]], g_wr_ptr[i], g_wr_grp[i] < 0 ? 0x80 : WR_REC); g_ph_n[B]++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ph_after(int B, int P) {
    g_ph_calls[P]++;
    __try {
        for (int i = 0; i < g_ph_n[B]; i++) {
            int len = g_ph_grp[B][i] < 0 ? 0x80 : WR_REC;
            for (int o = 0; o < len; o += 4)
                if (*(uint32_t*)(g_ph_ptr[B][i] + o) != *(uint32_t*)(g_ph_buf[B][i] + o)) {
                    g_ph_all[P]++;
                    uint32_t k = (uint32_t)(g_ph_grp[B][i] << 16 | o);
                    int j = 0;
                    while (j < g_ph_offs_n[P] && g_ph_offs[P][j] != k) j++;
                    if (j == g_ph_offs_n[P] && j < 12) g_ph_offs[P][g_ph_offs_n[P]++] = k;
                    if (g_ph_grp[B][i] >= 0 && o >= 0x10 && o < 0x50) {
                        g_ph_mat[P]++;
                        if (g_ph_first_n[P] < 4) g_ph_first[P][g_ph_first_n[P]++] = (uint32_t)(g_ph_grp[B][i] << 16 | o);
                    }
                }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

template <int P> static void __cdecl h_phase(void* prog) {
    if (!g_ph_active) { ((phase_t)g_ph_tramp[P])(prog); return; }
    ph_before(0);
    ((phase_t)g_ph_tramp[P])(prog);
    ph_after(0, P);
}
// ops are cdecl with 3-4 args (caller cleans): pass 4 through
template <int P> static int __cdecl h_op(int a, int b, int c, int d) {
    if (!g_ph_active || g_ph_depth) return ((opfn_t)g_ph_tramp[P])(a, b, c, d);
    g_ph_depth++;
    ph_before(1);
    int r = ((opfn_t)g_ph_tramp[P])(a, b, c, d);
    ph_after(1, P);
    g_ph_depth--;
    return r;
}
static void* const k_phase_thunk[NPH] = {(void*)&h_phase<0>, (void*)&h_phase<1>, (void*)&h_phase<2>,
    (void*)&h_op<3>, (void*)&h_op<4>, (void*)&h_op<5>, (void*)&h_op<6>, (void*)&h_op<7>, (void*)&h_op<8>,
    (void*)&h_op<9>, (void*)&h_op<10>, (void*)&h_op<11>, (void*)&h_op<12>};
static uint32_t k_op_va[NOPS] = {0x72D390, 0x72D1B0, 0x734BD0, 0x7354C0, 0x740ED0};
static const char* k_op_pat[NOPS] = {
    "55 8B EC 51 8B 45 10 8B 4D 08 8B 40", "55 8B EC 51 8B 45 10 8B 4D 08 8B 40",
    "55 8B EC 56 8B 35 ?? ?? ?? ?? 57 8B", "55 8B EC 83 EC 58 A1 ?? ?? ?? ?? 33", "55 8B EC 51 56 57 8B 7D 08 FF 77 04"};
static int k_op_n = 5;
static char g_ph_nm[NPH][40];

static void ph_dump() {
    static const char* nm[3] = {"pre callbacks 0x712E80", "particle ops 0x717010", "post callbacks 0x712E00"};
    for (int p = 0; p < 3 + k_op_n; p++) {
        if (p >= 3) { if (!g_ph_fn[p]) continue; sprintf_s(g_ph_nm[p], "op 0x%06X", k_op_va[p - 3]); }
        log_line("DIAG-FX010-WRITES phase %s: paused calls %u, changed dwords %u, matrix block +0x10..+0x4C %u (first: %X %X %X %X)",
                 p < 3 ? nm[p] : g_ph_nm[p], g_ph_calls[p], g_ph_all[p], g_ph_mat[p], g_ph_first[p][0], g_ph_first[p][1], g_ph_first[p][2], g_ph_first[p][3]);
        if (g_ph_offs_n[p]) {
            char b[160]; int n = 0;
            for (int j = 0; j < g_ph_offs_n[p]; j++)
                n += sprintf_s(b + n, sizeof(b) - n, " g%d+0x%X", (int)(int16_t)(g_ph_offs[p][j] >> 16), g_ph_offs[p][j] & 0xFFFF);
            log_line("    changed fields:%s", b);
        }
        g_ph_offs_n[p] = 0;
        g_ph_calls[p] = g_ph_all[p] = g_ph_mat[p] = 0; g_ph_first_n[p] = 0;
        g_ph_first[p][0] = g_ph_first[p][1] = g_ph_first[p][2] = g_ph_first[p][3] = 0;
    }
}

// ---- FX-010-LOOPDEFER (default 1): at the end of a logic step the particle loop (FUN_00717010 tail, CALL at
// 0x71723D) restarts looping particles that reached their end: age = loop start, FUN_00712d20 resets every op of the
// particle to its first key and runs the ops' init callbacks (birth state). Stock draws nothing until the next tick's
// ops have run on it; with FX-010 the paused render-only frame in between drew the restarted particles in their raw
// birth state (U 12/10/2026: the far-away pyrefly river blinks; D: DIAG-FX010-WRITES, op 0x72D390 rebuilding the
// matrix from the reset op data on paused calls). Fix: during a paced logic step the restart is queued, and the
// queue is applied at the start of that program's next logic step, before its ops (stock order).
typedef void(__cdecl* restart_t)(void*);
static restart_t g_restart = nullptr;               // FUN_00712d20
static uint8_t*  g_restart_site = nullptr;          // CALL at 0x71723D
static bool      g_loopdefer = false;
static void*     g_defer_prog = nullptr;            // program of the logic step running now (game thread)
struct Deferred { void* prog; void* part; uint32_t stamp; };
static Deferred  g_deferred[256];
static int       g_deferred_n = 0;
static uint32_t  g_inv_stamp = 0;                   // invocation counter (post)
static volatile LONG g_deferred_total = 0, g_deferred_applied = 0, g_deferred_dropped = 0;

// DIAG-FX010-RESTART: the step running now, to log restarts that were not deferred under 60 Hz pacing
static void*    g_rs_prog = nullptr;
static int      g_rs_mode = 0;                   // 0 logic, 1 paused render-only, 2 fresh render-only (unpaused)
static bool     g_diag_restart = false;
static int      g_loopdefer_stale = 0;
static volatile LONG g_raw_restarts = 0;

static void __cdecl h_restart(void* part) {
    if (g_defer_prog && g_deferred_n >= 256) {           // full: evict entries of programs not stepped for 10 s
        int w = 0;
        for (int i = 0; i < g_deferred_n; i++)
            if (g_inv_stamp - g_deferred[i].stamp <= 600) g_deferred[w++] = g_deferred[i];
            else InterlockedIncrement(&g_deferred_dropped);
        g_deferred_n = w;
    }
    if (g_defer_prog && g_deferred_n < 256) {
        g_deferred[g_deferred_n++] = {g_defer_prog, part, g_inv_stamp};
        InterlockedIncrement(&g_deferred_total);
        return;
    }
    if (g_diag_restart && pacing_active() && InterlockedIncrement(&g_raw_restarts) <= 200)
        log_line("DIAG-FX010-RESTART: raw restart prog %d part %p during %s step (%s, queue %d)",
                 g_rs_prog ? prog_index(g_rs_prog) : -1, part,
                 g_rs_mode == 0 ? "logic" : g_rs_mode == 1 ? "paused render-only" : "fresh render-only",
                 g_defer_prog ? "queue full" : "not deferred", g_deferred_n);
    g_restart(part);
}

static bool part_in_prog(uint8_t* prog, void* part) {
    __try {
        int ng = *(int*)(prog + 0x18);
        uint8_t* groups = *(uint8_t**)(prog + 0x3C);
        for (int g = 0; g < ng && g < 64 && groups; g++) {
            uint8_t* p = *(uint8_t**)(groups + g * 0x10 + 4);
            int cnt = *(uint16_t*)(groups + g * 0x10 + 0xC);
            for (int k = 0; k < cnt && p && k < 4096; k++, p = *(uint8_t**)p)
                if (p == part) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// apply (or drop) the queued restarts of one program; drop = the program ended or restarted
static void deferred_flush(void* prog, bool drop) {
    int w = 0;
    for (int i = 0; i < g_deferred_n; i++) {
        Deferred d = g_deferred[i];
        // A program that is not stepped for a while (off-screen water, Baaj ruins) must still get its restart at its
        // next logic step: dropped restarts were drawn un-restarted when the effect came into view (yellow flash,
        // U 14/10/2026; D DIAG-FX010-RESTART "stale" drops of programs 67..74). FX-010-LOOPDEFER-STALE = invocations
        // before a queued restart is dropped; 0 (default) = never (program end and part_in_prog still drop).
        bool stale = g_loopdefer_stale > 0 && g_inv_stamp - d.stamp > (uint32_t)g_loopdefer_stale;
        if (d.prog != prog && !stale) { g_deferred[w++] = d; continue; }
        if (!drop && !stale && part_in_prog((uint8_t*)prog, d.part)) {
            g_restart(d.part);
            InterlockedIncrement(&g_deferred_applied);
        } else {
            InterlockedIncrement(&g_deferred_dropped);
            if (g_diag_restart && InterlockedIncrement(&g_raw_restarts) <= 200)
                log_line("DIAG-FX010-RESTART: deferred restart dropped: prog %d part %p, queued %d invocations ago (%s; flushing prog %d)",
                         prog_index(d.prog), d.part, (int)(g_inv_stamp - d.stamp),
                         drop ? "program ended" : stale ? "stale" : "particle no longer in the program", prog_index(prog));
        }
    }
    g_deferred_n = w;
}

// FX-010-ROLLBACK (test switch, default 0): on paused steps of selected programs, save the program entry (0x80) and
// every particle record (first RB_REC bytes) before the step and put them back after it, so a paused step leaves no
// state behind (Besaid light shafts alternate per frame with FX-010; U 12/10/2026).
enum { RB_REC = 0x400, RB_MAXP = 64 };
static bool     g_rollback = false;
static uint8_t  g_rb_buf[RB_MAXP + 1][RB_REC];
static uint8_t* g_rb_ptr[RB_MAXP + 1];
static int      g_rb_len[RB_MAXP + 1];
static int      g_rb_n = 0;
static volatile LONG g_rb_calls = 0;

static void rb_save(uint8_t* prog) {
    g_rb_n = 0;
    __try {
        g_rb_ptr[0] = prog; g_rb_len[0] = 0x80; memcpy(g_rb_buf[0], prog, 0x80); g_rb_n = 1;
        int ng = *(int*)(prog + 0x18);
        uint8_t* groups = *(uint8_t**)(prog + 0x3C);
        for (int g = 0; g < ng && g < 32 && groups; g++) {
            uint8_t* grp = groups + g * 0x10;
            uint8_t* part = *(uint8_t**)(grp + 4);
            int cnt = *(uint16_t*)(grp + 0xC);
            for (int k = 0; k < cnt && part && g_rb_n <= RB_MAXP; k++) {
                g_rb_ptr[g_rb_n] = part; g_rb_len[g_rb_n] = RB_REC; memcpy(g_rb_buf[g_rb_n], part, RB_REC); g_rb_n++;
                part = *(uint8_t**)part;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_rb_n = 0; }
}

static void rb_restore() {
    __try { for (int i = 0; i < g_rb_n; i++) memcpy(g_rb_ptr[i], g_rb_buf[i], g_rb_len[i]); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_rb_n && InterlockedIncrement(&g_rb_calls) == 1)
        log_line("FX-010-ROLLBACK: first paused step rolled back (%d records)", g_rb_n);
    g_rb_n = 0;
}

// FX-010-EXTRAP (default 0: back off 12/10/2026, still finds new flicker cases; test option): paused render-only frames drew particles at their tick state, so field effects
// moved at 30 Hz next to 60 Hz characters (U 12/10/2026: boat splash in Kilika, smooth but 2x with FX-010 off).
// After every unpaused step each particle record is snapshotted (size = group def +0x20 + op count * 4, from the
// spawn FUN_00716770; age at +0xC, list link at +0). Before a paused step, every 4-byte word that changed between
// the last two snapshots of the same particle (age grew) and holds a float in a sane range in both is moved half a
// tick ahead (cur + k * (cur - prev)); words that changed sign are held, a result that would cross zero is clamped
// to 0 (fades, colours). The paused
// step draws from that, then the words are put back. Num 7 toggles it in game (dev builds).
static int  g_extrap = 0, g_extrap_pct = 50;
static LONG g_fx_n = 1;                          // unpaused (logic) invocations; main thread only
static LONG g_ex_parts = 0, g_ex_words = 0, g_ex_lines = 0;
// lo/hi: per word, the range of this life so far; plo/phi: of the previous life of a looping particle (its age went
// back: the loop restart), so a repeated fade is never moved past the peak it reached last loop.
struct PSnap { uint8_t* p; uint32_t len; LONG ncur, nprev, nprev2; uint8_t* cur; uint8_t* prev; uint8_t* prev2;
               float* lo; float* hi; float* plo; float* phi; bool plife; };
enum { PS_N = 4096, EX_MAXW = 1 << 16 };
static PSnap* g_psnap = nullptr;
static uint32_t g_ex_off[EX_MAXW];               // touched words (record pointer + offset) for the restore
static uint8_t* g_ex_rec[EX_MAXW];
static uint32_t g_ex_val[EX_MAXW];
static int g_ex_nw = 0;

static PSnap* psnap_find(uint8_t* p, bool create) {
    uint32_t h = (uint32_t)(((uintptr_t)p >> 4) * 2654435761u) % PS_N;
    PSnap* stale = nullptr;
    for (int i = 0; i < 32; i++) {
        PSnap* e = &g_psnap[(h + i) % PS_N];
        if (e->p == p) return e;
        if (!stale && (!e->p || e->ncur < g_fx_n - 2)) stale = e;
    }
    if (!create || !stale) return nullptr;
    stale->p = p; stale->ncur = g_fx_n; stale->nprev = stale->nprev2 = 0; stale->plife = false;   // stamped: not "stale" again in this step
    return stale;
}

// Calls fn(particle, record length, group def) for every live particle of the program.
template <class F> static void for_particles(uint8_t* prog, F fn) {
    int ng = *(int*)(prog + 0x18);
    uint8_t* groups = *(uint8_t**)(prog + 0x3C);
    for (int g = 0; g < ng && g < 64 && groups; g++) {
        uint8_t* grp = groups + g * 0x10;
        uint8_t* def = *(uint8_t**)grp;
        if (!def) continue;
        uint32_t len = (uint32_t)*(int*)(def + 0x20) + (uint32_t)*(int16_t*)(def + 0x26) * 4;
        if (len < 0x10 || len > 0x2000) continue;
        uint8_t* part = *(uint8_t**)(grp + 4);
        int cnt = *(uint16_t*)(grp + 0xC);
        for (int k = 0; k < cnt && k < 4096 && part; k++) { fn(part, len & ~3u, def); part = *(uint8_t**)part; }
    }
}

// How often each header word changes per tick, per group def (EMA, 255 = every tick). Moving parts change on every
// tick; a stepped word (+0x12C of the Echuilles rays, ~30% of ticks) never reaches the bar, so it is never moved.
struct DefRate { uint8_t* def; LONG seen; uint8_t r[0x800]; uint8_t big[0x800]; };   // big: word was ever > BIG_MAG
enum { NDR = 64, RATE_MIN = 200 };
static bool floatish(float x);
static const float BIG_MAG = 4096.0f;              // beyond field world scale: not geometry
static DefRate* g_dr = nullptr;

static DefRate* def_rate(uint8_t* def, bool create) {
    if (!g_dr) return nullptr;
    DefRate* old = &g_dr[0];
    for (int i = 0; i < NDR; i++) {
        if (g_dr[i].def == def) { g_dr[i].seen = g_fx_n; return &g_dr[i]; }
        if (g_dr[i].seen < old->seen) old = &g_dr[i];
    }
    if (!create) return nullptr;
    old->def = def; old->seen = g_fx_n;
    memset(old->r, 0, sizeof(old->r));
    memset(old->big, 0, sizeof(old->big));
    return old;
}

static void ex_snapshot(uint8_t* prog) {
    __try {
        for_particles(prog, [&](uint8_t* part, uint32_t len, uint8_t* def) {
            PSnap* e = psnap_find(part, true);
            if (!e) return;
            if (e->len != len) {
                free(e->cur); free(e->prev); free(e->prev2); free(e->lo); free(e->hi); free(e->plo); free(e->phi);
                e->cur = (uint8_t*)malloc(len); e->prev = (uint8_t*)malloc(len); e->prev2 = (uint8_t*)malloc(len);
                e->lo = (float*)malloc(len); e->hi = (float*)malloc(len); e->plo = (float*)malloc(len); e->phi = (float*)malloc(len);
                e->len = (e->cur && e->prev && e->prev2 && e->lo && e->hi && e->plo && e->phi) ? len : 0;
                e->ncur = e->nprev = e->nprev2 = 0;
                e->plife = false;
                if (!e->len) return;
            }
            uint8_t* t = e->prev2; e->prev2 = e->prev; e->prev = e->cur; e->cur = t;
            e->nprev2 = e->nprev; e->nprev = e->ncur;
            memcpy(e->cur, part, len);
            e->ncur = g_fx_n;
            {
                uint32_t nw = len / 4;
                const float* c = (const float*)e->cur;
                bool restart = e->nprev == e->ncur - 1 && *(int32_t*)(e->cur + 0xC) < *(int32_t*)(e->prev + 0xC);
                if (restart) {                               // a loop ended: its range becomes the previous life's
                    float* t1 = e->plo; e->plo = e->lo; e->lo = t1;
                    float* t2 = e->phi; e->phi = e->hi; e->hi = t2;
                    e->plife = true;
                }
                if (restart || e->nprev != e->ncur - 1) { memcpy(e->lo, c, len); memcpy(e->hi, c, len); if (!restart) e->plife = false; }
                else for (uint32_t w = 4; w < nw; w++) { if (c[w] < e->lo[w]) e->lo[w] = c[w]; if (c[w] > e->hi[w]) e->hi[w] = c[w]; }
            }
            if (e->nprev == e->ncur - 1 && *(int32_t*)(e->cur + 0xC) > *(int32_t*)(e->prev + 0xC)) {
                DefRate* d = def_rate(def, true);
                uint32_t hlen = (uint32_t)*(int*)(def + 0x20) & ~3u;
                if (hlen > len) hlen = len;
                for (uint32_t o = 0x10; d && o < hlen && o / 4 < 0x800; o += 4) {
                    uint8_t& r = d->r[o / 4];
                    bool chg = *(uint32_t*)(e->cur + o) != *(uint32_t*)(e->prev + o);
                    float v = *(float*)(e->cur + o);
                    if (floatish(v) && (v > BIG_MAG || v < -BIG_MAG)) d->big[o / 4] = 1;
                    r = (uint8_t)(r - (r >> 3) + (chg ? 31 : 0));
                }
            }
        });
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool floatish(float x) { float a = x < 0 ? -x : x; return x == 0.0f || (a >= 1e-4f && a <= 1e6f); }

// Which words to move (U 12/10/2026: EXTRAP flickers the Echuilles light rays). Record layout (FUN_00717010, S): a
// header of particle state, size group def +0x20 (age at +0xC), then one handle per op (its current keyframe in
// the program's shared op data), so every moved float is a header word.
// FX-010-EXTRAP-PROGS=a-b: only these program slots; -SKIPOFFS=<hex byte offset>,...: never these header words;
// -ONLYOFFS=...: only these; -DIAG=1: per program and word offset, how often it was moved (every ~20 s).
static int g_ex_lo = 0, g_ex_hi = 0x7FFFFFFF, g_ex_diag = 0;
static uint8_t g_ex_skipw[0x800];                // per word index: 1 = never move
static int g_ex_nonly = 0;
struct ExCnt { int prog; uint32_t off; LONG n; };
static ExCnt g_exc[512];
static int   g_nexc = 0;

static int  g_ex_unit = 1;                       // FX-010-EXTRAP-UNIT=0: also move 0..1 words
static int  g_ex_bigrel = 1;                     // FX-010-EXTRAP-BIGREL=0: also move words that were ever > 4096 or jump > 20% a tick above 1000
static int  g_ex_trace = -1;                     // FX-010-EXTRAP-TRACE=<hex offset>: log moves of this header word
static LONG g_ex_trace_n = 0;

static void ex_count(int prog, uint32_t off) {
    for (int i = 0; i < g_nexc; i++) if (g_exc[i].prog == prog && g_exc[i].off == off) { g_exc[i].n++; return; }
    if (g_nexc < 512) g_exc[g_nexc++] = { prog, off, 1 };
}

static void ex_apply(uint8_t* prog) {
    g_ex_nw = 0;
    int pi = prog_index(prog);
    if (pi < g_ex_lo || pi > g_ex_hi) return;
    float k = g_extrap_pct / 100.0f;
    __try {
        for_particles(prog, [&](uint8_t* part, uint32_t len, uint8_t* def) {
            uint32_t hlen = (uint32_t)*(int*)(def + 0x20) & ~3u;  // op handles after the header are not state
            if (hlen > len) hlen = len;
            PSnap* e = psnap_find(part, false);
            if (!e || e->len != len || e->ncur != g_fx_n || e->nprev != g_fx_n - 1 || e->nprev2 != g_fx_n - 2) return;
            bool age3 = *(int32_t*)(e->prev + 0xC) > *(int32_t*)(e->prev2 + 0xC);   // same particle three ticks
            DefRate* drate = def_rate(def, false);
            int32_t ac = *(int32_t*)(e->cur + 0xC), ap = *(int32_t*)(e->prev + 0xC);
            if (ac <= ap || *(int32_t*)(part + 0xC) != ac) return;     // respawned, restarted or already changed
            bool any = false;
            for (uint32_t o = 0x10; o < hlen && g_ex_nw < EX_MAXW; o += 4) {
                float a = *(float*)(e->prev + o), b = *(float*)(e->cur + o);
                if (*(uint32_t*)(e->prev + o) == *(uint32_t*)(e->cur + o) || !floatish(a) || !floatish(b)) continue;
                if (*(uint32_t*)(part + o) != *(uint32_t*)(e->cur + o)) continue;
                if ((a < 0) != (b < 0)) continue;                    // sign flip (angle wrap, zero crossing): hold
                // Steady motion only (U 12/10/2026: a stepped header word, +0x12C of the Echuilles light rays, changed
                // on ~30% of ticks; moving it half a tick on every paused frame flickered the rays): the word moved
                // in both of the last two ticks, the same way, by steps within half of each other.
                if (!age3) continue;
                if (!drate || drate->r[o / 4] < RATE_MIN) continue;   // changes on < ~80% of ticks: stepped
                float z = *(float*)(e->prev2 + o), d1 = b - a, d0 = a - z;
                if (!floatish(z) || d0 == 0.0f || (d0 < 0) != (d1 < 0)) continue;
                // 0..1 words (alpha, colour, blend factors) stay at the tick value: +0x12C of the Echuilles rays, a
                // 0.08..0.85 fade, still flashed when moved even inside last loop's range (U 12/10/2026).
                if (g_ex_unit && z >= 0.0f && z <= 1.0f && a >= 0.0f && a <= 1.0f && b >= 0.0f && b <= 1.0f) continue;
                // Large words that change by > 20% of their size per tick are not motion: +0xE4 of the Kilika woods
                // lights (programs 35..77) went 15052 -> 10700 -> 7174 -> 4806 and restarted; moved half a tick ahead,
                // the lights flashed (U 12/10/2026; D FX-010-EXTRAP-TRACE=E4; SKIPOFFS=E4 stopped it, U).
                if (g_ex_bigrel) {
                    // A word that was ever beyond world scale for this group def stays held, also once it is small again
                    // (the same lights wander 1e5..1e2; one pass under the 20% bar still flashed, U 12/10/2026).
                    if (drate->big[o / 4]) continue;
                    float ma = a < 0 ? -a : a, mb = b < 0 ? -b : b, mx = ma > mb ? ma : mb;
                    if (mx > 1000.0f && (d1 < 0 ? -d1 : d1) > 0.2f * mx) continue;
                }
                float m = (d0 < 0 ? -d0 : d0) > (d1 < 0 ? -d1 : d1) ? (d0 < 0 ? -d0 : d0) : (d1 < 0 ? -d1 : d1);
                float dd = d1 - d0;
                if ((dd < 0 ? -dd : dd) > 0.5f * m) continue;
                if (o / 4 < 0x800 && g_ex_skipw[o / 4]) continue;
                if (g_ex_diag) ex_count(pi, o);
                float x = b + k * (b - a);
                if (b == 0.0f || (x < 0) != (b < 0)) x = 0.0f;
                if (e->plife) {                              // looping particle: stay inside last loop's range
                    float pl = e->plo[o / 4], ph = e->phi[o / 4];
                    if (b <= ph && x > ph) x = ph;
                    if (b >= pl && x < pl) x = pl;
                }
                if ((int)o == g_ex_trace && InterlockedIncrement(&g_ex_trace_n) <= 120)
                    log_line("FX-010-EXTRAP-TRACE: program %d part %p age %d: +0x%X k-2 %.5f k-1 %.5f k %.5f -> drawn %.5f (rate %u)",
                             pi, part, *(int32_t*)(e->cur + 0xC), o, z, a, b, x, drate ? drate->r[o / 4] : 0);
                g_ex_rec[g_ex_nw] = part; g_ex_off[g_ex_nw] = o; g_ex_val[g_ex_nw] = *(uint32_t*)(e->cur + o); g_ex_nw++;
                *(float*)(part + o) = x;
                any = true;
            }
            if (any) InterlockedIncrement(&g_ex_parts);
        });
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_ex_words += g_ex_nw;
}

// FX-010-INTERP (test, default 0; overrides EXTRAP): logic invocations draw particles half a tick behind, between
// their last two tick states, like SMOOTH draws characters (U 12/10/2026: EXTRAP's half-tick guess flashed twice).
// The real step runs with skip-render set (the engine's own backlog mode: the submits FUN_006458a0 / FUN_00645ba0 and
// several draw ops return early on DAT_012fb7d0, S), so it advances without drawing; then a paused step draws with
// every eligible header word at (k-1 + k) / 2, and the words go back. Render-only invocations draw tick k as before.
// Interpolated values always lie between two real states (no overshoot); words that were ever beyond world scale
// stay at tick k (BIGREL sticky flag).
static int  g_interp = 0, g_interp_cfg = 0;
static LONG g_ip_parts = 0, g_ip_words = 0, g_ip_lines = 0, g_ip_redraws = 0;

static void ip_apply(uint8_t* prog) {
    g_ex_nw = 0;
    int pi = prog_index(prog);
    if (pi < g_ex_lo || pi > g_ex_hi) return;
    __try {
        for_particles(prog, [&](uint8_t* part, uint32_t len, uint8_t* def) {
            uint32_t hlen = (uint32_t)*(int*)(def + 0x20) & ~3u;
            if (hlen > len) hlen = len;
            PSnap* e = psnap_find(part, false);
            if (!e || e->len != len || e->ncur != g_fx_n || e->nprev != g_fx_n - 1) return;
            int32_t ac = *(int32_t*)(e->cur + 0xC), ap = *(int32_t*)(e->prev + 0xC);
            if (ac <= ap || *(int32_t*)(part + 0xC) != ac) return;     // new life, restart, or changed since
            DefRate* drate = def_rate(def, false);
            bool any = false;
            for (uint32_t o = 0x10; o < hlen && g_ex_nw < EX_MAXW; o += 4) {
                if (*(uint32_t*)(e->prev + o) == *(uint32_t*)(e->cur + o)) continue;
                if (*(uint32_t*)(part + o) != *(uint32_t*)(e->cur + o)) continue;
                float a = *(float*)(e->prev + o), b = *(float*)(e->cur + o);
                if (!floatish(a) || !floatish(b)) continue;
                if ((a < 0) != (b < 0)) continue;                    // sign flip (angle wrap): hold
                if (g_ex_bigrel && drate) {
                    if (drate->big[o / 4]) continue;
                    float ma = a < 0 ? -a : a, mb = b < 0 ? -b : b, mx = ma > mb ? ma : mb, d1 = b - a;
                    if (mx > 1000.0f && (d1 < 0 ? -d1 : d1) > 0.2f * mx) continue;
                }
                if (o / 4 < 0x800 && g_ex_skipw[o / 4]) continue;
                if (g_ex_diag) ex_count(pi, o);
                float x = a + 0.5f * (b - a);
                if ((int)o == g_ex_trace && InterlockedIncrement(&g_ex_trace_n) <= 120)
                    log_line("FX-010-INTERP-TRACE: program %d part %p age %d: +0x%X k-1 %.5f k %.5f -> drawn %.5f",
                             pi, part, ac, o, a, b, x);
                g_ex_rec[g_ex_nw] = part; g_ex_off[g_ex_nw] = o; g_ex_val[g_ex_nw] = *(uint32_t*)(e->cur + o); g_ex_nw++;
                *(float*)(part + o) = x;
                any = true;
            }
            if (any) InterlockedIncrement(&g_ip_parts);
        });
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_ip_words += g_ex_nw;
}

static void ex_restore() {
    __try { for (int i = 0; i < g_ex_nw; i++) *(uint32_t*)(g_ex_rec[i] + g_ex_off[i]) = g_ex_val[i]; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_ex_nw = 0;
}

static int __cdecl h_fp_step(void* prog, uint32_t flags) {
    if (row_FX010.on && !g_disabled && pacing_kind() == INV_BUILD && prog_selected(prog)) {
        if (*(int32_t*)((uint8_t*)prog + 8) == 0 && *(int32_t*)((uint8_t*)prog + 0xC) == 0) {
            InterlockedIncrement(&g_fresh_calls);
            g_rs_prog = prog; g_rs_mode = 2;
            int rf = g_tramp(prog, flags);
            g_rs_prog = nullptr;
            return rf;
        }
        g_rs_prog = prog; g_rs_mode = 1;
        InterlockedIncrement(&g_paused_calls);
        if (g_diag_submit) diag_step_begin(2);
        if (g_diag_writes) { wr_snapshot((uint8_t*)prog); g_ph_active = 1; }
        if (g_rollback) rb_save((uint8_t*)prog);
        if (g_extrap && !g_interp && g_psnap) ex_apply((uint8_t*)prog);
        int r = ppp_step_paused(g_tramp, prog, flags);
        if (g_ex_nw) ex_restore();
        g_ph_active = 0;
        if (g_diag_writes) wr_diff(2);
        if (g_rollback) rb_restore();
        if (g_diag_submit) diag_step_end(prog, 2);
        g_rs_prog = nullptr;
        return r;
    }
    if (g_diag_writes && row_FX010.on) {
        int pi = prog_index(prog);
        if (pi >= g_prog_lo && pi <= g_prog_hi && !g_loopdefer) {
            wr_snapshot((uint8_t*)prog);
            int r = g_tramp(prog, flags);
            wr_diff(1);
            return r;
        }
    }
    if (g_diag_submit) diag_step_begin(1);
    bool defer = g_loopdefer && row_FX010.on && !g_disabled && pacing_active() && pacing_kind() != INV_BUILD;
    if (g_restart_site && g_deferred_n) deferred_flush(prog, false);
    if (defer) {
        int pi = prog_index(prog);
        if (pi < g_prog_lo || pi > g_prog_hi) defer = false;
    }
    g_defer_prog = defer ? prog : nullptr;
    g_rs_prog = prog; g_rs_mode = 0;
    bool interp = g_interp && g_psnap && row_FX010.on && !g_disabled && pacing_active() && pacing_kind() != INV_BUILD &&
                  g_skip_render && *g_skip_render == 0 && prog_selected(prog);
    if (interp) *g_skip_render = 1;                          // advance without drawing (engine backlog mode)
    int r = g_tramp(prog, flags);
    if (interp) *g_skip_render = 0;
    g_defer_prog = nullptr;
    g_rs_prog = nullptr;
    if (r && g_deferred_n) deferred_flush(prog, true);       // program ended: the field loop frees it now
    if (!r && g_psnap && row_FX010.on && pacing_kind() != INV_BUILD) ex_snapshot((uint8_t*)prog);
    if (interp && !r) {                                      // draw once, paused, half a tick behind
        ip_apply((uint8_t*)prog);
        ppp_step_paused(g_tramp, prog, flags);
        if (g_ex_nw) ex_restore();
        InterlockedIncrement(&g_ip_redraws);
    }
    if (g_diag_submit) diag_step_end(prog, 1);
    return r;
}


// ---- FX-010-DRAWOPS: pppDrawShapeCamera 0x747540 / pppDrawShapeCameraDisPos 0x748790 skip their whole block,
// draw included, while the pause flag is set (re/ledger/FX-PAUSEFLICKER.md), so paused render-only calls drew
// nothing (pyreflies flickered every frame). Patch: the gate JNZ becomes NOPs (the draw always runs); the
// sub-particle spawn and integrate CALLs go to stubs that still skip while the flag is set.
typedef int(__cdecl* dc3_t)(int, int, int);
typedef int(__cdecl* dc2_t)(int, int);
static dc3_t g_dc_spawn[2] = {};
static dc2_t g_dc_integ[2] = {};
static uint8_t* g_dc_site[2] = {};
static int g_dc_count = 0;
static uint8_t* g_pyre_gate = nullptr;            // JNE rel32 at 0x75069B (draw op 0x750620)
static volatile LONG g_dc_skipped = 0;

template <int I> static int __cdecl h_dc_spawn(int a, int b, int c) {
    if (*g_ppp_pause != 0) { InterlockedIncrement(&g_dc_skipped); return 0; }
    return g_dc_spawn[I](a, b, c);
}
template <int I> static int __cdecl h_dc_integ(int a, int b) {
    if (*g_ppp_pause != 0) return 0;
    return g_dc_integ[I](a, b);
}
static void* const k_dc_spawn_stub[2] = {(void*)&h_dc_spawn<0>, (void*)&h_dc_spawn<1>};
static void* const k_dc_integ_stub[2] = {(void*)&h_dc_integ<0>, (void*)&h_dc_integ<1>};

static const char* k_dc_sig =
    "83 3D ?? ?? ?? ?? 00 75 30 A1 ?? ?? ?? ?? 8B 7D 0C 80 78 1D 00 75 0D FF 75 10 57 53 E8 ?? ?? ?? ?? 83 C4 0C "
    "56 FF 75 10 57 53 E8 ?? ?? ?? ?? 56 57 E8 ?? ?? ?? ?? 83 C4 18 5F 5E 5B 8B E5 5D C3";

static const uint8_t* g_dc_after = nullptr;     // accept only matches past this one
static bool dc_accept(const uint8_t* m) { return m > g_dc_after && read_abs(m + 2) == (uint8_t*)g_ppp_pause; }

// ---- FX-010-KEYADD (default 1): draw op 0x75AF80 (no pause check) adds two rates into its data (+0x9C, +0xA8)
// whenever the op key equals the particle's +0xC; a gated step op resets them on logic steps only, so a paused call
// added a second time and drew base + 2r (Besaid ruins light shafts, U 12/10/2026: alternate per frame; D:
// DIAG-FX010-WRITES-OPS, +0x1CC/+0x1D8 changed on every paused call; FX-010-ROLLBACK did not help). Its gated sibling
// 0x738EC0 skips the same add while paused. On paused calls the key is made not to match for the call.
static bool     g_keyadd = false;
static uint8_t* g_keyadd_fn = nullptr;
static int (__cdecl* g_keyadd_tramp)(int, int, int, int) = nullptr;
static volatile LONG g_keyadd_held = 0;

static int __cdecl h_keyadd(int part, int data, int c, int d) {
    int32_t* key = nullptr;
    int32_t saved = 0;
    if (*g_ppp_pause != 0) {
        __try {
            int32_t* k = (int32_t*)(intptr_t)data;
            if (*k == *(int32_t*)(intptr_t)(part + 0xC)) { key = k; saved = *k; *k = saved + 1; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { key = nullptr; }
    }
    int r = g_keyadd_tramp(part, data, c, d);
    if (key) {
        *key = saved;
        if (InterlockedIncrement(&g_keyadd_held) == 1) log_line("FX-010-KEYADD: first keyed add held on a paused call");
    }
    return r;
}

static bool keyadd_find() {
    // aligned-stack prologue, then MOV EDI,[EBX+8]; LEA ESI,[EDI+0xA0]; ADD ESI,EAX; MOV EAX,[ECX]; CMP EAX,[EDI+0xC]; JNE
    g_keyadd_fn = sig_find_unique("FX-010-KEYADD keyed draw op (0x75AF80)",
        "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 83 EC 38 A1 ?? ?? ?? ?? 33 C5 89 45 FC "
        "8B 43 10 8B 4B 0C 8B 40 0C 56 8B 00 57 8B 7B 08 8D B7 A0 00 00 00 03 F0 8B 01 3B 47 0C 75 32 D9 41 08",
        0x75AF80);
    if (!g_keyadd_fn) log_line("  FX-010-KEYADD: op not found - off");
    return g_keyadd_fn != nullptr;
}

static bool drawcam_find() {
    const uint32_t expect[2] = {0x747713, 0x748963};
    g_dc_after = nullptr;
    for (int i = 0; i < 2; i++) {
        int hits = 0;
        uint8_t* m = sig_scan_module(exe_base(), 0, k_dc_sig, &hits, dc_accept);
        if (!m) { log_line("  FX-010-DRAWOPS: draw op %d not found - refused", i); return false; }
        if (i == 0 && hits != 2) { log_line("  FX-010-DRAWOPS: %d draw op matches (expected 2) - refused", hits); return false; }
        log_line("  FX-010-DRAWOPS: draw op %d gate at gva=0x%08X (expected 0x%08X)", i, rt_to_gva_exe(m), expect[i]);
        g_dc_site[i] = m;
        g_dc_after = m;
    }
    g_dc_count = 2;
    // pyrefly draw op 0x750620 (D: DIAG-FX010SUBMIT, submit 0x751621 0 calls when paused): its entry
    // `CMP [pause],0; MOV [EBP-0x16C],EAX; JNE end` skips the whole draw; the body's one state write (+0x290 += , at
    // 0x751666) has its own pause check further down, which must be there.
    int hits = 0;
    g_dc_after = nullptr;
    uint8_t* m = sig_scan_module(exe_base(), 0,
        "83 3D ?? ?? ?? ?? 00 89 85 94 FE FF FF 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 8B 8D A8 FE FF FF", &hits, dc_accept);
    if (!m || hits != 1) { log_line("  FX-010-DRAWOPS: pyrefly draw op gate: %d matches (expected 1) - refused", hits); return false; }
    uint8_t* end = m + 0x13 + *(int32_t*)(m + 0xF);          // JNE target
    uint8_t* inner = end - 0x1F;                               // CMP [pause],0; JNE end; MOV; MOV; MOV; ADD [..+0x290]
    if (!sig_match(inner, "83 3D ?? ?? ?? ?? 00 75 16 8B 85 A0 FE FF FF 8B 8D B4 FE FF FF 8B 40 08 01 84 39 90 02 00 00") ||
        read_abs(inner + 2) != (uint8_t*)g_ppp_pause) {
        log_line("  FX-010-DRAWOPS: pyrefly draw op inner pause check not found at gva=0x%08X - refused", rt_to_gva_exe(inner));
        return false;
    }
    g_pyre_gate = m + 0xD;
    log_line("  FX-010-DRAWOPS: pyrefly draw op gate at gva=0x%08X (expected 0x0075069B), inner check gva=0x%08X",
             rt_to_gva_exe(g_pyre_gate), rt_to_gva_exe(inner));
    return true;
}

static bool nop_bytes(uint8_t* at, const char* expect, int n) {
    uint8_t nops[16];
    if (n > 16 || !sig_match(at, expect)) return false;
    memset(nops, 0x90, n);
    return patch_write(at, nops, n);
}

// FX-010-UNGATE=<hex VA>[,<hex VA>...] (test switch): NOP these JNEs, each the jump of a pause-flag gate (a
// `CMP [pause],0` must start within 32 bytes before it; VAs computed offline with a disassembler). The guarded block
// then runs on paused calls too (2x), which tells whether that block is what a blink needs.
static uint8_t* g_ungate[32] = {};
static int      g_ungate_n = 0;

static bool pause_cmp_before(const uint8_t* j) {
    for (const uint8_t* c = j - 7; c >= j - 32; c--)
        if (c[0] == 0x83 && c[1] == 0x3D && c[6] == 0x00 && read_abs(c + 2) == (uint8_t*)g_ppp_pause) return true;
    return false;
}

static void ungate_parse() {
    char buf[512];
    config_str("rows", "FX-010-UNGATE", buf, sizeof(buf), "");
    for (char* t = buf; *t && g_ungate_n < 32;) {
        char* end = nullptr;
        unsigned long va = strtoul(t, &end, 16);
        if (end == t) break;
        uint8_t* j = exe_base() + (va - 0x400000);
        bool ok = va > 0x400020 && va < 0x400000 + exe_size() &&
                  ((j[0] == 0x0F && j[1] == 0x85) || j[0] == 0x75) && pause_cmp_before(j);
        if (ok) g_ungate[g_ungate_n++] = j;
        log_line("  FX-010-UNGATE: JNE gva=0x%08lX %s", va, ok ? "accepted" : "is not a pause-gate JNE - ignored");
        t = end;
        while (*t == ',' || *t == ' ') t++;
    }
}

static void ungate_patch() {
    for (int i = 0; i < g_ungate_n; i++) {
        uint8_t* j = g_ungate[i];
        bool ok = j[0] == 0x0F ? nop_bytes(j, "0F 85", 6) : nop_bytes(j, "75", 2);
        log_line("FX-010-UNGATE: JNE gva=0x%08X %s", rt_to_gva_exe(j), ok ? "NOPed (block runs while paused)" : "patch failed");
    }
}

static bool drawcam_patch() {
    if (!nop_bytes(g_pyre_gate, "0F 85", 6)) return false;
    log_line("FX-010-DRAWOPS: pyrefly draw op gate gva=0x%08X NOPed (draws while paused; +0x290 still per tick)",
             rt_to_gva_exe(g_pyre_gate));
    for (int i = 0; i < g_dc_count; i++) {
        uint8_t* m = g_dc_site[i];
        uint8_t* sp = call_target(m + 0x1C);
        uint8_t* ig = call_target(m + 0x31);
        if (!sp || !ig) return false;
        g_dc_spawn[i] = (dc3_t)sp;
        g_dc_integ[i] = (dc2_t)ig;
        if (!patch_call_rel32(m + 0x1C, sp, k_dc_spawn_stub[i])) return false;
        if (!patch_call_rel32(m + 0x31, ig, k_dc_integ_stub[i])) {
            patch_call_rel32(m + 0x1C, k_dc_spawn_stub[i], sp);
            return false;
        }
        static const uint8_t nop2[2] = { 0x90, 0x90 };
        if (m[7] != 0x75 || m[8] != 0x30 || !patch_write(m + 7, nop2, 2)) return false;
        log_line("FX-010-DRAWOPS: draw op gate gva=0x%08X NOPed; spawn 0x%08X / integrate 0x%08X skipped while paused",
                 rt_to_gva_exe(m), rt_to_gva_exe(sp), rt_to_gva_exe(ig));
    }
    return true;
}
static bool g_drawcam = false;

static bool install() {
    // FUN_007122f0 (field particle step): prologue through its spawn CALL at +0x2A
    g_entry = sig_find_unique("FX-010 pppFpLoop step FUN_007122f0",
        "55 8B EC 53 8B 5D 0C 56 8B 75 08 89 35 ?? ?? ?? ?? F6 C3 10 74 09 56 E8 ?? ?? ?? ?? 83 C4 04 56 C7 05 ?? ?? ?? ?? 00 00 00 00 E8",
        0x7122F0);
    if (!g_entry) return false;
    if (!ppp_resolve_pause("FX-010", g_entry + 0x72)) return false;
    if (config_row("FX-010-SPAWN", 1)) {
        if (!ppp_hook_spawn_call("FX-010", g_entry + 0x2A)) return false;
    } else {
        log_line("  FX-010: spawn skip OFF (FX-010-SPAWN=0)");
    }
    g_drawcam = config_row("FX-010-DRAWOPS", 1) != 0;
    g_keyadd = config_row("FX-010-KEYADD", 1) != 0;
    g_diag_ops = config_row("DIAG-FX010-OPS", 0) != 0;
    g_diag_restart = config_row("DIAG-FX010-RESTART", 0) != 0;
    g_loopdefer_stale = config_row("FX-010-LOOPDEFER-STALE", 0);
    g_diag_writes = config_row("DIAG-FX010-WRITES", 0) != 0;
    g_rollback = config_row("FX-010-ROLLBACK", 0) != 0;
    g_extrap = config_row("FX-010-EXTRAP", 0);
    g_extrap_pct = config_row("FX-010-EXTRAP-PCT", 50);
    g_ex_diag = config_row("FX-010-EXTRAP-DIAG", 0);
    g_ex_unit = config_row("FX-010-EXTRAP-UNIT", 1);
    g_ex_bigrel = config_row("FX-010-EXTRAP-BIGREL", 1);
    g_interp = config_row("FX-010-INTERP", 0);
    g_interp_cfg = g_interp;
    {
        char t[32];
        config_str("rows", "FX-010-EXTRAP-TRACE", t, sizeof(t), "");
        if (t[0]) g_ex_trace = (int)strtoul(t, nullptr, 16);
        char b[512];
        config_str("rows", "FX-010-EXTRAP-PROGS", b, sizeof(b), "");
        int lo, hi;
        if (b[0] && sscanf_s(b, "%d-%d", &lo, &hi) == 2 && lo <= hi) { g_ex_lo = lo; g_ex_hi = hi; }
        else if (b[0] && sscanf_s(b, "%d", &lo) == 1) { g_ex_lo = g_ex_hi = lo; }
        int nskip = 0;
        for (int L = 0; L < 2; L++) {
            config_str("rows", L ? "FX-010-EXTRAP-ONLYOFFS" : "FX-010-EXTRAP-SKIPOFFS", b, sizeof(b), "");
            uint8_t listed[0x800] = {};
            int n = 0;
            for (char* q = b; *q;) {
                char* e2 = nullptr;
                unsigned long v = strtoul(q, &e2, 16);
                if (e2 == q) { q++; continue; }
                if (v / 4 < 0x800) { listed[v / 4] = 1; n++; }
                q = e2;
            }
            if (!n) continue;
            for (int w = 0; w < 0x800; w++) if (L ? !listed[w] : listed[w]) g_ex_skipw[w] = 1;
            if (L) g_ex_nonly = n; else nskip = n;
        }
        if (g_ex_lo != 0 || g_ex_hi != 0x7FFFFFFF || nskip || g_ex_nonly)
            log_line("  FX-010-EXTRAP: programs %d..%d, %d header words skipped, %d only", g_ex_lo, g_ex_hi, nskip, g_ex_nonly);
    }
    g_psnap = (PSnap*)calloc(PS_N, sizeof(PSnap));
    g_dr = (DefRate*)calloc(NDR, sizeof(DefRate));
    log_line("  FX-010-EXTRAP: %s (%d%% of a tick)%s", g_extrap ? "on" : "off", g_extrap_pct, g_interp ? " - overridden by FX-010-INTERP" : "");
    if (g_interp) log_line("  FX-010-INTERP: on (logic invocations draw particles half a tick behind; real step under skip-render)");
    if (g_rollback) log_line("  FX-010: FX-010-ROLLBACK on: paused steps of selected programs leave their records unchanged");
    g_loopdefer = config_row("FX-010-LOOPDEFER", 1) != 0;
    if (g_loopdefer) {
        // FUN_00717010 tail: MOV ECX,[EBX+0x18]; PUSH ESI; MOV [ESI+0xC],ECX; CALL FUN_00712d20; MOV EBX,[EBP-0xC]; ADD ESP,4
        uint8_t* m = sig_find_unique("FX-010-LOOPDEFER particle loop restart (FUN_00717010)",
            "8B 4B 18 56 89 4E 0C E8 ?? ?? ?? ?? 8B 5D F4 83 C4 04", 0x717236);
        uint8_t* t = m ? call_target(m + 7) : nullptr;
        if (!m || !t || rt_to_gva_exe(t) != 0x712D20) {
            log_line("  FX-010-LOOPDEFER: restart CALL not found - sub-switch off");
            g_loopdefer = false;
        } else {
            g_restart_site = m + 7;
            g_restart = (restart_t)t;
            log_line("  FX-010-LOOPDEFER: restart CALL gva=0x%08X -> FUN_00712d20 gva=0x%08X", rt_to_gva_exe(g_restart_site), rt_to_gva_exe(t));
        }
    }
    ungate_parse();
    char pr[32];
    config_str("rows", "FX-010-PROGS", pr, sizeof(pr), "");
    if (pr[0]) {
        int lo = 0, hi = 0;
        if (sscanf_s(pr, "%d-%d", &lo, &hi) == 2 && lo <= hi) { g_prog_lo = lo; g_prog_hi = hi; }
        log_line("  FX-010: FX-010-PROGS '%s': pausing program slots %d..%d only", pr, g_prog_lo, g_prog_hi);
    }
    g_diag_submit = config_row("DIAG-FX010SUBMIT", 0) != 0;
    if (g_diag_submit) {
        // FUN_006458a0 (draw submit): PUSH EBP; MOV EBP,ESP; MOV ECX,[..]; SUB ESP,8; PUSH ESI; CALL; CMP [..],0
        // two submit functions share this prologue: FUN_006458a0 (JNZ +0x173) and FUN_00645ba0 (JNZ +0x171)
        g_submit_fn[0] = sig_find_unique("DIAG-FX010SUBMIT draw submit FUN_006458a0",
            "55 8B EC 8B 0D ?? ?? ?? ?? 83 EC 08 56 E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 8B F0 0F 85 73 01 00 00", 0x6458A0);
        g_submit_fn[1] = sig_find_unique("DIAG-FX010SUBMIT draw submit FUN_00645ba0",
            "55 8B EC 8B 0D ?? ?? ?? ?? 83 EC 08 56 E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 8B F0 0F 85 71 01 00 00", 0x645BA0);
        g_submit_fn[2] = sig_find_unique("DIAG-FX010SUBMIT FUN_00645aa0",
            "55 8B EC 51 8B 4D 08 8B 55 0C 8B C1 0B C2 0F 84 E3 00 00 00", 0x645AA0);
        // twins share these prologues (0x645400/0x641460, 0x7E6A00/0x7166D0): hook both, pick each by its VA
        const char* draw5 = "55 8B EC FF 75 1C 8B 0D ?? ?? ?? ?? FF 75 18 FF 75 14 FF 75 10 FF 75 0C";
        const char* alloc = "55 8B EC 0F BE 15 ?? ?? ?? ?? 8B 4D 08 8B 04 95 ?? ?? ?? ?? 42 03 C8 89 0C 95";
        g_submit_fn[3] = diag_find_at(draw5, 0x645400);
        g_submit_fn[4] = diag_find_at(alloc, 0x7E6A00);
        g_submit_fn[5] = diag_find_at(draw5, 0x641460);
        g_submit_fn[6] = diag_find_at(alloc, 0x7166D0);
        for (int i = 0; i < NSUB; i++) if (!g_submit_fn[i]) g_diag_submit = false;
        log_line("  DIAG-FX010SUBMIT: %s", g_diag_submit ? "on (submit counts per caller/program, every 600 invocations)" : "off (submit not found)");
    }
    if (g_drawcam && !drawcam_find()) return false;
    if (g_keyadd && !keyadd_find()) g_keyadd = false;
    log_line("  FX-010: FUN_007122f0 gva=0x%08X, entry hooked on the game thread",
             rt_to_gva_exe(g_entry));
    return true;
}

#ifdef FPS60_DEV
static void ex_hotkey() {                        // dev hotkey Num 7
    static bool prev = false;
    if (!dev_key(VK_NUMPAD7, &prev) || !g_psnap) return;
    if (g_interp_cfg) {                          // FX-010-INTERP configured: Num 7 toggles interpolation instead
        g_interp = !g_interp;
        log_line("Num7: FX-010-INTERP=%d (logic invocations draw particles half a tick behind)", g_interp);
        return;
    }
    g_extrap = !g_extrap;
    log_line("Num7: FX-010-EXTRAP=%d (particles moved %d%% of a tick ahead on paused frames)", g_extrap, g_extrap_pct);
}
#endif

static void pre() {
    if (pacing_kind() != INV_BUILD) g_fx_n++;
#ifdef FPS60_DEV
    ex_hotkey();
#endif
    static LONG ex_dump_at = 0;
    if (g_ex_diag && (g_extrap || g_interp) && g_fx_n - ex_dump_at >= 600 && g_nexc) {
        ex_dump_at = g_fx_n;
        char line[600];
        int pos = 0, prog = -1;
        for (int i = 0; i <= g_nexc; i++) {
            if (i == g_nexc || (prog >= 0 && g_exc[i].prog != prog) || pos > 520) {
                if (pos) log_line("FX-010-EXTRAP-DIAG: program %d moved words (offset:count):%s", prog, line);
                pos = 0;
                if (i == g_nexc) break;
            }
            prog = g_exc[i].prog;
            pos += _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, " %X:%ld", g_exc[i].off, g_exc[i].n);
        }
        g_nexc = 0;
    }
    if (g_extrap && g_ex_parts - g_ex_lines >= 20000) {
        g_ex_lines = g_ex_parts;
        log_line("FX-010-EXTRAP: %ld particle draws extrapolated, %ld words", g_ex_parts, g_ex_words);
    }
    if (g_interp && g_ip_parts - g_ip_lines >= 20000) {
        g_ip_lines = g_ip_parts;
        log_line("FX-010-INTERP: %ld particle draws interpolated, %ld words, %ld program redraws", g_ip_parts, g_ip_words, g_ip_redraws);
    }
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("FX-010", g_entry, (void*)&h_fp_step, (void**)&g_tramp)) {
        log_line("FX-010: entry hook failed - row off");
        row_FX010.on = 0;
        return;
    }
    log_line("FX-010: FUN_007122f0 entry hooked on the game thread (tramp %p)", (void*)g_tramp);
    if (g_diag_writes) {
        static const int off[3] = {0x17, 0x53, 0x6A};
        static const uint32_t expect[3] = {0x712E80, 0x717010, 0x712E00};
        bool ok = true;
        for (int p = 0; p < 3 && ok; p++) {
            g_ph_fn[p] = call_target(g_entry + off[p]);
            ok = g_ph_fn[p] && rt_to_gva_exe(g_ph_fn[p]) == expect[p] &&
                 hook_entry("DIAG-FX010-WRITES phase", g_ph_fn[p], k_phase_thunk[p], &g_ph_tramp[p]);
            if (!ok) {
                log_line("DIAG-FX010-WRITES: phase %d hook failed - phase diff off", p);
                for (int q = 0; q < p; q++) hook_remove("DIAG-FX010-WRITES phase", g_ph_fn[q]);
            }
        }
        if (ok) log_line("DIAG-FX010-WRITES: phase hooks on 0x712E80 / 0x717010 / 0x712E00");
        // diagnostic only: the river program's ungated ops by VA (FX-010 proved the build), bytes verified
        // DIAG-FX010-WRITES-OPS=<hex VA>,...: hook these ops instead (prologue checked: PUSH EBP; MOV EBP,ESP or the
        // aligned-stack PUSH EBX; MOV EBX,ESP)
        char lst[200];
        config_str("rows", "DIAG-FX010-WRITES-OPS", lst, sizeof(lst), "");
        if (lst[0]) {
            k_op_n = 0;
            for (char* q = lst; *q && k_op_n < NOPS;) {
                char* e = nullptr;
                unsigned long v = strtoul(q, &e, 16);
                if (e == q) { q++; continue; }
                k_op_va[k_op_n] = (uint32_t)v;
                k_op_pat[k_op_n] = sig_match(exe_base() + (v - 0x400000), "53 8B DC") ? "53 8B DC 83 EC 08" : "55 8B EC";
                k_op_n++;
                q = e;
            }
        }
        for (int k = 0; ok && k < k_op_n; k++) {
            uint8_t* f = exe_base() + (k_op_va[k] - 0x400000);
            bool hk = sig_match(f, k_op_pat[k]) && hook_entry("DIAG-FX010-WRITES op", f, k_phase_thunk[3 + k], &g_ph_tramp[3 + k]);
            if (hk) g_ph_fn[3 + k] = f;
            log_line("DIAG-FX010-WRITES: op 0x%08X hook %s", k_op_va[k], hk ? "on" : "FAILED");
        }
    }
    for (int i = 0; g_diag_submit && i < NSUB; i++) {
        if (!hook_entry("DIAG-FX010SUBMIT", g_submit_fn[i], k_sub_thunk[i], &g_submit_tramp[i])) {
            log_line("DIAG-FX010SUBMIT: hook %s failed - diag off", k_sub_name[i]);
            g_diag_submit = false;
        }
    }
    ungate_patch();
    if (g_loopdefer && !patch_call_rel32(g_restart_site, (void*)g_restart, (void*)&h_restart)) {
        log_line("FX-010-LOOPDEFER: patch failed - off");
        g_loopdefer = false;
        g_restart_site = nullptr;
    } else if (g_loopdefer) {
        log_line("FX-010-LOOPDEFER: loop restarts of paced logic steps deferred to the program's next logic step");
    }
    if (g_drawcam && !drawcam_patch()) log_line("FX-010-DRAWOPS: patch failed (draw ops still blink while paused)");
    if (g_keyadd) {
        if (hook_entry("FX-010-KEYADD", g_keyadd_fn, (void*)&h_keyadd, (void**)&g_keyadd_tramp))
            log_line("FX-010-KEYADD: draw op gva 0x%08X hooked: keyed add skipped on paused calls", rt_to_gva_exe(g_keyadd_fn));
        else g_keyadd = false;
    }
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_paused_calls > 0) {
        reported = 1;
        log_line("FX-010: first paused field particle step on a render-only invocation (fresh unpaused: %ld)", g_fresh_calls);
    }
    g_inv_stamp++;
    static uint32_t tick = 0;
    if (++tick % 18000 == 0) log_line("FX-010: paused=%ld fresh=%ld; LOOPDEFER deferred=%ld applied=%ld dropped=%ld",
                                     g_paused_calls, g_fresh_calls, g_deferred_total, g_deferred_applied, g_deferred_dropped);
    static LONG loop_reported = 0;
    if (!loop_reported && g_deferred_applied > 0) {
        loop_reported = 1;
        log_line("FX-010-LOOPDEFER: first deferred restart applied (deferred %ld)", g_deferred_total);
    }
    if (g_diag_submit && tick % 600 == 0) diag_dump();
    if (g_diag_writes && tick % 600 == 0 && (g_wr_steps[1] || g_wr_steps[2])) { wr_dump(); ph_dump(); }
}

Row row_FX010 = {
    "FX-010",
    "field/map particle programs (FUN_007122f0): pause flag on render-only invocations (draw still runs)",
    1, install, pre, post, 0
};
