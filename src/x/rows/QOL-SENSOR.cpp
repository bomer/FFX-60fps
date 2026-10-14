// SPDX-License-Identifier: Apache-2.0
// QOL-SENSOR (gameplay option, default off): every party member has the Sensor auto-ability effect (enemy HP and
// info when targeting) without armor. The only gameplay reader of the Sensor bit (battle chr +0x6BC bit 0) is
// FUN_0078f9e0 0x78F9E0, "is any party member a Sensor holder": it walks party slots 0..0x11 and needs bit 0, chr+0xDCC
// == 0 and FUN_007b24a0(chr,1,1,0) (on the field, able). The bit test's JZ at 0x78FA00 becomes two NOPs, so every
// active party member counts; enemies, the holder checks and the target's Sensor immunity (FUN_0078f990) stay.
// The battle-script property 0x3A (FUN_007b2dc0) still reads the real bit. Details: re/ledger/QOL.md (QOL-SNS)
#include "../fps60.h"

static uint8_t* g_jz = nullptr;
static bool     g_patched = false;

static bool install() {
    // FUN_0078f9e0 loop head: PUSH ESI; XOR ESI,ESI; PUSH ESI; CALL chr(idx); ADD ESP,4;
    // TEST byte [EAX+0x6BC],1; JZ +0x1C; CMP byte [EAX+0xDCC],0
    uint8_t* at = sig_find_unique("QOL-SENSOR holder loop",
        "56 33 F6 56 E8 ?? ?? ?? ?? 83 C4 04 F6 80 BC 06 00 00 01 74 1C 80 B8 CC 0D 00 00 00", 0x78F9ED);
    if (!at) return false;
    g_jz = at + 0x13;
    log_line("  QOL-SENSOR: Sensor bit JZ at gva=0x%08X (patched on the game thread)", rt_to_gva_exe(g_jz));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    static const uint8_t nops[2] = { 0x90, 0x90 };
    if (!sig_match(g_jz, "74 1C") || !patch_write(g_jz, nops, 2)) {
        log_line("QOL-SENSOR: patch failed - row off");
        row_QOLSENSOR.on = 0;
        return;
    }
    log_line("QOL-SENSOR: every active party member counts as a Sensor holder");
}

Row row_QOLSENSOR = {
    "QOL-SENSOR",
    "gameplay option: every party member has the Sensor effect (enemy HP/info on targeting) without armor",
    0, install, pre, nullptr, 0
};
