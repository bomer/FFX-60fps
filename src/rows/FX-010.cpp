// SPDX-License-Identifier: Apache-2.0
// Field/map particles (rain, lightning, field lights) step on every draw and run at 2x. On render-only
// frames their step runs with the particle pause flag set, as MAG-006 does for DLL programs. Off by default.
// Details: docs/fps60/rows/FX-010.md
#include "../fps60.h"
#include <string.h>
#include <stdio.h>

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

static int __cdecl h_fp_step(void* prog, uint32_t flags) {
    if (row_FX010.on && !g_disabled && pacing_kind() == INV_BUILD) {
        if (*(int32_t*)((uint8_t*)prog + 8) == 0 && *(int32_t*)((uint8_t*)prog + 0xC) == 0) {
            InterlockedIncrement(&g_fresh_calls);
            return g_tramp(prog, flags);
        }
        InterlockedIncrement(&g_paused_calls);
        if (g_diag_submit) diag_step_begin(2);
        int r = ppp_step_paused(g_tramp, prog, flags);
        if (g_diag_submit) diag_step_end(prog, 2);
        return r;
    }
    if (g_diag_submit) diag_step_begin(1);
    int r = g_tramp(prog, flags);
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
    DWORD old;
    if (!sig_match(at, expect) || !VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memset(at, 0x90, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
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
        DWORD old;
        if (m[7] != 0x75 || m[8] != 0x30 || !VirtualProtect(m + 7, 2, PAGE_EXECUTE_READWRITE, &old)) return false;
        m[7] = 0x90; m[8] = 0x90;
        VirtualProtect(m + 7, 2, old, &old);
        FlushInstructionCache(GetCurrentProcess(), m + 7, 2);
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
    if (config_row("FX-010-SPAWN", 0)) {
        if (!ppp_hook_spawn_call("FX-010", g_entry + 0x2A)) return false;
    } else {
        log_line("  FX-010: spawn skip OFF (FX-010-SPAWN=0)");
    }
    g_drawcam = config_row("FX-010-DRAWOPS", 1) != 0;
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
    log_line("  FX-010: FUN_007122f0 gva=0x%08X, entry hooked on the game thread",
             rt_to_gva_exe(g_entry));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("FX-010", g_entry, (void*)&h_fp_step, (void**)&g_tramp)) {
        log_line("FX-010: entry hook failed - row off");
        row_FX010.on = 0;
        return;
    }
    log_line("FX-010: FUN_007122f0 entry hooked on the game thread (tramp %p)", (void*)g_tramp);
    for (int i = 0; g_diag_submit && i < NSUB; i++) {
        if (!hook_entry("DIAG-FX010SUBMIT", g_submit_fn[i], k_sub_thunk[i], &g_submit_tramp[i])) {
            log_line("DIAG-FX010SUBMIT: hook %s failed - diag off", k_sub_name[i]);
            g_diag_submit = false;
        }
    }
    if (g_drawcam && !drawcam_patch()) log_line("FX-010-DRAWOPS: patch failed (draw ops still blink while paused)");
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_paused_calls > 0) {
        reported = 1;
        log_line("FX-010: first paused field particle step on a render-only invocation (fresh unpaused: %ld)", g_fresh_calls);
    }
    static uint32_t tick = 0;
    if (++tick % 18000 == 0) log_line("FX-010: paused=%ld fresh=%ld", g_paused_calls, g_fresh_calls);
    if (g_diag_submit && tick % 600 == 0) diag_dump();
}

Row row_FX010 = {
    "FX-010",
    "field/map particle programs (FUN_007122f0): pause flag on render-only invocations (draw still runs)",
    0, install, pre, post, 0
};
