// SPDX-License-Identifier: Apache-2.0
// X2-MENU-001: PS2-layer menus (title, field menu, save/load, item-get, script menus) run as the coroutine
// Menu_CoroutineIterate 0x7645E0. While one is up ([0x016BC55C] or [0x016BC560] != 0) Sg_MainLoop jumps past the exec
// loop (0x60550C), so exec alternation does nothing there, and the coroutine steps once per HOST frame (gate on the
// turbo phase counter [0xDF6414] at 0x764824): 2x at a 60 Hz host (U 12/10/2026: title screen, main menu).
// Fix A (MENU.md): hold stock pacing (divider 2, exec 1) while a menu is up or requested ([0xDF6870] != -1); menus
// render at 30 fps as in the stock game. No game code is patched. Transitions are logged with the menu mode
// [0x016BC5EC] (confirms the title screen is a menu mode, H in MENU.md).
// Evidence: outer repo re/ledger/MENU.md (S); D: X2-DIAG-RATE, fps60x2.log 23:11.
#include "../x2.h"

namespace x2 {

static volatile int32_t* g_up = nullptr;         // 0x016BC55C menu coroutine in progress
static volatile int32_t* g_up2 = nullptr;        // 0x016BC560 second exit flag (same JNZ target)
static volatile int32_t* g_req = nullptr;        // 0x00DF6870 requested mode, -1 = none
static volatile uint32_t* g_mode = nullptr;      // 0x016BC5EC current mode (log only)
static bool g_holding = false;
extern Row row_MENU001;

static bool install() {
    // Sg_MainLoop: CALL ..; CMP [55c],0; JNZ exit; CMP [560],0; JNZ exit; CMP [514],1; JNZ
    uint8_t* ex = sig_find("X2-MENU-001 Sg_MainLoop menu exit",
        "E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 01 75 2D",
        0x605507);
    // Sg_MainLoop: PUSH EBX; CALL Sg_MenuRequestTest (starts MOV ECX,[request])
    uint8_t* rq = sig_find("X2-MENU-001 Sg_MainLoop menu test call",
        "53 E8 ?? ?? ?? ?? 8B 1D ?? ?? ?? ?? 8B 3D ?? ?? ?? ?? 85 C0 74 48", 0x60549F);
    uint8_t* co = sig_find("X2-MENU-001 Menu_CoroutineIterate",
        "55 8B EC 53 56 8B 75 08 8B DE 81 E3 00 00 FF FF 83 3D ?? ?? ?? ?? 00 57 89 35", 0x7645E0);
    if (!ex || !rq || !co) return false;
    // both JNZ go to the same exit
    uint8_t* j1 = ex + 0x12 + *(const int32_t*)(ex + 0x0E);
    uint8_t* j2 = ex + 0x1F + *(const int32_t*)(ex + 0x1B);
    if (j1 != j2) { log_line("  X2-MENU-001: the two menu exits differ - refused"); return false; }
    uint8_t* test = call_target(rq + 1);
    if (!test || !sig_match(test, "8B 0D")) { log_line("  X2-MENU-001: menu test %p unexpected - refused", test); return false; }
    g_up = (volatile int32_t*)abs_operand("menu in progress", ex + 7);
    g_up2 = (volatile int32_t*)abs_operand("menu in progress 2", ex + 0x14);
    g_req = (volatile int32_t*)abs_operand("menu request", test + 2);
    g_mode = (volatile uint32_t*)abs_operand("menu mode", co + 0x1A);
    if (!g_up || !g_up2 || !g_req || !g_mode) return false;
    if (*(const uint32_t*)(co + 0x12) != (uint32_t)(uintptr_t)g_up) {
        log_line("  X2-MENU-001: coroutine init flag is not the in-progress flag - refused");
        return false;
    }
    log_line("  X2-MENU-001: flags gva=0x%08X/0x%08X (expected 0x016BC55C/560), request gva=0x%08X (0x00DF6870), "
             "mode gva=0x%08X (0x016BC5EC); exit gva=0x%08X",
             rt_to_gva((const void*)g_up), rt_to_gva((const void*)g_up2), rt_to_gva((const void*)g_req),
             rt_to_gva((const void*)g_mode), rt_to_gva(j1));
    return true;
}

static void pre() {
    bool up = *g_up != 0 || *g_up2 != 0 || *g_req != -1;
    if (up == g_holding) return;
    g_holding = up;
    log_line("X2-MENU-001: menu %s (in progress %d/%d, request %d, mode 0x%X) - %s", up ? "up" : "closed", *g_up, *g_up2,
             *g_req, *g_mode, up ? "stock pacing" : "60 Hz pacing");
    pacing_hold_stock("X2-MENU-001 (PS2 menu)", up);
}

Row row_MENU001 = {
    "X2-MENU-001",
    "PS2-layer menus (title, field menu, save/load) at stock pacing: their coroutine steps once per host frame",
    1, install, pre, nullptr, 0
};

} // namespace x2
