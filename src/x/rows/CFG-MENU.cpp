// SPDX-License-Identifier: Apache-2.0
// CFG-MENU (default on): the mod's user options as extra rows of the game's own main-menu Config page. Static RE:
// re/ledger/CONFIG-MENU.md. Ghidra VAs below.
//
// - The Config page (menu page 10) is native: a pointer array of 0x2C-byte row objects (DAT_0186a44c, count
//   DAT_0186a448, fixed 8 by the setup) drawn at 740/count spacing with no scroll; left/right cycle a row's value and
//   call its apply at once (CONFIG-MENU-004..007, 015).
// - Cfg_PageEnter 0x8CB3C0 (cdecl(desc), plain RET) is detoured: before it runs, the array is pointed at a mod copy
//   (the stock rows + ours) and the count raised. The original then resets the cursor and calls every row's init.
//   The cursor/count/array globals are read from the enter's own operands (+0x1C, +0x23, +0x31).
// - Cfg_LabelText 0x8DC9B0 and Cfg_HelpText 0x8DC970 (cdecl(u16 id) -> text, plain RET) are detoured: ids 0xF00+ are
//   ours (stock ids end at 0x26, CONFIG-MENU-010) and return static FFX-charset buffers (the help setter keeps the
//   pointer, CONFIG-MENU-011). ON/OFF value texts are the stock ones (ids 0x0B/0x0C); help is looked up by the
//   value id, so each mod value has its own id.
// - A change applies at once through the rows' runtime paths (row_set, pacing_set, smooth_set). The Config page is
//   reachable from the field menu only, so no battle is running. fps60.ini is written once the page is left (page
//   state desc+0x1C past 3: 4/5/0xB exit).
#include "../fps60.h"
#include "smooth.h"
#include <string.h>

struct CfgRow {                                     // CONFIG-MENU-004
    uint32_t zero;
    int32_t  nvals;
    int32_t  cur;
    int32_t  selectable;
    void (__cdecl* init)(CfgRow*);
    void (__cdecl* apply)(CfgRow*);
    uint32_t label;
    uint32_t vals[4];
};
static_assert(sizeof(CfgRow) == 0x2C, "Config row object is 0x2C bytes");

typedef void (__cdecl* enter_t)(void* desc);
typedef uint8_t* (__cdecl* text_t)(uint32_t id);

enum { ID_LABEL = 0xF00, ID_VALUE = 0xF40, ID_ON = 0x0B, ID_OFF = 0x0C, MAX_STOCK = 16 };

static uint8_t* g_enter_fn = nullptr;  static enter_t g_enter = nullptr;
static uint8_t* g_label_fn = nullptr;  static text_t  g_label = nullptr;
static uint8_t* g_help_fn = nullptr;   static text_t  g_help = nullptr;
static volatile int8_t*  g_cursor = nullptr;        // DAT_0186a444
static volatile int32_t* g_count = nullptr;         // DAT_0186a448
static void* volatile*   g_array = nullptr;         // DAT_0186a44c
static bool g_hooked = false;
static volatile int32_t* g_desc_state = nullptr;    // page descriptor +0x1C (from the enter's argument)
static bool g_dirty = false;

// CFG-MENU-BARS (default 1): each row draws a 60-unit bar (FUN_008cb530: FLD [60.0] at 0x8CB548, + 6 units of shadow)
// and a 54-unit strip (FUN_008cbae0: FLD [54.0] at 0x8CBC7D), sized for 8 rows at 740/8 = 92.5 apart. With 16 rows
// (46 apart) they ran into the next row (U 16/10/2026 screenshot). The two FLD operands point at these floats
// instead (the .rdata constants are shared), sized from the row count at page enter. Both functions are Config only.
static uint8_t* g_bar_fld = nullptr;  static uint32_t g_bar_orig = 0;
static uint8_t* g_strip_fld = nullptr; static uint32_t g_strip_orig = 0;
static float    g_bar_h = 60.0f, g_strip_h = 54.0f;
static bool     g_bars_patched = false;

static void size_bars(int count) {
    float s = 740.0f / (float)(count > 0 ? count : 8);
    g_bar_h = s - 6.0f < 60.0f ? s - 6.0f : 60.0f;   // bar + its shadow fill one row step
    g_strip_h = g_bar_h - 6.0f;                       // stock: strip = bar - 6, drawn 3 units below the bar top
}

static bool patch_operand(uint8_t* fld, const void* target) {
    uint32_t v = (uint32_t)(uintptr_t)target;
    return patch_write(fld + 2, &v, 4);
}

// ---------------------------------------------------------------- options
// v = value index as shown: binary rows are ON (0) / OFF (1), the stock order.
static int  get_fps()     { return g_cfg.pacing ? 0 : 1; }
static void set_fps(int v) { pacing_set(v == 0); }
static int  get_smooth()  { return smooth_enabled() ? 0 : 1; }
static void set_smooth(int v) { smooth_set(v == 0); }
static int  row_get(Row& r) { return r.on ? 0 : 1; }
static int  get_sensor()  { return row_get(row_QOLSENSOR); }
static void set_sensor(int v) { row_set(row_QOLSENSOR, v == 0); }
static int  get_ap()      { return row_get(row_QOLAP); }
static void set_ap(int v) { row_set(row_QOLAP, v == 0); }
static int  get_rate()    { return qol_ap_rate() >= 100 ? 0 : 1; }                       // Full / Half
static void set_rate(int v) { qol_ap_set_rate(v == 0 ? 100 : 50); }
static bool ap_on()       { return row_QOLAP.on != 0; }
static int  get_retry()   { return row_get(row_QOLRETRY); }
static void set_retry(int v) { row_set(row_QOLRETRY, v == 0); }
static int  get_reset()   { return row_get(row_SR003); }
static void set_reset(int v) { row_set(row_SR003, v == 0); }
static int  get_turbo()   { return row_get(row_DEVTURBO); }
static void set_turbo(int v) { row_set(row_DEVTURBO, v == 0); }

struct Opt {
    const char* label;
    const char* vals[3];                            // nullptr = stock ON / OFF text
    const char* help[3];                            // per value; nullptr = help[0]
    int  nvals;
    int  (*get)();
    void (*set)(int v);
    bool (*enabled)();                              // nullptr = always selectable; else greyed out and skipped when false
};

static const Opt k_opts[] = {
    { "60 FPS", {}, { "Run the game at 60 frames per second.", "Original 30 frames per second." }, 2, get_fps, set_fps },
    { "Smooth Motion", {}, { "Smooth camera and character motion (F7 in game).",
      "Original camera and character motion." }, 2, get_smooth, set_smooth },
    { "Sensor (All)", {}, { "All party members have Sensor, no armor needed.",
      "Sensor from armor only." }, 2, get_sensor, set_sensor },
    { "AP (All)", {}, { "Party members who did not fight earn AP too.", "Only members who fought earn AP." }, 2,
      get_ap, set_ap },
    { "Non-Fighter AP", { "Full", "Half" }, { "Members who did not fight earn full AP.",
      "Members who did not fight earn half AP." }, 2, get_rate, set_rate, ap_on },
    { "Battle Retry", {}, { "After a party wipe, choose Retry or Give up.", "Original game over." }, 2,
      get_retry, set_retry },
    { "Soft Reset", {}, { "Hold the soft reset buttons to return to the title.", "No soft reset." }, 2,
      get_reset, set_reset },
    { "Turbo", {}, { "Hold the turbo key (fps60.ini) to fast-forward.", "No turbo key." }, 2, get_turbo, set_turbo },
};
enum { N_OPTS = sizeof(k_opts) / sizeof(k_opts[0]) };

// ---------------------------------------------------------------- FFX text (RETRY-UI-011)
static const char k_punct[] = " !\"#$%&'()*+,-./:;<=>?";       // 0x3A.. (the quotes are curly in the game font)

static bool encode(const char* s, uint8_t* out, int n) {
    int i = 0;
    for (; *s && i < n - 1; s++) {
        char c = *s;
        const char* p;
        if (c >= '0' && c <= '9') out[i++] = (uint8_t)(0x30 + c - '0');
        else if (c >= 'A' && c <= 'Z') out[i++] = (uint8_t)(0x50 + c - 'A');
        else if (c >= 'a' && c <= 'z') out[i++] = (uint8_t)(0x70 + c - 'a');
        else if ((p = strchr(k_punct, c)) != nullptr) out[i++] = (uint8_t)(0x3A + (p - k_punct));
        else return false;
    }
    out[i] = 0;
    return *s == 0;
}

static uint8_t g_txt_label[N_OPTS][24];
static uint8_t g_txt_val[N_OPTS][3][12];
static uint8_t g_txt_help[N_OPTS][3][64];

static CfgRow g_rows[N_OPTS];
static void*  g_arr[MAX_STOCK + N_OPTS];
static void*  g_stock = nullptr;
static int    g_stock_n = 0;

static int opt_index(CfgRow* r) {
    int i = (int)(r - g_rows);
    return i >= 0 && i < N_OPTS ? i : -1;
}

static void refresh(CfgRow* r) {
    int i = opt_index(r);
    if (i < 0) return;
    r->cur = k_opts[i].get();
    r->selectable = !k_opts[i].enabled || k_opts[i].enabled() ? 1 : 0;
}

static void __cdecl row_init(CfgRow* r) { refresh(r); }

static void __cdecl row_apply(CfgRow* r) {
    int i = opt_index(r);
    if (i < 0) return;
    __try {
        k_opts[i].set(r->cur);
        int now = k_opts[i].get();
        if (now != r->cur) log_line("CFG-MENU: %s could not change (shows value %d)", k_opts[i].label, now);
        g_dirty = true;
        for (int k = 0; k < N_OPTS; k++) refresh(&g_rows[k]);   // a row may depend on this one (Non-Fighter AP)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("CFG-MENU: %s apply faulted", k_opts[i].label);
    }
}

// ---------------------------------------------------------------- detours
static void __cdecl h_enter(void* desc) {
    if (!g_disabled && row_CFGMENU.on) {
        __try {
            void* cur = *g_array;
            int   n = *g_count;
            if (cur == (void*)g_arr) { cur = g_stock; n = g_stock_n; }   // entered again with ours still in place
            else { g_stock = cur; g_stock_n = n; }
            if (cur && n > 0 && n <= MAX_STOCK) {
                memcpy(g_arr, cur, n * sizeof(void*));
                for (int i = 0; i < N_OPTS; i++) g_arr[n + i] = &g_rows[i];
                *g_array = g_arr;
                *g_count = n + N_OPTS;
                size_bars(n + N_OPTS);
                g_desc_state = (volatile int32_t*)((uint8_t*)desc + 0x1C);
                static bool once = false;
                if (!once) { once = true; log_line("CFG-MENU: Config page opened: %d stock rows + %d mod rows", n, N_OPTS); }
            } else {
                log_line("CFG-MENU: unexpected Config row list (%p, %d) - left stock", cur, n);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            log_line("CFG-MENU: enter faulted - row off");
            row_CFGMENU.on = 0;
        }
    }
    g_enter(desc);
}

static uint8_t* lookup(uint32_t id, bool help) {
    id &= 0xFFFF;
    if (id >= ID_LABEL && id < ID_LABEL + N_OPTS && !help) return g_txt_label[id - ID_LABEL];
    if (id >= ID_VALUE && id < ID_VALUE + N_OPTS * 4) {
        int i = (id - ID_VALUE) / 4, v = (id - ID_VALUE) % 4;
        if (v >= k_opts[i].nvals) v = 0;
        if (help) return g_txt_help[i][v];
        if (k_opts[i].vals[v]) return g_txt_val[i][v];
        return g_label(v == 0 ? ID_ON : ID_OFF);
    }
    return nullptr;
}

static uint8_t* __cdecl h_label(uint32_t id) {
    if (!g_disabled) if (uint8_t* t = lookup(id, false)) return t;
    return g_label(id);
}

static uint8_t* __cdecl h_help(uint32_t id) {
    if (!g_disabled) if (uint8_t* t = lookup(id, true)) return t;
    return g_help(id);
}

// ---------------------------------------------------------------- ini write-back
static void write_ini() {
    g_dirty = false;
    const char* on01[2] = { "1", "0" };              // v 0 = ON
    config_write("fps60", "pacing", on01[get_fps()]);
    config_write("rows", "SMOOTH", on01[get_smooth()]);
    config_write("rows", "QOL-SENSOR", on01[get_sensor()]);
    config_write("rows", "QOL-AP", on01[get_ap()]);
    if (ap_on()) config_write("rows", "QOL-AP-RATE", get_rate() == 0 ? "100" : "50");
    config_write("rows", "QOL-RETRY", on01[get_retry()]);
    config_write("rows", "DEV-TURBO", on01[get_turbo()]);
    // soft_reset holds the chord: keep it when on, unless it is the off value
    char b[96];
    config_str("fps60", "soft_reset", b, sizeof(b), "");
    bool off_now = !b[0] || strcmp(b, "0") == 0 || _stricmp(b, "off") == 0;
    if (get_reset() == 1) config_write("fps60", "soft_reset", "0");
    else if (off_now && b[0]) config_write("fps60", "soft_reset", "L1+L2+R1+R2+START+SELECT");
    log_line("CFG-MENU: fps60.ini written (0 = on: fps %d, smooth %d, sensor %d, ap %d rate %d, retry %d, reset %d, turbo %d)",
             get_fps(), get_smooth(), get_sensor(), get_ap(), get_rate(), get_retry(), get_reset(), get_turbo());
}

// ---------------------------------------------------------------- row
static bool install() {
    // Cfg_PageEnter: PUSH EBP; MOV EBP,ESP; PUSH ESI; CALL; MOV [..],EAX; MOV EAX,[EBP+8]; XOR ESI,ESI;
    // MOV dword [EAX+0x1C],0; MOV byte [cursor],0; CMP [count],ESI; JLE +0x21
    g_enter_fn = sig_find_unique("CFG-MENU Cfg_PageEnter",
        "55 8B EC 56 E8 ?? ?? ?? ?? A3 ?? ?? ?? ?? 8B 45 08 33 F6 C7 40 1C 00 00 00 00 C6 05 ?? ?? ?? ?? 00 39 35 ?? ?? ?? ?? 7E 21",
        0x8CB3C0);
    // Cfg_LabelText: FUN_0078fcf0(7, id, FUN_007851f0()); the tail of the next function makes it unique
    g_label_fn = sig_find_unique("CFG-MENU Cfg_LabelText",
        "55 8B EC E8 ?? ?? ?? ?? 50 FF 75 08 6A 07 E8 ?? ?? ?? ?? 83 C4 0C 5D C3 CC CC CC CC CC CC CC CC 55 8B EC 5D E9",
        0x8DC9B0);
    // Cfg_HelpText: FUN_0078fbb0(7 or 0xE, id, FUN_007851f0())
    g_help_fn = sig_find_unique("CFG-MENU Cfg_HelpText",
        "55 8B EC 56 E8 ?? ?? ?? ?? 8B F0 E8 ?? ?? ?? ?? 56 FF 75 08 85 C0 75 0D 6A 07 E8 ?? ?? ?? ?? 83 C4 0C 5E 5D C3 6A 0E E8",
        0x8DC970);
    if (!g_enter_fn || !g_label_fn || !g_help_fn) return false;
    // enter +0x30: MOV EAX,[array] (the init loop)
    if (!sig_match(g_enter_fn + 0x30, "A1 ?? ?? ?? ?? 8B 04 B0 50 8B 40 10 FF D0")) {
        log_line("  CFG-MENU: Cfg_PageEnter init loop has an unexpected shape - refused");
        return false;
    }
    g_cursor = (volatile int8_t*)read_abs(g_enter_fn + 0x1C);
    g_count = (volatile int32_t*)read_abs(g_enter_fn + 0x23);
    g_array = (void* volatile*)read_abs(g_enter_fn + 0x31);
    if (!g_cursor || !g_count || !g_array) return false;
    if (config_row("CFG-MENU-BARS", 1)) {
        // FUN_008cb530 +0x18: FLD [60.0]; FSTP [ESP]; CALL y-scale; FSTP [EBP-4]; ADD ESP,4; FLD [EBP-4]; FADD [EBP+0xC]
        g_bar_fld = sig_find_unique("CFG-MENU-BARS bar height",
            "D9 05 ?? ?? ?? ?? D9 1C 24 E8 ?? ?? ?? ?? D9 5D FC 83 C4 04 D9 45 FC D8 45 0C 68 00 00 00 26", 0x8CB548);
        // FUN_008cbae0: FLD [54.0]; FSTP [ESP]; CALL y-scale; FSTP [ESP]; PUSH ECX; FLD [654.0]; ...; FLD double [EBP-0x230]
        g_strip_fld = sig_find_unique("CFG-MENU-BARS strip height",
            "D9 05 ?? ?? ?? ?? D9 1C 24 E8 ?? ?? ?? ?? D9 1C 24 51 D9 05 ?? ?? ?? ?? D9 1C 24 E8 ?? ?? ?? ?? D9 1C 24 51 DD 85 D0 FD FF FF",
            0x8CBC7D);
        float* b = g_bar_fld ? (float*)read_abs(g_bar_fld + 2) : nullptr;
        float* t = g_strip_fld ? (float*)read_abs(g_strip_fld + 2) : nullptr;
        if (b && t && *b == 60.0f && *t == 54.0f) {
            g_bar_orig = *(uint32_t*)(g_bar_fld + 2);
            g_strip_orig = *(uint32_t*)(g_strip_fld + 2);
        } else {
            log_line("  CFG-MENU-BARS: bar/strip height constants not found (%.1f, %.1f) - bars stay stock", b ? *b : -1.0f, t ? *t : -1.0f);
            g_bar_fld = g_strip_fld = nullptr;
        }
    }

    for (int i = 0; i < N_OPTS; i++) {
        const Opt& o = k_opts[i];
        bool ok = encode(o.label, g_txt_label[i], sizeof(g_txt_label[i]));
        for (int v = 0; v < o.nvals; v++) {
            if (o.vals[v]) ok = encode(o.vals[v], g_txt_val[i][v], sizeof(g_txt_val[i][v])) && ok;
            ok = encode(o.help[v] ? o.help[v] : o.help[0], g_txt_help[i][v], sizeof(g_txt_help[i][v])) && ok;
        }
        if (!ok) { log_line("  CFG-MENU: text for '%s' does not fit the FFX charset - refused", o.label); return false; }
        CfgRow& r = g_rows[i];
        memset(&r, 0, sizeof(r));
        r.nvals = o.nvals;
        r.selectable = !o.enabled || o.enabled() ? 1 : 0;
        r.init = row_init;
        r.apply = row_apply;
        r.label = ID_LABEL + i;
        for (int v = 0; v < o.nvals; v++) r.vals[v] = ID_VALUE + i * 4 + v;
    }
    log_line("  CFG-MENU: enter gva=0x%08X label gva=0x%08X help gva=0x%08X; cursor 0x%08X count 0x%08X array 0x%08X; %d rows",
             rt_to_gva_exe(g_enter_fn), rt_to_gva_exe(g_label_fn), rt_to_gva_exe(g_help_fn), rt_to_gva_exe((void*)g_cursor),
             rt_to_gva_exe((void*)g_count), rt_to_gva_exe((void*)g_array), N_OPTS);
    return true;
}

static void pre() {
    if (!g_hooked) {
        g_hooked = true;
        if (!hook_entry("CFG-MENU label", g_label_fn, (void*)&h_label, (void**)&g_label) ||
            !hook_entry("CFG-MENU help", g_help_fn, (void*)&h_help, (void**)&g_help) ||
            !hook_entry("CFG-MENU enter", g_enter_fn, (void*)&h_enter, (void**)&g_enter)) {
            hook_remove("CFG-MENU label", g_label_fn);
            hook_remove("CFG-MENU help", g_help_fn);
            log_line("CFG-MENU: hook failed - row off");
            row_CFGMENU.on = 0;
            return;
        }
        log_line("CFG-MENU: Config page hooked on the game thread");
    }
    if (g_bar_fld && !g_bars_patched) {
        g_bars_patched = true;
        if (patch_operand(g_bar_fld, &g_bar_h) && patch_operand(g_strip_fld, &g_strip_h))
            log_line("CFG-MENU-BARS: row bar and strip heights follow the row count");
        else log_line("CFG-MENU-BARS: patch failed");
    }
    if (g_dirty && g_desc_state && *g_desc_state > 3) write_ini();
}

// Dev hot reload / off: the stock row list back (the game must not keep pointers into this copy), unsaved changes
// written.
static void stop() {
    __try {
        if (g_array && *g_array == (void*)g_arr && g_stock) { *g_array = g_stock; *g_count = g_stock_n; *g_cursor = 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_bars_patched) {
        g_bars_patched = false;
        patch_write(g_bar_fld + 2, &g_bar_orig, 4);
        patch_write(g_strip_fld + 2, &g_strip_orig, 4);
    }
    if (g_dirty) write_ini();
}

Row row_CFGMENU = {
    "CFG-MENU",
    "mod options (60 FPS, Smooth, Sensor, AP, Non-Fighter AP, Retry, Soft Reset, Turbo) as rows of the game's Config page",
    1, install, pre, nullptr, 0, stop
};
