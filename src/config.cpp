// SPDX-License-Identifier: Apache-2.0
// fps60.ini next to FFX.exe. Read once at init. [fps60] holds the global switches; [rows] holds
// one key per ledger row mitigation (ROW_ID=1 on, 0 off; missing = the row's default).
#include "fps60.h"
#include <stdlib.h>

Config g_cfg;

static char g_ini[MAX_PATH * 2];

static long ini_int(const char* sect, const char* key, long def) {
    char buf[64];
    GetPrivateProfileStringA(sect, key, "", buf, sizeof(buf), g_ini);
    if (!buf[0]) return def;
    char* end = nullptr;
    long v = strtol(buf, &end, 0);
    if (end == buf) return def;
    return v;
}

void config_load() {
    wchar_t w[MAX_PATH];
    game_dir(w, MAX_PATH);
    lstrcatW(w, L"fps60.ini");
    WideCharToMultiByte(CP_ACP, 0, w, -1, g_ini, sizeof(g_ini), nullptr, nullptr);

    const char* S = "fps60";
    Config& c = g_cfg;
    c.enabled           = (int)ini_int(S, "enabled", 1);
    c.pacing            = (int)ini_int(S, "pacing", 1);
    c.rollback          = (int)ini_int(S, "rollback", 1);
    c.unpack_timeout_s  = (int)ini_int(S, "unpack_timeout_s", 60);
    c.log_pacing_events = (int)ini_int(S, "log_pacing_events", 1);
    c.crash_dump_mem    = (int)ini_int(S, "crash_dump_mem", 0);

    char buf[32];
    GetPrivateProfileStringA(S, "alt_backlog", "logic", buf, sizeof(buf), g_ini);
    c.alt_backlog = (_stricmp(buf, "toggle") == 0) ? ALT_BACKLOG_TOGGLE : ALT_BACKLOG_LOGIC;
    GetPrivateProfileStringA(S, "exec_method", "setter", buf, sizeof(buf), g_ini);
    c.exec_method = (_stricmp(buf, "byte") == 0) ? EXEC_BYTE : EXEC_SETTER;
    if (c.unpack_timeout_s < 5) c.unpack_timeout_s = 5;
}

int config_row(const char* row_id, int def) {
    return (int)ini_int("rows", row_id, def);
}
