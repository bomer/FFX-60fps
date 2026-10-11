// SPDX-License-Identifier: Apache-2.0
// UI-001: screen fades (incl. the lightning flash) step inside their draw call, which also runs on
// render-only invocations, so fades last half as long. Fix: on those invocations let the fade draw, then
// restore its state, so it steps once per logic tick.
// Details: docs/fps60/rows/UI-001.md
#include "../fps60.h"
#include <string.h>

typedef void(__cdecl* fade_t)(void);

static fade_t   g_fade = nullptr;
static uint8_t* g_state = nullptr;    // active mask + 5 slots (+ two read-only dwords)
static uint8_t* g_out = nullptr;      // flag, flag, latch, progress float
static uint8_t* g_out2 = nullptr;     // fade type output
static const uint32_t STATE_LEN = 0x58, OUT_LEN = 0x10, OUT2_LEN = 4;
static volatile LONG g_held = 0, g_passed = 0;
static int g_half = 0;                // UI-001-HALF: render-only frames draw the half step (60 Hz promotion)

// UI-001-HALF: rescale each active slot so the original call draws the midpoint between two logic ticks
// (the state is restored afterwards). Per-type formulas: docs/fps60/rows/UI-001.md.
static void half_step() {
    for (int n = 0; n < 5; n++) {
        uint8_t* slot = g_state + 8 + n * 0x10;
        int16_t* c = (int16_t*)(slot + 4);
        int16_t* t = (int16_t*)(slot + 6);
        int16_t* step = (int16_t*)(slot + 0xA);
        if (*c == 0) continue;
        switch (slot[0]) {
        case 0: *step = (int16_t)(*step / 2); break;
        case 2: case 3: case 5:
            if (*c > 0 && *t > 0 && *t <= 0x3FFF && *c <= 0x3FFF) { *c = (int16_t)(*c * 2); *t = (int16_t)(*t * 2); }
            break;
        case 4: case 6:
            if (*c > 0 && *t > 0 && *t <= 0x3FFF && *c < 0x3FFF) { *c = (int16_t)(*c * 2 + 1); *t = (int16_t)(*t * 2); }
            break;
        default: break;
        }
    }
}

static void __cdecl h_fade() {
    if (!row_UI001.on || g_disabled || pacing_kind() != INV_BUILD) {
        InterlockedIncrement(&g_passed);
        g_fade();
        return;
    }
    uint8_t s[STATE_LEN], o[OUT_LEN], o2[OUT2_LEN];
    memcpy(s, g_state, STATE_LEN);
    memcpy(o, g_out, OUT_LEN);
    memcpy(o2, g_out2, OUT2_LEN);
    if (g_half) half_step();
    g_fade();
    memcpy(g_state, s, STATE_LEN);
    memcpy(g_out, o, OUT_LEN);
    memcpy(g_out2, o2, OUT2_LEN);
    InterlockedIncrement(&g_held);
}

static bool install() {
    // Sg_MainLoop tail: the call to Sg_ScreenFadeUpdate (third CALL)
    uint8_t* at = sig_find_unique("UI-001 Sg_ScreenFadeUpdate call (Sg_MainLoop tail)",
        "6A 07 E8 ?? ?? ?? ?? 83 C4 08 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 3D", 0x8214E0);
    if (!at) return false;
    uint8_t* site = at + 0x0A;
    uint8_t* fn = call_target(site);
    // Sg_ScreenFadeUpdate prologue: two calls, then the active-mask test
    if (!sig_match(fn, "55 8B EC 83 EC 0C E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 3D")) {
        log_line("  UI-001: CALL target %p is not Sg_ScreenFadeUpdate (prologue mismatch) - refused", fn);
        return false;
    }
    uint32_t mask = *(uint32_t*)(fn + 0x12);
    uint32_t slots = 0;
    // MOV ESI,imm32 with imm == mask+8 (slot 0 countdown)
    for (uint32_t i = 0x20; i < 0x200; i++)
        if (fn[i] == 0xBE && *(uint32_t*)(fn + i + 1) == mask + 8) { slots = mask + 8; break; }
    // MOV [out2],EAX; MOV dword [out],1; FSTP [out+0xC]
    uint32_t out2 = 0, out = 0;
    for (uint32_t i = 0x100; i + 21 <= 0x600; i++) {
        const uint8_t* p = fn + i;
        if (p[0] == 0xA3 && p[5] == 0xC7 && p[6] == 0x05 && *(uint32_t*)(p + 11) == 1 && p[15] == 0xD9 && p[16] == 0x1D &&
            *(uint32_t*)(p + 17) == *(uint32_t*)(p + 7) + 0xC) {
            out2 = *(uint32_t*)(p + 1); out = *(uint32_t*)(p + 7); break;
        }
    }
    if (!slots || !out || !out2) {
        log_line("  UI-001: fade globals not found (mask=0x%08X slots=0x%08X out=0x%08X out2=0x%08X) - refused",
                 mask, slots, out, out2);
        return false;
    }
    g_state = (uint8_t*)(uintptr_t)mask;
    g_out = (uint8_t*)(uintptr_t)out;
    g_out2 = (uint8_t*)(uintptr_t)out2;
    g_fade = (fade_t)fn;
    g_half = config_row("UI-001-HALF", 0);
    if (!patch_call_rel32(site, fn, (void*)&h_fade)) {
        log_line("  UI-001: patch failed");
        return false;
    }
    log_line("  UI-001: CALL at gva=0x%08X -> stub; Sg_ScreenFadeUpdate gva=0x%08X, state gva=0x%08X+0x%X "
             "(expected 0x01300704), outputs gva=0x%08X (0x012FFAE8), 0x%08X (0x00C496D0): render-only invocations draw, state restored; UI-001-HALF=%d",
             rt_to_gva_exe(site), rt_to_gva_exe(fn), rt_to_gva_exe(g_state), STATE_LEN, rt_to_gva_exe(g_out), rt_to_gva_exe(g_out2), g_half);
    return true;
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_held > 0) {
        reported = 1;
        log_line("UI-001: first render-only fade update held (held=%ld passed=%ld)", g_held, g_passed);
    }
}

Row row_UI001 = {
    "UI-001",
    "screen fades (Sg_ScreenFadeUpdate, incl. lightning flash) step once per logic tick; render-only frames draw with state restored",
    1, install, nullptr, post, 0
};
