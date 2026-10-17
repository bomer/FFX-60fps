// SPDX-License-Identifier: Apache-2.0
// QOL-RETRY (gameplay option, default off): after a party wipe, the game's own choice window asks "Retry / Give up".
// Retry restarts the same battle with the party as it was when the battle started; Give up is the stock game over.
// Static RE: re/ledger/RETRY-BTL.md (battle start/end), re/ledger/RETRY-UI.md (choice window). Ghidra VAs below.
//
// - Snapshot: MsBattleSetupEncounter 0x781020 (cdecl(mode, area, group, form), plain RET) is the only retail battle
//   start (field branch of MsBattleProcess, 0x790F70). At its entry with mode 0: save block (0x68C0 bytes), battle
//   reward block 0x2310EA0 (0x1B0: acted/appeared/AP accumulators), battle flags b48/b50/b54, the request (a8e2,
//   c256/c258/c259, a9d4/a9d5). The battle RNG is not restored, so a retry gets fresh rolls.
// - Prompt: during the game-over sequence (battle state a8e0 0x1E..0x20, result a8e3 == 1, loss-allowed bit 0x200 of
//   b48 clear; after the SR-008 soft-reset chord too unless QOL-RETRY-CHORD=0), window slot 7 (free after state 0x17) opens with our own text and two
//   choices; the end-of-battle timer c9ec is held at 8 every invocation, which freezes the state machine
//   (0x790D2D) while window logic keeps running. Cancel moves the cursor to Give up.
// - Give up: release the timer; the stock chain goes on to teardown and the title.
// - Retry: release the timer; at teardown FUN_00861160 0x861160 (cdecl(scripted, encKey, stage, result), plain RET)
//   is detoured: the snapshot is restored and it returns 1 without the title or post-battle path. The next
//   invocation re-writes the request (a9d4 is zeroed at 0x790EF2 after the call), and the field branch starts the same
//   battle. If no battle starts within 60 invocations, the original FUN_00861160 runs with the saved arguments.
#include "../fps60.h"
#include <stdio.h>
#include <string.h>

typedef void(__cdecl* setup_t)(uint32_t mode, int area, int group, int form);
typedef int(__cdecl* post_t)(int scripted, uint32_t key, int stage, int result);
typedef int(__cdecl* w1_t)(int);
typedef int(__cdecl* w2_t)(int, int);
typedef int(__cdecl* w3_t)(int, int, int);
typedef int(__cdecl* w4_t)(int, int, int, int);
typedef int(__cdecl* w5_t)(int, int, int, int, int);

enum { SAVE_MAX = 0x8000, REWARD_LEN = 0x1B0, WIN = 7, HOLD = 8, REISSUE_WAIT = 60 };

static uint8_t* g_setup_fn = nullptr; static setup_t g_setup = nullptr;
static uint8_t* g_post_fn = nullptr;  static post_t  g_post = nullptr;
static uint8_t* g_busy_fn = nullptr;
static w1_t g_w_reset, g_w_focus, g_w_open, g_w_close, g_w_state;
static w2_t g_w_anchor2;
static w3_t g_w_style, g_w_text;
static w4_t g_w_anchor, g_w_choice;
static w5_t g_w_color;
static uint8_t** g_choice_tbl = nullptr;           // DAT_01868a90: per-window choice object
static uint8_t** g_win_tbl = nullptr;              // DAT_01865b3c: per-window object (+6 = input focus)
static volatile uint8_t* g_c9ec = nullptr;         // end-of-battle timer
static volatile uint8_t *g_a8e0, *g_a8e2, *g_a8e3;
static volatile uint8_t* g_c9f1 = nullptr;         // pending battle result (1 = party wiped)
typedef uint32_t(__cdecl* party_t)();
static party_t g_party = nullptr;                   // FUN_007928d0: 1 = party wiped (c9f1 if set)
static volatile uint8_t *g_c258, *g_c259, *g_a9d4, *g_a9d5;
static volatile uint16_t* g_c256;
static volatile uint32_t* g_b48;                   // b48, b50 = +8, b54 = +0xC
static uint8_t* g_save = nullptr; static uint32_t g_save_len = 0;
static uint8_t* g_reward = nullptr;

static uint8_t  g_snap_save[SAVE_MAX], g_snap_reward[REWARD_LEN];
static uint32_t g_snap_b48[3];
static const int k_b48_idx[3] = { 0, 2, 3 };       // b48, b50 (+8), b54 (+0xC)
static uint8_t  g_snap_a8e2, g_snap_c258, g_snap_c259, g_snap_a9d4, g_snap_a9d5;
static uint16_t g_snap_c256;
static bool     g_snap_ok = false;

enum { ST_IDLE, ST_PROMPT, ST_CLOSING, ST_RELEASED };
static int  g_state = ST_IDLE;
static bool g_retry = false;                        // player chose Retry, waiting for teardown
static bool g_early = false;                        // prompt opened at state 0x18, before the GAME OVER sequence
static bool g_quiet = false;                        // Retry from the early prompt: loss-allowed bit set for a quiet end
static int  g_reissue = 0;                          // invocations since the retry was re-armed (0 = none)
static int  g_post_args[4];
static bool g_patched = false;
static uint32_t g_retries = 0;
static int      g_refocus = 0;
static int      g_prompt_inv = 0;                    // invocations since the prompt opened
static bool     g_prompt_seen = false;               // the window left state 0/4 at least once
static bool     g_chord = true;                     // QOL-RETRY-CHORD: prompt after the SR-008 soft-reset chord too

// {09 30}{10 30}Retry{03}{10 31}Give up{00} (stock save-sphere layout, RETRY-UI-012; line feed 0x03), the words in the
// current text language (text.cpp), built when the prompt opens.
static uint8_t g_text[96];

static void build_text() {
    static const uint8_t head[] = { 0x09, 0x30, 0x10, 0x30 }, mid[] = { 0x03, 0x10, 0x31 };
    const char* a = (const char*)ffx_text(T_P_RETRY);
    const char* b = (const char*)ffx_text(T_P_GIVEUP);
    size_t la = strlen(a), lb = strlen(b), n = 0;
    if (sizeof(head) + la + sizeof(mid) + lb + 1 > sizeof(g_text)) { a = b = ""; la = lb = 0; }
    memcpy(g_text + n, head, sizeof(head)); n += sizeof(head);
    memcpy(g_text + n, a, la); n += la;
    memcpy(g_text + n, mid, sizeof(mid)); n += sizeof(mid);
    memcpy(g_text + n, b, lb); n += lb;
    g_text[n] = 0;
}
static int g_style = 0;                             // QOL-RETRY-STYLE: 0 field dialogue (choices), 1 battle caption

// QOL-RETRY-DIFF (diagnostic, default 0): find battle state the retry does not reset (U 18/10/2026: Yunalesca retried
// in her second form starts the new battle in that form). Copies FFX.exe's writable section when the first battle
// reaches state 1 (fresh) and when the prompt opens (end of the lost attempt); when the retry reaches state 1 it logs
// every range that differs from the fresh copy and equals the end copy, i.e. state carried over from the lost attempt.
static int g_diff = 0;
static int g_diff_phase = 0;                        // 1 wait fresh state 1, 2 wait prompt, 3 wait retry setup, 4 wait retry state 1
// Watched regions: [0] FFX.exe's largest writable section (.data with bss); [1] the heap allocation that holds the
// battle file buffer DAT_0112a9a8 (outside .data, D 18/10/2026 03:09:52), found at the fresh copy.
struct DiffRgn { uint8_t* p; uint32_t len; uint8_t* fresh; uint8_t* end; uint8_t* ok; };   // ok: per 4 KB page, 1 = in both copies
static DiffRgn g_rgn[2];
// Transformation window (D 18/10/2026 03:21: the chr-name buffer at Ghidra VA 0x012FC5C4 held "130/m130" at the fresh
// state 1 and "175/m175" at the retry's): a .data copy every 15 invocations of the lost attempt; when that buffer changes,
// every dword that changed since the previous copy is marked. The report lists carried dwords that were marked.
enum { WATCH_GVA = 0x012FC5C4, WIN_INV = 15 };
static uint8_t* g_prev = nullptr;
static uint32_t* g_tbits = nullptr;
static uint64_t g_watch = 0;
static int g_tick = 0;

static uint8_t* diff_buf(uint32_t len) { return (uint8_t*)VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
static void diff_free(DiffRgn& r) {
    if (r.fresh) VirtualFree(r.fresh, 0, MEM_RELEASE);
    if (r.end) VirtualFree(r.end, 0, MEM_RELEASE);
    if (r.ok) VirtualFree(r.ok, 0, MEM_RELEASE);
    memset(&r, 0, sizeof(r));
}

static bool diff_alloc() {
    DiffRgn& d = g_rgn[0];
    if (d.fresh) return true;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe_base();
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(exe_base() + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) || (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint32_t len = sec[i].Misc.VirtualSize & ~3u;
        if (len > d.len) { d.p = exe_base() + sec[i].VirtualAddress; d.len = len; }
    }
    if (!d.len) return false;
    d.fresh = diff_buf(d.len); d.end = diff_buf(d.len); d.ok = diff_buf(d.len / 0x1000 + 1);
    g_prev = diff_buf(d.len); g_tbits = (uint32_t*)diff_buf(d.len / 32 + 4);
    if (!d.fresh || !d.end || !d.ok || !g_prev || !g_tbits) {
        log_line("QOL-RETRY-DIFF: no memory for 2 x 0x%X bytes - off", d.len);
        diff_free(d);
        g_diff = 0;
        return false;
    }
    log_line("QOL-RETRY-DIFF: watching gva 0x%08X..0x%08X", rt_to_gva_exe(d.p), rt_to_gva_exe(d.p) + d.len);
    return true;
}

// The heap span can be decommitted between the copies (D 18/10/2026 03:16:11: the end copy faulted, the row went off
// with the prompt up and the battle hung): copy and compare only pages that are committed and readable.
static bool page_readable(const uint8_t* p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mbi.Protect & 0xFF) != 0;
}

static void hex16(char* out, const uint8_t* p, uint32_t n) {
    static const char hx[] = "0123456789ABCDEF";
    if (n > 16) n = 16;
    for (uint32_t i = 0; i < n; i++) { out[i * 3] = hx[p[i] >> 4]; out[i * 3 + 1] = hx[p[i] & 15]; out[i * 3 + 2] = ' '; }
    out[n ? n * 3 - 1 : 0] = 0;
}

// Every carried range goes to fps60_retrydiff.txt next to FFX.exe; the log gets the totals and the first 40 per region.
static void diff_report() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* sl = strrchr(path, '\\');
    if (sl) strcpy(sl + 1, "fps60_retrydiff.txt");
    FILE* f = fopen(path, "w");
    uint8_t* file = *(uint8_t* volatile*)(g_a8e0 + 0xC8);
    if (f) fprintf(f, "QOL-RETRY-DIFF: retry state 1 vs fresh state 1, ranges equal to the end of the lost attempt\n"
                      "battle file buffer now %p\n", file);
    for (int k = 0; k < 2; k++) {
        DiffRgn& r = g_rgn[k];
        if (!r.fresh) continue;
        const uint32_t* cur = (const uint32_t*)r.p;
        const uint32_t* fr = (const uint32_t*)r.fresh;
        const uint32_t* en = (const uint32_t*)r.end;
        uint32_t n = r.len / 4, changed = 0, carried = 0, ranges = 0, logged = 0;
        uint32_t start = 0, last = 0;
        bool open = false;
        auto flush = [&]() {
            ranges++;
            uint32_t off = start * 4, len = (last - start + 1) * 4;
            char a[64], b[64];
            hex16(a, r.fresh + off, len); hex16(b, r.p + off, len);
            uint32_t addr = k == 0 ? rt_to_gva_exe(r.p + off) : off;
            if (f) fprintf(f, "%s 0x%08X len 0x%X fresh [%s] retry [%s]\n", k == 0 ? "gva" : "file+", addr, len, a, b);
            if (logged < 40) {
                logged++;
                log_line("QOL-RETRY-DIFF: carried %s 0x%08X len 0x%X fresh [%s] retry [%s]", k == 0 ? "gva" : "file+", addr,
                         len, a, b);
            }
        };
        for (uint32_t i = 0; i < n; i++) {
            if ((i & 0x3FF) == 0 && !(r.ok[i >> 10] && page_readable(r.p + i * 4))) { i += 0x3FF; continue; }
            uint32_t c = cur[i];
            if (c == fr[i]) continue;
            changed++;
            if (c != en[i] && k == 0) continue;     // the file: every difference from the fresh load
            carried++;
            if (open && i - last <= (k == 0 ? 4u : 1u)) { last = i; continue; }
            if (open) flush();
            start = last = i; open = true;
        }
        if (open) flush();
        log_line("QOL-RETRY-DIFF: %s: %u dwords differ, %u carried from the lost attempt in %u ranges",
                 k == 0 ? ".data" : "heap", changed, carried, ranges);
        if (f) fprintf(f, "%s: %u dwords differ, %u carried in %u ranges\n", k == 0 ? ".data" : "heap", changed, carried, ranges);
        if (k == 0 && g_tbits) {                     // carried and changed in a chr-name window of the lost attempt
            uint32_t hits = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (!(g_tbits[i >> 5] & (1u << (i & 31)))) continue;
                if (cur[i] == fr[i] || cur[i] != en[i]) continue;
                hits++;
                uint32_t gva = rt_to_gva_exe(r.p + i * 4);
                if (f) fprintf(f, "TRANS gva 0x%08X fresh %08X retry %08X\n", gva, fr[i], cur[i]);
                if (hits <= 200) log_line("QOL-RETRY-DIFF: TRANS gva 0x%08X fresh %08X retry %08X", gva, fr[i], cur[i]);
            }
            log_line("QOL-RETRY-DIFF: %u carried dwords changed in a chr-name window", hits);
        }
    }
    if (f) fclose(f);
    log_line("QOL-RETRY-DIFF: battle file buffer now %p; all ranges in %s", file, path);
}

static bool copy_page(uint8_t* dst, const uint8_t* src, uint32_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The page test alone was not enough (D 18/10/2026 03:18:45: still faulted at the end copy), so each page is copied
// under its own guard too.
static void diff_copy(bool fresh) {
    for (int k = 0; k < 2; k++) {
        DiffRgn& r = g_rgn[k];
        if (!r.fresh) continue;
        uint32_t bad = 0;
        for (uint32_t off = 0, pg = 0; off < r.len; off += 0x1000, pg++) {
            uint32_t n = r.len - off < 0x1000 ? r.len - off : 0x1000;
            bool ok = page_readable(r.p + off) && page_readable(r.p + off + n - 1) &&
                      copy_page((fresh ? r.fresh : r.end) + off, r.p + off, n);
            if (!ok) bad++;
            r.ok[pg] = fresh ? ok : (r.ok[pg] && ok);
        }
        if (bad) log_line("QOL-RETRY-DIFF: %s copy: %u of %u pages not readable (skipped)", fresh ? "fresh" : "end", bad,
                          (r.len + 0xFFF) / 0x1000);
    }
}

static char printable(char c) { return (uint8_t)c < 0x20 || (uint8_t)c > 0x7E ? '.' : c; }

static void diff_window() {                         // phase 2, each invocation
    if (++g_tick % WIN_INV) return;
    DiffRgn& d = g_rgn[0];
    uint64_t w = *(volatile uint64_t*)(exe_base() + (WATCH_GVA - 0x400000));
    if (w != g_watch) {
        const uint32_t* cur = (const uint32_t*)d.p;
        const uint32_t* pv = (const uint32_t*)g_prev;
        uint32_t n = d.len / 4, marked = 0;
        for (uint32_t i = 0; i < n; i++)
            if (cur[i] != pv[i]) { g_tbits[i >> 5] |= 1u << (i & 31); marked++; }
        char a[9], b[9];
        memcpy(a, &g_watch, 8); memcpy(b, &w, 8);
        for (int i = 0; i < 8; i++) { a[i] = printable(a[i]); b[i] = printable(b[i]); }
        a[8] = b[8] = 0;
        log_line("QOL-RETRY-DIFF: chr name \"%s\" -> \"%s\" at invocation %d: %u dwords changed in the window",
                 a, b, g_tick, marked);
        g_watch = w;
    }
    memcpy(g_prev, d.p, d.len);
}

// Enemy slots (D 18/10/2026 03:33: no form flag among the carried .data values): per monster slot i < 8, the monster
// record FUN_00795aa0(i) = i * 0xF90 + [0x01134468] (+0x0E id, +0x48 model data) and the battle block
// FUN_00794020(0x14 + i) = (0x14 + i) * 0xF90 + [0x011334D4] (+0xF60 HP, +0x5A4 max HP, +0x590 flags).
// Logged when anything in that set changes, from the fresh state 1 to 40 s into the retry.
static uint32_t g_slot_sig = 0;
static int g_slot_log_until = 0;
static void diff_slots(const char* why) {
    uint8_t* eb = exe_base();
    uint8_t* mon = *(uint8_t**)(eb + (0x01134468 - 0x400000));
    uint8_t* blk = *(uint8_t**)(eb + (0x011334D4 - 0x400000));
    if (!mon || !blk) return;
    char buf[640]; int n = 0;
    uint32_t sig = 2166136261u;
    for (int i = 0; i < 8; i++) {
        uint8_t* m = mon + i * 0xF90;
        uint8_t* b = blk + (0x14 + i) * 0xF90;
        uint16_t id = *(uint16_t*)(m + 0xE);
        int32_t hp = *(int32_t*)(b + 0xF60), mhp = *(int32_t*)(b + 0x5A4);
        uint32_t data = *(uint32_t*)(m + 0x48), fl = *(uint8_t*)(b + 0x590);
        sig = (sig ^ id) * 16777619u; sig = (sig ^ (uint32_t)hp) * 16777619u; sig = (sig ^ (data != 0)) * 16777619u;
        sig = (sig ^ fl) * 16777619u;
        if (id == 0xFFFF && !data) continue;
        n += _snprintf(buf + n, sizeof(buf) - n, " [%d id %04X data %s hp %d/%d fl %02X]", i, id, data ? "y" : "n", hp, mhp, fl);
        if (n >= (int)sizeof(buf) - 64) break;
    }
    if (why || sig != g_slot_sig) log_line("QOL-RETRY-DIFF: slots%s%s:%s", why ? " at " : "", why ? why : "", buf);
    g_slot_sig = sig;
}

// "Load Character" FUN_0082a1c0 (cdecl(path, a, b, flags), plain RET 0x82A31B/0x82A377; one caller 0x82A002): the
// retry's first loaded chr was m175 (D 03:38, table 0x012FC598). Monster loads are logged with the FFX.exe return
// addresses of the stack, from the fresh battle to 40 s into the retry.
typedef void*(__cdecl* chrload_t)(const char* path, int a, int b, uint32_t flags);
static chrload_t g_chrload = nullptr;
static uint8_t* g_chrload_fn = nullptr;
static void* __cdecl h_chrload(const char* path, int a, int b, uint32_t flags) {
    if (g_diff && (g_diff_phase != 0 || g_slot_log_until > 0)) {
        __try {
            if (path && strstr(path, "mon")) {
                void* fr[16];
                USHORT nf = RtlCaptureStackBackTrace(0, 16, fr, nullptr);
                char chain[160]; int n = 0;
                uint8_t* eb = exe_base(); uint32_t es = exe_size();
                for (USHORT i = 0; i < nf && n < (int)sizeof(chain) - 12; i++)
                    if ((uint8_t*)fr[i] >= eb && (uint8_t*)fr[i] < eb + es)
                        n += _snprintf(chain + n, sizeof(chain) - n, " %08X", rt_to_gva_exe(fr[i]));
                chain[n] = 0;
                log_line("QOL-RETRY-DIFF: load chr %s (phase %d, a8e0 0x%02X) from%s", path, g_diff_phase, *g_a8e0, chain);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return g_chrload(path, a, b, flags);
}

static void diff_step_body() {
    uint8_t st = *g_a8e0;
    // lost attempt in battle (2), the retry's entry states (4), and 40 s of the retry (after the report)
    bool watch = (g_diff_phase == 2 && st == 1) || g_diff_phase == 4 || (g_diff_phase == 0 && g_slot_log_until > 0);
    if (g_diff_phase == 0 && g_slot_log_until > 0) --g_slot_log_until;
    if (watch) { __try { diff_slots(nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (g_diff_phase == 25) {                       // the prompt opened last invocation: end copy, apart from the prompt
        diff_copy(false);
        g_diff_phase = 3;
        log_line("QOL-RETRY-DIFF: end copy at the prompt");
    } else if (g_diff_phase == 1 && st == 1) {
        diff_copy(true);
        memcpy(g_prev, g_rgn[0].p, g_rgn[0].len);
        memset(g_tbits, 0, g_rgn[0].len / 32 + 4);
        g_watch = *(volatile uint64_t*)(exe_base() + (WATCH_GVA - 0x400000));
        g_tick = 0;
        g_diff_phase = 2;
        log_line("QOL-RETRY-DIFF: fresh copy at state 1");
        __try { diff_slots("fresh state 1"); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    } else if (g_diff_phase == 2) {
        diff_window();
    } else if (g_diff_phase == 4 && st == 1) {
        g_diff_phase = 0;
        __try { diff_slots("retry state 1"); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_slot_log_until = 2400;
        diff_report();
    }
}

// A fault here turns the diagnostic off, never the row (a row fault with the prompt up hung the battle twice).
static void diff_step() {                           // from pre(), each invocation
    if (!g_diff) return;
    __try { diff_step_body(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("QOL-RETRY-DIFF: fault 0x%08lX in phase %d - diagnostic off", GetExceptionCode(), g_diff_phase);
        g_diff = 0;
    }
}

static void __cdecl h_setup(uint32_t mode, int area, int group, int form) {
    if (g_state == ST_RELEASED) g_state = ST_IDLE;  // a new battle (or the retry): the prompt may arm again
    if (g_diff && mode == 0) {
        if (g_reissue && g_diff_phase == 3) g_diff_phase = 4;
        else if (!g_reissue && diff_alloc()) g_diff_phase = 1;
    }
    if (!g_disabled && mode == 0 && g_save_len) {
        __try {
            memcpy(g_snap_save, g_save, g_save_len);
            memcpy(g_snap_reward, g_reward, REWARD_LEN);
            for (int i = 0; i < 3; i++) g_snap_b48[i] = g_b48[k_b48_idx[i]];
            g_snap_a8e2 = *g_a8e2; g_snap_c256 = *g_c256; g_snap_c258 = *g_c258; g_snap_c259 = *g_c259;
            g_snap_a9d4 = *g_a9d4; g_snap_a9d5 = *g_a9d5;
            g_snap_ok = true;
            if (g_reissue) log_line("QOL-RETRY: retry battle starts (area %d group %d form %d)", area, group, form);
            else log_line("QOL-RETRY: snapshot at battle start (area %d group %d form %d, scripted %d, b48 0x%08X)",
                          area, group, form, g_snap_a9d4, g_snap_b48[0]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_snap_ok = false;
            log_line("QOL-RETRY: snapshot faulted - no retry for this battle");
        }
        g_reissue = 0;
    }
    g_setup(mode, area, group, form);
}

static void restore() {
    memcpy(g_save, g_snap_save, g_save_len);
    memcpy(g_reward, g_snap_reward, REWARD_LEN);
    for (int i = 0; i < 3; i++) g_b48[k_b48_idx[i]] = g_snap_b48[i];
}

static void rearm_request() {
    *g_c256 = g_snap_c256; *g_c258 = g_snap_c258; *g_c259 = g_snap_c259;
    *g_a9d4 = g_snap_a9d4; *g_a9d5 = g_snap_a9d5;
    *g_a8e2 = g_snap_a8e2 ? g_snap_a8e2 : 1;
}

static int __cdecl h_post(int scripted, uint32_t key, int stage, int result) {
    if (!g_disabled && g_retry && g_snap_ok && result == 1 && (g_quiet || !(*g_b48 & 0x200))) {
        g_retry = false;
        __try {
            restore();
            rearm_request();
            g_post_args[0] = scripted; g_post_args[1] = (int)key; g_post_args[2] = stage; g_post_args[3] = result;
            g_reissue = 1;
            g_retries++;
            log_line("QOL-RETRY: game over skipped, party state restored, battle re-requested (retry %u%s)", g_retries,
                     g_quiet ? ", no GAME OVER" : "");
            g_quiet = false;
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            log_line("QOL-RETRY: restore faulted - stock game over");
        }
    }
    if (g_quiet) { *g_b48 &= ~0x200u; g_quiet = false; }   // never let our loss-allowed bit reach the stock path
    g_retry = false;
    return g_post(scripted, key, stage, result);
}

// FUN_0079d050 (cdecl, no args, plain RET 0x79D082/0x79D087): "battle message window busy?"; when slot 7's text is
// done (FUN_008b7ca0(7) == 3, state 2) it closes slot 7 and returns 0. Game-over state 0x1E waits on it (and other
// battle code polls it), so it closed the prompt one frame after it was shown (D 16/10 11:32:05: window state 1 -> 4,
// choice [1] = 1, never answered). While the prompt is up it reports busy (-1) and leaves slot 7 alone.
typedef int(__cdecl* busy_t)();
static busy_t g_busy = nullptr;
static int __cdecl h_busy() {
    if (!g_disabled && g_state == ST_PROMPT) return -1;
    return g_busy();
}

// FUN_008af7d0 (cdecl(w), plain RET 0x8AF831): give window w the input focus (+6 = 1, cleared on the 7 others). Battle
// text code calls it for its own windows (5 call sites in 0x0089xxxx); while the prompt is up the focus stays on it.
typedef void(__cdecl* focus_t)(int);
static focus_t g_focus_tramp = nullptr;
static void __cdecl h_focus(int w) {
    if (!g_disabled && g_state == ST_PROMPT && w != WIN) w = WIN;
    g_focus_tramp(w);
}

static bool window_free() { int s = g_w_state(WIN); return s == 0 || s == 4; }

static void open_prompt() {
    g_w_reset(WIN);
    g_w_color(WIN, 0x80, 0x80, 0x80, 0x80);
    // Field dialogue style as the stock raw-text opener FUN_00860e40 (style (0,0), FUN_008afb90(w, w)): it lays out the
    // option lines and takes cursor input. The battle caption style (1, 0x2402; FUN_0089b1d0) drew the two options on
    // one line ("Retry: Title") and took no input (U 16/10/2026).
    if (g_style == 1) g_w_style(WIN, 1, 0x2402);
    else g_w_style(WIN, 0, 0);
    build_text();
    g_w_text(WIN, (int)(uintptr_t)g_text, (int)(uintptr_t)g_text);
    g_w_anchor(WIN, 4, 0x100, 0xE0);
    g_w_anchor2(WIN, g_style == 1 ? 4 : WIN);
    g_w_focus(WIN);
    g_w_choice(WIN, 0, 1, 1);                        // cursor on Retry, cancel -> Give up, one pick
    g_w_open(WIN);
}

static void pre() {
    if (!g_patched) {
        g_patched = true;
        if (!hook_entry("QOL-RETRY setup", g_setup_fn, (void*)&h_setup, (void**)&g_setup) ||
            !hook_entry("QOL-RETRY post", g_post_fn, (void*)&h_post, (void**)&g_post) ||
            !hook_entry("QOL-RETRY window busy", g_busy_fn, (void*)&h_busy, (void**)&g_busy) ||
            !hook_entry("QOL-RETRY window focus", (void*)g_w_focus, (void*)&h_focus, (void**)&g_focus_tramp)) {
            log_line("QOL-RETRY: hook failed - row off");
            row_QOLRETRY.on = 0;
            return;
        }
        log_line("QOL-RETRY: battle setup and post-battle hooked on the game thread");
        if (g_diff && g_chrload_fn &&
            !hook_entry("QOL-RETRY-DIFF load chr", g_chrload_fn, (void*)&h_chrload, (void**)&g_chrload))
            log_line("QOL-RETRY-DIFF: load chr hook failed - not logged");
    }
    if (g_disabled) return;
    __try {
        diff_step();
        if (g_reissue) {                             // retry armed: the field branch should start the battle
            if (*g_a8e2 == 0 && *g_a8e0 == 0) rearm_request();
            if (++g_reissue > REISSUE_WAIT) {
                g_reissue = 0;
                log_line("QOL-RETRY: retry battle did not start - stock game over");
                g_post(g_post_args[0], (uint32_t)g_post_args[1], g_post_args[2], g_post_args[3]);
            }
        }
        // SR-008 (soft-reset chord, its pre runs before ours) clears bit 0x200 every tick until the battle is down;
        // a quiet retry needs it set when case 0x18 runs.
        if (g_quiet && g_retry) *g_b48 |= 0x200;
        uint8_t st = *g_a8e0;
        switch (g_state) {
        case ST_IDLE: {
            // Case 0x17 falls through into 0x18 in the same call (FUN_007917d0), so 0x18 is never seen between
            // invocations: arm early when the party check FUN_007928d0 (read-only; returns c9f1 when set, e.g. by
            // the SR-008 chord) says 1 = wiped. The late window 0x1E..0x20 (GAME OVER already shown) stays a fallback.
            // Case 2 also falls into 0x17 in one call (it sets 0x17 and drops through), so the first stop between
            // invocations after a wipe is state 2 (case 1 sets it and returns). Not when a captured party member
            // is about to be released (case 2: c9e8 != 0 sends the battle back to 1; c9e8 = c9ec - 4, SR-007.1 #5).
            bool early = (st == 2 || st == 0x17) && g_c9ec[-4] == 0 && g_party() == 1;
            bool late = st >= 0x1E && st <= 0x20 && *g_a8e3 == 1;
            bool ok = g_snap_ok && (early || late) && !(*g_b48 & 0x200) && (g_chord || !sr008_battle_reset_pending());
            if (ok && early && !window_free()) {        // a battle message still in slot 7: hold, let it close
                *g_c9ec = HOLD;
                g_busy();
                break;
            }
            if (ok && window_free()) {
                g_early = early;
                *g_c9ec = HOLD;
                open_prompt();
                g_prompt_inv = 0;
                g_prompt_seen = false;
                g_state = ST_PROMPT;
                log_line("QOL-RETRY: game over (state 0x%02X): Retry / Give up prompt open", st);
                if (g_diff && g_diff_phase == 2) g_diff_phase = 25;   // copied by diff_step next invocation
            }
            break;
        }
        case ST_PROMPT: {
            *g_c9ec = HOLD;
            uint8_t* ch = g_choice_tbl[WIN];
            int ws = g_w_state(WIN);
            ++g_prompt_inv;
            // FUN_008af7d0(w) gives w the input focus (+6 = 1) and clears it on the other 7 windows. Battle code takes it
            // for its own window after the prompt opens (D 16/10 11:41:33: +6 1 -> 0, pad presses latched but ignored),
            // so the prompt takes it back while it is up.
            uint8_t* wo = g_win_tbl[WIN];
            if (wo && *(int16_t*)(wo + 6) != 1 && (ws == 1 || ws == 2)) {
                g_w_focus(WIN);
                if (++g_refocus == 1) log_line("QOL-RETRY: prompt lost the input focus - taken back (logged once)");
            }
            if (g_prompt_inv % 30 == 1 && g_prompt_inv < 1200) {
                // diagnostic only (fixed Ghidra VAs, re/ledger/RETRY-UI.md): window input path
                uint8_t* eb = exe_base();
                auto at = [eb](uint32_t va) { return eb + (va - 0x400000); };
                uint8_t* win = *(uint8_t**)at(0x01865B3C + WIN * 4);
                uint8_t* txt = *(uint8_t**)at(0x018676F0 + WIN * 4);
                log_line("QOL-RETRY: prompt inv %d: window state %d, choice [1]=%d [2]=%d [4]=%d [5]=%d [7]=%d; win +6=%d; "
                         "text +0x2c=%d; latch c0=%d c1=%d d2=0x%04X d4=0x%04X d6=0x%04X; a8e0 0x%02X kind %d",
                         g_prompt_inv, ws, ch ? ch[1] : -1, ch ? (int8_t)ch[2] : -1, ch ? (int8_t)ch[4] : -1,
                         ch ? (int8_t)ch[5] : -1, ch ? (int8_t)ch[7] : -1, win ? *(int16_t*)(win + 6) : -1,
                         txt ? *(int16_t*)(txt + 0x2C) : -1, *at(0x025D09C0), *at(0x025D09C1),
                         *(uint16_t*)at(0x025D09D2), *(uint16_t*)at(0x025D09D4), *(uint16_t*)at(0x025D09D6), st, pacing_kind());
            }
            if (ws != 0 && ws != 4) g_prompt_seen = true;
            else if (g_prompt_seen && !(ch && ch[1] == 2)) { // closed by someone else, no answer: stock game over
                log_line("QOL-RETRY: window closed without an answer (state %d) - prompt dropped, stock game over", ws);
                g_retry = false;
                g_state = ST_RELEASED;
                break;
            }
            if (!g_prompt_seen && g_prompt_inv > 180) {      // ~3 s and the window never came up: stock game over
                log_line("QOL-RETRY: window never opened (state %d) - prompt dropped, stock game over", ws);
                g_retry = false;
                g_state = ST_RELEASED;
                break;
            }
            if (ch && ch[1] == 2 && (int8_t)ch[7] > 0) {
                g_retry = ch[0x18] == 0;
                // Retry from the early prompt: case 0x18 then takes the loss-allowed branch (no GAME OVER effect or
                // music; FUN_0086c290 = bit 0x200 of b48); the detour restores the snapshot's b48 at teardown.
                if (g_retry && g_early) { *g_b48 |= 0x200; g_quiet = true; }
                g_w_close(WIN);
                g_state = ST_CLOSING;
                log_line("QOL-RETRY: player chose %s", g_retry ? "Retry" : "Give up");
            }
            break;
        }
        case ST_CLOSING:
            if (window_free()) g_state = ST_RELEASED;   // stop holding: the stock chain goes on
            else *g_c9ec = HOLD;
            break;
        case ST_RELEASED:
            // Stays released until the next battle starts (h_setup). The early prompt answers at state 2, which the
            // held state machine has not left yet, so a state test here re-armed it at once (Give up looped, U
            // 16/10/2026, fps60.log 12:27:47..52).
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("QOL-RETRY: fault 0x%08lX in pre - row off", GetExceptionCode());
        row_QOLRETRY.on = 0;
        // release the battle: the busy and focus detours stay installed and hold slot 7 while g_state == ST_PROMPT
        if (g_state == ST_PROMPT) { __try { g_w_close(WIN); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        g_state = ST_IDLE;
        g_retry = false;
    }
}

static void stop() {                                 // dev hot reload: close our window, let the battle go on
    __try { if (g_state == ST_PROMPT) g_w_close(WIN); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_state = ST_IDLE;
    g_retry = false;
}

#define NEED(x) do { if (!(x)) { log_line("  QOL-RETRY: " #x " not resolved - refused"); return false; } } while (0)

static bool install() {
    ffx_text_init();
    g_chord = config_row("QOL-RETRY-CHORD", 1) != 0;
    g_style = config_row("QOL-RETRY-STYLE", 0);
    g_diff = config_row("QOL-RETRY-DIFF", 0);
    if (g_diff)                                      // one match in fps60_image.bin
        g_chrload_fn = sig_find_unique("QOL-RETRY-DIFF load chr FUN_0082a1c0",
            "55 8B EC 81 EC 84 02 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 53 8B 5D 14 56 8B 75 08 57 68 80 00 00 00", 0x82A1C0);
    g_setup_fn = sig_find_unique("QOL-RETRY MsBattleSetupEncounter",
        "55 8B EC 53 56 57 E8 ?? ?? ?? ?? 8B 5D 08 85 C0 74 28 85 DB 75 37 68", 0x781020);
    g_post_fn = sig_find_unique("QOL-RETRY post-battle FUN_00861160",
        "55 8B EC 56 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B F0 C7 05 ?? ?? ?? ?? 00 00 00 00 E8 ?? ?? ?? ?? 6A 00 E8", 0x861160);
    // FUN_00781c90 (script battle request): c256 +0x39, c258 +0x42, c259 +0x4B, a9d4 +0x53, a8e2 +0x5C, a9d5 +0x62
    uint8_t* sb = sig_find_unique("QOL-RETRY script battle request FUN_00781c90",
        "55 8B EC 83 EC 0C 80 3D ?? ?? ?? ?? 01 74 57 E8 ?? ?? ?? ?? 85 C0 75 4E 8D 45 F4 50 8D 45 F8 50 8D 45 FC 50 "
        "FF 75 08 E8 ?? ?? ?? ?? 83 C4 10 85 C0 74 33 66 8B 45 FC 66 A3 ?? ?? ?? ?? 0F B6 45 F8 A2 ?? ?? ?? ?? 0F B6 45 "
        "F4 A2 ?? ?? ?? ?? 8A 45 0C A2 ?? ?? ?? ?? 8A 45 10 C6 05 ?? ?? ?? ?? 02 A2", 0x781C90);
    // MsBattleProcess teardown test 0x790E6C: MOV AL,[a8e0]; CMP AL,0x21; JZ; MOV AL,[a8e3]; CMP AL,5; JZ; CMP AL,6; JNZ
    uint8_t* td = sig_find_unique("QOL-RETRY battle state test",
        "A0 ?? ?? ?? ?? 3C 21 74 0D A0 ?? ?? ?? ?? 3C 05 74 04 3C 06 75", 0x790E6C);
    // FUN_007a5330 (script "end battle with result"): CMP byte [a8e0],1 at +0x11; MOV [c9f1],AL at +0x18
    uint8_t* eb = sig_find_unique("QOL-RETRY end-battle opcode FUN_007a5330",
        "55 8B EC FF 75 10 FF 75 08 E8 ?? ?? ?? ?? 83 C4 08 80 3D ?? ?? ?? ?? 01 A2 ?? ?? ?? ?? 75 07 C6 05 ?? ?? ?? ?? 02 5D C3",
        0x7A5330);
    // FUN_007928d0 (party check, cdecl, no args, plain RET 0x7929AF): PUSH EBX/ESI/EDI; XOR x3; LEA ESP,[ESP];
    // MOVZX EAX,byte [ESI+party]; CMP EAX,0xFF; JZ; PUSH EAX; CALL chr
    g_party = (party_t)sig_find_unique("QOL-RETRY party check FUN_007928d0",
        "53 56 57 33 FF 33 DB 33 F6 8D A4 24 00 00 00 00 0F B6 86 ?? ?? ?? ?? 3D FF 00 00 00 74 41 50 E8", 0x7928D0);
    uint8_t* hold = sig_find_unique("QOL-RETRY end-of-battle timer gate",
        "8A 0D ?? ?? ?? ?? A2 ?? ?? ?? ?? 84 C9 74 08 FE C9 88 0D", 0x790D2D);
    uint8_t* b48 = sig_find_unique("QOL-RETRY battle flags site",
        "8B 0D ?? ?? ?? ?? C7 05 ?? ?? ?? ?? ?? ?? ?? ?? 89 0D ?? ?? ?? ?? 83 F8 01 75 12 F7 C1 00 02 00 00 75 0A E8", 0x8611FF);
    uint8_t* sc = sig_find_unique("QOL-RETRY save clear (save block)",
        "68 C0 68 00 00 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 40 10 6A 0D FF D0", 0x786B13);
    uint8_t* ap = sig_find_unique("QOL-RETRY battle-end AP gate (reward block)",
        "80 BE ?? ?? ?? ?? 00 74 12 80 B8 CC 0D 00 00 00 75 09 80 B8 CE 0D 00 00 00 74 0E 8B 04 B5 ?? ?? ?? ?? 89 04 B5", 0x798B7C);
    // FUN_0089b1d0 (battle text window): E8 at +0x08 ada30, +0x15 8b0000, +0x20 8b89d0, +0x32 8af850, +0x3A 8afb90,
    // +0x40 8af7d0, +0x46 8ade10
    uint8_t* bt = sig_find_unique("QOL-RETRY battle text window FUN_0089b1d0",
        "55 8B EC 56 8B 75 08 56 E8 ?? ?? ?? ?? 68 02 24 00 00 6A 01 56 E8 ?? ?? ?? ?? 8B 45 0C 50 50 56 E8 ?? ?? ?? ?? "
        "68 E0 00 00 00 68 00 01 00 00 6A 04 56 E8", 0x89B1D0);
    uint8_t* chs = sig_find_unique("QOL-RETRY set choices FUN_008b0020",
        "55 8B EC 8B 55 08 B9 01 00 00 00 8B 04 95 ?? ?? ?? ?? 8B 40 14 66 89 48 1C", 0x8B0020);
    uint8_t* cq = sig_find_unique("QOL-RETRY choice query FUN_008b7ff0",
        "55 8B EC 8B 45 08 8B 04 85 ?? ?? ?? ?? 80 78 01 02 74 05 83 C8 FF 5D C3 0F BE 40 07 5D C3", 0x8B7FF0);
    // FUN_0079d050 (battle closes slot 7): E8 at +0x13 window state FUN_00906c20, +0x27 close FUN_008ab8f0
    uint8_t* cl = sig_find_unique("QOL-RETRY battle window close FUN_0079d050",
        "56 6A 07 E8 ?? ?? ?? ?? 8B F0 83 C4 04 85 F6 74 1E 6A 07 E8 ?? ?? ?? ?? 83 C4 04 83 FE 03 75 13 83 F8 02 75 0E 6A 07 E8",
        0x79D050);
    // FUN_00860e40 (field raw-text window): E8 at +0x56 frame colours FUN_00906d90
    uint8_t* fw = sig_find_unique("QOL-RETRY field text window FUN_00860e40",
        "55 8B EC 56 57 8B 7D 08 57 E8 ?? ?? ?? ?? 83 C4 04 8B F0 57 85 F6 0F 85", 0x860E40);
    NEED(g_setup_fn); NEED(g_post_fn); NEED(sb); NEED(td); NEED(hold); NEED(b48); NEED(sc); NEED(ap);
    NEED(bt); NEED(chs); NEED(cq); NEED(cl); NEED(fw); NEED(eb); NEED(g_party);
    g_busy_fn = cl;

    g_c256 = (volatile uint16_t*)read_abs(sb + 0x39);
    g_c258 = read_abs(sb + 0x42); g_c259 = read_abs(sb + 0x4B);
    g_a9d4 = read_abs(sb + 0x53); g_a8e2 = read_abs(sb + 0x5C); g_a9d5 = read_abs(sb + 0x62);
    g_a8e0 = read_abs(td + 1); g_a8e3 = read_abs(td + 0xA);
    g_c9ec = read_abs(hold + 2);
    g_c9f1 = read_abs(eb + 0x19);
    NEED(g_c9f1 && read_abs(eb + 0x13) == (uint8_t*)g_a8e0);
    g_b48 = (volatile uint32_t*)read_abs(b48 + 2);
    g_save_len = *(uint32_t*)(sc + 1); g_save = read_abs(sc + 6);
    uint8_t* acted = read_abs(ap + 2);
    g_reward = acted ? acted - 0x24 : nullptr;
    g_choice_tbl = (uint8_t**)read_abs(cq + 9);
    g_win_tbl = (uint8_t**)read_abs(chs + 0x0E);
    NEED(g_c256 && g_c258 && g_c259 && g_a9d4 && g_a8e2 && g_a9d5 && g_a8e0 && g_a8e3 && g_c9ec && g_b48);
    NEED(g_save && g_reward && g_choice_tbl && g_win_tbl);
    NEED(g_save_len > 0 && g_save_len <= SAVE_MAX);
    NEED(g_a8e2 == g_a8e0 + 2 && g_a8e3 == g_a8e0 + 3);   // one battle-state struct (0x0112A8D0 +0x10..)

    g_w_reset   = (w1_t)call_target(bt + 0x08);
    g_w_style   = (w3_t)call_target(bt + 0x15);
    g_w_text    = (w3_t)call_target(bt + 0x20);
    g_w_anchor  = (w4_t)call_target(bt + 0x32);
    g_w_anchor2 = (w2_t)call_target(bt + 0x3A);
    g_w_focus   = (w1_t)call_target(bt + 0x40);
    g_w_open    = (w1_t)call_target(bt + 0x46);
    g_w_choice  = (w4_t)chs;
    g_w_state   = (w1_t)call_target(cl + 0x13);
    g_w_close   = (w1_t)call_target(cl + 0x27);
    g_w_color   = (w5_t)call_target(fw + 0x56);
    NEED(g_w_reset && g_w_style && g_w_text && g_w_anchor && g_w_anchor2 && g_w_focus && g_w_open);
    NEED(g_w_state && g_w_close && g_w_color);

    log_line("  QOL-RETRY: setup gva=0x%08X post gva=0x%08X; save gva=0x%08X len 0x%X, reward gva=0x%08X, b48 gva=0x%08X, "
             "a8e0 gva=0x%08X, c9ec gva=0x%08X, choices gva=0x%08X", rt_to_gva_exe(g_setup_fn), rt_to_gva_exe(g_post_fn),
             rt_to_gva_exe(g_save), g_save_len, rt_to_gva_exe(g_reward), rt_to_gva_exe((void*)g_b48),
             rt_to_gva_exe((void*)g_a8e0), rt_to_gva_exe((void*)g_c9ec), rt_to_gva_exe(g_choice_tbl));
    log_line("  QOL-RETRY: window fns reset 0x%08X style 0x%08X text 0x%08X anchor 0x%08X/0x%08X focus 0x%08X open 0x%08X "
             "choice 0x%08X state 0x%08X close 0x%08X colour 0x%08X",
             rt_to_gva_exe((void*)g_w_reset), rt_to_gva_exe((void*)g_w_style), rt_to_gva_exe((void*)g_w_text),
             rt_to_gva_exe((void*)g_w_anchor), rt_to_gva_exe((void*)g_w_anchor2), rt_to_gva_exe((void*)g_w_focus),
             rt_to_gva_exe((void*)g_w_open), rt_to_gva_exe((void*)g_w_choice), rt_to_gva_exe((void*)g_w_state),
             rt_to_gva_exe((void*)g_w_close), rt_to_gva_exe((void*)g_w_color));
    return true;
}

Row row_QOLRETRY = {
    "QOL-RETRY",
    "gameplay option: Retry / Give up prompt after a party wipe; Retry restarts the battle as it started",
    0, install, pre, nullptr, 0, stop
};
