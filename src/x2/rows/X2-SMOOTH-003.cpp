// SPDX-License-Identifier: Apache-2.0
// X2-SMOOTH-003 (port of FFX SMOOTH-003, minimal first build): with X2-SMOOTH-002 the root moves at 60 Hz but the
// skeleton pose still changes at 30 Hz. This row turns on the engine's half-rate motion switch and takes a second half
// step of each actor's motion on render-only invocations, so limbs animate at 60 Hz at stock speed (needs X2-SMOOTH=1;
// F7 toggles). Sub-switches as FFX: GATES, HOLD, LOOPGUARD, LOOPEND, SHORTLOOP (default 1), CLOTHHOLD (0 or 2, default 2).
// Not ported in this build: KEYHOLD, SANITY, ENDEXTRAP, BLEND, CLOTH, CLOTHHOLD modes 1/3/4, DIAG*/STOCK-ID/TRACE.
// FFX-2 changes (PORT-SMOOTH3.md §1-§7): step CALL pushes EDI (actor), the tween loop after it is inlined (cross-check
// windows sa+0x280); the walk writes no skeleton dirty flag (FFX-2 has none) and dispatches at >= 150 bones (FFX > 199);
// Ch_NoSkelPass call site has a new shape (E8 at +0x13); actor stride 0x910 with per-block field shifts (low block +0xC,
// motion block +0x10, 0x7F0+ block +0x14); hold regions re-mapped, the look-at block held whole (+0x418..+0x43B, H).
// All CALL retargets and the cloth job entry hook are written on the game thread at the first pre().
// Evidence: outer repo G:\Work\ffx2-fps60 re/ledger/PORT-SMOOTH3.md (S unless tagged H), sigs tools/sigs_x2_smooth3.txt.
#include "x2_smooth.h"
#include <string.h>
#include <stdlib.h>

namespace x2 {

extern Row row_SMOOTH003;

typedef void(__cdecl* step_t)(uint8_t* actor, int full);
typedef void(__cdecl* actor_fn_t)(uint8_t* actor);
typedef void(__cdecl* void_fn_t)(void);
typedef void(__cdecl* dispatch_t)(uint8_t* start, int mode, int span);
typedef void(__cdecl* cjob_t)(uint8_t** list, int* count);

static const int      MAXA = 512;
// Actor fields (PORT-SMOOTH3.md §5, all S).
static const uint32_t STRIDE   = 0x910;   // FFX 0x880
static const uint32_t ACTIVE   = 0x2;     // unchanged
static const uint32_t RANGE_A  = 0x18F;   // FFX 0x180: range-skip byte
static const uint32_t RANGE_B  = 0x190;   // FFX 0x181
static const uint32_t FLAGS    = 0x1A0;   // FFX 0x194 (bits 0x40, 0x1000000 unchanged)
static const uint32_t PARENT   = 0x1A8;   // FFX 0x19C
static const uint32_t HDR      = 0x1CC;   // FFX 0x1C0 (bone count u16 at +0xA)
static const uint32_t HDR_BONES = 0xA;    // unchanged
// Keyframe clock fields (set by the motion start, wrapped by Ch_MotionLoopWrap, read by the sampler).
static const uint32_t M_SEQ    = 0x724;   // FFX 0x714
static const uint32_t M_START  = 0x728;   // FFX 0x718
static const uint32_t M_END    = 0x72C;   // FFX 0x71C
static const uint32_t M_LEN    = 0x730;   // FFX 0x720
static const uint32_t M_LOOPS  = 0x734;   // FFX 0x724 (s16)
static const uint32_t M_CLOCK  = 0x750;   // FFX 0x740
static const int      BATCH_BONES = 0x96; // FFX-2 dispatches at >= 150 bones (FFX > 199)
static const uint32_t F_SCRIPT = 0x40, F_STEP = 0x1000000;

static step_t     g_step = nullptr;
static actor_fn_t g_seq = nullptr, g_motev = nullptr;
static void_fn_t  g_noskel = nullptr, g_sync = nullptr;
static dispatch_t g_dispatch = nullptr;
static volatile int32_t*  g_half = nullptr;     // engine half-rate motion switch 0x0116E3D4
static volatile int8_t*   g_freeze = nullptr;   // actor freeze flag 0x00DF67F3
static uint8_t* volatile* g_table = nullptr;    // pointer to the actor array 0x01CC9604
static volatile int32_t*  g_count = nullptr;    // actor count 0x01CC9600

static uint8_t* g_site_step = nullptr, *g_site_noskel = nullptr, *g_site_seq = nullptr, *g_site_motev = nullptr;
static bool g_patched = false;

static int  g_gates = 1, g_hold = 1, g_loopguard = 1, g_shortloop = 1, g_loopend = 1;
static int  g_kind = INV_STOCK;
static bool g_half_on = false;                   // we wrote 1 into the switch
static volatile bool g_in_step = false;          // inside one of OUR Ch_StepMotion calls
static volatile bool g_walked_inv = false;

static uint8_t* g_debt_base = nullptr;
static uint8_t  g_debt[MAXA];
static uint16_t g_debt_id[MAXA];
static volatile uint8_t g_walked[MAXA];

// Hold buffer: per slot, common regions (all walked / range actors) then the face regions (parents).
struct Region { uint32_t off, len; };
static constexpr Region COMMON[] = {
    { 0x15C, 4 },      // tilt ease, unchanged (S)
    { 0x180, 8 },      // new store in FUN_006e3970 (H)
    { 0x520, 0x1C4 },  // reference points, FFX 0x510 (start S, end +0x6E3 H)
};
static constexpr Region FACE[] = {
    { 0x418, 0x24 },   // look-at block re-laid out, held whole +0x418..+0x43B (H; FFX {0x418,8})
    { 0x4CC, 4 },      // FFX 0x4BC (H)
    { 0x4E0, 4 },      // FFX 0x4D0 (S)
    { 0x4EC, 8 },      // mouth/blink, FFX 0x4DC (S)
};
template <size_t N> static constexpr uint32_t region_len(const Region (&r)[N]) {
    uint32_t s = 0;
    for (size_t i = 0; i < N; i++) s += r[i].len;
    return s;
}
static constexpr uint32_t HOLD_SZ = region_len(COMMON) + region_len(FACE);
static_assert(HOLD_SZ == 0x204, "hold layout");
static uint8_t* g_hold_buf = nullptr;            // MAXA * HOLD_SZ
static uint8_t  g_held[MAXA];                    // 0 = none, 1 = common, 2 = common + face
static uint8_t* g_hold_base = nullptr;
static int      g_hold_n = 0;

static LONG g_loopend_holds = 0, g_stock_steps = 0, g_guard_wrap = 0, g_guard_clamp = 0, g_guard_hold = 0;
static LONG g_walks = 0, g_stepped = 0, g_pays = 0, g_batches = 0, g_gated_seq = 0, g_gated_motev = 0;

// CLOTHHOLD (mode 2 only): the cloth job Ch_ClothJob 0x6DB500 (cdecl(actor**, int* count), worker threads) is skipped
// for walked actors on render-only invocations, so hair and cloth step once per logic tick.
static int      g_clothhold = 2;
static uint8_t* g_cjob_fn = nullptr;
static cjob_t   g_cjob_tramp = nullptr;
static volatile LONG g_cloth_held = 0;

static void fault(const char* where, DWORD code) {
    log_line("X2-SMOOTH-003: fault 0x%08lX in %s - row off", code, where);
    row_SMOOTH003.on = 0;
}

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
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; fault("Ch_StepMotion", GetExceptionCode()); }
    g_in_step = false;
    return ok;
}

static bool safe_dispatch(uint8_t* start, int span) {
    __try { g_dispatch(start, 0, span); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("skeleton dispatch", GetExceptionCode()); return false; }
    InterlockedIncrement(&g_batches);
    return true;
}

static bool safe_sync() {
    __try { g_sync(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("job sync", GetExceptionCode()); return false; }
    return true;
}

// SHORTLOOP: a looping motion of at most one frame (e.g. an enemy hold pose) is only ever sampled at whole frames at
// stock; a half-frame sample twists the body, so such actors run exactly as stock.
static bool short_loop(const uint8_t* a) {
    uint32_t fl = *(const uint32_t*)(a + FLAGS);
    if ((fl & F_SCRIPT) != 0 || *(const uint32_t*)(a + M_SEQ) == 0) return false;
    return *(const int16_t*)(a + M_LOOPS) != 1 && *(const int32_t*)(a + M_LEN) <= 0x100;
}

// Logic step stub (E8 0x6E1394 -> Ch_StepMotion 0x6E78F0, cdecl(actor, full)).
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
    if (*g_half != 0 && g_shortloop && short_loop(a)) {
        *g_half = 0;                                    // whole stock step: no half step, no debt
        g_step(a, full);
        *g_half = 1;
        InterlockedIncrement(&g_stock_steps);
        return;
    }
    g_step(a, full);
    if (i >= 0 && *g_half != 0) { g_debt[i] = 1; g_debt_id[i] = *(uint16_t*)a; }
}

// ---------------------------------------------------------------- gates (X2-SMOOTH-003-GATES)
// Ch_SeqFrame is a per-call script VM and Ch_MotevDispatch fires motion events: neither may run twice per tick.
static void __cdecl h_seq(uint8_t* a) {
    if (g_in_step && g_gates && !g_disabled) { InterlockedIncrement(&g_gated_seq); return; }
    g_seq(a);
}

static void __cdecl h_motev(uint8_t* a) {
    if (g_walked_inv && g_gates && !g_disabled && row_SMOOTH003.on) {
        int i = slot_of(a, *g_table, *g_count);
        if (i >= 0 && g_walked[i]) { InterlockedIncrement(&g_gated_motev); return; }
    }
    g_motev(a);
}

// ---------------------------------------------------------------- loop guard (X2-SMOOTH-003-LOOPGUARD)
// Returns the clock the stock logic step would sample at for clock c (the loop-wrap arithmetic of Ch_MotionLoopWrap
// 0x6E8310, read-only; no loop count is decremented, the new FFX-2 wrap counter +0x803 is not touched). kind:
// 0 = unchanged, 1 = wrapped, 2 = clamped (play once), 3 = no stock equivalent (hold).
static int32_t guard_clock(const uint8_t* a, int32_t c, int* kind) {
    *kind = 0;
    int32_t end = *(const int32_t*)(a + M_END);
    int32_t len = *(const int32_t*)(a + M_LEN);
    int16_t loops = *(const int16_t*)(a + M_LOOPS);
    if (c < 0) { *kind = 1; return 0; }
    if (loops == 1) {
        int32_t last = end & ~0xFF;                     // play once: stock never samples past the last whole frame
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

// The render-only full step of one actor (bit 24 already set). Returns false on a fault (the walk aborts).
static bool walk_step(uint8_t* a, uint32_t fl) {
    int32_t c0 = *(int32_t*)(a + M_CLOCK);
    int kind = 0;
    int32_t cs = c0;
    bool keyed = (fl & F_SCRIPT) == 0 && *(uint32_t*)(a + M_SEQ) != 0;   // the script paths wrap on every call
    if (g_loopguard && keyed) cs = guard_clock(a, c0, &kind);
    // LOOPEND: a half-frame sample inside the last frame of a loop reads past the loop end (bad pose); stock samples
    // there only at whole frames, so hold the pose for that one frame.
    if (g_loopend && kind <= 1 && keyed && (cs & 0xFF) != 0 && *(int16_t*)(a + M_LOOPS) != 1 &&
        *(int32_t*)(a + M_LEN) > 0 && (cs >> 8) >= (*(int32_t*)(a + M_END) >> 8)) {
        if (!safe_step(a, 0)) return false;               // clock only: pose stays at tick k
        InterlockedIncrement(&g_loopend_holds);
        return true;
    }
    if (kind == 3) {
        if (!safe_step(a, 0)) return false;               // clock only: pose stays at tick k
        InterlockedIncrement(&g_guard_hold);
        return true;
    }
    if (kind) *(int32_t*)(a + M_CLOCK) = cs;
    bool ok = safe_step(a, 1);
    if (kind) {
        // Unwrapped clock + the half step just added: the next logic tick wraps / counts down exactly as stock.
        *(int32_t*)(a + M_CLOCK) = c0 + (*(int32_t*)(a + M_CLOCK) - cs);
        InterlockedIncrement(kind == 2 ? &g_guard_clamp : &g_guard_wrap);
    }
    return ok;
}

// ---------------------------------------------------------------- render-only walk
// Mirrors the logic walk of Ch_UpdateAllActors 0x6E10A0 for actors that owe a half step, then dispatches skeleton
// jobs. FFX-2: no dirty-flag store, batch at >= 150 bones, next batch starts at a + 0x910.
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
            if (a[ACTIVE] == 0 || (a[RANGE_A] != 0 && a[RANGE_B] == 0)) continue;
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
        if (a[ACTIVE] != 0 && (a[RANGE_A] == 0 || a[RANGE_B] != 0) && g_debt[i]) {
            g_debt[i] = 0;
            uint32_t fl = *(uint32_t*)(a + FLAGS);
            if (g_debt_id[i] != *(uint16_t*)a) goto next;                     // slot reused: drop the stale debt
            if (g_shortloop && short_loop(a)) { g_debt[i] = 1; goto next; }  // stock from the next logic step
            *(uint32_t*)(a + FLAGS) = fl | F_STEP;
            if (!walk_step(a, fl)) return;
            g_walked[i] = 1;
            g_walked_inv = true;
            InterlockedIncrement(&g_stepped);
            if (*(uint32_t*)(a + PARENT) == 0) {
                uint8_t* hdr = *(uint8_t**)(a + HDR);
                if (hdr) bones += *(uint16_t*)(hdr + HDR_BONES);
            }
        }
    next:
        if (bones >= BATCH_BONES) {
            if (!safe_dispatch(cursor, span)) return;
            cursor = a + STRIDE;
            span = 0;
            bones = 0;
        }
    }
    if (bones != 0) safe_dispatch(cursor, span);
}

// Sg_MainLoop stub (E8 0x60586C -> Ch_NoSkelPass 0x6E1610, cdecl(void)).
static void __cdecl h_noskel() {
    if (row_SMOOTH003.on && !g_disabled && g_kind == INV_BUILD && smooth_live(3) && *g_freeze == 0) {
        g_walks++;
        __try { walk(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { fault("walk", GetExceptionCode()); }
    }
    g_noskel();
}

// ---------------------------------------------------------------- CLOTHHOLD mode 2 (worker threads)
static void __cdecl h_cjob(uint8_t** list, int* count) {
    if (g_clothhold == 2 && g_walked_inv && !g_disabled && row_SMOOTH003.on) {
        bool all = true;
        __try {
            for (int k = 0; k < *count && all; k++) {
                int i = slot_of(list[k], *g_table, *g_count);
                all = i >= 0 && g_walked[i];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { all = false; }
        if (all) {
            if (InterlockedIncrement(&g_cloth_held) == 1)
                log_line("X2-SMOOTH-003-CLOTHHOLD: first cloth job skipped on a render-only invocation (mode 2)");
            return;
        }
    }
    g_cjob_tramp(list, count);
}

// ---------------------------------------------------------------- per invocation
// Game thread, between invocations: the CALL retargets and the cloth entry hook.
static bool apply_patches() {
    uint8_t* sites[4] = { g_site_step, g_site_noskel, g_site_seq, g_site_motev };
    void* olds[4] = { (void*)g_step, (void*)g_noskel, (void*)g_seq, (void*)g_motev };
    void* news[4] = { (void*)&h_step, (void*)&h_noskel, (void*)&h_seq, (void*)&h_motev };
    int count = g_gates ? 4 : 2;
    for (int i = 0; i < count; i++) {
        if (!patch_call_rel32(sites[i], olds[i], news[i])) {
            for (int j = 0; j < i; j++) patch_call_rel32(sites[j], news[j], olds[j]);
            log_line("X2-SMOOTH-003: CALL patch %d failed - row off", i);
            return false;
        }
    }
    if (g_clothhold == 2) {
        if (hook_entry("X2-SMOOTH-003-CLOTHHOLD", g_cjob_fn, (void*)&h_cjob, (void**)&g_cjob_tramp))
            log_line("X2-SMOOTH-003-CLOTHHOLD: Ch_ClothJob gva=0x%08X hooked: whole job once per logic tick (mode 2)", rt_to_gva(g_cjob_fn));
        else { g_clothhold = 0; log_line("X2-SMOOTH-003-CLOTHHOLD: hook failed - off"); }
    }
    log_line("X2-SMOOTH-003: %d CALLs retargeted on the game thread", count);
    return true;
}

static void pre() {
    if (!g_patched) {
        g_patched = true;
        if (!apply_patches()) { row_SMOOTH003.on = 0; return; }
    }
    smooth_poll_hotkey();
    if (g_hold_base) {
        __try { hold_restore(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_hold_base = nullptr; fault("hold restore", GetExceptionCode()); }
    }
    g_kind = g_disabled ? INV_STOCK : pacing_kind();
    bool want = !g_disabled && row_SMOOTH003.on && smooth_live(3) && (g_kind == INV_LOGIC || g_kind == INV_BUILD);
    *g_half = want ? 1 : 0;
    g_half_on = want;
    g_walked_inv = false;
    memset((void*)g_walked, 0, sizeof(g_walked));
}

static void post() {
    static LONG last = 0;
    if (g_walks - last >= 1800) {
        last = g_walks;
        log_line("X2-SMOOTH-003: walks=%ld stepped=%ld batches=%ld pays=%ld gated(seq=%ld motev=%ld) "
                 "loopguard(wrap=%ld clamp=%ld hold=%ld) loopend_holds=%ld stock_steps=%ld cloth_held=%ld half=%d live=%d",
                 g_walks, g_stepped, g_batches, g_pays, g_gated_seq, g_gated_motev, g_guard_wrap, g_guard_clamp,
                 g_guard_hold, g_loopend_holds, g_stock_steps, (LONG)g_cloth_held, *g_half, smooth_live(3));
    }
    static LONG reported_s = 0;
    if (!reported_s && g_stock_steps > 0) {
        reported_s = 1;
        log_line("X2-SMOOTH-003: first stock logic step for a short-loop actor (no half step)");
    }
    static LONG reported = 0;
    if (!reported && g_stepped > 0) {
        reported = 1;
        log_line("X2-SMOOTH-003: first half-step motion walk on a render-only invocation");
    }
}

// ---------------------------------------------------------------- install (resolve + check only)
static bool install() {
    if (!smooth_master()) { log_line("  X2-SMOOTH-003: master X2-SMOOTH=0 - off"); return false; }
    // Ch_UpdateAllActors: sets the motion-step flag (bit 24), PUSH full; PUSH EDI (actor); CALL Ch_StepMotion (+0x18)
    uint8_t* sa = sig_find("X2-SMOOTH-003 logic step (Ch_UpdateAllActors)",
        "81 8E 5C FE FF FF 00 00 00 01 8B 86 5C FE FF FF C1 E8 18 83 E0 01 50 57 E8 ?? ?? ?? ?? D9 46 F8", 0x6E137C);
    // Ch_UpdateAllActors: parent test, CALL Ch_IsFrozen (+0x09), bone count of +0x1CC (no dirty store in FFX-2)
    uint8_t* sc = sig_find("X2-SMOOTH-003 skeleton bones + freeze (Ch_UpdateAllActors)",
        "83 BE 64 FE FF FF 00 75 15 E8 ?? ?? ?? ?? 85 C0 75 0C 8B 86 88 FE FF FF 0F B7 40 0A 03 D8", 0x6E156D);
    // Ch_UpdateAllActors: dispatches a skeleton job batch at >= 150 bones (E8 at +0x11)
    uint8_t* sb = sig_find("X2-SMOOTH-003 skeleton dispatch (Ch_UpdateAllActors)",
        "81 FB 96 00 00 00 7C 20 8B 45 F8 50 6A 00 FF 75 FC E8 ?? ?? ?? ?? 8D 96 CC 05 00 00", 0x6E158B);
    // Ch_StepMotion: CALL Ch_SeqFrame (+0x08), then halves the step if the half-rate switch (+0x12) is on
    uint8_t* sd = sig_find("X2-SMOOTH-003 SeqFrame call + half switch (Ch_StepMotion)",
        "56 66 89 BE 66 07 00 00 E8 ?? ?? ?? ?? 83 C4 04 83 3D ?? ?? ?? ?? 00 74 09 8B C7 99 2B C2 8B F8 D1 FF 83 7D 0C 00", 0x6E7A5D);
    // Sg_MainLoop: CMP gate; MOV [timing],0 (new in FFX-2); JZ; CALL Ch_NoSkelPass (+0x13)
    uint8_t* se = sig_find("X2-SMOOTH-003 Ch_NoSkelPass call (Sg_MainLoop)",
        "80 3D ?? ?? ?? ?? 00 C7 05 ?? ?? ?? ?? 00 00 00 00 74 20 E8 ?? ?? ?? ?? A1", 0x605859);
    // Ch_NoSkelPass: for a stepped actor, CALL Ch_MotevDispatch (+0x0E) and clear the pose-hold bit 0x1000
    uint8_t* sf = sig_find("X2-SMOOTH-003 Motev call (Ch_NoSkelPass)",
        "8B 86 A0 01 00 00 A9 00 00 00 01 74 15 56 E8 ?? ?? ?? ?? 83 C4 04 81 A6 A0 01 00 00 FF EF FF FF", 0x6E1787);
    // Ch_DecodeKeys: MOV EBX,[EDI+M_CLOCK]; MOV EAX,[EDI+M_END] - checks the motion-block shift (+0x10)
    uint8_t* sk = sig_find("X2-SMOOTH-003 sample clock (Ch_DecodeKeys)",
        "8B 9F 50 07 00 00 8B 87 2C 07 00 00 ?? 8D B3 FF 01 00 00", 0x6E841C);
    if (!sa || !sb || !sc || !sd || !se || !sf || !sk) return false;

    g_site_step = sa + 0x18; g_site_seq = sd + 0x08; g_site_noskel = se + 0x13; g_site_motev = sf + 0x0E;
    uint8_t* step = call_target(g_site_step);
    uint8_t* seq = call_target(g_site_seq);
    uint8_t* noskel = call_target(g_site_noskel);
    uint8_t* motev = call_target(g_site_motev);
    uint8_t* disp = call_target(sb + 0x11);
    uint8_t* frz = call_target(sc + 0x09);
    // Cross-checks at the resolved targets: Ch_StepMotion prologue; Ch_NoSkelPass loads the actor table, calls the job
    // sync, loads the count; job sync; skeleton dispatch (pushes its job fn); freeze getter. Windows widened for the
    // inlined tween loop (sb - sa = 0x20F, sc - sa = 0x1F1).
    static const char P_STEP[] = "55 8B EC";
    static const char P_NOSKEL[] = "55 8B EC 83 EC 08 A1 ?? ?? ?? ?? 53 89 45 FC A1 ?? ?? ?? ?? 56 57 33 DB 33 FF 89 45 F8 E8 ?? ?? ?? ?? A1";
    static const char P_SYNC[] = "8B 0D ?? ?? ?? ?? 6A 00 E8 ?? ?? ?? ?? C3";
    static const char P_DISP[] = "55 8B EC 8B 0D ?? ?? ?? ?? 6A 00 FF 75 10 FF 75 0C FF 75 08 68";
    static const char P_FRZ[] = "0F BE 05 ?? ?? ?? ?? C3";
    uint8_t* sync = (noskel && sig_match(noskel, P_NOSKEL)) ? call_target(noskel + 0x1D) : nullptr;
    bool ok = step && sig_match(step, P_STEP) && seq && motev && sync && sig_match(sync, P_SYNC) &&
              disp && sig_match(disp, P_DISP) && frz && sig_match(frz, P_FRZ) &&
              sd > step && sd < step + 0x300 && sb > sa && sb < sa + 0x280 && sc > sa && sc < sa + 0x280 && sc < sb &&
              sf > noskel && sf < noskel + 0x200 &&
              *(const uint32_t*)(sk + 2) == M_CLOCK && *(const uint32_t*)(sk + 8) == M_END;
    if (!ok) {
        log_line("  X2-SMOOTH-003: cross-check failed (step %p seq %p noskel %p sync %p motev %p disp %p freeze %p) - refused",
                 step, seq, noskel, sync, motev, disp, frz);
        return false;
    }
    g_table = (uint8_t* volatile*)abs_operand("actor table", noskel + 0x07);
    g_count = (volatile int32_t*)abs_operand("actor count", noskel + 0x23);
    g_half = (volatile int32_t*)abs_operand("half-rate switch", sd + 0x12);
    g_freeze = (volatile int8_t*)abs_operand("freeze flag", frz + 0x03);
    if (!g_table || !g_count || !g_half || !g_freeze) { log_line("  X2-SMOOTH-003: global operand outside FFX-2.exe - refused"); return false; }
    if (*g_half != 0) log_line("  X2-SMOOTH-003: note: half-rate switch was %d at install (debug menu?)", *g_half);
    g_step = (step_t)step;
    g_seq = (actor_fn_t)seq;
    g_motev = (actor_fn_t)motev;
    g_noskel = (void_fn_t)noskel;
    g_sync = (void_fn_t)sync;
    g_dispatch = (dispatch_t)disp;
    g_gates = config_row("X2-SMOOTH-003-GATES", 1);
    g_hold = config_row("X2-SMOOTH-003-HOLD", 1);
    g_loopguard = config_row("X2-SMOOTH-003-LOOPGUARD", 1);
    g_loopend = config_row("X2-SMOOTH-003-LOOPEND", 1);
    g_shortloop = config_row("X2-SMOOTH-003-SHORTLOOP", 1);
    g_clothhold = config_row("X2-SMOOTH-003-CLOTHHOLD", 2);
    if (g_clothhold != 0 && g_clothhold != 2) {
        log_line("  X2-SMOOTH-003-CLOTHHOLD: mode %d not ported in this build - using mode 2", g_clothhold);
        g_clothhold = 2;
    }
    if (g_clothhold) {
        // entry through MOV ESI,[EBP+8]; MOV EAX,[ESI+0x1A0]; TEST EAX,0x200000 (cloth actor bit)
        uint8_t* cloth = sig_find("X2-SMOOTH-003-CLOTHHOLD Ch_ClothSimulate",
            "55 8B EC 81 EC B8 02 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 56 8B 75 08 89 B5 78 FD FF FF 8B 86 A0 01 00 00 A9 00 00 20 00",
            0x6F4C90);
        // Ch_ClothJob: loop over count actors: CALL attach (+0x19); CALL Ch_ClothSimulate (+0x1F); deform gate; deform
        g_cjob_fn = sig_find("X2-SMOOTH-003-CLOTHHOLD cloth job (Ch_ClothJob)",
            "55 8B EC 53 8B 5D 0C 56 33 F6 39 33 7E ?? 57 90 8B 45 08 6A 00 8B 3C B0 57 E8 ?? ?? ?? ?? 57 E8 ?? ?? ?? ?? 83 C4 0C "
            "83 3D ?? ?? ?? ?? 00 75 09 57 E8", 0x6DB500);
        if (!cloth || !g_cjob_fn || call_target(g_cjob_fn + 0x1F) != cloth) {
            g_clothhold = 0;
            g_cjob_fn = nullptr;
            log_line("  X2-SMOOTH-003-CLOTHHOLD: cloth job not found or does not call Ch_ClothSimulate - off");
        }
    }
    g_hold_buf = (uint8_t*)calloc(MAXA, HOLD_SZ);
    if (!g_hold_buf) { log_line("  X2-SMOOTH-003: out of memory - refused"); return false; }

    log_line("  X2-SMOOTH-003: Ch_StepMotion gva=0x%08X (0x006E78F0) call 0x%08X (0x006E1394); Ch_NoSkelPass gva=0x%08X (0x006E1610) "
             "call 0x%08X (0x0060586C); SeqFrame gva=0x%08X (0x006D2720) call 0x%08X (0x006E7A65); Motev gva=0x%08X (0x006E27F0) "
             "call 0x%08X (0x006E1795); sync 0x%08X (0x00B520D0) dispatch 0x%08X (0x00B520E0); half gva=0x%08X (0x0116E3D4) "
             "freeze gva=0x%08X (0x00DF67F3); actors 0x%08X (0x01CC9604) count 0x%08X (0x01CC9600) stride 0x910; "
             "gates=%d hold=%d loopguard=%d loopend=%d shortloop=%d clothhold=%d; patches at the first pre(); F7 toggles",
             rt_to_gva(step), rt_to_gva(g_site_step), rt_to_gva(noskel), rt_to_gva(g_site_noskel),
             rt_to_gva(seq), rt_to_gva(g_site_seq), rt_to_gva(motev), rt_to_gva(g_site_motev),
             rt_to_gva(sync), rt_to_gva(disp), rt_to_gva((const void*)g_half), rt_to_gva((const void*)g_freeze),
             rt_to_gva((const void*)g_table), rt_to_gva((const void*)g_count),
             g_gates, g_hold, g_loopguard, g_loopend, g_shortloop, g_clothhold);
    return true;
}

Row row_SMOOTH003 = {
    "X2-SMOOTH-003",
    "60 Hz limbs: engine half-rate motion + a half step on render-only frames (needs X2-SMOOTH=1)",
    1, install, pre, post, 0
};

} // namespace x2
