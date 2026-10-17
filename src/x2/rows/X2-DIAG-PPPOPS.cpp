// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-PPPOPS (diagnostic, default 0): counts the calls of every particle draw op (descriptor slot +0xC, the
// pppDraw* / pppKe* ops) with the particle pause flag set (render-only steps, X2-MAG-006) and clear, and logs the
// ops that ran every 10 s. Which op a blinking effect uses (U 16/10/2026: mission-start hex grid, magic_0718) decides
// which pause gate needs the DRAWOPS treatment. Observe only: each op is entry-hooked and forwarded unchanged.
// Descriptor records {char* name; +4 group op; +8 calc op; +0xC draw op; ...} (0x28 bytes, .data, two copies of one
// table), found from the record that names "pppDrawShapeCamera" (outer repo re/ledger/DRAWCAM.md D1, S).
#include "../x2.h"
#include <string.h>

namespace x2 {

extern Row row_DIAGPPPOPS;

enum { MAXOPS = 128 };
typedef void(__cdecl* op_t)(void*, void*, void*);    // (particle, keydata, opdata), cdecl, plain RET (DRAWCAM D3 shape)
static op_t        g_orig[MAXOPS];
static uint8_t*    g_fn[MAXOPS];
static const char* g_name[MAXOPS];
static int         g_n = 0;
static volatile int32_t* g_pause = nullptr;
static volatile LONG g_paused[MAXOPS], g_live[MAXOPS], g_last_paused[MAXOPS], g_last_live[MAXOPS];

// X2-DIAG-PPPOPS-CALLERS (default "pppKeShpTail3X pppKeLnsFls"): for these ops, distinct call chains (5 return
// addresses as gva) with counts and the pause state, logged with the 10 s counts (U 17/10/2026 Full Throttle glow:
// these ops are never reached on paused steps; which dispatcher calls them).
static bool g_trace[MAXOPS];
struct Chain { uint32_t op; uint32_t f[9]; int paused; LONG n; };
enum { MAXCH = 32 };
static Chain g_ch[MAXCH];
static int g_nch = 0;
static void note_chain(int i) {
    void* fr[10] = {};
    USHORT k = RtlCaptureStackBackTrace(2, 9, fr, nullptr);
    Chain c = {};
    c.op = (uint32_t)i;
    c.paused = *g_pause ? 1 : 0;
    for (int j = 0; j < k && j < 9; j++) c.f[j] = in_image(fr[j]) ? rt_to_gva(fr[j]) : (uint32_t)(uintptr_t)fr[j];
    for (int j = 0; j < g_nch; j++)
        if (g_ch[j].op == c.op && g_ch[j].paused == c.paused && !memcmp(g_ch[j].f, c.f, sizeof(c.f))) { g_ch[j].n++; return; }
    if (g_nch < MAXCH) { c.n = 1; g_ch[g_nch++] = c; }
}
// Per (op, program) counts: the current program is [0x01CF5A44] (Ppp_ProgramStep 0x7FEE2E stores it before the group
// loop). Logged every 10 s for pairs whose paused calls fall below 80 % of their live calls (live >= 40): the program
// that draws less on paused steps (U 17/10/2026: Full Throttle glow blink, F11 fixes it; pppKeTh paused 105 / live 325).
static void* volatile* g_curprog = (void* volatile*)0x01CF5A44;   // rebased at install
struct PP { void* prog; int op; LONG p, l; };
enum { MAXPP = 256 };
static PP g_pp[MAXPP];
static int g_npp = 0;
static void note_pp(int i, bool paused) {
    void* pr = *g_curprog;
    for (int j = 0; j < g_npp; j++)
        if (g_pp[j].prog == pr && g_pp[j].op == i) { if (paused) g_pp[j].p++; else g_pp[j].l++; return; }
    if (g_npp < MAXPP) { g_pp[g_npp] = { pr, i, paused ? 1 : 0, paused ? 0 : 1 }; g_npp++; }
}
template <int I> static void __cdecl h_op(void* a, void* b, void* c) {
    InterlockedIncrement(*g_pause ? &g_paused[I] : &g_live[I]);
    note_pp(I, *g_pause != 0);
    if (g_trace[I]) note_chain(I);
    g_orig[I](a, b, c);
}
template <int... I> struct Tbl { static constexpr void* v[sizeof...(I)] = { (void*)&h_op<I>... }; };
template <int N, int... I> struct Make : Make<N - 1, N - 1, I...> {};
template <int... I> struct Make<0, I...> { using type = Tbl<I...>; };
static void* const* const DETOURS = Make<MAXOPS>::type::v;

static bool image(uint8_t** lo, uint32_t* size) {
    uint8_t* b = exe_base();
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    *lo = b;
    *size = nt->OptionalHeader.SizeOfImage;
    return true;
}

static bool is_name(uint8_t* lo, uint32_t size, uint32_t p) {
    if (p < (uint32_t)lo || p - (uint32_t)lo > size - 4) return false;
    const char* s = (const char*)p;
    for (int i = 0; i < 3; i++) if (s[i] < 0x21 || s[i] > 0x7E) return false;
    return true;
}

// Only ops inside an executable section are hooked (one table record points its draw slot into .data: "STOP").
static bool in_code(const uint8_t* p) {
    uint8_t* b = exe_base();
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const uint8_t* lo = b + sec[i].VirtualAddress;
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && p >= lo && p < lo + sec[i].Misc.VirtualSize) return true;
    }
    return false;
}

static bool install() {
    uint8_t* lo; uint32_t size;
    image(&lo, &size);
    static const char key[] = "pppDrawShapeCamera";
    uint8_t* str = nullptr;
    for (uint8_t* a = lo; a + sizeof(key) <= lo + size; a++)
        if (a[0] == 'p' && !memcmp(a, key, sizeof(key))) { str = a; break; }     // sizeof includes the NUL
    if (!str) { log_line("  X2-DIAG-PPPOPS: op name not found - off"); return false; }
    uint8_t* rec = nullptr;
    for (uint8_t* a = lo; a + 4 <= lo + size; a += 4)
        if (*(uint32_t*)a == (uint32_t)str) { rec = a; break; }
    if (!rec) { log_line("  X2-DIAG-PPPOPS: op record not found - off"); return false; }
    while (rec - 0x28 >= lo && is_name(lo, size, *(uint32_t*)(rec - 0x28))) rec -= 0x28;
    int recs = 0;
    for (; rec + 0x28 <= lo + size && is_name(lo, size, *(uint32_t*)rec); rec += 0x28, recs++) {
        uint8_t* fn = *(uint8_t**)(rec + 0xC);
        if (!fn || !in_code(fn)) continue;
        bool seen = false;
        for (int i = 0; i < g_n; i++) if (g_fn[i] == fn) { seen = true; break; }
        if (seen || g_n >= MAXOPS) continue;
        g_fn[g_n] = fn;
        g_name[g_n] = *(const char**)rec;
        g_n++;
    }
    // pause flag: the CMP operand at pppDrawShapeCamera +0x224 (DRAWCAM D2)
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_name[i], key) && sig_match(g_fn[i] + 0x224, "83 3D ?? ?? ?? ?? 00 75"))
            g_pause = (volatile int32_t*)abs_operand("particle pause flag", g_fn[i] + 0x226);
    if (!g_pause) { log_line("  X2-DIAG-PPPOPS: pause flag not found - off"); return false; }
    // current program pointer: Ppp_ProgramStep 0x7FEE2E `MOV [0x01CF5A44],ESI` (89 35 abs)
    {
        uint8_t* st = sig_find("X2-DIAG-PPPOPS current program store",
            "F6 45 0C 10 89 35 ?? ?? ?? ?? 74 23 33 FF 39 7E 14", 0x7FEE2A);
        uint8_t* cp = st ? abs_operand("current program", st + 6) : nullptr;
        if (!cp) { log_line("  X2-DIAG-PPPOPS: current program store not found - off"); return false; }
        g_curprog = (void* volatile*)cp;
    }
    char tr[256];
    config_str("rows", "X2-DIAG-PPPOPS-CALLERS", tr, sizeof(tr), "pppKeShpTail3X pppKeLnsFls");
    for (int i = 0; i < g_n; i++) {
        const char* hit = strstr(tr, g_name[i]);
        size_t L = strlen(g_name[i]);
        while (hit && ((hit > tr && hit[-1] != ' ' && hit[-1] != ',') || (hit[L] && hit[L] != ' ' && hit[L] != ','))) hit = strstr(hit + 1, g_name[i]);
        g_trace[i] = hit != nullptr;
    }
    log_line("  X2-DIAG-PPPOPS: %d records, %d distinct draw ops; pause flag gva=0x%08X; hooks at the first pre()", recs, g_n,
             rt_to_gva((const void*)g_pause));
    return g_n > 0;
}

static void pre() {
    static bool done = false;
    if (done) return;
    done = true;
    int hooked = 0;                              // game thread, between invocations: no op is running
    for (int i = 0; i < g_n; i++)
        if (hook_entry("X2-DIAG-PPPOPS", g_fn[i], DETOURS[i], (void**)&g_orig[i])) hooked++;
        else log_line("X2-DIAG-PPPOPS: %s gva=0x%08X not hooked", g_name[i], rt_to_gva(g_fn[i]));
    log_line("X2-DIAG-PPPOPS: %d of %d draw ops hooked", hooked, g_n);
}

static void post() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 10000) return;
    last = now;
    for (int i = 0; i < g_n; i++) {
        LONG p = g_paused[i] - g_last_paused[i], l = g_live[i] - g_last_live[i];
        if (!p && !l) continue;
        g_last_paused[i] = g_paused[i];
        g_last_live[i] = g_live[i];
        log_line("X2-DIAG-PPPOPS: %-30s gva=0x%08X  paused %ld  live %ld (10 s)", g_name[i], rt_to_gva(g_fn[i]), p, l);
    }
    for (int j = 0; j < g_npp; j++) {
        PP& q = g_pp[j];
        if (q.l >= 40 && q.p * 5 < q.l * 4) {
            char mod[64] = "?"; HMODULE hm = nullptr; uint32_t off = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)q.prog, &hm) && hm) {
                char path[MAX_PATH]; GetModuleFileNameA(hm, path, MAX_PATH);
                const char* b = strrchr(path, '\'); strncpy_s(mod, b ? b + 1 : path, _TRUNCATE);
                off = (uint32_t)((uint8_t*)q.prog - (uint8_t*)hm);
            }
            log_line("X2-DIAG-PPPOPS prog %p (%s+0x%X) %-24s paused %ld live %ld", q.prog, mod, off, g_name[q.op], q.p, q.l);
        }
    }
    g_npp = 0;
    for (int j = 0; j < g_nch; j++) {
        Chain& c = g_ch[j];
        if (!c.n) continue;
        log_line("X2-DIAG-PPPOPS caller: %s %s x%ld <- %08X <- %08X <- %08X <- %08X <- %08X <- %08X <- %08X <- %08X <- %08X",
                 g_name[c.op], c.paused ? "PAUSED" : "live", c.n, c.f[0], c.f[1], c.f[2], c.f[3], c.f[4], c.f[5], c.f[6], c.f[7], c.f[8]);
        c.n = 0;
    }
}

Row row_DIAGPPPOPS = {
    "X2-DIAG-PPPOPS",
    "diagnostic: per particle draw op, calls with the pause flag set / clear (which op blinks)",
    0, install, pre, post, 0
};

} // namespace x2
