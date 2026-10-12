// SPDX-License-Identifier: Apache-2.0
// fps60x2.ini next to FFX-2.exe, read once at init. [x2] global switches, [rows] one key per X2- row.
// The file is optional: missing keys take the defaults below.
#include "x2.h"
#include <stdio.h>

namespace x2 {

Config g_cfg;
static wchar_t g_ini[MAX_PATH];

void config_load() {
    wchar_t dir[MAX_PATH];
    game_dir(dir, MAX_PATH);
    _snwprintf_s(g_ini, MAX_PATH, _TRUNCATE, L"%sfps60x2.ini", dir);
    g_cfg.enabled = GetPrivateProfileIntW(L"x2", L"enabled", 1, g_ini);
    g_cfg.pacing = GetPrivateProfileIntW(L"x2", L"pacing", 1, g_ini);
    g_cfg.exec_method = GetPrivateProfileIntW(L"x2", L"exec_method", 0, g_ini);
    g_cfg.unpack_timeout_s = GetPrivateProfileIntW(L"x2", L"unpack_timeout_s", 60, g_ini);
    g_cfg.log_pacing_events = GetPrivateProfileIntW(L"x2", L"log_pacing_events", 1, g_ini);
}

int config_row(const char* row_id, int def) {
    wchar_t key[64];
    _snwprintf_s(key, 64, _TRUNCATE, L"%S", row_id);
    return GetPrivateProfileIntW(L"rows", key, def, g_ini);
}

} // namespace x2
