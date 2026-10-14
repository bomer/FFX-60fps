// SPDX-License-Identifier: Apache-2.0
// W2A-002: FMVs advance one frame per invocation, so at 60 fps a movie plays 2x and drifts from its audio.
// Fix: hold stock 30 Hz pacing while a movie plays.
// W2A-002-MODE=2 (default; U 12/10/2026 fixed): scenes with real-time characters over a playing movie went 30 fps (U 12/10/2026, Kilika, between
// two cutscenes). Instead of holding stock, the FMV step FUN_00645df0 (void(void), plain RET 0x645E1C; its only
// caller is Sg_MainLoop 0x820B2F; it runs the skip check FUN_006d9460 and the frame advance FUN_006d7680) returns at
// once on render-only invocations, so the movie advances one frame per logic tick, as in stock.
// Details: docs/fps60/rows/W2A-002.md
#include "../fps60.h"

static uint8_t* volatile* g_singleton = nullptr;   // &DAT_00cded3c (runtime)
static bool g_holding = false;
static int  g_mode = 2;
static void (*g_step_tramp)() = nullptr;
static uint8_t* g_step = nullptr;
static bool g_hooked = false;
static volatile LONG g_skipped = 0;

static void h_step() {
    if (pacing_kind() == INV_BUILD) {
        if (InterlockedIncrement(&g_skipped) == 1) log_line("W2A-002: first FMV step skipped on a render-only invocation (mode 2)");
        return;
    }
    g_step_tramp();
}

static bool install() {
    // FMV step: get the FMV manager, check its playing flag, then decode the next frame
    uint8_t* at = sig_find_unique("W2A-002 FMV step FUN_00645df0",
        "E8 ?? ?? ?? ?? 80 B8 D0 06 00 00 00 74 1E E8 ?? ?? ?? ?? 50 E8", 0x645DF0);
    if (!at) return false;
    g_step = at;
    g_mode = config_row("W2A-002-MODE", 2);
    uint8_t* getter = call_target(at);
    // The manager getter: MOV EAX,[singleton]; TEST EAX,EAX; JNZ
    if (!getter || getter[0] != 0xA1 || getter[5] != 0x85 || getter[6] != 0xC0 || getter[7] != 0x75) {
        log_line("  W2A-002: getter %p bytes unexpected - refused", getter);
        return false;
    }
    g_singleton = (uint8_t* volatile*)read_abs(getter + 1);
    log_line("  W2A-002: FMV manager singleton at gva=0x%08X (expected 0x00CDED3C), playing flag +0x6D0",
             rt_to_gva_exe((const void*)g_singleton));
    log_line("  W2A-002: mode %d (%s)", g_mode, g_mode == 2 ? "60 Hz, movie advanced once per logic tick" : "stock pacing while a movie plays");
    return true;
}

static void pre() {
    if (g_mode == 2) {
        if (!g_hooked) {
            g_hooked = true;
            if (hook_entry("W2A-002", g_step, (void*)&h_step, (void**)&g_step_tramp))
                log_line("W2A-002: FMV step gva 0x%08X hooked (mode 2)", rt_to_gva_exe(g_step));
            else { g_mode = 1; log_line("W2A-002: hook failed - mode 1"); }
        }
    }
    uint32_t mgr = 0;
    bool playing = false;
    if (safe_read_u32((const void*)g_singleton, &mgr) && mgr) {
        uint32_t w = 0;
        if (safe_read_u32((const void*)(uintptr_t)(mgr + 0x6D0), &w)) playing = (w & 0xFF) != 0;
    }
    if (g_mode == 2) {
        if (playing != g_holding) { g_holding = playing; log_line("W2A-002: movie %s (mode 2, pacing stays 60 Hz)", playing ? "starts" : "stops"); }
        return;
    }
    if (playing != g_holding) {
        g_holding = playing;
        pacing_hold_stock("W2A-002 (FMV)", playing);
    }
}

Row row_W2A002 = {
    "W2A-002",
    "hold stock pacing while an FMV plays (video/audio sync)",
    1, install, pre, nullptr, 0
};
