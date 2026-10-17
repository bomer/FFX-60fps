// SPDX-License-Identifier: Apache-2.0
// DEV-DMG (test tool, default off): every hit the party lands on an enemy deals a fixed HP damage, so a boss phase can
// be reached quickly (asked 18/10/2026 to test QOL-RETRY on Yunalesca's second form).
// ini [rows]: DEV-DMG=9999 or DEV-DMG=99999 (any other non-zero value = 9999).
//
// FUN_0078f060 0x78F060 (cdecl(attacker, action, target, out, flags), plain RET 0x78F67E; its one caller 0x78D9F4
// does ADD ESP,0x14) applies the pending hits of `attacker` on `target` from the target's battle block
// (FUN_00794020(target)): two slots at +0x77B (stride 0x2D8) matched on slot[-5] == attacker and slot[-4] == action,
// pending record index slot[-7] < count slot[-6], record = slot - 7 + index * 0x2C + 0x18. Record +0x18 (u16) is the
// hit mask, bit 0 = HP; +0x20 (s32) is the HP damage, applied as `HP(+0xF60) -= damage` (c/00780000.c, 0x78F060).
// Enemies are battle chrs 0x14..0x1B (FUN_0079aef0). The detour sets positive HP damage (not heals) before the call.
#include "../fps60.h"

typedef uint32_t(__cdecl* apply_t)(uint32_t attacker, uint32_t action, uint32_t target, uint32_t* out, uint32_t flags);

static uint8_t* g_apply_fn = nullptr;
static apply_t  g_apply = nullptr;
static uint8_t** g_chr_base = nullptr;              // DAT_011334d4: battle chr blocks, 0xF90 each (chr < 0x1F)
static int      g_value = 9999;
static bool     g_patched = false;
static volatile LONG g_hits = 0;

static uint32_t __cdecl h_apply(uint32_t attacker, uint32_t action, uint32_t target, uint32_t* out, uint32_t flags) {
    uint32_t a = attacker & 0xFF, t = target & 0xFF;
    if (row_DEVDMG.on && !g_disabled && a < 0x14 && t >= 0x14 && t <= 0x1B && *g_chr_base) {
        __try {
            uint8_t* blk = *g_chr_base + t * 0xF90;
            for (int s = 0; s < 2; s++) {
                uint8_t* slot = blk + 0x77B + s * 0x2D8;
                if (slot[-5] != a || slot[-4] != (uint8_t)action || slot[-7] >= slot[-6]) continue;
                uint8_t* rec = slot - 7 + slot[-7] * 0x2C + 0x18;
                int32_t* dmg = (int32_t*)(rec + 0x20);
                if ((*(uint16_t*)(rec + 0x18) & 1) && *dmg > 0) {
                    if (InterlockedIncrement(&g_hits) <= 5)
                        log_line("DEV-DMG: chr 0x%02X -> enemy 0x%02X: %d damage set to %d", a, t, *dmg, g_value);
                    *dmg = g_value;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            log_line("DEV-DMG: fault reading the hit record - row off");
            row_DEVDMG.on = 0;
        }
    }
    return g_apply(attacker, action, target, out, flags);
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("DEV-DMG hit apply", g_apply_fn, (void*)&h_apply, (void**)&g_apply)) {
        log_line("DEV-DMG: hook failed - row off");
        row_DEVDMG.on = 0;
        return;
    }
    log_line("DEV-DMG: hit apply hooked on the game thread (%d per party hit on an enemy)", g_value);
}

static bool install() {
    g_value = config_row("DEV-DMG", 0) >= 99999 ? 99999 : 9999;
    // 0x78F060: PUSH EBP; MOV EBP,ESP; SUB ESP,0x44; PUSH EBX/ESI/EDI; PUSH [EBP+8]; XOR EAX,EAX; XOR ECX,ECX;
    // MOV [EBP-4],EAX; MOV [EBP-0x2C],ECX; CALL FUN_00794020 (one match in fps60_image.bin)
    g_apply_fn = sig_find_unique("DEV-DMG hit apply FUN_0078f060",
        "55 8B EC 83 EC 44 53 56 57 FF 75 08 33 C0 33 C9 89 45 FC 89 4D D4 E8", 0x78F060);
    if (!g_apply_fn) return false;
    // FUN_00794020 (chr block getter): MOV EAX,[ESP+4]; ... IMUL ...,0xF90; ADD EAX,[DAT_011334d4]
    uint8_t* get = call_target(g_apply_fn + 0x16);
    if (!get) return false;
    for (int i = 0; i < 0x20 && !g_chr_base; i++)
        if (get[i] == 0x03 && get[i + 1] == 0x05) g_chr_base = (uint8_t**)read_abs(get + i + 2);   // ADD EAX,[abs]
    if (!g_chr_base) { log_line("  DEV-DMG: chr block base not found in FUN_00794020 - refused"); return false; }
    log_line("  DEV-DMG: apply gva=0x%08X, chr blocks at gva 0x%08X, damage %d", rt_to_gva_exe(g_apply_fn),
             rt_to_gva_exe(g_chr_base), g_value);
    return true;
}

Row row_DEVDMG = {
    "DEV-DMG",
    "test tool: every party hit on an enemy deals DEV-DMG damage (9999 or 99999)",
    0, install, pre, nullptr, 0, nullptr
};
