// SPDX-License-Identifier: Apache-2.0
// QOL-AP (gameplay option, default off): every party member earns AP after a battle, not only those who acted.
// Stock (re/ledger/QOL.md, QOL-AP): each kill credits base AP to all 18 battle slots through FUN_00798a00 0x798A00
// (cdecl, plain RET) into ap_acc[18]; Double/Triple AP apply only to slots on the field (+0xDC8) and not KO (+0xDCC).
// At battle end the gate FUN_00798b70 0x798B70 (void, plain RET; one caller, 0x790EA4 in MsBattleProcess) resets
// ap_acc[i] to the battle-start snapshot ap_snap[i] unless acted[i] && !KO && !petrified, then counts a battle for
// each appeared[i]. The results window lists and credits only appeared[i] slots 0..7 (0x8BDE11, 0x8BDE40).
// This row retargets that one CALL to a wrapper. For slots 0..7 in the party (PlySave +0x2C bit 0, FUN_00785380) and
// not escaped (+0xDCD):
//   - a member who did not act is marked acted for the gate, then gets snapshot + gain * QOL-AP-RATE / 100;
//   - QOL-AP-KO=1: what the gate took from a KO'd or petrified member is put back;
//   - appeared[i] is set after the gate, so the window lists and credits the member (battle count not raised).
// Sub-switch QOL-AP-DT (default 1): the Double/Triple test skips the on-field check (JZ at 0x798A2F -> 0x798A47), so
// a member's own Double/Triple AP applies off the field too (no Gillionaire there).
#include "../fps60.h"

typedef void(__stdcall* gate_t)();
typedef uint8_t*(__cdecl* chr_t)(int idx);
typedef int(__cdecl* in_party_t)(int idx);

static uint8_t*   g_site = nullptr;           // CALL gate in MsBattleProcess
static uint8_t*   g_dt_jz = nullptr;
static gate_t     g_gate = nullptr;
static chr_t      g_chr = nullptr;
static in_party_t g_in_party = nullptr;
static uint8_t*   g_acted = nullptr;          // uint8_t[18]
static int32_t*   g_snap = nullptr;           // int32_t[18]
static int32_t*   g_acc = nullptr;            // int32_t[18]
static uint8_t*   g_appeared = nullptr;       // uint8_t[18]
static int        g_ko = 1, g_rate = 100, g_dt = 1;
static bool       g_patched = false;

static void __stdcall h_gate() {
    if (g_disabled || !row_QOLAP.on) { g_gate(); return; }   // off at runtime (CFG-MENU): the retarget stays
    bool qual[8] = {}, added[8] = {};
    int32_t before[8] = {};
    for (int i = 0; i < 8; i++) {
        uint8_t* c = g_chr(i);
        if (!c || !(g_in_party(i) & 1) || c[0xDCD]) continue;
        qual[i] = true;
        before[i] = g_acc[i];
        if (!g_acted[i]) { g_acted[i] = 1; added[i] = true; }
    }
    g_gate();
    for (int i = 0; i < 8; i++) {
        if (!qual[i]) continue;
        uint8_t* c = g_chr(i);
        bool restored = false;
        if (g_ko && g_acc[i] != before[i]) { g_acc[i] = before[i]; restored = true; }
        int32_t got = g_acc[i] - g_snap[i];
        if (added[i]) {
            g_acted[i] = 0;
            if (g_rate != 100 && got > 0) g_acc[i] = g_snap[i] + (int32_t)((int64_t)got * g_rate / 100);
        }
        bool listed = g_appeared[i] != 0;
        g_appeared[i] = 1;
        log_line("QOL-AP: slot %d %s%s%s ap +%d -> +%d (on field %d, KO %d, auto-ab 0x%04X)", i,
                 added[i] ? "did not act" : "acted", restored ? ", KO/petrify AP kept" : "", listed ? "" : ", newly listed",
                 got, g_acc[i] - g_snap[i], c[0xDC8], c[0xDCC], *(uint16_t*)(c + 0x6BE));
    }
}

static bool install() {
    g_ko = config_row("QOL-AP-KO", 1);
    g_rate = config_row("QOL-AP-RATE", 100);
    if (g_rate < 0) g_rate = 0;
    if (g_rate > 100) g_rate = 100;
    g_dt = config_row("QOL-AP-DT", 1);
    // FUN_00798b70: PUSH ESI; XOR ESI,ESI; PUSH ESI; CALL chr; ADD ESP,4; CMP byte [ESI+acted],0; JZ;
    // CMP byte [EAX+0xDCC],0; JNZ; CMP byte [EAX+0xDCE],0; JZ; MOV EAX,[ESI*4+snap]; MOV [ESI*4+acc],EAX;
    // CMP byte [ESI+appeared],0
    uint8_t* gate = sig_find_unique("QOL-AP battle-end gate",
        "56 33 F6 56 E8 ?? ?? ?? ?? 83 C4 04 80 BE ?? ?? ?? ?? 00 74 12 80 B8 CC 0D 00 00 00 75 09 80 B8 CE 0D 00 00 00 74 0E "
        "8B 04 B5 ?? ?? ?? ?? 89 04 B5 ?? ?? ?? ?? 80 BE", 0x798B70);
    // MsBattleProcess: CALL gate; CMP byte [..],0; JNZ +5; CALL; CALL; MOVSX EAX,byte [..]
    g_site = sig_find_unique("QOL-AP gate call",
        "E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 75 05 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F BE 05", 0x790EA4);
    // FUN_00785380 (cdecl, idx): PlySave[idx] +0x2C & 1
    g_in_party = (in_party_t)sig_find_unique("QOL-AP in-party flag",
        "55 8B EC 0F B6 45 08 69 C0 94 00 00 00 0F B6 80 ?? ?? ?? ?? 83 E0 01 5D C3", 0x785380);
    if (!gate || !g_site || !g_in_party) return false;
    if (call_target(g_site) != gate) {
        log_line("  QOL-AP: the call at gva=0x%08X does not target the gate - refused", rt_to_gva_exe(g_site));
        return false;
    }
    g_gate = (gate_t)gate;
    g_chr = (chr_t)call_target(gate + 4);
    g_acted = read_abs(gate + 0x0E);
    g_snap = (int32_t*)read_abs(gate + 0x2A);
    g_acc = (int32_t*)read_abs(gate + 0x31);
    g_appeared = read_abs(gate + 0x37);
    if (!g_chr || !g_acted || !g_snap || !g_acc || !g_appeared) return false;
    if (g_dt) {
        // FUN_00798a00: CMP byte [ECX+0xDC8],0; PUSH ESI; PUSH EDI; JZ +0x2C; CMP byte [ECX+0xDCC],0; JNZ; MOV EDI,2
        uint8_t* dt = sig_find_unique("QOL-AP-DT on-field test",
            "80 B9 C8 0D 00 00 00 56 57 74 2C 80 B9 CC 0D 00 00 00 75 23 BF 02 00 00 00", 0x798A26);
        if (dt) g_dt_jz = dt + 9;
        else log_line("  QOL-AP-DT: not found - Double/Triple stay on-field only");
    }
    log_line("  QOL-AP: gate gva=0x%08X call gva=0x%08X; acted 0x%08X snap 0x%08X acc 0x%08X appeared 0x%08X; KO %d, rate %d%%, DT %s",
             rt_to_gva_exe(gate), rt_to_gva_exe(g_site), rt_to_gva_exe(g_acted), rt_to_gva_exe(g_snap),
             rt_to_gva_exe(g_acc), rt_to_gva_exe(g_appeared), g_ko, g_rate, g_dt_jz ? "on" : "off");
    return true;
}

static bool g_dt_on = false;

static void pre() {
    if (!g_patched) {
        g_patched = true;
        if (!patch_call_rel32(g_site, (void*)g_gate, (void*)&h_gate)) {
            log_line("QOL-AP: call retarget failed - row off");
            row_QOLAP.on = 0;
            return;
        }
        log_line("QOL-AP: battle-end AP gate wrapped");
    }
    if (g_dt_jz && !g_dt_on) {
        g_dt_on = true;
        static const uint8_t jz[2] = { 0x74, 0x16 };   // -> 0x798A47 MOV EDI,[EBP+0x14] (Double/Triple tests)
        if (!sig_match(g_dt_jz, "74 2C") || !patch_write(g_dt_jz, jz, 2)) { log_line("QOL-AP-DT: patch failed"); g_dt_jz = nullptr; }
    }
}

// Runtime off (CFG-MENU, dev hot reload): h_gate passes through while the row is off; the Double/Triple JZ goes
// back to stock (the Config page is field-only, so no kill is being credited meanwhile).
static void stop() {
    if (!g_dt_on) return;
    g_dt_on = false;
    static const uint8_t jz[2] = { 0x74, 0x2C };
    if (!g_dt_jz || !sig_match(g_dt_jz, "74 16") || !patch_write(g_dt_jz, jz, 2)) log_line("QOL-AP-DT: could not restore the JZ");
}

int  qol_ap_rate() { return g_rate; }
void qol_ap_set_rate(int pct) { g_rate = pct < 0 ? 0 : pct > 100 ? 100 : pct; }

Row row_QOLAP = {
    "QOL-AP",
    "gameplay option: every party member earns AP after a battle (QOL-AP-KO, QOL-AP-RATE, QOL-AP-DT)",
    0, install, pre, nullptr, 0, stop
};
