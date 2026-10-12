// SPDX-License-Identifier: Apache-2.0
// fps60.log next to FFX.exe (install lines, row toggles, pacing events). Each line is handed to the OS with
// WriteFile at once, so the tail survives a game crash (the OS cache still writes it). No FlushFileBuffers: it waited
// for the disk on the game thread and stalled the game 1.7 s on one DIAG-FRAMETIME line (D 12/10/2026 18:03:30,
// "rows: DIAG-FRAMETIME pre took 1708.6 ms"; U: big stutter). Only an OS crash or power loss can lose lines now.
#include "fps60.h"
#include <stdio.h>
#include <stdarg.h>

static HANDLE           g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_cs;
static bool             g_log_init = false;

void game_dir(wchar_t* out, size_t cap) {
    out[0] = 0;
    DWORD n = GetModuleFileNameW(nullptr, out, (DWORD)cap);
    if (n == 0 || n >= cap) { out[0] = 0; return; }
    for (DWORD i = n; i-- > 0;) {
        if (out[i] == L'\\' || out[i] == L'/') { out[i + 1] = 0; return; }
    }
    out[0] = 0;
}

// Dev hot reload: reload.cpp loads each new copy from fps60_live_<tick>.dll, so a copy is a reload only when its own
// file has that name. (It used to be the environment variable FPS60_HOT, which a restarted game inherited: the stale
// base dbghelp.dll then ran as a "hot reload", appended to the old log and kept the live copies, U+D 12/10/2026.)
bool hot_copy() {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&hot_copy, &self)) return false;
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const wchar_t* base = path;
    for (DWORD i = 0; i < n; i++) if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
    return _wcsnicmp(base, L"fps60_live_", 11) == 0;
}

void log_open() {
    if (g_log_init) return;
    InitializeCriticalSection(&g_log_cs);
    g_log_init = true;

    wchar_t path[MAX_PATH];
    game_dir(path, MAX_PATH);
    if (!path[0] || lstrlenW(path) > MAX_PATH - 20) return;
    // Dev hot reload: the previous copy closed the log; continue the same file.
    if (hot_copy()) {
        lstrcatW(path, L"fps60.log");
        g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_log != INVALID_HANDLE_VALUE) SetFilePointer(g_log, 0, nullptr, FILE_END);
        return;
    }
    // Keep the previous 5 sessions: fps60.log -> fps60.1.log -> ... -> fps60.5.log (10/10/2026: a
    // stock reference run was lost when the next start truncated the log).
    int dir_len = lstrlenW(path);
    for (int i = 5; i >= 1; i--) {
        wchar_t from[MAX_PATH], to[MAX_PATH];
        lstrcpyW(from, path); lstrcpyW(to, path);
        if (i == 1) lstrcatW(from, L"fps60.log");
        else wsprintfW(from + dir_len, L"fps60.%d.log", i - 1);
        wsprintfW(to + dir_len, L"fps60.%d.log", i);
        MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
    }
    lstrcatW(path, L"fps60.log");
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
}

static bool log_write(bool try_only, const char* fmt, va_list args) {
    char msg[1024];
    int m = vsnprintf(msg, sizeof(msg), fmt, args);
    if (m < 0) return false;

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1152];
    int k = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u | %s\r\n",
                     t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    if (k <= 0) return false;
    if (k >= (int)sizeof(line)) k = (int)sizeof(line) - 1;

    if (try_only) { if (!TryEnterCriticalSection(&g_log_cs)) return false; }
    else EnterCriticalSection(&g_log_cs);
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_log, line, (DWORD)k, &written, nullptr);
    }
    LeaveCriticalSection(&g_log_cs);
    if (!try_only) OutputDebugStringA(line);
    return true;
}

// Dev hot reload: a failed load gives the log back to the passive copy.
void log_reopen() {
    if (!g_log_init || g_log != INVALID_HANDLE_VALUE) return;
    wchar_t path[MAX_PATH];
    game_dir(path, MAX_PATH);
    if (!path[0] || lstrlenW(path) > MAX_PATH - 20) return;
    lstrcatW(path, L"fps60.log");
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log != INVALID_HANDLE_VALUE) SetFilePointer(g_log, 0, nullptr, FILE_END);
}

void log_close() {
    if (!g_log_init) return;
    EnterCriticalSection(&g_log_cs);
    if (g_log != INVALID_HANDLE_VALUE) { CloseHandle(g_log); g_log = INVALID_HANDLE_VALUE; }
    LeaveCriticalSection(&g_log_cs);
}

// Memory ring for high-rate diagnostics (SMOOTH-003 DIAG-ID): lines are kept, not written, until log_ring_dump
// (dev hotkey Num 3 when the glitch is seen), so the frames before it are in the log too.
enum { RING_N = 8192, RING_W = 240 };
static char  g_ring[RING_N][RING_W];
static LONG  g_ring_pos = 0;
static CRITICAL_SECTION g_ring_cs;
static bool  g_ring_init = false;

void log_ring(const char* fmt, ...) {
    if (!g_log_init) return;
    if (!g_ring_init) { InitializeCriticalSection(&g_ring_cs); g_ring_init = true; }   // first use: game thread
    SYSTEMTIME t;
    GetLocalTime(&t);
    char msg[RING_W];
    int k = snprintf(msg, sizeof(msg), "%02u:%02u:%02u.%03u | ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    if (k > 0 && k < (int)sizeof(msg)) vsnprintf(msg + k, sizeof(msg) - k, fmt, args);
    va_end(args);
    EnterCriticalSection(&g_ring_cs);
    memcpy(g_ring[g_ring_pos % RING_N], msg, sizeof(msg));
    g_ring_pos++;
    LeaveCriticalSection(&g_ring_cs);
}

void log_ring_dump(const char* why) {
    if (!g_ring_init) { log_line("%s: ring empty", why); return; }
    EnterCriticalSection(&g_ring_cs);
    LONG n = g_ring_pos < RING_N ? g_ring_pos : RING_N, first = g_ring_pos - n;
    log_line("==== %s: last %ld ring lines (oldest first) ====", why, n);
    EnterCriticalSection(&g_log_cs);
    for (LONG i = 0; i < n && g_log != INVALID_HANDLE_VALUE; i++) {
        const char* m = g_ring[(first + i) % RING_N];
        DWORD w;
        WriteFile(g_log, "  ring ", 7, &w, nullptr);
        WriteFile(g_log, m, (DWORD)strnlen(m, RING_W), &w, nullptr);
        WriteFile(g_log, "\r\n", 2, &w, nullptr);
    }
    LeaveCriticalSection(&g_log_cs);
    g_ring_pos = 0;
    LeaveCriticalSection(&g_ring_cs);
    log_line("==== %s: end of ring ====", why);
}

void log_line(const char* fmt, ...) {
    if (!g_log_init) return;
    va_list args;
    va_start(args, fmt);
    log_write(false, fmt, args);
    va_end(args);
}

// DIAG-HANGDUMP: the watchdog must not block on a log CS that a hung thread holds.
bool log_line_try(const char* fmt, ...) {
    if (!g_log_init) return false;
    va_list args;
    va_start(args, fmt);
    bool ok = log_write(true, fmt, args);
    va_end(args);
    return ok;
}

DWORD log_cs_owner() {
    return g_log_init ? (DWORD)(uintptr_t)g_log_cs.OwningThread : 0;
}
