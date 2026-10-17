// SPDX-License-Identifier: Apache-2.0
// The mod's own text (CFG-MENU rows, QOL-RETRY prompt) in the game's current text language.
//
// - The text language is FUN_006410e0 (no args, 19 bytes: CALL; TEST; JNZ; RET; CALL; MOV EAX,[EAX+4]; RET), the same
//   getter the Config page setup and the data-folder switch FUN_0088ccf0 use: 0 jp, 2 fr, 3 sp, 4 de, 5 it, 9 kr,
//   10 ch, anything else us (English). It returns 0 before the settings object exists, so it is read when text is
//   drawn, never at install.
// - The strings are text/strings.json, encoded through each language's glyph table by tools/gen_text.py (outer repo)
//   into text_strings.h, round-trip checked against the tables in FFX_Data.vbf.
#define FFX_TEXT_DATA
#include "fps60.h"

typedef int (__stdcall* lang_t)();
static lang_t g_lang = nullptr;
static bool   g_tried = false;

bool ffx_text_init() {
    if (g_tried) return g_lang != nullptr;
    g_tried = true;
    g_lang = (lang_t)sig_find_unique("FFX text language FUN_006410e0",
        "E8 ?? ?? ?? ?? 85 C0 75 01 C3 E8 ?? ?? ?? ?? 8B 40 04 C3", 0x6410E0);
    if (g_lang && call_target((uint8_t*)g_lang) != call_target((uint8_t*)g_lang + 10)) {
        log_line("  text: FUN_006410e0 calls two different functions - text stays English");
        g_lang = nullptr;
    }
    return g_lang != nullptr;
}

int ffx_text_lang() {
    if (!g_lang) return 1;
    __try { return g_lang(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}

const uint8_t* ffx_text(int id) {
    if (id < 0 || id >= T_COUNT) return (const uint8_t*)"";
    int col;                                         // strings.json "langs": en fr sp de it jp kr ch
    switch (ffx_text_lang()) {
    case 0:  col = 5; break;
    case 2:  col = 1; break;
    case 3:  col = 2; break;
    case 4:  col = 3; break;
    case 5:  col = 4; break;
    case 9:  col = 6; break;
    case 10: col = 7; break;
    default: col = 0; break;
    }
    static_assert(TEXT_LANGS == 8, "text_strings.h language order changed: update ffx_text");
    return k_text[col][id];
}
