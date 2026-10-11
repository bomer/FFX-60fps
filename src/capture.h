// SPDX-License-Identifier: Apache-2.0
// Capture (TRIAGE E0): FRAMEDIFF's snapshot/diff engine inside fps60, observe-only. Internal
// header shared by capture.cpp and capture_config.cpp; the public API is in fps60.h.
// Ported from src/framediff/src/framediff.h (regions.txt / allow.txt types are unchanged, so the
// same files work for both DLLs).
#pragma once
#include "fps60.h"

// Diff boundary (see docs/fps60/capture.md and README.md "Capture (E0)").
enum CapBoundary { BOUNDARY_OUTER = 0, BOUNDARY_INNER = 1 };

struct CapConfig {
    int      enabled;            // read once at init: 0 = nothing allocated, nothing registered
    char     scenario[64];
    int      invocations;        // N counted invocations
    int      warmup;             // invocations before the baseline (>= 1)
    int      hotkey;             // VK code
    int      merge_gap;          // bytes: changed ranges closer than this are merged
    int      max_total_mb;       // snapshot + counter budget
    int      split_outside;      // also diff at the pre boundary: between-invocation writes -> OUTSIDE
    int      per_inv_ranges;     // list ranges per invocation too
    int      per_inv_cap;        // max per-invocation range lines per invocation
    int      report_cap;         // max range lines per class in the summary
    int      boundary;           // CapBoundary
    int      dll_notify;         // register LdrRegisterDllNotification (init only)
    wchar_t  log_dir[MAX_PATH];
    wchar_t  regions_file[MAX_PATH];
    wchar_t  allow_file[MAX_PATH];
    uint32_t ghidra_base_exe;    // 0x400000
    uint32_t ghidra_base_magic;  // 0x10000000
};
extern CapConfig g_ccfg;
void capture_config_load();                      // (re)reads [capture] of fps60.ini
const char* boundary_name(int b);

// regions.txt (same syntax as FRAMEDIFF)
enum RegionDefKind { RDEF_SECTION = 0, RDEF_HEAP = 1, RDEF_LABEL = 2 };
struct RegionDef {
    int      kind;
    char     name[64];           // heap/label name
    char     module_glob[64];    // section
    char     section_glob[16];   // section
    char     sym[48];            // heap: pointer global; label: static symbol
    int      ndrefs;
    int32_t  drefs[4];           // heap: base = *(base + drefs[i]) after the first deref
    int32_t  add;                // heap/label: final offset
    uint32_t size;               // fixed size, or stride when count_sym is set
    uint32_t stride;             // element stride for log annotation (0 = none; defaults to size with count_sym)
    char     count_sym[48];      // heap: size = *(int*)count_sym * size
    uint32_t max_size;
    int      line;
};
int regions_load(RegionDef* out, int cap);       // returns count, logs parse errors

// allow.txt (same syntax as FRAMEDIFF)
enum AllowSpec { ALLOW_SYM = 0, ALLOW_GVA = 1, ALLOW_OFF = 2 };
struct AllowRule {
    char     region_glob[64];
    int      spec;
    char     sym[48];
    uint32_t value;              // gva / off / sym addend
    uint32_t len;
    uint32_t stride;             // 0 = single span
    uint32_t count;              // 0 = unlimited (when stride != 0)
    char     reason[160];
    int      line;
    uint64_t suppressed_bytes;   // filled during a report
};
int allow_load(AllowRule* out, int cap);

bool glob_match(const char* pat, const char* s); // case-insensitive, '*' and '?'
