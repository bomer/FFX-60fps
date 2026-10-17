// SPDX-License-Identifier: Apache-2.0
// Mod text in the game's own charset, for the current text language (CFG-MENU rows, QOL-RETRY prompt).
//
// - The text language is FUN_006410e0 (no args, 19 bytes: CALL; TEST; JNZ; RET; CALL; MOV EAX,[EAX+4]; RET), the same
//   getter the Config page setup uses. Its glyph table (FUN_00646920): 0 = ffxsjistbl_jp, 9 = _kr, 10 = _ch, any
//   other = _us (the Latin languages share it).
// - Glyph codes (decoder at 0x8B7xxx): a byte >= 0x30 is table index byte - 0x30; a lead 0x2C..0x2F + byte is
//   (lead - 0x2B) * 0xD0 + byte - 0x30. Leads 0x26..0x2B are other font sets, 0x00..0x23 control codes.
// - The index maps below come from the tables in FFX_Data.vbf (ffx_ps2/ffx/master/jppc/ffx_encoding/, 16/10/2026):
//   ASCII 0x20..0x7E -> table index, -1 = no glyph. jp/kr/ch use the full-width forms; kr and ch agree on every
//   character the mod writes, so ch uses the kr map.
#include "fps60.h"

static const int16_t k_us[95] = {
    10,11,-1,13,14,15,16,-1,18,19,20,21,22,23,24,25,0,1,2,3,4,5,6,7,8,9,26,27,28,29,30,31,-1,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,-1,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,
    80,81,82,83,84,85,86,87,88,89,90,91,92,93 };
static const int16_t k_jp[95] = {
    10,11,-1,-1,-1,15,16,-1,18,19,20,21,22,23,24,25,0,1,2,3,4,5,6,7,8,9,26,27,-1,29,-1,31,-1,208,209,210,211,212,213,214,
    215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,-1,-1,-1,-1,-1,-1,234,235,236,237,238,239,
    240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,-1,-1,-1,13 };
static const int16_t k_kr[95] = {
    10,11,-1,-1,-1,15,16,37,18,19,20,21,22,23,24,25,0,1,2,3,4,5,6,7,8,9,26,27,-1,29,-1,31,-1,42,43,44,45,46,47,48,49,50,
    51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,-1,-1,-1,-1,-1,-1,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,
    84,85,86,87,88,89,90,91,92,93,-1,-1,-1,13 };

typedef int (__stdcall* lang_t)();
static lang_t g_lang = nullptr;
static bool   g_tried = false;

bool ffx_text_init() {
    if (g_tried) return g_lang != nullptr;
    g_tried = true;
    g_lang = (lang_t)sig_find_unique("FFX text language FUN_006410e0",
        "E8 ?? ?? ?? ?? 85 C0 75 01 C3 E8 ?? ?? ?? ?? 8B 40 04 C3", 0x6410E0);
    if (g_lang && call_target((uint8_t*)g_lang) != call_target((uint8_t*)g_lang + 10)) {
        log_line("  text: FUN_006410e0 calls two different functions - language fixed to the Latin table");
        g_lang = nullptr;
    }
    return g_lang != nullptr;
}

int ffx_text_lang() {
    if (!g_lang) return 1;
    __try { return g_lang(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}

int ffx_text_encode(const char* s, uint8_t* out, int n, int lang) {
    const int16_t* map = lang == 0 ? k_jp : (lang == 9 || lang == 10) ? k_kr : k_us;
    int i = 0;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        int idx = (c >= 0x20 && c < 0x7F) ? map[c - 0x20] : -1;
        if (idx < 0) return -1;
        if (idx < 0xD0) {
            if (i + 1 >= n) return -1;
            out[i++] = (uint8_t)(0x30 + idx);
        } else {
            int lead = 0x2B + idx / 0xD0;
            if (lead > 0x2F || i + 2 >= n) return -1;
            out[i++] = (uint8_t)lead;
            out[i++] = (uint8_t)(0x30 + idx % 0xD0);
        }
    }
    out[i] = 0;
    return i;
}
