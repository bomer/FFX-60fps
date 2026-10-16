// SPDX-License-Identifier: Apache-2.0
// DEV-TURBO (test tool): while a key is held, the game runs DEV-TURBO-SPEED times faster through its own turbo
// path. The game scales the Sg_MainLoop delta by its turbo factor (FUN_006f7340, table {1,2,4}); Sg_WaitFrameSync
// turns the larger delta into a backlog count, and the runner executes that many invocations, all but the last
// with skip-render (re/ledger/CLK.md CLK-009/010). Frameskip is only allowed while turbo is active
// (FUN_006f7330 -> FUN_0081fd30). The row forces both while the key is held. Pacing treats the extra invocations
// as backlog (logic) invocations.
// ini [rows]: DEV-TURBO=1, DEV-TURBO-KEY=<virtual-key code, decimal; 96 = numpad 0>, DEV-TURBO-SPEED=<factor>.
// The game's booster also switches the sound into its turbo mode when it engages: FUN_006f7340 calls
// FUN_00821c60(1) ("OnEnable", sound commands FUN_0067a380 0x3a / FUN_0067a570) and (0) when it disengages. The key
// path skipped that (U 12/10/2026: turbo does not speed up sound, voiced cutscenes wait for the dialog), so the row
// calls it on the key edges (DEV-TURBO-SOUND=1). FUN_00821c60: cdecl(int), plain RET 0x821CAA, own state 0x012FB8EC.
// Voiced cutscenes still waited at 1x under turbo (U 12/10/2026). re/ledger/TURBO-VOICE.md, DEV-TURBO-VOICE=1:
// - A: voice-busy query FUN_0067a710 (cdecl, no args, EAX, plain RET 0x67A71D; sound cmd 0x11 = FmodVoice
//   +0x40|+0x44, cleared in real time) returns 0 while the key is held, so VoiceStatus/VoiceSync and the spin
//   waits before music/SE commands do not wait for the (muted) line.
// - B: SyncData scenes ignore the delta: Sg_WaitFrameSync 0x821E80 (cdecl(float), plain RET 0x822244) compares the
//   us clock FUN_00641420 minus the base DAT_012fb818 with the entry time and catches up late entries. While held,
//   the base is moved back by the extra turbo time (capped), so the game's own catch-up runs the scene faster.
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
static void (__cdecl* g_sound_fn)(int) = nullptr;   // FUN_00821c60
static volatile int32_t* g_booster_on = nullptr;     // DAT_00ce82b8 (index + 4)
static int (__cdecl* g_vbusy_tramp)() = nullptr;
static void (__cdecl* g_wfs_tramp)(float) = nullptr;
static uint8_t* g_vbusy_fn = nullptr;               // FUN_0067a710
static uint8_t* g_wfs_fn = nullptr;                 // Sg_WaitFrameSync
static volatile int32_t* g_sync_n = nullptr;        // DAT_012fb808 invocations to run
static volatile int32_t* g_sync_mode = nullptr;     // DAT_012fb8a0 == 1: SyncData scene
static volatile int32_t* g_sync_first = nullptr;    // DAT_00c494d4 == 1: base not set yet
static volatile uint64_t* g_sync_base = nullptr;    // DAT_012fb818 (us)
static volatile LONG g_vbusy_n = 0, g_sync_shift_n = 0;

static int __cdecl h_vbusy() {
    if (g_held) {
        if (InterlockedIncrement(&g_vbusy_n) == 1) log_line("DEV-TURBO: first voice-busy query answered idle under turbo");
        return 0;
    }
    return g_vbusy_tramp();
}

static void __cdecl h_wfs(float delta) {
    if (g_held && *g_sync_n == 0 && *g_sync_mode == 1 && *g_sync_first != 1 && delta > 0) {
        float extra = delta * (1.0f - 1.0f / (float)g_speed);
        if (extra > 0.25f) extra = 0.25f;                // < 30 SyncData entries per host frame (table DAT_012fb828)
        *g_sync_base -= (uint64_t)(extra * 1e6f);
        if (InterlockedIncrement(&g_sync_shift_n) == 1) log_line("DEV-TURBO: first SyncData base shift (%.0f ms)", extra * 1000.0f);
    }
    g_wfs_tramp(delta);
}

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
    if (config_row("DEV-TURBO-SOUND", 1)) {
        g_sound_fn = (void (__cdecl*)(int))sig_find_unique("DEV-TURBO booster sound switch FUN_00821c60",
            "55 8B EC A1 ?? ?? ?? ?? 56 85 C0 75 13 8B 75 08 85 F6 74 34 C7 05 ?? ?? ?? ?? 01 00 00 00 EB 12 83 F8 01 75 23", 0x821C60);
        g_booster_on = (volatile int32_t*)read_abs(g_scale_fn + 0x98);   // CMP [DAT_00ce82b8],0 after the index test
    }
    if (config_row("DEV-TURBO-VOICE", 1)) {
        g_vbusy_fn = sig_find_unique("DEV-TURBO voice-busy query FUN_0067a710", "8B 0D ?? ?? ?? ?? 6A 11 E8 ?? ?? ?? ?? C3", 0x67A710);
        g_wfs_fn = sig_find_unique("DEV-TURBO Sg_WaitFrameSync",
            "55 8B EC 83 EC 18 D9 05 ?? ?? ?? ?? D8 45 08 DC 0D ?? ?? ?? ?? D9 5D FC E8", 0x821E80);
        if (g_wfs_fn && sig_match(g_wfs_fn + 0x4B, "83 3D ?? ?? ?? ?? 00 0F 85") && sig_match(g_wfs_fn + 0x67, "83 3D ?? ?? ?? ?? 01 0F 85")
            && sig_match(g_wfs_fn + 0x74, "83 3D ?? ?? ?? ?? 01 C7 05") && g_wfs_fn[0x98] == 0xA3) {
            g_sync_n = (volatile int32_t*)read_abs(g_wfs_fn + 0x4D);
            g_sync_mode = (volatile int32_t*)read_abs(g_wfs_fn + 0x69);
            g_sync_first = (volatile int32_t*)read_abs(g_wfs_fn + 0x76);
            g_sync_base = (volatile uint64_t*)read_abs(g_wfs_fn + 0x99);
            log_line("  DEV-TURBO: voice-busy gva=0x%08X, Sg_WaitFrameSync gva=0x%08X, SyncData n/mode/first/base gva 0x%08X/0x%08X/0x%08X/0x%08X",
                     rt_to_gva_exe(g_vbusy_fn), rt_to_gva_exe(g_wfs_fn), rt_to_gva_exe((void*)g_sync_n), rt_to_gva_exe((void*)g_sync_mode),
                     rt_to_gva_exe((void*)g_sync_first), rt_to_gva_exe((void*)g_sync_base));
        } else {
            g_wfs_fn = nullptr;
            log_line("  DEV-TURBO: Sg_WaitFrameSync shape unexpected - SyncData shift off");
        }
    }
    if (g_sound_fn) log_line("  DEV-TURBO: booster sound switch gva=0x%08X, booster flag gva=0x%08X (expected 0x00CE82B8)", rt_to_gva_exe((void*)g_sound_fn), rt_to_gva_exe((void*)g_booster_on));
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
        if (g_vbusy_fn && !hook_entry("DEV-TURBO voice", g_vbusy_fn, (void*)&h_vbusy, (void**)&g_vbusy_tramp)) g_vbusy_fn = nullptr;
        if (g_wfs_fn && !hook_entry("DEV-TURBO syncdata", g_wfs_fn, (void*)&h_wfs, (void**)&g_wfs_tramp)) g_wfs_fn = nullptr;
        log_line("DEV-TURBO: hooked on the game thread (voice %d, SyncData %d)", g_vbusy_fn != nullptr, g_wfs_fn != nullptr);
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
        if (g_sound_fn && (down || *g_booster_on == 0)) {
            __try { g_sound_fn(down ? 1 : 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_sound_fn = nullptr; log_line("DEV-TURBO: booster sound switch faulted - off"); }
        }
    }
}

// Runtime off (CFG-MENU, dev hot reload): a turbo held at that moment is released (pre no longer runs to see the key
// go up); the hooks stay and pass through.
static void stop() {
    if (!g_held) return;
    g_held = false;
    log_line("DEV-TURBO: off (row stopped)");
    if (g_sound_fn && *g_booster_on == 0) {
        __try { g_sound_fn(0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_sound_fn = nullptr; }
    }
}

Row row_DEVTURBO = {
    "DEV-TURBO",
    "test tool: hold a key (DEV-TURBO-KEY) to run the game DEV-TURBO-SPEED times faster via its own turbo/backlog path",
    1, install, pre, nullptr, 0, stop
};
