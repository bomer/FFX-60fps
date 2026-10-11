// SPDX-License-Identifier: Apache-2.0
// MAG-026: family-X effect records (FUN_00925440 node walk) step inside their own draw, FUN_00930060: position
// integration (FUN_0092f9d0), the record's particle program (FUN_0092f010 -> FUN_0092eec0 -> Ppp_ProgramStep, an
// EXE caller MAG-006 does not pause) and sprite/model animation. MAG-020 already keeps the state handlers at logic
// rate; on render-only invocations it still calls each record's draw, so Vorban's Mortar (magic_0360) ran its
// hand glow and fireball programs and the fireball's flight at 2x. This row, on render-only invocations:
//   MAG-026-PPP: pauses that Ppp_ProgramStep call through MAG-006's EXE caller list;
//   MAG-026-REC: restores each record's fields (+0x0C..+0x280, +0x330..+0x490) after its draw (MAG-020 calls
//                mag026_draw_record); the particle program block +0x280..+0x330 is left to the pause.
// Details: docs/fps60/rows/MAG-026.md
#include "../fps60.h"
#include <string.h>

typedef void(__cdecl* node_fn_t)(uint8_t* node);

static bool g_rec = true;
static volatile LONG g_held = 0;

// Called by MAG-020's draw-only walk on render-only invocations. Head and tail sentinels (0xBC bytes) are drawn
// as they are; records are 0x490-byte pool blocks (FUN_0092ff20).
void mag026_draw_record(uint8_t* head, uint8_t* nd, void* cb) {
    node_fn_t fn = (node_fn_t)cb;
    if (!g_rec || !row_MAG026.on || nd == head || nd == head + 0xBC) { fn(nd); return; }
    static uint8_t save[0x490];
    memcpy(save, nd, 0x490);
    fn(nd);
    memcpy(nd + 0x0C, save + 0x0C, 0x280 - 0x0C);
    memcpy(nd + 0x330, save + 0x330, 0x490 - 0x330);
    InterlockedIncrement(&g_held);
}

static bool install() {
    if (!row_MAG020.on) { log_line("  MAG-026: needs MAG-020 (its draw-only node walk) - refused"); return false; }
    g_rec = config_row("MAG-026-REC", 1) != 0;
    if (config_row("MAG-026-PPP", 1)) {
        uint8_t* ppp = mag006_entry();
        if (!ppp) { log_line("  MAG-026: MAG-026-PPP needs MAG-006 (its Ppp_ProgramStep hook) - refused"); return false; }
        // FUN_0092f010: MOV EAX,[..]; PUSH 0x30; PUSH EDI; PUSH [EAX]; CALL FUN_0092eec0; ADD ESP,0x2C; TEST EAX,EAX; JNZ
        uint8_t* at = sig_find_unique("MAG-026 record program step call (FUN_0092f010)",
            "A1 ?? ?? ?? ?? 6A 30 57 FF 30 E8 ?? ?? ?? ?? 83 C4 2C 85 C0 0F 85", 0x92F1AB);
        if (!at) return false;
        // FUN_0092eec0: PUSH EBP; MOV EBP,ESP; PUSH [EBP+10]; MOV EAX,[..]; MOV ECX,[EBP+8]; PUSH [EBP+C]; MOV [EAX],ECX;
        // CALL Ppp_ProgramStep (+0x13); ADD ESP,8 (+0x18)
        uint8_t* step = call_target(at + 10);
        if (!step || !sig_match(step, "55 8B EC FF 75 10 A1 ?? ?? ?? ?? 8B 4D 08 FF 75 0C 89 08 E8 ?? ?? ?? ?? 83 C4 08 5D C3")) {
            log_line("  MAG-026: CALL at %p is not FUN_0092eec0 - refused", at + 10);
            return false;
        }
        if (call_target(step + 0x13) != ppp) { log_line("  MAG-026: FUN_0092eec0 does not call Ppp_ProgramStep - refused"); return false; }
        if (!mag006_pause_exe_caller(step + 0x18)) { log_line("  MAG-026: MAG-006 caller list full - refused"); return false; }
        log_line("  MAG-026: record program step call gva=0x%08X (return 0x%08X) paused on render-only invocations via MAG-006",
                 rt_to_gva_exe(step + 0x13), rt_to_gva_exe(step + 0x18));
    }
    log_line("  MAG-026: record fields restored after render-only draws: %s", g_rec ? "yes" : "no (MAG-026-REC=0)");
    return true;
}

static void post() {
    static uint32_t tick = 0;
    static LONG seen = 0;
    if (++tick % 600 == 0 && g_held != seen) {
        seen = g_held;
        log_line("MAG-026: family-X record draws held on render-only invocations so far: %ld", seen);
    }
}

Row row_MAG026 = {
    "MAG-026",
    "family-X effect records (FUN_00930060): render-only draws do not step programs, motion or animation (Mortar)",
    1, install, nullptr, post, 0
};
