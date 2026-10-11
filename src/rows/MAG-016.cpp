// SPDX-License-Identifier: Apache-2.0
// MAG-016: the W/V magic overlays' PPP record driver runs from the draw, so record clocks (start delay, age,
// callbacks, sound timeline) run 2x at 60 fps. Render-only frames still draw, with those fields held.
// Details: docs/fps60/rows/MAG-016.md
#include "../fps60.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <utility>

typedef uint32_t(__fastcall* driver_t)(void* list, int param);

// ---------------------------------------------------------------- per-overlay slots
enum { MAX_SLOTS = 8 };
struct Slot {
    uint8_t* mod;             // DLL base (null = free)
    uint32_t size;
    uint8_t* entry;           // the driver entry in the DLL
    int      n;               // prologue bytes (sub esp imm32 = 9, imm8 = 6; identifies the shape)
    driver_t orig;            // MinHook trampoline
    uint8_t* head_abs;        // B: entry address holding the list head's address (read per call, relocated)
    uint8_t  is_0304;
    char     name[24];
    uint8_t** head_last;      // diag 4: list head seen by the last driver call
    uint32_t load_tick;
    uint8_t* seen[64];        // diag 2: records already reported as born (reset per load)
    int      nseen;
    int      diag_lines;
};
static Slot      g_slots[MAX_SLOTS];
static volatile LONG g_hold = 0;
// Hold parts (F11 cycles): 1 = age/delay, 2 = +0x104 callback, 4 = +0x100 trigger, 8 = +0x110 timeline.
static volatile LONG g_mask = 0xF;
static const LONG MASKS[] = { 0xF, 0x7, 0xB, 0xD, 0xE, 0x1 };
static int       g_diag = 0;
static uint8_t*  g_0304 = nullptr;     // live magic_0304 base, for the diag

// ---------------------------------------------------------------- diag counters (game thread)
static uint32_t g_logic_tick = 0;
static uint32_t g_calls = 0, g_held_calls = 0, g_held_recs = 0;

// ---------------------------------------------------------------- the wrapper
// PPP record (stride 0x130) and its list node. Field meanings: docs/fps60/rows/MAG-016.md.
struct PppRecord {
    uint8_t  _000[0x100];
    uint32_t cb_age;          // +0x100 callback, run when age == trigger_age
    uint32_t cb_call;         // +0x104 callback, run every call
    uint8_t  _108[4];
    uint32_t f10C;            // +0x10C (unproven: child callbacks)
    uint32_t se_timeline;     // +0x110 sound-effect timeline
    int32_t  delay;           // +0x114 start delay, counts down before the age runs
    int32_t  age;             // +0x118
    int32_t  trigger_age;     // +0x11C
    uint8_t  _120[4];
    uint16_t flags;           // +0x124, 0x8000 = live
    uint8_t  b126;            // +0x126 (unproven)
};
static_assert(offsetof(PppRecord, cb_age) == 0x100 && offsetof(PppRecord, f10C) == 0x10C &&
              offsetof(PppRecord, se_timeline) == 0x110 && offsetof(PppRecord, trigger_age) == 0x11C &&
              offsetof(PppRecord, flags) == 0x124 && offsetof(PppRecord, b126) == 0x126, "PppRecord layout");
static PppRecord& R(uint8_t* rec) { return *(PppRecord*)rec; }
static uint8_t* node_next(uint8_t* node) { return *(uint8_t**)(node + 4); }
static uint8_t* node_rec(uint8_t* node) { return *(uint8_t**)(node + 8); }

struct Saved { uint8_t* rec; uint32_t cb_age, cb_call, se_timeline; int32_t delay, age; };
enum { MAX_RECS = 128 };

static const char* kind_name() {
    static const char* const K[] = { "LOGIC", "BUILD", "BACKLOG", "STOCK" };
    int k = pacing_kind();
    return (k >= 0 && k <= 3) ? K[k] : "?";
}

static uint32_t mod_off(const Slot& sl, uint32_t p) {
    return (p >= (uint32_t)(uintptr_t)sl.mod && p < (uint32_t)(uintptr_t)sl.mod + sl.size) ? p - (uint32_t)(uintptr_t)sl.mod : p;
}

// Diag 2: record lifecycle (born = first seen in a list, killed = live bit 0x8000 cleared by this call).
struct Life { uint8_t* rec; uint16_t flags; int32_t age, delay; };
static uint32_t diag_life_call(Slot& sl, uint8_t** head, void* list, int param, bool held, uint32_t (*call)(Slot&, uint8_t**, void*, int)) {
    Life lv[MAX_RECS];
    int n = 0;
    for (uint8_t* node = *head; node && n < MAX_RECS; node = node_next(node)) {
        uint8_t* rec = node_rec(node);
        if (!rec) continue;
        lv[n].rec = rec; lv[n].flags = R(rec).flags;
        lv[n].age = R(rec).age; lv[n].delay = R(rec).delay;
        n++;
        bool known = false;
        for (int i = 0; i < sl.nseen; i++) if (sl.seen[i] == rec) { known = true; break; }
        if (!known && sl.diag_lines < 3000) {
            if (sl.nseen < 64) sl.seen[sl.nseen++] = rec;
            sl.diag_lines++;
            log_line("MAG-016 life: %s %-5s tick=%u hold=%ld BORN rec=+0x%X flags=%04X age=%d delay=%d trig=%d cb100=+0x%X cb104=+0x%X se110=0x%X cb10C=0x%X kind126=%u",
                     sl.name, kind_name(), g_logic_tick, g_hold, mod_off(sl, (uint32_t)(uintptr_t)rec), lv[n - 1].flags,
                     lv[n - 1].age, lv[n - 1].delay, R(rec).trigger_age,
                     mod_off(sl, R(rec).cb_age), mod_off(sl, R(rec).cb_call),
                     R(rec).se_timeline, R(rec).f10C, R(rec).b126);
        }
    }
    uint32_t r = call(sl, head, list, param);
    for (int i = 0; i < n; i++) {
        uint16_t f = R(lv[i].rec).flags;
        if ((lv[i].flags & 0x8000) && !(f & 0x8000) && sl.diag_lines < 3000) {
            sl.diag_lines++;
            log_line("MAG-016 life: %s %-5s tick=%u hold=%ld KILL rec=+0x%X flags %04X->%04X age=%d delay=%d (%s)%s",
                     sl.name, kind_name(), g_logic_tick, g_hold, mod_off(sl, (uint32_t)(uintptr_t)lv[i].rec), lv[i].flags, f,
                     lv[i].age, lv[i].delay, (lv[i].flags & 0x4000) ? "0x4000 kill request" : "program ended or callback",
                     held ? " [held call]" : "");
        }
        if ((lv[i].flags & 0x8000) && !(f & 0x8000))       // pool slot may be reused: report its next birth
            for (int k = 0; k < sl.nseen; k++) if (sl.seen[k] == lv[i].rec) { sl.seen[k] = sl.seen[--sl.nseen]; break; }
    }
    return r;
}

static uint32_t call_plain(Slot& sl, uint8_t**, void* list, int param) {
    return sl.orig(list, param);
}

static uint32_t call_held(Slot& sl, uint8_t** head, void* list, int param);

// Variant B ignores ecx/edx and its eax result is unused; passing them through is harmless.
static uint32_t mag016_common(void* list, int param, uint32_t slot_idx) {
    Slot& sl = g_slots[slot_idx];
    g_calls++;
    uint8_t** head = sl.head_abs ? *(uint8_t***)sl.head_abs : (uint8_t**)((uint8_t*)list + 0x30);
    sl.head_last = head;
    bool held = g_hold && row_MAG016.on && !g_disabled && pacing_kind() == INV_BUILD;
    if (g_diag & 2) return diag_life_call(sl, head, list, param, held, held ? call_held : call_plain);
    return held ? call_held(sl, head, list, param) : call_plain(sl, head, list, param);
}

static uint32_t call_held(Slot& sl, uint8_t** head, void* list, int param) {
    driver_t orig = sl.orig;
    LONG mask = g_mask;
    Saved sv[MAX_RECS];
    int n = 0;
    for (uint8_t* node = *head; node && n < MAX_RECS; node = node_next(node)) {
        uint8_t* rec = node_rec(node);
        if (!rec) continue;
        Saved& s = sv[n++];
        s.rec  = rec;
        s.cb_age = R(rec).cb_age;
        s.cb_call = R(rec).cb_call;
        s.se_timeline = R(rec).se_timeline;
        s.delay = R(rec).delay;
        s.age = R(rec).age;
        if (mask & 4) R(rec).cb_age = 0;
        if (mask & 2) R(rec).cb_call = 0;
        if (mask & 8) R(rec).se_timeline = 0;
    }
    uint32_t r = orig(list, param);
    for (int i = 0; i < n; i++) {
        uint8_t* rec = sv[i].rec;
        if (!(R(rec).flags & 0x8000)) continue;    // killed in this call: leave it
        if (mask & 4) R(rec).cb_age = sv[i].cb_age;
        if (mask & 2) R(rec).cb_call = sv[i].cb_call;
        if (mask & 8) R(rec).se_timeline = sv[i].se_timeline;
        if (mask & 1) {
            R(rec).delay = sv[i].delay;
            R(rec).age = sv[i].age;
        }
    }
    g_held_calls++;
    g_held_recs += n;
    return r;
}

// ---------------------------------------------------------------- per-load install
// Driver body: a live record either ages (+0x118) or counts down its start delay (+0x114)
static const char SIG[] = "FF 86 18 01 00 00 EB ?? 48 89 86 14 01 00 00";

// The driver's inner signature, if it occurs exactly once in the module.
static uint8_t* find_inner(uint8_t* mod, uint32_t size, int* hits) {
    uint8_t* found = sig_scan_module(mod, size, SIG, hits);
    return *hits == 1 ? found : nullptr;
}

// One detour per slot (the driver is fastcall(list, param), plain RET).
template <int I> static uint32_t __fastcall h_driver(void* list, int param) { return mag016_common(list, param, I); }
template <int... I> static void* const* detours(std::integer_sequence<int, I...>) {
    static void* const list[] = { (void*)&h_driver<I>... };
    return list;
}
static void* const* const DETOURS = detours(std::make_integer_sequence<int, MAX_SLOTS>{});

// Nearest `(CC|C3) 55 8B EC (81|83) EC` before the inner signature, then the variant check.
// Returns the entry; *n = relocatable prologue length, *head_abs = B's head operand (else null).
static uint8_t* find_entry(uint8_t* mod, uint8_t* inner, int* n, uint8_t** head_abs) {
    for (uint8_t* p = inner - 4; p > inner - 0x800 && p > mod + 0x1000; p--) {
        if ((p[-1] != 0xCC && p[-1] != 0xC3) || p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC || p[4] != 0xEC) continue;
        if (p[3] == 0x81) *n = 9;                          // sub esp, imm32
        else if (p[3] == 0x83) *n = 6;                     // sub esp, imm8
        else return nullptr;
        *head_abs = nullptr;
        for (int k = *n; k < 0x20; k++)                    // A: mov eax,[ecx+0x30]
            if (p[k] == 0x8B && p[k + 1] == 0x41 && p[k + 2] == 0x30) return p;
        for (int k = *n; k < 0x20; k++) {                  // B: after the stack cookie, load the global list head
            if (!sig_match(p + k, "33 C5 89 45 FC A1")) continue;
            uint32_t h = *(uint32_t*)(p + k + 6);
            for (int j = k + 10; j < k + 0x20; j++)        // ... mov [head+8],eax (the cursor)
                if (p[j] == 0xA3 && *(uint32_t*)(p + j + 1) == h + 8) { *head_abs = p + k + 6; return p; }
            return nullptr;
        }
        return nullptr;
    }
    return nullptr;
}

// magic_mods.cpp, loader lock. The prologue has no absolute operand, so it can be hooked before relocation.
static void on_load(uint8_t* mod, uint32_t size, const char* name) {
    if (!row_MAG016.on || g_disabled) return;
    ldr_step(1);
    log_line("MAG-016: %s loaded at tick %u", name, g_logic_tick);
    ldr_step(2);
    bool is_0304 = _stricmp(name, "magic_0304.dll") == 0;
    if (is_0304) g_0304 = mod;

    int hits = 0;
    uint8_t* inner = find_inner(mod, size, &hits);
    if (!inner) {
        if (hits > 1) log_line("MAG-016: %s has %d driver signatures - not hooked", name, hits);
        return;                                              // no driver in this overlay
    }
    int n = 0;
    uint8_t* head_abs = nullptr;
    ldr_step(3);
    uint8_t* entry = find_entry(mod, inner, &n, &head_abs);
    if (!entry) {
        log_line("MAG-016: %s driver signature at +0x%X, entry not recognised - not hooked", name, (uint32_t)(inner - mod));
        return;
    }
    int idx = -1;
    for (int i = 0; i < MAX_SLOTS; i++) if (!g_slots[i].mod) { idx = i; break; }
    if (idx < 0) { log_line("MAG-016: %s - no free slot, not hooked", name); return; }
    Slot& s = g_slots[idx];
    s.entry = entry; s.n = n; s.size = size; s.head_abs = head_abs; s.is_0304 = is_0304 ? 1 : 0;
    s.nseen = 0; s.diag_lines = 0; s.head_last = nullptr; s.load_tick = g_logic_tick;
    memcpy(s.name, name, sizeof(s.name));
    ldr_step(5);
    if (!hook_entry("MAG-016", entry, DETOURS[idx], (void**)&s.orig)) return;
    s.mod = mod;
    ldr_step(6);
    log_line("MAG-016: %s driver hooked: variant %c, entry +0x%X (gva 0x%08X), prologue %d bytes, slot %d, hold=%ld mask=0x%lX, tick %u",
             name, head_abs ? 'B' : 'A', (uint32_t)(entry - mod), 0x10000000u + (uint32_t)(entry - mod), n, idx, g_hold, g_mask, g_logic_tick);
}

static void on_unload(uint8_t* mod) {
    if (mod == g_0304) g_0304 = nullptr;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_slots[i].mod == mod) {
            log_line("MAG-016: %s unloaded, slot %d freed, tick %u", g_slots[i].name, i, g_logic_tick);
            hook_remove("MAG-016", g_slots[i].entry);
            g_slots[i].mod = nullptr;
        }
}

static bool install() {
    g_hold = config_row("MAG-016-HOLD", 1) ? 1 : 0;
    g_diag = config_row("MAG-016-DIAG", 0);
    if (!magic_mods_subscribe("MAG-016", on_load, on_unload)) return false;
    log_line("  MAG-016: overlay load hook registered (sig %s), hold=%ld (F10 toggles), diag=%d", SIG, g_hold, g_diag);
    return true;
}

// ---------------------------------------------------------------- F10 toggle + diag
static void pre() {
    static bool down_prev = false;
    bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (down && !down_prev) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            LONG v = g_hold ? 0 : 1;
            InterlockedExchange(&g_hold, v);
            log_line("MAG-016: F10 -> hold=%ld (logic tick %u)", v, g_logic_tick);
        }
    }
    down_prev = down;
    static bool f11_prev = false;
    bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f11 && !f11_prev) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            int k = 0;
            for (int i = 0; i < (int)(sizeof(MASKS) / sizeof(MASKS[0])); i++) if (MASKS[i] == g_mask) { k = i; break; }
            LONG m = MASKS[(k + 1) % (sizeof(MASKS) / sizeof(MASKS[0]))];
            InterlockedExchange(&g_mask, m);
            log_line("MAG-016: F11 -> mask=0x%lX (1 age/delay, 2 cb104, 4 cb100, 8 timeline110; logic tick %u)", m, g_logic_tick);
        }
    }
    f11_prev = f11;
    if (pacing_kind() != INV_BUILD) g_logic_tick++;
    g_calls = g_held_calls = g_held_recs = 0;
}

// Diag 4: per logic invocation, every record of each hooked overlay's last driver list (stock vs paced diff).
static void dump_records() {
    for (int i = 0; i < MAX_SLOTS; i++) {
        Slot& sl = g_slots[i];
        if (!sl.mod || !sl.head_last || sl.diag_lines >= 6000) continue;
        int n = 0;
        for (uint8_t* node = *sl.head_last; node && n < 32; node = node_next(node), n++) {
            uint8_t* r = node_rec(node);
            if (!r) continue;
            sl.diag_lines++;
            log_line("MAG-016 st: %s t=+%u %s rec=+0x%X f=%04X a=%d d=%d clk=%d/%d stp=%d end=%d p=(%.2f,%.2f,%.2f,%.2f) "
                     "s=(%.3f,%.3f,%.3f) k=%u w=%08X %08X %08X %08X %08X %08X %08X %08X",
                     sl.name, g_logic_tick - sl.load_tick, kind_name(), mod_off(sl, (uint32_t)(uintptr_t)r),
                     *(uint16_t*)(r + 0x124), *(int32_t*)(r + 0x118), *(int32_t*)(r + 0x114),
                     *(int32_t*)(r + 8), *(int32_t*)(r + 0xC), *(int32_t*)(r + 0x10), *(int32_t*)(r + 4),
                     *(float*)(r + 0xD0), *(float*)(r + 0xD4), *(float*)(r + 0xD8), *(float*)(r + 0xDC),
                     *(float*)(r + 0x60), *(float*)(r + 0x64), *(float*)(r + 0x68), *(uint8_t*)(r + 0x126),
                     *(uint32_t*)(r + 0x14), *(uint32_t*)(r + 0x18), *(uint32_t*)(r + 0x1C), *(uint32_t*)(r + 0x20),
                     *(uint32_t*)(r + 0x24), *(uint32_t*)(r + 0x28), *(uint32_t*)(r + 0x2C), *(uint32_t*)(r + 0x30));
        }
    }
}

static void post() {
    if ((g_diag & 4) && pacing_kind() != INV_BUILD) {
        __try { dump_records(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_diag &= ~4; log_line("MAG-016 diag 4: read fault - state dump off"); }
    }
    static int lines = 0;
    static uint8_t* last_mod = nullptr;
    if (!(g_diag & 1) || !g_0304) return;           // diag 1: magic_0304 fields (offsets in the doc)
    if (g_0304 != last_mod) { last_mod = g_0304; lines = 0; }
    if (lines >= 4000) return;
    __try {
        uint8_t* b = g_0304;
        uint8_t* ctx = *(uint8_t**)(b + 0x34B8EC);
        uint32_t head = *(uint32_t*)(b + 0x18D0B8 + 0x30);
        if (!ctx || !head) return;
        int kind = pacing_kind();
        static const char* const K[] = { "LOGIC", "BUILD", "BACKLOG", "STOCK" };
        float y = *(float*)(ctx + 0xD4), v = *(float*)(ctx + 0x1720);
        log_line("MAG-016 diag: %-7s tick=%u hold=%ld rec0 age=%d delay=%d y=%.3f v=%.4f end=%u arg358=0x%08X calls=%u held=%u/%u",
                 (kind >= 0 && kind <= 3) ? K[kind] : "?", g_logic_tick, g_hold,
                 *(int32_t*)(ctx + 0x118), *(int32_t*)(ctx + 0x114), y, v, *(uint32_t*)(ctx + 0x1728),
                 *(uint32_t*)(b + 0x18D158), g_calls, g_held_calls, g_held_recs);
        lines++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { lines = 4000; log_line("MAG-016 diag: read fault, diag stopped for this load"); }
}

Row row_MAG016 = {
    "MAG-016",
    "W/V PPP record clocks (delay, age, callbacks, SE timeline) held on render-only invocations",
    1, install, pre, post, 0
};
