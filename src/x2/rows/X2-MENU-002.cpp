// SPDX-License-Identifier: Apache-2.0
// X2-MENU-002: the field main menu at 60 fps without running 2x. X2-MENU-001 holds stock pacing (30 fps) in every
// PS2-layer menu; for the field main menu (mode 0 / 0x4000000) this row keeps the 60 Hz host and halves the menu
// logic instead: Menu_CoroutineIterate 0x7645E0 alternates logic and render-only passes.
//   render-only pass: the step gate CALL 0x51C100 (0x764824) returns 0x7FFFFFFF so the step block is skipped; the
//     always-run task step CALL Menu_TasksStep(1) (0x764894) is skipped; the cursor counter INC [0x1CCE9B8]
//     (0x76473A, before the gate) is undone at the gate; the fade ramp [0x16BDCE8] (0x7649BB, after the draws that read
//     it) is put back after the pass.
// Logic / render-only = the pacing core's alternation (render_only()), so X2-UI-017's pad latch lands on logic passes.
//   logic pass: everything runs; the slides are 16-byte tween records stepped by Tween_Step 0x772EB0 (value +0xE,
//     phase +0xA), read by the draws through Tween_GetValue 0x773010 / Tween_GetPhase 0x772FF0. Each step is recorded
//     (before/after); in the draw part of the logic pass the getters return the midpoint, so the drawn sequence is
//     k-0.5 (logic), k (render-only), k+0.5 ... A record whose value changed after the step (retarget) is not blended.
// U 14/10/2026: "main menu animation seems 30fps, can we make it 60fps without speeding it up" - windows sliding first.
// Evidence: outer repo re/ledger/MENU-SMOOTH.md, re/ledger/MENU.md §2/§5 (S).
#include "../x2.h"

namespace x2 {

typedef int(__cdecl* gate_t)();
typedef void(__cdecl* tasks_t)(uint32_t phase);
typedef int(__cdecl* tween_step_t)(uint8_t* t);

static gate_t       g_gate = nullptr;            // 0x51C100 turbo factor
static tasks_t      g_tasks = nullptr;           // 0x764170 Menu_TasksStep
static tween_step_t g_tween_step = nullptr;      // trampoline of 0x772EB0
static volatile int32_t*  g_counter = nullptr;   // 0x01CCE9B8 cursor / blink counter
static volatile int32_t*  g_fade = nullptr;      // 0x016BDCE8 fade ramp value
static volatile int32_t*  g_up = nullptr;        // 0x016BC55C menu in progress
static volatile int32_t*  g_up2 = nullptr;       // 0x016BC560
static volatile int32_t*  g_req = nullptr;       // 0x00DF6870 request, -1 = none
static volatile uint32_t* g_mode = nullptr;      // 0x016BC5EC mode

static bool  g_active = false;     // this invocation: main menu claimed
static bool  g_pass_render = false, g_blend = false, g_fade_saved_ok = false;
static int32_t g_fade_saved = 0;
static uint32_t g_serial = 1;
static LONG  g_passes = 0, g_render_passes = 0, g_blended = 0, g_rec_full = 0;

struct TweenRec { uint8_t* p; uint32_t serial; int16_t v0, v1, ph0, ph1; };
static TweenRec g_rec[256];
static int g_nrec = 0;
extern Row row_MENU002;

static bool main_menu_mode(uint32_t m) { m &= 0xFFFF0000u; return m == 0 || m == 0x04000000u; }

// Called by X2-MENU-001 every invocation: true = this row runs the menu at 60 Hz (no stock hold).
bool menu002_claims() {
    if (!row_MENU002.on || g_disabled || !g_cfg.pacing || !g_up) return false;
    int32_t req = *g_req;
    if (*g_up == 0 && *g_up2 == 0 && req == -1) return false;   // no menu up or requested
    return req != -1 ? main_menu_mode((uint32_t)req) : main_menu_mode(*g_mode);
}

static TweenRec* find(const uint8_t* p) {
    for (int i = 0; i < g_nrec; i++) if (g_rec[i].p == p) return g_rec[i].serial == g_serial ? &g_rec[i] : nullptr;
    return nullptr;
}

static int __cdecl h_gate() {
    int n = g_gate();
    if (!g_active) return n;
    g_serial++;
    g_nrec = 0;
    InterlockedIncrement(&g_passes);
    // the pacing core's alternation: X2-UI-017 latches the pad on the same logic invocations, so no press is lost
    if (!render_only()) return n;
    g_pass_render = true;
    InterlockedIncrement(&g_render_passes);
    (*g_counter)--;                                   // undo this pass's INC (0x76473A ran before the gate)
    g_fade_saved = *g_fade;                           // restored after the pass (post)
    g_fade_saved_ok = true;
    return 0x7FFFFFFF;                                // CMP [0xDF6414],EAX; JL -> step block skipped
}

// X2-MENU-002-DRAWREPLAY: a task's step can arm a one-shot draw through its draw-phase byte +0x3E, e.g. the party
// panels' step sets 5 (0x773A2E) and the draw FUN_00773b60 paints the 14 panel shadows only at 5, then sets 7. With
// the step skipped, render-only passes lost the shadows (U 14/10/2026: "the shadow in the menu blink", 2 consecutive
// frames). The logic pass records +0x3E of every task after the step; a render-only pass puts it back before the
// draws, so both passes draw what the stock game draws every frame.
static uint8_t*  g_task0 = nullptr;              // 0x016BC5F0, stride 0x9C, 16 slots (Menu_TasksStep loop)
struct TaskSnap { uint32_t step, draw; uint8_t ph; };
static TaskSnap g_snap[16];
static bool g_snap_ok = false;
static LONG g_replays = 0;
static int  g_drawreplay = 1;
static const int k_task_stride = 0x9C, k_tasks = 16;

static void __cdecl h_tasks(uint32_t phase) {
    if (g_pass_render) {                              // the always-run task step: logic passes only
        if (!g_snap_ok || !g_drawreplay) return;
        for (int i = 0; i < k_tasks; i++) {
            uint8_t* t = g_task0 + i * k_task_stride;
            if (*(uint32_t*)(t + 0xC) != g_snap[i].step || *(uint32_t*)(t + 0x10) != g_snap[i].draw) continue;
            if (t[0x3E] != g_snap[i].ph) { t[0x3E] = g_snap[i].ph; InterlockedIncrement(&g_replays); }
        }
        return;
    }
    g_tasks(phase);
    if (!g_active) return;
    g_blend = true;                                   // from here on: the draw part of a logic pass
    for (int i = 0; i < k_tasks; i++) {
        uint8_t* t = g_task0 + i * k_task_stride;
        g_snap[i].step = *(uint32_t*)(t + 0xC);
        g_snap[i].draw = *(uint32_t*)(t + 0x10);
        g_snap[i].ph = t[0x3E];
    }
    g_snap_ok = true;
}

static int __cdecl h_tween_step(uint8_t* t) {
    if (!g_active || g_pass_render) return g_tween_step(t);
    int16_t v0 = *(int16_t*)(t + 0xE), ph0 = *(int16_t*)(t + 0xA);
    int r = g_tween_step(t);
    TweenRec* e = nullptr;
    for (int i = 0; i < g_nrec; i++) if (g_rec[i].p == t) { e = &g_rec[i]; break; }
    if (!e) {
        if (g_nrec >= (int)(sizeof(g_rec) / sizeof(g_rec[0]))) { InterlockedIncrement(&g_rec_full); return r; }
        e = &g_rec[g_nrec++];
        e->p = t;
        e->v0 = v0; e->ph0 = ph0;                     // first step this pass keeps the pre-step values
    }
    e->serial = g_serial;
    e->v1 = *(int16_t*)(t + 0xE);
    e->ph1 = *(int16_t*)(t + 0xA);
    return r;
}

static int __cdecl h_get_value(uint8_t* t) {
    int v = *(int16_t*)(t + 0xE);
    if (!g_blend) return v;
    TweenRec* e = find(t);
    if (!e || e->v1 != v || e->v0 == e->v1) return v;
    InterlockedIncrement(&g_blended);
    return ((int)e->v0 + (int)e->v1) / 2;
}

static int __cdecl h_get_phase(uint8_t* t) {
    int v = *(int16_t*)(t + 0xA);
    if (!g_blend) return v;
    TweenRec* e = find(t);
    if (!e || e->ph1 != v || e->ph0 == e->ph1) return v;
    return ((int)e->ph0 + (int)e->ph1) / 2;
}

static void pre() {
    bool a = menu002_claims();
    if (a != g_active) {
        log_line("X2-MENU-002: main menu %s (mode 0x%X, request %d)", a ? "at 60 Hz, logic every 2nd pass, slides blended" : "released",
                 *g_mode, *g_req);
        g_nrec = 0;
        g_snap_ok = false;
    }
    g_active = a;
    g_pass_render = false;
    g_blend = false;
    g_fade_saved_ok = false;
}

static void post() {
    if (g_pass_render && g_fade_saved_ok) *g_fade = g_fade_saved;   // the ramp ran once more this pass: undo it
    g_pass_render = false;
    g_blend = false;
    static LONG last = 0;
    if (g_passes - last >= 1800) {
        last = g_passes;
        log_line("X2-MENU-002: passes=%ld render-only=%ld blended reads=%ld rec_full=%ld draw replays=%ld", g_passes, g_render_passes,
                 g_blended, g_rec_full, g_replays);
    }
}

static bool install() {
    // Sg_MainLoop menu exit (as X2-MENU-001): the in-progress flags
    uint8_t* ex = sig_find("X2-MENU-002 Sg_MainLoop menu exit",
        "E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 01 75 2D",
        0x605507);
    uint8_t* rq = sig_find("X2-MENU-002 Sg_MainLoop menu test call",
        "53 E8 ?? ?? ?? ?? 8B 1D ?? ?? ?? ?? 8B 3D ?? ?? ?? ?? 85 C0 74 48", 0x60549F);
    uint8_t* co = sig_find("X2-MENU-002 Menu_CoroutineIterate",
        "55 8B EC 53 56 8B 75 08 8B DE 81 E3 00 00 FF FF 83 3D ?? ?? ?? ?? 00 57 89 35", 0x7645E0);
    // INC [parity]; INC [counter]; CMP byte [..],0
    uint8_t* cn = sig_find("X2-MENU-002 cursor counter INC",
        "FE 05 ?? ?? ?? ?? FF 05 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00", 0x764734);
    // CALL turbo factor; CMP [0xDF6414],EAX; JL +0x61; CALL (pad/menu input)
    uint8_t* gt = sig_find("X2-MENU-002 step gate",
        "E8 ?? ?? ?? ?? 39 05 ?? ?? ?? ?? 7C 61 E8 ?? ?? ?? ?? E8", 0x764824);
    // CALL 0x780770; CALL Win_StateStep; PUSH 1; CALL Menu_TasksStep; ADD ESP,4; CMP EBX,0x10000
    uint8_t* tl = sig_find("X2-MENU-002 step tail",
        "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 81 FB 00 00 01 00", 0x764888);
    // MOV EAX,[fade]; ADD ESP,0x14; MOV [..],ESI; ADD EAX,[fade step]; MOV [fade],EAX
    uint8_t* fd = sig_find("X2-MENU-002 fade ramp",
        "A1 ?? ?? ?? ?? 83 C4 14 89 35 ?? ?? ?? ?? 03 05 ?? ?? ?? ?? A3", 0x7649BB);
    uint8_t* ts = sig_find("X2-MENU-002 Tween_Step",
        "55 8B EC 57 8B 7D 08 8A 07 84 C0 74 0C FE C8 88 07 B8 01 00 00 00 5F 5D C3 8A 4F 01 53 0F B7 5F 08", 0x772EB0);
    uint8_t* gp = sig_find("X2-MENU-002 Tween_GetPhase", "55 8B EC 8B 45 08 0F BF 40 0A 5D C3", 0x772FF0);
    if (!ex || !rq || !co || !cn || !gt || !tl || !fd || !ts || !gp) return false;
    uint8_t* gv = gp + 0x20;                          // Tween_GetValue 0x773010 (0x773000 = the +4 getter)
    if (!sig_match(gp + 0x10, "55 8B EC 8B 45 08 0F BF 40 04 5D C3") || !sig_match(gv, "55 8B EC 8B 45 08 0F BF 40 0E 5D C3")) {
        log_line("  X2-MENU-002: Tween_GetValue not at Tween_GetPhase+0x20 - refused");
        return false;
    }
    uint8_t* test = call_target(rq + 1);
    g_up = (volatile int32_t*)abs_operand("menu in progress", ex + 7);
    g_up2 = (volatile int32_t*)abs_operand("menu in progress 2", ex + 0x14);
    g_req = test && sig_match(test, "8B 0D") ? (volatile int32_t*)abs_operand("menu request", test + 2) : nullptr;
    g_mode = (volatile uint32_t*)abs_operand("menu mode", co + 0x1A);
    g_counter = (volatile int32_t*)abs_operand("cursor counter", cn + 8);
    g_fade = (volatile int32_t*)abs_operand("fade value", fd + 1);
    if (!g_up || !g_up2 || !g_req || !g_mode || !g_counter || !g_fade ||
        *(const uint32_t*)(fd + 0x15) != (uint32_t)(uintptr_t)g_fade) {
        log_line("  X2-MENU-002: menu globals not as proven - refused");
        return false;
    }
    // all sites inside Menu_CoroutineIterate, in order
    if (!(co < cn && cn < gt && gt < tl && tl < fd && fd < co + 0x500)) {
        log_line("  X2-MENU-002: coroutine sites out of order - refused");
        return false;
    }
    uint8_t* gate = call_target(gt);
    uint8_t* tasks = call_target(tl + 0x0C);
    if (!gate || !tasks || gt + 0x0D + 0x61 != tl + 0x0A) {   // JL rel8 0x61 (gt+0xB) must land on PUSH 1 (tl+0xA)
        log_line("  X2-MENU-002: gate JL does not land on the task step - refused");
        return false;
    }
    // Menu_TasksStep: MOV ESI,&task[0].step (+0xC); ... ADD ESI,0x9C; CMP ESI,end; JL
    if (!sig_match(tasks, "55 8B EC 56 57 8B 7D 08 BE ?? ?? ?? ?? 8D 49 00 0F B6 46 33 3B C7") ||
        !sig_match(tasks + 0x63, "81 C6 9C 00 00 00 81 FE ?? ?? ?? ?? 7C 9F")) {
        log_line("  X2-MENU-002: Menu_TasksStep not as proven - refused");
        return false;
    }
    uint8_t* first = abs_operand("task table", tasks + 9);
    uint32_t end = *(const uint32_t*)(tasks + 0x6B);
    if (!first || end - (uint32_t)(uintptr_t)first != (uint32_t)(k_tasks * k_task_stride)) {
        log_line("  X2-MENU-002: task table is not 16 x 0x9C - refused");
        return false;
    }
    g_task0 = first - 0xC;
    g_drawreplay = config_row("X2-MENU-002-DRAWREPLAY", 1);
    g_gate = (gate_t)gate;
    g_tasks = (tasks_t)tasks;
    if (!hook_entry("X2-MENU-002 Tween_Step", ts, (void*)&h_tween_step, (void**)&g_tween_step)) return false;
    void* dummy = nullptr;
    if (!hook_entry("X2-MENU-002 Tween_GetValue", gv, (void*)&h_get_value, &dummy) ||
        !hook_entry("X2-MENU-002 Tween_GetPhase", gp, (void*)&h_get_phase, &dummy)) return false;
    if (!patch_call_rel32(gt, gate, (void*)&h_gate)) return false;
    if (!patch_call_rel32(tl + 0x0C, tasks, (void*)&h_tasks)) {
        patch_call_rel32(gt, (void*)&h_gate, gate);
        return false;
    }
    log_line("  X2-MENU-002: gate gva=0x%08X (0x00764824) -> 0x%08X, tasks gva=0x%08X (0x00764894) -> 0x%08X, counter 0x%08X, fade "
             "0x%08X, Tween_Step 0x%08X, GetValue 0x%08X, GetPhase 0x%08X",
             rt_to_gva(gt), rt_to_gva(gate), rt_to_gva(tl + 0x0C), rt_to_gva(tasks), rt_to_gva((const void*)g_counter),
             rt_to_gva((const void*)g_fade), rt_to_gva(ts), rt_to_gva(gv), rt_to_gva(gp));
    return true;
}

Row row_MENU002 = {
    "X2-MENU-002",
    "field main menu at 60 fps: menu logic every 2nd pass, tween slides drawn half-way on logic passes",
    1, install, pre, post, 0
};

} // namespace x2
