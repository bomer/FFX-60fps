// SPDX-License-Identifier: Apache-2.0
// SMOOTH-003: with SMOOTH-002 the root moves at 60 Hz but the skeleton pose still changes at 30 Hz. This row
// turns on the engine's half-rate motion switch and takes a second half step of each actor's motion on
// render-only frames, so limbs animate at 60 Hz at stock speed (needs master SMOOTH=1).
// Details: docs/fps60/rows/SMOOTH-003.md
#include "smooth.h"
#include <string.h>
#include <stdlib.h>

typedef void(__cdecl* step_t)(uint8_t* actor, int full);
typedef void(__cdecl* actor_fn_t)(uint8_t* actor);
typedef void(__cdecl* void_fn_t)(void);
typedef void(__cdecl* dispatch_t)(uint8_t* start, int mode, int span);

static const int      MAXA = 512;
static const uint32_t STRIDE = 0x880, FLAGS = 0x194, PARENT = 0x19C, HDR = 0x1C0, DIRTY = 0x878;
static const uint32_t BLEND_N = 0x752, MODE1 = 0x871;
// Keyframe clock fields (set by the motion start, wrapped by Ch_SeqFrame, read by the sampler).
static const uint32_t M_START = 0x718, M_END = 0x71C, M_LEN = 0x720, M_LOOPS = 0x724, M_CLOCK = 0x740,
                      M_DONE = 0x7DC, M_SEQ = 0x714;

static step_t     g_step = nullptr;
static actor_fn_t g_seq = nullptr, g_motev = nullptr;
static void_fn_t  g_noskel = nullptr, g_sync = nullptr;
static dispatch_t g_dispatch = nullptr;
static volatile int32_t*  g_half = nullptr;     // engine half-rate motion switch
static volatile int8_t*   g_freeze = nullptr;   // actor freeze flag
static uint8_t* volatile* g_table = nullptr;    // pointer to the actor array
static volatile int32_t*  g_count = nullptr;    // actor count

static int  g_gates = 1, g_hold = 1, g_blend = 0, g_cloth = 0, g_diag = 0, g_loopguard = 1, g_shortloop = 1, g_loopend = 1, g_sanity = 1, g_keyhold = 1, g_endextrap = 1;
static int  g_diag_id = 0;                       // SMOOTH-003-DIAG-ID: log every walk of this actor id (decimal)
static int  g_diag_id_end = 0;                   // SMOOTH-003-DIAG-ID-END: with DIAG-ID, an inclusive id range
static LONG g_kd_lines = 0;
static bool diag_id_match(uint16_t id) { return g_diag_id && id >= (uint16_t)g_diag_id && id <= (uint16_t)(g_diag_id_end ? g_diag_id_end : g_diag_id); }
static LONG g_diag_id_lines = 0;
static int  g_kind = INV_STOCK;
static bool g_half_on = false;                   // we wrote 1 into the switch
static volatile bool g_in_step = false;          // inside one of OUR Ch_StepMotion calls
static bool g_walked_inv = false;

static uint8_t* g_debt_base = nullptr;
static uint8_t  g_debt[MAXA];
static uint16_t g_debt_id[MAXA];
static uint8_t  g_walked[MAXA];
// Hold buffer: per slot, common regions (all walked / range actors) then the face regions (parents).
struct Region { uint32_t off, len; };
static const Region COMMON[] = { { 0x15C, 4 }, { 0x510, 0x1C4 } };
static const Region FACE[]   = { { 0x418, 8 }, { 0x4BC, 4 }, { 0x4D0, 4 }, { 0x4DC, 8 } };
static const uint32_t HOLD_SZ = 0x220;
static uint8_t* g_hold_buf = nullptr;            // MAXA * HOLD_SZ
static uint8_t  g_held[MAXA];                    // 0 = none, 1 = common, 2 = common + face
static uint8_t* g_hold_base = nullptr;
static int      g_hold_n = 0;

static LONG g_sanity_reverts = 0, g_sanity_lines = 0, g_loopend_holds = 0, g_stock_steps = 0, g_guard_wrap = 0, g_guard_clamp = 0, g_guard_hold = 0, g_diag_lines = 0, g_diag_events = 0;
static LONG g_walks = 0, g_stepped = 0, g_pays = 0, g_clock_only = 0, g_batches = 0, g_gated_seq = 0, g_gated_motev = 0;

static int slot_of(const uint8_t* a, uint8_t* base, int n) {
    if (!base || a < base) return -1;
    uintptr_t d = (uintptr_t)(a - base);
    if (d % STRIDE) return -1;
    int i = (int)(d / STRIDE);
    return (i < n && i < MAXA) ? i : -1;
}

// ---------------------------------------------------------------- hold (save / restore)
static void hold_save(int i, uint8_t* a, bool face) {
    uint8_t* p = g_hold_buf + i * HOLD_SZ;
    for (const Region& r : COMMON) { memcpy(p, a + r.off, r.len); p += r.len; }
    if (face) for (const Region& r : FACE) { memcpy(p, a + r.off, r.len); p += r.len; }
    g_held[i] = face ? 2 : 1;
}

static void hold_restore() {
    if (!g_hold_base) return;
    if (*g_table == g_hold_base) {
        int n = *g_count;
        if (n > g_hold_n) n = g_hold_n;
        for (int i = 0; i < n; i++) {
            if (!g_held[i]) continue;
            uint8_t* a = g_hold_base + i * STRIDE;
            uint8_t* p = g_hold_buf + i * HOLD_SZ;
            for (const Region& r : COMMON) { memcpy(a + r.off, p, r.len); p += r.len; }
            if (g_held[i] == 2) for (const Region& r : FACE) { memcpy(a + r.off, p, r.len); p += r.len; }
        }
    }
    memset(g_held, 0, sizeof(g_held));
    g_hold_base = nullptr;
}

// ---------------------------------------------------------------- our calls into game code (SEH)
static bool safe_step(uint8_t* a, int full) {
    g_in_step = true;
    bool ok = true;
    __try { g_step(a, full); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; on_fault("SMOOTH-003 Ch_StepMotion", GetExceptionCode()); }
    g_in_step = false;
    return ok;
}

// where_sub() codes 0x31..0x36 mark the walk's sub-steps for DIAG-HANGDUMP (diag.cpp names them).
static bool safe_dispatch(uint8_t* start, int span) {
    where_sub(0x33);
    __try { g_dispatch(start, 0, span); }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-003 skeleton dispatch", GetExceptionCode()); return false; }
    InterlockedIncrement(&g_batches);
    return true;
}

static bool safe_sync() {
    where_sub(0x32);
    __try { g_sync(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-003 job sync", GetExceptionCode()); return false; }
    return true;
}

// ---------------------------------------------------------------- pose sanity (SMOOTH-003-SANITY)
// Safety net against twisted poses: a bone angle of the render-only step that jumps far more than the actor
// moved over the last tick keeps its tick value.
static const uint32_t POSE = 0x748, BONES = 0x328, POSE_STRIDE = 0x18C, BONE_STRIDE = 0x160;
static const int MAXB = 256;
static const uint32_t BANG[3] = { 0xE4, 0xE8, 0xEC };   // bone rotation (radians)
static const float INV_2PI = 0.15915494f;
static float*   g_prev_ang = nullptr;     // MAXA * MAXB * 3: angles after the previous logic step (tick k-1)
static float*   g_cur_ang = nullptr;      // MAXA * MAXB * 3: angles after the last logic step (tick k)
static uint16_t g_ang_id[MAXA];
static uint8_t  g_ang_n[MAXA];            // valid samples 0..2
static uint8_t  g_pose_save[MAXB * POSE_STRIDE], g_bone_save[MAXB * BONE_STRIDE];

static inline float turn_diff(float x, float y) {
    float d = x - y;
    d -= (float)(int)d;                                   // fractional part, sign kept
    if (d > 0.5f) d -= 1.0f;
    if (d < -0.5f) d += 1.0f;
    return d < 0 ? -d : d;
}

static int bone_count(const uint8_t* a) {
    const uint8_t* hdr = *(uint8_t* const*)(a + HDR);
    if (!hdr) return 0;
    int n = *(const uint16_t*)(hdr + 10);
    return (n > 0 && n <= MAXB) ? n : 0;
}

// After a logic step: rotate tick k-1 <- tick k <- this pose.
static void record_angles(int i, const uint8_t* a) {
    if (!g_cur_ang || i < 0 || i >= MAXA) return;
    int nb = bone_count(a);
    const uint8_t* bones = *(uint8_t* const*)(a + BONES);
    if (!nb || !bones) { g_ang_n[i] = 0; return; }
    uint16_t id = *(const uint16_t*)a;
    if (g_ang_id[i] != id) { g_ang_n[i] = 0; g_ang_id[i] = id; }
    float* prev = g_prev_ang + (size_t)i * MAXB * 3;
    float* cur = g_cur_ang + (size_t)i * MAXB * 3;
    memcpy(prev, cur, sizeof(float) * MAXB * 3);
    for (int b = 0; b < nb; b++)
        for (int c = 0; c < 3; c++) cur[b * 3 + c] = *(const float*)(bones + b * BONE_STRIDE + BANG[c]) * INV_2PI;
    if (g_ang_n[i] < 2) g_ang_n[i]++;
}

// Before the render-only step: save pose + bones. Returns the bone count (0 = cannot check).
static int sanity_save(int i, const uint8_t* a) {
    if (i < 0 || i >= MAXA || (!g_sanity && !g_keyhold)) return 0;
    int nb = bone_count(a);
    const uint8_t* pose = *(uint8_t* const*)(a + POSE);
    const uint8_t* bones = *(uint8_t* const*)(a + BONES);
    if (!nb || !pose || !bones) return 0;
    memcpy(g_pose_save, pose, (size_t)nb * POSE_STRIDE);
    memcpy(g_bone_save, bones, (size_t)nb * BONE_STRIDE);
    return nb;
}

// After the render-only step: an angle that moved more than max(0.06 turn, 3x its last tick delta + 0.01) is a
// bad sample; restore pose + bones (tick k) and log. Returns true if reverted.
// ---------------------------------------------------------------- key hold (SMOOTH-003-KEYHOLD)
// re/ledger/SMOOTH-003-KEYDECODE.md. The render-only step samples at a half frame, where rotation channels are
// lerped per channel between the stream keys prev (+0x18) and value (+0x16) (FUN_00839e80, 1/4096 turn). Cutscene
// data sometimes stores one rotation as two Euler forms in consecutive keys (x+1/2, 1/2-y, z+1/2) or near gimbal
// lock: a per-channel lerp between them is a wrong pose (KD-008/009). A bone whose keys differ by more than
// KEYHOLD_D on any rotation channel keeps tick k for all three channels. Only per-call outputs are written (pose
// +0x1C/+0x20, bone +0xE4..+0xEC and +0x114..+0x11C); the stream cursors are untouched, so the next logic decode
// is unchanged (KD-007).
static const uint32_t SAMPLE_CLOCK = 0x744, NODE_LIST = 0x74C, NODE_SIZE = 0x2C;
static const int KEYHOLD_D = 0x300;                  // ~0.19 turn between two consecutive keys
static LONG g_keyholds = 0, g_keyhold_lines = 0;
static uint8_t g_hold_bone[MAXB];
static uint32_t g_keyhold_seen[256];                 // (id << 8 | bone) already logged
static int g_nkeyhold_seen = 0;

static bool keyhold_first(uint16_t id, int b) {
    uint32_t k = ((uint32_t)id << 8) | (uint32_t)(b & 0xFF);
    for (int j = 0; j < g_nkeyhold_seen; j++) if (g_keyhold_seen[j] == k) return false;
    if (g_nkeyhold_seen < 256) g_keyhold_seen[g_nkeyhold_seen++] = k;
    return g_nkeyhold_seen < 256;
}

// Pose entry -> bone index (FUN_00838f70 0x838F70): with a remap table at actor+0x758, pose entry c drives bone
// *(u16*)(table + 8 + 2c) (0xFFFF = none, entries < *(u16*)(table + 2)); without one, bone = c. Returns -1 for none.
static const uint32_t BONE_MAP = 0x758;
static int bone_of(const uint8_t* a, int c, int nb) {
    const uint8_t* map = *(const uint8_t* const*)(a + BONE_MAP);
    if (!map) return c < nb ? c : -1;
    if (c >= *(const uint16_t*)(map + 2)) return -1;
    uint16_t v = *(const uint16_t*)(map + 8 + 2 * c);
    return (v == 0xFFFF || v >= nb) ? -1 : (int)v;
}

static bool keyhold(int i, uint8_t* a, int nb) {
    // Also during a motion blend (+0x752 > 1): the bad half sample is the blend target there; holding the bone at
    // tick k only skips that bone's blend progress for this one render-only frame (U 11/10: villagers still broke).
    if (!nb || (*(const uint32_t*)(a + SAMPLE_CLOCK) & 0xFF) == 0) return false;
    uint8_t* pose = *(uint8_t**)(a + POSE);
    uint8_t* bones = *(uint8_t**)(a + BONES);
    if (!pose || !bones) return false;
    memset(g_hold_bone, 0, (size_t)nb);
    bool any = false;
    int guard = 0;
    for (uint8_t* node = *(uint8_t**)(a + NODE_LIST); node && guard < nb * 9; node = *(uint8_t**)node, guard++) {
        if (node < pose || node >= pose + (size_t)nb * POSE_STRIDE) continue;
        if (node[5] == 0) {
            // Linear channel (diagnostic only): log big or sign-changing jumps between the two keys.
            int16_t pv = *(const int16_t*)(node + 0x18), vv = *(const int16_t*)(node + 0x16);
            int dl = vv - pv; if (dl < 0) dl = -dl;
            if (diag_id_match(*(const uint16_t*)a) && (dl > 0x400 || ((pv ^ vv) < 0 && dl > 0x80)) &&
                InterlockedIncrement(&g_kd_lines) <= 4000)
                log_line("SMOOTH-003 keydiag: slot=%d id=0x%04X bone %d ch %d LINEAR keys %d -> %d clock 0x%X out %.3f/%.3f",
                         i, *(const uint16_t*)a, (int)((node - pose) / POSE_STRIDE), (int)(((node - pose) % POSE_STRIDE) / NODE_SIZE),
                         pv, vv, *(const uint32_t*)(a + SAMPLE_CLOCK), *(const float*)(node + 0x1C), *(const float*)(node + 0x20));
            continue;
        }
        int d = (int)(uint16_t)(*(const int16_t*)(node + 0x16) - *(const int16_t*)(node + 0x18)) & 0xFFF;
        if (d > 0x800) d = 0x1000 - d;
        if (d > 0x100 && diag_id_match(*(const uint16_t*)a) && InterlockedIncrement(&g_kd_lines) <= 4000)
            log_line("SMOOTH-003 keydiag: slot=%d id=0x%04X bone %d ch %d keys %d -> %d d 0x%X clock 0x%X %s", i, *(const uint16_t*)a,
                     (int)((node - pose) / POSE_STRIDE), (int)(((node - pose) % POSE_STRIDE) / NODE_SIZE),
                     *(const int16_t*)(node + 0x18), *(const int16_t*)(node + 0x16), d, *(const uint32_t*)(a + SAMPLE_CLOCK),
                     d > KEYHOLD_D ? "HELD" : "passed");
        if (d <= KEYHOLD_D) continue;
        int b = (int)((node - pose) / POSE_STRIDE);
        if (!g_hold_bone[b] && keyhold_first(*(const uint16_t*)a, b)) {
            InterlockedIncrement(&g_keyhold_lines);
            int ch = (int)(((node - pose) % POSE_STRIDE) / NODE_SIZE);
            log_line("SMOOTH-003 keyhold: slot=%d id=0x%04X bone %d ch %d: keys %d -> %d (d 0x%X) clock 0x%X blend %d - bone held at tick k (first for this bone)",
                     i, *(const uint16_t*)a, b, ch, *(const int16_t*)(node + 0x18), *(const int16_t*)(node + 0x16), d,
                     *(const uint32_t*)(a + SAMPLE_CLOCK), (int)*(const int16_t*)(a + BLEND_N));
        }
        g_hold_bone[b] = 1;
        any = true;
    }
    if (!any) return false;
    if (g_keyhold == 2) {
        // Whole actor: every node output (+0x1C target, +0x20 current) and every bone back to tick k for this
        // render-only frame. The stream state in the nodes (+0x6, +0xC, +0x14..+0x18) stays as the decoder left it:
        // the next logic decode continues from it (KD-007).
        for (int pe = 0; pe < nb; pe++)
            for (int c = 0; c < 9; c++) {
                uint32_t po = (uint32_t)pe * POSE_STRIDE + (uint32_t)c * NODE_SIZE;
                *(float*)(pose + po + 0x1C) = *(const float*)(g_pose_save + po + 0x1C);
                *(float*)(pose + po + 0x20) = *(const float*)(g_pose_save + po + 0x20);
            }
        memcpy(bones, g_bone_save, (size_t)nb * BONE_STRIDE);
        InterlockedIncrement(&g_keyholds);
        return true;
    }
    for (int pe = 0; pe < nb; pe++) {                 // pe = pose entry; its bone may differ (remap table)
        if (!g_hold_bone[pe]) continue;
        int bi = bone_of(a, pe, nb);
        for (int c = 0; c < 3; c++) {
            uint32_t po = (uint32_t)pe * POSE_STRIDE + (uint32_t)c * NODE_SIZE;
            *(float*)(pose + po + 0x1C) = *(const float*)(g_pose_save + po + 0x1C);
            *(float*)(pose + po + 0x20) = *(const float*)(g_pose_save + po + 0x20);
            if (bi < 0) continue;
            uint32_t bo = (uint32_t)bi * BONE_STRIDE + BANG[c];
            *(float*)(bones + bo) = *(const float*)(g_bone_save + bo);
            *(float*)(bones + bo + 0x30) = *(const float*)(g_bone_save + bo + 0x30);
        }
        InterlockedIncrement(&g_keyholds);
    }
    return true;
}

// Diagnostic (DIAG-ID actors): after KEYHOLD, log every bone whose final render-only rotation is still far from
// tick k, with the stream keys of its rotation nodes. Observe only.
static LONG g_outdiag_lines = 0;
static void out_diag(int i, uint8_t* a, int nb) {
    uint8_t* pose = *(uint8_t**)(a + POSE);
    uint8_t* bones = *(uint8_t**)(a + BONES);
    if (!pose || !bones) return;
    for (int pe = 0; pe < nb; pe++) {
        int b = bone_of(a, pe, nb);
        if (b < 0) continue;
        float worst = 0; int wc = 0;
        for (int c = 0; c < 3; c++) {
            float d = turn_diff(*(const float*)(bones + b * BONE_STRIDE + BANG[c]) * INV_2PI,
                                *(const float*)(g_bone_save + b * BONE_STRIDE + BANG[c]) * INV_2PI);
            if (d > worst) { worst = d; wc = c; }
        }
        if (worst <= 0.08f || InterlockedIncrement(&g_outdiag_lines) > 4000) continue;
        const uint8_t* n0 = pose + (size_t)pe * POSE_STRIDE;
        log_line("SMOOTH-003 outdiag: slot=%d id=0x%04X pose %d bone %d ch %d off %.3f turn from tick k; clock 0x%X; keys x %d->%d y %d->%d z %d->%d",
                 i, *(const uint16_t*)a, pe, b, wc, worst, *(const uint32_t*)(a + SAMPLE_CLOCK),
                 *(const int16_t*)(n0 + 0x18), *(const int16_t*)(n0 + 0x16),
                 *(const int16_t*)(n0 + NODE_SIZE + 0x18), *(const int16_t*)(n0 + NODE_SIZE + 0x16),
                 *(const int16_t*)(n0 + 2 * NODE_SIZE + 0x18), *(const int16_t*)(n0 + 2 * NODE_SIZE + 0x16));
    }
}

// ---------------------------------------------------------------- end extrapolation (SMOOTH-003-ENDEXTRAP)
// At the end of a motion the render-only sample cannot move on: a play-once motion is clamped to its last frame
// (guard kind 2) and LOOPEND holds a loop's last frame. Tidus's run is a chain of 28-frame play-once strides and
// his idle a 40-frame loop, so each end repeated one pose for a render-only frame (U 11/10: stutter at the end of
// his run animation). Instead, continue each bone rotation half a tick past tick k from its last two logic
// values: k + (k - (k-1)) / 2. A channel that moved more than ENDX_MAX in the last tick (flip, cut) keeps tick k.
static const float ENDX_MAX = 0.08f;               // turn per tick
static LONG g_endx = 0;

static void end_extrapolate(int i, uint8_t* a) {
    if (!g_cur_ang || i < 0 || i >= MAXA || g_ang_n[i] < 2 || g_ang_id[i] != *(const uint16_t*)a) return;
    int nb = bone_count(a);
    uint8_t* bones = *(uint8_t**)(a + BONES);
    if (!nb || !bones) return;
    const float* prev = g_prev_ang + (size_t)i * MAXB * 3;
    const float* cur = g_cur_ang + (size_t)i * MAXB * 3;
    for (int b = 0; b < nb; b++)
        for (int c = 0; c < 3; c++) {
            float d = cur[b * 3 + c] - prev[b * 3 + c];
            d -= (float)(int)d;                          // shortest signed difference in turns
            if (d > 0.5f) d -= 1.0f;
            if (d < -0.5f) d += 1.0f;
            if (d > ENDX_MAX || d < -ENDX_MAX) continue;
            float v = (cur[b * 3 + c] + 0.5f * d) * 6.2831853f;
            *(float*)(bones + b * BONE_STRIDE + BANG[c]) = v;
            *(float*)(bones + b * BONE_STRIDE + BANG[c] + 0x30) = v;
        }
    if (InterlockedIncrement(&g_endx) == 1)
        log_line("SMOOTH-003: first end-of-motion half-tick extrapolation (ENDEXTRAP), id 0x%04X", *(const uint16_t*)a);
}

static bool sanity_check(int i, uint8_t* a, int nb, int32_t c0) {
    if (!nb || !g_sanity || !g_cur_ang || g_ang_n[i] < 2 || g_ang_id[i] != *(const uint16_t*)a) return false;
    uint8_t* bones = *(uint8_t**)(a + BONES);
    if (!bones) return false;
    const float* prev = g_prev_ang + (size_t)i * MAXB * 3;
    const float* cur = g_cur_ang + (size_t)i * MAXB * 3;
    bool any = false;
    for (int b = 0; b < nb; b++) {
        for (int c = 0; c < 3; c++) {
            const uint8_t* kb = g_bone_save + b * BONE_STRIDE;
            uint8_t* hb = bones + b * BONE_STRIDE;
            float vk = *(const float*)(kb + BANG[c]) * INV_2PI;
            float vh = *(const float*)(hb + BANG[c]) * INV_2PI;
            float dh = turn_diff(vh, vk), dt = turn_diff(cur[b * 3 + c], prev[b * 3 + c]);
            if (dh <= 0.06f || dh <= 3.0f * dt + 0.01f) continue;   // NaN falls through to the restore
            // Per channel: this bone axis keeps the tick value; every other channel keeps its half-step sample.
            *(float*)(hb + BANG[c]) = *(const float*)(kb + BANG[c]);
            *(float*)(hb + BANG[c] + 0x30) = *(const float*)(kb + BANG[c] + 0x30);
            InterlockedIncrement(&g_sanity_reverts);
            any = true;
            if (g_sanity_lines < 20) {
                InterlockedIncrement(&g_sanity_lines);
                log_line("SMOOTH-003 sanity: slot=%d id=0x%04X bone %d ch %d: k-1 %.4f k %.4f half %.4f (d %.4f vs tick %.4f) "
                         "clock %d->%d start=%d end=%d len=%d loops=%d done=%d blend=%d fl=%08X - channel held",
                         i, *(const uint16_t*)a, b, c, prev[b * 3 + c], vk, vh, dh, dt, c0, *(int32_t*)(a + M_CLOCK),
                         *(int32_t*)(a + M_START), *(int32_t*)(a + M_END), *(int32_t*)(a + M_LEN),
                         (int)*(int16_t*)(a + M_LOOPS), *(int32_t*)(a + M_DONE), (int)*(int16_t*)(a + BLEND_N),
                         *(uint32_t*)(a + FLAGS));
            }
        }
    }
    return any;
}

// SMOOTH-003-SHORTLOOP: a looping motion of at most one frame (e.g. an enemy hold pose) is only ever sampled at
// whole frames at stock; a half-frame sample twists the body, so such actors run exactly as stock.
static bool short_loop(const uint8_t* a) {
    uint32_t fl = *(const uint32_t*)(a + FLAGS);
    if ((fl & 0x40) != 0 || *(const uint32_t*)(a + M_SEQ) == 0) return false;
    return *(const int16_t*)(a + M_LOOPS) != 1 && *(const int32_t*)(a + M_LEN) <= 0x100;
}

// Actors that keep stock 30 Hz limbs: their logic step runs with the half switch off (after any half-step debt is
// paid), so their pose is sampled exactly where stock samples it, and the render-only walk leaves them alone.
// SMOOTH-003-STOCK-ID / -STOCK-ID-END (decimal, inclusive): actors in this id range take whole stock steps
// (bisect switch: which actor's half-step causes a glitch).
static int g_stock_id = 0, g_stock_id_end = 0;
static bool stock_id(const uint8_t* a) {
    if (!g_stock_id) return false;
    uint16_t id = *(const uint16_t*)a;
    return id >= (uint16_t)g_stock_id && id <= (uint16_t)(g_stock_id_end ? g_stock_id_end : g_stock_id);
}

static bool stock_actor(const uint8_t* a) {
    if (stock_id(a)) return true;
    if (g_shortloop && short_loop(a)) return true;
    uint32_t fl = *(const uint32_t*)(a + FLAGS);
    return g_cloth == 1 && (fl & 0x600000) == 0x600000;
}

static void __cdecl h_step(uint8_t* a, int full) {
    if (g_disabled || !row_SMOOTH003.on) {
        if (g_half_on) { *g_half = 0; g_half_on = false; }
        g_step(a, full);
        return;
    }
    uint8_t* base = *g_table;
    int n = *g_count;
    if (base != g_debt_base) { memset(g_debt, 0, sizeof(g_debt)); g_debt_base = base; }
    int i = slot_of(a, base, n);
    if (i >= 0 && g_debt[i]) {
        g_debt[i] = 0;
        if (g_debt_id[i] == *(uint16_t*)a) {
            // Pay the half step the render-only walk did not take: clock only, switch forced on.
            int32_t old = *g_half;
            *g_half = 1;
            bool ok = safe_step(a, 0);
            *g_half = old;
            if (!ok) { *g_half = 0; g_half_on = false; g_step(a, full); return; }
            InterlockedIncrement(&g_pays);
        }
    }
    if (*g_half != 0 && stock_actor(a)) {
        *g_half = 0;                                    // whole stock step: no half step, no debt
        g_step(a, full);
        *g_half = 1;
        InterlockedIncrement(&g_stock_steps);
        if (full && (g_sanity || g_endextrap)) __try { record_angles(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    g_step(a, full);
    if (full && (g_sanity || g_endextrap)) __try { record_angles(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (i >= 0 && *g_half != 0) { g_debt[i] = 1; g_debt_id[i] = *(uint16_t*)a; }
}

// ---------------------------------------------------------------- gates (SMOOTH-003-GATES)
// Ch_SeqFrame is a per-call script VM and Ch_MotevDispatch fires motion events: neither may run twice per tick.
static void __cdecl h_seq(uint8_t* a) {
    if (g_in_step && g_gates && !g_disabled) { InterlockedIncrement(&g_gated_seq); return; }
    g_seq(a);
}

static void __cdecl h_motev(uint8_t* a) {
    if (g_walked_inv && g_gates && !g_disabled) {
        int i = slot_of(a, *g_table, *g_count);
        if (i >= 0 && g_walked[i]) { InterlockedIncrement(&g_gated_motev); return; }
    }
    g_motev(a);
}

// ---------------------------------------------------------------- loop guard (SMOOTH-003-LOOPGUARD)
// Returns the clock the stock logic step would sample at for clock c (the loop-wrap arithmetic of Ch_SeqFrame,
// read-only; no loop count is decremented). kind: 0 = unchanged, 1 = wrapped, 2 = clamped (play once), 3 = no
// stock equivalent (hold).
static int32_t guard_clock(const uint8_t* a, int32_t c, int* kind) {
    *kind = 0;
    int32_t end = *(const int32_t*)(a + M_END);
    int32_t len = *(const int32_t*)(a + M_LEN);
    int16_t loops = *(const int16_t*)(a + M_LOOPS);
    if (c < 0) { *kind = 1; return 0; }
    if (loops == 1) {
        // Play once: stock never samples past the last whole frame.
        int32_t last = end & ~0xFF;
        if (c > last) { *kind = 2; return last; }
        return c;
    }
    if (c <= end) return c;
    if (len <= 0) {
        if (loops == 0 && len == 0) { *kind = 1; return *(const int32_t*)(a + M_START); }
        *kind = 3;
        return c;
    }
    int n = 64;
    while (c > end && n-- > 0) c -= len;
    if (c > end) { *kind = 3; return c; }
    *kind = 1;
    return c;
}

static const char* const GUARD_NAME[] = { "none", "wrap", "clamp", "hold" };

static void diag_actor(int i, const uint8_t* a, int32_t c0, int32_t cs, int32_t c1, int done0, int kind, bool child) {
    int done1 = *(const int32_t*)(a + M_DONE);
    bool rewind = done1 < done0;
    int16_t blend = *(const int16_t*)(a + BLEND_N);
    uint8_t m1 = a[MODE1];
    LONG ev;
    if (g_diag_id) {
        if (!diag_id_match(*(const uint16_t*)a)) return;
        ev = InterlockedIncrement(&g_diag_events);
        if (InterlockedIncrement(&g_diag_id_lines) > 20000) return;
    } else {
        if (!(kind || rewind || blend > 1 || m1 || child)) return;
        ev = InterlockedIncrement(&g_diag_events);
        if (g_diag_lines >= 300 && (ev % 200) != 0) return;
        InterlockedIncrement(&g_diag_lines);
    }
    log_line("SMOOTH-003 diag: slot=%d id=0x%04X clock %d (f%d+%02X) sample %d -> %d start=%d end=%d len=%d loops=%d "
             "done %d->%d%s guard=%s blend=%d mode1=%d parent=%08X fl=%08X seq=%08X ev=%ld",
             i, *(const uint16_t*)a, c0, c0 >> 8, c0 & 0xFF, cs, c1, *(const int32_t*)(a + M_START),
             *(const int32_t*)(a + M_END), *(const int32_t*)(a + M_LEN), (int)*(const int16_t*)(a + M_LOOPS), done0, done1,
             rewind ? " REWIND" : "", GUARD_NAME[kind & 3], (int)blend, (int)m1, *(const uint32_t*)(a + PARENT),
             *(const uint32_t*)(a + FLAGS), *(const uint32_t*)(a + M_SEQ), ev);
}

// The render-only full step of one actor (bit 24 already set). Returns false on a fault (the walk aborts).
static bool walk_step(int i, uint8_t* a, uint32_t fl) {
    bool child = *(uint32_t*)(a + PARENT) != 0;
    int32_t c0 = *(int32_t*)(a + M_CLOCK);
    int done0 = *(int32_t*)(a + M_DONE);
    int kind = 0;
    int32_t cs = c0;
    // Normal keyframe path only (+0x714 != 0, bit 0x40 clear); the bit-0x40 script paths wrap the clock on every call.
    if (g_loopguard && (fl & 0x40) == 0 && *(uint32_t*)(a + M_SEQ) != 0) cs = guard_clock(a, c0, &kind);
    // SMOOTH-003-LOOPEND: a half-frame sample inside the last frame of a loop reads past the loop end (bad pose);
    // stock samples there only at whole frames, so hold the pose for that one frame.
    if (g_loopend && kind <= 1 && (fl & 0x40) == 0 && *(uint32_t*)(a + M_SEQ) != 0 && (cs & 0xFF) != 0 &&
        *(int16_t*)(a + M_LOOPS) != 1 && *(int32_t*)(a + M_LEN) > 0 && (cs >> 8) >= (*(int32_t*)(a + M_END) >> 8)) {
        if (!safe_step(a, 0)) return false;               // clock only: pose stays at tick k
        InterlockedIncrement(&g_loopend_holds);
        if (g_endextrap) __try { end_extrapolate(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (diag_id_match(*(const uint16_t*)a) && InterlockedIncrement(&g_kd_lines) <= 4000)
            log_line("SMOOTH-003 diag: slot=%d id=0x%04X LOOPEND hold: clock %d (f%d+%02X) sample %d start=%d end=%d len=%d loops=%d",
                     i, *(const uint16_t*)a, c0, c0 >> 8, c0 & 0xFF, cs, *(const int32_t*)(a + M_START),
                     *(const int32_t*)(a + M_END), *(const int32_t*)(a + M_LEN), (int)*(const int16_t*)(a + M_LOOPS));
        return true;
    }
    if (kind == 3) {
        if (!safe_step(a, 0)) return false;               // clock only: pose stays at tick k
        InterlockedIncrement(&g_guard_hold);
        if (g_diag >= 2) diag_actor(i, a, c0, c0, *(int32_t*)(a + M_CLOCK), done0, kind, child);
        return true;
    }
    if (kind) *(int32_t*)(a + M_CLOCK) = cs;
    int nb = 0;
    __try { nb = sanity_save(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) { nb = 0; }
    bool ok = safe_step(a, 1);
    // Clamped play-once end: extrapolate only when the logic tick already showed the last frame (the clamped
    // sample then repeats it); an earlier clock gets a real new frame from the clamp.
    if (ok && kind == 2 && g_endextrap && c0 >= (*(int32_t*)(a + M_END) & ~0xFF))
        __try { end_extrapolate(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (ok && nb) {
        if (g_keyhold) { __try { keyhold(i, a, nb); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        if (diag_id_match(*(const uint16_t*)a)) { __try { out_diag(i, a, nb); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        else if (g_sanity) { __try { sanity_check(i, a, nb, c0); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    }
    if (kind) {
        // Unwrapped clock + the half step just added: the next logic Ch_SeqFrame wraps / counts down exactly as stock.
        *(int32_t*)(a + M_CLOCK) = c0 + (*(int32_t*)(a + M_CLOCK) - cs);
        InterlockedIncrement(kind == 2 ? &g_guard_clamp : &g_guard_wrap);
    }
    if (ok && g_diag >= 2) diag_actor(i, a, c0, cs, *(int32_t*)(a + M_CLOCK), done0, kind, child);
    return ok;
}

// ---------------------------------------------------------------- render-only walk
// Mirrors the logic walk of Ch_UpdateAllActors for actors that owe a half step, then dispatches skeleton jobs.
static void walk() {
    uint8_t* base = *g_table;
    int n = *g_count;
    if (!base || n <= 0 || base != g_debt_base) return;
    if (n > MAXA) n = MAXA;
    if (!safe_sync()) return;
    if (g_hold) {
        g_hold_base = base;
        g_hold_n = n;
        for (int i = 0; i < n; i++) {
            uint8_t* a = base + i * STRIDE;
            if (a[2] == 0 || (a[0x180] != 0 && a[0x181] == 0)) continue;
            bool child = *(uint32_t*)(a + PARENT) != 0;
            if (!child) hold_save(i, a, true);                  // mode-0 jobs touch every parent in range
            else if (g_debt[i]) hold_save(i, a, false);         // children: only the rebuild state
        }
    }
    uint8_t* cursor = base;
    int span = 0, bones = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * STRIDE;
        span++;
        if (a[2] != 0 && (a[0x180] == 0 || a[0x181] != 0) && g_debt[i]) {
            g_debt[i] = 0;
            uint32_t fl = *(uint32_t*)(a + FLAGS);
            bool cloth = (fl & 0x600000) == 0x600000;
            if (g_debt_id[i] != *(uint16_t*)a) goto next;      // slot reused: drop the stale debt
            if (cloth && g_cloth == 1) { g_debt[i] = 1; goto next; }  // paid at the next logic step
            if (g_shortloop && short_loop(a)) { g_debt[i] = 1; goto next; }  // stock from the next logic step
            if (stock_id(a)) { g_debt[i] = 1; goto next; }                    // bisect switch, stock from the next logic step
            {
                bool hold_pose = g_blend && (fl & 0x40) == 0 && (*(int16_t*)(a + BLEND_N) > 1 || a[MODE1] != 0);
                if (hold_pose) {
                    if (!safe_step(a, 0)) return;               // clock only: pose stays at tick k
                    InterlockedIncrement(&g_clock_only);
                    goto next;
                }
                *(uint32_t*)(a + FLAGS) = fl | 0x1000000;
                if (!walk_step(i, a, fl)) return;
                g_walked[i] = 1;
                g_walked_inv = true;
                InterlockedIncrement(&g_stepped);
                if (*(uint32_t*)(a + PARENT) == 0) {
                    uint8_t* hdr = *(uint8_t**)(a + HDR);
                    if (hdr) {
                        *(uint32_t*)(a + DIRTY) = 1;
                        bones += *(uint16_t*)(hdr + 10);
                    }
                }
            }
        }
    next:
        if (bones > 199) {
            if (!safe_dispatch(cursor, span)) return;
            cursor = a + STRIDE;
            span = 0;
            bones = 0;
        }
    }
    if (bones != 0) safe_dispatch(cursor, span);
}

static void __cdecl h_noskel() {
    if (row_SMOOTH003.on && !g_disabled && g_kind == INV_BUILD && smooth_live() && *g_freeze == 0) {
        g_walks++;
        where_sub(0x31);
        __try { walk(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-003 walk", GetExceptionCode()); }
    }
    where_sub(0x34);
    g_noskel();
    where_sub(0x35);
    if (g_walked_inv && g_cloth == 2 && !g_disabled) {
        __try {
            uint8_t* base = *g_table;
            int n = *g_count;
            if (n > MAXA) n = MAXA;
            for (int i = 0; i < n; i++) {
                if (!g_walked[i]) continue;
                uint32_t* fl = (uint32_t*)(base + i * STRIDE + FLAGS);
                if ((*fl & 0x600000) == 0x600000) *fl |= 0x1000;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-003 cloth hold", GetExceptionCode()); }
    }
    where_sub(0x36);
}

// ---------------------------------------------------------------- per invocation
// ---------------------------------------------------------------- SMOOTH-003-CLOTHHOLD (default 4)
// Ch_ClothSimulate 0x822DE0 (void cdecl(actor), plain RET 0x8245CF; only caller the cloth job FUN_0082e390, worker
// thread) is position Verlet with a fixed step and no dt. A walked actor has 0x1000 cleared on render-only frames,
// so its cloth job ran twice per tick: hair and cloth swung 2x and vibrated (U 12/10/2026: Kimahri's hair). On those
// frames the step returns at once; the cloth mesh matrix is rebuilt per Ch_DrawMain call (FUN_008314c0), so held
// cloth follows the half-stepped bones. re/ledger/SMOOTH-CLOTH.md.
// Mode 1 still vibrated (U 12/10/2026): the pinned rows follow the half-stepped bones while the free rows hold.
// Mode 2 skips the whole cloth job FUN_0082e390 (cdecl (actor**, int* count), plain RET; attach FUN_00832110, sim,
// deform FUN_0082dbc0) for walked actors: the cloth mesh keeps its tick shape, rigid to its root bone.
static int g_clothhold = 4;
static uint8_t* g_cjob_fn = nullptr;
static void (__cdecl* g_cjob_tramp)(uint8_t**, int*) = nullptr;
static uint8_t* g_cloth_fn = nullptr;
static void (__cdecl* g_cloth_tramp)(uint8_t*) = nullptr;
static bool g_cloth_hooked = false;
static volatile LONG g_cloth_held = 0;

static void __cdecl h_cloth(uint8_t* a) {
    if (g_walked_inv && !g_disabled) {
        int i = slot_of(a, *g_table, *g_count);
        if (i >= 0 && g_walked[i]) {
            if (InterlockedIncrement(&g_cloth_held) == 1) log_line("SMOOTH-003-CLOTHHOLD: first cloth step held on a render-only frame (id 0x%04X)", *(uint16_t*)a);
            return;
        }
    }
    g_cloth_tramp(a);
}

// Mode 3: mode 2 made the cloth 30 Hz (U 12/10/2026: "cloth movement not as smooth"). On render-only frames the
// job is replaced: every dynamic-mesh vertex array (entry+0x08, vec4 x desc+4) is extrapolated half a tick from
// its last two job results (pinned and free vertices alike, in the cloth root-bone frame, matching the half-
// stepped bones), the deform FUN_0082dbc0 (job +0x31, gated by the global at job +0x29) rebuilds the mesh from
// it, and the array is put back. No sim step runs, so the cloth cadence and Verlet state stay stock.
static void (__cdecl* g_deform)(uint8_t*) = nullptr;
static volatile int32_t* g_deform_gate = nullptr;
static int g_extrap_pct = 50;                    // SMOOTH-003-CLOTHEXTRAP-PCT: lead in percent of one tick
static volatile LONG g_step_n = 1;               // invocations that ran logic (main thread writes, jobs read)
static volatile LONG g_extrap_n = 0, g_extrap_miss = 0;

struct ClothSnap { float* vtex; uint8_t* actor; uint32_t n; LONG ncur, nprev; float* cur; float* prev; };
enum { NSNAP = 128 };
static ClothSnap g_snap[NSNAP];
static SRWLOCK g_snap_lock = SRWLOCK_INIT;

static ClothSnap* snap_get(float* vtex, uint8_t* actor, uint32_t n, bool create) {
    AcquireSRWLockExclusive(&g_snap_lock);
    ClothSnap* s = nullptr; ClothSnap* old = &g_snap[0];
    for (int i = 0; i < NSNAP; i++) {
        if (g_snap[i].vtex == vtex) { s = &g_snap[i]; break; }
        if (g_snap[i].ncur < old->ncur) old = &g_snap[i];
    }
    if (!s && create) { s = old; s->vtex = vtex; s->ncur = s->nprev = 0; }
    if (s && create && (s->n != n || s->actor != actor)) {
        free(s->cur); free(s->prev);
        s->cur = (float*)malloc(n * 16); s->prev = (float*)malloc(n * 16);
        s->n = (s->cur && s->prev) ? n : 0; s->actor = actor; s->ncur = s->nprev = 0;
    }
    ReleaseSRWLockExclusive(&g_snap_lock);
    return s;
}

// Calls fn(entry vtex, vertex count) for each dynamic mesh of the actor.
template <class F> static void for_dyn_mesh(uint8_t* a, F fn) {
    uint8_t* ents = *(uint8_t**)(a + 0x1BC);
    uint8_t* mdl = *(uint8_t**)(a + 0x1C0);
    if (!ents || !mdl) return;
    uint32_t ne = *(uint16_t*)(mdl + 6);
    for (uint32_t e = 0; e < ne; e++) {
        uint8_t* en = ents + e * 0x38;
        uint8_t* desc = *(uint8_t**)en;
        float* vtex = *(float**)(en + 8);
        if (!desc || !vtex || (!*(void**)(en + 0x2C) && !*(void**)(en + 0x0C))) continue;
        uint32_t n = *(uint16_t*)(desc + 4);
        if (n) fn(en, vtex, n);
    }
}

static void cloth_snap_after_step(uint8_t* a, bool reset) {
    for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t n) {
        ClothSnap* s = snap_get(vtex, a, n, true);
        if (!s || !s->n) return;
        float* t = s->prev; s->prev = s->cur; s->cur = t;   // old cur becomes prev
        s->nprev = reset ? 0 : s->ncur;
        memcpy(s->cur, vtex, n * 16);
        s->ncur = g_step_n;
    });
}

// Mode 4 (default; U 12/10/2026: looks best of 0/2/3/4 via F5): extrapolation overshot the swing and amplified the per-tick wind jitter (U 12/10/2026: "faster than
// expected"). Interpolation instead: after the logic job the mesh is redeformed at the midpoint of the last two job
// results, and a render-only frame deforms the latest one. Cloth trails the half-stepped bones by half a tick.
// True when every dynamic mesh had fresh job results (and the previous tick's, if need_prev); then the mesh is
// deformed from cur + k * (cur - prev).
static bool cloth_extrap(uint8_t* a, float k, bool need_prev) {
    LONG now = g_step_n;
    bool ok = true;
    for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t n) {
        ClothSnap* s = snap_get(vtex, a, n, false);
        if (!s || s->n != n || s->ncur != now || (need_prev && s->nprev != now - 1)) ok = false;
    });
    if (!ok) return false;
    for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t n) {
        ClothSnap* s = snap_get(vtex, a, n, false);
        for (uint32_t v = 0; v < n * 4; v += 4)
            for (int c = 0; c < 3; c++) vtex[v + c] = s->cur[v + c] + k * (s->cur[v + c] - s->prev[v + c]);
    });
    if (*g_deform_gate == 0) g_deform(a);
    for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t n) {
        ClothSnap* s = snap_get(vtex, a, n, false);
        memcpy(vtex, s->cur, n * 16);
    });
    return true;
}

static void __cdecl h_cjob(uint8_t** list, int* count) {
    if (g_clothhold >= 2 && g_walked_inv && !g_disabled) {
        bool all = true;
        __try {
            for (int k = 0; k < *count && all; k++) {
                int i = slot_of(list[k], *g_table, *g_count);
                all = i >= 0 && g_walked[i];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { all = false; }
        if (all) {
            if (g_clothhold >= 3) {
                __try {
                    for (int k = 0; k < *count; k++) {
                        if (g_clothhold == 3 ? cloth_extrap(list[k], g_extrap_pct / 100.0f, true) : cloth_extrap(list[k], 0.0f, false)) {
                            LONG c = InterlockedIncrement(&g_extrap_n);
                            if (c % 3600 == 0) log_line("SMOOTH-003-CLOTHHOLD: %ld meshes extrapolated, %ld held (no two fresh job results)", c, (LONG)g_extrap_miss);
                            if (c == 1) log_line("SMOOTH-003-CLOTHHOLD: first cloth mesh extrapolated on a render-only frame (mode %d, id 0x%04X)", g_clothhold, *(uint16_t*)list[k]);
                        } else InterlockedIncrement(&g_extrap_miss);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-003 cloth extrap", GetExceptionCode()); }
            } else if (InterlockedIncrement(&g_cloth_held) == 1) log_line("SMOOTH-003-CLOTHHOLD: first cloth job skipped on a render-only frame (mode 2)");
            return;
        }
    }
    if (g_clothhold < 3) { g_cjob_tramp(list, count); return; }
    // Reset/teleport (+0x810 counter, +0x194 bit 0x800000) or an uninitialised mesh: no extrapolation from it.
    bool reset[8] = {};
    int n = *count < 8 ? *count : 8;
    __try {
        for (int k = 0; k < n; k++) {
            uint8_t* a = list[k];
            reset[k] = *(uint16_t*)(a + 0x810) != 0 || (*(uint32_t*)(a + 0x194) & 0x800000) != 0;
            for_dyn_mesh(a, [&](uint8_t* en, float*, uint32_t) { if (*(void**)(en + 0x0C) && !*(int*)(en + 0x34)) reset[k] = true; });
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_cjob_tramp(list, count);
    __try {
        for (int k = 0; k < n; k++) {
            cloth_snap_after_step(list[k], reset[k]);
            if (g_clothhold == 4 && !reset[k]) cloth_extrap(list[k], -0.5f, true);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-003 cloth snap", GetExceptionCode()); }
}

// Test hotkey F5 (game window in front): cycle the job-hook modes 4 -> 3 -> 2 -> 0 (stock, cloth 2x) -> 4.
static void cloth_hotkey() {
    static bool prev = false;
    bool down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    bool edge = down && !prev;
    prev = down;
    if (!edge || !g_cjob_tramp) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    int m = g_clothhold == 4 ? 3 : g_clothhold == 3 ? 2 : g_clothhold == 2 ? 0 : 4;
    if (m >= 3 && !g_deform) m = m == 4 ? 2 : 0;
    g_clothhold = m;
    log_line("F5: SMOOTH-003-CLOTHHOLD mode %d (%s)", m, m == 4 ? "interpolated" : m == 3 ? "extrapolated" : m == 2 ? "held, 30 Hz" : "stock, cloth 2x");
}

static void pre() {
    cloth_hotkey();
    if (g_clothhold && !g_cloth_hooked) {
        g_cloth_hooked = true;
        if (g_clothhold >= 2) {
            if (hook_entry("SMOOTH-003-CLOTHHOLD", g_cjob_fn, (void*)&h_cjob, (void**)&g_cjob_tramp))
                log_line("SMOOTH-003-CLOTHHOLD: cloth job gva 0x%08X hooked: whole job once per logic tick (mode %d%s)", rt_to_gva_exe(g_cjob_fn),
                         g_clothhold, g_clothhold == 3 ? ", render-only frames extrapolated" : g_clothhold == 4 ? ", interpolated half a tick behind" : "");
            else g_clothhold = 0;
        } else if (hook_entry("SMOOTH-003-CLOTHHOLD", g_cloth_fn, (void*)&h_cloth, (void**)&g_cloth_tramp))
            log_line("SMOOTH-003-CLOTHHOLD: Ch_ClothSimulate gva 0x%08X hooked: cloth steps once per logic tick", rt_to_gva_exe(g_cloth_fn));
        else g_clothhold = 0;
    }
    smooth_poll_hotkey();
    if (g_hold_base) {
        __try { hold_restore(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-003 hold restore", GetExceptionCode()); }
    }
    g_kind = g_disabled ? INV_STOCK : pacing_kind();
    if (g_kind != INV_BUILD) InterlockedIncrement(&g_step_n);
    bool want = !g_disabled && smooth_live() && (g_kind == INV_LOGIC || g_kind == INV_BUILD);
    *g_half = want ? 1 : 0;
    g_half_on = want;
    g_walked_inv = false;
    memset(g_walked, 0, sizeof(g_walked));
}

static void post() {
    static LONG last = 0;
    if (g_diag && g_walks - last >= 600) {
        last = g_walks;
        log_line("SMOOTH-003: walks=%ld stepped=%ld clock_only=%ld batches=%ld pays=%ld gated(seq=%ld motev=%ld) "
                 "loopguard(wrap=%ld clamp=%ld hold=%ld) stock_steps=%ld loopend_holds=%ld sanity_reverts=%ld diag_events=%ld half=%d live=%d",
                 g_walks, g_stepped, g_clock_only, g_batches, g_pays, g_gated_seq, g_gated_motev,
                 g_guard_wrap, g_guard_clamp, g_guard_hold, g_stock_steps, g_loopend_holds, g_sanity_reverts, g_diag_events, *g_half, smooth_live());
    }
    static LONG reported_s = 0;
    if (!reported_s && g_stock_steps > 0) {
        reported_s = 1;
        log_line("SMOOTH-003: first stock logic step for a short-loop / short-loop actor (no half step)");
    }
    static LONG reported = 0;
    if (!reported && g_stepped > 0) {
        reported = 1;
        log_line("SMOOTH-003: first half-step motion walk on a render-only invocation");
    }
}

// ---------------------------------------------------------------- install

static bool install() {
    if (!smooth_master()) { log_line("  SMOOTH-003: master switch SMOOTH=0 - off, nothing scanned or patched"); return false; }
    // Ch_UpdateAllActors: sets the motion-step flag (bit 24), then calls Ch_StepMotion
    uint8_t* sa = sig_find_unique("SMOOTH-003 logic step (Ch_UpdateAllActors)",
        "81 8E 94 01 00 00 00 00 00 01 8B 86 94 01 00 00 C1 E8 18 83 E0 01 50 56 E8 ?? ?? ?? ?? 83 C4 08 8D BE 30 03 00 00", 0x83325B);
    // Ch_UpdateAllActors: freeze check, then marks the skeleton dirty and adds its bone count
    uint8_t* sc = sig_find_unique("SMOOTH-003 skeleton dirty (Ch_UpdateAllActors)",
        "E8 ?? ?? ?? ?? 8B 5D FC 85 C0 75 74 8B 86 C0 01 00 00 C7 86 78 08 00 00 01 00 00 00 0F B7 40 0A", 0x83329D);
    // Ch_UpdateAllActors: dispatches a skeleton job batch once more than 199 bones are pending
    uint8_t* sb = sig_find_unique("SMOOTH-003 skeleton dispatch (Ch_UpdateAllActors)",
        "81 FB C8 00 00 00 7C 22 FF 75 F4 6A 00 FF 75 F8 E8 ?? ?? ?? ?? 83 C4 0C", 0x83331D);
    // Ch_StepMotion: calls Ch_SeqFrame, then halves the step if the half-rate switch is on
    uint8_t* sd = sig_find_unique("SMOOTH-003 SeqFrame call + half switch (Ch_StepMotion)",
        "56 66 89 BE 56 07 00 00 E8 ?? ?? ?? ?? 83 C4 04 83 3D ?? ?? ?? ?? 00 74 09 8B C7 99 2B C2 8B F8 D1 FF 83 7D 0C 00", 0x838DFD);
    // Sg_MainLoop: calls Ch_NoSkelPass when the actor system is up
    uint8_t* se = sig_find_unique("SMOOTH-003 Ch_NoSkelPass call (Sg_MainLoop)",
        "80 3D ?? ?? ?? ?? 00 74 0F E8 ?? ?? ?? ?? 6A 02 E8", 0x8210D1);
    // Ch_NoSkelPass: for a stepped actor, calls Ch_MotevDispatch and clears the pose-hold bit 0x1000
    uint8_t* sf = sig_find_unique("SMOOTH-003 Motev call (Ch_NoSkelPass)",
        "8B 86 94 01 00 00 A9 00 00 00 01 74 15 56 E8 ?? ?? ?? ?? 83 C4 04 81 A6 94 01 00 00 FF EF FF FF", 0x8334B7);
    if (!sa || !sb || !sc || !sd || !se || !sf) return false;

    uint8_t* site_step = sa + 0x18, *site_seq = sd + 8, *site_noskel = se + 9, *site_motev = sf + 0xE;
    uint8_t* step = call_target(site_step);
    uint8_t* seq = call_target(site_seq);
    uint8_t* noskel = call_target(site_noskel);
    uint8_t* motev = call_target(site_motev);
    uint8_t* disp = call_target(sb + 0x10);
    uint8_t* frz = call_target(sc);
    // Cross-checks at the resolved targets: Ch_StepMotion prologue; Ch_NoSkelPass loads the actor table, calls
    // the job sync, loads the count; job sync; skeleton dispatch (pushes its job fn); freeze getter.
    static const char P_STEP[] = "55 8B EC";
    static const char P_NOSKEL[] = "55 8B EC 83 EC 08 A1 ?? ?? ?? ?? 53 89 45 FC A1 ?? ?? ?? ?? 56 57 33 DB 33 FF 89 45 F8 E8 ?? ?? ?? ?? A1";
    static const char P_SYNC[] = "8B 0D ?? ?? ?? ?? 6A 00 E8 ?? ?? ?? ?? C3";
    static const char P_DISP[] = "55 8B EC 8B 0D ?? ?? ?? ?? 6A 00 FF 75 10 FF 75 0C FF 75 08 68";
    static const char P_FRZ[] = "0F BE 05 ?? ?? ?? ?? C3";
    uint8_t* sync = sig_match(noskel, P_NOSKEL) ? call_target(noskel + 0x1D) : nullptr;
    bool ok = sig_match(step, P_STEP) && seq && motev && sync && sig_match(sync, P_SYNC) &&
              sig_match(disp, P_DISP) && sig_match(frz, P_FRZ) &&
              sd > step && sd < step + 0x300 && sb > sa && sb < sa + 0x100 && sc > sa && sc < sa + 0x80 &&
              sf > noskel && sf < noskel + 0x200;
    if (!ok) {
        log_line("  SMOOTH-003: cross-check failed (step %p seq %p noskel %p sync %p motev %p disp %p freeze %p) - refused",
                 step, seq, noskel, sync, motev, disp, frz);
        return false;
    }
    // The job fn pushed by the dispatcher must be the one Ch_NoSkelPass's own dispatches use (same disp fn).
    g_table = (uint8_t* volatile*)read_abs(noskel + 7);
    g_count = (volatile int32_t*)read_abs(noskel + 0x23);
    uint8_t* st = sym_rt("actor_table");
    uint8_t* sn = sym_rt("actor_count");
    if ((st && st != (uint8_t*)g_table) || (sn && sn != (uint8_t*)g_count)) {
        log_line("  SMOOTH-003: actor table differs from the scan symbols - refused");
        return false;
    }
    g_half = (volatile int32_t*)read_abs(sd + 0x12);
    g_freeze = (volatile int8_t*)read_abs(frz + 3);
    if (*g_half != 0) log_line("  SMOOTH-003: note: half-rate switch was %d at install (debug menu?)", *g_half);
    g_step = (step_t)step;
    g_seq = (actor_fn_t)seq;
    g_motev = (actor_fn_t)motev;
    g_noskel = (void_fn_t)noskel;
    g_sync = (void_fn_t)sync;
    g_dispatch = (dispatch_t)disp;
    g_gates = config_row("SMOOTH-003-GATES", 1);
    g_hold = config_row("SMOOTH-003-HOLD", 1);
    g_blend = config_row("SMOOTH-003-BLEND", 0);
    g_cloth = config_row("SMOOTH-003-CLOTH", 0);
    g_clothhold = config_row("SMOOTH-003-CLOTHHOLD", 4);
    g_extrap_pct = config_row("SMOOTH-003-CLOTHEXTRAP-PCT", 50);
    if (g_clothhold) {
        // entry through MOV EAX,[EDI+0x194]; TEST EAX,0x200000 (cloth actor bit)
        g_cloth_fn = sig_find_unique("SMOOTH-003-CLOTHHOLD Ch_ClothSimulate",
            "55 8B EC 81 EC AC 02 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 57 8B 7D 08 89 BD C8 FD FF FF 8B 87 94 01 00 00 A9 00 00 20 00",
            0x822DE0);
        if (!g_cloth_fn) { g_clothhold = 0; log_line("  SMOOTH-003-CLOTHHOLD: Ch_ClothSimulate not found - off"); }
    }
    if (g_clothhold >= 2) {
        // FUN_0082e390: loop over count actors: CALL attach; CALL Ch_ClothSimulate; ADD ESP,0xC
        g_cjob_fn = sig_find_unique("SMOOTH-003-CLOTHHOLD cloth job (FUN_0082e390)",
            "55 8B EC 53 8B 5D 0C 56 33 F6 39 33 7E ?? 57 90 8B 45 08 6A 00 8B 3C B0 57 E8 ?? ?? ?? ?? 57 E8 ?? ?? ?? ?? 83 C4 0C",
            0x82E390);
        if (!g_cjob_fn || call_target(g_cjob_fn + 0x1F) != g_cloth_fn) {
            g_clothhold = 0;
            log_line("  SMOOTH-003-CLOTHHOLD: cloth job not found or does not call Ch_ClothSimulate - off");
        }
    }
    if (g_clothhold >= 2) {
        // job +0x27: CMP dword [gate],0 / JNZ +9 / PUSH EDI / CALL deform FUN_0082dbc0
        const uint8_t* j = g_cjob_fn;
        if (j[0x27] == 0x83 && j[0x28] == 0x3D && j[0x2D] == 0x00 && j[0x2E] == 0x75 && j[0x30] == 0x57 && j[0x31] == 0xE8) {
            g_deform_gate = *(volatile int32_t**)(j + 0x29);
            g_deform = (void (__cdecl*)(uint8_t*))call_target(j + 0x31);
            log_line("  SMOOTH-003-CLOTHHOLD: mode %d, deform gva 0x%08X, lead %d%% of a tick (mode 3)", g_clothhold, rt_to_gva_exe((uint8_t*)g_deform), g_extrap_pct);
        } else {
            if (g_clothhold > 2) g_clothhold = 2;
            log_line("  SMOOTH-003-CLOTHHOLD: deform call not at job +0x31 - modes 3/4 unavailable");
        }
    }
    g_diag = config_row("SMOOTH-003-DIAG", 0);
    g_diag_id = config_row("SMOOTH-003-DIAG-ID", 0);
    g_diag_id_end = config_row("SMOOTH-003-DIAG-ID-END", 0);
    g_loopguard = config_row("SMOOTH-003-LOOPGUARD", 1);
    g_shortloop = config_row("SMOOTH-003-SHORTLOOP", 1);
    g_loopend = config_row("SMOOTH-003-LOOPEND", 1);
    g_sanity = config_row("SMOOTH-003-SANITY", 1);
    g_keyhold = config_row("SMOOTH-003-KEYHOLD", 1);
    g_stock_id = config_row("SMOOTH-003-STOCK-ID", 0);
    g_stock_id_end = config_row("SMOOTH-003-STOCK-ID-END", 0);
    g_endextrap = config_row("SMOOTH-003-ENDEXTRAP", 1);
    if (g_sanity || g_endextrap) {
        g_prev_ang = (float*)calloc((size_t)MAXA * MAXB * 3, sizeof(float));
        g_cur_ang = (float*)calloc((size_t)MAXA * MAXB * 3, sizeof(float));
        if (!g_prev_ang || !g_cur_ang) { g_sanity = 0; g_endextrap = 0; log_line("  SMOOTH-003: SANITY/ENDEXTRAP off (out of memory)"); }
    }
    g_hold_buf = (uint8_t*)calloc(MAXA, HOLD_SZ);
    if (!g_hold_buf) { log_line("  SMOOTH-003: out of memory - refused"); return false; }

    uint8_t* sites[4] = { site_step, site_noskel, site_seq, site_motev };
    void* olds[4] = { step, noskel, seq, motev };
    void* news[4] = { (void*)&h_step, (void*)&h_noskel, (void*)&h_seq, (void*)&h_motev };
    int count = g_gates ? 4 : 2;
    for (int i = 0; i < count; i++) {
        if (!patch_call_rel32(sites[i], olds[i], news[i])) {
            for (int j = 0; j < i; j++) patch_call_rel32(sites[j], news[j], olds[j]);
            log_line("  SMOOTH-003: patch %d failed - refused", i);
            return false;
        }
    }
    log_line("  SMOOTH-003: Ch_StepMotion gva=0x%08X (0x00838C90) call 0x%08X (0x00833273); Ch_NoSkelPass gva=0x%08X (0x00833390) "
             "call 0x%08X (0x008210DA); SeqFrame gva=0x%08X (0x008377C0) call 0x%08X (0x00838E05); Motev gva=0x%08X (0x00834350) "
             "call 0x%08X (0x008334C5); sync 0x%08X (0x00A44B80) dispatch 0x%08X (0x00A44B90); half gva=0x%08X (0x013017F8) "
             "freeze gva=0x%08X (0x012FBBBB); gates=%d hold=%d blend=%d cloth=%d diag=%d loopguard=%d",
             rt_to_gva_exe(step), rt_to_gva_exe(site_step), rt_to_gva_exe(noskel), rt_to_gva_exe(site_noskel),
             rt_to_gva_exe(seq), rt_to_gva_exe(site_seq), rt_to_gva_exe(motev), rt_to_gva_exe(site_motev),
             rt_to_gva_exe(sync), rt_to_gva_exe(disp), rt_to_gva_exe((const void*)g_half), rt_to_gva_exe((const void*)g_freeze),
             g_gates, g_hold, g_blend, g_cloth, g_diag, g_loopguard);
    return true;
}

Row row_SMOOTH003 = {
    "SMOOTH-003",
    "60 Hz limbs: engine half-rate motion + a half step on render-only frames (needs master SMOOTH=1)",
    1, install, pre, post, 0
};
