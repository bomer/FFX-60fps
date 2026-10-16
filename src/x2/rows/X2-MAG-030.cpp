// SPDX-License-Identifier: Apache-2.0
// X2-MAG-030 (FFX-2 only): the "T" magic overlays (71 magic_*.dll files, magic_0718 shape) run a record driver from
// a script task (service +0x60C -> task group 0x40, run by FUN_00680980 in Sg_MainLoop's second loop, so on every
// invocation). Each record starts (init callback, once) when its start frame <= the driver clock; the clock advances
// at the end of a call only when the exec getter +0x400 is non-zero. Stock exec is always 1, so a record starts on the
// tick after the clock reaches it, after that tick's overlay update re-polls the script flags (+0x420). At 60 Hz the
// render-only invocation right after a logic one starts it half a tick early, with flags polled before the script set
// them: magic_0718's init FUN_10001220 then finds no variant flag and writes through a null object (crash, D 16/10/2026
// 17:39:31, CoreDump.dmp: magic_0718_us.dll+0x1418 MOV [EAX+0x130],1, EAX=0, map 0x91).
// Fix: on render-only invocations the driver runs with its clock one tick back (then restored), so no record starts
// or ends there; records already running are called as before.
// Evidence: outer repo re/ledger/MAG-TASK.md (S); offline scan of magicFiles/FFX-2: 71 DLLs, 1 hit each, all checks.
#include "../x2.h"
#include "../../../third_party/minhook/include/MinHook.h"
#include <string.h>

namespace x2 {

extern Row row_MAG030;

// ---------------------------------------------------------------- signatures (magic_0718_us Ghidra VAs, base 0x10000000)
// Driver FUN_10003870: __fastcall (ecx passed on to +0x400), no stack args, plain RETs (+0x27, +0xFF, +0x10B).
static const char SIG_ENTRY[] =     // entry: exec getter +0x400 -> [exec], live count test, record loop setup
    "55 8B EC 51 A1 ?? ?? ?? ?? 8B 80 00 04 00 00 FF D0 83 3D ?? ?? ?? ?? 00 A3 ?? ?? ?? ?? 75 09 B8 01 00 00 00 "
    "8B E5 5D C3 53 56 33 DB 57 BE ?? ?? ?? ?? 39 1D ?? ?? ?? ?? 0F 8E";
static const char SIG_START[] =     // entry+0x44: record test, start != -1, CMP start,[clock] (operand +0x5E)
    "83 C7 0C 33 D2 89 55 FC 39 16 0F 84 ?? ?? ?? ?? 8B 47 F8 83 F8 FF 74 ?? 3B 05";
static const char SIG_TAIL[] =      // entry+0xE8: if ([exec]) INC [clock] (operand +0xF3); plain RET
    "83 3D ?? ?? ?? ?? 00 74 06 FF 05 ?? ?? ?? ?? 5F 5E 33 C0 5B 8B E5 5D C3";
enum { OFF_SVC = 5, OFF_EXEC = 0x19, OFF_START = 0x44, OFF_CLOCK = 0x5E, OFF_TAIL = 0xE8, OFF_TAIL_EXEC = 0xEA, OFF_TAIL_CLOCK = 0xF3 };

// ---------------------------------------------------------------- local scanner (DLL executable sections)
struct Pat { uint8_t b[64]; uint8_t m[64]; int len; };

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parse(const char* s, Pat* p) {
    p->len = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (p->len >= 64) return false;
        if (s[0] == '?' && s[1] == '?') { p->b[p->len] = 0; p->m[p->len] = 0; }
        else {
            int hi = hexv(s[0]), lo = hexv(s[1]);
            if (hi < 0 || lo < 0) return false;
            p->b[p->len] = (uint8_t)(hi * 16 + lo); p->m[p->len] = 1;
        }
        p->len++;
        s += 2;
    }
    return p->len > 0;
}

static bool match(const uint8_t* at, const Pat& p) {
    for (int i = 0; i < p.len; i++) if (p.m[i] && at[i] != p.b[i]) return false;
    return true;
}

// Hits of `pattern` in the module's executable sections (stops counting at 2); -1 = bad headers/pattern.
static int scan_mod(uint8_t* mod, uint32_t size, const char* pattern, uint8_t** first) {
    Pat p;
    *first = nullptr;
    if (!parse(pattern, &p)) return -1;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || (uint32_t)dos->e_lfanew > size - sizeof(IMAGE_NT_HEADERS32)) return -1;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int hits = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint32_t va = sec[i].VirtualAddress, vs = sec[i].Misc.VirtualSize;
        if (va >= size || vs > size - va) return -1;
        uint8_t* lo = mod + va;
        uint8_t* hi = lo + vs;
        for (uint8_t* a = lo; a + p.len <= hi; a++) {
            if (p.m[0] && a[0] != p.b[0]) continue;
            if (match(a, p)) { if (!hits) *first = a; if (++hits > 1) return hits; }
        }
    }
    return hits;
}

static bool match_str(const uint8_t* at, const char* pattern) {
    Pat p;
    return parse(pattern, &p) && match(at, p);
}

// ---------------------------------------------------------------- per-DLL slots
typedef uint32_t(__fastcall* driver_t)(void* ecx, void* edx);

enum { MAX_SLOTS = 8 };
struct Slot {
    uint8_t*          mod;
    uint8_t*          entry;
    volatile int32_t* clock;
    driver_t          orig;
    char              name[48];
};
static Slot          g_slots[MAX_SLOTS];
static volatile LONG g_held_calls = 0, g_fixups = 0;

static uint32_t driver_common(void* ecx, void* edx, int idx) {
    Slot& s = g_slots[idx];
    if (!row_MAG030.on || g_disabled || !render_only()) return s.orig(ecx, edx);
    InterlockedIncrement(&g_held_calls);
    int32_t c = *s.clock;
    *s.clock = c - 1;                        // start <= clock / end <= clock: as of the last logic call
    uint32_t r = s.orig(ecx, edx);           // exec is 0 here: the driver does not advance the clock
    if (*s.clock != c - 1) InterlockedIncrement(&g_fixups);
    *s.clock = c;
    return r;
}

template <int I> static uint32_t __fastcall h_driver(void* ecx, void* edx) { return driver_common(ecx, edx, I); }
static void* const DRIVERS[MAX_SLOTS] = { (void*)&h_driver<0>, (void*)&h_driver<1>, (void*)&h_driver<2>, (void*)&h_driver<3>,
                                          (void*)&h_driver<4>, (void*)&h_driver<5>, (void*)&h_driver<6>, (void*)&h_driver<7> };

// ---------------------------------------------------------------- load / unload (loader lock)
// The hooked prologue (9 bytes) holds an absolute operand (A1 <service table ptr>): it is hooked only once it points
// inside the loaded module (relocated), so MinHook's copy is final.
static bool in_mod(uint8_t* mod, uint32_t size, uint32_t v) { return v >= (uint32_t)mod && v - (uint32_t)mod < size; }

static void on_load(uint8_t* mod, uint32_t size, const char* name) {
    if (!row_MAG030.on || g_disabled) return;
    uint8_t* entry = nullptr;
    int h = scan_mod(mod, size, SIG_ENTRY, &entry);
    if (h == 0) return;                                      // not a T overlay (silent)
    if (h != 1) { log_line("X2-MAG-030: %s driver entry: %d hits - refused", name, h); return; }
    uint32_t off = (uint32_t)(entry - mod);
    auto u = [&](int o) { return *(const uint32_t*)(entry + o); };
    const char* why = nullptr;
    if (entry <= mod + 1 || (entry[-1] != 0xCC && entry[-1] != 0xC3)) why = "entry boundary";
    else if (!match_str(entry + OFF_START, SIG_START)) why = "start test";
    else if (!match_str(entry + OFF_TAIL, SIG_TAIL)) why = "tail (clock INC, plain RET)";
    else if (u(OFF_CLOCK) != u(OFF_TAIL_CLOCK) || u(OFF_EXEC) != u(OFF_TAIL_EXEC)) why = "clock/exec operands";
    else if (!in_mod(mod, size, u(OFF_SVC)) || !in_mod(mod, size, u(OFF_CLOCK))) why = "operands not relocated yet";
    if (why) { log_line("X2-MAG-030: %s driver at +0x%X: %s mismatch - refused", name, off, why); return; }

    int idx = -1;
    for (int i = 0; i < MAX_SLOTS; i++) if (!g_slots[i].mod) { idx = i; break; }
    if (idx < 0) { log_line("X2-MAG-030: %s - no free slot, refused", name); return; }
    Slot& s = g_slots[idx];
    s.entry = entry;
    s.clock = (volatile int32_t*)u(OFF_CLOCK);
    s.orig = nullptr;
    memcpy(s.name, name, sizeof(s.name));
    s.name[sizeof(s.name) - 1] = 0;
    if (!hook_entry("X2-MAG-030 driver", entry, DRIVERS[idx], (void**)&s.orig)) {
        log_line("X2-MAG-030: %s driver hook failed - refused", name);
        return;
    }
    s.mod = mod;
    log_line("X2-MAG-030: %s hooked: T record driver +0x%X (gva 0x%08X), clock +0x%X, slot %d", name, off,
             0x10000000u + off, (uint32_t)((uint8_t*)s.clock - mod), idx);
}

static void on_unload(uint8_t* mod) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        Slot& s = g_slots[i];
        if (s.mod != mod) continue;
        MH_STATUS a = MH_RemoveHook(s.entry);
        log_line("X2-MAG-030: %s unloaded, slot %d freed (unhook %s)", s.name, i, MH_StatusToString(a));
        s.mod = nullptr;
    }
}

static bool install() {
    if (!magic_mods_subscribe("X2-MAG-030", on_load, on_unload)) return false;
    log_line("  X2-MAG-030: magic overlay load hook registered (T record driver: no record start/end on render-only)");
    return true;
}

static void post() {
    static LONG reported = 0, fix_reported = 0;
    if (!reported && g_held_calls) { reported = 1; log_line("X2-MAG-030: first T driver call held a tick back on a render-only invocation"); }
    if (!fix_reported && g_fixups) { fix_reported = 1; log_line("X2-MAG-030: a render-only driver call changed its clock (restored)"); }
}

Row row_MAG030 = {
    "X2-MAG-030",
    "T magic overlays (magic_0718 shape): records do not start or end on render-only invocations (crash at init)",
    1, install, nullptr, post, 0
};

} // namespace x2
