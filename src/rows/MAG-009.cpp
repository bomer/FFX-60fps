// SPDX-License-Identifier: Apache-2.0
// Magic DLLs time their effects off the global frame counter, which counts every invocation and so runs
// at 2x. This row gives them a counter that only advances on logic ticks, through their service table.
// Details: docs/fps60/rows/MAG-009.md
#include "../fps60.h"
#include <string.h>

typedef void* (__cdecl* fc_getter_t)(int32_t* out);

static volatile int32_t g_logic_fc = 0;
static uint32_t   g_fc_before = 0;
static fc_getter_t g_getter_orig = nullptr;

static void* __cdecl h_fc_getter(int32_t* out) {
    void* r = g_getter_orig(out);
    if (out && row_MAG009.on && !g_disabled) *out = g_logic_fc;
    return r;
}

static bool install() {
    if (!g_frame_counter) { log_line("  MAG-009: frame counter not resolved - refused"); return false; }
    // MagicFile_Start: pushes the service table it hands to each magic DLL
    uint8_t* at = sig_find_unique("MAG-009 MagicFile_Start table push",
        "0F 94 C0 50 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? FF 35", 0x9DA816);
    if (!at) return false;
    uint8_t* table = (uint8_t*)read_abs(at + 0x0A);
    uint32_t* slot_ptr = (uint32_t*)(table + 0xBF0);
    uint32_t* slot_get = (uint32_t*)(table + 0x3D4);
    if (*slot_ptr != (uint32_t)(uintptr_t)g_frame_counter) {
        log_line("  MAG-009: table+0xBF0 = 0x%08X, not the frame counter %p - refused", *slot_ptr, g_frame_counter);
        return false;
    }
    uint8_t* getter = (uint8_t*)(uintptr_t)*slot_get;
    // the frame-counter getter: if (out) *out = counter
    if (!sig_match(getter, "55 8B EC 8B 4D 08 85 C9 74 07 A1") || read_abs(getter + 11) != (uint8_t*)g_frame_counter) {
        log_line("  MAG-009: table+0x3D4 = %p is not the frame-counter getter - refused", getter);
        return false;
    }
    g_getter_orig = (fc_getter_t)getter;
    g_logic_fc = (int32_t)*g_frame_counter;

    DWORD old;
    if (!VirtualProtect(table + 0x3D4, 0xBF0 - 0x3D4 + 4, PAGE_READWRITE, &old)) return false;
    InterlockedExchange((volatile LONG*)slot_ptr, (LONG)(uintptr_t)&g_logic_fc);
    InterlockedExchange((volatile LONG*)slot_get, (LONG)(uintptr_t)&h_fc_getter);
    DWORD tmp;
    VirtualProtect(table + 0x3D4, 0xBF0 - 0x3D4 + 4, old, &tmp);
    log_line("  MAG-009: service table gva=0x%08X: +0xBF0 -> logic counter %p, +0x3D4 getter gva=0x%08X -> stub",
             rt_to_gva_exe(table), (void*)&g_logic_fc, rt_to_gva_exe(getter));
    return true;
}

static void pre() { g_fc_before = *g_frame_counter; }

static void post() {
    uint32_t d = *g_frame_counter - g_fc_before;
    if (pacing_kind() != INV_BUILD && d < 16) g_logic_fc += (int32_t)d;
}

Row row_MAG009 = {
    "MAG-009",
    "magic DLLs read a logic-rate frame counter (service table +0xBF0 pointer, +0x3D4 getter)",
    1, install, pre, post, 0
};
