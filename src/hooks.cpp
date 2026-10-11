// SPDX-License-Identifier: Apache-2.0
// Hooks: the frame driver (the runner's CALL Sg_MainLoop re-pointed to a wrapper that runs pre/post around
// every invocation), the SKS_SetFrameDivider entry hook, and the CALL-retarget helper used by rows.
// Details: docs/fps60/hooks.md
#include "fps60.h"
#include <string.h>

volatile LONG g_disabled = 0;
bool g_divider_hooked = false;

static void* g_mainloop_target = nullptr;      // read by the naked wrapper

void on_fault(const char* where, DWORD code) {
    if (InterlockedExchange(&g_disabled, 1) == 0) {
        log_line("FAULT 0x%08lX in %s - fps60 disabled, hooks are pass-through from now on", code, where);
        capture_abort("fps60 disabled by a fault");    // its own SEH; closes an open capture log
    }
}

// ---------------------------------------------------------------- capture points (TRIAGE E0)
// Each point has its own SEH frame: a capture fault disables the capture only, never fps60.
static void cap(int point) {
    __try { capture_point(point); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        static const char* const names[] = { "capture PRE_OUTER", "capture PRE_PACED", "capture PRE_INNER",
                                             "capture POST_INNER", "capture POST_OUTER" };
        capture_on_fault((point >= 0 && point <= CAP_POST_OUTER) ? names[point] : "capture", GetExceptionCode());
    }
}

// ---------------------------------------------------------------- SEH-guarded C bodies
// Order per invocation. Capture points (no-ops unless [capture] enabled=1) mark the outer and inner
// diff boundaries (docs/fps60/capture.md).
extern "C" void __cdecl fp_pre() {
    if (g_disabled) return;
    __try {
        where_set(W_PRE, 0xFF);
        hooks_install_divider_on_game_thread();
        cap(CAP_PRE_OUTER);
        where_set(W_PACING_PRE, 0xFF);
        pacing_pre();
        cap(CAP_PRE_PACED);
        where_set(W_ROLLBACK_PRE, 0xFF);
        rollback_pre();
        rows_pre();
        cap(CAP_PRE_INNER);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("pre", GetExceptionCode()); }
    where_set(W_MAINLOOP, 0xFF);                 // DIAG-HANGDUMP breadcrumb
}

extern "C" void __cdecl fp_post() {
    if (g_disabled) return;
    __try {
        cap(CAP_POST_INNER);
        rows_post();
        where_set(W_ROLLBACK_POST, 0xFF);
        rollback_post();
        where_set(W_PACING_POST, 0xFF);
        pacing_post();
        cap(CAP_POST_OUTER);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("post", GetExceptionCode()); }
    where_set(W_BETWEEN, 0xFF);
}

__declspec(naked) static void sg_mainloop_wrapper() {
    __asm {
        call fp_pre
        push dword ptr [esp + 4]          // the float delta (caller's arg)
        call dword ptr [g_mainloop_target]
        add  esp, 4
        call fp_post
        ret
    }
}

bool hooks_install_mainloop() {
    uint8_t* site = g_mainloop_callsite;
    if (!site || !g_sg_mainloop) return false;

    // Re-verify the exact bytes right before patching: E8 <rel32 -> Sg_MainLoop>.
    int32_t want_rel = (int32_t)((uint8_t*)g_sg_mainloop - (site + 5));
    if (site[0] != 0xE8 || *(int32_t*)(site + 1) != want_rel) {
        log_line("hook mainloop: call site %p changed under us - NOT installed", site);
        return false;
    }
    g_mainloop_target = g_sg_mainloop;

    int32_t rel = (int32_t)((uint8_t*)&sg_mainloop_wrapper - (site + 5));
    volatile LONG* p = (volatile LONG*)(site + 1);
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        log_line("hook mainloop: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    bool aligned = (((uintptr_t)p) & 3) == 0;
    if (aligned) InterlockedExchange(p, (LONG)rel);    // atomic: the game thread may be executing nearby
    else         memcpy((void*)p, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);

    log_line("hook mainloop: ON - CALL at gva=0x%08X (rt %p) re-pointed to wrapper %p (rel32 write %s); "
             "Sg_MainLoop gva=0x%08X",
             rt_to_gva_exe(site), site, (void*)&sg_mainloop_wrapper, aligned ? "atomic" : "NON-atomic",
             rt_to_gva_exe(g_sg_mainloop));
    return true;
}

// ---------------------------------------------------------------- CALL rel32 retarget
// Re-points one verified `CALL old_target` to new_target (same method as the main loop hook:
// one aligned rel32 write, atomic where possible). The callee keeps the caller's convention.
bool patch_call_rel32(uint8_t* site, void* old_target, void* new_target) {
    if (!site || site[0] != 0xE8 || call_target(site) != (uint8_t*)old_target) return false;
    int32_t rel = (int32_t)((uint8_t*)new_target - (site + 5));
    volatile LONG* p = (volatile LONG*)(site + 1);
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    if ((((uintptr_t)p) & 3) == 0) InterlockedExchange(p, (LONG)rel);
    else memcpy((void*)p, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

// ---------------------------------------------------------------- divider hook
static set_divider_t g_divider_tramp = nullptr;

static void __cdecl h_set_divider(uint32_t req) {
    bool swallow = false;
    if (!g_disabled) {
        __try { swallow = pacing_divider_request(req); }
        __except (EXCEPTION_EXECUTE_HANDLER) { swallow = false; on_fault("divider hook", GetExceptionCode()); }
        __try { capture_divider_event(req, swallow); }  // observe only (flag D)
        __except (EXCEPTION_EXECUTE_HANDLER) { capture_on_fault("capture divider event", GetExceptionCode()); }
    }
    if (!swallow) g_divider_tramp(req);
}

void hooks_install_divider_on_game_thread() {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    if (!g_set_divider_orig || !g_divider) {
        log_line("hook divider: SKS_SetFrameDivider not resolved - pacing stays stock");
        return;
    }
    uint8_t* entry = (uint8_t*)g_set_divider_orig;
    if (!hook_entry("divider", entry, (void*)&h_set_divider, (void**)&g_divider_tramp)) {
        log_line("hook divider: NOT installed, pacing stays stock");
        return;
    }
    g_set_divider_orig = g_divider_tramp;        // our own calls bypass the hook
    g_divider_hooked = true;
    log_line("hook divider: ON - SKS_SetFrameDivider gva=0x%08X entry hooked (tramp %p), divider now %u",
             rt_to_gva_exe(entry), (void*)g_divider_tramp, *g_divider);
}
