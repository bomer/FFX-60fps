// SPDX-License-Identifier: Apache-2.0
// Capture (TRIAGE E0): FRAMEDIFF's snapshot/diff engine inside fps60, so a capture can be taken with fps60's
// pacing and rows active. Observe only; default off, and then every capture point is one branch.
// Details: docs/fps60/capture.md (design), README.md "Capture (E0)" (usage)
#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <intrin.h>
#include <emmintrin.h>
#include <psapi.h>                 // K32* (kernel32, PSAPI_VERSION 2)

extern "C" IMAGE_DOS_HEADER __ImageBase;     // this DLL

// The first four keep FRAMEDIFF's indices and names; STOCK = pacing_kind() INV_STOCK (60 Hz pacing
// not applied: menus, FMV hold, pacing=0). CLS_NONE = a diff that only refreshes the snapshot.
enum { CLS_NONE = -1, CLS_BUILD = 0, CLS_LOGIC = 1, CLS_BACKLOG = 2, CLS_OUTSIDE = 3, CLS_STOCK = 4, CLS_COUNT = 5 };
static const char* const CLS_NAME[CLS_COUNT] = { "BUILD", "LOGIC", "BACKLOG", "OUTSIDE", "STOCK" };

enum RegionKind { RK_EXE_SECTION = 0, RK_MOD_SECTION = 1, RK_HEAP = 2 };

struct AllowIv { uint32_t s, e; int rule; };

struct Region {
    char      name[96];
    int       kind;
    uint8_t*  rt;
    uint32_t  size;            // bytes, multiple of 4
    uint32_t  alloc_size;      // snapshot capacity (heap rebase limit)
    uint32_t  gva_base;        // sections: Ghidra VA of rt
    uint32_t  ghidra_base;     // sections: module Ghidra image base (off = gva - ghidra_base)
    uint8_t*  mod;             // sections: module base
    uint32_t  stride;          // heap: element stride for annotation
    int       def_idx;         // heap: index into g_defs
    uint8_t*  snap;
    uint16_t* cnt[CLS_COUNT];
    bool      cnt_tried[CLS_COUNT];
    uint64_t  changed[CLS_COUNT];
    volatile LONG dead;
    int       added_at;        // counted invocation index when added (0 = at start)
    int       moves;
    AllowIv*  iv;
    uint32_t* iv_maxend;
    int       niv;
};

struct Label { char name[64]; uint8_t* rt; uint32_t size; };

static const int MAX_REGIONS = 256;
static const int MAX_DEFS    = 128;
static const int MAX_RULES   = 256;
static const int MAX_LABELS  = 128;
static const int MAX_IV      = 65536;

// Tables live in one VirtualAlloc block made by capture_init only when [capture] enabled=1.
static Region*    g_reg = nullptr;               // [MAX_REGIONS]
static volatile LONG g_nreg = 0;
static RegionDef* g_defs = nullptr;              // [MAX_DEFS]
static int        g_ndefs = 0;
static AllowRule* g_rules = nullptr;             // [MAX_RULES]
static int        g_nrules = 0;
static Label*     g_labels = nullptr;            // [MAX_LABELS]
static int        g_nlabels = 0;
static SRWLOCK    g_lock = SRWLOCK_INIT;

static uint64_t   g_budget = 0, g_used = 0;

// ---------------------------------------------------------------- capture state
enum { ST_IDLE = 0, ST_WARMUP = 1, ST_CAPTURE = 2 };

struct CapState {
    int      state;
    int      boundary;         // CapBoundary, fixed for the capture
    int      split_outside;    // effective (inner forces 1)
    int      warm_left;
    int      inv;              // counted invocations completed
    int      abort_req;
    int      in_inv;           // PRE_OUTER seen for the current invocation (points are paired)
    int      cur_cls;
    int8_t   exec_start;
    int8_t   exec_end;
    int8_t   exec_prev_post;   // exec at the previous POST_OUTER (R flag)
    int      have_prev_post;
    int      eff_start;
    int      backlog_at_pre;
    char     flags[8];
    uint32_t out_bytes, out_ranges;
    uint32_t bytes, ranges;    // class diff of the current invocation
    uint32_t mod_bytes;        // inner: fps60's own writes absorbed this invocation
    int      n_inv[CLS_COUNT];
    int      n_reforced, n_changed_in, n_div_events, n_mod_events, n_moves, n_faults;
    uint64_t mod_bytes_total;
    uint32_t fc_start, vb_start;
    LARGE_INTEGER qpf, t_prev;
    wchar_t  path[MAX_PATH];
};
static CapState g;

static bool          g_cap_on = false;        // capture_init succeeded ([capture] enabled=1)
static volatile LONG g_cap_dead = 0;          // a capture fault: capture is pass-through from now on
static volatile LONG g_pending_load = 0;
static volatile LONG g_div_req_count = 0;     // divider requests not yet logged
static volatile LONG g_div_req_last = 0;
static volatile LONG g_div_req_swallowed = 0; // last request was swallowed by pacing
static volatile LONG g_mod_unload_count = 0;  // unload events not yet logged
static bool g_hotkey_down = false;

// ---------------------------------------------------------------- buffered capture log
static const int OBUF_SIZE = 1 << 16;
static HANDLE g_out = INVALID_HANDLE_VALUE;
static char*  g_obuf = nullptr;                // [OBUF_SIZE], allocated with the tables
static int    g_on = 0;

static void out_flush() {
    if (g_out != INVALID_HANDLE_VALUE && g_on > 0) {
        DWORD w;
        WriteFile(g_out, g_obuf, (DWORD)g_on, &w, nullptr);
    }
    g_on = 0;
}

static void outf(const char* fmt, ...) {
    if (g_out == INVALID_HANDLE_VALUE) return;
    char tmp[1024];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(tmp, sizeof(tmp) - 2, fmt, a);
    va_end(a);
    if (n < 0) return;
    if (n > (int)sizeof(tmp) - 3) n = (int)sizeof(tmp) - 3;
    tmp[n++] = '\r';
    tmp[n++] = '\n';
    if (g_on + n > OBUF_SIZE) out_flush();
    memcpy(g_obuf + g_on, tmp, (size_t)n);
    g_on += n;
}

static void out_close() {
    out_flush();
    if (g_out != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_out);
        CloseHandle(g_out);
    }
    g_out = INVALID_HANDLE_VALUE;
}

// ---------------------------------------------------------------- memory helpers
static bool range_readable(const uint8_t* p, uint32_t n) {
    const uint8_t* end = p + n;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        DWORD pr = mbi.Protect & 0xFF;
        if ((mbi.Protect & PAGE_GUARD) || pr == PAGE_NOACCESS || pr == PAGE_EXECUTE) return false;
        const uint8_t* next = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        if (next <= p) return false;
        p = next;
    }
    return true;
}

static void* big_alloc(uint64_t n) {
    if (n == 0 || n > 0x7FFFFFFF) return nullptr;
    if (g_used + n > g_budget) return nullptr;
    void* p = VirtualAlloc(nullptr, (SIZE_T)n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p) g_used += n;
    return p;
}

static void big_free(void* p, uint64_t n) {
    if (p) {
        VirtualFree(p, 0, MEM_RELEASE);
        g_used -= n;
    }
}

static uint64_t cnt_bytes(const Region& r) { return (uint64_t)((r.size / 4 + 8) & ~7u) * 2; }

static uint16_t* ensure_cnt(Region& r, int cls) {
    if (r.cnt[cls] || r.cnt_tried[cls]) return r.cnt[cls];
    r.cnt_tried[cls] = true;
    r.cnt[cls] = (uint16_t*)big_alloc(cnt_bytes(r));
    if (!r.cnt[cls]) {
        log_line("capture: %s counters for region %s not allocated (budget %u MB) - class not reported for it",
                 CLS_NAME[cls], r.name, (unsigned)(g_budget >> 20));
        outf("# NOTE %s counters for region %s not allocated (budget) - class not reported for it",
             CLS_NAME[cls], r.name);
    }
    return r.cnt[cls];
}

// ---------------------------------------------------------------- address formatting
static uint32_t region_off(const Region& r, uint32_t rel) {
    if (r.kind == RK_HEAP) return rel;
    return r.gva_base + rel - r.ghidra_base;
}

static void fmt_addr(const Region& r, uint32_t rel, char* buf, size_t cap) {
    if (r.kind == RK_HEAP) {
        int n = snprintf(buf, cap, "rt=0x%08X off=+0x%06X", (uint32_t)(uintptr_t)(r.rt + rel), rel);
        if (r.stride && n > 0 && (size_t)n < cap)
            snprintf(buf + n, cap - (size_t)n, " [elem %u +0x%X]", rel / r.stride, rel % r.stride);
    } else {
        snprintf(buf, cap, "gva=0x%08X off=0x%08X", r.gva_base + rel, region_off(r, rel));
    }
}

static void fmt_label(const uint8_t* rt, char* buf, size_t cap) {
    buf[0] = 0;
    const Label* best = nullptr;
    for (int i = 0; i < g_nlabels; i++) {
        const Label& l = g_labels[i];
        if (rt >= l.rt && rt < l.rt + l.size && (!best || l.size < best->size)) best = &l;
    }
    if (best) snprintf(buf, cap, " [%s+0x%X]", best->name, (uint32_t)(rt - best->rt));
}

// ---------------------------------------------------------------- allow-list intervals
static int iv_cmp(const void* a, const void* b) {
    const AllowIv* x = (const AllowIv*)a;
    const AllowIv* y = (const AllowIv*)b;
    return x->s < y->s ? -1 : x->s > y->s ? 1 : 0;
}

static void build_allow(Region& r) {
    AllowIv* tmp = (AllowIv*)malloc(sizeof(AllowIv) * MAX_IV);
    if (!tmp) return;
    int n = 0;
    bool capped = false;
    for (int i = 0; i < g_nrules && !capped; i++) {
        AllowRule& a = g_rules[i];
        if (!glob_match(a.region_glob, r.name)) continue;
        int64_t rel;
        if (a.spec == ALLOW_SYM) {
            uint8_t* s = sym_rt(a.sym);
            if (!s) continue;
            rel = (int64_t)(intptr_t)(s + a.value) - (int64_t)(intptr_t)r.rt;
        } else if (a.spec == ALLOW_GVA) {
            if (r.kind == RK_HEAP) continue;
            rel = (int64_t)a.value - (int64_t)r.gva_base;
        } else {
            rel = (r.kind == RK_HEAP) ? (int64_t)a.value
                                      : (int64_t)a.value - (int64_t)(r.gva_base - r.ghidra_base);
        }
        uint32_t reps = a.stride ? (a.count ? a.count : 0xFFFFFFFFu) : 1;
        for (uint32_t k = 0; k < reps; k++) {
            int64_t s = rel + (int64_t)k * a.stride;
            int64_t e = s + a.len;
            if (s >= (int64_t)r.size) break;
            if (e <= 0) continue;
            if (s < 0) s = 0;
            if (e > (int64_t)r.size) e = r.size;
            if (n >= MAX_IV) { capped = true; break; }
            tmp[n].s = (uint32_t)s;
            tmp[n].e = (uint32_t)e;
            tmp[n].rule = i;
            n++;
        }
    }
    if (capped) log_line("allow.txt: region %s exceeds %d intervals - extra intervals ignored", r.name, MAX_IV);
    if (n == 0) { free(tmp); return; }
    qsort(tmp, (size_t)n, sizeof(AllowIv), iv_cmp);
    r.iv = (AllowIv*)realloc(tmp, sizeof(AllowIv) * (size_t)n);
    if (!r.iv) r.iv = tmp;
    r.iv_maxend = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)n);
    if (!r.iv_maxend) { free(r.iv); r.iv = nullptr; return; }
    uint32_t m = 0;
    for (int i = 0; i < n; i++) { if (r.iv[i].e > m) m = r.iv[i].e; r.iv_maxend[i] = m; }
    r.niv = n;
}

// Splits [s,e) into the pieces NOT covered by allow intervals; calls fn for each.
// count_suppressed: credit the rules (summary pass only).
typedef void (*piece_fn)(Region& r, uint32_t s, uint32_t e, void* ctx);

static void subtract_allow(Region& r, uint32_t s, uint32_t e, bool count_suppressed, piece_fn fn, void* ctx) {
    if (!r.niv) { fn(r, s, e, ctx); return; }
    int lo = 0, hi = r.niv;                // first i with maxend > s
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (r.iv_maxend[mid] > s) hi = mid; else lo = mid + 1;
    }
    uint32_t cur = s;
    for (int i = lo; i < r.niv && r.iv[i].s < e && cur < e; i++) {
        const AllowIv& v = r.iv[i];
        if (v.e <= cur) continue;
        if (v.s > cur) { fn(r, cur, v.s < e ? v.s : e, ctx); cur = v.s; if (cur >= e) break; }
        uint32_t stop = v.e < e ? v.e : e;
        if (count_suppressed) g_rules[v.rule].suppressed_bytes += stop - cur;
        cur = stop;
    }
    if (cur < e) fn(r, cur, e, ctx);
}

// ---------------------------------------------------------------- per-invocation range tracking
struct RunAcc {
    bool     open;
    uint32_t s, e;
    uint32_t ranges;
    uint32_t bytes;
    int      listed;
    int      cls;
};

static void list_piece(Region& r, uint32_t s, uint32_t e, void* vctx) {
    RunAcc* acc = (RunAcc*)vctx;
    if (acc->listed >= g_ccfg.per_inv_cap) {
        if (acc->listed == g_ccfg.per_inv_cap) outf("    ~ ... (per_invocation_cap %d reached)", g_ccfg.per_inv_cap);
        acc->listed++;
        return;
    }
    acc->listed++;
    char a[96], l[96];
    fmt_addr(r, s, a, sizeof(a));
    fmt_label(r.rt + s, l, sizeof(l));
    outf("    ~ %-7s %-28s %s len=%u%s", CLS_NAME[acc->cls], r.name, a, e - s, l);
}

static void run_flush(Region& r, RunAcc* acc) {
    if (!acc->open) return;
    acc->open = false;
    acc->ranges++;
    if (g_ccfg.per_inv_ranges && acc->cls >= 0) subtract_allow(r, acc->s, acc->e, false, list_piece, acc);
}

static inline void run_byte(Region& r, RunAcc* acc, uint32_t o) {
    if (acc->open && o <= acc->e + (uint32_t)g_ccfg.merge_gap) {
        if (o + 1 > acc->e) acc->e = o + 1;
        return;
    }
    run_flush(r, acc);
    acc->open = true;
    acc->s = o;
    acc->e = o + 1;
}

static inline void record_word(uint16_t* c, uint32_t w, unsigned lanes) {
    uint16_t v = c[w];
    uint16_t hits = (uint16_t)(v >> 4);
    if (hits < 0x0FFF) hits++;
    c[w] = (uint16_t)((hits << 4) | (v & 0xF) | lanes);
}

static void record_mask(Region& r, uint16_t* c, uint32_t base, uint32_t mask, RunAcc* acc) {
    acc->bytes += (uint32_t)__popcnt(mask);
    if (c) {
        for (int k = 0; k < 8; k++) {
            unsigned lanes = (mask >> (k * 4)) & 0xF;
            if (lanes) record_word(c, (base >> 2) + (uint32_t)k, lanes);
        }
    }
    uint32_t m = mask;
    unsigned long bit;
    while (m) {
        _BitScanForward(&bit, m);
        run_byte(r, acc, base + bit);
        m &= m - 1;
    }
}

// Diffs one region against its snapshot and updates the snapshot. SEH: a fault marks the region dead.
static void diff_region(Region& r, int cls, RunAcc* acc) {
    uint16_t* c = (cls >= 0) ? ensure_cnt(r, cls) : nullptr;
    __try {
        uint8_t* live = r.rt;
        uint8_t* snap = r.snap;
        uint32_t n = r.size;
        uint32_t i = 0;
        uint32_t changed = 0;
        for (; i + 32 <= n; i += 32) {
            __m128i a0 = _mm_loadu_si128((const __m128i*)(live + i));
            __m128i a1 = _mm_loadu_si128((const __m128i*)(live + i + 16));
            __m128i b0 = _mm_load_si128((const __m128i*)(snap + i));
            __m128i b1 = _mm_load_si128((const __m128i*)(snap + i + 16));
            unsigned eq = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(a0, b0)) |
                          ((unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(a1, b1)) << 16);
            if (eq == 0xFFFFFFFFu) continue;
            _mm_store_si128((__m128i*)(snap + i), a0);
            _mm_store_si128((__m128i*)(snap + i + 16), a1);
            uint32_t before = acc->bytes;
            record_mask(r, c, i, ~eq, acc);
            changed += acc->bytes - before;
        }
        if (i < n) {                                  // tail (< 32 bytes, multiple of 4)
            uint8_t t[32];
            memcpy(t, live + i, n - i);
            uint32_t mask = 0;
            for (uint32_t k = 0; k < n - i; k++) {
                if (t[k] != snap[i + k]) mask |= 1u << k;
            }
            if (mask) {
                memcpy(snap + i, t, n - i);
                uint32_t before = acc->bytes;
                record_mask(r, c, i, mask, acc);
                changed += acc->bytes - before;
            }
        }
        run_flush(r, acc);
        if (cls >= 0) r.changed[cls] += changed;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&r.dead, 1);
        acc->open = false;
        g.n_faults++;
        log_line("capture: FAULT 0x%08lX reading region %s at %p - region disabled", GetExceptionCode(), r.name, r.rt);
        outf("  event: FAULT reading region %s - region disabled from here on", r.name);
    }
}

static bool snapshot_region(Region& r) {
    return safe_copy(r.snap, r.rt, r.size);
}

// Re-copies [p, p+n) into the snapshot of whatever region holds it, so OUR OWN writes (exec byte,
// divider) are not attributed to the game.
static void snap_refresh(const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p;
    LONG nr = g_nreg;
    for (LONG i = 0; i < nr; i++) {
        Region& r = g_reg[i];
        if (r.dead || !r.snap) continue;
        if (b >= r.rt && b + n <= r.rt + r.size) {
            safe_copy(r.snap + (b - r.rt), b, n);
        }
    }
}

// ---------------------------------------------------------------- region construction
static IMAGE_NT_HEADERS32* nt_of(uint8_t* base) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

static bool region_exists_mod_section(uint8_t* rt) {
    for (LONG i = 0; i < g_nreg; i++) {
        if (g_reg[i].kind != RK_HEAP && g_reg[i].rt == rt && !g_reg[i].dead) return true;
    }
    return false;
}

static int g_expected_classes[CLS_COUNT];

// Allocates snapshot + expected counters and takes the baseline. Appends under the exclusive lock.
static bool region_commit(Region& proto, bool take_snapshot) {
    if (g_nreg >= MAX_REGIONS) {
        log_line("capture: more than %d regions - %s skipped", MAX_REGIONS, proto.name);
        return false;
    }
    uint64_t need = proto.size;
    for (int c = 0; c < CLS_COUNT; c++) if (g_expected_classes[c]) need += cnt_bytes(proto);
    if (g_used + need > g_budget) {
        log_line("capture: region %s (0x%X bytes, needs %u KB) DROPPED - over max_total_mb=%d",
                 proto.name, proto.size, (unsigned)(need >> 10), g_ccfg.max_total_mb);
        outf("# DROPPED region %s size=0x%X (budget)", proto.name, proto.size);
        return false;
    }
    proto.snap = (uint8_t*)big_alloc(proto.size);
    if (!proto.snap) {
        log_line("capture: region %s snapshot alloc failed", proto.name);
        return false;
    }
    proto.alloc_size = proto.size;
    for (int c = 0; c < CLS_COUNT; c++) if (g_expected_classes[c]) ensure_cnt(proto, c);
    build_allow(proto);
    if (take_snapshot && !snapshot_region(proto)) {
        log_line("capture: region %s unreadable at baseline - dropped", proto.name);
        big_free(proto.snap, proto.size);
        for (int c = 0; c < CLS_COUNT; c++) big_free(proto.cnt[c], cnt_bytes(proto));
        free(proto.iv);
        free(proto.iv_maxend);
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_reg[g_nreg] = proto;
    InterlockedIncrement(&g_nreg);
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

static void module_name(uint8_t* base, char* out, size_t cap) {
    wchar_t w[MAX_PATH];
    out[0] = 0;
    if (!K32GetModuleBaseNameW(GetCurrentProcess(), (HMODULE)base, w, MAX_PATH)) return;
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)cap, nullptr, nullptr);
}

// Adds the writable sections of one module that match a 'section' def. Returns sections added.
static int add_module_sections(uint8_t* base, const char* modname, const RegionDef& d, bool take_snapshot, int added_at) {
    IMAGE_NT_HEADERS32* nt = nt_of(base);
    if (!nt) return 0;
    bool is_exe = base == exe_base();
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int added = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char sname[9];
        memcpy(sname, sec[i].Name, 8);
        sname[8] = 0;
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        if (!glob_match(d.section_glob, sname)) continue;
        uint8_t* rt = base + sec[i].VirtualAddress;
        if (region_exists_mod_section(rt)) continue;
        uint32_t size = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        size = (size + 3) & ~3u;                 // sections are page-granular: rounding up stays inside
        if (!size) continue;
        if (!range_readable(rt, size)) {
            log_line("capture: %s:%s at %p size 0x%X not fully readable - skipped", modname, sname, rt, size);
            continue;
        }
        Region r;
        memset(&r, 0, sizeof(r));
        snprintf(r.name, sizeof(r.name), "%s:%s", modname, sname);
        r.kind = is_exe ? RK_EXE_SECTION : RK_MOD_SECTION;
        r.rt = rt;
        r.size = size;
        r.mod = base;
        r.ghidra_base = is_exe ? g_ccfg.ghidra_base_exe : g_ccfg.ghidra_base_magic;
        r.gva_base = r.ghidra_base + sec[i].VirtualAddress;
        r.def_idx = -1;
        r.added_at = added_at;
        if (region_commit(r, take_snapshot)) {
            added++;
            log_line("capture: region %-28s rt=%p size=0x%08X gva=0x%08X (regions.txt:%d)", r.name, rt, size,
                     r.gva_base, d.line);
        }
    }
    return added;
}

static int scan_modules_for_sections(bool take_snapshot, int added_at) {
    HMODULE mods[1024];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return 0;
    int nm = (int)(need / sizeof(HMODULE));
    if (nm > 1024) nm = 1024;
    int added = 0;
    for (int di = 0; di < g_ndefs; di++) {
        const RegionDef& d = g_defs[di];
        if (d.kind != RDEF_SECTION) continue;
        for (int m = 0; m < nm; m++) {
            char name[MAX_PATH];
            module_name((uint8_t*)mods[m], name, sizeof(name));
            if (!name[0] || !glob_match(d.module_glob, name)) continue;
            if ((uint8_t*)mods[m] == (uint8_t*)&__ImageBase) continue;   // never ourselves
            added += add_module_sections((uint8_t*)mods[m], name, d, take_snapshot, added_at);
        }
    }
    return added;
}

static bool heap_resolve(const RegionDef& d, uint8_t** out_base, uint32_t* out_size, const char** why) {
    uint8_t* s = sym_rt(d.sym);
    if (!s) { *why = "pointer symbol unresolved"; return false; }
    uint32_t v;
    if (!safe_read_u32(s, &v)) { *why = "pointer unreadable"; return false; }
    for (int i = 0; i < d.ndrefs; i++) {
        if (!v) { *why = "null pointer in deref chain"; return false; }
        if (!safe_read_u32((uint8_t*)(uintptr_t)v + d.drefs[i], &v)) { *why = "deref unreadable"; return false; }
    }
    if (!v) { *why = "null pointer"; return false; }
    uint8_t* base = (uint8_t*)(uintptr_t)v + d.add;
    uint64_t size = d.size;
    if (d.count_sym[0]) {
        uint8_t* cs = sym_rt(d.count_sym);
        uint32_t cnt;
        if (!cs || !safe_read_u32(cs, &cnt)) { *why = "count symbol unresolved"; return false; }
        if ((int32_t)cnt <= 0 || cnt > 1000000) { *why = "count out of range"; return false; }
        size = (uint64_t)cnt * d.size;
    }
    if (size > d.max_size) size = d.max_size;
    size &= ~3ull;
    if (!size) { *why = "size 0"; return false; }
    if (!range_readable(base, (uint32_t)size)) { *why = "range not committed/readable"; return false; }
    *out_base = base;
    *out_size = (uint32_t)size;
    return true;
}

static void add_heaps() {
    for (int di = 0; di < g_ndefs; di++) {
        const RegionDef& d = g_defs[di];
        if (d.kind != RDEF_HEAP) continue;
        uint8_t* base;
        uint32_t size;
        const char* why = "";
        if (!heap_resolve(d, &base, &size, &why)) {
            log_line("capture: heap %s (regions.txt:%d) not added: %s", d.name, d.line, why);
            outf("# heap %s not added: %s", d.name, why);
            continue;
        }
        Region r;
        memset(&r, 0, sizeof(r));
        snprintf(r.name, sizeof(r.name), "%s", d.name);
        r.kind = RK_HEAP;
        r.rt = base;
        r.size = size;
        r.stride = d.stride;
        r.def_idx = di;
        if (region_commit(r, true))
            log_line("capture: region %-28s rt=%p size=0x%08X (heap via %s, regions.txt:%d)", r.name, base, size, d.sym, d.line);
    }
}

static void add_labels() {
    g_nlabels = 0;
    for (int di = 0; di < g_ndefs && g_nlabels < MAX_LABELS; di++) {
        const RegionDef& d = g_defs[di];
        if (d.kind != RDEF_LABEL) continue;
        uint8_t* s = sym_rt(d.sym);
        if (!s) { log_line("capture: label %s: symbol %s unresolved", d.name, d.sym); continue; }
        Label& l = g_labels[g_nlabels++];
        snprintf(l.name, sizeof(l.name), "%s", d.name);
        l.rt = s + d.add;
        l.size = d.size;
    }
    for (int i = 0; i < sym_count() && g_nlabels < MAX_LABELS; i++) {     // built-in: every resolved symbol
        const Sym* s = sym_at(i);
        if (!s || !s->rt) continue;
        Label& l = g_labels[g_nlabels++];
        snprintf(l.name, sizeof(l.name), "%s", s->name);
        l.rt = s->rt;
        l.size = strcmp(s->name, "exec_byte") == 0 ? 1 : 4;
    }
}

static void free_regions() {
    AcquireSRWLockExclusive(&g_lock);
    for (LONG i = 0; i < g_nreg; i++) {
        Region& r = g_reg[i];
        big_free(r.snap, r.alloc_size);
        Region tmp = r;
        tmp.size = r.alloc_size;
        for (int c = 0; c < CLS_COUNT; c++) big_free(r.cnt[c], cnt_bytes(tmp));
        free(r.iv);
        free(r.iv_maxend);
    }
    memset(g_reg, 0, sizeof(Region) * MAX_REGIONS);
    g_nreg = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_used = 0;
}

// ---------------------------------------------------------------- heap tracking during capture
static void check_heaps() {
    for (LONG i = 0; i < g_nreg; i++) {
        Region& r = g_reg[i];
        if (r.kind != RK_HEAP || r.dead) continue;
        uint8_t* base;
        uint32_t size;
        const char* why = "";
        bool ok = heap_resolve(g_defs[r.def_idx], &base, &size, &why);
        if (ok && base == r.rt && size == r.size) continue;
        g.n_moves++;
        r.moves++;
        g.flags[3] = 'V';
        if (ok && size <= r.alloc_size) {
            outf("  event: heap %s moved/resized %p/0x%X -> %p/0x%X before inv %d - rebased (new baseline, counters keep offsets)",
                 r.name, r.rt, r.size, base, size, g.inv + 1);
            log_line("capture: heap %s rebased %p/0x%X -> %p/0x%X", r.name, r.rt, r.size, base, size);
            r.rt = base;
            r.size = size;
            if (!snapshot_region(r)) InterlockedExchange(&r.dead, 1);
        } else {
            outf("  event: heap %s %s before inv %d (%s) - region disabled", r.name, ok ? "grew past its snapshot" : "lost",
                 g.inv + 1, ok ? "size" : why);
            log_line("capture: heap %s disabled (%s)", r.name, ok ? "grew" : why);
            InterlockedExchange(&r.dead, 1);
        }
    }
}

// ---------------------------------------------------------------- DLL notifications
typedef struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } FD_UNICODE_STRING;
typedef struct {
    ULONG Flags;
    const FD_UNICODE_STRING* FullDllName;
    const FD_UNICODE_STRING* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} FD_LDR_NOTIF_DATA;
typedef VOID(CALLBACK* FD_LDR_NOTIF_FN)(ULONG, const FD_LDR_NOTIF_DATA*, PVOID);
typedef LONG(NTAPI* LdrRegisterDllNotification_t)(ULONG, FD_LDR_NOTIF_FN, PVOID, PVOID*);

// DIAG-HANGDUMP breadcrumb (re/ledger/HANG-SPELLLOAD.md).
static void dll_notif_body(ULONG reason, const FD_LDR_NOTIF_DATA* data);
static void CALLBACK dll_notif(ULONG reason, const FD_LDR_NOTIF_DATA* data, PVOID) {
    ldr_enter("capture", reason, data ? data->DllBase : nullptr);
    dll_notif_body(reason, data);
    ldr_leave();
}

static void dll_notif_body(ULONG reason, const FD_LDR_NOTIF_DATA* data) {
    __try {
        if (g_cap_dead || g.state == ST_IDLE || !data) return;
        if (reason == 1) {                      // LDR_DLL_NOTIFICATION_REASON_LOADED
            InterlockedExchange(&g_pending_load, 1);
        } else if (reason == 2) {               // UNLOADED: stop reading its sections first
            uint8_t* b = (uint8_t*)data->DllBase;
            AcquireSRWLockExclusive(&g_lock);
            for (LONG i = 0; i < g_nreg; i++) {
                if (g_reg[i].kind == RK_MOD_SECTION && g_reg[i].mod == b && !g_reg[i].dead) {
                    InterlockedExchange(&g_reg[i].dead, 1);
                    InterlockedIncrement(&g_mod_unload_count);
                }
            }
            ReleaseSRWLockExclusive(&g_lock);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void capture_init() {
    capture_config_load();
    if (!g_ccfg.enabled) {
        log_line("capture (E0): OFF (ini [capture] enabled=0) - nothing allocated or registered");
        return;
    }
    // One block for every table, so a disabled capture costs nothing.
    SIZE_T need = sizeof(Region) * MAX_REGIONS + sizeof(RegionDef) * MAX_DEFS + sizeof(AllowRule) * MAX_RULES +
                  sizeof(Label) * MAX_LABELS + OBUF_SIZE;
    uint8_t* blk = (uint8_t*)VirtualAlloc(nullptr, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!blk) {
        log_line("capture (E0): table allocation failed (%lu) - capture OFF", GetLastError());
        return;
    }
    g_reg = (Region*)blk;                     blk += sizeof(Region) * MAX_REGIONS;
    g_defs = (RegionDef*)blk;                 blk += sizeof(RegionDef) * MAX_DEFS;
    g_rules = (AllowRule*)blk;                blk += sizeof(AllowRule) * MAX_RULES;
    g_labels = (Label*)blk;                   blk += sizeof(Label) * MAX_LABELS;
    g_obuf = (char*)blk;
    QueryPerformanceFrequency(&g.qpf);
    g.state = ST_IDLE;

    if (!g_ccfg.dll_notify) {
        log_line("capture dll_notify: OFF (ini) - magic overlays loaded mid-capture are not added; unloads rely on SEH");
    } else {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        LdrRegisterDllNotification_t reg =
            nt ? (LdrRegisterDllNotification_t)GetProcAddress(nt, "LdrRegisterDllNotification") : nullptr;
        PVOID cookie = nullptr;
        if (reg && reg(0, dll_notif, nullptr, &cookie) == 0)
            log_line("capture dll_notify: ON - LdrRegisterDllNotification (mid-capture magic loads added, unloads disable their regions)");
        else
            log_line("capture dll_notify: FAILED to register - unloads rely on SEH only");
    }
    g_cap_on = true;
    log_line("capture (E0): ON, observe only - hotkey 0x%02X, boundary=%s, log_dir %ls (the rest of [capture], "
             "regions.txt and allow.txt are re-read at every capture start)",
             g_ccfg.hotkey, boundary_name(g_ccfg.boundary), g_ccfg.log_dir);
}

// ---------------------------------------------------------------- divider requests (observe only)
// From the divider hook (hooks.cpp), after pacing decided. Recorded as flag D and an event line.
void capture_divider_event(uint32_t req, bool swallowed) {
    if (!g_cap_on || g_cap_dead || g.state == ST_IDLE) return;
    InterlockedExchange(&g_div_req_last, (LONG)req);
    InterlockedExchange(&g_div_req_swallowed, swallowed ? 1 : 0);
    InterlockedIncrement(&g_div_req_count);
}

static int eff_exec() {
    __try { return g_get_exec ? g_get_exec() : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// ---------------------------------------------------------------- start / stop
static double qpc_ms(LARGE_INTEGER a, LARGE_INTEGER b) {
    return g.qpf.QuadPart ? (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)g.qpf.QuadPart : 0.0;
}

static uint32_t rd32(volatile uint32_t* p) { return p ? *p : 0; }

static int kind_to_cls(int k) {
    switch (k) {
    case INV_LOGIC:   return CLS_LOGIC;
    case INV_BUILD:   return CLS_BUILD;
    case INV_BACKLOG: return CLS_BACKLOG;
    default:          return CLS_STOCK;
    }
}

static void write_rows_header() {
    char line[900];
    int n = snprintf(line, sizeof(line), "# rows:");
    for (int i = 0; i < rows_count() && n > 0 && n < (int)sizeof(line) - 64; i++) {
        const Row* r = row_at(i);
        if (!r) continue;
        int ini = config_row(r->id, r->default_on);
        const char* st = r->on ? "ON" : (ini ? "OFF(failed-or-turned-off)" : "OFF(ini)");
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=%s", r->id, st);
    }
    outf("%s", line);
}

static void capture_start() {
    capture_config_load();                      // re-read [capture] (enabled is fixed at init)
    g.boundary = g_ccfg.boundary;
    g.split_outside = g_ccfg.split_outside;
    if (g.boundary == BOUNDARY_INNER && !g.split_outside) {
        g.split_outside = 1;
        log_line("capture: boundary=inner forces split_outside=1");
    }

    memset(g_expected_classes, 0, sizeof(g_expected_classes));
    g_expected_classes[CLS_BUILD] = 1;          // BACKLOG / STOCK counters are allocated on first use
    g_expected_classes[CLS_LOGIC] = 1;
    if (g.split_outside) g_expected_classes[CLS_OUTSIDE] = 1;
    g_budget = (uint64_t)g_ccfg.max_total_mb << 20;
    g_used = 0;

    // capture log file: <scenario>_observe-<boundary>_<stamp>.log
    SYSTEMTIME t;
    GetLocalTime(&t);
    CreateDirectoryW(g_ccfg.log_dir, nullptr);
    wchar_t wscen[64];
    MultiByteToWideChar(CP_ACP, 0, g_ccfg.scenario, -1, wscen, 64);
    _snwprintf_s(g.path, MAX_PATH, _TRUNCATE, L"%s\\%s_observe-%s_%04u%02u%02u-%02u%02u%02u.log", g_ccfg.log_dir, wscen,
                 g.boundary == BOUNDARY_INNER ? L"inner" : L"outer", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    g_out = CreateFileW(g.path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_out == INVALID_HANDLE_VALUE) {
        log_line("capture: cannot create capture log (error %lu) - capture not started", GetLastError());
        return;
    }
    g_on = 0;

    // The mode token carries the boundary, so w2a_parse-style scripts keep outer and inner apart.
    outf("# FRAMEDIFF capture (fps60 E0, observe only)");
    outf("# scenario=%s mode=observe-%s N=%d warmup=%d exec_method=%s alt_backlog=%s split_outside=%d merge_gap=%d boundary=%s",
         g_ccfg.scenario, boundary_name(g.boundary), g_ccfg.invocations, g_ccfg.warmup,
         g_cfg.exec_method == EXEC_BYTE ? "byte" : "setter", g_cfg.alt_backlog == ALT_BACKLOG_TOGGLE ? "toggle" : "logic",
         g.split_outside, g_ccfg.merge_gap, boundary_name(g.boundary));
    outf("# date=%04u-%02u-%02u %02u:%02u:%02u  FFX.exe base=%p (Ghidra 0x%X), magic Ghidra base 0x%X",
         t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, exe_base(), g_ccfg.ghidra_base_exe,
         g_ccfg.ghidra_base_magic);
    outf("# address forms: gva = Ghidra VA, off = gva - Ghidra base (module-relative); heap regions: rt = runtime, off = region-relative");
    outf("# fps60: pacing=%d rollback=%d rollback_spans=%d divider_hooked=%d", g_cfg.pacing, g_cfg.rollback,
         rollback_span_count(), g_divider_hooked ? 1 : 0);
    write_rows_header();
    if (g.boundary == BOUNDARY_OUTER)
        outf("# boundary=outer: class = fp_pre start -> fp_post end (row/rollback pre+post writes inside; a rolled-back "
             "span is unchanged by construction, TRIAGE P2)");
    else
        outf("# boundary=inner: class = after rows_pre -> before rows_post (bare Sg_MainLoop; fps60's own pre/post writes "
             "are absorbed unreported, counted as mod_bytes)");

    // regions / allow / labels
    g_ndefs = regions_load(g_defs, MAX_DEFS);
    g_nrules = allow_load(g_rules, MAX_RULES);
    add_labels();
    scan_modules_for_sections(false, 0);
    add_heaps();

    uint64_t total = 0;
    for (LONG i = 0; i < g_nreg; i++) total += g_reg[i].size;
    outf("# regions: %ld, %llu bytes snapshotted; memory used %llu KB of max_total_mb=%d",
         g_nreg, (unsigned long long)total, (unsigned long long)(g_used >> 10), g_ccfg.max_total_mb);
    for (LONG i = 0; i < g_nreg; i++) {
        Region& r = g_reg[i];
        if (r.kind == RK_HEAP)
            outf("#   region %-28s rt=0x%08X size=0x%08X heap stride=0x%X allow_intervals=%d", r.name,
                 (uint32_t)(uintptr_t)r.rt, r.size, r.stride, r.niv);
        else
            outf("#   region %-28s rt=0x%08X size=0x%08X gva=0x%08X..0x%08X allow_intervals=%d", r.name,
                 (uint32_t)(uintptr_t)r.rt, r.size, r.gva_base, r.gva_base + r.size, r.niv);
    }
    outf("# allow rules: %d", g_nrules);
    for (int i = 0; i < g_nrules; i++) outf("#   allow.txt:%d %s ; %s", g_rules[i].line, g_rules[i].region_glob, g_rules[i].reason);
    outf("# labels: %d", g_nlabels);
    for (int i = 0; i < g_nlabels; i++)
        outf("#   label %-20s gva=0x%08X size=0x%X", g_labels[i].name, rt_to_gva_exe(g_labels[i].rt), g_labels[i].size);
    log_line("capture: %ld regions, %llu bytes, memory %llu KB", g_nreg, (unsigned long long)total,
             (unsigned long long)(g_used >> 10));
    if (g_nreg == 0) {
        log_line("capture: no regions - aborted");
        outf("# ABORTED: no regions");
        out_close();
        return;
    }

    g.fc_start = rd32(g_frame_counter);
    g.vb_start = rd32(g_vblank);
    outf("# start: frame_counter=%u vblank=%u exec=%d eff_exec=%d divider=%u backlog=%d pacing_active=%d",
         g.fc_start, g.vb_start, *g_exec_byte, eff_exec(), rd32(g_divider), *g_backlog_count, pacing_active() ? 1 : 0);
    outf("# columns: inv class exec(start->end) eff_exec backlog skip frame_counter vblank divider dt_ms bytes ranges "
         "[out_bytes out_ranges] [mod_bytes] flags");
    outf("# flags: R exec changed between invocations, X exec changed inside inv, D divider request (event line says "
         "swallowed/passed), M module (un)load, V heap moved");
    out_flush();

    memset(g.n_inv, 0, sizeof(g.n_inv));
    g.n_reforced = g.n_changed_in = g.n_div_events = g.n_mod_events = g.n_moves = g.n_faults = 0;
    g.mod_bytes_total = 0;
    g.inv = 0;
    g.abort_req = 0;
    g.in_inv = 0;
    g.have_prev_post = 0;
    g.warm_left = g_ccfg.warmup;
    g_pending_load = 0;
    g_div_req_count = 0;
    g_mod_unload_count = 0;
    g.t_prev.QuadPart = 0;
    g.state = ST_WARMUP;
    log_line("CAPTURE START scenario=%s mode=observe boundary=%s N=%d warmup=%d -> %ls", g_ccfg.scenario,
             boundary_name(g.boundary), g_ccfg.invocations, g_ccfg.warmup, g.path);
}

// ---------------------------------------------------------------- summary
struct SumCtx { int cls; int lines; int n; };

static void sum_piece(Region& r, uint32_t s, uint32_t e, void* vctx) {
    SumCtx* sc = (SumCtx*)vctx;
    uint16_t* c = r.cnt[sc->cls];
    unsigned mn = 0xFFFF, mx = 0, dirty = 0;
    for (uint32_t w = s >> 2; w <= (e - 1) >> 2; w++) {
        uint16_t v = c[w];
        unsigned lanes = v & 0xF;
        uint32_t wb = w * 4;
        for (int k = 0; k < 4; k++) {               // only lanes inside [s,e)
            if (wb + (uint32_t)k < s || wb + (uint32_t)k >= e) lanes &= ~(1u << k);
        }
        if (!lanes) continue;
        unsigned h = v >> 4;
        if (h < mn) mn = h;
        if (h > mx) mx = h;
        dirty += (unsigned)__popcnt(lanes);
    }
    if (!dirty) return;
    sc->lines++;
    if (sc->lines > g_ccfg.report_cap) {
        if (sc->lines == g_ccfg.report_cap + 1) outf("  ... report_cap %d reached for %s", g_ccfg.report_cap, CLS_NAME[sc->cls]);
        return;
    }
    char a[96], l[96];
    fmt_addr(r, s, a, sizeof(a));
    fmt_label(r.rt + s, l, sizeof(l));
    int den = sc->n;
    outf("  %-28s %s len=%-6u dirty=%-6u hits=%u..%u/%d%s%s", r.name, a, e - s, dirty, mn, mx, den,
         (mx >= 0x0FFF) ? "(sat)" : "", l);
}

static void write_class_report(int cls) {
    if (!g.n_inv[cls]) return;
    outf("");
    outf("== %s: %d invocations ==", CLS_NAME[cls], g.n_inv[cls]);
    SumCtx sc = { cls, 0, g.n_inv[cls] };
    for (LONG i = 0; i < g_nreg; i++) {
        Region& r = g_reg[i];
        uint16_t* c = r.cnt[cls];
        if (!c) continue;
        uint32_t words = r.size / 4;
        bool open = false;
        uint32_t s = 0, e = 0;
        const __m128i z = _mm_setzero_si128();
        for (uint32_t w = 0; w < words;) {
            if ((w & 7) == 0 && w + 8 <= words &&
                _mm_movemask_epi8(_mm_cmpeq_epi16(_mm_load_si128((const __m128i*)(c + w)), z)) == 0xFFFF) {
                w += 8;
                continue;
            }
            uint16_t v = c[w];
            if (v) {
                unsigned lanes = v & 0xF;
                unsigned long lo, hi;
                _BitScanForward(&lo, lanes);
                _BitScanReverse(&hi, lanes);
                uint32_t bs = w * 4 + lo, be = w * 4 + hi + 1;
                if (open && bs <= e + (uint32_t)g_ccfg.merge_gap) {
                    if (be > e) e = be;
                } else {
                    if (open) subtract_allow(r, s, e, true, sum_piece, &sc);
                    open = true;
                    s = bs;
                    e = be;
                }
            }
            w++;
        }
        if (open) subtract_allow(r, s, e, true, sum_piece, &sc);
    }
    outf("== %s: %d range lines ==", CLS_NAME[cls], sc.lines);
}

static void capture_stop(const char* why) {
    if (g.state == ST_WARMUP) {
        outf("# STOPPED during warmup (%s) - no data", why);
        out_close();
        free_regions();
        g.state = ST_IDLE;
        log_line("CAPTURE STOP during warmup (%s)", why);
        return;
    }

    outf("");
    outf("# stop: %s after %d/%d invocations; frame_counter %u -> %u, vblank %u -> %u", why, g.inv, g_ccfg.invocations,
         g.fc_start, rd32(g_frame_counter), g.vb_start, rd32(g_vblank));
    outf("# counts: BUILD=%d LOGIC=%d BACKLOG=%d OUTSIDE=%d STOCK=%d  reforced(R)=%d changed_in(X)=%d divider_requests(D)=%d "
         "module_events(M)=%d heap_moves(V)=%d faults=%d mod_bytes=%llu",
         g.n_inv[CLS_BUILD], g.n_inv[CLS_LOGIC], g.n_inv[CLS_BACKLOG], g.n_inv[CLS_OUTSIDE], g.n_inv[CLS_STOCK],
         g.n_reforced, g.n_changed_in, g.n_div_events, g.n_mod_events, g.n_moves, g.n_faults,
         (unsigned long long)g.mod_bytes_total);

    outf("");
    outf("== regions: changed bytes per class (raw, before allow-list) ==");
    for (LONG i = 0; i < g_nreg; i++) {
        Region& r = g_reg[i];
        outf("  %-28s size=0x%08X BUILD=%llu LOGIC=%llu BACKLOG=%llu OUTSIDE=%llu STOCK=%llu%s%s", r.name, r.size,
             (unsigned long long)r.changed[CLS_BUILD], (unsigned long long)r.changed[CLS_LOGIC],
             (unsigned long long)r.changed[CLS_BACKLOG], (unsigned long long)r.changed[CLS_OUTSIDE],
             (unsigned long long)r.changed[CLS_STOCK],
             r.dead ? " DISABLED" : "", r.added_at ? " (added mid-capture: hit denominators too high)" : "");
    }
    for (int c = 0; c < CLS_COUNT; c++) write_class_report(c);

    outf("");
    outf("== allow-list suppression (bytes, summed over classes) ==");
    for (int i = 0; i < g_nrules; i++)
        outf("  allow.txt:%-4d %-24s suppressed=%llu ; %s", g_rules[i].line, g_rules[i].region_glob,
             (unsigned long long)g_rules[i].suppressed_bytes, g_rules[i].reason);
    outf("# end");
    out_close();
    free_regions();
    g.state = ST_IDLE;
    log_line("CAPTURE STOP (%s): %d invocations, BUILD=%d LOGIC=%d BACKLOG=%d STOCK=%d -> %ls", why, g.inv,
             g.n_inv[CLS_BUILD], g.n_inv[CLS_LOGIC], g.n_inv[CLS_BACKLOG], g.n_inv[CLS_STOCK], g.path);
}

// Disables the capture only; fps60 (pacing, rollback, rows) keeps running.
void capture_on_fault(const char* where, DWORD code) {
    if (InterlockedExchange(&g_cap_dead, 1) == 0)
        log_line("capture FAULT 0x%08lX in %s - capture disabled (pass-through), fps60 keeps running", code, where);
    if (g.state != ST_IDLE) {
        __try {
            outf("# ABORTED: fault 0x%08lX in %s", code, where);
            out_close();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try { free_regions(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g.state = ST_IDLE;
    }
}

// fps60 itself faulted (hooks.cpp on_fault): no more capture points will run.
void capture_abort(const char* why) {
    if (!g_cap_on || g.state == ST_IDLE) return;
    InterlockedExchange(&g_cap_dead, 1);
    __try {
        outf("# ABORTED: %s", why);
        out_close();
        free_regions();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g.state = ST_IDLE;
    log_line("CAPTURE ABORTED (%s)", why);
}

// ---------------------------------------------------------------- per-invocation capture points
static bool hotkey_pressed() {
    bool down = (GetAsyncKeyState(g_ccfg.hotkey) & 0x8000) != 0;
    bool edge = down && !g_hotkey_down;
    g_hotkey_down = down;
    if (!edge) return false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void diff_all(int cls, uint32_t* bytes, uint32_t* ranges) {
    *bytes = 0;
    *ranges = 0;
    AcquireSRWLockShared(&g_lock);
    for (LONG i = 0; i < g_nreg; i++) {
        Region& r = g_reg[i];
        if (r.dead) continue;
        RunAcc acc;
        memset(&acc, 0, sizeof(acc));
        acc.cls = cls;
        diff_region(r, cls, &acc);
        *bytes += acc.bytes;
        *ranges += acc.ranges;
    }
    ReleaseSRWLockShared(&g_lock);
}

// Diff with no class: refreshes the snapshot over fps60's own writes (inner boundary).
static void absorb_mod_writes() {
    uint32_t b, rg;
    diff_all(CLS_NONE, &b, &rg);
    g.mod_bytes += b;
    g.mod_bytes_total += b;
}

static void handle_async_events() {
    LONG unl = InterlockedExchange(&g_mod_unload_count, 0);
    if (unl) {
        g.n_mod_events += unl;
        g.flags[4] = 'M';
        outf("  event: %ld module section region(s) unloaded before inv %d - disabled", unl, g.inv + 1);
    }
    if (InterlockedExchange(&g_pending_load, 0)) {
        int added = scan_modules_for_sections(true, g.state == ST_CAPTURE ? g.inv + 1 : 0);
        if (added) {
            g.n_mod_events++;
            g.flags[4] = 'M';
            outf("  event: %d section region(s) added before inv %d (module loaded mid-capture; baseline taken now):", added, g.inv + 1);
            for (LONG i = g_nreg - added; i < g_nreg; i++)
                outf("    + %s rt=0x%08X size=0x%X gva=0x%08X", g_reg[i].name, (uint32_t)(uintptr_t)g_reg[i].rt,
                     g_reg[i].size, g_reg[i].gva_base);
        }
    }
    LONG dr = InterlockedExchange(&g_div_req_count, 0);
    if (dr) {
        g.n_div_events += dr;
        g.flags[2] = 'D';
        outf("  event: game asked SKS_SetFrameDivider(%ld) x%ld before inv %d - last one %s by fps60 pacing",
             g_div_req_last, dr, g.inv + 1, g_div_req_swallowed ? "swallowed" : "passed");
    }
}

// PRE_OUTER: the true boundary, before pacing_pre and any row pre-write.
static void point_pre_outer() {
    if (g.state == ST_IDLE) {
        if (!hotkey_pressed()) return;
        capture_start();
        if (g.state == ST_IDLE) return;
    } else if (hotkey_pressed()) {
        g.abort_req = 1;
        log_line("capture: hotkey pressed - stopping after this invocation");
    }
    g.in_inv = 1;
    memcpy(g.flags, "-----", 6);
    g.mod_bytes = 0;
    g.bytes = g.ranges = 0;
    handle_async_events();
    check_heaps();

    int8_t exec_now = *g_exec_byte;
    if (g.state == ST_CAPTURE && g.have_prev_post && exec_now != g.exec_prev_post) {
        g.n_reforced++;
        g.flags[0] = 'R';
        outf("  event: exec %d -> %d between invocations (before inv %d)", g.exec_prev_post, exec_now, g.inv + 1);
    }

    g.out_bytes = g.out_ranges = 0;
    if (g.state == ST_CAPTURE && g.split_outside) {
        diff_all(CLS_OUTSIDE, &g.out_bytes, &g.out_ranges);
        g.n_inv[CLS_OUTSIDE]++;
    }
    g.backlog_at_pre = *g_backlog_count;
}

// PRE_PACED: after pacing_pre. Class from fps60's pacing; its exec/divider writes are not the game's.
static void point_pre_paced() {
    if (!g.in_inv) return;
    g.cur_cls = kind_to_cls(pacing_kind());
    if (g.boundary == BOUNDARY_OUTER) {
        snap_refresh((const void*)g_exec_byte, 1);
        if (g_divider) snap_refresh((const void*)g_divider, 4);
    }
    g.exec_start = *g_exec_byte;
    g.eff_start = eff_exec();
}

// PRE_INNER: after rollback_pre/rows_pre, right before Sg_MainLoop.
static void point_pre_inner() {
    if (!g.in_inv || g.boundary != BOUNDARY_INNER) return;
    if (g.state == ST_CAPTURE) absorb_mod_writes();   // pacing exec/divider + row pre-writes
    g.exec_start = *g_exec_byte;
}

// POST_INNER: right after Sg_MainLoop, before rows_post/rollback_post.
static void point_post_inner() {
    if (!g.in_inv || g.boundary != BOUNDARY_INNER) return;
    g.exec_end = *g_exec_byte;
    if (g.state == ST_CAPTURE) diff_all(g.cur_cls, &g.bytes, &g.ranges);
}

// POST_OUTER: end of fp_post. Warmup/baseline, the outer class diff, the inv line, stop.
static void point_post_outer() {
    if (!g.in_inv) return;
    g.in_inv = 0;
    if (g.boundary == BOUNDARY_OUTER) g.exec_end = *g_exec_byte;
    g.exec_prev_post = *g_exec_byte;
    g.have_prev_post = 1;

    if (g.state == ST_WARMUP) {
        if (--g.warm_left > 0 && !g.abort_req) return;
        if (g.abort_req) { capture_stop("hotkey during warmup"); return; }
        AcquireSRWLockShared(&g_lock);
        for (LONG i = 0; i < g_nreg; i++) {
            if (!g_reg[i].dead && !snapshot_region(g_reg[i])) InterlockedExchange(&g_reg[i].dead, 1);
        }
        ReleaseSRWLockShared(&g_lock);
        g.state = ST_CAPTURE;
        QueryPerformanceCounter(&g.t_prev);
        outf("# baseline taken after warmup (at fp_post end); frame_counter=%u vblank=%u exec=%d", rd32(g_frame_counter),
             rd32(g_vblank), *g_exec_byte);
        log_line("capture: warmup done, baseline taken, counting %d invocations", g_ccfg.invocations);
        return;
    }

    if (g.boundary == BOUNDARY_OUTER) diff_all(g.cur_cls, &g.bytes, &g.ranges);
    else absorb_mod_writes();                         // row post-writes + rollback restores
    g.n_inv[g.cur_cls]++;
    g.inv++;
    if (g.exec_end != g.exec_start) {
        g.n_changed_in++;
        g.flags[1] = 'X';
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = qpc_ms(g.t_prev, now);
    g.t_prev = now;

    char tail[64];
    tail[0] = 0;
    if (g.boundary == BOUNDARY_INNER) snprintf(tail, sizeof(tail), " mod_bytes=%u", g.mod_bytes);
    if (g.split_outside)
        outf("inv %4d %-7s exec=%d->%d eff=%d bl=%d skip=%d fc=%u vb=%u div=%u dt=%.2f bytes=%u ranges=%u out_bytes=%u out_ranges=%u%s %s",
             g.inv, CLS_NAME[g.cur_cls], g.exec_start, g.exec_end, g.eff_start, g.backlog_at_pre, g_skip_render ? *g_skip_render : -1,
             rd32(g_frame_counter), rd32(g_vblank), rd32(g_divider), dt, g.bytes, g.ranges, g.out_bytes, g.out_ranges, tail, g.flags);
    else
        outf("inv %4d %-7s exec=%d->%d eff=%d bl=%d skip=%d fc=%u vb=%u div=%u dt=%.2f bytes=%u ranges=%u%s %s",
             g.inv, CLS_NAME[g.cur_cls], g.exec_start, g.exec_end, g.eff_start, g.backlog_at_pre, g_skip_render ? *g_skip_render : -1,
             rd32(g_frame_counter), rd32(g_vblank), rd32(g_divider), dt, g.bytes, g.ranges, tail, g.flags);

    if (g.inv >= g_ccfg.invocations) capture_stop("N reached");
    else if (g.abort_req) capture_stop("hotkey");
}

void capture_point(int point) {
    if (!g_cap_on || g_cap_dead) return;
    switch (point) {
    case CAP_PRE_OUTER:  point_pre_outer();  break;
    case CAP_PRE_PACED:  point_pre_paced();  break;
    case CAP_PRE_INNER:  point_pre_inner();  break;
    case CAP_POST_INNER: point_post_inner(); break;
    case CAP_POST_OUTER: point_post_outer(); break;
    default: break;
    }
}
