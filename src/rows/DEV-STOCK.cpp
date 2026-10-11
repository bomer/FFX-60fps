// SPDX-License-Identifier: Apache-2.0
// DEV-STOCK (test tool): Ctrl + DEV-STOCK-KEY toggles stock 30 Hz pacing at runtime, through the same pacing hold
// W2A-002 uses during FMVs, so one game session can compare a scene at stock and at 60 Hz. Logged as
// "DEV-STOCK: stock pacing on/off".
// ini [rows]: DEV-STOCK=1, DEV-STOCK-KEY=<virtual-key code, decimal; 116 = F5>.
#include "../fps60.h"

static int  g_key = 0x74;
static bool g_prev = false, g_stock = false;

static bool install() {
    g_key = config_row("DEV-STOCK-KEY", 0x74);
    log_line("  DEV-STOCK: Ctrl + VK 0x%02X toggles stock 30 Hz pacing", g_key);
    return true;
}

static void pre() {
    bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(g_key) & 0x8000);
    bool edge = down && !g_prev;
    g_prev = down;
    if (!edge) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    g_stock = !g_stock;
    pacing_hold_stock("DEV-STOCK", g_stock);
    log_line("DEV-STOCK: stock pacing %s", g_stock ? "on" : "off");
}

Row row_DEVSTOCK = {
    "DEV-STOCK",
    "test tool: Ctrl + DEV-STOCK-KEY toggles stock 30 Hz pacing at runtime (stock vs 60 in one session)",
    0, install, pre, nullptr, 0
};
