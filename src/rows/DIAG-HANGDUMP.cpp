// SPDX-License-Identifier: Apache-2.0
// Diagnostic DIAG-HANGDUMP (observe only): when the game thread stops completing Sg_MainLoop invocations, a
// watchdog writes fps60_hang.txt (breadcrumbs, lock owners, every thread's stack) and fps60_hang.dmp.
// Details: docs/fps60/rows/DIAG-HANGDUMP.md
#include "../fps60.h"
#include <stdio.h>
#include <stdarg.h>

static volatile LONG g_count = 0;
static volatile DWORD g_game_tid = 0;
static int    g_sec = 10;
static HANDLE g_txt = INVALID_HANDLE_VALUE, g_dmp = INVALID_HANDLE_VALUE;
static void*  g_minidump = nullptr;              // System32 MiniDumpWriteDump
typedef LONG(NTAPI* NtQSI_t)(ULONG, PVOID, ULONG, PULONG);
static NtQSI_t g_ntqsi = nullptr;
static DWORD  g_watch_tid = 0;

// ---------------------------------------------------------------- report helpers (no heap, no locks)
static char g_line[1024];
static void rep(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int n = _vsnprintf_s(g_line, sizeof(g_line) - 2, _TRUNCATE, fmt, a);
    va_end(a);
    if (n < 0) n = (int)strlen(g_line);
    g_line[n++] = '\r'; g_line[n++] = '\n';
    DWORD w;
    if (g_txt != INVALID_HANDLE_VALUE) WriteFile(g_txt, g_line, (DWORD)n, &w, nullptr);
}

static bool rd(const void* p, void* out, size_t n) { return safe_copy(out, p, n); }

// Module + Ghidra VA of an address, from VirtualQuery and the module's own PE export name.
static void describe(uint32_t a, char* out, size_t cap) {
    MEMORY_BASIC_INFORMATION mb;
    if (a < 0x10000 || !VirtualQuery((void*)(uintptr_t)a, &mb, sizeof(mb)) || mb.Type != MEM_IMAGE) {
        _snprintf_s(out, cap, _TRUNCATE, "0x%08X", a);
        return;
    }
    uint8_t* base = (uint8_t*)mb.AllocationBase;
    char name[40] = "?";
    if (base == exe_base()) strcpy_s(name, "FFX.exe");
    else {
        IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS32 nt; IMAGE_EXPORT_DIRECTORY ex;
        if (rd(base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && rd(base + dos.e_lfanew, &nt, sizeof(nt)) &&
            nt.Signature == IMAGE_NT_SIGNATURE &&
            nt.OptionalHeader.NumberOfRvaAndSizes > 0 && nt.OptionalHeader.DataDirectory[0].VirtualAddress &&
            rd(base + nt.OptionalHeader.DataDirectory[0].VirtualAddress, &ex, sizeof(ex)) && ex.Name) {
            rd(base + ex.Name, name, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
        }
    }
    uint32_t off = a - (uint32_t)(uintptr_t)base;
    if (base == exe_base()) _snprintf_s(out, cap, _TRUNCATE, "FFX.exe gva 0x%08X", 0x400000u + off);
    else if (_strnicmp(name, "magic", 5) == 0) _snprintf_s(out, cap, _TRUNCATE, "%s@%p gva 0x%08X", name, base, 0x10000000u + off);
    else _snprintf_s(out, cap, _TRUNCATE, "%s+0x%X", name, off);
}

static bool is_code(uint32_t a) {
    MEMORY_BASIC_INFORMATION mb;
    return a >= 0x10000 && VirtualQuery((void*)(uintptr_t)a, &mb, sizeof(mb)) && mb.Type == MEM_IMAGE &&
           (mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

// A return address: code, and a CALL right before it (E8 rel32, FF /2 with 2..7-byte forms).
static bool is_ret(uint32_t a) {
    if (!is_code(a)) return false;
    uint8_t b[8];
    if (!rd((void*)(uintptr_t)(a - 8), b, 8)) return false;
    if (b[3] == 0xE8) return true;                                   // a-5
    for (int k = 2; k <= 7; k++) if (b[8 - k] == 0xFF && ((b[9 - k] >> 3) & 7) == 2) return true;
    return false;
}

static uint32_t g_stack[2048];                   // 8 KB copy of a thread's stack top

static void report_thread(DWORD tid, const char* tags, ULONG state, ULONG wait) {
    char d[160];
    if (tid == g_watch_tid) return;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { rep("thread %lu%s: OpenThread failed (%lu)", tid, tags, GetLastError()); return; }
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    int words = 0;
    bool ok = false;
    if (SuspendThread(h) != (DWORD)-1) {
        ok = GetThreadContext(h, &c) != 0;
        if (ok) {
            MEMORY_BASIC_INFORMATION mb;
            uint32_t sp = c.Esp, end = sp + sizeof(g_stack);
            if (VirtualQuery((void*)(uintptr_t)sp, &mb, sizeof(mb))) {
                uint32_t top = (uint32_t)(uintptr_t)mb.BaseAddress + (uint32_t)mb.RegionSize;
                if (end > top) end = top;
            }
            if (end > sp && rd((void*)(uintptr_t)sp, g_stack, end - sp)) words = (int)((end - sp) / 4);
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    if (!ok) { rep("thread %lu%s: no context", tid, tags); return; }
    describe(c.Eip, d, sizeof(d));
    rep("thread %lu%s: state %lu wait %lu, eip %s, esp %08lX ebp %08lX eax %08lX ecx %08lX edx %08lX ebx %08lX esi %08lX edi %08lX",
        tid, tags, state, wait, d, c.Esp, c.Ebp, c.Eax, c.Ecx, c.Edx, c.Ebx, c.Esi, c.Edi);
    // EBP chain (within the copied window)
    uint32_t bp = c.Ebp;
    for (int i = 0; i < 16; i++) {
        if (bp < c.Esp || bp + 8 > c.Esp + (uint32_t)words * 4) break;
        uint32_t next = g_stack[(bp - c.Esp) / 4], ret = g_stack[(bp - c.Esp) / 4 + 1];
        if (!is_code(ret)) break;
        describe(ret, d, sizeof(d));
        rep("    ebp[%d] %s", i, d);
        if (next <= bp) break;
        bp = next;
    }
    // Stack scan: CALL-preceded return addresses
    int shown = 0;
    for (int i = 0; i < words && shown < 28; i++) {
        uint32_t v = g_stack[i];
        if (!is_ret(v)) continue;
        describe(v, d, sizeof(d));
        rep("    [esp+0x%04X] %s", i * 4, d);
        shown++;
    }
}

static uint8_t g_spi[1 << 20];                   // SystemProcessInformation buffer (1 MB)

static void report(LONG count) {
    char w[200], l[300];
    LONG where = g_where;
    where_text(where, w, sizeof(w));
    ldr_text(l, sizeof(l));
    DWORD ldr_owner = 0;
    {
        uint8_t* peb = (uint8_t*)(uintptr_t)__readfsdword(0x30);
        uint8_t* ll = nullptr;
        if (rd(peb + 0xA0, &ll, 4) && ll) rd(ll + 0x0C, &ldr_owner, 4);   // RTL_CRITICAL_SECTION.OwningThread
    }
    DWORD log_owner = log_cs_owner();
    SYSTEMTIME t;
    GetLocalTime(&t);
    rep("fps60 DIAG-HANGDUMP report %04u-%02u-%02u %02u:%02u:%02u: no Sg_MainLoop invocation for %d s (count %ld)",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, g_sec, count);
    rep("game thread %lu: %s", g_game_tid, w);
    rep("%s", l);
    rep("loader lock owner: thread %lu; log CS owner: thread %lu; inv kind %d; MAG-021 %s",
        ldr_owner, log_owner, pacing_kind(), mag021_state());

    // Interesting threads first (in case the full listing gets stuck), then all.
    report_thread(g_game_tid, " [GAME]", 0, 0);
    if (ldr_owner && ldr_owner != g_game_tid) report_thread(ldr_owner, " [LOADER LOCK OWNER]", 0, 0);
    if (log_owner && log_owner != g_game_tid && log_owner != ldr_owner) report_thread(log_owner, " [LOG CS OWNER]", 0, 0);
    rep("---- all threads");
    ULONG len = 0;
    if (g_ntqsi && g_ntqsi(5 /*SystemProcessInformation*/, g_spi, sizeof(g_spi), &len) == 0) {
        DWORD pid = GetCurrentProcessId();
        for (uint32_t off = 0; off + 0xB8 <= sizeof(g_spi);) {
            uint8_t* p = g_spi + off;
            uint32_t next = *(uint32_t*)p, nthr = *(uint32_t*)(p + 4);
            if (*(uint32_t*)(p + 0x44) == pid) {
                for (uint32_t i = 0; i < nthr && i < 512; i++) {
                    uint8_t* th = p + 0xB8 + i * 0x40;
                    if (th + 0x40 > g_spi + sizeof(g_spi)) break;
                    DWORD tid = *(DWORD*)(th + 0x24);
                    char tags[64] = "";
                    if (tid == g_game_tid) strcat_s(tags, " [GAME]");
                    if (tid == ldr_owner) strcat_s(tags, " [LOADER LOCK OWNER]");
                    if (tid == log_owner) strcat_s(tags, " [LOG CS OWNER]");
                    report_thread(tid, tags, *(ULONG*)(th + 0x34), *(ULONG*)(th + 0x38));
                }
                break;
            }
            if (!next) break;
            off += next;
        }
    } else rep("NtQuerySystemInformation failed - other threads not listed");
    rep("---- end of report; minidump follows in fps60_hang.dmp");
    FlushFileBuffers(g_txt);
    log_line_try("DIAG-HANGDUMP: no Sg_MainLoop invocation for %d s - game thread %s; %s; loader lock owner %lu, log CS owner %lu "
                 "- report fps60_hang.txt", g_sec, w, l, ldr_owner, log_owner);

    typedef BOOL(WINAPI* md_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    static bool dumped = false;                  // one minidump per session (the file is pre-opened)
    if (g_minidump && g_dmp != INVALID_HANDLE_VALUE && !dumped) {
        dumped = true;
        const int type = 0x00001000 /*ThreadInfo*/ | 0x00000040 /*IndirectlyReferencedMemory*/ | 0x00000010 /*ScanMemory*/;
        BOOL ok = ((md_t)g_minidump)(GetCurrentProcess(), GetCurrentProcessId(), g_dmp, type, nullptr, nullptr, nullptr);
        DWORD err = ok ? 0 : GetLastError();
        FlushFileBuffers(g_dmp);
        rep("minidump %s (err %lu)", ok ? "written" : "FAILED", err);
        log_line_try("DIAG-HANGDUMP: minidump fps60_hang.dmp %s (err %lu)", ok ? "written" : "FAILED", err);
    }
    FlushFileBuffers(g_txt);
}

static DWORD WINAPI watchdog(LPVOID) {
    g_watch_tid = GetCurrentThreadId();
    LONG last = -1;
    DWORD still_ms = 0;
    LONG where_1s = 0;
    char l[300] = "";
    bool reported = false;
    for (;;) {
        Sleep(250);
        LONG c = g_count;
        if (c == 0 || g_disabled) continue;              // game loop not started yet
        if (c != last) {
            if (still_ms >= 1000 && !reported) {
                char w[200];
                where_text(where_1s, w, sizeof(w));
                log_line_try("DIAG-HANGDUMP: game thread stalled ~%lu ms (recovered) at %s; %s", still_ms, w, l);
            }
            last = c; still_ms = 0; reported = false;
            continue;
        }
        still_ms += 250;
        if (still_ms == 1000) { where_1s = g_where; ldr_text(l, sizeof(l)); }
        if (!reported && still_ms >= (DWORD)g_sec * 1000) {
            reported = true;
            if (g_txt != INVALID_HANDLE_VALUE) SetFilePointer(g_txt, 0, nullptr, FILE_END);
            report(c);
        }
    }
}

static int g_test = 0;                           // DIAG-HANGDUMP-TEST=1: Ctrl+F6 stalls the game thread to test the report

static void pre() {
    if (!g_game_tid) g_game_tid = GetCurrentThreadId();
    InterlockedIncrement(&g_count);
    if (!g_test) return;
    static bool prev = false;
    bool down = (GetAsyncKeyState(VK_F6) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
    bool edge = down && !prev;
    prev = down;
    if (!edge) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    log_line("DIAG-HANGDUMP: TEST - Ctrl+F6, game thread sleeps %d s inside rows_pre", g_sec + 3);
    Sleep((DWORD)(g_sec + 3) * 1000);
    log_line("DIAG-HANGDUMP: TEST - game thread resumed");
}

// The previous session's non-empty fps60_hang.<ext> becomes fps60_hang_<last write time>.<ext>; then a fresh one is opened.
static HANDLE open_rotated(const wchar_t* ext) {
    wchar_t dir[MAX_PATH], path[MAX_PATH], old[MAX_PATH];
    game_dir(dir, MAX_PATH);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%sfps60_hang.%s", dir, ext);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh)) {
        FILETIME lt; SYSTEMTIME st;
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &st);
        _snwprintf_s(old, MAX_PATH, _TRUNCATE, L"%sfps60_hang_%04u%02u%02u-%02u%02u%02u.%s", dir, st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, ext);
        MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
    }
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static bool install() {
    g_sec = config_row("DIAG-HANGDUMP-SEC", 10);
    if (g_sec < 3) g_sec = 3;
    g_test = config_row("DIAG-HANGDUMP-TEST", 0);
    g_minidump = proxy_real_minidump();
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    g_ntqsi = nt ? (NtQSI_t)GetProcAddress(nt, "NtQuerySystemInformation") : nullptr;
    g_txt = open_rotated(L"txt");
    g_dmp = open_rotated(L"dmp");
    HANDLE t = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    if (!t) { log_line("  DIAG-HANGDUMP: watchdog thread failed - off"); return false; }
    CloseHandle(t);
    log_line("  DIAG-HANGDUMP: watchdog on - a %d s stall of Sg_MainLoop writes fps60_hang.txt (where/loader breadcrumbs, "
             "lock owners, all thread stacks) then fps60_hang.dmp; stalls >= 1 s that recover are logged "
             "(report file %s, minidump %s)", g_sec, g_txt != INVALID_HANDLE_VALUE ? "open" : "NOT open",
             g_minidump ? "resolved" : "NOT resolved");
    return true;
}

Row row_DIAG_HANGDUMP = {
    "DIAG-HANGDUMP",
    "diagnostic: where-breadcrumbs + all-thread stack report + minidump when Sg_MainLoop stalls; observe only",
    1, install, pre, nullptr, 0
};
