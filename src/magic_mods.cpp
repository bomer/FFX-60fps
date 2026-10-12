// SPDX-License-Identifier: Apache-2.0
// Magic overlay (magic_*.dll) load/unload tracking shared by the MAG rows: one loader notification,
// subscriber callbacks under the loader lock. The image is still mapped at unload, so rows unhook there.
// Details: docs/fps60/magic_mods.md
#include "fps60.h"
#include <string.h>

typedef struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } MM_UNICODE_STRING;
typedef struct { ULONG Flags; const MM_UNICODE_STRING* FullDllName; const MM_UNICODE_STRING* BaseDllName; PVOID DllBase; ULONG SizeOfImage; } MM_LDR_NOTIF_DATA;
typedef VOID(CALLBACK* MM_LDR_NOTIF_FN)(ULONG, const MM_LDR_NOTIF_DATA*, PVOID);
typedef LONG(NTAPI* MM_LdrRegisterDllNotification_t)(ULONG, MM_LDR_NOTIF_FN, PVOID, PVOID*);

enum { MAX_SUBS = 16 };
struct Sub { const char* row; magic_load_fn on_load; magic_unload_fn on_unload; };
static Sub  g_subs[MAX_SUBS];
static volatile LONG g_nsubs = 0;
static bool g_registered = false;

static void call_load(const Sub& s, uint8_t* base, uint32_t size, const char* name) {
    __try { s.on_load(base, size, name); }
    __except (EXCEPTION_EXECUTE_HANDLER) { log_line("%s: fault 0x%08lX in the load callback for %s", s.row, GetExceptionCode(), name); }
}

static void call_unload(const Sub& s, uint8_t* base) {
    __try { s.on_unload(base); }
    __except (EXCEPTION_EXECUTE_HANDLER) { log_line("%s: fault 0x%08lX in the unload callback", s.row, GetExceptionCode()); }
}

static void CALLBACK dll_notif(ULONG reason, const MM_LDR_NOTIF_DATA* d, PVOID) {
    if (!d) return;
    char name[24] = {};
    __try {
        const MM_UNICODE_STRING* s = d->BaseDllName;
        if (!s || !s->Buffer) return;
        int len = s->Length / 2;
        for (int i = 0; i < len && i < 23; i++) name[i] = (char)s->Buffer[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (_strnicmp(name, "magic_", 6) != 0) return;

    uint8_t* base = (uint8_t*)d->DllBase;
    LONG n = g_nsubs;
    LARGE_INTEGER f, t0, a, b = {};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    double slow_ms = 0;
    const char* slow = "-";
    for (LONG i = 0; i < n; i++) {
        const Sub& s = g_subs[i];
        QueryPerformanceCounter(&a);
        ldr_enter(s.row, reason, base);
        if (reason == 1 && s.on_load) call_load(s, base, (uint32_t)d->SizeOfImage, name);
        else if (reason == 2 && s.on_unload) call_unload(s, base);
        ldr_leave();
        QueryPerformanceCounter(&b);
        double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        if (ms > slow_ms) { slow_ms = ms; slow = s.row; }
    }
    // Spell-start pause (U 12/10/2026, ~5 s at some casts): how long our callbacks hold the loader thread per load
    double total = (b.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    if (n > 0)
        log_line("magic_mods: %s %s: %ld callbacks %.2f ms (slowest %s %.2f ms), thread %lu", name,
                 reason == 1 ? "load" : "unload", n, total, slow, slow_ms, GetCurrentThreadId());
}

// Init thread (row install). Subscribers added after a load see only later loads.
bool magic_mods_subscribe(const char* row, magic_load_fn on_load, magic_unload_fn on_unload) {
    if (g_nsubs >= MAX_SUBS) { log_line("  %s: magic_mods: no free subscriber slot", row); return false; }
    if (!g_registered) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        MM_LdrRegisterDllNotification_t reg =
            nt ? (MM_LdrRegisterDllNotification_t)GetProcAddress(nt, "LdrRegisterDllNotification") : nullptr;
        PVOID cookie = nullptr;
        if (!reg || reg(0, dll_notif, nullptr, &cookie) != 0) {
            log_line("  %s: LdrRegisterDllNotification failed", row);
            return false;
        }
        g_registered = true;
    }
    g_subs[g_nsubs] = { row, on_load, on_unload };
    InterlockedIncrement(&g_nsubs);              // publish after the slot is written
    return true;
}
