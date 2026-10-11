// SPDX-License-Identifier: Apache-2.0
// Row registry: one entry per ledger-row mitigation, each toggled by its row ID in [rows] and logging one
// install line. How to add a row: README.md "Adding a row".
#include "fps60.h"
#include <string.h>

// ---------------------------------------------------------------- row table
// Order follows re/TRIAGE.md.
static Row* g_rows[] = {
    &row_MAG008,
    &row_W2A002,
    &row_CLK002,
    &row_LOOP005,
    &row_LOOP004,
    &row_MAG006,
    &row_MAG009,
    &row_MAG016,
    &row_MAG017,
    &row_MAG018,
    &row_MAG019,
    &row_MAG020,
    &row_MAG021,
    &row_MAG022,
    &row_MAG023,
    &row_MAG024,
    &row_UI001,
    &row_UI017,
    &row_UISPH001,
    &row_UISPH002,
    &row_UI014,
    &row_UIBTLL1,
    &row_TEXANIM001,
    &row_SR003,
    &row_DEVTURBO,
    &row_FX010,
    &row_DIAG_BTLSIG,
    &row_DIAG_DRAWSTEP,
    &row_DIAG_TEXTDUMP,
    &row_DIAG_HANGDUMP,
    &row_DIAG_DYNGEO,
    &row_SMOOTH001,                              // after LOOP-004/LOOP-005 (install reads row_on)
    &row_SMOOTH002,
    &row_SMOOTH003,
    nullptr
};

void rows_install_all() {
    int n = 0, on = 0;
    for (Row** p = g_rows; *p; p++) {
        Row& r = **p;
        n++;
        r.on = 0;
        if (!config_row(r.id, r.default_on)) {
            log_line("row %-10s OFF (ini) - %s", r.id, r.summary);
            continue;
        }
        bool ok = false;
        __try { ok = r.install ? r.install() : true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        r.on = ok ? 1 : 0;
        if (ok) on++;
        log_line("row %-10s %s - %s", r.id, ok ? "ON" : "FAILED to install, stays off", r.summary);
    }
    log_line("rows: %d registered, %d on", n, on);
}

bool row_on(const char* id) {
    for (Row** p = g_rows; *p; p++) if (strcmp((*p)->id, id) == 0) return (*p)->on != 0;
    return true;                                 // not a registered row: never gated
}

// Diagnostic hotkey F8: turn the camera rows (LOOP-005, LOOP-004) off/on at runtime, to tell whether
// they cause a soft-lock (Ixion summon, 10/10/2026). Only rows that installed can be turned back on.
static void hotkey_camera_rows() {
    static bool prev = false;
    static int  installed[2] = { -1, -1 };
    Row* rows[2] = { &row_LOOP005, &row_LOOP004 };
    bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool edge = down && !prev;
    prev = down;
    if (!edge) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    for (int i = 0; i < 2; i++) if (installed[i] < 0) installed[i] = rows[i]->on;
    int v = (rows[0]->on || rows[1]->on) ? 0 : 1;
    for (int i = 0; i < 2; i++) rows[i]->on = v && installed[i];
    log_line("F8: camera rows LOOP-005=%d LOOP-004=%d", rows[0]->on, rows[1]->on);
}

void rows_pre() {
    hotkey_camera_rows();
    for (Row** p = g_rows; *p; p++)
        if ((*p)->on && (*p)->pre) { where_set(W_ROW_PRE, (int)(p - g_rows)); (*p)->pre(); }
}

void rows_post() {
    for (Row** p = g_rows; *p; p++)
        if ((*p)->on && (*p)->post) { where_set(W_ROW_POST, (int)(p - g_rows)); (*p)->post(); }
}

int rows_count() { return (int)(sizeof(g_rows) / sizeof(g_rows[0])) - 1; }

const Row* row_at(int i) { return (i >= 0 && i < rows_count()) ? g_rows[i] : nullptr; }
