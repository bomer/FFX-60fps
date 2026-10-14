// SPDX-License-Identifier: Apache-2.0
// MAG-019: paused particle draws (more frequent at 60 fps) reach a VFX sub-instance whose materials are not
// set yet, and its shader lookup crashes on strstr(NULL). The setup is skipped until the materials exist.
// Details: docs/fps60/rows/MAG-019.md
#include "../fps60.h"
#include <string.h>

typedef int(__fastcall* setup_t)(void* self, void* edx, int a, int b);

static uint8_t* g_entry = nullptr;
static setup_t  g_tramp = nullptr;
static bool     g_patched = false;
static volatile LONG g_trips[4] = {};          // per InvKind
static volatile LONG g_logged = 0;

// thiscall(this, a, b) taken as fastcall; RET 8 on both paths. 0 = "nothing to set up" for the caller.
static int __fastcall h_setup(void* self, void* edx, int a, int b) {
    if (!g_disabled) {
        uint32_t data = 0, count = 0, mat = 0, name = 0;
        bool ok = safe_read_u32((uint8_t*)self + 0x24, &data) && data &&
                  safe_read_u32((const void*)(uintptr_t)(data + 0x90), &count) && (int32_t)count >= 1 &&
                  safe_read_u32((const void*)(uintptr_t)(data + 0x94), &mat) && mat &&
                  safe_read_u32((const void*)(uintptr_t)(mat + 0x20), &name) && name;
        if (!ok) {
            int k = pacing_kind();
            if (k >= 0 && k < 4) InterlockedIncrement(&g_trips[k]);
            if (InterlockedIncrement(&g_logged) <= 20) {
                static const char* const K[] = { "LOGIC", "BUILD", "BACKLOG", "STOCK" };
                log_line("MAG-019: unset materials skipped (%s): this=%p data=%08X count=%d mat=%08X name=%08X a=%d b=%d",
                         (k >= 0 && k < 4) ? K[k] : "?", self, data, (int32_t)count, mat, name, a, b);
            }
            return 0;
        }
    }
    return g_tramp(self, edx, a, b);
}

static bool install() {
    // VFX shader setup FUN_006a48d0: prologue up to the strstr(material name, "GL/") call
    g_entry = sig_find_unique("MAG-019 VFX shader setup FUN_006a48d0",
        "55 8B EC 81 EC 8C 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 53 8B 5D 0C 56 57 8B F9 68 ?? ?? ?? ?? "
        "8B 47 3C 89 85 74 FF FF FF 8B 47 24 8B 80 94 00 00 00 FF 70 20 FF 15", 0x6A48D0);
    if (!g_entry) return false;
    const char* needle = (const char*)read_abs(g_entry + 0x1C);
    uint32_t w = 0;
    if (!safe_read_u32(needle, &w) || memcmp(&w, "GL/", 4) != 0) {
        log_line("  MAG-019: pushed string at %p is not \"GL/\" - refused", needle);
        return false;
    }
    log_line("  MAG-019: FUN_006a48d0 at gva=0x%08X (entry hooked on the game thread)", rt_to_gva_exe(g_entry));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("MAG-019", g_entry, (void*)&h_setup, (void**)&g_tramp)) {
        log_line("MAG-019: entry hook failed - row off");
        row_MAG019.on = 0;
        return;
    }
    log_line("MAG-019: FUN_006a48d0 entry hooked on the game thread (tramp %p)", (void*)g_tramp);
}

static void post() {
    static uint32_t tick = 0;
    static LONG seen = 0;
    LONG total = g_trips[0] + g_trips[1] + g_trips[2] + g_trips[3];
    if (++tick % 600 == 0 && total != seen) {
        seen = total;
        log_line("MAG-019: unset-material setups skipped so far: LOGIC %ld BUILD %ld BACKLOG %ld STOCK %ld",
                 g_trips[0], g_trips[1], g_trips[2], g_trips[3]);
    }
}

Row row_MAG019 = {
    "MAG-019",
    "VFX shader setup FUN_006a48d0: skipped while the VFX materials are unset (paused-draw crash, Thunder/Thundara)",
    1, install, pre, post, 0
};
