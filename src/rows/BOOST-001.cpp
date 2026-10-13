// SPDX-License-Identifier: Apache-2.0
// BOOST-001 (default on): the game's speed booster (2x / 4x) at 60 Hz. The booster does not run extra ticks itself:
// FUN_006f7340 0x6F7340 (first call in Sg_MainLoop) multiplies the frame delta by table {1,2,4}[DAT_00ce82b4] while
// it is engaged (DAT_00ce82b8 = 1), and Sg_WaitFrameSync turns delta into a backlog count in 30 Hz ticks
// (ftol(delta * 30), only when >= 2 vblanks; re/ledger/CLK.md CLK-009, re/MAINLOOP.md section 5). At stock 30 Hz that
// gives 2 / 4 ticks per frame. Under 60 Hz pacing delta is 1/60 s: 2x gives count 1 (no extra tick: 2x did nothing)
// and 4x count 2 (one backlog tick + the alternation, 3x). While the booster is engaged this row makes every
// invocation a logic tick (pacing_force_logic, kind INV_BACKLOG): 2x = 1 tick per 60 Hz frame (60/s), 4x = the
// game's backlog tick + 1 (120/s), the stock booster rates. Read once per invocation; the flag is the previous
// invocation's (one frame late at the edges).
#include "../fps60.h"

static volatile int32_t* g_index = nullptr;   // DAT_00ce82b4: 0 off, 1 = 2x, 2 = 4x
static volatile int32_t* g_engaged = nullptr; // DAT_00ce82b8: set by FUN_006f7340 when the scale applied
static volatile float*   g_table = nullptr;   // DAT_00c38e1c {1,2,4}
static bool g_on = false;

static void pre() {
    int32_t idx = *g_index;
    bool on = !g_disabled && *g_engaged != 0 && idx > 0 && idx < 3;
    if (on != g_on) {
        g_on = on;
        pacing_force_logic(on);
        if (on) log_line("BOOST-001: booster %.0fx engaged - every invocation is a logic tick", g_table[idx]);
        else    log_line("BOOST-001: booster off - logic / render-only alternation");
    }
}

static bool install() {
    // FUN_006f7340: PUSH EBP; MOV EBP,ESP; CMP byte [enabled],0; JZ far; PUSH EBX; CALL; CMP EAX,1; SETZ BH; CALL
    uint8_t* f = sig_find_unique("BOOST-001 booster delta scale FUN_006f7340",
        "55 8B EC 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 53 E8 ?? ?? ?? ?? 83 F8 01 0F 94 C7 E8", 0x6F7340);
    if (!f) return false;
    // 0x6F73CD MOV EAX,[index]; TEST EAX,EAX; JZ; CMP dword [engaged],0; FLD float [EAX*4+table]
    if (!sig_match(f + 0x8D, "A1 ?? ?? ?? ?? 85 C0 74 ?? 83 3D ?? ?? ?? ?? 00 D9 04 85")) {
        log_line("  BOOST-001: FUN_006f7340 body has an unexpected shape - refused");
        return false;
    }
    g_index = (volatile int32_t*)read_abs(f + 0x8E);
    g_engaged = (volatile int32_t*)read_abs(f + 0x98);
    g_table = (volatile float*)read_abs(f + 0xA0);
    if (!g_index || !g_engaged || !g_table) return false;
    log_line("  BOOST-001: index gva=0x%08X engaged gva=0x%08X table gva=0x%08X {%.0f,%.0f,%.0f}",
             rt_to_gva_exe((void*)g_index), rt_to_gva_exe((void*)g_engaged), rt_to_gva_exe((void*)g_table),
             g_table[0], g_table[1], g_table[2]);
    return true;
}

static void stop() {
    if (g_on) pacing_force_logic(false);
    g_on = false;
}

Row row_BOOST001 = {
    "BOOST-001",
    "game speed booster 2x/4x at the stock rates under 60 Hz pacing (every invocation a logic tick while engaged)",
    1, install, pre, nullptr, 0, stop
};
