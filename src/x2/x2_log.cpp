// SPDX-License-Identifier: Apache-2.0
// fps60x2.log next to FFX-2.exe. Same line format as the FFX mod's fps60.log, separate file and state.
#include "x2.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace x2 {

static HANDLE           g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;
static bool             g_cs_ready = false;

void game_dir(wchar_t* out, size_t cap) {
    out[0] = 0;
    DWORD n = GetModuleFileNameW(nullptr, out, (DWORD)cap);
    if (n == 0 || n >= cap) { out[0] = 0; return; }
    for (DWORD i = n; i > 0; i--) if (out[i - 1] == L'\\' || out[i - 1] == L'/') { out[i] = 0; return; }
    out[0] = 0;
}

void log_open() {
    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = true; }
    wchar_t dir[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    game_dir(dir, MAX_PATH);
    if (!dir[0]) return;
    for (int i = 3; i >= 1; i--) {                   // fps60x2.log -> .1 -> .2 -> .3
        if (i == 1) _snwprintf_s(from, MAX_PATH, _TRUNCATE, L"%sfps60x2.log", dir);
        else _snwprintf_s(from, MAX_PATH, _TRUNCATE, L"%sfps60x2.%d.log", dir, i - 1);
        _snwprintf_s(to, MAX_PATH, _TRUNCATE, L"%sfps60x2.%d.log", dir, i);
        MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
    }
    _snwprintf_s(from, MAX_PATH, _TRUNCATE, L"%sfps60x2.log", dir);
    g_log = CreateFileW(from, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void log_line(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE || !g_cs_ready) return;
    char buf[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond,
                        t.wMilliseconds);
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf_s(buf + n, sizeof(buf) - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    n = (m < 0) ? (int)strlen(buf) : n + m;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_cs);
    DWORD w;
    WriteFile(g_log, buf, (DWORD)n, &w, nullptr);
    LeaveCriticalSection(&g_cs);
}

} // namespace x2
