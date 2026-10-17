// SPDX-License-Identifier: Apache-2.0
// The FFX-2 mod's own text (X2-CFG-MENU rows) in the game's current text language.
//
// - The text language is [settings + 4], settings = [0xDD0210]: FUN_004a02a0 returns that pointer (creating the object
//   when it is still null) and FUN_00439630 switches the data folder on +4: 0 jp, 1 us, 2 fr, 3 sp, 4 de, 5 it, 9 kr,
//   10 ch (S; the glyph-table loader FUN_00511120 switches on the same 0 / 9 / 10). The pointer is read here, never
//   created: while it is null the text is English.
// - The strings are x2/text/strings.json, encoded through FFX-2's glyph tables (FFX2_Data.vbf ffx2_encoding) by
//   tools/gen_text_x2.py (outer repo G:\Work\ffx2-fps60) into x2_text_strings.h, round-trip checked.
#include "x2.h"
#define X2_TEXT_DATA
#include "x2_text_strings.h"

namespace x2 {

static uint8_t* volatile* g_settings = nullptr;   // [0xDD0210]
static bool g_tried = false;

static bool text_init() {
    if (g_tried) return g_settings != nullptr;
    g_tried = true;
    // The getter's shape is shared by 8 lazy getters, so it is found through its call in FUN_00439630 (0x439643:
    // MOV [EBP-0x108],ECX; CALL FUN_004a02a0; MOV EAX,[EAX+4]; CMP EAX,0xA; JA; JMP [EAX*4+table]).
    uint8_t* site = sig_find("X2 text language switch FUN_00439630",
        "89 8D F8 FE FF FF E8 ?? ?? ?? ?? 8B 40 04 83 F8 0A 77 ?? FF 24 85", 0x439643);
    // FUN_004a02a0: MOV EAX,[settings]; TEST EAX,EAX; JNZ ret; PUSH; PUSH 1; CALL; MOV EAX,[settings]; ADD ESP,8; RET
    uint8_t* f = site ? call_target(site + 6) : nullptr;
    if (f && sig_match(f, "A1 ?? ?? ?? ?? 85 C0 75 14 68 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 C4 08 C3")) {
        uint8_t* a = abs_operand("settings pointer", f + 1);
        if (a && a == abs_operand("settings pointer", f + 0x16)) g_settings = (uint8_t* volatile*)a;
    }
    if (!g_settings) log_line("  text: settings pointer not found - mod text stays English");
    else log_line("  text: settings pointer gva=0x%08X (0x00DD0210), language at +4", rt_to_gva((const void*)g_settings));
    return g_settings != nullptr;
}

static int text_lang() {
    if (!g_settings) return 1;
    __try {
        uint8_t* s = *g_settings;
        return s ? *(int32_t*)(s + 4) : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}

const uint8_t* x2_text(int id) {
    text_init();
    if (id < 0 || id >= T_COUNT) return (const uint8_t*)"";
    int col;                                         // strings.json "langs": en fr sp de it jp kr ch
    switch (text_lang()) {
    case 0:  col = 5; break;
    case 2:  col = 1; break;
    case 3:  col = 2; break;
    case 4:  col = 3; break;
    case 5:  col = 4; break;
    case 9:  col = 6; break;
    case 10: col = 7; break;
    default: col = 0; break;
    }
    static_assert(TEXT_LANGS == 8, "x2_text_strings.h language order changed: update x2_text");
    return k_text[col][id];
}

} // namespace x2
