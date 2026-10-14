// SPDX-License-Identifier: Apache-2.0
// Diagnostic DIAG-TEXTDUMP: writes the unpacked FFX.exe code to fps60_image.bin once at install (the file on
// disk is DRM-encrypted); input for tools/sigcheck.py. Never commit the file.
// Details: docs/fps60/rows/DIAG-TEXTDUMP.md
#include "../fps60.h"
#include <stdlib.h>
#include <string.h>

static bool install() {
    uint8_t* base = exe_base();
    uint32_t size = exe_size();
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    uint8_t* img = (uint8_t*)calloc(size, 1);
    if (!img) { log_line("  DIAG-TEXTDUMP: no memory for 0x%X bytes", size); return false; }
    memcpy(img, base, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint32_t va = sec[i].VirtualAddress, n = sec[i].Misc.VirtualSize;
        if (va + n > size || !safe_copy(img + va, base + va, n))
            log_line("  DIAG-TEXTDUMP: section %d (rva 0x%X) not copied", i, va);
    }

    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = path;
    for (DWORD i = 0; i < len; i++) if (path[i] == L'\\') slash = path + i + 1;
    lstrcpyW(slash, L"fps60_image.bin");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, img, size, &wrote, nullptr) && wrote == size;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(img);
    log_line("  DIAG-TEXTDUMP: %s fps60_image.bin (0x%X bytes, image base %p)", ok ? "wrote" : "FAILED to write", size, base);
    return ok;                                   // no pre/post: nothing runs per invocation
}

Row row_DIAG_TEXTDUMP = {
    "DIAG-TEXTDUMP",
    "diagnostic: write the unpacked FFX.exe code to fps60_image.bin once (input for tools/sigcheck.py)",
    0, install, nullptr, nullptr, 0
};
