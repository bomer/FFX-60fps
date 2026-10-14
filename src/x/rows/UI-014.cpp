// SPDX-License-Identifier: Apache-2.0
// UI-014: the minimap draw Ui_MinimapDraw 0x91DA80 runs on every invocation and adds 1 to the player-arrow pulse
// counter obj+0x74 per call (0x91EC86), read as a 30-call triangle wave for the arrow scale (0x91E605): at 60 Hz the
// arrow pulses 2x. On render-only invocations the draw still runs and the counter is put back afterwards (it is read
// before the increment, so the phase holds). Details: re/ledger/UI.md (UI-014)
#include "../fps60.h"

typedef void(__cdecl* draw_t)(uint8_t* obj, int mode);

static uint8_t* g_entry = nullptr;
static draw_t   g_tramp = nullptr;
static bool     g_patched = false;
static volatile LONG g_held = 0;

// cdecl(obj, mode), aligned-stack prologue, plain RET (0x91DD7D, 0x91ECA0); callers clean (ADD ESP,8 at
// 0x919D1C, 0x91DA1D). obj is [EBX+8] -> [EBP-0x318] -> ESI at the INC.
static void __cdecl h_draw(uint8_t* obj, int mode) {
    uint32_t saved = 0;
    bool hold = !g_disabled && pacing_kind() == INV_BUILD && obj && safe_read_u32(obj + 0x74, &saved);
    g_tramp(obj, mode);
    if (hold) {
        *(uint32_t*)(obj + 0x74) = saved;
        if (InterlockedIncrement(&g_held) == 1) log_line("UI-014: first minimap arrow pulse held on a render-only invocation");
    }
}

static bool install() {
    // Ui_MinimapDraw: PUSH EBX; MOV EBX,ESP; SUB ESP,8; AND ESP,-16; ADD ESP,4; PUSH EBP; MOV EBP,[EBX+4];
    // MOV [ESP+4],EBP; MOV EBP,ESP; SUB ESP,0x368; MOV EAX,[cookie]; XOR EAX,EBP
    g_entry = sig_find_unique("UI-014 Ui_MinimapDraw",
        "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 68 03 00 00 A1 ?? ?? ?? ?? 33 C5",
        0x91DA80);
    if (!g_entry) return false;
    // The pulse increment this row compensates: INC dword [ESI+0x74] at +0x1206 (0x91EC86)
    if (!sig_match(g_entry + 0x1206, "FF 46 74")) {
        log_line("  UI-014: no INC [ESI+0x74] at +0x1206 - refused");
        return false;
    }
    log_line("  UI-014: Ui_MinimapDraw gva=0x%08X (entry hooked on the game thread), pulse INC at gva=0x%08X",
             rt_to_gva_exe(g_entry), rt_to_gva_exe(g_entry + 0x1206));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("UI-014", g_entry, (void*)&h_draw, (void**)&g_tramp)) {
        log_line("UI-014: entry hook failed - row off");
        row_UI014.on = 0;
        return;
    }
    log_line("UI-014: Ui_MinimapDraw entry hooked on the game thread (tramp %p)", (void*)g_tramp);
}

Row row_UI014 = {
    "UI-014",
    "minimap player-arrow pulse (Ui_MinimapDraw obj+0x74) steps once per logic tick; render-only frames draw it held",
    1, install, pre, nullptr, 0
};
