// SPDX-License-Identifier: Apache-2.0
// Signature scan + byte verification: every hook target and global is found at runtime in FFX.exe's code.
// Each pattern must match exactly once; a failed required signature means nothing gets installed.
// Details: docs/fps60/sigs.md
#include "fps60.h"
#include <string.h>
#include <stdio.h>

set_exec_t    g_set_exec = nullptr;
get_exec_t    g_get_exec = nullptr;
set_divider_t g_set_divider_orig = nullptr;
void*         g_sg_mainloop = nullptr;
uint8_t*      g_mainloop_callsite = nullptr;
volatile int8_t*   g_exec_byte = nullptr;
volatile uint32_t* g_divider = nullptr;
volatile int32_t*  g_backlog_count = nullptr;
volatile int32_t*  g_skip_render = nullptr;
volatile uint32_t* g_frame_counter = nullptr;
volatile uint32_t* g_vblank = nullptr;

static const uint32_t GHIDRA_EXE_BASE = 0x400000;

uint8_t* exe_base() { return (uint8_t*)GetModuleHandleW(nullptr); }

static IMAGE_NT_HEADERS32* nt_headers(uint8_t* base) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    return nt;
}

uint32_t exe_size() {
    IMAGE_NT_HEADERS32* nt = nt_headers(exe_base());
    return nt ? nt->OptionalHeader.SizeOfImage : 0;
}

uint32_t rt_to_gva_exe(const void* rt) {
    return (uint32_t)((const uint8_t*)rt - exe_base()) + GHIDRA_EXE_BASE;
}

// ---------------------------------------------------------------- pattern scanning
struct Pattern {
    uint8_t bytes[128];   // tools/sigcheck.py MAX_PATTERN must match
    uint8_t mask[128];    // 1 = must match
    int     len;
};

static bool parse_pattern(const char* s, Pattern* p) {
    p->len = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (p->len >= (int)sizeof(p->bytes)) return false;
        if (s[0] == '?') {
            p->bytes[p->len] = 0; p->mask[p->len] = 0;
            s += (s[1] == '?') ? 2 : 1;
        } else {
            unsigned v = 0;
            for (int i = 0; i < 2; i++) {
                char c = s[i];
                v <<= 4;
                if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
                else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                else return false;
            }
            p->bytes[p->len] = (uint8_t)v; p->mask[p->len] = 1;
            s += 2;
        }
        p->len++;
    }
    return p->len > 0 && p->mask[0];
}

static bool match_at(const uint8_t* at, const Pattern* p) {
    for (int i = 0; i < p->len; i++) {
        if (p->mask[i] && at[i] != p->bytes[i]) return false;
    }
    return true;
}

// match_at from byte `from` on (the bytes before it were overwritten by another mod's hook).
static bool match_at_from(const uint8_t* at, const Pattern* p, int from) {
    for (int i = from; i < p->len; i++) {
        if (p->mask[i] && at[i] != p->bytes[i]) return false;
    }
    return true;
}

// Scans every executable section of a mapped image (size = SizeOfImage, 0 = trust the headers).
// Returns the match count (stops after 8); *first = first match. `accept` filters candidates.
static int scan_image(uint8_t* base, uint32_t size, const Pattern* p, uint8_t** first, bool (*accept)(const uint8_t*)) {
    IMAGE_NT_HEADERS32* nt = nt_headers(base);
    if (!nt) return 0;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int hits = 0;
    *first = nullptr;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* s = base + sec[i].VirtualAddress;
        uint32_t n = sec[i].Misc.VirtualSize;
        if (n < (uint32_t)p->len || (size && sec[i].VirtualAddress + n > size)) continue;
        uint8_t* end = s + n - p->len + 1;
        uint8_t* q = s;
        while (q < end) {
            q = (uint8_t*)memchr(q, p->bytes[0], (size_t)(end - q));
            if (!q) break;
            if (match_at(q, p) && (!accept || accept(q))) {
                if (!hits) *first = q;
                hits++;
                if (hits > 8) return hits;      // hopeless, stop early
            }
            q++;
        }
    }
    return hits;
}

static int scan_text(const Pattern* p, uint8_t** first) { return scan_image(exe_base(), 0, p, first, nullptr); }

// Rows: scan a loaded module (magic overlays). No logging; *hits = match count (capped at 9).
uint8_t* sig_scan_module(uint8_t* base, uint32_t size, const char* pattern, int* hits, bool (*accept)(const uint8_t*)) {
    Pattern p;
    uint8_t* first = nullptr;
    *hits = 0;
    if (!parse_pattern(pattern, &p)) { log_line("  BAD PATTERN (bug): %s", pattern); return nullptr; }
    *hits = scan_image(base, size, &p, &first, accept);
    return first;
}

// Rows: verify the bytes at a resolved address (a call target, a fixed RVA in a known DLL build).
bool sig_match(const void* at, const char* pattern) {
    Pattern p;
    if (!at || !parse_pattern(pattern, &p)) return false;
    __try { return match_at((const uint8_t*)at, &p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------- signature table
enum {
    SIG_RUNNER, SIG_MAINLOOP, SIG_SET_EXEC, SIG_GET_EXEC, SIG_SET_DIVIDER, SIG_BACKLOG,
    SIG_ACTORS, SIG_FXSCENE, SIG_FRAMECTR, SIG_VBLANK, SIG_CAMTWEEN, SIG_CAMBLOCK, SIG_MINIMAP, SIG_FXCTX,
    SIG_COUNT
};

struct SigDef {
    const char* name;
    const char* pattern;
    uint32_t    expected_gva;
    bool        required;
    uint8_t*    found;
};

static SigDef g_sigs[SIG_COUNT] = {
    // Runs Sg_MainLoop(delta) in a loop while the backlog count is non-zero
    { "Sg_MainLoopBacklogRunner",
      "55 8B EC 83 3D ?? ?? ?? ?? 00 74 05 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
      "D9 45 08 51 D9 1C 24 E8 ?? ?? ?? ?? 83 C4 04 83 3D ?? ?? ?? ?? 00 75 CB",
      0x822840, true, nullptr },
    // Main loop prologue (checked at the runner's CALL target)
    { "Sg_MainLoop",   // verified AT the runner's call target, not scanned
      "55 8B EC 81 EC A4 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC D9 45 08 51 D9 1C 24 E8",
      0x820AE0, true, nullptr },
    // Stores the exec byte, clamped to 0x63
    { "Sg_SetExecFrames",
      "55 8B EC 8B 4D 08 83 F9 63 7E 09 C6 05 ?? ?? ?? ?? 63 5D C3 33 C0 85 C9 0F 98 C0 48 23 C1 "
      "A2 ?? ?? ?? ?? 5D C3",
      0x82DA60, true, nullptr },
    // Returns the exec byte (or its override)
    { "Sg_GetCurExecFrames",
      "80 3D ?? ?? ?? ?? 00 56 0F BE 35 ?? ?? ?? ?? 75 07 0F BE 35 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0B C6 5E C3",
      0x82D7E0, true, nullptr },
    // Stores the frame divider (1 or 2)
    { "SKS_SetFrameDivider",
      "55 8B EC 8B 45 08 83 F8 02 73 11 83 F8 01 73 11 C7 05 ?? ?? ?? ?? 01 00 00 00 5D C3 "
      "B8 02 00 00 00 A3 ?? ?? ?? ?? 5D C3",
      0x625090, false, nullptr },   // only alternate mode needs it
    // Sg_MainLoop: if backlog > 1, set the three skip-render flags and count down
    { "Sg_MainLoop backlog/skip consumer",
      "A1 ?? ?? ?? ?? 83 F8 01 7E 26 48 C7 05 ?? ?? ?? ?? 01 00 00 00 C7 05 ?? ?? ?? ?? 01 00 00 00 "
      "C7 05 ?? ?? ?? ?? 01 00 00 00 A3",
      0x820BA8, true, nullptr },
    // Walks the actor table clearing flag bits at actor+0x194
    { "actor flag clear FUN_00832db0",
      "8B 15 ?? ?? ?? ?? 56 8B 35 ?? ?? ?? ?? 8B CE 69 C9 80 08 00 00 8B C2 03 CA 3B C1 74 ?? "
      "8D 49 00 80 78 02 00 74 ?? 81 A0 94 01 00 00 FF FF FF F8",
      0x832DB0, false, nullptr },
    // Returns an fx scene slot from the two scene arrays
    { "fx scene slot FUN_00794020",
      "55 8B EC 8B 45 08 25 FF 00 00 00 83 F8 1F 7D 0E 69 C0 90 0F 00 00 03 05 ?? ?? ?? ?? 5D C3 "
      "83 F8 5D 7D 11 83 C0 E1 69 C0 90 03 00 00 03 05 ?? ?? ?? ??",
      0x794020, false, nullptr },
    // Sg_MainLoop tail: increments the frame counter
    { "frame counter (Sg_MainLoop tail)",
      "A1 ?? ?? ?? ?? 40 50 68 ?? ?? ?? ?? A3 ?? ?? ?? ?? E8 ?? ?? ?? ?? 68 80 00 00 00 6A 01 6A 00",
      0x821617, false, nullptr },
    // Sg_WaitFrameSync: adds 2 to both vblank counters
    { "vblank (Sg_WaitFrameSync)",
      "85 C0 74 0E 83 05 ?? ?? ?? ?? 02 83 05 ?? ?? ?? ?? 02",
      0x82220D, false, nullptr },
    // Indexes the per-viewport camera tween slots
    { "camera tween slots FUN_007bbfc0",
      "33 C9 83 F8 02 77 3F 8D 14 C5 00 00 00 00 2B D0 8B 45 0C 83 E0 01 03 D0 C1 E2 06 81 C2",
      0x7BBFC6, false, nullptr },
    // Indexes the battle camera blocks
    { "camera blocks FUN_007bf940",
      "C1 FA 08 8B CE 69 C9 A0 0D 00 00 8D 98",
      0x7BF973, false, nullptr },
    // Reads the minimap root pointer
    { "minimap root FUN_00921be0",
      "55 8B EC A1 ?? ?? ?? ?? 85 C0 74 04 8B 08 EB 02 33 C9 85 C9 74 22 85 C0",
      0x921BE0, false, nullptr },
    // Pushes the fx context pointer (operand at +0x0D) before an fx call
    { "fx ctx FUN_007fb170",
      "8B 35 ?? ?? ?? ?? 85 F6 74 2A 56 FF 35 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 89 35",
      0x7FB176, false, nullptr },
};

// ---------------------------------------------------------------- resolved symbols
// Names used by regions.txt / allow.txt.
static Sym g_syms[] = {
    { "exec_byte",        nullptr, 0x00C496D4, true  },
    { "divider",          nullptr, 0x00C30E88, false },
    { "backlog_count",    nullptr, 0x012FB808, true  },
    { "skip_render",      nullptr, 0x012FB7D0, true  },
    { "skip_render_d8",   nullptr, 0x012FB7D8, false },
    { "skip_render_d4",   nullptr, 0x012FB7D4, false },
    { "actor_table",      nullptr, 0x023C44E4, false },
    { "actor_count",      nullptr, 0x023C44E0, false },
    { "fx_scene_array",   nullptr, 0x011334D4, false },
    { "fx_scene_array2",  nullptr, 0x0113446C, false },
    { "frame_counter",    nullptr, 0x023CBBF0, false },
    { "vblank",           nullptr, 0x012FB7F0, false },
    { "vblank2",          nullptr, 0x012FB7F4, false },
    { "cam_tween_slots",  nullptr, 0x011370DC, false },
    { "cam_blocks",       nullptr, 0x01137570, false },
    { "minimap_root",     nullptr, 0x0193BF80, false },
    { "fx_ctx",           nullptr, 0x012A40C0, false },
};
static const int NSYMS = (int)(sizeof(g_syms) / sizeof(g_syms[0]));

uint8_t* sym_rt(const char* name) {
    for (int i = 0; i < NSYMS; i++) {
        if (_stricmp(g_syms[i].name, name) == 0) return g_syms[i].rt;
    }
    return nullptr;
}

int sym_count() { return NSYMS; }
const Sym* sym_at(int i) { return (i >= 0 && i < NSYMS) ? &g_syms[i] : nullptr; }

static Sym* sym_get(const char* name) {
    for (int i = 0; i < NSYMS; i++) {
        if (strcmp(g_syms[i].name, name) == 0) return &g_syms[i];
    }
    return nullptr;
}

// The absolute address operand stored at `at` (already relocated by the loader).
uint8_t* read_abs(const uint8_t* at) { return (uint8_t*)(uintptr_t)*(const uint32_t*)at; }

static bool in_exe(const uint8_t* p) {
    uint8_t* b = exe_base();
    return p >= b && p < b + exe_size();
}

// Sets a symbol from an absolute operand at sig+off, optionally cross-checked against sig2+off2.
static bool set_sym(const char* name, int sig, int off, int sig2 = -1, int off2 = 0) {
    Sym* s = sym_get(name);
    if (!s || !g_sigs[sig].found) return false;
    uint8_t* v = read_abs(g_sigs[sig].found + off);
    if (!in_exe(v)) {
        log_line("  sym %-16s operand %p outside FFX.exe - REJECTED", name, v);
        return false;
    }
    if (sig2 >= 0) {
        if (!g_sigs[sig2].found) return false;
        uint8_t* v2 = read_abs(g_sigs[sig2].found + off2);
        if (v2 != v) {
            log_line("  sym %-16s cross-check MISMATCH %p vs %p - REJECTED", name, v, v2);
            return false;
        }
    }
    s->rt = v;
    uint32_t gva = rt_to_gva_exe(v);
    log_line("  sym %-16s rt=%p gva=0x%08X expected=0x%08X %s%s", name, v, gva, s->expected_gva,
             gva == s->expected_gva ? "match" : "SHIFTED", sig2 >= 0 ? " (cross-checked)" : "");
    return true;
}

// Rows resolve their own sites with this: the pattern must match exactly once in .text.
uint8_t* sig_find_unique(const char* label, const char* pattern, uint32_t expected_gva) {
    Pattern p;
    if (!parse_pattern(pattern, &p)) { log_line("  sig %-34s BAD PATTERN (bug)", label); return nullptr; }
    uint8_t* first = nullptr;
    int hits = scan_text(&p, &first);
    if (hits != 1) {
        log_line("  sig %-34s %s (%d hits)", label, hits == 0 ? "NOT FOUND" : "AMBIGUOUS", hits);
        return nullptr;
    }
    uint32_t gva = rt_to_gva_exe(first);
    log_line("  sig %-34s rt=%p gva=0x%08X expected=0x%08X %s (%d bytes verified)", label, first, gva, expected_gva,
             gva == expected_gva ? "match" : "SHIFTED", p.len);
    return first;
}

// The absolute target of a CALL rel32 at site (site[0] must be E8), or null.
uint8_t* call_target(const uint8_t* site) {
    if (!site || site[0] != 0xE8) return nullptr;
    return (uint8_t*)(site + 5 + *(const int32_t*)(site + 1));
}

// The absolute target of a JMP rel32 at site (site[0] must be E9), or null.
uint8_t* jmp_target(const uint8_t* site) {
    if (!site || site[0] != 0xE9) return nullptr;
    return (uint8_t*)(site + 5 + *(const int32_t*)(site + 1));
}

bool sigs_text_ready() {
    Pattern p;
    if (!parse_pattern(g_sigs[SIG_RUNNER].pattern, &p)) return false;
    uint8_t* first = nullptr;
    return scan_text(&p, &first) == 1;
}

bool sigs_resolve_all() {
    bool ok = true;
    log_line("signature scan (FFX.exe base %p, Ghidra base 0x%X):", exe_base(), GHIDRA_EXE_BASE);

    for (int i = 0; i < SIG_COUNT; i++) {
        if (i == SIG_MAINLOOP) continue;          // verified at the call target below
        SigDef& d = g_sigs[i];
        Pattern p;
        if (!parse_pattern(d.pattern, &p)) {
            log_line("  sig %-34s BAD PATTERN (bug)", d.name);
            if (d.required) ok = false;
            continue;
        }
        uint8_t* first = nullptr;
        int hits = scan_text(&p, &first);
        if (hits == 1) {
            d.found = first;
            uint32_t gva = rt_to_gva_exe(first);
            log_line("  sig %-34s rt=%p gva=0x%08X expected=0x%08X %s (%d bytes verified)", d.name, first, gva,
                     d.expected_gva, gva == d.expected_gva ? "match" : "SHIFTED", p.len);
        } else {
            log_line("  sig %-34s %s (%d hits)%s", d.name, hits == 0 ? "NOT FOUND" : "AMBIGUOUS", hits,
                     d.required ? " - REQUIRED" : " - optional, dependent features off");
            if (d.required) ok = false;
        }
    }

    // Sg_MainLoop = the runner's CALL target (+0x27): cdecl, one float, plain RET (docs/fps60/sigs.md).
    if (g_sigs[SIG_RUNNER].found) {
        uint8_t* site = g_sigs[SIG_RUNNER].found + 0x27;
        if (site[0] != 0xE8 || site[5] != 0x83 || site[6] != 0xC4 || site[7] != 0x04) {
            log_line("  Sg_MainLoop call site %p bytes unexpected - REQUIRED", site);
            ok = false;
        } else {
            uint8_t* target = site + 5 + *(int32_t*)(site + 1);
            Pattern p;
            parse_pattern(g_sigs[SIG_MAINLOOP].pattern, &p);
            // Another mod (e.g. Fahrenheit) may already hook the entry with a JMP; we only re-point the
            // runner's CALL, so verify the prologue past the 9 bytes such a hook relocates.
            bool hooked = in_exe(target) && target[0] == 0xE9;
            bool prologue_ok = hooked ? in_exe(target) && match_at_from(target, &p, 9) : in_exe(target) && match_at(target, &p);
            if (hooked && prologue_ok)
                log_line("  Sg_MainLoop entry already hooked by another mod (JMP -> %p); kept, fps60 calls through it",
                         target + 5 + *(int32_t*)(target + 1));
            if (prologue_ok) {
                g_sigs[SIG_MAINLOOP].found = target;
                g_sg_mainloop = target;
                g_mainloop_callsite = site;
                uint32_t gva = rt_to_gva_exe(target);
                log_line("  sig %-34s rt=%p gva=0x%08X expected=0x%08X %s (prologue %d bytes verified at CALL target; "
                         "call site gva=0x%08X, caller cleans ADD ESP,4 -> cdecl 1 float)",
                         "Sg_MainLoop", target, gva, g_sigs[SIG_MAINLOOP].expected_gva,
                         gva == g_sigs[SIG_MAINLOOP].expected_gva ? "match" : "SHIFTED", p.len, rt_to_gva_exe(site));
            } else {
                log_line("  Sg_MainLoop prologue mismatch at CALL target %p - REQUIRED", target);
                ok = false;
            }
        }
    }

    // Globals (absolute operands; relocated by the loader, so they are already runtime addresses).
    if (!set_sym("exec_byte", SIG_SET_EXEC, 0x0D, SIG_SET_EXEC, 0x1F)) ok = false;
    else if (g_sigs[SIG_GET_EXEC].found && read_abs(g_sigs[SIG_GET_EXEC].found + 0x14) != sym_rt("exec_byte")) {
        log_line("  exec byte: getter cross-check MISMATCH - REQUIRED");
        ok = false;
    }
    if (!set_sym("backlog_count", SIG_BACKLOG, 0x01, SIG_RUNNER, 0x31)) ok = false;
    if (!set_sym("skip_render", SIG_BACKLOG, 0x0D)) ok = false;
    set_sym("skip_render_d8", SIG_BACKLOG, 0x17);
    set_sym("skip_render_d4", SIG_BACKLOG, 0x21);
    if (g_sigs[SIG_SET_DIVIDER].found) set_sym("divider", SIG_SET_DIVIDER, 0x12, SIG_SET_DIVIDER, 0x22);
    if (g_sigs[SIG_ACTORS].found) {
        set_sym("actor_table", SIG_ACTORS, 0x02);
        set_sym("actor_count", SIG_ACTORS, 0x09);
    }
    if (g_sigs[SIG_FXSCENE].found) {
        set_sym("fx_scene_array", SIG_FXSCENE, 0x18);
        set_sym("fx_scene_array2", SIG_FXSCENE, 0x2E);
    }
    if (g_sigs[SIG_FRAMECTR].found) set_sym("frame_counter", SIG_FRAMECTR, 0x01, SIG_FRAMECTR, 0x0D);
    if (g_sigs[SIG_VBLANK].found) {
        set_sym("vblank", SIG_VBLANK, 0x06);
        set_sym("vblank2", SIG_VBLANK, 0x0D);
    }
    if (g_sigs[SIG_CAMTWEEN].found) set_sym("cam_tween_slots", SIG_CAMTWEEN, 0x1D);
    if (g_sigs[SIG_CAMBLOCK].found) set_sym("cam_blocks", SIG_CAMBLOCK, 0x0D);
    if (g_sigs[SIG_MINIMAP].found)  set_sym("minimap_root", SIG_MINIMAP, 0x04);
    if (g_sigs[SIG_FXCTX].found)    set_sym("fx_ctx", SIG_FXCTX, 0x0D);

    if (!ok) return false;

    g_set_exec  = (set_exec_t)g_sigs[SIG_SET_EXEC].found;
    g_get_exec  = (get_exec_t)g_sigs[SIG_GET_EXEC].found;
    g_set_divider_orig = (set_divider_t)g_sigs[SIG_SET_DIVIDER].found;   // may be null
    g_exec_byte     = (volatile int8_t*)sym_rt("exec_byte");
    g_divider       = (volatile uint32_t*)sym_rt("divider");
    g_backlog_count = (volatile int32_t*)sym_rt("backlog_count");
    g_skip_render   = (volatile int32_t*)sym_rt("skip_render");
    g_frame_counter = (volatile uint32_t*)sym_rt("frame_counter");
    g_vblank        = (volatile uint32_t*)sym_rt("vblank");
    if (!g_divider) g_set_divider_orig = nullptr;
    log_line("signature scan OK (Sg_SetExecFrames/SKS_SetFrameDivider/Sg_GetCurExecFrames: cdecl, plain RET, "
             "verified statically - see re/W2-TOOL-STATUS.md)");
    return true;
}

// ---------------------------------------------------------------- safe memory helpers
bool safe_read_u32(const void* p, uint32_t* out) {
    __try { *out = *(const volatile uint32_t*)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_copy(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* exec_alloc(size_t n) {
    void* p = VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (p) memset(p, 0xCC, n);
    return p;
}
