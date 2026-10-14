// SPDX-License-Identifier: Apache-2.0
// CLK-002: map/BG object keyframe animations (FUN_00914a10) read a clock in 1/60 s units from FUN_00908970
// (frame counter * 2) or FUN_00908f70 (vblank), both +2 per invocation: 2x at 60 Hz.
// Fix: both getters return a mod clock that adds half the source's delta on 60 Hz invocations (+1 per
// 1/60 s: correct speed, and the consumer's lerp makes the motion 60 Hz smooth), the full delta otherwise.
// Details: docs/fps60/rows/CLK-002.md
#include "../fps60.h"

typedef uint32_t(__cdecl* clock_t_)(void);

struct ModClock {
    const char*        name;
    volatile uint32_t* src;
    uint32_t           mul;          // source units -> 1/60 s at stock
    uint8_t*           entry;
    clock_t_           tramp;
    uint32_t           last;         // source value at the last commit
    uint32_t           base;         // mod clock at seed
    uint64_t           half;         // committed advance since seed, in 1/120 s
    volatile LONG      calls;        // getter calls since the last report
    uint32_t           rep_src, rep_mod;   // source (x mul) and mod clock at the last report
};

static ModClock g_fc = { "frame counter getter FUN_00908970", nullptr, 2 };
static ModClock g_vb = { "vblank getter FUN_00908f70",        nullptr, 1 };
static bool g_hooked = false;

static bool paced_kind() {
    int k = pacing_kind();
    return k == INV_LOGIC || k == INV_BUILD;     // backlog/stock invocations are 1/30 s each
}

static uint32_t live(const ModClock& c) { return *c.src * c.mul; }

static uint32_t value(const ModClock& c) {
    uint64_t d = (uint32_t)(live(c) - c.last);
    uint64_t h = c.half + (paced_kind() ? d : d * 2);
    return c.base + (uint32_t)(h >> 1);
}

static void commit(ModClock& c) {
    uint32_t now = live(c);
    uint64_t d = (uint32_t)(now - c.last);
    c.half += paced_kind() ? d : d * 2;
    c.last = now;
}

static uint32_t __cdecl h_fc() { InterlockedIncrement(&g_fc.calls); return value(g_fc); }
static uint32_t __cdecl h_vb() { InterlockedIncrement(&g_vb.calls); return value(g_vb); }

static bool install() {
    // FUN_00914a10: copy +0xba3dc -> +0xba3e0; JNZ; CALL FUN_00908970; JMP; CALL FUN_00908f70
    uint8_t* at = sig_find_unique("CLK-002 map anim clock select FUN_00914a10",
        "8B 87 ?? ?? ?? ?? 89 87 ?? ?? ?? ?? 75 07 E8 ?? ?? ?? ?? EB 05 E8", 0x914A3E);
    if (!at) return false;
    g_fc.entry = call_target(at + 0x0E);
    g_vb.entry = call_target(at + 0x15);
    g_fc.src = (volatile uint32_t*)sym_rt("frame_counter");
    g_vb.src = (volatile uint32_t*)sym_rt("vblank");
    // MOV EAX,[frame_counter]; ADD EAX,EAX; RET  /  MOV EAX,[vblank]; RET (plain RET, no args)
    if (!g_fc.src || !g_vb.src || !g_fc.entry || !g_vb.entry ||
        !sig_match(g_fc.entry, "A1 ?? ?? ?? ?? 03 C0 C3") || read_abs(g_fc.entry + 1) != (uint8_t*)g_fc.src ||
        !sig_match(g_vb.entry, "A1 ?? ?? ?? ?? C3")       || read_abs(g_vb.entry + 1) != (uint8_t*)g_vb.src) {
        log_line("  CLK-002: clock getters %p / %p do not match - refused", g_fc.entry, g_vb.entry);
        return false;
    }
    log_line("  CLK-002: getters at gva=0x%08X / 0x%08X (expected 0x00908970 / 0x00908F70), hooked on the game thread",
             rt_to_gva_exe(g_fc.entry), rt_to_gva_exe(g_vb.entry));
    return true;
}

static void seed(ModClock& c) {
    c.last = live(c);
    c.base = c.last;
    c.half = 0;
    c.rep_src = c.last;
    c.rep_mod = c.base;
}

// Every ~10 s while a map animation reads a clock: calls and how far the source and the mod clock moved.
static void report(ModClock& c) {
    LONG n = InterlockedExchange(&c.calls, 0);
    uint32_t src = live(c), mod = c.base + (uint32_t)(c.half >> 1);
    if (n > 0) {
        uint32_t ds = src - c.rep_src, dm = mod - c.rep_mod;
        log_line("CLK-002: %s: %ld calls in the last 600 invocations; source +%u, mod clock +%u (ratio %.2f)",
                 c.name, n, ds, dm, ds ? (double)dm / (double)ds : 0.0);
    }
    c.rep_src = src;
    c.rep_mod = mod;
}

static void pre() {
    if (g_hooked) return;
    g_hooked = true;
    seed(g_fc);
    seed(g_vb);
    if (!hook_entry("CLK-002 fc", g_fc.entry, (void*)&h_fc, (void**)&g_fc.tramp) ||
        !hook_entry("CLK-002 vb", g_vb.entry, (void*)&h_vb, (void**)&g_vb.tramp)) {
        hook_remove("CLK-002 fc", g_fc.entry);
        log_line("CLK-002: getter hook failed - row off");
        row_CLK002.on = 0;
        return;
    }
    log_line("CLK-002: map clock getters hooked (fc=%u vb=%u)", g_fc.base, g_vb.base);
}

static void post() {
    commit(g_fc);
    commit(g_vb);
    static uint32_t tick = 0;
    if (++tick % 600 == 0) { report(g_fc); report(g_vb); }
}

Row row_CLK002 = {
    "CLK-002",
    "map/BG object animation clock advances at 60 Hz units of wall time under 60 Hz pacing",
    1, install, pre, post, 0
};
