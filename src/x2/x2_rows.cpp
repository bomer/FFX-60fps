// SPDX-License-Identifier: Apache-2.0
// Row registry and rollback engine for FFX-2 (same model as the FFX mod's rows.cpp / rollback.cpp, own state).
// A row = one ledger row's mitigation, toggled by its ID in [rows] of fps60x2.ini; install() runs once on the init
// thread after the signature scan, pre()/post() around every Sg_MainLoop invocation while the row is on.
// Rollback: spans registered by rows are copied out before each render-only invocation and back after it.
#include "x2.h"
#include <string.h>

namespace x2 {

// ---------------------------------------------------------------- registry
extern Row row_MAG008, row_MAG009, row_LOOP005, row_LOOP004, row_UI001, row_UI017, row_W2A002, row_CLK002;

static Row* g_rows[] = {
    &row_MAG008,                                 // rows/X2-MAG-008.cpp
    &row_MAG009,                                 // rows/X2-MAG-009.cpp
    &row_LOOP005,                                // rows/X2-LOOP-005.cpp
    &row_LOOP004,                                // rows/X2-LOOP-004.cpp
    &row_UI001,                                  // rows/X2-UI-001.cpp
    &row_UI017,                                  // rows/X2-UI-017.cpp
    &row_W2A002,                                 // rows/X2-W2A-002.cpp
    &row_CLK002,                                 // rows/X2-CLK-002.cpp
    nullptr
};

void rows_install_all() {
    int n = 0, on = 0;
    for (Row** p = g_rows; *p; p++) {
        Row& r = **p;
        n++;
        r.on = 0;
        if (!config_row(r.id, r.default_on)) {
            log_line("row %-12s OFF (ini) - %s", r.id, r.summary);
            continue;
        }
        bool ok = false;
        __try { ok = r.install ? r.install() : true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        r.on = ok ? 1 : 0;
        if (ok) on++;
        log_line("row %-12s %s - %s", r.id, ok ? "ON" : "FAILED to install, stays off", r.summary);
    }
    log_line("rows: %d registered, %d on", n, on);
}

bool row_on(const char* id) {
    for (Row** p = g_rows; *p; p++) if (strcmp((*p)->id, id) == 0) return (*p)->on != 0;
    return true;                                 // not a registered row: never gated
}

void rows_pre() {
    for (Row** p = g_rows; *p; p++) if ((*p)->on && (*p)->pre) (*p)->pre();
}

void rows_post() {
    for (Row** p = g_rows; *p; p++) if ((*p)->on && (*p)->post) (*p)->post();
}

// ---------------------------------------------------------------- rollback
struct Span {
    const char*     row_id;
    uint8_t*        rt;          // static base, or null for dynamic
    span_resolver_t fn;
    void*           ctx;
    uint32_t        max_len;
    uint8_t*        buf;
    uint8_t*        saved_at;    // base used for the current snapshot (null = nothing saved)
    uint32_t        saved_len;
    int             dead;
};

static const int MAX_SPANS = 64;
static Span g_spans[MAX_SPANS];
static int  g_nspans = 0;

static Span* add_span(const char* row_id, uint32_t max_len) {
    if (g_nspans >= MAX_SPANS || max_len == 0 || max_len > (16u << 20)) {
        log_line("rollback: %s span rejected (count %d, len 0x%X)", row_id, g_nspans, max_len);
        return nullptr;
    }
    uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, max_len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { log_line("rollback: %s no memory for 0x%X bytes", row_id, max_len); return nullptr; }
    Span& s = g_spans[g_nspans++];
    memset(&s, 0, sizeof(s));
    s.row_id = row_id;
    s.max_len = max_len;
    s.buf = buf;
    return &s;
}

bool rollback_add_static(const char* row_id, uint8_t* rt, uint32_t len) {
    if (!rt) { log_line("rollback: %s static span unresolved - not added", row_id); return false; }
    Span* s = add_span(row_id, len);
    if (!s) return false;
    s->rt = rt;
    log_line("rollback: %s span gva=0x%08X len=0x%X (static)", row_id, rt_to_gva(rt), len);
    return true;
}

bool rollback_add_dynamic(const char* row_id, span_resolver_t fn, void* ctx, uint32_t max_len) {
    if (!fn) return false;
    Span* s = add_span(row_id, max_len);
    if (!s) return false;
    s->fn = fn;
    s->ctx = ctx;
    log_line("rollback: %s span (dynamic, max len 0x%X)", row_id, max_len);
    return true;
}

void rollback_pre() {
    for (int i = 0; i < g_nspans; i++) g_spans[i].saved_at = nullptr;
    if (pacing_kind() != INV_BUILD) return;
    for (int i = 0; i < g_nspans; i++) {
        Span& s = g_spans[i];
        if (s.dead || !row_on(s.row_id)) continue;
        uint8_t* base = s.rt;
        uint32_t len = s.max_len;
        if (s.fn) {
            bool ok = false;
            __try { ok = s.fn(s.ctx, &base, &len); }
            __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
            if (!ok || !base || len == 0) continue;  // block not present this frame
            if (len > s.max_len) len = s.max_len;
        }
        if (!safe_copy(s.buf, base, len)) {
            s.dead = 1;
            log_line("rollback: %s span %p len 0x%X faulted on snapshot - disabled", s.row_id, base, len);
            continue;
        }
        s.saved_at = base;
        s.saved_len = len;
    }
}

void rollback_post() {
    for (int i = 0; i < g_nspans; i++) {
        Span& s = g_spans[i];
        if (!s.saved_at) continue;
        if (!safe_copy(s.saved_at, s.buf, s.saved_len)) {
            s.dead = 1;
            log_line("rollback: %s span %p faulted on restore - disabled", s.row_id, s.saved_at);
        }
        s.saved_at = nullptr;
    }
}

} // namespace x2
