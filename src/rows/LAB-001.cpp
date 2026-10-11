// SPDX-License-Identifier: Apache-2.0
// LAB-001 (diagnostic, default off): combat lab proof of concept. In battle, Ctrl+LAB-001-KEY queues one cast of
// LAB-001-CMD; at the next party turn the game's own submit FUN_00792d60(cue, 0, 0) gets the command instead of
// the battle menu opening (CALL FUN_0089bb10 in the input dispatcher FUN_00792a90, the call the AI path sits
// next to). While armed: the game's debug flags invincible_mon/ply and no_mp_cost are set, and monster CTB
// counters (chr +0x65C) are held high so the party always acts next. Ctrl+LAB-001-OFFKEY disarms and restores the flags.
// Keys default to numpad 1 / numpad 3 (F5 is the game's hide-HUD key).
// Logs: cast id/name/caster/targets/anim DLLs, magic_*.dll load/unload with logic ticks since the cast.
// Names: optional <game>\fps60_lab.txt, lines "<hex id> <name> <state>" (tools/lab/lab_list.py).
// List mode (LAB-001-CMD=0): the key toggles a loop that casts the next "run" command of fps60_lab.txt at each party
// turn. fps60_lab_progress.txt (append only) gets "S <id> <name>" at each submit and "D <id>" when the next party turn
// comes; arming resumes after the last S, and an S without its D (crash or quit mid-cast) is logged.
// Details: re/ledger/LAB.md
#include "../fps60.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef int (__cdecl* submit_t)(uint8_t* cue, uint32_t p2, int type);   // FUN_00792d60, plain RET
typedef void(__cdecl* menu_t)(uint32_t chr);                            // FUN_0089bb10, plain RET

static submit_t g_submit = nullptr;
static menu_t   g_menu = nullptr;
static uint8_t* g_btl = nullptr;                 // Btl 0x0112A8D0: +0x10 state, +0x28 debug flags, +0x5C command table
static uint8_t** g_chr_tab = nullptr;            // DAT_011334D4: battle chr blocks, stride 0xF90, ids 0..0x1E
static int      g_cmd = 0x3000;

static volatile LONG g_queued = 0;               // casts waiting for a party turn
static bool     g_armed = false;
static uint8_t  g_saved_flags[3];                // invincible_mon, invincible_ply, no_mp_cost before arming
static bool     g_key_down = false;
static int      g_key = VK_NUMPAD1, g_offkey = VK_NUMPAD3;
static volatile LONG g_pending = 0;               // 1 = submitted, its magic DLL not loaded yet
static uint32_t g_last_tg = 0;
static volatile LONG g_tick = 0;                 // logic invocations since arming
static volatile LONG g_cast_tick = -1;           // g_tick at the last cast, -1 = none
static int      g_last_cmd = 0;

static int      g_mot_n = 0;                     // LAB-001-MOTCLK: invocations to log after each cast (0 = off)
static int      g_mot_left = 0;
static uint8_t* g_mot_actor = nullptr;           // caster's actor (battle chr +0, Fahrenheit Chr.actor)
bool lab001_mot_window() { return g_mot_left > 0; }   // other rows' per-cast diagnostics follow this window
static char g_names[0x400][32];                  // index id - 0x3000
static uint16_t g_list[0x400];                   // list mode: "run" ids in file order
static int      g_list_n = 0, g_list_pos = 0;
static bool     g_list_mode = false;
static bool     g_done_due = false;              // list mode: write "D" at the next party turn

static const char* cmd_name(int id) {
    int i = id - 0x3000;
    return (i >= 0 && i < 0x400 && g_names[i][0]) ? g_names[i] : "?";
}

static FILE* progress_open(const wchar_t* mode) {
    wchar_t p[MAX_PATH];
    game_dir(p, MAX_PATH);
    wcscat_s(p, L"fps60_lab_progress.txt");
    FILE* f = nullptr;
    return (_wfopen_s(&f, p, mode) || !f) ? nullptr : f;
}

static void progress(const char* fmt, ...) {
    FILE* f = progress_open(L"a");
    if (!f) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fclose(f);
}

// Arming in list mode: continue after the last started command.
static void resume() {
    int last = -1;
    bool done = true;
    if (FILE* f = progress_open(L"r")) {
        char line[96];
        while (fgets(line, sizeof(line), f)) {
            int id = (int)strtol(line + 1, nullptr, 16);
            if (line[0] == 'S') { last = id; done = false; }
            else if (line[0] == 'D' && id == last) done = true;
        }
        fclose(f);
    }
    g_list_pos = 0;
    if (last >= 0) {
        for (int i = 0; i < g_list_n; i++) if (g_list[i] == last) { g_list_pos = i + 1; break; }
        if (!done) log_line("LAB-001: the last run stopped during 0x%04X %s (no D line: crash or quit mid-cast)", last, cmd_name(last));
    }
    log_line("LAB-001: list mode, %d commands, resuming at #%d", g_list_n, g_list_pos);
}

static uint8_t* chr(uint32_t id) { return (id < 0x1F && *g_chr_tab) ? *g_chr_tab + id * 0xF90 : nullptr; }

static int target_hp(uint32_t mask) {
    for (uint32_t id = 0; id < 0x1F; id++)
        if (mask & (1u << id)) { uint8_t* c = chr(id); return c ? *(int*)(c + 0x5D0) : -1; }
    return -1;
}

// FUN_007ab870 on the command table (Btl +0x5C): u16 count, then 12-byte ranges {lo, hi, stride, strings, u32 data}.
static uint8_t* cmd_record(int id) {
    uint8_t* t = *(uint8_t**)(g_btl + 0x5C);
    if (!t) return nullptr;
    int n = *(int16_t*)t, k = id & 0xFFF;
    for (int i = 0; i < n; i++) {
        uint8_t* r = t + 8 + i * 12;
        uint16_t lo = *(uint16_t*)r, hi = *(uint16_t*)(r + 2), stride = *(uint16_t*)(r + 4);
        if (lo <= k && k <= hi) return t + *(uint32_t*)(r + 8) + stride * (k - lo);
    }
    return nullptr;
}

// Target mask from the record's flags_target (+0x1A, Fahrenheit command.cs): bit1 enemies, bit2 multiple, bit3 self.
static uint32_t pick_targets(uint32_t caster, uint8_t ft) {
    if (ft & 0x08) return 1u << caster;
    uint32_t lo = (ft & 0x02) ? 0x14 : 0, hi = (ft & 0x02) ? 0x1B : 7, m = 0;
    for (uint32_t id = lo; id <= hi; id++) {
        uint8_t* c = chr(id);
        if (!c || !c[0xDC8] || c[0xDCC]) continue;   // in battle, not removed (FUN_00790fb0 gates)
        m |= 1u << id;
        if (!(ft & 0x04)) break;
    }
    return m ? m : (1u << caster);
}

// Replaces CALL FUN_0089bb10(chr) at 0x792CF3: the party member's turn would open the battle menu.
static void __cdecl h_menu(uint32_t id) {
    if (g_done_due && id < 0x12) { progress("D %04X\n", g_last_cmd); g_done_due = false; }
    if (!g_disabled && g_armed && g_queued > 0 && id < 0x12) {
        __try {
            if (g_list_mode) {
                uint8_t* r = nullptr;
                while (g_list_pos < g_list_n && !(r = cmd_record(g_list[g_list_pos]))) {
                    log_line("LAB-001: no record for 0x%04X - skipped", g_list[g_list_pos]);
                    g_list_pos++;
                }
                if (!r) { log_line("LAB-001: list done"); g_queued = 0; g_menu(id); return; }
                g_cmd = g_list[g_list_pos++];
            }
            uint8_t* rec = cmd_record(g_cmd);
            uint8_t* c = chr(id);
            if (rec && c) {
                uint8_t cue[0x48] = {};
                uint32_t tg = pick_targets(id, rec[0x1A]);
                cue[0] = (uint8_t)id;
                cue[3] = 1;                                     // command count
                *(uint16_t*)(cue + 0x08) = (uint16_t)g_cmd;
                *(uint16_t*)(cue + 0x0A) = 0xFF;
                *(uint32_t*)(cue + 0x10) = tg;
                *(uint32_t*)(cue + 0x18) = 0x00FF00FF;          // unused entries as FUN_0089afc0 leaves them
                *(uint32_t*)(cue + 0x28) = 0x00FF00FF;
                *(uint32_t*)(cue + 0x38) = 0x00FF00FF;
                if (!g_list_mode) InterlockedDecrement(&g_queued);
                else { progress("S %04X %s\n", g_cmd, cmd_name(g_cmd)); g_done_due = true; }
                g_cast_tick = g_tick;
                g_last_cmd = g_cmd;
                g_last_tg = tg;
                g_pending = 1;
                log_line("LAB-001: cast%s 0x%04X %s by chr %u -> targets 0x%08X (flags_target 0x%02X, owner 0x%02X, "
                         "anim 0x%04X/0x%04X, caster hp %d mp %d, first target hp %d) tick %ld", g_list_mode ? " (list)" : "",
                         g_cmd, cmd_name(g_cmd), id, tg, rec[0x1A], rec[0x19],
                         *(uint16_t*)(rec + 0x10), *(uint16_t*)(rec + 0x12), *(int*)(c + 0x5D0), *(int*)(c + 0x5D4),
                         target_hp(tg), g_tick);
                if (g_mot_n) { g_mot_actor = *(uint8_t**)c; g_mot_left = g_mot_n; }
                int r = g_submit(cue, 0, 0);
                log_line("LAB-001: submit returned %d", r);
                return;
            }
            log_line("LAB-001: no record for 0x%04X or chr %u - menu opens", g_cmd, id);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("LAB-001: fault 0x%08lX building the cue - menu opens", GetExceptionCode()); }
    }
    if (g_armed && g_cast_tick >= 0 && id < 0x12) {
        uint8_t* c = chr(id);
        log_line("LAB-001: party turn chr %u at tick %ld (+%ld since cast 0x%04X), hp %d", id, g_tick,
                 g_tick - g_cast_tick, g_last_cmd, c ? *(int*)(c + 0x5D0) : -1);
    }
    g_menu(id);
}

static void on_load(uint8_t* base, uint32_t, const char* name) {
    g_pending = 0;
    if (g_armed && g_cast_tick >= 0)
        log_line("LAB-001: %s load at %p, +%ld ticks since cast 0x%04X %s", name, base, g_tick - g_cast_tick,
                 g_last_cmd, cmd_name(g_last_cmd));
}
static void on_unload(uint8_t* base) {
    if (g_armed && g_cast_tick >= 0)
        log_line("LAB-001: magic module %p unload, +%ld ticks since cast 0x%04X, first target hp %d", base,
                 g_tick - g_cast_tick, g_last_cmd, target_hp(g_last_tg));
}

static void load_names() {
    wchar_t p[MAX_PATH];
    game_dir(p, MAX_PATH);
    wcscat_s(p, L"fps60_lab.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, p, L"r") || !f) { log_line("  LAB-001: no fps60_lab.txt, names show as ?"); return; }
    char line[128];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char* e = nullptr;
        long id = strtol(line, &e, 16);
        int i = (int)id - 0x3000;
        if (e == line || i < 0 || i >= 0x400) continue;
        while (*e == ' ' || *e == '\t') e++;
        size_t k = strcspn(e, " \t\r\n"), c = k > 31 ? 31 : k;
        memcpy(g_names[i], e, c);
        g_names[i][c] = 0;
        e += k;
        while (*e == ' ' || *e == '\t') e++;
        if (!strncmp(e, "run", 3) && g_list_n < 0x400) g_list[g_list_n++] = (uint16_t)id;
        n++;
    }
    fclose(f);
    log_line("  LAB-001: %d command names from fps60_lab.txt, %d marked run", n, g_list_n);
}

static bool install() {
    // FUN_00792a90 AI branch: CMP byte [mon_control],0; JNZ; build cue on the stack; PUSH type; PUSH 0; PUSH cue; CALL submit
    uint8_t* at = sig_find_unique("LAB-001 input dispatcher AI submit (FUN_00792a90)",
        "80 3D ?? ?? ?? ?? 00 0F 85 85 00 00 00 88 5D B4 C6 45 B7 00 C7 45 C4 00 00 00 00 0F B6 47 01 50 8D 45 B4 6A 00 50 E8",
        0x792C60);
    if (!at) return false;
    uint8_t* site = at + 0x93;                   // PUSH EBX; CALL FUN_0089bb10; ADD ESP,4; CMP dword [..],0
    if (!sig_match(site - 1, "53 E8 ?? ?? ?? ?? 83 C4 04 83 3D")) { log_line("  LAB-001: menu CALL not found - refused"); return false; }
    uint8_t* submit = call_target(at + 0x26);
    if (!sig_match(submit, "55 8B EC 53 56 8B 75 08 57 C6 05 ?? ?? ?? ?? 00 85 F6 74 ?? 0F B6 3E 57 E8")) {
        log_line("  LAB-001: submit %p has an unexpected shape - refused", submit);
        return false;
    }
    uint8_t* getchr = call_target(submit + 0x18);   // FUN_00794020: id < 0x1F -> id * 0xF90 + [DAT_011334D4]
    if (!sig_match(getchr, "55 8B EC 8B 45 08 25 FF 00 00 00 83 F8 1F 7D 0E 69 C0 90 0F 00 00 03 05")) {
        log_line("  LAB-001: chr getter %p has an unexpected shape - refused", getchr);
        return false;
    }
    g_chr_tab = (uint8_t**)read_abs(getchr + 0x18);
    g_btl = read_abs(at + 2) - 0x2A;             // mon_control = Btl +0x2A (debug flags +0x28)
    g_submit = (submit_t)submit;
    g_menu = (menu_t)call_target(site);
    g_cmd = config_row("LAB-001-CMD", 0x3000);
    g_list_mode = g_cmd == 0;
    g_mot_n = config_row("LAB-001-MOTCLK", 0);
    g_key = config_row("LAB-001-KEY", VK_NUMPAD1);
    g_offkey = config_row("LAB-001-OFFKEY", VK_NUMPAD3);
    load_names();
    if (!magic_mods_subscribe("LAB-001", on_load, on_unload)) return false;
    if (!patch_call_rel32(site, (uint8_t*)g_menu, (void*)&h_menu)) { log_line("  LAB-001: patch failed"); return false; }
    log_line("  LAB-001: menu CALL gva=0x%08X -> stub, submit gva=0x%08X, Btl gva=0x%08X, chr table gva=0x%08X, cmd 0x%04X %s "
             "(Ctrl+VK 0x%02X = cast once, Ctrl+VK 0x%02X = disarm)", rt_to_gva_exe(site), rt_to_gva_exe(submit),
             rt_to_gva_exe(g_btl), rt_to_gva_exe(g_chr_tab), g_cmd, cmd_name(g_cmd), g_key, g_offkey);
    return true;
}

static void set_flags(bool on) {
    uint8_t* f = g_btl + 0x28;
    if (on) { f[0] = 1; f[1] = 1; f[9] = 1; }
    else { f[0] = g_saved_flags[0]; f[1] = g_saved_flags[1]; f[9] = g_saved_flags[2]; }
}

static void pre() {
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool on = ctrl && (GetAsyncKeyState(g_key) & 0x8000), off = ctrl && (GetAsyncKeyState(g_offkey) & 0x8000);
    bool down = on || off;
    if (down && !g_key_down) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            uint8_t* f = g_btl + 0x28;
            if (off) {
                if (g_armed) { set_flags(false); g_armed = false; g_queued = 0; log_line("LAB-001: disarmed, flags restored"); }
            } else {
                if (!g_armed) {
                    g_saved_flags[0] = f[0]; g_saved_flags[1] = f[1]; g_saved_flags[2] = f[9];
                    g_armed = true;
                    g_tick = 0;
                    g_cast_tick = -1;
                    log_line("LAB-001: armed (debug flags were %u/%u/%u)", f[0], f[1], f[9]);
                    if (g_list_mode) resume();
                }
                if (g_list_mode) {
                    g_queued = g_queued ? 0 : 1;
                    log_line("LAB-001: list loop %s at #%d of %d", g_queued ? "on" : "paused", g_list_pos, g_list_n);
                    g_key_down = down;
                    return;
                }
                InterlockedIncrement(&g_queued);
                log_line("LAB-001: cast 0x%04X %s queued (%ld waiting), battle state %u, input queue %u (head chr %u, menu %u)",
                         g_cmd, cmd_name(g_cmd), g_queued, g_btl[0x10], g_btl[0x1510], g_btl[0x1B0], g_btl[0x1B3]);
            }
        }
    }
    g_key_down = down;
}

// Per logic tick while armed and in a fight: debug flags on, monster CTB counters held high.
static void post() {
    // LAB-001-MOTCLK: the caster actor's keyframe fields after every invocation (SMOOTH-003 field map: +0x714 seq,
    // +0x718 start, +0x71C end, +0x724 loops, +0x740 clock, +0x744 sample clock, +0x7DC done). Kind L = logic,
    // B = render-only, K = backlog, S = stock. With SMOOTH on, B rows include its half step (restored at the next pre).
    if (g_mot_left > 0 && g_mot_actor) {
        g_mot_left--;
        __try {
            const uint8_t* a = g_mot_actor;
            log_line("LAB-001 mot: %c tick %ld seq %08X start %d end %d loops %d clock %d sample %d done %u",
                     "LBKS"[pacing_kind() & 3], g_tick, *(uint32_t*)(a + 0x714), *(int32_t*)(a + 0x718), *(int32_t*)(a + 0x71C),
                     *(int32_t*)(a + 0x724), *(int32_t*)(a + 0x740), *(int32_t*)(a + 0x744), a[0x7DC]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_mot_left = 0; }
    }
    if (!g_armed || pacing_kind() == INV_BUILD) return;
    g_tick++;
    if (g_btl[0x10] != 1) return;
    __try {
        // Submitted but no DLL yet: every 60 ticks, the attack-queue head and the executor gates (FUN_00792210 entry:
        // DAT_0112c9f5, c9d4, ca0b, ca0d, c944 = Btl +0x2125/+0x2104/+0x213B/+0x213D/+0x2074).
        if (g_pending && g_cast_tick >= 0 && (g_tick - g_cast_tick) % 60 == 30) {
            const uint8_t* q = g_btl + 0x3A0;
            log_line("LAB-001: waiting +%ld ticks: attack queue %u, head chr %u type %u state %u cmd 0x%04X; input queue %u; "
                     "gates %u %u %u %u msg %u", g_tick - g_cast_tick, g_btl[0x1511], q[0], q[1], q[4], *(uint16_t*)(q + 8),
                     g_btl[0x1510], g_btl[0x2125], g_btl[0x2104], g_btl[0x213B], g_btl[0x213D], g_btl[0x2074]);
        }
        set_flags(true);
        for (uint32_t id = 0x14; id <= 0x1B; id++) {
            uint8_t* c = chr(id);
            if (c && c[0xDC8] && c[0x65C] < 0xF0) c[0x65C] = 0xF0;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

Row row_LAB001 = {
    "LAB-001",
    "combat lab (diagnostic): Ctrl+Numpad1 casts LAB-001-CMD at the next party turn via the game's submit; immortal, party first",
    0, install, pre, post, 0
};
