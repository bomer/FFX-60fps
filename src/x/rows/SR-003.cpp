// SPDX-License-Identifier: Apache-2.0
// SR-003 (feature): soft reset. Holding L1+L2+R1+R2+Start+Select returns to the title screen through the game's
// own game-over path FUN_0086da10 (stops sound/events, then stages the jump to room 0x17; the load runs in the
// next exec frame). In battle (SR-008) the fight is ended as a defeat first, so the game tears the battle down itself.
// ini: [fps60] soft_reset=<buttons joined by +> or 0/off (config_row maps it to SR-003).
// Details: docs/fps60/rows/SR-003.md
#include "../fps60.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef void(__cdecl* reset_t)(void);
static reset_t g_reset = nullptr;
static bool    g_armed = true;

// Globals by VA, valid only because install() proved this build (FUN_0086da10 at its expected VA).
static uint8_t* gva(uint32_t va) { return exe_base() + (va - 0x400000); }
template <typename T> static T rd(uint32_t va) { return *(volatile T*)gva(va); }

static uint16_t g_chord = 0x090F;          // default L1+L2+R1+R2+Start+Select
static char     g_chord_text[96];

// PS2 pad bits in the held word (SR-007: L1/L2/R1/R2/Start/Select and the d-pad S; the rest by PS2 layout, H)
static const struct { const char* name; uint16_t bit; } k_buttons[] = {
    {"L2", 0x0001}, {"R2", 0x0002}, {"L1", 0x0004}, {"R1", 0x0008},
    {"TRIANGLE", 0x0010}, {"CIRCLE", 0x0020}, {"CROSS", 0x0040}, {"SQUARE", 0x0080},
    {"SELECT", 0x0100}, {"L3", 0x0200}, {"R3", 0x0400}, {"START", 0x0800},
    {"UP", 0x1000}, {"RIGHT", 0x2000}, {"DOWN", 0x4000}, {"LEFT", 0x8000},
};

// "L1+L2+R1+R2+START+SELECT" (any case, separators + , space) or a raw mask "0x090F"; 0 = invalid
static uint16_t parse_chord(const char* s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (uint16_t)strtoul(s, nullptr, 16);
    uint16_t mask = 0;
    char tok[16];
    while (*s) {
        while (*s == '+' || *s == ',' || *s == ' ' || *s == '\t') s++;
        int n = 0;
        while (*s && *s != '+' && *s != ',' && *s != ' ' && *s != '\t') { if (n < 15) tok[n++] = (char)toupper((unsigned char)*s); s++; }
        tok[n] = 0;
        if (!n) break;
        bool found = false;
        for (auto& b : k_buttons) if (!strcmp(tok, b.name)) { mask |= b.bit; found = true; }
        if (!found) { log_line("  SR-003: unknown button '%s' in soft_reset", tok); return 0; }
    }
    return mask;
}

static uint16_t pad_held() {
    uint8_t idx = rd<uint8_t>(0x01330288 + 0x9C);
    return rd<uint16_t>(0x01330288 + (idx & 3) * 0x20 + 4);
}

// SR-005: 0 = may reset, else the first blocking condition (logged). In battle (SR-008) the battle main state
// must be 1 (running; 3..0x16 / 0x22 intro, 0x17..0x21 already ending) with no result decided yet.
static const char* blocked() {
    int8_t bs = rd<int8_t>(0x0112A8E0);
    if (bs != 0 && bs != 1)                     return "battle starting or ending";
    if (bs == 1 && (rd<int8_t>(0x0112C9F1) != 0 || rd<int8_t>(0x0112A8E3) != 0)) return "battle result already decided";
    if (rd<int32_t>(0x01340824) != 0)           return "menu screen running";
    if (rd<int32_t>(0x01340828) != 0 || rd<int32_t>(0x01340850) != 0) return "paused";
    if (rd<uint32_t>(0x012FBC38) != 0xFFFFFFFF) return "screen request pending";
    if (rd<int32_t>(0x00CCB994) != 0)           return "HD system UI / save busy";
    if (rd<int32_t>(0x0133084C) != 0)           return "map change in flight";
    uint16_t room = rd<uint16_t>(0x0112CA90);
    if (bs == 0 && (room == 0x17 || room == 0x18F)) return "at the title / quit room";
    if (rd<int32_t>(0x00C40E28) != -1 || rd<int32_t>(0x0112A00C) != 0) return "movie playing";
    if (rd<int32_t>(0x01327108) != 0)           return "quit countdown";
    return nullptr;
}

// SR-008 (re/ledger/SOFTRESET.md): in battle, end the fight as a defeat through the game's own path. Clear the
// "loss allowed" bit 0x200 of DAT_01326b48 (read only at 0x86121A and FUN_0086c290) and set the battle result
// DAT_0112c9f1 = 1, the write the script opcode FUN_007a5330 makes; the party check FUN_007928d0 returns it. At the
// next idle point the battle runs 2 -> 0x17 .. 0x21, tears down (magic DLLs included), and FUN_00861160(result 1)
// calls the game-over return to title FUN_0086da10. If the state is still 1 after ~3 s (the command menu may keep
// the idle gate closed, H), the state goes to 2 as the opcode does (`if (a8e0 == 1) a8e0 = 2`).
static int  g_btl_pending = 0;                  // logic ticks since the battle reset was requested, 0 = none
static bool g_btl_kicked = false;

static void battle_reset_tick() {
    int8_t bs = rd<int8_t>(0x0112A8E0);
    if (bs == 0) { log_line("SR-008: battle torn down after %d ticks (title load follows)", g_btl_pending); g_btl_pending = 0; return; }
    *(volatile uint32_t*)gva(0x01326B48) &= ~0x200u;   // a script may set it again while the battle ends
    if (++g_btl_pending == 90 && bs == 1 && !g_btl_kicked) {
        g_btl_kicked = true;
        *(volatile int8_t*)gva(0x0112A8E0) = 2;
        log_line("SR-008: battle still running after 90 ticks - state 1 -> 2 (as opcode FUN_007a5330)");
    }
    if (g_btl_pending > 1800) { log_line("SR-008: battle did not end in 60 s (state 0x%02X) - given up", (uint8_t)bs); g_btl_pending = 0; }
}

static void post() {
    if (!g_reset || g_disabled || pacing_kind() == INV_BUILD) return;
    if (g_btl_pending) { battle_reset_tick(); return; }
    bool chord = (pad_held() & g_chord) == g_chord;
    if (!chord) { g_armed = true; return; }
    if (!g_armed) return;
    g_armed = false;                            // once per chord: release to re-arm
    if (const char* why = blocked()) {
        log_line("SR-003: soft reset chord ignored (%s)", why);
        return;
    }
    if (rd<int8_t>(0x0112A8E0) == 1) {
        *(volatile uint32_t*)gva(0x01326B48) &= ~0x200u;
        *(volatile int8_t*)gva(0x0112C9F1) = 1;
        g_btl_pending = 1;
        g_btl_kicked = false;
        log_line("SR-008: soft reset in battle -> defeat (loss-allowed bit cleared), game over -> title follows");
        return;
    }
    log_line("SR-003: soft reset -> title (room 0x%04X)", rd<uint16_t>(0x0112CA90));
    g_reset();
}

static bool install() {
    // FUN_0086da10: CALL; CALL; CALL; PUSH 1; PUSH -1; CALL; ADD ESP,8; CALL; TEST EAX,EAX; JS
    uint8_t* fn = sig_find_unique("SR-003 game-over return to title FUN_0086da10",
        "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 01 6A FF E8 ?? ?? ?? ?? 83 C4 08 E8 ?? ?? ?? ?? 85 C0 78 0E",
        0x86DA10);
    if (!fn) return false;
    if (rt_to_gva_exe(fn) != 0x86DA10) {
        log_line("  SR-003: FUN_0086da10 not at its expected VA (other build); the globals would be stale - refused");
        return false;
    }
    g_reset = (reset_t)fn;
    config_str("fps60", "soft_reset", g_chord_text, sizeof(g_chord_text), "L1+L2+R1+R2+START+SELECT");
    uint16_t m = parse_chord(g_chord_text);
    if (!m || (m & (m - 1)) == 0) {
        log_line("  SR-003: soft_reset '%s' is invalid or a single button; using L1+L2+R1+R2+START+SELECT", g_chord_text);
        m = 0x090F;
    }
    g_chord = m;
    log_line("  SR-003: reset gva=0x%08X; chord '%s' (mask 0x%04X), field and battle", rt_to_gva_exe(fn), g_chord_text, g_chord);
    return true;
}

Row row_SR003 = {
    "SR-003",
    "soft reset: hold L1+L2+R1+R2+Start+Select to return to the title (game-over path FUN_0086da10; in battle via a forced defeat)",
    1, install, nullptr, post, 0
};
