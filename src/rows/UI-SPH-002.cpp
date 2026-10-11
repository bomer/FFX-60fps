// SPDX-License-Identifier: Apache-2.0
// UI-SPH-002: the Sphere Grid's particle programs (node activation effects) step from its draw path,
// FUN_00a45930 -> Ppp_ProgramStep (call 0xA459B5), on every invocation: 2x at 60 Hz. MAG-006 pauses only DLL
// callers; this row adds this one EXE call site to its list (draw still runs, the program does not advance).
// Details: re/ledger/UI-SPHERE.md (UI-SPH-010)
#include "../fps60.h"

static bool install() {
    uint8_t* ppp = mag006_entry();
    if (!ppp) { log_line("  UI-SPH-002: needs MAG-006 (its Ppp_ProgramStep hook) - refused"); return false; }
    // FUN_00a45930: MOV EAX,[..]; PUSH 0x30; PUSH ESI; MOV [EAX],imm; CALL Ppp_ProgramStep; ADD ESP,0x30; TEST EAX,EAX; JZ
    uint8_t* at = sig_find_unique("UI-SPH-002 Sphere Grid Ppp_ProgramStep call (FUN_00a45930)",
        "A1 ?? ?? ?? ?? 6A 30 56 C7 00 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 30 85 C0 74", 0xA459A7);
    if (!at) return false;
    if (call_target(at + 14) != ppp) { log_line("  UI-SPH-002: CALL at %p is not Ppp_ProgramStep - refused", at + 14); return false; }
    if (!mag006_pause_exe_caller(at + 19)) { log_line("  UI-SPH-002: MAG-006 caller list full - refused"); return false; }
    log_line("  UI-SPH-002: Ppp_ProgramStep call gva=0x%08X (return 0x%08X) paused on render-only invocations via MAG-006",
             rt_to_gva_exe(at + 14), rt_to_gva_exe(at + 19));
    return true;
}

Row row_UISPH002 = {
    "UI-SPH-002",
    "Sphere Grid particle programs (draw path FUN_00a45930) paused on render-only invocations (via MAG-006)",
    1, install, nullptr, nullptr, 0
};
