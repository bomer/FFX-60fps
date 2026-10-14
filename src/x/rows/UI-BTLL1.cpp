// SPDX-License-Identifier: Apache-2.0
// UI-BTLL1: in battle, the party swap (L1, PC action bit 0x8000000) in the top command window FUN_0089c900 asks the
// PC action latch for a fresh press: FUN_00630cf0(mask) = (cur & mask) && !(prev & mask) on the latch at
// DAT_00CCB170 (+0 current, +4 previous), which advances once per app frame. The handler runs only on logic
// invocations, so a press first seen before a render-only invocation reads as "held" on the next logic one and is
// lost (user: 2-3 presses needed). New presses are collected at every invocation and handed to this call site
// until a logic invocation has run; the latch itself is not touched (its other readers stay per frame).
// Details: re/ledger/UI-BTLL1.md (BL1-001..007, fix BL1-FIX)
#include "../fps60.h"

typedef int(__cdecl* edge_t)(uint32_t mask);

static volatile uint32_t* g_latch = nullptr;     // DAT_00CCB170: [0] current, [1] previous
static edge_t   g_edge = nullptr;                // FUN_00630cf0
static uint32_t g_sticky = 0;                    // presses seen since the last logic invocation (game thread)
static volatile LONG g_saved = 0;

// cdecl(mask) -> 0/1 in EAX, plain RET (0x630D06); the caller cleans (ADD ESP,4 at 0x89C9FF).
static int __cdecl h_edge(uint32_t mask) {
    int live = g_edge(mask);
    if (!live && !g_disabled && (g_sticky & mask)) {
        if (InterlockedIncrement(&g_saved) <= 20)
            log_line("UI-BTLL1: press of action 0x%08X kept from a render-only invocation (saved %ld)", mask, g_saved);
        g_sticky &= ~mask;
        return 1;
    }
    g_sticky &= ~mask;                           // consumed (or not pressed): never fire twice
    return live;
}

static bool install() {
    // FUN_0089c900: PUSH 0x8000000; CALL FUN_00630cf0; ADD ESP,4; TEST EAX,EAX; JZ +0x3F; MOVSX EAX,word [ESI+8]
    uint8_t* at = sig_find_unique("UI-BTLL1 battle party-swap press test (FUN_0089c900)",
        "68 00 00 00 08 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 3F 0F BF 46 08", 0x89C9F5);
    if (!at) return false;
    uint8_t* site = at + 5;
    uint8_t* fn = call_target(site);
    // FUN_00630cf0: PUSH EBP; MOV EBP,ESP; PUSH [EBP+8]; CALL latch getter; MOV ECX,EAX; CALL edge test
    if (!sig_match(fn, "55 8B EC FF 75 08 E8 ?? ?? ?? ?? 8B C8 E8")) {
        log_line("  UI-BTLL1: CALL target %p is not the action edge accessor - refused", fn);
        return false;
    }
    // Latch getter FUN_00631300: MOV EAX,imm32; RET
    uint8_t* getter = call_target(fn + 6);
    if (!sig_match(getter, "B8 ?? ?? ?? ?? C3")) {
        log_line("  UI-BTLL1: latch getter %p has an unexpected shape - refused", getter);
        return false;
    }
    g_latch = (volatile uint32_t*)read_abs(getter + 1);
    g_edge = (edge_t)fn;
    if (!patch_call_rel32(site, fn, (void*)&h_edge)) { log_line("  UI-BTLL1: patch failed"); return false; }
    log_line("  UI-BTLL1: CALL at gva=0x%08X -> stub (edge accessor gva=0x%08X, action latch gva=0x%08X)",
             rt_to_gva_exe(site), rt_to_gva_exe(fn), rt_to_gva_exe((const void*)g_latch));
    return true;
}

// Every invocation: collect presses of this poll (the latch advanced just before it).
static void pre() {
    __try { g_sticky |= g_latch[0] & ~g_latch[1]; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// After a logic invocation the handler has had its chance: drop what was not consumed.
static void post() {
    int k = pacing_kind();
    if (k != INV_BUILD) g_sticky = 0;
}

Row row_UIBTLL1 = {
    "UI-BTLL1",
    "battle party swap (L1, PC action 0x8000000): presses seen on render-only invocations reach the logic tick",
    1, install, pre, post, 0
};
