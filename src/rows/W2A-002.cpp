// SPDX-License-Identifier: Apache-2.0
// W2A-002: FMVs advance one frame per invocation, so at 60 fps a movie plays 2x and drifts from its audio.
// Fix: hold stock 30 Hz pacing while a movie plays.
// Details: docs/fps60/rows/W2A-002.md
#include "../fps60.h"

static uint8_t* volatile* g_singleton = nullptr;   // &DAT_00cded3c (runtime)
static bool g_holding = false;

static bool install() {
    // FMV step: get the FMV manager, check its playing flag, then decode the next frame
    uint8_t* at = sig_find_unique("W2A-002 FMV step FUN_00645df0",
        "E8 ?? ?? ?? ?? 80 B8 D0 06 00 00 00 74 1E E8 ?? ?? ?? ?? 50 E8", 0x645DF0);
    if (!at) return false;
    uint8_t* getter = call_target(at);
    // The manager getter: MOV EAX,[singleton]; TEST EAX,EAX; JNZ
    if (!getter || getter[0] != 0xA1 || getter[5] != 0x85 || getter[6] != 0xC0 || getter[7] != 0x75) {
        log_line("  W2A-002: getter %p bytes unexpected - refused", getter);
        return false;
    }
    g_singleton = (uint8_t* volatile*)read_abs(getter + 1);
    log_line("  W2A-002: FMV manager singleton at gva=0x%08X (expected 0x00CDED3C), playing flag +0x6D0",
             rt_to_gva_exe((const void*)g_singleton));
    return true;
}

static void pre() {
    uint32_t mgr = 0;
    bool playing = false;
    if (safe_read_u32((const void*)g_singleton, &mgr) && mgr) {
        uint32_t w = 0;
        if (safe_read_u32((const void*)(uintptr_t)(mgr + 0x6D0), &w)) playing = (w & 0xFF) != 0;
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
