// SPDX-License-Identifier: Apache-2.0
// Entry hooks through MinHook (third_party/minhook, BSD-2-Clause, v1.3.4): it relocates whole instructions,
// freezes the other threads while it writes the JMP, and chains over another mod's hook (Fahrenheit uses
// MinHook too). Single-CALL retargets stay on patch_call_rel32 (hooks.cpp).
#include "fps60.h"
#include "../../third_party/minhook/include/MinHook.h"

static volatile LONG g_mh_state = 0;   // 0 = not tried, 1 = ready, 2 = failed

bool hook_init() {
    if (InterlockedCompareExchange(&g_mh_state, 0, 0) == 0) {
        MH_STATUS s = MH_Initialize();
        // Already initialized = a concurrent first call won the race; MinHook is usable either way.
        bool ok = s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED;
        InterlockedCompareExchange(&g_mh_state, ok ? 1 : 2, 0);
        if (!ok) log_line("hook: MH_Initialize failed (%s) - no entry hook can install", MH_StatusToString(s));
    }
    return g_mh_state == 1;
}

bool hook_entry(const char* name, void* target, void* detour, void** orig) {
    if (!hook_init()) return false;
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s != MH_OK) {
        log_line("hook %s: MH_CreateHook at %p failed (%s)", name, target, MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        log_line("hook %s: MH_EnableHook at %p failed (%s)", name, target, MH_StatusToString(s));
        MH_RemoveHook(target);
        return false;
    }
    return true;
}

void hook_disable_all() {
    if (g_mh_state != 1) return;
    MH_STATUS s = MH_DisableHook(MH_ALL_HOOKS);
    log_line("hooks: all entry hooks disabled (%s)", MH_StatusToString(s));
}

bool hook_remove(const char* name, void* target) {
    if (g_mh_state != 1) return false;
    MH_STATUS s = MH_RemoveHook(target);
    if (s != MH_OK) {
        log_line("hook %s: MH_RemoveHook at %p failed (%s)", name, target, MH_StatusToString(s));
        return false;
    }
    return true;
}
