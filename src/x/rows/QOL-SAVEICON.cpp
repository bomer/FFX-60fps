// SPDX-License-Identifier: Apache-2.0
// QOL-SAVEICON (gameplay option, default off): no running-chocobo animation when the game autosaves. The save itself is
// untouched. Config page row "Autosave Chocobo" (ON = stock, OFF = this row on). Asked 18/10/2026.
//
// - The autosave is written at map load (FUN_0088e070: FUN_0088e800, FUN_008b50b0, FUN_006484d0), which also sets
//   DAT_023cbebc = 1 ("show the icon"). FUN_00869e40 later clears it and, when FUN_00647e10() == 0, tail-jumps
//   (0x869F69) to FUN_00643de0 = MOV ECX,[app]; JMP FUN_0066f480.
// - FUN_0066f480 0x66F480 (__fastcall(app), plain RET 0x66F4CA; its only reference is that JMP) frees the previous
//   movie at app+0x10DDC and creates /FFX_Data/GameData/PS3Data/flash/chocobo.swf at 1220,660 (50x50) through
//   FUN_006e9250. The detour returns without creating it while the row is on.
#include "../fps60.h"

typedef void(__fastcall* chocobo_t)(void* app);

static uint8_t*  g_fn = nullptr;
static chocobo_t g_orig = nullptr;
static bool      g_patched = false;
static volatile LONG g_skipped = 0;

static void __fastcall h_chocobo(void* app) {
    if (row_QOLSAVEICON.on && !g_disabled) {
        if (InterlockedIncrement(&g_skipped) <= 3) log_line("QOL-SAVEICON: autosave chocobo not shown");
        return;
    }
    g_orig(app);
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("QOL-SAVEICON autosave chocobo", g_fn, (void*)&h_chocobo, (void**)&g_orig)) {
        log_line("QOL-SAVEICON: hook failed - row off");
        row_QOLSAVEICON.on = 0;
        return;
    }
    log_line("QOL-SAVEICON: autosave chocobo hooked on the game thread");
}

static bool install() {
    // 0x66F480: PUSH ESI/EDI; MOV EDI,ECX; MOV ESI,[EDI+0x10DDC]; ... CMP [DAT_01984c6c],0; JZ; PUSH 0x32 x2;
    // PUSH 0x294; PUSH 0x4C4; PUSH path (one match in fps60_image.bin)
    g_fn = sig_find_unique("QOL-SAVEICON chocobo movie FUN_0066f480",
        "56 57 8B F9 8B B7 DC 0D 01 00 85 F6 74 10 8B CE E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 04 83 3D ?? ?? ?? ?? 00 "
        "74 21 6A 32 6A 32 68 94 02 00 00 68 C4 04 00 00 68", 0x66F480);
    return g_fn != nullptr;
}

Row row_QOLSAVEICON = {
    "QOL-SAVEICON",
    "gameplay option: no running-chocobo animation when the game autosaves",
    0, install, pre, nullptr, 0, nullptr
};
