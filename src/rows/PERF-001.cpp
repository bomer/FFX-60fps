// SPDX-License-Identifier: Apache-2.0
// PERF-001 (default on): frame limiter timing on low-power PCs (handhelds). The host limiter SKS_WinMainLoop 0x62EE50
// waits with Sleep(ms) under timeBeginPeriod(5) (FUN_0042fe10, re/MAINLOOP.md section 2), so at 60 Hz every wait can
// overshoot by up to 5 ms. Windows 11 may also coarsen timers and lower CPU QoS for a process (power throttling,
// on battery). This row asks for a 1 ms timer and opts the process out of execution-speed and timer-resolution
// throttling. No game code is touched. H: a reported 30 fps cap on a ROG Ally (no log yet).
#include "../fps60.h"

#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

struct PowerThrottling { ULONG Version, ControlMask, StateMask; };
typedef BOOL(WINAPI* set_proc_info_t)(HANDLE, int, void*, DWORD);
typedef UINT(WINAPI* time_period_t)(UINT);

static time_period_t g_end_period = nullptr;
static bool g_period = false;

static bool install() {
    // ProcessPowerThrottling = 4 (Windows 8+; the timer flag is honoured from Windows 11)
    auto spi = (set_proc_info_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    if (spi) {
        PowerThrottling pt = { 1, PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
        BOOL ok = spi(GetCurrentProcess(), 4, &pt, sizeof(pt));
        if (!ok) {   // older Windows rejects the timer flag: retry with execution speed only
            pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            ok = spi(GetCurrentProcess(), 4, &pt, sizeof(pt));
        }
        log_line("  PERF-001: power throttling opt-out %s (mask 0x%X, err %lu)", ok ? "ok" : "failed", pt.ControlMask,
                 ok ? 0 : GetLastError());
    } else {
        log_line("  PERF-001: SetProcessInformation not available");
    }
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    auto begin = winmm ? (time_period_t)GetProcAddress(winmm, "timeBeginPeriod") : nullptr;
    g_end_period = winmm ? (time_period_t)GetProcAddress(winmm, "timeEndPeriod") : nullptr;
    if (begin && g_end_period) {
        g_period = begin(1) == 0;
        log_line("  PERF-001: timeBeginPeriod(1) %s", g_period ? "ok" : "failed");
    }
    return true;
}

// Dev hot reload: the next copy asks again; drop this copy's request so they do not stack.
static void stop() {
    if (g_period && g_end_period) g_end_period(1);
    g_period = false;
}

Row row_PERF001 = {
    "PERF-001",
    "frame limiter timing: 1 ms timer, no Windows power throttling (low-power PCs, handhelds)",
    1, install, nullptr, nullptr, 0, stop
};
