// SPDX-License-Identifier: Apache-2.0
// [capture] of fps60.ini, regions.txt and allow.txt (same parsers as FRAMEDIFF). enabled and dll_notify are
// read once at init; the rest is re-read at every capture start, so it can be edited while the game runs.
#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CapConfig g_ccfg;

const char* boundary_name(int b) { return b == BOUNDARY_INNER ? "inner" : "outer"; }

static void ini_path(char* out, int cap) {
    wchar_t w[MAX_PATH];
    game_dir(w, MAX_PATH);
    lstrcatW(w, L"fps60.ini");
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, cap, nullptr, nullptr);
}

static long ini_int(const char* ini, const char* sect, const char* key, long def) {
    char buf[64];
    GetPrivateProfileStringA(sect, key, "", buf, sizeof(buf), ini);
    if (!buf[0]) return def;
    char* end = nullptr;
    long v = strtol(buf, &end, 0);
    if (end == buf) return def;
    return v;
}

static void dir_relative(const char* name, wchar_t* out) {
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH);
    if (w[0] && (w[1] == L':' || (w[0] == L'\\' && w[1] == L'\\'))){   // absolute
        lstrcpynW(out, w, MAX_PATH);
        return;
    }
    game_dir(out, MAX_PATH);
    if (lstrlenW(out) + lstrlenW(w) < MAX_PATH - 1) lstrcatW(out, w);
}

void capture_config_load() {
    char ini[MAX_PATH * 2];
    ini_path(ini, sizeof(ini));
    CapConfig& c = g_ccfg;
    const char* S = "capture";

    c.enabled = (int)ini_int(ini, S, "enabled", 0);
    GetPrivateProfileStringA(S, "scenario", "S1", c.scenario, sizeof(c.scenario), ini);
    for (char* p = c.scenario; *p; p++) {               // keep it filename-safe
        if (strchr("\\/:*?\"<>| ", *p)) *p = '-';
    }
    c.invocations    = (int)ini_int(ini, S, "invocations", 120);
    c.warmup         = (int)ini_int(ini, S, "warmup", 2);
    c.hotkey         = (int)ini_int(ini, S, "hotkey", VK_F9);
    c.merge_gap      = (int)ini_int(ini, S, "merge_gap", 8);
    c.max_total_mb   = (int)ini_int(ini, S, "max_total_mb", 160);
    c.split_outside  = (int)ini_int(ini, S, "split_outside", 1);
    c.per_inv_ranges = (int)ini_int(ini, S, "per_invocation_ranges", 0);
    c.per_inv_cap    = (int)ini_int(ini, S, "per_invocation_cap", 64);
    c.report_cap     = (int)ini_int(ini, S, "report_cap", 20000);
    c.dll_notify     = (int)ini_int(ini, S, "dll_notify", 1);
    char b[32];
    GetPrivateProfileStringA(S, "boundary", "outer", b, sizeof(b), ini);
    c.boundary = _stricmp(b, "inner") == 0 ? BOUNDARY_INNER : BOUNDARY_OUTER;

    char tmp[MAX_PATH];
    GetPrivateProfileStringA(S, "log_dir", "fps60_captures", tmp, sizeof(tmp), ini);
    dir_relative(tmp, c.log_dir);
    GetPrivateProfileStringA(S, "regions", "regions.txt", tmp, sizeof(tmp), ini);
    dir_relative(tmp, c.regions_file);
    GetPrivateProfileStringA(S, "allow", "allow.txt", tmp, sizeof(tmp), ini);
    dir_relative(tmp, c.allow_file);

    c.ghidra_base_exe   = (uint32_t)ini_int(ini, S, "ghidra_exe_base", 0x400000);
    c.ghidra_base_magic = (uint32_t)ini_int(ini, S, "ghidra_magic_base", 0x10000000);

    // sanity clamps (as FRAMEDIFF)
    if (c.invocations < 1)    c.invocations = 1;
    if (c.invocations > 100000) c.invocations = 100000;
    if (c.warmup < 1)         c.warmup = 1;       // the last warmup invocation primes the baseline
    if (c.merge_gap < 0)      c.merge_gap = 0;
    if (c.max_total_mb < 8)   c.max_total_mb = 8;
    if (c.max_total_mb > 768) c.max_total_mb = 768;
    if (c.per_inv_cap < 1)    c.per_inv_cap = 1;
    if (c.report_cap < 16)    c.report_cap = 16;
    if (c.hotkey <= 0 || c.hotkey > 0xFE) c.hotkey = VK_F9;
}

// ---------------------------------------------------------------- tiny text helpers
bool glob_match(const char* pat, const char* s) {
    // iterative '*' / '?' matcher, ASCII case-insensitive
    const char* star = nullptr;
    const char* ss = nullptr;
    while (*s) {
        char p = *pat, c = *s;
        if (p >= 'A' && p <= 'Z') p = (char)(p + 32);
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (p == '?' || (p && p == c && p != '*')) { pat++; s++; continue; }
        if (p == '*') { star = pat++; ss = s; continue; }
        if (star) { pat = star + 1; s = ++ss; continue; }
        return false;
    }
    while (*pat == '*') pat++;
    return *pat == 0;
}

static char* read_text_file(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size > (1u << 20)) { CloseHandle(h); return nullptr; }
    char* buf = (char*)malloc(size + 1);
    if (!buf) { CloseHandle(h); return nullptr; }
    DWORD got = 0;
    ReadFile(h, buf, size, &got, nullptr);
    CloseHandle(h);
    buf[got] = 0;
    return buf;
}

static int split_tokens(char* line, char** tok, int cap) {
    int n = 0;
    char* p = line;
    while (*p && n < cap) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

static bool parse_u32(const char* s, uint32_t* out) {
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_i32(const char* s, int32_t* out) {
    char* end = nullptr;
    long v = strtol(s, &end, 0);
    if (end == s || *end) return false;
    *out = (int32_t)v;
    return true;
}

static void copy_str(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

// Iterates lines; strips comments starting with '#'. For allow.txt the text after ';' is the
// reason and is handed back separately.
typedef bool (*line_fn)(char* line, char* reason, int lineno, void* ctx);

static void for_each_line(char* text, bool split_reason, line_fn fn, void* ctx) {
    int lineno = 0;
    char* p = text;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        lineno++;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = 0;
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        char* reason = (char*)"";
        if (split_reason) {
            char* semi = strchr(line, ';');
            if (semi) { *semi = 0; reason = semi + 1; while (*reason == ' ') reason++; }
        }
        char* q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (!*q) continue;
        if (!fn(q, reason, lineno, ctx)) break;
    }
}

// ---------------------------------------------------------------- regions.txt
struct RegionsCtx { RegionDef* out; int cap; int n; };

static bool parse_region_line(char* line, char*, int lineno, void* vctx) {
    RegionsCtx* ctx = (RegionsCtx*)vctx;
    char* tok[16];
    int nt = split_tokens(line, tok, 16);
    if (nt < 2) { log_line("regions.txt:%d: too few fields", lineno); return true; }
    if (ctx->n >= ctx->cap) { log_line("regions.txt: more than %d entries, rest ignored", ctx->cap); return false; }

    RegionDef d;
    memset(&d, 0, sizeof(d));
    d.line = lineno;

    if (_stricmp(tok[0], "section") == 0) {
        d.kind = RDEF_SECTION;
        copy_str(d.module_glob, sizeof(d.module_glob), tok[1]);
        copy_str(d.section_glob, sizeof(d.section_glob), nt >= 3 ? tok[2] : "*");
        ctx->out[ctx->n++] = d;
        return true;
    }

    if (_stricmp(tok[0], "heap") == 0 || _stricmp(tok[0], "label") == 0) {
        d.kind = _stricmp(tok[0], "heap") == 0 ? RDEF_HEAP : RDEF_LABEL;
        copy_str(d.name, sizeof(d.name), tok[1]);
        bool have_sym = false, have_size = false, bad = false;
        for (int i = 2; i < nt && !bad; i++) {
            char* eq = strchr(tok[i], '=');
            if (!eq) { bad = true; break; }
            *eq = 0;
            const char* k = tok[i];
            char* v = eq + 1;
            if ((_stricmp(k, "ptr") == 0 && d.kind == RDEF_HEAP) || (_stricmp(k, "sym") == 0 && d.kind == RDEF_LABEL)) {
                copy_str(d.sym, sizeof(d.sym), v);
                have_sym = true;
            } else if (_stricmp(k, "deref") == 0 && d.kind == RDEF_HEAP) {
                char* s = v;
                while (*s && d.ndrefs < 4) {
                    char* c = strchr(s, ',');
                    if (c) *c = 0;
                    if (!parse_i32(s, &d.drefs[d.ndrefs++])) bad = true;
                    if (!c) break;
                    s = c + 1;
                }
            } else if (_stricmp(k, "add") == 0) {
                if (!parse_i32(v, &d.add)) bad = true;
            } else if (_stricmp(k, "size") == 0) {
                if (v[0] == '@') {
                    char* star = strchr(v, '*');
                    if (!star || d.kind != RDEF_HEAP) { bad = true; break; }
                    *star = 0;
                    copy_str(d.count_sym, sizeof(d.count_sym), v + 1);
                    if (!parse_u32(star + 1, &d.size)) bad = true;
                } else if (!parse_u32(v, &d.size)) bad = true;
                have_size = true;
            } else if (_stricmp(k, "max") == 0) {
                if (!parse_u32(v, &d.max_size)) bad = true;
            } else if (_stricmp(k, "stride") == 0 && d.kind == RDEF_HEAP) {
                if (!parse_u32(v, &d.stride)) bad = true;
            } else {
                bad = true;
            }
        }
        if (bad || !have_sym || !have_size || d.size == 0) {
            log_line("regions.txt:%d: bad %s entry (need %s=<sym> size=<hex>|size=@<count_sym>*<stride>)",
                     lineno, tok[0], d.kind == RDEF_HEAP ? "ptr" : "sym");
            return true;
        }
        if (d.count_sym[0] && d.stride == 0) d.stride = d.size;
        if (d.max_size == 0) d.max_size = 0x01000000;    // 16 MB per heap block unless told otherwise
        ctx->out[ctx->n++] = d;
        return true;
    }

    log_line("regions.txt:%d: unknown kind '%s' (section|heap|label)", lineno, tok[0]);
    return true;
}

int regions_load(RegionDef* out, int cap) {
    char* text = read_text_file(g_ccfg.regions_file);
    if (!text) { log_line("regions.txt not found/readable - using built-in 'section FFX.exe *'"); }
    RegionsCtx ctx = { out, cap, 0 };
    if (text) {
        for_each_line(text, false, parse_region_line, &ctx);
        free(text);
    }
    if (ctx.n == 0 && cap > 0) {
        memset(&out[0], 0, sizeof(out[0]));
        out[0].kind = RDEF_SECTION;
        copy_str(out[0].module_glob, sizeof(out[0].module_glob), "FFX.exe");
        copy_str(out[0].section_glob, sizeof(out[0].section_glob), "*");
        ctx.n = 1;
    }
    return ctx.n;
}

// ---------------------------------------------------------------- allow.txt
struct AllowCtx { AllowRule* out; int cap; int n; };

static bool parse_allow_line(char* line, char* reason, int lineno, void* vctx) {
    AllowCtx* ctx = (AllowCtx*)vctx;
    char* tok[12];
    int nt = split_tokens(line, tok, 12);
    if (nt < 3) { log_line("allow.txt:%d: need '<region-glob> sym=|gva=|off=<x> len=<hex> ; reason'", lineno); return true; }
    if (ctx->n >= ctx->cap) { log_line("allow.txt: more than %d rules, rest ignored", ctx->cap); return false; }

    AllowRule r;
    memset(&r, 0, sizeof(r));
    r.line = lineno;
    copy_str(r.region_glob, sizeof(r.region_glob), tok[0]);
    bool have_spec = false, bad = false;
    for (int i = 1; i < nt && !bad; i++) {
        char* eq = strchr(tok[i], '=');
        if (!eq) { bad = true; break; }
        *eq = 0;
        const char* k = tok[i];
        char* v = eq + 1;
        if (_stricmp(k, "sym") == 0) {
            r.spec = ALLOW_SYM;
            char* plus = strchr(v, '+');
            if (plus) { *plus = 0; if (!parse_u32(plus + 1, &r.value)) bad = true; }
            copy_str(r.sym, sizeof(r.sym), v);
            have_spec = true;
        } else if (_stricmp(k, "gva") == 0) {
            r.spec = ALLOW_GVA; have_spec = parse_u32(v, &r.value); bad = !have_spec;
        } else if (_stricmp(k, "off") == 0) {
            r.spec = ALLOW_OFF; have_spec = parse_u32(v, &r.value); bad = !have_spec;
        } else if (_stricmp(k, "len") == 0) {
            if (!parse_u32(v, &r.len)) bad = true;
        } else if (_stricmp(k, "stride") == 0) {
            if (!parse_u32(v, &r.stride)) bad = true;
        } else if (_stricmp(k, "count") == 0) {
            if (strcmp(v, "*") == 0) r.count = 0;
            else if (!parse_u32(v, &r.count)) bad = true;
        } else bad = true;
    }
    if (bad || !have_spec || r.len == 0) {
        log_line("allow.txt:%d: bad rule", lineno);
        return true;
    }
    if (!reason || !*reason) {
        log_line("allow.txt:%d: rule has no '; reason' - rejected (every allow entry needs a reason)", lineno);
        return true;
    }
    copy_str(r.reason, sizeof(r.reason), reason);
    ctx->out[ctx->n++] = r;
    return true;
}

int allow_load(AllowRule* out, int cap) {
    char* text = read_text_file(g_ccfg.allow_file);
    if (!text) { log_line("allow.txt not found - no suppression"); return 0; }
    AllowCtx ctx = { out, cap, 0 };
    for_each_line(text, true, parse_allow_line, &ctx);
    free(text);
    return ctx.n;
}
