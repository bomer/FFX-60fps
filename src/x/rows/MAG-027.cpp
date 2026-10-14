// SPDX-License-Identifier: Apache-2.0
// Event timeline walker (magic_0539 FUN_10002510 and 21 more DLLs, thiscall, plain RET), run from the DLL's draw:
// an event starts when the list clock equals its start tick, and the clock only advances when exec != 0
// (service +0x3F4). On a render-only invocation the clock has already moved to the next tick, so an event due
// there started in that draw and again on the logic draw. magic_0539's start fixes up a cached block's offsets in
// place (FUN_00711fa0 via +0xB1C); the second fixup doubles the pointers and faults at 0x712010.
// On render-only invocations, events whose start tick equals the clock are hidden from the walk (flag cleared and
// put back), so they start once, on the logic invocation; running events step as before.
// Details: docs/fps60/rows/MAG-027.md
#include "../fps60.h"
#include <string.h>
#include <utility>

// Entry up to LEA EBX,[EDI+0xC]: list +4 count, +8 active, +0xC flags (stride 8), +0x10C event table (stride 0x18)
static const char SIG[] = "55 8B EC 83 EC 08 57 8B F9 83 7F 08 00 75 07 33 C0 5F 8B E5 5D C3 83 7F 04 00 53 56 8B B7 0C 01 00 00 8D 5F 0C";
// Clock step at +0xAB: call [svc+0x3F4]; TEST; MOV EAX,1; JE +2; INC [EDI]
static bool clock_tail(const uint8_t* p) { return sig_match(p + 0xAB, "A1 ?? ?? ?? ?? 8B 80 F4 03 00 00 FF D0 85 C0 B8 01 00 00 00 74 02 FF 07"); }

typedef int(__fastcall* walk_t)(uint8_t* list, void* edx);

enum { MAX_MODS = 16, MAX_EVENTS = 32 };          // flags +0xC..+0x10C: 32 slots of 8
struct Mod {
    uint8_t* base; uint32_t size; char name[24];
    bool dead, seen;
    uint8_t* entry;
    walk_t orig;
};
static Mod g_mods[MAX_MODS];
static volatile uint8_t g_ro = 0;
static volatile LONG g_held = 0;

template <int I> static int __fastcall h_walk(uint8_t* list, void* edx) {
    if (!g_ro) return g_mods[I].orig(list, edx);
    int32_t* flags[MAX_EVENTS];
    int32_t saved[MAX_EVENTS];
    int n = 0;
    __try {
        int cnt = *(int32_t*)(list + 4);
        const uint8_t* ev = *(const uint8_t**)(list + 0x10C);
        int32_t clock = *(int32_t*)list;
        for (int i = 0; i < cnt && i < MAX_EVENTS; i++) {
            int32_t* f = (int32_t*)(list + 0xC + i * 8);
            if (*f && *(const int32_t*)(ev + i * 0x18) == clock) { saved[n] = *f; flags[n++] = f; *f = 0; }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    int r = g_mods[I].orig(list, edx);
    for (int i = 0; i < n; i++) *flags[i] = saved[i];
    if (n && InterlockedIncrement(&g_held) == 1)
        log_line("MAG-027: first event start held on a render-only invocation (%s)", g_mods[I].name);
    return r;
}
template <int... I> static void* const* detours(std::integer_sequence<int, I...>) {
    static void* const list[] = { (void*)&h_walk<I>... };
    return list;
}
static void* const* const DETOURS = detours(std::make_integer_sequence<int, MAX_MODS>{});

static void patch(int idx) {
    Mod& m = g_mods[idx];
    m.seen = true;
    int hits = 0;
    uint8_t* e = sig_scan_module(m.base, m.size, SIG, &hits, clock_tail);
    if (hits != 1) { if (hits > 1) log_line("MAG-027: %s has %d timeline walkers - not handled", m.name, hits); return; }
    if (!hook_entry("MAG-027", e, DETOURS[idx], (void**)&m.orig)) return;
    m.entry = e;
    log_line("MAG-027: %s timeline walker at gva 0x%08X: starts held on render-only invocations", m.name,
             0x10000000u + (uint32_t)(e - m.base));
}

// ---------------------------------------------------------------- module tracking (magic_mods.cpp, loader lock)
static void on_load(uint8_t* base, uint32_t size, const char* name) {
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (m.base || m.dead) continue;
        memset(&m, 0, sizeof(m));
        m.size = size;
        strncpy(m.name, name, sizeof(m.name) - 1);
        m.base = base;                           // publish last
        return;
    }
}

static void on_unload(uint8_t* base) {
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (m.base != base) continue;
        if (m.entry) hook_remove("MAG-027", m.entry);
        m.entry = nullptr;
        m.base = nullptr;
        m.dead = true;
    }
}

static void pre() {
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (m.dead) { memset(&m, 0, sizeof(m)); continue; }
        if (!m.base || m.seen) continue;
        __try { patch(i); }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("MAG-027: fault 0x%08lX patching %s - left alone", GetExceptionCode(), m.name); }
    }
    g_ro = (!g_disabled && pacing_kind() == INV_BUILD) ? 1 : 0;
}

static void post() { g_ro = 0; }

static bool install() {
    if (!magic_mods_subscribe("MAG-027", on_load, on_unload)) return false;
    log_line("  MAG-027: watching magic DLL loads for the event timeline walker (magic_0539 and 21 more)");
    return true;
}

Row row_MAG027 = {
    "MAG-027",
    "magic-DLL event timeline (magic_0539 FUN_10002510, 22 DLLs): events start only on logic invocations (double fixup crash)",
    1, install, pre, post, 0
};
