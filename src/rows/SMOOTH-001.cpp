// SPDX-License-Identifier: Apache-2.0
// SMOOTH-001: at 60 Hz the camera is still built once per logic tick, so it moves at 30 Hz. This row
// draws logic frames with the view half-way between the last two ticks (needs master SMOOTH=1).
// Details: docs/fps60/rows/SMOOTH-001.md
#include "smooth.h"
#include <string.h>

typedef int (__cdecl* finalize_t)(int);
typedef void(__cdecl* commit_t)(float*);

static finalize_t g_finalize = nullptr;
static commit_t   g_commit = nullptr;
static float*     g_view = nullptr;           // the view matrix the draw reads (16 floats)
static volatile uint32_t* g_src2 = nullptr;   // != 0: camera source s2 (look-at from a fixed camera block)
static volatile uint32_t* g_src3 = nullptr;   // != 0: camera source s3 (field map camera)
static int        g_diag = 0;

struct CamHist {
    float prev[16];      // canonical matrix of the previous logic tick
    bool  valid;
    int   tag;           // source of prev (1..3)
    float pend[16];      // sample of the current logic invocation
    int   pend_tag;
    bool  has_pend;
};
static CamHist g_h[3];
static int   g_kind = INV_STOCK;
// Deferred restore state (one view matrix; the last write wins).
static bool  g_written = false;
static bool  g_committed = false;
static float g_orig[16], g_out[16];
static SmoothStats g_st;
static LONG  g_logic_calls = 0, g_skip_hist = 0;

static int src_tag() { return *g_src3 ? 3 : (*g_src2 ? 2 : 1); }

static void write_view(const float* out, int tag) {
    memcpy(g_orig, g_view, 64);
    memcpy(g_out, out, 64);
    memcpy(g_view, out, 64);
    g_written = true;
    if (tag == 3 || tag == 1) { g_commit(g_view); g_committed = true; }
}

static void on_call(int vp) {
    if (g_kind != INV_LOGIC && g_kind != INV_BACKLOG) return;    // render-only frames draw tick k
    CamHist& h = g_h[vp];
    int tag = src_tag();
    bool ours = g_written && memcmp(g_view, g_out, 64) == 0;  // already ours (s3: nothing rebuilt it)
    // History always holds the canonical (game) matrix, never our interpolated one.
    memcpy(h.pend, ours ? g_orig : g_view, 64);
    h.pend_tag = tag;
    h.has_pend = true;
    if (!smooth_live() || ours) return;
    g_logic_calls++;
    if (tag == 1 && vp == 1) return;                          // special commit paths (MsCommitCameraFovAspect)
    if (!h.valid || h.tag != tag) { g_skip_hist++; return; }
    float out[16];
    if (!smooth_interp_view(h.prev, g_view, 0.5f, SMOOTH_CUT_DIST, out)) { InterlockedIncrement(&g_st.interp_cut); return; }
    if (memcmp(out, g_view, 64) == 0) return;
    write_view(out, tag);
    InterlockedIncrement(&g_st.interp);
}

static int __cdecl h_finalize(int vp) {
    int r = g_finalize(vp);
    if (r != 0 || vp < 0 || vp > 2 || g_disabled || !row_SMOOTH001.on) return r;
    __try { on_call(vp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-001 finalize stub", GetExceptionCode()); }
    return r;
}

static void restore() {
    if (!g_written) return;
    if (memcmp(g_view, g_out, 64) == 0) {
        memcpy(g_view, g_orig, 64);
        if (g_committed) g_commit(g_view);
    }
    g_written = false;
    g_committed = false;
}

static void pre() {
    smooth_poll_hotkey();
    __try { restore(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-001 restore", GetExceptionCode()); }
    g_kind = g_disabled ? INV_STOCK : pacing_kind();
    for (int i = 0; i < 3; i++) {
        g_h[i].has_pend = false;
        if (g_kind == INV_STOCK) g_h[i].valid = false;
    }
}

static void post() {
    if (g_kind == INV_LOGIC || g_kind == INV_BACKLOG) {
        for (int i = 0; i < 3; i++) {
            CamHist& h = g_h[i];
            if (!h.has_pend) { h.valid = false; continue; }   // viewport not drawn this tick: history broken
            memcpy(h.prev, h.pend, 64);
            h.tag = h.pend_tag;
            h.valid = true;
        }
    }
    static LONG last = 0;
    if (g_diag && g_logic_calls - last >= 600) {
        last = g_logic_calls;
        log_line("SMOOTH-001: logic calls=%ld interp=%ld interp_cut=%ld skip_hist=%ld | src=%d live=%d",
                 g_logic_calls, g_st.interp, g_st.interp_cut, g_skip_hist, src_tag(), smooth_live());
    }
    static LONG reported = 0;
    if (!reported && g_st.interp > 0) {
        reported = 1;
        log_line("SMOOTH-001: first interpolated camera on a logic invocation (source %d)", src_tag());
    }
}

static bool install() {
    if (!smooth_master()) { log_line("  SMOOTH-001: master switch SMOOTH=0 - off, nothing scanned or patched"); return false; }
    // Sg_MainLoop draw: first per-viewport CALL to the viewport finalize (FUN_007bc110)
    uint8_t* a = sig_find_unique("SMOOTH-001 viewport loop (Sg_MainLoop draw)",
        "33 FF 33 DB 53 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 8F ?? ?? ?? ?? 0F 85", 0x821184);
    // Sg_MainLoop draw: the second-pass and main-viewport finalize CALLs
    uint8_t* b = sig_find_unique("SMOOTH-001 viewport loop tail (Sg_MainLoop draw)",
        "43 53 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 8E ?? ?? ?? ?? 47 83 FF 02 0F 8C ?? ?? ?? ?? 0F BE 05 ?? ?? ?? ?? 50 E8", 0x82135C);
    // Field map camera: commits its view matrix (gives the view address and the commit function)
    uint8_t* c = sig_find_unique("SMOOTH-001 map camera commit (FUN_00920c00)",
        "68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 0C 5F 5E 8B 4D FC", 0x920E1A);
    if (!a || !b || !c) return false;
    uint8_t* sites[3] = { a + 5, b + 2, b + 0x24 };
    uint8_t* fin = call_target(sites[0]);
    if (!fin || call_target(sites[1]) != fin || call_target(sites[2]) != fin) {
        log_line("  SMOOTH-001: the three viewport CALLs do not share one target - refused");
        return false;
    }
    // Viewport finalize: picks the camera source (src2 look-at, else src3 = map camera)
    uint8_t* d = sig_find_unique("SMOOTH-001 camera sources (FUN_007bc110)",
        "39 1D ?? ?? ?? ?? 74 3A FF 75 FC 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? D9 05 ?? ?? ?? ?? 83 C4 04 D9 1C 24 "
        "68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 14 E9 ?? ?? ?? ?? "
        "83 3D ?? ?? ?? ?? 00 0F 85", 0x7BC3D1);
    if (!d) return false;
    if (d < fin || d > fin + 0x600) { log_line("  SMOOTH-001: source block not inside the finalize function - refused"); return false; }
    uint32_t src2 = *(uint32_t*)(d + 2), view = *(uint32_t*)(d + 0x31), src3 = *(uint32_t*)(d + 0x44);
    uint32_t view2 = *(uint32_t*)(c + 1);
    uint8_t* commit = call_target(c + 5);
    // Commit function: prologue, then the skip-render check
    uint8_t* skip = sym_rt("skip_render");
    if (view != view2 || !sig_match(commit, "55 8B EC 81 EC C0 00 00 00 83 3D") ||
        (skip && read_abs(commit + 0xB) != skip)) {
        log_line("  SMOOTH-001: view/commit cross-check failed (view 0x%08X vs 0x%08X, commit %p) - refused", view, view2, commit);
        return false;
    }
    g_finalize = (finalize_t)fin;
    g_commit = (commit_t)commit;
    g_view = (float*)(uintptr_t)view;
    g_src2 = (volatile uint32_t*)(uintptr_t)src2;
    g_src3 = (volatile uint32_t*)(uintptr_t)src3;
    g_diag = config_row("SMOOTH-001-DIAG", 0);
    for (int i = 0; i < 3; i++) {
        if (!patch_call_rel32(sites[i], fin, (void*)&h_finalize)) {
            for (int j = 0; j < i; j++) patch_call_rel32(sites[j], (void*)&h_finalize, fin);
            log_line("  SMOOTH-001: patch %d failed - refused", i);
            return false;
        }
    }
    if (!row_on("LOOP-005") || !row_on("LOOP-004"))
        log_line("  SMOOTH-001: note: LOOP-005/LOOP-004 off; frames where the game moves the camera itself are left alone");
    log_line("  SMOOTH-001: finalize gva=0x%08X (expected 0x007BC110), CALLs gva=0x%08X/0x%08X/0x%08X -> stub; view gva=0x%08X "
             "(0x02311440), src2 0x%08X (0x0231142C), src3 0x%08X (0x023114C4), commit gva=0x%08X (0x00639860); interpolation; diag=%d; F7 toggles",
             rt_to_gva_exe(fin), rt_to_gva_exe(sites[0]), rt_to_gva_exe(sites[1]), rt_to_gva_exe(sites[2]),
             rt_to_gva_exe(g_view), rt_to_gva_exe((const void*)g_src2), rt_to_gva_exe((const void*)g_src3),
             rt_to_gva_exe(commit), g_diag);
    return true;
}

Row row_SMOOTH001 = {
    "SMOOTH-001",
    "60 Hz camera: logic frames draw the view half-way between the last two ticks (interpolation; needs master SMOOTH=1)",
    1, install, pre, post, 0
};
