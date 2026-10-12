// SPDX-License-Identifier: Apache-2.0
// 60 Hz pacing: hold the frame divider at 1 and alternate exec 1/0, so every other Sg_MainLoop invocation is
// render-only. Game requests for 30 Hz are swallowed; requests for every-vsync (menus) and rows holding
// stock pacing pass through. Details: docs/fps60/pacing.md, README.md "Pacing policy"
#include "fps60.h"

static uint32_t g_wanted = 0;        // last divider value the GAME asked for
static bool     g_wanted_known = false;
static int      g_hold = 0;          // number of rows currently holding stock pacing
static bool     g_active = false;    // alternation applied this invocation
static bool     g_was_active = false;
static int      g_phase = 1;         // 1 = next rendered invocation is LOGIC
static int      g_kind = INV_STOCK;
static uint64_t g_count[4] = { 0, 0, 0, 0 };
static uint64_t g_inv = 0;
static int      g_log_budget = 200;  // rate limit for pacing event lines

#define PLOG(...) do { if (g_cfg.log_pacing_events && g_log_budget > 0) { g_log_budget--; log_line(__VA_ARGS__); } } while (0)

static void set_exec(int v) {
    if (g_cfg.exec_method == EXEC_BYTE) *g_exec_byte = (int8_t)v;
    else g_set_exec(v);
}

static void set_divider(uint32_t v) {
    if (g_set_divider_orig) g_set_divider_orig(v);   // trampoline once hooked: bypasses our hook
}

bool pacing_active() { return g_active; }
int  pacing_kind() { return g_kind; }

void pacing_hold_stock(const char* who, bool hold) {
    g_hold += hold ? 1 : -1;
    if (g_hold < 0) g_hold = 0;
    PLOG("pacing: %s %s stock pacing (holders now %d)", who, hold ? "holds" : "releases", g_hold);
}

bool pacing_divider_request(uint32_t req) {
    uint32_t prev = g_wanted;
    g_wanted = req;
    g_wanted_known = true;
    bool swallow = g_cfg.pacing && g_hold == 0 && req >= 2;
    if (req != prev) PLOG("pacing: game asks divider %u (was %u) -> %s", req, prev, swallow ? "swallowed, held at 1" : "passed");
    return swallow;
}

void pacing_pre() {
    g_inv++;
    if (!g_wanted_known && g_divider_hooked) {       // the game set its divider before our hook existed
        g_wanted = *g_divider;
        g_wanted_known = true;
        log_line("pacing: initial game divider %u", g_wanted);
    }

    bool want_active = g_cfg.pacing && g_divider_hooked && g_hold == 0 && g_wanted_known && g_wanted >= 2;

    if (!want_active) {
        if (g_was_active) {                          // leaving 60 Hz: give the game back its pacing
            set_divider(g_wanted ? g_wanted : 2);
            set_exec(1);
            g_phase = 1;
            PLOG("pacing: stock (divider -> %u, exec -> 1) at inv %llu", g_wanted, g_inv);
        }
        g_active = g_was_active = false;
        g_kind = INV_STOCK;
        g_count[INV_STOCK]++;
        return;
    }

    if (!g_was_active) PLOG("pacing: 60 Hz on at inv %llu (game divider %u)", g_inv, g_wanted);
    if (*g_divider != 1) {
        if (g_was_active) PLOG("pacing: divider was %u at pre of inv %llu - re-asserted 1", *g_divider, g_inv);
        set_divider(1);
    }

    bool backlog = *g_backlog_count > 1;
    int target;
    if (backlog && g_cfg.alt_backlog == ALT_BACKLOG_LOGIC) {
        target = 1;
        g_kind = INV_BACKLOG;
    } else {
        target = g_phase;
        g_phase ^= 1;
        g_kind = target ? INV_LOGIC : INV_BUILD;
    }
    if (*g_exec_byte != target) set_exec(target);
    g_count[g_kind]++;
    g_active = g_was_active = true;
}

// Dev hot reload: the game gets its own divider and exec back before this copy goes passive.
void pacing_shutdown() {
    if (g_was_active) {
        set_divider(g_wanted ? g_wanted : 2);
        set_exec(1);
        log_line("pacing: shutdown, divider -> %u, exec -> 1", g_wanted ? g_wanted : 2);
    }
    g_active = g_was_active = false;
    g_kind = INV_STOCK;
}

void pacing_post() {
    if ((g_inv % 36000) == 0) {                      // ~10 min at 60 Hz
        log_line("pacing stats: inv=%llu LOGIC=%llu BUILD=%llu BACKLOG=%llu STOCK=%llu", g_inv,
                 g_count[INV_LOGIC], g_count[INV_BUILD], g_count[INV_BACKLOG], g_count[INV_STOCK]);
    }
}
