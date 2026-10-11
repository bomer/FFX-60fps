// SPDX-License-Identifier: Apache-2.0
// DEV-TURBO (test tool): while a key is held, the game runs DEV-TURBO-SPEED times faster through its own turbo
// path. The game scales the Sg_MainLoop delta by its turbo factor (FUN_006f7340, table {1,2,4}); Sg_WaitFrameSync
// turns the larger delta into a backlog count, and the runner executes that many invocations, all but the last
// with skip-render (re/ledger/CLK.md CLK-009/010). Frameskip is only allowed while turbo is active
// (FUN_006f7330 -> FUN_0081fd30). The row forces both while the key is held. Pacing treats the extra invocations
// as backlog (logic) invocations.
// ini [rows]: DEV-TURBO=1, DEV-TURBO-KEY=<virtual-key code, decimal; 96 = numpad 0>, DEV-TURBO-SPEED=<factor>.
#include "../fps60.h"
#include <stdio.h>

typedef float(__cdecl* scale_t)(float delta);
typedef int(__cdecl* active_t)(void);

static uint8_t* g_scale_fn = nullptr;     // FUN_006f7340
static uint8_t* g_active_fn = nullptr;    // FUN_006f7330
static scale_t  g_scale_tramp = nullptr;
static active_t g_active_tramp = nullptr;
static bool     g_patched = false;
static int      g_key = 0x60, g_speed = 10;
static volatile bool g_held = false;

// cdecl(float delta) -> float in ST0, plain RET (0x6F7421). The original still runs (its own turbo state).
static float __cdecl h_scale(float delta) {
    float r = g_scale_tramp(delta);
    return g_held ? delta * (float)g_speed : r;
}

// cdecl, no args, returns turbo index != 0 in EAX, plain RET (0x6F733B).
static int __cdecl h_active() {
    return g_held ? 1 : g_active_tramp();
}

static bool install() {
    g_key = config_row("DEV-TURBO-KEY", 0x60);
    g_speed = config_row("DEV-TURBO-SPEED", 10);
    if (g_speed < 2) g_speed = 2;
    if (g_speed > 30) g_speed = 30;
    // FUN_006f7340: PUSH EBP; MOV EBP,ESP; CMP byte [enabled],0; JZ far; PUSH EBX; CALL; CMP EAX,1; SETZ BH; CALL
    g_scale_fn = sig_find_unique("DEV-TURBO turbo delta scale FUN_006f7340",
        "55 8B EC 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 53 E8 ?? ?? ?? ?? 83 F8 01 0F 94 C7 E8", 0x6F7340);
    if (!g_scale_fn) return false;
    // The turbo index global, from 0x6F73CD MOV EAX,[index] (+0x8D) after FLD table[EAX*4] check shape.
    if (!sig_match(g_scale_fn + 0x8D, "A1 ?? ?? ?? ?? 85 C0 74 ?? 83 3D ?? ?? ?? ?? 00 D9 04 85")) {
        log_line("  DEV-TURBO: FUN_006f7340 body has an unexpected shape - refused");
        return false;
    }
    uint32_t idx = (uint32_t)(uintptr_t)read_abs(g_scale_fn + 0x8E);
    // FUN_006f7330: XOR EAX,EAX; CMP [index],EAX; SETNZ AL; RET (same global)
    char pat[96];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "33 C0 39 05 %02X %02X %02X %02X 0F 95 C0 C3",
                idx & 0xFF, (idx >> 8) & 0xFF, (idx >> 16) & 0xFF, idx >> 24);
    g_active_fn = sig_find_unique("DEV-TURBO turbo active FUN_006f7330", pat, 0x6F7330);
    if (!g_active_fn) return false;
    log_line("  DEV-TURBO: scale gva=0x%08X, active gva=0x%08X, index gva=0x%08X; hold VK 0x%02X for x%d",
             rt_to_gva_exe(g_scale_fn), rt_to_gva_exe(g_active_fn), rt_to_gva_exe((void*)(uintptr_t)idx), g_key, g_speed);
    return true;
}

static void pre() {
    if (!g_patched) {
        g_patched = true;
        if (!hook_entry("DEV-TURBO scale", g_scale_fn, (void*)&h_scale, (void**)&g_scale_tramp) ||
            !hook_entry("DEV-TURBO active", g_active_fn, (void*)&h_active, (void**)&g_active_tramp)) {
            hook_remove("DEV-TURBO scale", g_scale_fn);
            log_line("DEV-TURBO: hook failed - row off");
            row_DEVTURBO.on = 0;
            return;
        }
        log_line("DEV-TURBO: hooked on the game thread");
    }
    bool down = (GetAsyncKeyState(g_key) & 0x8000) != 0;
    if (down) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        down = pid == GetCurrentProcessId();
    }
    if (down != g_held) {
        g_held = down;
        log_line("DEV-TURBO: %s (x%d)", down ? "on" : "off", g_speed);
    }
}

Row row_DEVTURBO = {
    "DEV-TURBO",
    "test tool: hold a key (DEV-TURBO-KEY) to run the game DEV-TURBO-SPEED times faster via its own turbo/backlog path",
    1, install, pre, nullptr, 0
};
