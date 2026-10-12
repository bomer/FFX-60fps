// SPDX-License-Identifier: Apache-2.0
// X2-SMOOTH-001 (port of FFX SMOOTH-001): at 60 Hz the camera is still built once per logic tick, so it moves at
// 30 Hz. Logic invocations draw the view half-way between the last two ticks (interpolation; render-only invocations
// draw tick k). The game view is put back at the next invocation. Needs master X2-SMOOTH=1; F7 toggles.
// FFX-2 changes (PORT-SMOOTH.md): finalize Cam_ViewportFinalize(vp, pass) and commit Cam_CommitView(view, slot) take
// 2 args (re-commit uses slot 0, as every tag-1 commit for vp 0/2 and the map camera do); no interpolation while the
// debug camera flag [0x01ACCE10] is set; FOV (X2-SMOOTH-001-FOV): FFX-2 has no FOV commit helper, so the two inline
// setter CALLs (Gfx_SetCameraFov / Gfx_SetCameraAspect) record the game's values, and an interpolated frame calls the
// setters again with the midpoints (the cut test stays on camera block +0x10/+0x14/+0x1C).
// Evidence: outer repo re/ledger/PORT-SMOOTH.md §1-§3 (S).
#include "x2_smooth.h"
#include <string.h>
#include <math.h>

namespace x2 {

typedef int (__cdecl* finalize_t)(int vp, int pass);
typedef void(__cdecl* commit_t)(float* view, int slot);
typedef void(__cdecl* fovset_t)(float* v);

static finalize_t g_finalize = nullptr;
static commit_t   g_commit = nullptr;
static float*     g_view = nullptr;           // 0x01ACCD80
static volatile uint32_t* g_src2 = nullptr;   // 0x01ACCD74 != 0: fixed look-at
static volatile uint32_t* g_src3 = nullptr;   // 0x01ACCE04 != 0: map camera
static volatile uint32_t* g_dbg = nullptr;    // 0x01ACCE10 != 0: debug camera (H: 0 in retail)
static int        g_fov = 1;
static fovset_t   g_setfov = nullptr, g_setasp = nullptr;
static float*     g_blk = nullptr;            // viewport camera blocks 0xE12A40, stride 0x1C0
static const int  k_fovoff[3] = { 0x10, 0x14, 0x1C };
static int        g_cur_vp = -1;              // viewport inside the finalize (setter stubs record per vp)
static float      g_rec_fov[3], g_rec_asp[3];
static bool       g_rec_ok[3];
static LONG       g_fov_interp = 0, g_fov_cut = 0;
extern Row row_SMOOTH001;

static void __cdecl h_setfov(float* v) { if (g_cur_vp >= 0 && v) g_rec_fov[g_cur_vp] = *v; g_setfov(v); }
static void __cdecl h_setasp(float* v) {
    if (g_cur_vp >= 0 && v) { g_rec_asp[g_cur_vp] = *v; g_rec_ok[g_cur_vp] = true; }
    g_setasp(v);
}

struct CamHist {
    float prevfov[3], pendfov[3];        // camera block fields (cut test)
    float prevset[2], pendset[2];        // game FOV / aspect values passed to the setters
    bool  prevset_ok, pendset_ok;
    float prev[16];
    bool  valid;
    int   tag;
    float pend[16];
    int   pend_tag;
    bool  has_pend;
};
static CamHist g_h[3];
static int   g_kind = INV_STOCK;
static bool  g_written = false, g_committed = false;
static float g_orig[16], g_out[16];
static SmoothStats g_st;
static LONG  g_logic_calls = 0, g_skip_hist = 0;

static int src_tag() { return *g_src3 ? 3 : (*g_src2 ? 2 : 1); }

static void write_view(const float* out, int tag) {
    memcpy(g_orig, g_view, 64);
    memcpy(g_out, out, 64);
    memcpy(g_view, out, 64);
    g_written = true;
    if (tag == 3 || tag == 1) { g_commit(g_view, 0); g_committed = true; }
}

static void on_call(int vp) {
    if (g_kind != INV_LOGIC && g_kind != INV_BACKLOG) return;    // render-only frames draw tick k
    CamHist& h = g_h[vp];
    int tag = src_tag();
    bool ours = g_written && memcmp(g_view, g_out, 64) == 0;
    memcpy(h.pend, ours ? g_orig : g_view, 64);                // history holds the game matrix
    h.pend_tag = tag;
    h.has_pend = true;
    float* blk = (float*)((uint8_t*)g_blk + vp * 0x1C0);
    for (int i = 0; i < 3; i++) h.pendfov[i] = *(float*)((uint8_t*)blk + k_fovoff[i]);
    h.pendset_ok = g_rec_ok[vp];
    h.pendset[0] = g_rec_fov[vp];
    h.pendset[1] = g_rec_asp[vp];
    if (!smooth_live(1) || ours || *g_dbg) return;
    g_logic_calls++;
    if (tag == 1 && vp == 1) return;                          // special commit slots on viewport 1
    if (!h.valid || h.tag != tag) { g_skip_hist++; return; }
    bool fov_mid = false;
    if (g_fov && tag == 1 && h.pendset_ok && h.prevset_ok) {
        for (int i = 0; i < 3; i++) {
            float a = h.prevfov[i], b = h.pendfov[i], m = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
            if (m > 0 && fabsf(a - b) > 0.25f * m) { InterlockedIncrement(&g_fov_cut); return; }   // zoom cut
            if (a != b) fov_mid = true;
        }
        if (h.prevset[0] != h.pendset[0] || h.prevset[1] != h.pendset[1]) fov_mid = true;
    }
    float out[16];
    if (!smooth_interp_view(h.prev, g_view, 0.5f, SMOOTH_CUT_DIST, out)) { InterlockedIncrement(&g_st.interp_cut); return; }
    if (fov_mid) {
        float f = 0.5f * (h.prevset[0] + h.pendset[0]), a = 0.5f * (h.prevset[1] + h.pendset[1]);
        g_setfov(&f);                                         // the next finalize sets the game values again
        g_setasp(&a);
        InterlockedIncrement(&g_fov_interp);
    }
    if (memcmp(out, g_view, 64) == 0) return;
    write_view(out, tag);
    InterlockedIncrement(&g_st.interp);
}

static int __cdecl h_finalize(int vp, int pass) {
    bool rec = vp >= 0 && vp <= 2;
    if (rec) { g_cur_vp = vp; g_rec_ok[vp] = false; }
    int r = g_finalize(vp, pass);
    g_cur_vp = -1;
    if (r != 0 || !rec || g_disabled || !row_SMOOTH001.on) return r;
    __try { on_call(vp); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("X2-SMOOTH-001: fault 0x%08lX in the finalize stub - row off", GetExceptionCode());
        row_SMOOTH001.on = 0;
    }
    return r;
}

static void restore() {
    if (!g_written) return;
    if (memcmp(g_view, g_out, 64) == 0) {
        memcpy(g_view, g_orig, 64);
        if (g_committed) g_commit(g_view, 0);
    }
    g_written = false;
    g_committed = false;
}

static void pre() {
    smooth_poll_hotkey();
    __try { restore(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-SMOOTH-001: fault in restore"); }
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
            memcpy(h.prevfov, h.pendfov, sizeof(h.prevfov));
            memcpy(h.prevset, h.pendset, sizeof(h.prevset));
            h.prevset_ok = h.pendset_ok;
            h.tag = h.pend_tag;
            h.valid = true;
        }
    }
    static LONG last = 0;
    if (g_logic_calls - last >= 1800) {
        last = g_logic_calls;
        log_line("X2-SMOOTH-001: logic calls=%ld interp=%ld cut=%ld skip_hist=%ld fov_interp=%ld fov_cut=%ld | src=%d live=%d",
                 g_logic_calls, g_st.interp, g_st.interp_cut, g_skip_hist, g_fov_interp, g_fov_cut, src_tag(), smooth_live(1));
    }
    static LONG reported = 0;
    if (!reported && g_st.interp > 0) {
        reported = 1;
        log_line("X2-SMOOTH-001: first interpolated camera on a logic invocation (source %d)", src_tag());
    }
}

static bool install() {
    if (!smooth_master()) { log_line("  X2-SMOOTH-001: master X2-SMOOTH=0 - off"); return false; }
    uint8_t* a = sig_find("X2-SMOOTH-001 viewport loop head",
        "BF 01 00 00 00 33 F6 33 DB 57 53 E8 ?? ?? ?? ?? 83 C4 08 85 C0 0F 8F ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 33 FF", 0x60593F);
    uint8_t* b = sig_find("X2-SMOOTH-001 viewport loop tail",
        "43 57 53 E8 ?? ?? ?? ?? 83 C4 08 85 C0 0F 8E ?? ?? ?? ?? 46 83 FE 02 0F 8C ?? ?? ?? ?? 0F BE 05 ?? ?? ?? ?? 57 50 E8",
        0x605BB7);
    uint8_t* d = sig_find("X2-SMOOTH-001 src2 / debug cam",
        "83 3D ?? ?? ?? ?? 00 74 7C 83 3D ?? ?? ?? ?? 00 8D 43 10 50 53 74 37 68", 0x650113);
    uint8_t* v = sig_find("X2-SMOOTH-001 view / src3",
        "68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 14 E9 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D", 0x650186);
    uint8_t* c = sig_find("X2-SMOOTH-001 map camera commit",
        "50 E8 ?? ?? ?? ?? 6A 00 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 10 8B 4D FC", 0x7DB050);
    if (!a || !b || !d || !v || !c) return false;
    uint8_t* sites[3] = { a + 0x0B, b + 0x03, b + 0x26 };
    uint8_t* fin = call_target(sites[0]);
    if (!fin || call_target(sites[1]) != fin || call_target(sites[2]) != fin) {
        log_line("  X2-SMOOTH-001: the three viewport CALLs do not share one target - refused");
        return false;
    }
    if (d < fin || d > fin + 0xA00 || v < fin || v > fin + 0xA00) {
        log_line("  X2-SMOOTH-001: camera source blocks not inside the finalize - refused");
        return false;
    }
    g_src2 = (volatile uint32_t*)abs_operand("src2", d + 2);
    g_dbg = (volatile uint32_t*)abs_operand("debug cam", d + 0x0B);
    g_view = (float*)abs_operand("view", v + 1);
    g_src3 = (volatile uint32_t*)abs_operand("src3", v + 0x14);
    if (!g_src2 || !g_dbg || !g_view || !g_src3 || *(const uint32_t*)(v + 0x21) != (uint32_t)(uintptr_t)g_dbg) {
        log_line("  X2-SMOOTH-001: camera globals not as proven - refused");
        return false;
    }
    // map camera: PUSH 0; PUSH view; CALL Cam_CommitView (prologue + skip-render check at +0xB)
    uint8_t* commit = call_target(c + 0x0D);
    if (*(const uint32_t*)(c + 9) != (uint32_t)(uintptr_t)g_view ||
        !sig_match(commit, "55 8B EC 81 EC 00 01 00 00 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 8B 45 08 D9 00") ||
        !g_backlog_count) {
        log_line("  X2-SMOOTH-001: view/commit cross-check failed (commit %p) - refused", commit);
        return false;
    }
    // the camera blocks, as X2-LOOP-004 (LEA EAX,[EBX+base] in the finalize)
    uint8_t* bb = sig_find("X2-SMOOTH-001 camera blocks",
        "69 F6 40 10 00 00 DD 05 ?? ?? ?? ?? 8D 83 ?? ?? ?? ?? 83 C4 04 81 C6 ?? ?? ?? ?? 8D B8 C2 00 00 00 "
        "8D 88 DC 00 00 00", 0x64FE9E);
    if (!bb) return false;
    g_blk = (float*)abs_operand("camera blocks", bb + 0x0E);
    if (!g_blk) return false;
    g_finalize = (finalize_t)fin;
    g_commit = (commit_t)commit;
    g_fov = config_row("X2-SMOOTH-001-FOV", 1);
    uint8_t* fs = g_fov ? sig_find("X2-SMOOTH-001-FOV setters",
        "8D 45 CC 50 E8 ?? ?? ?? ?? 8D 45 D0 50 E8 ?? ?? ?? ?? 83 C4 08 83 3D", 0x6504DF) : nullptr;
    if (g_fov) {
        uint8_t* sf = fs ? call_target(fs + 4) : nullptr;
        uint8_t* sa = fs ? call_target(fs + 0x0D) : nullptr;
        if (!fs || fs < fin || fs > fin + 0xA00 || !sf || !sa) {
            log_line("  X2-SMOOTH-001: FOV setters not confirmed - X2-SMOOTH-001-FOV off");
            g_fov = 0;
        } else {
            g_setfov = (fovset_t)sf;
            g_setasp = (fovset_t)sa;
            if (!patch_call_rel32(fs + 4, sf, (void*)&h_setfov) || !patch_call_rel32(fs + 0x0D, sa, (void*)&h_setasp)) {
                patch_call_rel32(fs + 4, (void*)&h_setfov, sf);
                log_line("  X2-SMOOTH-001: FOV setter patch failed - X2-SMOOTH-001-FOV off");
                g_fov = 0;
            }
        }
    }
    for (int i = 0; i < 3; i++) {
        if (!patch_call_rel32(sites[i], fin, (void*)&h_finalize)) {
            for (int j = 0; j < i; j++) patch_call_rel32(sites[j], (void*)&h_finalize, fin);
            log_line("  X2-SMOOTH-001: patch %d failed - refused", i);
            return false;
        }
    }
    log_line("  X2-SMOOTH-001: finalize gva=0x%08X (expected 0x0064FC10), CALLs gva=0x%08X/0x%08X/0x%08X -> stub; view gva=0x%08X "
             "(0x01ACCD80), src2 0x%08X, src3 0x%08X, debug 0x%08X, commit gva=0x%08X (0x00449960); FOV %s; F7 toggles",
             rt_to_gva(fin), rt_to_gva(sites[0]), rt_to_gva(sites[1]), rt_to_gva(sites[2]), rt_to_gva(g_view),
             rt_to_gva((const void*)g_src2), rt_to_gva((const void*)g_src3), rt_to_gva((const void*)g_dbg),
             rt_to_gva(commit), g_fov ? "ON" : "OFF");
    return true;
}

Row row_SMOOTH001 = {
    "X2-SMOOTH-001",
    "60 Hz camera: logic frames draw the view half-way between the last two ticks (needs X2-SMOOTH=1)",
    1, install, pre, post, 0
};

} // namespace x2
