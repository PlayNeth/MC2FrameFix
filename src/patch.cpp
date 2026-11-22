#include "common.h"
#include "patch.h"
#include "game.h"

// ---------------------------------------------------------------------------
// Patch helpers
// ---------------------------------------------------------------------------
bool CheckBytes(DWORD at, const void* exp, size_t n) {
    return !IsBadReadPtr((void*)at, n) && !memcmp((void*)at, exp, n);
}
bool CheckCall(DWORD at, DWORD target) {
    const BYTE* p = (const BYTE*)at;
    if (IsBadReadPtr(p, 5) || p[0] != 0xE8) return false;
    return at + 5 + *(const int32_t*)(p + 1) == target;
}
void WriteRel(DWORD at, BYTE op, const void* target) {
    DWORD old; VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE*)at = op;
    *(int32_t*)(at + 1) = (int32_t)((DWORD)target - (at + 5));
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
}
void WriteNops(DWORD at, int n) {
    DWORD old; VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memset((void*)at, 0x90, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
}
bool PatchVtable(DWORD slot, DWORD expect, const void* fn) {
    if (IsBadReadPtr((void*)slot, 4) || G<DWORD>(slot) != expect) return false;
    DWORD old; VirtualProtect((void*)slot, 4, PAGE_EXECUTE_READWRITE, &old);
    G<DWORD>(slot) = (DWORD)fn;
    VirtualProtect((void*)slot, 4, old, &old);
    return true;
}
