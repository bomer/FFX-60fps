// SPDX-License-Identifier: Apache-2.0
// X2-MAG-006 (port of FFX MAG-006 v3+v4): particle programs (Ppp_ProgramStep FUN_007fed00) step on every draw, so
// at 60 fps they run 2x. On render-only invocations DLL callers (service table +0xB00) step with the engine's pause
// flag 0x01CF579C set: the particles draw but their clock, age and ops don't advance. EXE callers keep stock
// behaviour (pausing them crashed in FFX), except call sites other rows list (mag006_pause_exe_caller). A program that never advanced (+8 == 0 && +0xC == 0) steps unpaused
// (FFX v4 FRESH: a paused first step draws unset state).
// FFX-2 only: the step's first call FUN_006ba090 (E8 at entry+8) can clear the pause flag for the loaded magic
// overlay (DAT_00e20b24 && DAT_00e10398); it is skipped while this row pauses a step.
// Open (H, PORT-B §2): FFX-2's start-delay clock prog+0x78 (FUN_00804870) runs 2x for FRESH programs.
// Evidence: outer repo re/ledger/PORT-B.md §2 (S).
#include "../x2.h"
#include <intrin.h>

namespace x2 {

typedef int(__cdecl* ppp_step_t)(void* prog, uint32_t flags);   // bool cdecl(prog, flags), plain RET
typedef void(__cdecl* override_t)(void* prog);

static uint8_t*   g_entry = nullptr;
static ppp_step_t g_tramp = nullptr;
static override_t g_override = nullptr;
static volatile int32_t* g_pause = nullptr;
static bool       g_patched = false;
static int        g_fresh_on = 1;                // [rows] X2-MAG-006-FRESH
static int        g_pausing = 0;                 // game thread: this row is inside a paused step
static volatile LONG g_paused_calls = 0, g_exe_calls = 0, g_fresh_calls = 0, g_override_skipped = 0;
extern Row row_MAG006;

// EXE call sites paused like DLL callers (return addresses; X2-MAG-026 adds the family-X record step).
static uint8_t* g_pause_exe[4];
static int      g_npause_exe = 0;
static volatile LONG g_exe_paused_calls = 0;
uint8_t* mag006_entry() { return row_MAG006.on ? g_entry : nullptr; }
volatile int32_t* mag006_pause_flag() { return g_pause; }   // 0x01CF579C (null before install)
bool mag006_pause_exe_caller(uint8_t* ret) {
    if (g_npause_exe >= 4) return false;
    g_pause_exe[g_npause_exe++] = ret;
    return true;
}
static bool listed(const void* ret) {
    for (int i = 0; i < g_npause_exe; i++) if (g_pause_exe[i] == ret) return true;
    return false;
}

static void __cdecl h_override(void* prog) {
    if (g_pausing) { InterlockedIncrement(&g_override_skipped); return; }
    g_override(prog);
}

// Test switch (F11, default off): DLL-caller steps on render-only invocations run with the pause flag CLEAR, overriding
// the pause a DLL sets itself (magic_0718's draw entry writes +0xBF0 = (exec==0)). Effects then step 2x; a blink that
// stops tells that it lives on the paused path (U 16/10/2026: mission-start hex grid).
static volatile LONG g_unpause_test = 0, g_unpaused_calls = 0;

// X2-FX-010-LOOPDEFER (port of FFX FX-010-LOOPDEFER, default 1; U 16/10/2026: mission-start hex grid blinks, stops with
// the F11 unpause test). At the end of an unpaused step the group loop FUN_00805b80 (and its twin FUN_00806130) restarts
// looping particles that reached their end: age = loop start, FUN_007fff80 resets every op of the particle to its first
// key and runs the ops' init callbacks (birth state). Stock draws nothing until the next tick's ops have run on it; the
// paused render-only step in between drew the restarted particles in their raw birth state. Fix: under 60 Hz pacing every
// restart is queued and applied at the start of that program's next unpaused step, before its ops (stock order); see
// step_deferred. Dropped if the program ends or the particle left its list. Covers EXE-called programs too (they run
// paused whenever a DLL left the pause flag set). Sites 0x8060A3 / 0x8063EF (CALL FUN_007fff80, cdecl(particle), plain
// RET 0x7FFFD9, caller ADD ESP,4).
typedef void(__cdecl* restart_t)(void*);
static restart_t g_restart = nullptr;
static uint8_t*  g_restart_site[2] = {};
static int       g_loopdefer = 0;
static void*     g_defer_prog = nullptr;         // DLL-called program in a logic step now (game thread)
struct Deferred { void* prog; void* part; };
enum { MAXDEF = 2048 };
static Deferred  g_deferred[MAXDEF];
static int       g_deferred_n = 0;
static volatile LONG g_def_total = 0, g_def_applied = 0, g_def_dropped = 0, g_def_full = 0;

static void __cdecl h_restart(void* part) {
    if (g_defer_prog && g_deferred_n < MAXDEF) {
        g_deferred[g_deferred_n++] = { g_defer_prog, part };
        InterlockedIncrement(&g_def_total);
        return;
    }
    if (g_defer_prog) InterlockedIncrement(&g_def_full);
    g_restart(part);
}

// the particle is still on the program's particle list (+0x30, next at +0)
static bool part_in_prog(uint8_t* prog, void* part) {
    __try {
        uint8_t* p = *(uint8_t**)(prog + 0x30);
        for (int k = 0; p && k < 8192; k++, p = *(uint8_t**)p)
            if (p == part) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static void deferred_flush(void* prog, bool drop) {
    int w = 0;
    for (int i = 0; i < g_deferred_n; i++) {
        Deferred d = g_deferred[i];
        if (d.prog != prog) { g_deferred[w++] = d; continue; }
        if (!drop && part_in_prog((uint8_t*)prog, d.part)) { g_restart(d.part); InterlockedIncrement(&g_def_applied); }
        else InterlockedIncrement(&g_def_dropped);
    }
    g_deferred_n = w;
}

// One step with LOOPDEFER: restarts made by this step are queued; queued restarts are applied at the start of the
// program's next UNPAUSED step (its ops then run on them, the stock order). A paused step keeps them queued: EXE-called
// programs (field particles, e.g. the pyrefly river) also run paused on render-only frames whenever a magic DLL left the
// pause flag set (magic_0718's draw entry writes it per draw; U 16/10/2026: river blinks ~1/s on the mission screen).
static int step_deferred(void* prog, uint32_t flags, bool will_pause, ppp_step_t step = nullptr) {
    if (!step) step = g_tramp;
    if (!g_loopdefer || pacing_kind() == INV_STOCK) {
        if (g_deferred_n) deferred_flush(prog, false);           // stock step (e.g. a menu holds pacing): apply first
        return step(prog, flags);
    }
    if (g_deferred_n && !will_pause) deferred_flush(prog, false);
    void* outer = g_defer_prog;
    g_defer_prog = prog;
    int r = step(prog, flags);
    g_defer_prog = outer;
    if (r && g_deferred_n) deferred_flush(prog, true);           // program ended
    return r;
}

// Field particle programs (pyrefly river, map effects) step through FUN_007fef20 (FFX FUN_007122f0: field loop
// FUN_00810190 / FUN_00810F90 -> FUN_007fef20(prog, 0x30) -> group loop FUN_00806130, restart site 0x8063EF), not
// Ppp_ProgramStep, on every invocation, with the pause flag as left by others (magic_0718 sets it per draw). Same
// deferral (U 16/10/2026: river still blinked with only Ppp_ProgramStep covered; D: "EXE callers passed 0").
static uint8_t*   g_fstep = nullptr;
static ppp_step_t g_fstep_tramp = nullptr;
static int __cdecl h_field_step(void* prog, uint32_t flags) {
    if (!row_MAG006.on || g_disabled) return g_fstep_tramp(prog, flags);
    return step_deferred(prog, flags, *g_pause != 0, g_fstep_tramp);
}

static int __cdecl h_ppp_step(void* prog, uint32_t flags) {
    if (!row_MAG006.on || g_disabled || g_pausing) return g_tramp(prog, flags);
    void* ret = _ReturnAddress();
    bool exe = in_image(ret) && !listed(ret);    // listed EXE callers take the DLL path
    if (!render_only() || exe) {                 // logic steps, and EXE callers (stock pause state)
        if (exe && render_only()) InterlockedIncrement(&g_exe_calls);
        return step_deferred(prog, flags, *g_pause != 0);
    }
    if (g_unpause_test) {
        InterlockedIncrement(&g_unpaused_calls);
        int32_t saved = *g_pause;
        *g_pause = 0;
        int r = step_deferred(prog, flags, false);
        *g_pause = saved;
        return r;
    }
    if (g_fresh_on && *(int32_t*)((uint8_t*)prog + 8) == 0 && *(int32_t*)((uint8_t*)prog + 0xC) == 0) {
        InterlockedIncrement(&g_fresh_calls);
        return step_deferred(prog, flags, *g_pause != 0);
    }
    InterlockedIncrement(&g_paused_calls);
    if (in_image(ret) && InterlockedIncrement(&g_exe_paused_calls) == 1)
        log_line("X2-MAG-006: first paused step from a listed EXE caller (return gva 0x%08X)", rt_to_gva(ret));
    int32_t saved = *g_pause;                   // values written by event scripts survive (FFX MAG-004)
    *g_pause = 1;
    g_pausing = 1;
    int r = g_tramp(prog, flags);
    g_pausing = 0;
    *g_pause = saved;
    return r;
}

static bool install() {
    // Ppp_ProgramStep: PUSH ESI; CALL override(prog); ADD ESP,4; CMP [..],0; JNZ; PUSH ESI; CALL start-delay
    g_entry = sig_find("X2-MAG-006 Ppp_ProgramStep",
        "55 8B EC 56 8B 75 08 56 E8 ?? ?? ?? ?? 83 C4 04 83 3D ?? ?? ?? ?? 00 75 20 56 E8", 0x7FED00);
    if (!g_entry) return false;
    // the step's own pause test: CMP dword [flag],0 at entry+0x1BF
    if (!sig_match(g_entry + 0x1BF, "83 3D ?? ?? ?? ?? 00 5F 75 3F 80 7E 1C 00")) {
        log_line("  X2-MAG-006: pause CMP not at entry+0x1BF - refused");
        return false;
    }
    g_pause = (volatile int32_t*)abs_operand("particle pause flag", g_entry + 0x1BF + 2);
    if (!g_pause) return false;
    // override FUN_006ba090: if (DAT_00e20b24) flag &= ~-(DAT_00e10398 != 0)
    uint8_t* ov = call_target(g_entry + 8);
    if (!ov || !in_image(ov)) return false;
    g_override = (override_t)ov;
    g_fresh_on = config_row("X2-MAG-006-FRESH", 1);
    g_loopdefer = config_row("X2-FX-010-LOOPDEFER", 1);
    if (g_loopdefer) {
        // group loop tails: MOV EAX,[ESI+0xC]; CMP EAX,[ECX+0x1C]; JL; MOV EAX,[ECX+0x18]; PUSH ESI; MOV [ESI+0xC],EAX; CALL
        uint8_t* a = sig_find("X2-FX-010-LOOPDEFER restart (FUN_00805b80)",
            "8B 46 0C 3B 41 1C 7C 72 8B 41 18 56 89 46 0C E8 ?? ?? ?? ?? 83 C4 04", 0x806094);
        uint8_t* b = sig_find("X2-FX-010-LOOPDEFER restart (FUN_00806130)",
            "8B 46 0C 3B 41 1C 0F 8C ?? ?? ?? ?? 8B 41 18 56 89 46 0C E8 ?? ?? ?? ?? 83 C4 04", 0x8063DC);
        uint8_t* ta = a ? call_target(a + 0xF) : nullptr;
        uint8_t* tb = b ? call_target(b + 0x13) : nullptr;
        // FUN_007fff80: MOV EAX,[ESP+4]-style cdecl(particle) reset; both sites must call it
        if (!ta || ta != tb || !in_image(ta)) {
            log_line("  X2-FX-010-LOOPDEFER: restart CALLs not found or differ - sub-switch off");
            g_loopdefer = 0;
        } else {
            // field step FUN_007fef20: PUSH EBP; MOV EBP,ESP; FLD1; PUSH EBX; FST [..]; PUSH ESI; FST [..]; PUSH imm; FST; FSTP
            g_fstep = sig_find("X2-FX-010-LOOPDEFER field step FUN_007fef20",
                "55 8B EC D9 E8 53 D9 15 ?? ?? ?? ?? 56 D9 15 ?? ?? ?? ?? 68 ?? ?? ?? ?? D9 15 ?? ?? ?? ?? D9 1D ?? ?? ?? ??",
                0x7FEF20);
            if (!g_fstep) log_line("  X2-FX-010-LOOPDEFER: field step not found - field programs not covered");
            g_restart = (restart_t)ta;
            g_restart_site[0] = a + 0xF;
            g_restart_site[1] = b + 0x13;
            log_line("  X2-FX-010-LOOPDEFER: restart gva=0x%08X (0x007FFF80), CALLs gva=0x%08X / 0x%08X (0x008060A3 / 0x008063EF)",
                     rt_to_gva(ta), rt_to_gva(g_restart_site[0]), rt_to_gva(g_restart_site[1]));
        }
    }
    log_line("  X2-MAG-006: Ppp_ProgramStep gva=0x%08X (entry + override CALL patched on the game thread), pause flag gva=0x%08X "
             "(expected 0x01CF579C), override gva=0x%08X skipped while paused, FRESH %s",
             rt_to_gva(g_entry), rt_to_gva((const void*)g_pause), rt_to_gva(ov), g_fresh_on ? "ON" : "OFF");
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    // both patches on the game thread, between invocations: the step never runs while they are written
    if (!patch_call_rel32(g_entry + 8, (void*)g_override, (void*)&h_override)) {
        log_line("X2-MAG-006: override CALL patch failed - row off");
        row_MAG006.on = 0;
        return;
    }
    if (!hook_entry("X2-MAG-006", g_entry, (void*)&h_ppp_step, (void**)&g_tramp)) {
        log_line("X2-MAG-006: entry hook failed - row off (the override stub is pass-through alone)");
        row_MAG006.on = 0;
        return;
    }
    if (g_loopdefer) {
        if (!patch_call_rel32(g_restart_site[0], (void*)g_restart, (void*)&h_restart) ||
            !patch_call_rel32(g_restart_site[1], (void*)g_restart, (void*)&h_restart)) {
            patch_call_rel32(g_restart_site[0], (void*)&h_restart, (void*)g_restart);
            log_line("X2-FX-010-LOOPDEFER: restart CALL patch failed - off");
            g_loopdefer = 0;
        } else {
            if (g_fstep && !hook_entry("X2-FX-010-LOOPDEFER field step", g_fstep, (void*)&h_field_step, (void**)&g_fstep_tramp)) {
                log_line("X2-FX-010-LOOPDEFER: field step hook failed - field programs not covered");
                g_fstep = nullptr;
            }
            log_line("X2-FX-010-LOOPDEFER: particle loop restarts deferred to the program's next unpaused step (DLL and EXE "
                     "callers%s)", g_fstep ? ", field step FUN_007fef20" : "");
        }
    }
}

static void post() {
    static bool down_prev = false;
    bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (down && !down_prev) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            LONG v = g_unpause_test ? 0 : 1;
            InterlockedExchange(&g_unpause_test, v);
            log_line("F11: X2-MAG-006-UNPAUSE-TEST %s (DLL particle steps on render-only frames %s; unpaused so far %ld)",
                     v ? "ON" : "OFF", v ? "UNPAUSED, effects 2x" : "paused as normal", g_unpaused_calls);
        }
    }
    down_prev = down;
    static LONG reported = 0;
    if (!reported && g_paused_calls > 0) {
        reported = 1;
        log_line("X2-MAG-006: first paused DLL particle step on a render-only invocation (EXE callers passed %ld, fresh %ld)",
                 g_exe_calls, g_fresh_calls);
    }
    static DWORD last_def = 0;
    static LONG last_total = 0;
    DWORD now = GetTickCount();
    if (g_loopdefer && now - last_def >= 10000 && g_def_total != last_total) {
        last_def = now;
        last_total = g_def_total;
        log_line("X2-FX-010-LOOPDEFER: deferred %ld applied %ld dropped %ld queue-full %ld (queued now %d)",
                 g_def_total, g_def_applied, g_def_dropped, g_def_full, g_deferred_n);
    }
    static LONG ov_seen = 0;
    if (g_override_skipped && !ov_seen) {
        ov_seen = 1;
        log_line("X2-MAG-006: pause override FUN_006ba090 skipped inside a paused step (first time)");
    }
}

Row row_MAG006 = {
    "X2-MAG-006",
    "DLL particle programs: pause flag on render-only invocations (draw still runs); EXE callers untouched",
    1, install, pre, post, 0
};

} // namespace x2
