// SPDX-License-Identifier: Apache-2.0
// X2-W2A-002 (port of FFX W2A-002): the FMV step (playing check, skip check FUN_00437dc0, frame advance
// FUN_00435600) is inlined in FUN_00466ef0, which animate runs once per host frame after the backlog runner, so at a
// 60 Hz host a movie plays 2x and drifts from its audio.
// Mode 2 (default, as FFX): the CALL of the FMV manager getter at 0x4672E7 goes to a stub that returns a zeroed
// dummy manager (playing flag 0, both calls skipped) unless a logic invocation ran since the last FMV step, so the
// movie advances once per logic tick. Mode 1: hold stock pacing while [mgr]+0x6CC is set.
// Evidence: outer repo re/ledger/PORT-A.md §6 (S).
#include "../x2.h"

namespace x2 {

typedef uint8_t* (__cdecl* getter_t)(void);      // MOV EAX,[singleton]; TEST; JNZ ... plain no-arg getter

static uint8_t* volatile* g_singleton = nullptr;
static getter_t g_getter = nullptr;
static uint32_t g_flag_off = 0x6CC;
static int  g_mode = 2;
static bool g_holding = false;
static volatile LONG g_logic_since_step = 1;
static volatile LONG g_skipped = 0;
static uint8_t g_dummy[0x800];                   // zeroed: "not playing"
extern Row row_W2A002;

static uint8_t* __cdecl h_getter() {
    if (!row_W2A002.on || g_disabled || InterlockedExchange(&g_logic_since_step, 0)) return g_getter();
    if (InterlockedIncrement(&g_skipped) == 1) log_line("X2-W2A-002: first FMV step skipped (no logic invocation since the last one)");
    return g_dummy;
}

static bool install() {
    uint8_t* at = sig_find("X2-W2A-002 FMV step (FUN_00466ef0)",
        "E8 ?? ?? ?? ?? 80 B8 CC 06 00 00 00 74 1E E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
        "8B C8 E8", 0x4672E7);
    if (!at) return false;
    uint8_t* getter = call_target(at);
    if (!getter || !sig_match(getter, "A1 ?? ?? ?? ?? 85 C0 75")) {
        log_line("  X2-W2A-002: getter %p bytes unexpected - refused", getter);
        return false;
    }
    g_singleton = (uint8_t* volatile*)abs_operand("FMV manager singleton", getter + 1);
    if (!g_singleton) return false;
    g_flag_off = *(const uint32_t*)(at + 7);
    if (g_flag_off + 1 > sizeof(g_dummy)) return false;
    g_getter = (getter_t)getter;
    g_mode = config_row("X2-W2A-002-MODE", 2);
    if (g_mode == 2 && !patch_call_rel32(at, getter, (void*)&h_getter)) {
        log_line("  X2-W2A-002: patch failed - mode 1");
        g_mode = 1;
    }
    log_line("  X2-W2A-002: singleton gva=0x%08X (expected 0x00DC594C), playing flag +0x%X; mode %d (%s)",
             rt_to_gva((const void*)g_singleton), g_flag_off, g_mode,
             g_mode == 2 ? "60 Hz, movie advanced once per logic tick" : "stock pacing while a movie plays");
    return true;
}

static void pre() {
    if (!render_only()) InterlockedExchange(&g_logic_since_step, 1);
    uint32_t mgr = 0, w = 0;
    bool playing = false;
    if (safe_read_u32((const void*)g_singleton, &mgr) && mgr && safe_read_u32((const void*)(uintptr_t)(mgr + g_flag_off), &w))
        playing = (w & 0xFF) != 0;
    if (playing == g_holding) return;
    g_holding = playing;
    if (g_mode == 2) log_line("X2-W2A-002: movie %s (mode 2, pacing stays 60 Hz)", playing ? "starts" : "stops");
    else pacing_hold_stock("X2-W2A-002 (FMV)", playing);
}

Row row_W2A002 = {
    "X2-W2A-002",
    "FMV advances once per logic tick (mode 2) or stock pacing while a movie plays (mode 1)",
    1, install, pre, nullptr, 0
};

} // namespace x2
