// SPDX-License-Identifier: Apache-2.0
// PERF-002 (default on): fall back to stock 30 fps when the PC cannot hold 60. Game logic advances once per logic
// invocation, and at divider 1 the backlog re-run only starts at >= 33 ms per frame (re/MAINLOOP.md section 5), so a PC
// that draws 45 fps under 60 Hz pacing runs the game at 0.75x speed. Stock pacing (divider 2) keeps game speed
// right at 30 fps. Frame times are measured at Sg_MainLoop entry on 60 Hz invocations, in windows of
// PERF-002-WINDOW frames; frames over 50 ms (loads, hitches) are counted apart, and a window with more than a quarter
// of them is ignored. Two slow windows in a row (mean below PERF-002-FPS) hold stock pacing; 60 Hz is retried after
// 20 s, then 40, 80 ... up to 640 s while retries keep failing (one slow window ends a retry). Every decision is logged.
#include "../fps60.h"

enum { HITCH_MS = 50, BASE_S = 20, MAX_S = 640 };

static LARGE_INTEGER g_freq = {}, g_prev = {};
static double   g_limit_ms = 1000.0 / 55;
static uint32_t g_win = 120;
static uint32_t g_n = 0, g_hitch = 0;
static double   g_sum = 0;
static int      g_slow = 0;                       // consecutive slow windows
static bool     g_held = false, g_retry = false;
static uint32_t g_backoff_s = BASE_S;
static LONGLONG g_retry_at = 0;
static uint32_t g_falls = 0;

static void hold(bool on) {
    if (g_held == on) return;
    g_held = on;
    pacing_hold_stock("PERF-002", on);
}

static void reset_window() { g_n = g_hitch = 0; g_sum = 0; }

static void window_done() {
    if (g_hitch * 4 > g_win) {
        log_line("PERF-002: window ignored (%u of %u frames over %d ms: loading or hitches)", g_hitch, g_win, HITCH_MS);
        reset_window();
        return;
    }
    double mean = g_n ? g_sum / g_n : 0;
    bool slow = mean > g_limit_ms;
    reset_window();
    if (!slow) {
        if (g_retry) {
            log_line("PERF-002: 60 fps retry holds (mean %.2f ms, %.1f fps); back-off reset", mean, 1000.0 / mean);
            g_retry = false;
            g_backoff_s = BASE_S;
        }
        g_slow = 0;
        return;
    }
    g_slow++;
    if (g_slow < (g_retry ? 1 : 2)) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_retry) g_backoff_s = g_backoff_s * 2 > MAX_S ? MAX_S : g_backoff_s * 2;
    g_retry_at = now.QuadPart + (LONGLONG)g_backoff_s * g_freq.QuadPart;
    g_retry = false;
    g_slow = 0;
    g_falls++;
    log_line("PERF-002: 60 fps not held (mean %.2f ms, %.1f fps, limit %.1f fps): stock 30 fps, retry in %u s (fall %u)",
             mean, 1000.0 / mean, 1000.0 / g_limit_ms, g_backoff_s, g_falls);
    hold(true);
}

static void pre() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_held) {
        g_prev.QuadPart = 0;
        if (now.QuadPart >= g_retry_at) {
            log_line("PERF-002: retrying 60 fps");
            g_retry = true;
            reset_window();
            hold(false);
        }
        return;
    }
    int k = pacing_kind();
    if (k != INV_LOGIC && k != INV_BUILD) { g_prev.QuadPart = 0; return; }   // stock, backlog, other holders
    if (g_prev.QuadPart) {
        double ms = (now.QuadPart - g_prev.QuadPart) * 1000.0 / g_freq.QuadPart;
        if (ms > HITCH_MS) g_hitch++;
        else { g_sum += ms; g_n++; }
        if (g_n + g_hitch >= g_win) window_done();
    }
    g_prev = now;
}

static bool install() {
    QueryPerformanceFrequency(&g_freq);
    int fps = config_row("PERF-002-FPS", 55);
    if (fps < 30) fps = 30;
    if (fps > 59) fps = 59;
    g_limit_ms = 1000.0 / fps;
    g_win = (uint32_t)config_row("PERF-002-WINDOW", 120);
    if (g_win < 30) g_win = 30;
    log_line("  PERF-002: stock 30 fps below %d fps over 2 windows of %u frames; retry after %d s, doubling to %d s",
             fps, g_win, BASE_S, MAX_S);
    return true;
}

static void stop() { hold(false); }

Row row_PERF002 = {
    "PERF-002",
    "fall back to stock 30 fps (right game speed) when the PC cannot hold 60; retries 60 with back-off",
    1, install, pre, nullptr, 0, stop
};
